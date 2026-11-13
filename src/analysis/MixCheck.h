#pragma once

#include "MixAssistant.h"

#include <set>

// Mix Check: a report on the mix that says in plain words what is wrong, where, and why it
// matters, with a Fix for what Auto Mix's moves can cure. Plain computation over the
// measurements the Analyser makes (loudness, true peak, stereo, spectrum, clipping, hum),
// so it runs anywhere; the Fix is an ordinary MixPlan (applyMix writes it as one undo step).
//
// The tracks are measured as their sources play (SessionAnalyser at Point::source, shared
// with Auto Mix through the ProfileCache) and the mix as it plays now, through the master
// (Point::stem, mix only). Thresholds are first guesses from common practice, not tuned on
// real songs.
namespace analysis
{
enum class FixKind
{
    none,
    master,     // loudness / true peak: the mastering chain (or the limiter that's there)
    tonal,      // a gentle master EQ towards the typical finished balance
    monoLows,   // imager: mono under 120 Hz
    narrow,     // imager: narrower, so it folds down to mono better
    masking,    // an EQ dip on the track that covers the voice
    clipping,   // input gain down on a clipped track
    hum,        // narrow cuts on the mains hum
    deEss,      // de-esser on a sibilant voice
    voiceLevel  // the voice's fader towards the style's balance
};

const char* getFixKey (FixKind) noexcept;

struct Finding
{
    juce::String key;     // loudness, truePeak, overCompressed, tonalBalance, lowEndStereo, monoLoss, phase, masking,
                          // clipping, hum, sibilance, leadBalance, silenceHead, silenceTail, neverHeard, dcOffset
    int severity = 1;     // 0 note, 1 worth fixing, 2 a problem
    int trackId = 0;      // the track it's about (0: the whole mix)
    int otherTrackId = 0; // masking: the voice that gets covered
    double from = 0.0, to = 0.0; // seconds (both 0: the whole song)
    float value = 0.0f;   // what was measured
    float target = 0.0f;  // what it should be
    int band = -1;        // tonalBalance: 0 low, 1 mids, 2 presence, 3 air; masking and sibilance: the 1/3-octave band
    FixKind fix = FixKind::none;
    juce::String title;   // "Too loud for streaming"
    juce::String detail;  // the numbers, in words
    juce::String why;     // why it matters
};

struct Report
{
    std::vector<Finding> findings; // most severe first
    int score = 100;               // 0..100

    // The headline numbers.
    float lufs = -200.0f, truePeakDb = -200.0f, loudnessRange = 0.0f, plr = 0.0f; // plr: true peak minus loudness
    float targetLufs = -14.0f;
    double seconds = 0.0;
    int tracksChecked = 0;
    bool empty = true; // the mix is silent: nothing was checked
};

// What the check needs to know about the session.
struct CheckContext
{
    MixStyle style = MixStyle::pop;
    float targetLufs = -14.0f; // -14 streaming, -16 dynamic / podcast, -10 loud
    std::map<int, Role> roles;
    std::map<int, juce::String> names;
    std::map<int, float> volumes;     // dB, by track id
    std::set<int> muted;              // left out of every check
    std::map<int, std::vector<fx::ModuleType>> inserts; // by track id (the master is Edit::masterId)
    float masterVolume = 0.0f;
    // The master's insert values by ID (all its inserts together: "limDrive", "meqLowGain"...).
    std::map<juce::String, float> masterValues;
};

Report checkMix (const SessionAnalyser::Result& tracks, const Profile& mix, const CheckContext&);

// The moves that cure one finding (the same measurements as the check ran on).
MixPlan planFix (const Finding&, const SessionAnalyser::Result& tracks, const Profile& mix, const CheckContext&);

// The report as plain text, for saving or pasting.
juce::String reportToText (const Report&, const std::map<int, juce::String>& trackNames);

// 4-6 dB floors and the like per style, exposed for the tests.
float getPeakToLoudnessFloor (MixStyle) noexcept;
}
