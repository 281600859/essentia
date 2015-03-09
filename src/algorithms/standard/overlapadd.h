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

#ifndef ESSENTIA_OVERLAPADD_H
#define ESSENTIA_OVERLAPADD_H

#include "algorithm.h"

namespace essentia {
namespace standard {

class OverlapAdd : public Algorithm {

 private:

  Input<std::vector<Real> > _windowedFrame;
  Output<std::vector<Real> > _output;


//    Output<std::vector<AudioSample> > _audio; // ?? check what data type is appropriate
  int _frameSize;
  int _hopSize;
  std::vector<Real> _frameHistory;
int _frameCounter; // debug sawtooth

 public:
  OverlapAdd() {
    declareInput(_windowedFrame, "signal", "the windowed input audio frame");
    //declareOutput(_frame, "frame", "the output overlap-add audio signal frame");
    declareOutput(_output, "signal", "the output overlap-add audio signal frame");
  }

  void declareParameters() {
    declareParameter("frameSize", "the frame size for computing the overlap-add process", "(0,inf)", 2048);
    declareParameter("hopSize", "the hop size with which the overlap-add function is computed", "(0,inf)", 128);

  }
  void compute();
  void configure();

  static const char* name;
  static const char* description;

};

} // namespace standard
} // namespace essentia

#include "streamingalgorithmwrapper.h"

namespace essentia {
namespace streaming {

class OverlapAdd : public StreamingAlgorithmWrapper {
//class OverlapAdd : public Algorithm {

 protected:

//  Sink<std::vector<Real> > _windowedFrame; // input
//  Source<std::vector<Real> > _output; // output ?? should be like this?
  Sink<std::vector<Real> > _windowedFrame; // input
  Source<Real> _output;

  int _frameSize;
  int _hopSize;


  bool _configured;
  std::vector<Real> _frameHistory;

 public:
  OverlapAdd() {
    declareAlgorithm("OverlapAdd");
    //declareInput(_windowedFrame, TOKEN, "frame");

    int preferredSize = 4096;
    declareInput(_windowedFrame, TOKEN,"signal");
    declareOutput(_output, TOKEN, "signal");

    _output.setBufferType(BufferUsage::forLargeAudioStream);

  }
};

} // namespace streaming
} // namespace essentia

#endif // ESSENTIA_ZEROCROSSINGRATE_H
