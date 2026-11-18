#pragma once

#include "MixBench.h"

// Measurements for learning Auto Mix's targets from human mixes (see tools/README-mixbench.md,
// "Learn"). For each song of the bench this writes one small file of numbers, no audio:
// every raw track and the human mix as power per 1/3-octave band per 100 ms hop, per
// channel, with the left/right correlation per band, plus each hop's peak and RMS. The
// fitting (levels, EQ, compression, pan, reverb, loudness) is done from these files by
// tools/mixlearn/learn.py, so it can be repeated and changed without the audio.
//
// File (<name>.mlf, gzip): "MLF1", a uint32 header length, the header as JSON (song, style,
// tempo, hop, band centres, and per file: name, kind "track" or "mix", role, sampleRate,
// channels, hops), then per file in header order, int16 little-endian:
//   left band power dB x100        [hops][30]
//   right band power dB x100       [hops][30]   (stereo files only)
//   left/right correlation x10000  [hops][30]   (stereo files only)
//   peak dBFS x100, RMS dBFS x100  [hops] each
// Silence is -200 dB.
namespace analysis::learn
{
// Measures one file. Empty on failure.
struct FileBands
{
    double sampleRate = 0.0;
    int channels = 0;
    std::vector<Bands> left, right, correlation; // right and correlation empty for mono
    std::vector<float> peakDb, rmsDb;
    int getNumHops() const noexcept { return (int) left.size(); }
};

FileBands measureFile (const juce::File&);

// Writes `out` for the song. `roles` by track file name without extension (the bench's
// guesses); a track that isn't in it is written with role "other".
bool writeSong (const bench::Song&, const std::map<juce::String, Role>& roles, const juce::File& out,
                const std::function<void (const juce::String&)>& log = {});
}
