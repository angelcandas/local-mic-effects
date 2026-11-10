#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_data_structures/juce_data_structures.h>
#include <juce_dsp/juce_dsp.h>

#include "dsp/PitchDetector.h"

#include <array>
#include <functional>
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

    float integratedLufs = -200.0f;   // gated, over the whole signal (-200 = silence)
    float shortTermMaxLufs = -200.0f; // loudest 3 s
    float peakDb = -200.0f;           // sample peak, dBFS

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

    static constexpr int fftOrder = 13, fftSize = 1 << fftOrder;
    juce::dsp::FFT fft { fftOrder };
    std::vector<float> history;  // the last fftSize mono samples, as a ring
    int historyPos = 0;
    std::vector<float> window, fftData;
    double windowPower = 0.0;    // sum of squared window values
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
// Profiles of every track of a DAW session (and the mix), rendered offline from a
// snapshot like an export, so the session and what you hear are untouched.
class SessionAnalyser
{
public:
    enum class Point
    {
        source, // each track's clips alone: no inserts, unity volume, centre pan
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

    // Any thread.
    Result run (Progress = {});

private:
    Profile render (const juce::ValueTree& session, const Progress&, double from, double to, bool& cancelled);

    juce::ValueTree snapshot;
    Point point;
    bool includeMix, includeTracks;
    double sampleRate;
    double songSeconds = 0.0;
};
}
