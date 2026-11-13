#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

// Phase and polarity between two recordings of the same sound: two mics on one source
// (a kick in and out, a snare top and bottom, a guitar amp's close and room mic), a DI
// and its amp, the close mics and the overheads of a kit. Recorded at once, one arrives
// a little later (it's further away) or upside down (a mic facing the other way), and
// summed they thin out. Plain computation: runs anywhere.
namespace analysis
{
struct Alignment
{
    double delaySeconds = 0.0; // how late `b` is against `a` (move b earlier by this to line them up; < 0: b is early)
    bool inverted = false;     // b is upside down against a (flip its polarity)
    float confidence = 0.0f;   // 0..1: how much of the two is the same sound (the height of the correlation peak)
    float sumGainDb = 0.0f;    // how much fuller a + b get once b is lined up (dB; 0 when it changes nothing)
    bool related = false;      // they picked up the same sound clearly enough to line them up

    juce::int64 getDelaySamples (double sampleRate) const noexcept { return (juce::int64) std::llround (delaySeconds * sampleRate); }
};

// The delay of `b` against `a` within +/- maxSeconds by GCC-PHAT (cross-spectra summed
// over the parts where both play, whitened, so any sound lines up the same, bright or
// dark), and the polarity from the sign of the peak. Both from the same timeline and
// sample rate; any number of channels (summed to mono). Looks at most `maxAnalysedSeconds`.
Alignment measureAlignment (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b, double sampleRate,
                            double maxSeconds = 0.02, double maxAnalysedSeconds = 60.0);

inline constexpr float minAlignmentConfidence = 0.12f; // below: two different sounds, left as they are
}
