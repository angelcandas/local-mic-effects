#pragma once

#include "StemSplitter.h"

#include <juce_core/juce_core.h>

// Everything Demucs does around its neural network, so that the network itself can run
// anywhere (Core ML on the GPU): the spectrogram, the normalising, the segments with their
// crossfades, then the mask, the inverse spectrogram and the sum with the time branch.
// It follows demucs.cpp (and the Python original, apply_model with split and overlap 0.25).
//
// The network core is the part of HTDemucs.forward between "normalise" and "undo the
// normalising": see scripts/export_htdemucs_coreml.py. Needs libs/demucs.cpp (its STFT).
namespace stems
{
struct CoreNetwork
{
    static constexpr int segmentSamples = 343980; // 7.8 s at 44.1 kHz, what the model was trained on
    static constexpr int freqBins = 2048;
    static constexpr int frames = 336;
    static constexpr int freqChannels = 4;        // left/right, real/imaginary
    static constexpr int timeChannels = 2;

    // One segment. All arrays are row-major and contiguous.
    //   x     [4][2048][336]    normalised spectrogram: re L, im L, re R, im R
    //   xt    [2][segmentSamples] normalised waveform
    //   xOut  [16][2048][336]   source s is channels 4s..4s+3
    //   xtOut [8][segmentSamples] source s is channels 2s..2s+1
    // Returns an empty string, or why it failed.
    std::function<juce::String (const float* x, const float* xt, float* xOut, float* xtOut)> run;
};

// A separator that cuts each chunk into segments, runs the network on every one, and
// crossfades the results.
Separator makeSeparator (std::shared_ptr<const CoreNetwork>);
}
