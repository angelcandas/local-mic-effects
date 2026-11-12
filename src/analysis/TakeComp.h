#pragma once

#include "Analysis.h"

#include <juce_data_structures/juce_data_structures.h>

#include <functional>
#include <vector>

// Auto Comp: picks the best parts of a track's takes (see daw::Edit, "Takes and
// comping"), phrase by phrase, the way a producer would listen through them.
//
// Every take is measured on the same 100 ms grid (see Profile). The song is cut into
// phrases where every take is quiet (a breath, a rest), so no cut lands inside a word;
// a phrase that runs on too long without a rest is cut at its quietest moment. In each
// phrase every take that covers it gets a score:
//   - pitch: how close its sung notes sit to the nearest note (of the song's key when
//     it's known), against the takes' common tuning; vibrato counts the same for all
//   - the whole line: a take that's silent where the others sing missed something
//   - clipping: hops that hit full scale
//   - level: far quieter than the other takes (off the mic)
// and a short dynamic programme picks one take per phrase, with a small cost for every
// switch, so close calls stay on the same take instead of cutting back and forth.
//
// Plain computation, for a background thread.
namespace analysis
{
struct TakeAudio
{
    int takeId = -1;
    juce::String name;
    Profile profile;            // over the comp's range, from CompInput::start
    std::vector<bool> covered;  // per hop: the take has audio there
    std::vector<float> peak;    // per hop: sample peak, linear
};

struct CompInput
{
    double start = 0.0;          // timeline seconds of hop 0
    std::vector<TakeAudio> takes;
    int keyTonic = -1;           // the song's key (Edit::getKey), -1 when unknown
    bool keyMinor = false;
};

struct CompPick
{
    double start = 0.0, end = 0.0; // timeline seconds
    int takeId = -1;
    juce::String takeName;
    float score = 0.0f;            // 0..100
    juce::String reason;           // why this take, in a few words
};

struct CompPlan
{
    std::vector<CompPick> picks;   // in time order, joined where the same take carries on
    int phrases = 0;               // phrases judged
    juce::String summary;
};

// Reads a track's takes (a copy of its TRACK tree) and measures them. File reads and
// analysis happen here, so call it off the message thread. `shouldContinue` false stops
// early (the result is then empty).
CompInput measureTakes (const juce::ValueTree& track, const std::function<bool()>& shouldContinue = {});

// Needs at least two takes; returns an empty plan otherwise.
CompPlan planComp (const CompInput&);

// For tests: the cuts between phrases (timeline seconds) the planner would use.
std::vector<double> findPhraseCuts (const CompInput&);

inline constexpr double analysisRate = 24000.0;
inline constexpr double minPhraseSeconds = 1.2;
inline constexpr double maxPhraseSeconds = 10.0;
inline constexpr float switchCost = 3.0f; // score points a cut between takes has to win
}
