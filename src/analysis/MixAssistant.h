#pragma once

#include "Analysis.h"
#include "dsp/FxModule.h"

#include <map>

namespace daw { class Edit; }

// Auto Mix: turns the analysis of a voice and its backing into concrete mixer moves,
// using the effects the app already has. Everything it proposes is ordinary track
// state (volumes and insert values), so you can see it, tweak it and undo it.
//
//   Voice:   parametric EQ (low cut from the lowest sung note, a cut where it's muddy or
//            boxy, a cut where it's harsh or a lift where it lacks presence, air),
//            compressor from how much the level swings, de-esser if the S's stick out.
//   Backing: a parametric EQ dip where it covers the words most.
//   Balance: the voice's volume so it sits a style's number of LU over the backing.
//   Master:  volume towards a loudness target, kept below clipping.
//
// Plain computation: plan() runs anywhere; apply() on the message thread.
namespace analysis
{
enum class Role { voice, backing, other };

enum class MixStyle { pop, ballad, rock, coaching };

struct MixTargets
{
    float voiceOverBackingLu;  // loudness of the voice over the backing while it sings
    float presenceDb;          // tilt of the voice target above 2 kHz (+ is brighter)
    float warmthDb;            // tilt of the voice target in 150-400 Hz (+ is warmer)
    float masterLufs = -14.0f; // integrated loudness of the mix
};

MixTargets getTargets (MixStyle);
juce::String getStyleName (MixStyle);

// A guess from the name and the sound: voice when the name says so or the track is
// mostly pitched; backing when it plays most of the song; otherwise other.
Role guessRole (const juce::String& trackName, const Profile&, double songSeconds);

//==============================================================================
struct MixMove
{
    int trackId = 0;                 // daw::Edit::masterId for the master bus
    std::optional<fx::ModuleType> insert; // empty: the track's (or master's) volume
    std::map<juce::String, float> values; // insert values by ID, or "volume" in dB
    juce::StringArray reasons;       // one line per change, for the UI
};

struct MixPlan
{
    std::vector<MixMove> moves;
    juce::StringArray notes;          // things it couldn't do or measured, for the UI

    // Predictions after the moves (for the UI).
    float voiceOverBackingLu = 0.0f;
    float mixLufs = -200.0f;
};

// Plans the moves from a SessionAnalyser result at Point::source, the roles of the
// tracks and the session's current volumes (dB) by track id.
MixPlan planMix (const SessionAnalyser::Result& analysis, const std::map<int, Role>& roles,
                 const std::map<int, float>& currentVolumes, float currentMasterVolume, MixStyle);

// Writes the plan into the session as one undo step. Inserts it needs are reused when
// the track already has one of that type, otherwise added at the start of the chain.
// `amount` 0..1 scales every move from neutral (EQ gains, compression) or from the
// current volume.
void applyMix (daw::Edit&, const MixPlan&, float amount = 1.0f);
}
