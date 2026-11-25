#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <vector>

// Vocal alignment: line a double, a harmony or a re-sung line up in time with a lead (the
// guide), syllable by syllable, the way a careful editor would by hand.
//
// Both recordings are turned into frames of what they sound like (the shape of the
// spectrum, how strongly new sound arrives, how loud), and a path is found through every
// shift the dub could have against the guide (within maxShift either way), frame by frame:
// it pays for sounding different, for changing its shift and, a little, for shifting at
// all. Where the dub is quiet the path costs nothing, so the shift is free to change in
// the gaps between phrases. The path is then smoothed and turned into a few warp markers
// (dub time -> where it should play), kept within stretch limits so the voice never
// sounds stretched.
//
// Plain computation: runs anywhere. analysis::Alignment is the other kind of lining up:
// the phase of two mics on one sound, a few ms.
namespace analysis::align
{
// What a recording sounds like every `hop` seconds. Frame i is at i * hop seconds of the
// buffer it came from.
struct Features
{
    double hop = 0.01;
    int bands = 0;
    std::vector<float> shape; // size() * bands: the spectral envelope in bands (without the harmonics' comb), less its mean, at length 1
    std::vector<float> onset; // 0..1: how strongly new sound arrives (relative to the loudest attack)
    std::vector<float> fall;  // 0..1: how strongly sound dies away (the same, for note ends)
    std::vector<float> level; // dB (RMS of the frame)

    int size() const noexcept { return (int) level.size(); }
    float loudLevel() const;  // the level the loud tenth of the frames reach (dB)
};

// Mono (channels averaged). Any sample rate from 8 kHz.
Features analyse (const juce::AudioBuffer<float>&, double sampleRate);

struct Options
{
    double strength = 1.0;    // 0..1: how far the dub moves towards the guide
    double maxShift = 0.25;   // seconds: the farthest a moment of the dub moves, either way
    double maxStretch = 1.5;  // while the dub sings: no stretch plays faster or slower than this (1.05..4)
    double gapStretch = 4.0;  // the same between phrases, where it's silence that stretches
    float silenceDb = 40.0f;  // dub frames this far under its loud level are gaps
    double tolerance = 0.008; // seconds: how far the markers may stray from the smoothed path
};

// One move: the moment `source` seconds into the dub plays at `target` seconds.
struct Marker
{
    double source = 0.0, target = 0.0;
};

struct Result
{
    std::vector<Marker> markers;  // in time order; empty when nothing should move
    std::vector<float> shift;     // per dub frame from `from`: the smoothed shift found (seconds, full strength; 0 in gaps)
    float match = 0.0f;           // 0..1: share of the dub's sung frames that found the same sound in the guide
    float chance = 0.0f;          // the match against a part of the guide it doesn't sing (0 when the clip is too short to tell)
    bool related = false;         // the dub sings what the guide sings (match >= minMatch, and minLead over chance)
    double meanShift = 0.0;       // seconds, the average size of the move over the sung frames (full strength)
    double largestShift = 0.0;    // seconds, the largest
};

inline constexpr float minMatch = 0.5f, minLead = 0.2f;

// Lines the dub's frames between `from` and `to` seconds (to < 0: its end) up with the
// guide. Both feature sets must share one timeline and hop (frame i of each at the same
// moment), as two tracks of a song analysed from 0 s. Marker times are on that timeline.
Result align (const Features& guide, const Features& dub, const Options& = {}, double from = 0.0, double to = -1.0);

// Both recordings from the same moment, at the same rate.
Result align (const juce::AudioBuffer<float>& guide, const juce::AudioBuffer<float>& dub, double sampleRate, const Options& = {});

// Where a moment of the dub lands through the markers: piecewise linear between them,
// shifted like the nearest one outside them (as a warp map plays it).
double mapTime (const std::vector<Marker>&, double seconds);
}
