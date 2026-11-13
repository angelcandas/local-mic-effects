#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <memory>
#include <vector>

// Rhythm analysis for quantizing: where notes and hits start (onsets), how fast a
// performance goes (tempo) and where its beats fall, for a voice or an instrument.
//
// Plain computation with no audio-thread constraints.
namespace analysis::rhythm
{
// How strongly new sound arrives over time: spectral flux of the log magnitude of a mono
// mix, one value per hop (about 5 ms). values[i] belongs to time i * hopSeconds.
struct OnsetEnvelope
{
    double hopSeconds = 0.005;
    std::vector<float> values;
};

OnsetEnvelope getOnsetEnvelope (const juce::AudioBuffer<float>&, double sampleRate);

struct Onset
{
    double time = 0.0;     // seconds from the buffer's start, at the start of the attack
    float strength = 0.0f; // 0..1, relative to the strongest onset in the buffer
};

// Note and hit starts between `from` and `to` seconds (to < 0: the end). Sensitivity 0..1:
// higher finds softer onsets. Onsets are at least 50 ms apart.
std::vector<Onset> findOnsets (const juce::AudioBuffer<float>&, double sampleRate, double from = 0.0, double to = -1.0,
                               float sensitivity = 0.5f);

// Every note start a take could have, found once at full sensitivity, for hitpoints that
// are then filtered (and hand-corrected) without analysing again. Candidates are at least
// 20 ms apart, in time order.
struct OnsetCandidate
{
    double time = 0.0;     // seconds from the buffer's start, at the start of the attack
    float strength = 0.0f; // 0..1: the flux peak, relative to the strongest in the buffer
    float margin = 0.0f;   // 0..1: how far it stands out (findOnsets keeps margin >= 0.02 + 0.2 * (1 - sensitivity))
    float peak = 0.0f;     // the loudest sample level of its attack (linear, 0..1)
};

std::vector<OnsetCandidate> findOnsetCandidates (const juce::AudioBuffer<float>&, double sampleRate);

// The same, remembered per recording (by its shape and a sparse sample of its audio).
std::shared_ptr<const std::vector<OnsetCandidate>> findOnsetCandidatesCached (const juce::AudioBuffer<float>&, double sampleRate);

// The margin a candidate needs at a threshold (0: every candidate, 1: only clear ones), so
// threshold = 1 - sensitivity picks what findOnsets picks.
inline float marginForThreshold (double threshold) { return 0.02f + 0.2f * (float) juce::jlimit (0.0, 1.0, threshold); }


//==============================================================================
struct Tempo
{
    double bpm = 0.0;           // 0 when no steady beat was found
    double confidence = 0.0;    // 0..1: how much the beat stands out
    std::vector<double> beats;  // every beat followed through the performance, seconds
    int firstDownbeat = 0;      // index into beats of the first beat of a bar (a guess)

    // The straight grid that fits the beats best: beat n at gridStart + n * 60 / bpm.
    double gridStart = 0.0;
};

// Finds the tempo between 60 and 200 BPM (a performance that goes faster or slower can
// come out at half or double), then follows the beats through the performance, so the
// tempo may drift. `beatsPerBar` guides the guess at the downbeat.
Tempo detectTempo (const juce::AudioBuffer<float>&, double sampleRate, int beatsPerBar = 4);

//==============================================================================
// Grid points to quantize to, in seconds, between `from` and `to`.
//
// A straight grid at a tempo: lines every `beats` beats (0.25 = sixteenths in 4/4) from
// `origin` (a time where a beat falls). Swing 0..1 delays every second line, up to the
// triplet feel at 1 (only for steps shorter than a beat).
std::vector<double> makeGrid (double bpm, double beats, double from, double to, double origin = 0.0, double swing = 0.0);

// A groove: lines that follow played beats (from detectTempo), divided into `beats`-long
// steps (or every n-th beat for steps longer than one). Past the first and last beat the
// grid carries on at the nearest beat spacing.
std::vector<double> makeGroove (const std::vector<double>& beats, double steps, double from, double to, double swing = 0.0);
}
