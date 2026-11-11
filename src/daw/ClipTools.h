#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <vector>

// Analysis of a clip's audio for clip commands (normalise, strip silence). The audio is
// the clip's whole file as the AudioPool decodes it; offset and length pick the window
// the clip plays, in samples.
namespace daw::cliptools
{
// Peak level (linear) of the window, over every channel.
inline float getPeak (const juce::AudioBuffer<float>& audio, juce::int64 offset, juce::int64 length)
{
    const auto start = (int) juce::jlimit ((juce::int64) 0, (juce::int64) audio.getNumSamples(), offset);
    const auto end = (int) juce::jlimit ((juce::int64) start, (juce::int64) audio.getNumSamples(), offset + length);
    return end > start ? audio.getMagnitude (start, end - start) : 0.0f;
}

// The clip gain (dB) that brings the window's peak to `targetDb`, or 0 for silence.
inline float getNormaliseGain (const juce::AudioBuffer<float>& audio, juce::int64 offset, juce::int64 length, float targetDb = -1.0f)
{
    const auto peak = getPeak (audio, offset, length);
    if (peak < 1.0e-5f)
        return 0.0f;
    return juce::jlimit (-60.0f, 24.0f, targetDb - juce::Decibels::gainToDecibels (peak));
}

struct Region { double start, end; }; // seconds from the start of the window

// The parts of the window with sound in them: anything louder than thresholdDb (10 ms
// peak windows), with silences shorter than minSilence bridged, sounds shorter than
// minSound dropped, and `pad` seconds kept around each part so breaths and consonants
// aren't clipped.
inline std::vector<Region> findSound (const juce::AudioBuffer<float>& audio, juce::int64 offset, juce::int64 length,
                                      double sampleRate, float thresholdDb = -45.0f, double minSilence = 0.3,
                                      double minSound = 0.08, double pad = 0.06)
{
    std::vector<Region> regions;
    const auto window = juce::jmax (1, (int) (sampleRate * 0.01));
    const auto threshold = juce::Decibels::decibelsToGain (thresholdDb);
    const auto total = juce::jlimit ((juce::int64) 0, (juce::int64) audio.getNumSamples() - offset, length);
    const auto seconds = (double) total / sampleRate;

    double soundStart = -1.0, lastLoud = -1.0;

    for (juce::int64 at = 0; at < total; at += window)
    {
        const auto n = (juce::int64) juce::jmin ((juce::int64) window, total - at);
        const auto loud = getPeak (audio, offset + at, n) >= threshold;
        const auto t = (double) at / sampleRate, tEnd = (double) (at + n) / sampleRate;

        if (! loud)
            continue;

        if (soundStart < 0.0)
            soundStart = t;
        else if (t - lastLoud >= minSilence)
        {
            regions.push_back ({ soundStart, lastLoud });
            soundStart = t;
        }

        lastLoud = tEnd;
    }

    if (soundStart >= 0.0)
        regions.push_back ({ soundStart, lastLoud });

    std::vector<Region> result;
    for (auto r : regions)
    {
        if (r.end - r.start < minSound)
            continue;
        r.start = juce::jmax (0.0, r.start - pad);
        r.end = juce::jmin (seconds, r.end + pad);
        if (! result.empty() && r.start <= result.back().end)
            result.back().end = r.end;
        else
            result.push_back (r);
    }

    return result;
}
}
