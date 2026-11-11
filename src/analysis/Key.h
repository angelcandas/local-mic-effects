#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <functional>
#include <vector>

// The musical key of a song, found locally from its harmony: each 100 ms of audio
// becomes a chroma vector (how much of each note name sounds, whatever the octave),
// the song's average chroma is compared with the typical note weights of all 24 major
// and minor keys, and the best match wins.
//
// The chroma is kept at a third of a semitone so a song tuned away from A = 440 Hz
// (a 432 Hz recording, a tape running slow) still lands on the right notes.
namespace analysis
{
//==============================================================================
// Energy per third of a semitone over one octave: bin 3n is note n (0 = C ... 11 = B)
// at A = 440 Hz, bins 3n - 1 and 3n + 1 are a third of a semitone below and above it.
inline constexpr int chromaBinsPerNote = 3, numChromaBins = 12 * chromaBinsPerNote;
using Chroma = std::array<float, numChromaBins>;

// Adds the tonal peaks of one magnitude spectrum (bins 0 .. fftSize / 2 of a real FFT
// of `fftSize` samples) between about 60 Hz and 5 kHz into `chroma`, normalised so the
// frame's strongest bin is 1. Returns false (and adds nothing) when nothing tonal
// sounds. Noise and drums spread evenly and mostly cancel out.
bool computeChroma (const float* magnitudes, int fftSize, double sampleRate, Chroma& chroma);

//==============================================================================
struct KeyEstimate
{
    int key = -1;            // tonic, 0 = C ... 11 = B; -1 when the audio has no clear harmony
    bool minor = false;
    float confidence = 0.0f; // 0..1: how far the best key stands out from the next best
    float correlation = 0.0f;// of the song's chroma with the best key's profile, -1..1

    int runnerUpKey = -1;    // the second best key (often the relative major / minor)
    bool runnerUpMinor = false;

    float tuningCents = 0.0f; // the song's tuning against A = 440 Hz, -50 .. 50

    std::array<float, 12> chroma {}; // the song's note weights (max 1), after the tuning correction
    std::array<float, 24> scores {}; // correlation per key: 0..11 major, 12..23 minor

    bool isValid() const noexcept { return key >= 0; }

    // "A minor", "F# major", or "" when invalid.
    juce::String getName() const;
    static juce::String getName (int key, bool minor);

    // The scale index for the autotune (PitchEngine::getScaleNames()): Major or Minor.
    int getAutotuneScale() const noexcept { return minor ? 2 : 1; }
};

// The key from the chroma of many frames, summed (each frame normalised by
// computeChroma). Frames where `use` is false are skipped when it's given.
KeyEstimate estimateKey (const std::vector<Chroma>& frames, const std::vector<bool>* use = nullptr);
KeyEstimate estimateKey (const Chroma& sum);

// Straight from audio: the whole buffer (mono or stereo) in 100 ms hops.
// `shouldContinue` is polled now and then; false stops early (and returns no key).
KeyEstimate detectKey (const juce::AudioBuffer<float>&, double sampleRate,
                       const std::function<bool()>& shouldContinue = {});

// Combines the chroma of several sources (e.g. the tracks of a song), each weighted.
void addChroma (Chroma& into, const Chroma& from, float weight = 1.0f);
Chroma sumChroma (const std::vector<Chroma>& frames, const std::vector<bool>* use = nullptr);
}
