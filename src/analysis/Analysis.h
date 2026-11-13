#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_data_structures/juce_data_structures.h>
#include <juce_dsp/juce_dsp.h>

#include "dsp/PitchDetector.h"
#include "dsp/Smooth.h"
#include "Key.h"

#include <algorithm>
#include <array>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <vector>

namespace daw { class Edit; }

// Measurements for Auto Mix: how loud a signal is (EBU R128 / ITU-R BS.1770), when it
// is active, and how its energy spreads over frequency, sampled every 100 ms. Two
// profiles on the same time grid can be compared, e.g. how much a backing track covers
// the voice while it sings.
//
// Everything here is plain computation with no audio-thread constraints: it runs on a
// background thread over offline renders.
namespace analysis
{
//==============================================================================
// 1/3-octave bands from 20 Hz to 20 kHz (centres 1 kHz * 2^(n/3), n = -16 .. 13).
inline constexpr int numBands = 30;
using Bands = std::array<float, numBands>;

double getBandCentre (int band) noexcept;
double getBandLow (int band) noexcept;   // lower edge (centre / 2^(1/6))
double getBandHigh (int band) noexcept;  // upper edge
int findBand (double frequency) noexcept; // the band holding `frequency`, clamped

// Weight of each band for how much it carries the words of a voice (peaks around
// 2 kHz, near zero below 150 Hz and above 8 kHz); sums to 1.
const Bands& getSpeechImportance() noexcept;

//==============================================================================
// One signal over time.
struct Profile
{
    static constexpr double hopSeconds = 0.1;

    double sampleRate = 48000.0;

    // Per 100 ms hop:
    std::vector<float> momentaryLufs; // K-weighted loudness over the last 400 ms (-inf as -200)
    std::vector<float> rmsDb;         // unweighted level of this hop, dBFS (a full-scale sine is -3 dB)
    std::vector<Bands> bandsDb;       // power per 1/3-octave band over the last ~170 ms, dB re full-scale mean square
    std::vector<bool> active;         // something is playing (see analyse())
    std::vector<float> pitchHz;       // YIN pitch at the end of the hop, 0 when unpitched
    std::vector<Chroma> chroma;       // notes sounding over the last ~170 ms (see Key.h), all zero when nothing tonal

    // Stereo and level checks for Mix Check (see MixCheck.h). Stereo bands: low < 120 Hz, mid
    // 120 Hz - 4 kHz, high above (powers are mean squares per hop of the mid (L+R)/2 and side
    // (L-R)/2 signals; correlation is the L/R correlation coefficient over the hop).
    static constexpr int stereoBands = 3;
    struct StereoHop
    {
        std::array<float, stereoBands> mid {}, side {};
        float correlation = 1.0f;
    };
    bool stereo = false;              // two channels were measured (the fields below are then filled)
    std::vector<StereoHop> stereoHops;
    float truePeakDb = -200.0f;       // inter-sample peak (4x oversampled), dBTP
    float dcOffset = 0.0f;            // the larger of the two channels' mean, linear (0.01 = 1 % of full scale)
    int clippedRuns = 0;              // runs of 3 or more samples at full scale
    std::vector<float> clipTimes;     // where the first (up to 64) of them start, seconds

    float integratedLufs = -200.0f;   // gated, over the whole signal (-200 = silence)
    float shortTermMaxLufs = -200.0f; // loudest 3 s
    float peakDb = -200.0f;           // sample peak, dBFS

    // Mains hum, measured where nothing plays (or in the quietest hops when it never
    // stops): 50 or 60 Hz, 0 when there's none. Per harmonic (1x to 4x the mains),
    // how far that line stands over the spectrum around it, in dB.
    static constexpr int humHarmonics = 4;
    float humHz = 0.0f;
    std::array<float, humHarmonics> humHarmonicsDb {};
    float getHumProminenceDb() const noexcept { return *std::max_element (humHarmonicsDb.begin(), humHarmonicsDb.end()); }

    // Resonances: per hop, the deepest cut the resonance suppressor (Smooth, Smooth.h) at
    // its default settings would be making at the end of the hop, in three ranges.
    enum ResonanceRange { body, harsh, top };   // 500 Hz-2 kHz, 2-8 kHz, 8-18 kHz
    static constexpr int numResonanceRanges = 3;
    std::vector<std::array<float, numResonanceRanges>> resonanceDb;

    // Share of the active hops (or those where `mask` is true) where that range is cut by
    // at least `minimumDb`, and the cut there on the worst of them (`fraction` percentile).
    double getResonantFraction (ResonanceRange, float minimumDb = 3.0f, const std::vector<bool>* mask = nullptr) const;
    float getResonanceDb (ResonanceRange, double fraction = 0.9, const std::vector<bool>* mask = nullptr) const;

    int getNumFrames() const noexcept { return (int) rmsDb.size(); }
    double getActiveSeconds() const;

    // Share of the active frames with a clear pitch (a sung or spoken voice is mostly
    // pitched; a full backing track mostly isn't to a monophonic detector).
    double getPitchedFraction() const;

    // The pitch below which only `fraction` of the pitched active frames lie (0 if none).
    float getPitchPercentile (double fraction) const;

    // Average spectrum (power mean, dB) over the frames where `mask` is true, or over
    // the active frames when no mask is given. -200 in every band when no frame counts.
    Bands getAverageSpectrum (const std::vector<bool>* mask = nullptr) const;

    // Loudness (power mean of the momentary values, LUFS) over the frames where
    // `mask` is true, or over the active frames.
    float getLoudnessOver (const std::vector<bool>* mask = nullptr) const;

    // The chroma summed over the active frames (or those where `mask` is true), and
    // the musical key it points to.
    Chroma getChromaSum (const std::vector<bool>* mask = nullptr) const;
    KeyEstimate getKey (const std::vector<bool>* mask = nullptr) const { return estimateKey (getChromaSum (mask)); }
};

//==============================================================================
// Builds a Profile from audio pushed in blocks of any size (mono or stereo).
class Analyser
{
public:
    explicit Analyser (double sampleRate);

    // `right` may be null for mono (then counted as one channel, not two).
    void push (const float* left, const float* right, int numSamples);

    // Finishes the gating and the activity detection and returns the result.
    Profile finish();

    // Gain of the BS.1770 K-weighting filter at `frequency`, in dB.
    static double getKWeightingDb (double frequency, double sampleRate);

private:
    struct KWeighting
    {
        void setup (double sampleRate);
        double process (int channel, double x) noexcept;
        std::array<double, 5> shelf {}, highPass {}; // b0 b1 b2 a1 a2
        std::array<std::array<double, 4>, 2> state {}; // per channel: shelf z1 z2, high-pass z1 z2
    };

    void endHop();

    Profile profile;
    KWeighting kWeighting;
    int hopSize, hopFill = 0;
    bool stereo = false;
    double hopWeighted[2] {}, hopSquares = 0.0;
    float peak = 0.0f;
    std::vector<double> hopPowers; // K-weighted mean square of each 100 ms hop (channels summed)

    fx::PitchDetector pitch;

    // Mix Check measurements.
    struct Biquad
    {
        void setup (double sampleRate, double frequency, bool highPass);
        double process (double x) noexcept;
        double b0 = 0, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    };
    Biquad msLow[2], msHigh[2]; // [0] mid, [1] side: low-pass at 120 Hz, high-pass at 4 kHz
    double hopMid[Profile::stereoBands] {}, hopSide[Profile::stereoBands] {};
    double hopLL = 0.0, hopRR = 0.0, hopLR = 0.0;
    double sumL = 0.0, sumR = 0.0;
    juce::int64 samplesSeen = 0;
    float history4[2][12] {};  // the last 12 samples per channel, for the true peak
    float truePeak = 0.0f;
    int clipRun[2] {};
    bool clipCounted[2] {};

    static constexpr int fftOrder = 13, fftSize = 1 << fftOrder;
    juce::dsp::FFT fft { fftOrder };
    std::vector<float> history;  // the last fftSize mono samples, as a ring
    int historyPos = 0;
    std::vector<float> window, fftData;
    double windowPower = 0.0;    // sum of squared window values

    // The resonance suppressor, listening only, fed the mono signal a hop at a time.
    fx::ResonanceSuppressor resonance;
    std::vector<float> hopMono;

    // Per hop, per mains frequency (50, 60 Hz) and harmonic: line over its surroundings, dB.
    std::vector<std::array<std::array<float, Profile::humHarmonics>, 2>> humLines;
};

// A whole buffer at once.
Profile analyse (const juce::AudioBuffer<float>&, double sampleRate);

// An audio file (WAV, AIFF, FLAC, Ogg, MP3...), decoded in blocks. Empty (no frames)
// if it can't be read. `shouldContinue` is polled now and then; false stops early.
Profile analyseFile (const juce::File&, const std::function<bool()>& shouldContinue = {});

//==============================================================================
// How a backing track covers a voice, measured over the frames where the voice is
// active. Both profiles must come from the same timeline and sample rate.
struct Masking
{
    Bands backingOverVoiceDb {};       // per band: backing power minus voice power
    float importanceWeightedDb = 0.0f; // the same, weighted by getSpeechImportance()
    int worstBand = -1;                // the important band (0.03+ weight) where the backing covers most
    float voiceOverBackingLu = 0.0f;   // loudness of the voice minus the backing, while the voice sings
    double overlapSeconds = 0.0;       // how long the voice was active
};

Masking measureMasking (const Profile& voice, const Profile& backing);

//==============================================================================
// Profiles already measured, by what was rendered: the track's part of the session
// (clips, notes, instrument, inserts when they count), its audio files' sizes and
// dates, the sample rate and the song's length. "Listen again" then renders only the
// tracks that changed; a fader move changes nothing that's measured. Thread-safe.
class ProfileCache
{
public:
    std::optional<Profile> find (const juce::String& key) const;
    void store (const juce::String& key, const Profile&);
    void clear();
    int size() const;

    static constexpr int maxEntries = 128; // the oldest go first

private:
    mutable std::mutex lock;
    std::map<juce::String, Profile> profiles;
    std::vector<juce::String> order; // oldest first
};

//==============================================================================
// Profiles of every track of a DAW session (and the mix), rendered offline from a
// snapshot like an export, so the session and what you hear are untouched.
class SessionAnalyser
{
public:
    enum class Point
    {
        source, // each track's clips alone: no inserts, unity volume, centre pan, no volume ride
                // (its chain's input gain still counts)
        stem    // each track through its inserts, volume and pan (like an exported stem)
    };

    struct Result
    {
        std::vector<std::pair<int, Profile>> tracks; // by track id, in track order
        std::optional<Profile> mix;                  // the full mix through the master, if asked for
        bool cancelled = false;
    };

    using Progress = std::function<bool (double)>; // 0..1; return false to cancel

    // Message thread: takes a snapshot of the session.
    SessionAnalyser (daw::Edit&, Point, bool includeMix, double sampleRate = 48000.0, bool includeTracks = true);

    // Any thread. The renders run side by side, up to maxThreads at once (offline,
    // faster than real time); `progress` is called from them one at a time.
    // With a cache, tracks it already knows aren't rendered again, and new ones are added.
    // Tracks with a third-party plugin instrument render one at a time (a plugin may
    // not be safe on several threads at once).
    Result run (Progress = {}, ProfileCache* = nullptr);
    static constexpr int maxThreads = 8;

    int getLastRenderCount() const noexcept { return lastRenderCount; } // renders the last run() did

private:
    Profile render (const juce::ValueTree& session, const Progress&, double from, double to, bool& cancelled);

    juce::ValueTree snapshot;
    Point point;
    bool includeMix, includeTracks;
    double sampleRate;
    double songSeconds = 0.0;
    int lastRenderCount = 0;
};
}
