#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <functional>
#include <vector>

// The song map: a song's beats, bars and sections (intro, verse, chorus...), found
// locally from its audio.
//
// 1. Beats: rhythm::detectTempo follows the beats through the performance (the tempo may
//    drift, as in a take played without a click).
// 2. Per beat: what sounds (chroma, see Key.h), the tone colour (1/3-octave bands) and
//    the loudness.
// 3. Downbeats: the beat of the bar where the chords change and the low end hits most.
// 4. Per bar, the features above; bars compared with each other (a self-similarity
//    matrix); where the music changes (novelty) gives the section boundaries, preferring
//    sections of 4, 8 or 16 bars.
// 5. Sections that sound alike get the same letter; the loudest repeated one is the
//    chorus, the other repeated ones verses, a one-off in the middle the bridge, and a
//    one-off first or last the intro or outro.
//
// Plain computation on a background thread; no model, no network.
namespace analysis::structure
{
struct Section
{
    juce::String name;   // "Intro", "Verse 1", "Chorus 2", "Bridge", "Outro", "Part A"...
    juce::String kind;   // "intro", "verse", "chorus", "bridge", "outro", "part"
    int letter = 0;      // sections that sound alike share it: 0 = A, 1 = B...
    int firstBar = 0;    // index into SongMap::downbeats
    int bars = 0;
    double start = 0.0, end = 0.0; // seconds
    float loudnessDb = -200.0f;    // mean level over the section, dBFS

    juce::String getLetterName() const { return juce::String::charToString ((juce::juce_wchar) ('A' + juce::jlimit (0, 25, letter))); }

    // The colour the app shows it in ("#rrggbb"): by kind, or by letter for plain parts.
    juce::String getColour() const;
};

struct SongMap
{
    double bpm = 0.0;               // overall (a straight line through the beats); 0 when no steady beat
    double tempoConfidence = 0.0;   // 0..1, from rhythm::detectTempo
    int beatsPerBar = 4;
    std::vector<double> beats;      // every beat followed through the song, seconds
    int firstDownbeat = 0;          // index into beats of the first beat of a bar
    std::vector<double> downbeats;  // the start of every bar, seconds (the last one ends the last bar)
    std::vector<Section> sections;  // in time order, back to back, covering the bars
    bool cancelled = false;

    bool hasBeats() const noexcept { return beats.size() >= 4 && bpm > 0.0; }
    int getNumBars() const noexcept { return juce::jmax (0, (int) downbeats.size() - 1); }

    // The section at a time, or -1.
    int getSectionAt (double seconds) const noexcept;

    // The tempo of each bar, BPM.
    std::vector<double> getBarTempos() const;

    // How much the tempo moves: the spread of the bar tempos (standard deviation over the
    // mean, 0.01 = 1 %). A take played to a click stays under about 0.005.
    double getTempoDrift() const;
};

struct Options
{
    int beatsPerBar = 4;
    int minSectionBars = 2, maxSectionBars = 32;
};

// The whole song (mono or stereo). `shouldContinue` is polled now and then; false stops
// early (the result is then marked cancelled and empty).
SongMap analyse (const juce::AudioBuffer<float>&, double sampleRate, const Options& = {},
                 const std::function<bool()>& shouldContinue = {});

//==============================================================================
// The pieces, for tests and for callers that already have beats.

// Per beat features (from beats[i] to beats[i + 1]; the last beat lasts one beat period).
struct BeatFeatures
{
    std::vector<std::array<float, 12>> chroma; // max 1 per beat, all zero when nothing tonal
    std::vector<std::array<float, 12>> timbre; // log band energies (bands grouped to 12), mean removed
    std::vector<float> levelDb;                // RMS, dBFS
    std::vector<float> lowOnset;               // how much new low end (< 150 Hz) arrives right at the beat
    std::vector<float> chordChange;            // 0..1: how different the harmony is from the beat before
};

BeatFeatures measureBeats (const juce::AudioBuffer<float>&, double sampleRate, const std::vector<double>& beats,
                           const std::function<bool()>& shouldContinue = {});

// Which beat (0 .. beatsPerBar - 1) starts the bars: where the harmony changes and the
// low end hits. `hint` (detectTempo's guess) breaks near ties.
int findDownbeatPhase (const BeatFeatures&, int beatsPerBar, int hint = 0);

// Per bar: the chroma of each of its beats in a row (what is played and in what order),
// and the bar's mean chroma, tone colour and level.
struct BarFeatures
{
    int beatsPerBar = 4;
    std::vector<std::vector<float>> chromaSequence; // 12 * beatsPerBar values per bar
    std::vector<std::array<float, 12>> chroma, timbre;
    std::vector<float> levelDb;

    int size() const noexcept { return (int) levelDb.size(); }
};

BarFeatures getBarFeatures (const BeatFeatures&, int firstDownbeat, int beatsPerBar, int numBars);

// How different two bars are: harmony in order, tone colour (6 dB = 1) and level (6 dB = 1).
// About 0.3 between bars of the same part, 1.5 or more between different parts.
float getBarDistance (const BarFeatures&, int a, int b);

// Sections from bar features (seconds left at 0: the caller fills start and end).
std::vector<Section> findSections (const BarFeatures&, const Options& = {});
}
