#pragma once

#include "Analysis.h"
#include "dsp/FxModule.h"

#include <map>

namespace daw { class Edit; }

// Auto Mix: turns the analysis of a voice and its backing into concrete mixer moves,
// using the effects the app already has. Everything it proposes is ordinary track
// state (volumes and insert values), so you can see it, tweak it and undo it.
//
// Every track has a role, tagged by you before Auto Mix listens (or guessed from its
// name and sound): what it is decides what it gets.
//
//   Voice:      parametric EQ (low cut from the lowest sung note, a cut where it's muddy
//               or boxy, a cut where it's harsh or a lift where it lacks presence, air),
//               compressor from how much the level swings, de-esser if the S's stick out,
//               reverb at the end of the chain when the recording is dry and the style
//               wants space.
//   Background vocals: like a voice, but a higher low cut, readier de-esser, more reverb,
//               and a level a style's number of LU under the lead.
//   Instruments (guitar, piano, bass, drums, other): parametric EQ (low cut under their
//               lowest notes, set per instrument: none for bass and kick, high for a
//               guitar; a dip where they cover the words), compressor when their level
//               swings a lot (always a gentle one on a bass).
//   Backing:    a parametric EQ dip where it covers the words most.
//   Balance:    the voice's volume so it sits a style's number of LU over everything else.
//   Master:  volume towards a loudness target, kept below clipping.
//   Rides:   volume automation on the voices (phrase to phrase) and the instruments
//            (down while the lead sings, up between lines).
//
// Plain computation: plan() runs anywhere; apply() on the message thread.
namespace analysis
{
// voice: the lead (or a sung part); backingVocal: background vocals / harmonies;
// backing: a full instrumental (karaoke track, the band on one track); instrument: any
// other instrument; other: left as it is.
enum class Role { voice, backingVocal, backing, guitar, piano, bass, drums, instrument, other };

inline constexpr Role allRoles[] = { Role::voice, Role::backingVocal, Role::backing, Role::guitar, Role::piano,
                                     Role::bass, Role::drums, Role::instrument, Role::other };

const char* getRoleKey (Role);              // "voice", "background", "instrumental", "guitar"... (project files, UI)
Role getRoleFromKey (const juce::String&);  // other when unknown
juce::String getRoleName (Role);            // "Voice", "Background vocals"...

// The roles played by one instrument (guitar, piano, bass, drums, other instrument).
bool isInstrument (Role) noexcept;

enum class MixStyle { pop, ballad, rock, coaching, hipHop, rnb, acoustic, electronic, jazz, lofi };

inline constexpr MixStyle allStyles[] = { MixStyle::pop, MixStyle::ballad, MixStyle::rock, MixStyle::coaching, MixStyle::hipHop,
                                          MixStyle::rnb, MixStyle::acoustic, MixStyle::electronic, MixStyle::jazz, MixStyle::lofi };

struct MixTargets
{
    float voiceOverBackingLu;  // loudness of the voice over the backing while it sings
    float presenceDb;          // tilt of the voice target above 2 kHz (+ is brighter)
    float warmthDb;            // tilt of the voice target in 150-400 Hz (+ is warmer)
    float masterLufs = -14.0f; // integrated loudness of the mix
    float reverbMix = 0.0f;    // % of reverb on a dry voice (0: none)
    float reverbSize = 50.0f;  // % room size
    float backingVocalUnderLeadLu = 6.0f; // background vocals sit this far under the lead

    // Character, each 0 for none:
    float voiceSaturationDb = 0.0f; // saturation drive on the voice (mixed in at 35%)
    float bassSaturationDb = 0.0f;  // and on the bass, so it's heard on small speakers
    float exciterAmount = 0.0f;     // % exciter on the voice above 4 kHz (air)
    float echoMix = 0.0f;           // % tempo-synced echo on the lead voice
    float echoBeats = 0.5f;         // its time in beats (0.5 = an eighth note)
    float echoFeedback = 25.0f;     // %
    float widthAmount = 100.0f;     // % stereo width on instruments and background vocals (100 = as is)

    // Automation ("volume rides"):
    float rideRangeDb = 4.0f;       // the most the vocal ride moves a phrase up or down
    float duckDb = 1.5f;            // instruments dip this much while the lead sings (0: no dip)

    // Levels for "normalize" (LUFS each track is brought to before the rest is planned).
    // The voices sit at voiceLufs; the others by role.
    float voiceLufs = -18.0f;
    float instrumentalLufs = -18.0f, drumsLufs = -20.0f, bassLufs = -21.0f, guitarLufs = -23.0f,
          pianoLufs = -23.0f, otherInstrumentLufs = -24.0f;

    MixStyle style = MixStyle::pop; // set by getTargets (its name goes in the reasons)
};

// What Auto Mix is asked to do.
//   mix:   the whole song, every track by its role, and the master (global mixing).
//   fit:   one track (your voice or instrument) into a backing that's already mixed:
//          that track gets the full treatment and its level; the others only make room
//          for it (a dip where they cover it, down while it plays). The master stays.
//   clean: clean up and polish one track and nothing else: rumble, hum, noise between
//          phrases, mud, harshness, S's, uneven level. No space, no character, no level.
enum class MixMode { mix, fit, clean };

const char* getModeKey (MixMode);              // "mix", "fit", "clean"
MixMode getModeFromKey (const juce::String&);  // mix when unknown

// Which effects Auto Mix may add. Volumes are always planned.
struct MixOptions
{
    MixMode mode = MixMode::mix;
    int focusTrackId = 0; // the track fit and clean work on (0: the lead voice)

    bool eq = true, compressor = true, deEsser = true, reverb = true;
    bool saturation = true, echo = true, exciter = true, width = true; // used when the style asks for them

    // Volume automation: a ride on each voice that evens out its phrases (what a
    // compressor leaves), and on the instruments a dip while the lead sings, back up
    // between lines.
    bool rides = true;

    // Normalize: first bring every track (voices, instruments) to a level that fits its
    // role in the style, with the chain's input gain ("inGain" moves); the rest of the
    // plan follows from those levels. Tracks left as they are keep theirs.
    bool normalize = false;

    double tempo = 120.0; // BPM, for the echo's time

    // Insert types each track already has (by track id). A voice that already has a
    // reverb or an echo keeps its own space: Auto Mix doesn't add or change one.
    std::map<int, std::vector<fx::ModuleType>> existingInserts;

    // Track names by id, for the notes (optional).
    std::map<int, juce::String> trackNames;
};

MixTargets getTargets (MixStyle);
juce::String getStyleName (MixStyle);

// A guess from the track's name alone ("Lead vocal", "Kick", "Bajo"...), or nothing
// when the name says nothing (Track 3). This is the tag the panel starts with, before
// anything has been listened to.
std::optional<Role> guessRoleFromName (const juce::String& trackName);

// A guess from the name, then the sound: voice when mostly pitched; drums when short
// unpitched hits; bass when nearly all of it is under 250 Hz; backing when it plays
// most of the song; otherwise instrument, or other when it's silent.
Role guessRole (const juce::String& trackName, const Profile&, double songSeconds);

//==============================================================================
struct MixMove
{
    int trackId = 0;                 // daw::Edit::masterId for the master bus
    std::optional<fx::ModuleType> insert; // empty: the track's (or master's) "volume", or a change
                                          // to its chain's input gain "inGain" (dB, normalize)
    bool atEnd = false;              // a new insert goes after the track's own effects (reverb)
    int instance = 0;                // which insert of that type (0: the first; 1: a second one, added if missing)
    std::map<juce::String, float> values; // insert values by ID, or "volume" in dB
    std::vector<std::pair<double, float>> ride; // volume automation (seconds, dB), when not empty (no insert, no values)
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
                 const std::map<int, float>& currentVolumes, float currentMasterVolume, MixStyle,
                 const MixOptions& = {});

// Matches the mix to a reference song you like: a gentle master EQ (shelves and three
// wide bells) towards the reference's long-term tonal balance, and the master volume
// towards its loudness, held below clipping. `mix` is the session's mix as it plays
// now (SessionAnalyser with includeMix), at `currentMasterVolume`.
MixPlan planReferenceMatch (const Profile& mix, const Profile& reference, float currentMasterVolume);

// Writes the plan into the session as one undo step. Inserts it needs are reused when
// the track already has one of that type (or that many, for a move's instance), otherwise added at the start of the chain
// (at the end for a move with atEnd). `amount` 0..1 scales every move from neutral
// (EQ gains, compression, reverb mix) or from the current volume.
void applyMix (daw::Edit&, const MixPlan&, float amount = 1.0f);
}
