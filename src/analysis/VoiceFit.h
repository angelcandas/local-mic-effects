#pragma once

#include "Analysis.h"

#include <utility>
#include <vector>

// Fit a chain to the voice (vocal effects V5): keep the chain's devices and character and
// set only what depends on how loud the voice and the room arrive. A factory vocal chain's
// thresholds are set for a voice at -18 LUFS at the chain's input (played on both sides:
// a mono voice at -21 LUFS); this finds the input
// trim that brings this voice there, and the gate and Classic de-esser thresholds for
// what the voice and the room will be after that trim.
//
// Plain computation on a Profile of the voice as it arrives at the chain now (after the
// chain's input gain): Live's Sound check and the DAW's "Fit to this track" both use it.
namespace analysis::fit
{
inline constexpr float targetLufs = -18.0f;

// What the chain has now.
struct Chain
{
    float inputGainDb = 0.0f;                // the chain's input gain
    float minInputDb = -24.0f, maxInputDb = 24.0f;
    bool gate = false;                       // a gate (or expander) is switched on
    bool classicDeEsser = false;             // the de-esser is on in Classic mode (Smart follows the voice)
    bool denoise = false;                    // the denoiser is on
};

struct Result
{
    float inputGainDb = 0.0f;                // the chain's new input gain
    float trimDb = 0.0f;                     // how much that changes it
    std::vector<std::pair<juce::String, float>> values; // "gateThreshold", "deessThreshold" (dB, after the trim)
    std::vector<std::pair<juce::String, juce::String>> changes; // effect name -> what changed and why
    juce::StringArray notes;                 // what it measured or left alone
    float voiceLufs = -200.0f;               // the voice as it arrives now (LUFS played on both sides)
    float noiseDb = -200.0f;                 // the room between phrases now (dBFS RMS), -200 when there was no pause
    int noiseFrame = -1;                     // the quietest half second without voice (first hop), -1 when none
    juce::String error;                      // set when there was too little voice
};

inline constexpr int noiseFrames = 5;       // half a second of hops

Result plan (const Profile& voice, const Chain& chain, double minVoiceSeconds = 4.0);
}
