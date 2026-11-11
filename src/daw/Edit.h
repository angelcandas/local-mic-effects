#pragma once

#include <juce_data_structures/juce_data_structures.h>

#include "dsp/FxModule.h"
#include "Warp.h"

#include <optional>

// The DAW session as a ValueTree, so every change can be undone and the whole thing
// saves to a file as it is:
//
//   PROJECT  version, tempo, beatsPerBar, loopOn, loopStart, loopEnd
//     MARKER  id, name, time
//     MASTER  volume (dB)
//       INSERTS
//         INSERT  type ("eq", "echo", ...), id, plus one property per module value
//     TRACK  id, name, colour, volume (dB), pan (-1..1), mute, solo, arm, monitor, transpose
//       INSERTS ...
//       CLIP  id, name, file, start, offset, length, gain (dB), fadeIn, fadeOut, mute,
//             transpose (semitones), warp (warp::Map as text)
//
// A clip with a warp map or a transpose (its own plus its track's) plays a rendered copy
// of its file (see Warp.h); its offset and length are then measured in that copy. The
// file itself is never changed.
//
// Times are in seconds, so a project survives a sample-rate change. Insert values use
// the plugin parameter IDs (see Parameters.cpp) in real units, as presets do.
//
// Message thread only. DawEngine listens to the tree and keeps the audio in step.
namespace daw
{
namespace ids
{
#define MICFX_DAW_ID(name) inline const juce::Identifier name { #name };
    MICFX_DAW_ID (PROJECT) MICFX_DAW_ID (MASTER) MICFX_DAW_ID (TRACK) MICFX_DAW_ID (CLIP)
    MICFX_DAW_ID (INSERTS) MICFX_DAW_ID (INSERT)
    MICFX_DAW_ID (version) MICFX_DAW_ID (tempo) MICFX_DAW_ID (id) MICFX_DAW_ID (name) MICFX_DAW_ID (colour)
    MICFX_DAW_ID (volume) MICFX_DAW_ID (pan) MICFX_DAW_ID (mute) MICFX_DAW_ID (solo) MICFX_DAW_ID (arm)
    MICFX_DAW_ID (monitor) MICFX_DAW_ID (type) MICFX_DAW_ID (file) MICFX_DAW_ID (start) MICFX_DAW_ID (offset)
    MICFX_DAW_ID (length) MICFX_DAW_ID (gain) MICFX_DAW_ID (fadeIn) MICFX_DAW_ID (fadeOut)
    MICFX_DAW_ID (loopOn) MICFX_DAW_ID (loopStart) MICFX_DAW_ID (loopEnd)
    MICFX_DAW_ID (beatsPerBar) MICFX_DAW_ID (MARKER) MICFX_DAW_ID (time)
    MICFX_DAW_ID (transpose) MICFX_DAW_ID (warp)
    MICFX_DAW_ID (inGain) MICFX_DAW_ID (outGain)
#undef MICFX_DAW_ID
}

class Edit
{
public:
    static constexpr int formatVersion = 1;
    static constexpr double minClipSeconds = 0.01;

    Edit();

    juce::ValueTree& getState() noexcept           { return state; }
    juce::UndoManager& getUndoManager() noexcept   { return undoManager; }
    juce::ValueTree getMaster() const              { return state.getChildWithName (ids::MASTER); }

    // Replaces the whole session (e.g. when opening a project). Clears the undo history.
    void replaceState (const juce::ValueTree& newState);

    //==============================================================================
    // Tracks are identified by their `id` property, which stays the same for the
    // track's lifetime (and in saved projects).
    juce::ValueTree addTrack (const juce::String& name, int index = -1);
    void removeTrack (int trackId);
    void moveTrack (int trackId, int newIndex);
    juce::ValueTree getTrack (int trackId) const;

    // A copy of the track right below it, with its clips, inserts and mixer settings
    // (not armed). Returns the new track's id, or -1.
    int duplicateTrack (int trackId);

    // Replaces a track's (or the master's) inserts with copies of another's.
    void copyInserts (int fromTrackId, int toTrackId);

    // Solo just this track (or nothing, with -1), and clear every mute and solo.
    void soloExclusive (int trackId);
    void clearMuteAndSolo();
    juce::Array<juce::ValueTree> getTracks() const;

    // Mixer controls. Volume in dB (-inf is muted), pan -1 (left) .. 1 (right).
    void setTrackVolume (int trackId, float decibels);
    void setTrackPan (int trackId, float pan);
    void setTrackMute (int trackId, bool);
    void setTrackSolo (int trackId, bool);
    void setMasterVolume (float decibels);

    // The insert chain's input and output gain (before and after its effects), in dB,
    // -24..24. On a track, or the master with masterId.
    static constexpr float maxChainGain = 24.0f;
    void setChainGain (int trackId, bool output, float decibels);

    // Recording: armed tracks get the take; monitored tracks hear the mic live.
    void setTrackArm (int trackId, bool);
    void setTrackMonitor (int trackId, bool);

    void setTempo (double bpm);
    double getTempo() const { return state.getProperty (ids::tempo, 120.0); }

    // Time signature: beats in a bar (2..12), for the click's accent, bar numbers and
    // the count-in. The tempo counts these beats.
    void setBeatsPerBar (int beats);
    int getBeatsPerBar() const { return juce::jlimit (2, 12, (int) state.getProperty (ids::beatsPerBar, 4)); }

    //==============================================================================
    // Markers: named points on the timeline (verse, chorus...). Times in seconds.
    juce::ValueTree addMarker (double timeSeconds, const juce::String& name = {});
    void removeMarker (int markerId);
    void setMarkerTime (int markerId, double timeSeconds);
    void setMarkerName (int markerId, const juce::String& name);
    juce::Array<juce::ValueTree> getMarkers() const; // in time order

    //==============================================================================
    juce::ValueTree addClip (int trackId, const juce::File&, double startSeconds, double lengthSeconds,
                             double offsetSeconds = 0.0);
    void removeClip (int clipId);
    juce::ValueTree getClip (int clipId) const;

    // Clip editing. Everything is non-destructive: clips are windows onto their files.
    // Times are in seconds on the timeline.
    void moveClip (int clipId, double newStart, int toTrackId = -1);  // -1 = same track
    void trimClipStart (int clipId, double newStart);  // keeps the audio in place; can't go before the file's start
    void trimClipEnd (int clipId, double newEnd);
    int splitClip (int clipId, double time);           // returns the new (right-hand) clip, or -1
    int duplicateClip (int clipId);                    // placed right after the original
    void setClipGain (int clipId, float decibels);
    void setClipFades (int clipId, double fadeInSeconds, double fadeOutSeconds);
    void setClipName (int clipId, const juce::String&);
    void setClipMute (int clipId, bool);

    // Pitch and time warp, non-destructive. Transpose in semitones (fractions are cents),
    // -24..24. A clip's warp map says where each moment of its file plays (see Warp.h);
    // setting one keeps the clip's window where it is, so the audio moves under it.
    void setClipTranspose (int clipId, double semitones);
    void setTrackTranspose (int trackId, double semitones);
    void setClipWarp (int clipId, const warp::Map&);
    static warp::Map getClipWarp (const juce::ValueTree& clip);

    // What the clip's rendered copy needs: its warp map and its transpose plus its track's.
    static warp::Settings getWarpSettings (const juce::ValueTree& clip);

    // Replaces a clip with copies that play only the given parts of it (seconds from the
    // clip's start), each with short fades. For strip silence. Returns the new clip ids.
    juce::Array<int> keepClipParts (int clipId, const std::vector<std::pair<double, double>>& parts, double fadeSeconds = 0.01);

    // The nearest grid line at the project tempo, `beats` apart (1 = quarter notes,
    // 4 = bars in 4/4, 0.5 = eighths).
    double snapToGrid (double seconds, double beats) const;

    // Loop range: while on, playback wraps from the end back to the start.
    void setLoop (bool on, double startSeconds, double endSeconds);

    //==============================================================================
    // Inserts on a track, or on the master bus with trackId == masterId. A new insert
    // starts from the module's defaults and switched on.
    static constexpr int masterId = 0;

    juce::ValueTree addInsert (int trackId, fx::ModuleType, int index = -1);
    void removeInsert (int insertId);
    void moveInsert (int insertId, int newIndex);
    void setInsertValue (int insertId, const juce::String& valueId, float value);
    juce::ValueTree getInsert (int insertId) const;

    static juce::ValueTree getInserts (const juce::ValueTree& trackOrMaster);

    // Every module type by its project-file key ("eq", "echo", ...).
    static std::optional<fx::ModuleType> moduleTypeFromKey (const juce::String&);

private:
    int takeNextId();
    juce::ValueTree findById (const juce::Identifier& type, int id) const;
    juce::ValueTree getTrackOrMaster (int trackId) const;

    juce::ValueTree state;
    juce::UndoManager undoManager;
    int nextId = 1; // shared by tracks, clips and inserts; never reused

    JUCE_DECLARE_NON_COPYABLE (Edit)
};
}
