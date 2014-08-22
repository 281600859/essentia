/*
 * Copyright (C) 2006-2013  Music Technology Group - Universitat Pompeu Fabra
 *
 * This file is part of Essentia
 *
 * Essentia is free software: you can redistribute it and/or modify it under
 * the terms of the GNU Affero General Public License as published by the Free
 * Software Foundation (FSF), either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the Affero GNU General Public License
 * version 3 along with this program.  If not, see http://www.gnu.org/licenses/
 */

#include "MusicExtractor.h"
using namespace std;
using namespace essentia;
using namespace streaming;
using namespace scheduler;

void MusicExtractor::compute(const string& audioFilename){
  
  AlgorithmFactory& factory = AlgorithmFactory::instance();
  
  analysisSampleRate = options.value<Real>("analysisSampleRate");
  startTime = options.value<Real>("startTime");
  endTime = options.value<Real>("endTime");
  downmix = "mix";

  results.set("metadata.version.essentia", essentia::version);
  results.set("metadata.version.essentia_git_sha", essentia::version_git_sha);
  results.set("metadata.version.extractor", EXTRACTOR_VERSION);
  // TODO: extractor_build_id

  results.set("metadata.audio_properties.equal_loudness", false);
  results.set("metadata.audio_properties.analysis_sample_rate", analysisSampleRate);

  // TODO: we still compute some low-level descriptors with equal loudness filter...
  // TODO: remove for consistency? evaluate on classification tasks?

  readMetadata(audioFilename);
  computeReplayGain(audioFilename); // compute replay gain and the duration of the track
  
  if (endTime > results.value<Real>("metadata.audio_properties.length")) {
      endTime = results.value<Real>("metadata.audio_properties.length");
  }

  // normalize the audio with replay gain and compute as many lowlevel, rhythm,
  // and tonal descriptors as possible           

  Algorithm* loader = factory.create("EasyLoader",
                                    "filename",   audioFilename,
                                    "sampleRate", analysisSampleRate,
                                    "startTime",  startTime,
                                    "endTime",    endTime,
                                    "replayGain", replayGain,
                                    "downmix",    downmix);

  MusicLowlevelDescriptors *lowlevel = new MusicLowlevelDescriptors(options);
  MusicRhythmDescriptors *rhythm = new MusicRhythmDescriptors(options);
  MusicTonalDescriptors *tonal = new MusicTonalDescriptors(options);

  SourceBase& source = loader->output("audio");
  lowlevel->createNetworkNeqLoud(source, results);
  lowlevel->createNetworkEqLoud(source, results);
  lowlevel->createNetworkLoudness(source, results);
  rhythm->createNetwork(source, results);
  tonal->createNetworkTuningFrequency(source, results);
  
  Network network(loader,false);
  network.run();


  // Descriptors that require values from other descriptors in the previous chain
  lowlevel->computeAverageLoudness(results);  // requires 'loudness'
  
  // compute onset rate = len(onsets) / len(audio)
  // we do not need onset times, as they are most probably incorrect, while onset_rate is more informative
  results.set(rhythm->nameSpace + "onset_rate", results.value<vector<Real> >(rhythm->nameSpace + "onset_times").size()
     / (Real) loader->output("audio").totalProduced()
     * results.value<Real>("metadata.audio_properties.analysis_sample_rate"));
  results.remove(rhythm->nameSpace + "onset_times");



  Algorithm* loader_2 = factory.create("EasyLoader",
                                       "filename",   audioFilename,
                                       "sampleRate", analysisSampleRate,
                                       "startTime",  startTime,
                                       "endTime",    endTime,
                                       "replayGain", replayGain,
                                       "downmix",    downmix);

  SourceBase& source_2 = loader_2->output("audio");
  rhythm->createNetworkBeatsLoudness(source_2, results);  // requires 'beat_positions'
  tonal->createNetwork(source_2, results);                // requires 'tuning frequency'

  Network network_2(loader_2);
  network_2.run();


  // Descriptors that require values from other descriptors in the previous chain
  tonal->computeTuningSystemFeatures(results); // requires 'hpcp_highres'

  // TODO is this necessary? tuning_frequency should always have one value:
  Real tuningFreq = results.value<vector<Real> >(tonal->nameSpace + "tuning_frequency").back();
  results.remove(tonal->nameSpace + "tuning_frequency");
  results.set(tonal->nameSpace + "tuning_frequency", tuningFreq);


  cout << "Compute Aggregation"<<endl; 
  this->stats = this->computeAggregation(results);

  // pre-trained classifiers are only available in branches devoted for that 
  // (eg: 2.0.1)
  /*
#if HAVE_GAIA2 
  computeSVMDescriptors(stats); 
#else 
  cout << "Warning: Essentia was compiled without Gaia2 library, skipping SVM models" << endl;
#endif
  */

  cout << "All done"<<endl;
  return;
}


Pool MusicExtractor::computeAggregation(Pool& pool){

  // choose which descriptors stats to output
  const char* defaultStats[] = { "mean", "var", "min", "max", "dmean", "dmean2", "dvar", "dvar2" };

  map<string, vector<string> > exceptions;
  const vector<string>& descNames = pool.descriptorNames();
  for (int i=0; i<(int)descNames.size(); i++) {
    if (descNames[i].find("lowlevel.mfcc") != string::npos) {
      exceptions[descNames[i]] = options.value<vector<string> >("lowlevel.mfccStats");
      continue;
    }
    if (descNames[i].find("lowlevel.gfcc") != string::npos) {
      exceptions[descNames[i]] = options.value<vector<string> >("lowlevel.gfccStats");
      continue;
    }
    if (descNames[i].find("lowlevel.") != string::npos) {
      exceptions[descNames[i]] = options.value<vector<string> >("lowlevel.stats");
      continue;
    }
    if (descNames[i].find("rhythm.") != string::npos) {
      exceptions[descNames[i]] = options.value<vector<string> >("rhythm.stats");
      continue;
    }
    if (descNames[i].find("tonal.") != string::npos) {
      exceptions[descNames[i]] = options.value<vector<string> >("tonal.stats");
      continue;
    }
    if (descNames[i].find("sfx.") != string::npos) {  // TODO sfx not computed for music?
      exceptions[descNames[i]] = options.value<vector<string> >("sfx.stats");
      continue;
    }
  }

  standard::Algorithm* aggregator = standard::AlgorithmFactory::create("PoolAggregator",
                                                                       "defaultStats", arrayToVector<string>(defaultStats),
                                                                       "exceptions", exceptions);
  Pool poolStats;
  aggregator->input("input").set(pool);
  aggregator->output("output").set(poolStats);

  cout << "Process step: Aggregation" << endl;

  aggregator->compute();


  // add descriptors that may be missing due to content
  const Real emptyVector[] = { 0, 0, 0, 0, 0, 0};
  
  int statsSize = int(sizeof(defaultStats)/sizeof(defaultStats[0]));

  if(!pool.contains<vector<Real> >("rhythm.beats_loudness")){
    for (int i=0; i<statsSize; i++)
        poolStats.set(string("rhythm.beats_loudness.")+defaultStats[i],0); 
    }
  if(!pool.contains<vector<vector<Real> > >("rhythm.beats_loudness_band_ratio"))
    for (int i=0; i<statsSize; i++) 
      poolStats.set(string("rhythm.beats_loudness_band_ratio.")+defaultStats[i],
        arrayToVector<Real>(emptyVector));
  else if (pool.value<vector<vector<Real> > >("rhythm.beats_loudness_band_ratio").size()<2){
      poolStats.remove(string("rhythm.beats_loudness_band_ratio"));
      for (int i=0; i<statsSize; i++) {
        if(i==1 || i==6 || i==7)// var, dvar and dvar2 are 0
          poolStats.set(string("rhythm.beats_loudness_band_ratio.")+defaultStats[i],
              arrayToVector<Real>(emptyVector));
        else
          poolStats.set(string("rhythm.beats_loudness_band_ratio.")+defaultStats[i],
              pool.value<vector<vector<Real> > >("rhythm.beats_loudness_band_ratio")[0]);
      }
  }

    
  // variable descriptor length counts:

  // poolStats.set(string("rhythm.onset_count"), pool.value<vector<Real> >("rhythm.onset_times").size());
  poolStats.set(string("rhythm.beats_count"), pool.value<vector<Real> >("rhythm.beats_position").size());
  //poolStats.set(string("tonal.chords_count"), pool.value<vector<string> >("tonal.chords_progression").size());
  
  delete aggregator;

  return poolStats;
}


void MusicExtractor::readMetadata(const string& audioFilename) {
  // Pool Connector in streaming mode currently does not support Pool sources,
  // therefore, using standard mode
  standard::Algorithm* metadata = standard::AlgorithmFactory::create("MetadataReader",
                                                                     "filename", audioFilename,
                                                                     "failOnError", true,
                                                                     "tagPoolName", "metadata.tags");
  string title, artist, album, comment, genre, tracknumber, date;
  int duration, sampleRate, bitrate, channels;

  Pool poolTags;
  metadata->output("title").set(title);
  metadata->output("artist").set(artist);
  metadata->output("album").set(album);
  metadata->output("comment").set(comment);
  metadata->output("genre").set(genre);
  metadata->output("tracknumber").set(tracknumber);
  metadata->output("date").set(date);

  metadata->output("bitrate").set(bitrate);
  metadata->output("channels").set(channels);
  metadata->output("duration").set(duration);
  metadata->output("sampleRate").set(sampleRate);

  metadata->output("tagPool").set(poolTags);

  metadata->compute();

  results.merge(poolTags);
  delete metadata;

  /*
  AlgorithmFactory& factory = AlgorithmFactory::instance();
  Algorithm* metadata = factory.create("MetadataReader",
                                       "filename", audioFilename,
                                       "failOnError", true);

  metadata->output("title")       >> PC(results, "metadata.tags.title");
  metadata->output("artist")      >> PC(results, "metadata.tags.artist");
  metadata->output("album")       >> PC(results, "metadata.tags.album");
  metadata->output("comment")     >> PC(results, "metadata.tags.comment");
  metadata->output("genre")       >> PC(results, "metadata.tags.genre");
  metadata->output("tracknumber") >> PC(results, "metadata.tags.tracknumber");
  metadata->output("date")        >> PC(results, "metadata.tags.date");
  //metadata->output("tagPool")     >> PC(results, "metadata.tags.all");  // currently not supported
  metadata->output("bitrate")     >> PC(results, "metadata.audio_properties.bitrate");
  metadata->output("channels")    >> PC(results, "metadata.audio_properties.channels");
  metadata->output("duration")    >> NOWHERE; // let audio loader take care of this // TODO ???
  metadata->output("sampleRate")  >> NOWHERE; // let the audio loader take care of this // TODO ???

  Network(metadata).run();
  */
}


void MusicExtractor::computeReplayGain(const string& audioFilename) {

  // get metadata and compute replay gain

  AlgorithmFactory& factory = AlgorithmFactory::instance();

  cout << "Process step: Replay Gain" << endl;

  replayGain = 0.0;
  int length = 0;
  
  while (true) {
    Algorithm* audio = factory.create("EqloudLoader",
                                      "filename",   audioFilename,
                                      "sampleRate", analysisSampleRate,
                                      "startTime",  startTime,
                                      "endTime",    endTime,
                                      "downmix",    downmix);
    Algorithm* rgain = factory.create("ReplayGain", "applyEqloud", false);

    audio->output("audio")      >> rgain->input("signal");
    rgain->output("replayGain") >> PC(results, "metadata.audio_properties.replay_gain");

    try {
      Network network(audio);
      network.run();
      length = audio->output("audio").totalProduced();
      replayGain = results.value<Real>("metadata.audio_properties.replay_gain");
    }

    catch (const EssentiaException&) {
      if (downmix == "mix") {
        downmix = "left";
      }
      else {
        cout << "ERROR: File looks like a completely silent file... Aborting..." << endl;
        exit(4);
      }    
      
      try {
        results.remove("metadata.audio_properties.replay_gain");
      }
      catch (EssentiaException&) {}
      continue;
    }

    if (replayGain <= 40.0) { 
      // normal replay gain value; threshold set to 20 was found too conservative
      break;
    } 

    // otherwise, a very high value for replayGain: we are probably analyzing a 
    // silence even though it is not a pure digital silence. except if it was 
    // some electro music where someone thought it was smart to have opposite 
    // left and right channels... Try with only the left channel, then.
    if (downmix == "mix") {
      downmix = "left";
      results.remove("metadata.audio_properties.replay_gain");
    }
    else {
      cout << "ERROR: File looks like a completely silent file... Aborting..." << endl;
      exit(5);
    }
  }
  
  results.set("metadata.audio_properties.downmix", downmix);
  
  // set length (actually duration) of the file
  results.set("metadata.audio_properties.length", length/analysisSampleRate);
}


void MusicExtractor::outputToFile(Pool& pool, const string& outputFilename){

  cout << "Writing results to file " << outputFilename << endl;

  string format = options.value<string>("outputFormat");
  standard::Algorithm* output = standard::AlgorithmFactory::create("YamlOutput",
                                                                   "filename", outputFilename + "." + format,
                                                                   "doubleCheck", true,
                                                                   "format", format);
  output->input("pool").set(pool);
  output->compute();
  delete output;
}


void MusicExtractor::computeSVMDescriptors(Pool& pool) {
  cout << "Process step 6: SVM Models" << endl;
  //const char* svmModels[] = {}; // leave this empty if you don't have any SVM models
  const char* svmModels[] = { "genre_tzanetakis", "genre_dortmund",
                              "genre_electronica", "genre_rosamerica",
                              "mood_acoustic", "mood_aggressive",
                              "mood_electronic", "mood_happy", "mood_party", 
                              "mood_relaxed", "mood_sad", "timbre", "culture", 
                              "gender", "mirex-moods", "ballroom", 
                              "voice_instrumental" };

  string pathToSvmModels;

#ifdef OS_WIN32
  pathToSvmModels = "svm_models\\";
#else
  pathToSvmModels = "svm_models/";
#endif

  for (int i=0; i<(int)ARRAY_SIZE(svmModels); i++) {
    //cout << "adding HL desc: " << svmModels[i] << endl;
    string modelFilename = pathToSvmModels + string(svmModels[i]) + ".history";
    standard::Algorithm* svm = standard::AlgorithmFactory::create("GaiaTransform",
                                                                  "history", modelFilename);

    svm->input("pool").set(pool);
    svm->output("pool").set(pool);
    svm->compute();

    delete svm;
  }
}


void MusicExtractor::setExtractorOptions(const std::string& filename) {
  setExtractorDefaultOptions();

  if (filename.empty()) return;

  Pool opts;
  standard::Algorithm * yaml = standard::AlgorithmFactory::create("YamlInput", "filename", filename);
  yaml->output("pool").set(opts);
  yaml->compute();
  delete yaml;
  options.merge(opts, "replace");
}

void MusicExtractor::setExtractorDefaultOptions() {
  // general
  options.set("startTime", 0);
  options.set("endTime", 1e6);
  options.set("analysisSampleRate", 44100.0);
  options.set("outputFrames", false);
  options.set("outputFormat", "json");

  string silentFrames = "noise";
  int zeroPadding = 0;
  string windowType = "hann";

  // lowlevel
  options.set("lowlevel.frameSize", 2048);
  options.set("lowlevel.hopSize", 1024);
  options.set("lowlevel.zeroPadding", zeroPadding);
  options.set("lowlevel.windowType", "blackmanharris62");
  options.set("lowlevel.silentFrames", silentFrames);

  // average_loudness
  options.set("average_loudness.frameSize", 88200);
  options.set("average_loudness.hopSize", 44100);
  options.set("average_loudness.windowType", windowType);
  options.set("average_loudness.silentFrames", silentFrames);

  // rhythm
  options.set("rhythm.method", "degara");
  options.set("rhythm.minTempo", 40);
  options.set("rhythm.maxTempo", 208);

  // tonal
  options.set("tonal.frameSize", 4096);
  options.set("tonal.hopSize", 2048);
  options.set("tonal.zeroPadding", zeroPadding);
  options.set("tonal.windowType", "blackmanharris62");
  options.set("tonal.silentFrames", silentFrames);

  // stats
  const char* statsArray[] = { "mean", "var", "median", "min", "max", "dmean", "dmean2", "dvar", "dvar2" };
  const char* mfccStatsArray[] = { "mean", "cov", "icov" };
  const char* gfccStatsArray[] = { "mean", "cov", "icov" };

  vector<string> stats = arrayToVector<string>(statsArray);
  vector<string> mfccStats = arrayToVector<string>(mfccStatsArray);
  vector<string> gfccStats = arrayToVector<string>(gfccStatsArray);
  for (int i=0; i<(int)stats.size(); i++) {
    options.add("lowlevel.stats", stats[i]);
    options.add("tonal.stats", stats[i]);
    options.add("rhythm.stats", stats[i]);
    options.add("sfx.stats", stats[i]);
  }
  for (int i=0; i<(int)mfccStats.size(); i++)
    options.add("lowlevel.mfccStats", mfccStats[i]);
  for (int i=0; i<(int)gfccStats.size(); i++)
    options.add("lowlevel.gfccStats", gfccStats[i]);
}