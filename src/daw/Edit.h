#pragma once

#include <juce_data_structures/juce_data_structures.h>

#include "dsp/FxModule.h"
#include "TempoMap.h"
#include "Warp.h"

#include <optional>

// The DAW session as a ValueTree, so every change can be undone and the whole thing
// saves to a file as it is:
//
//   PROJECT  version, tempo, tempoChanges, beatsPerBar, loopOn, loopStart, loopEnd
//     MARKER  id, name, time
//     MASTER  volume (dB)
//       INSERTS
//         INSERT  type ("eq", "echo", ...), id, plus one property per module value
//     TRACK  id, name, colour, volume (dB), pan (-1..1), mute, solo, arm, monitor, transpose,
//            kind ("midi" for MIDI tracks; audio when missing), mixRole (Auto Mix's tag:
//            "voice", "drums"... see analysis::getRoleKey; missing when not tagged),
//            ride (volume automation, see RidePoint; missing when none),
//            output (the RETURN id the track plays into instead of the master; missing = master)
//       INSERTS ...
//       SEND  id, target (a RETURN's id), level (dB), pre (true = before the fader)
//       INSTRUMENT  id, plugin, name, format, vendor, description, state   (MIDI tracks)
//       CLIP  id, name, file, start, offset, length, gain (dB), fadeIn, fadeOut, mute,
//             transpose (semitones), warp (warp::Map as text),
//             stretch (warp::Algorithm key; mix when missing), formant (keep formants)
//       CLIP  kind "midi", id, name, start, offset, length, mute          (MIDI clips)
//         NOTE  id, pitch (0..127), time, length, velocity (1..127)
//     RETURN  id, name, colour, volume, pan, mute, solo, inGain, outGain
//       INSERTS ...
//
// Return tracks (aux buses) hold shared effects: tracks send some of their signal to
// them with SENDs, or play into one entirely with `output` (a group bus). Returns play
// into the master. A new project starts with a Reverb return and a Chorus return
// (PROJECT `defaultReturns` says they were made, so deleting them sticks).
//
// A MIDI track plays its MIDI clips through its instrument (a built-in synth or a
// VST3/AU plugin, see Instruments.h), then through its inserts like any track. An
// INSTRUMENT's `plugin` is its id ("builtin:keys", or the plugin's identifier string),
// `description` the plugin's PluginDescription as XML (to find it again), and `state`
// the plugin's own settings, base64. A MIDI clip is a window onto its notes, like an
// audio clip onto its file: note times are seconds from the start of the clip's
// content, and `offset` says where the clip starts within it.
//
// A clip with a warp map or a transpose (its own plus its track's) plays a rendered copy
// of its file (see Warp.h); its offset and length are then measured in that copy. The
// file itself is never changed.
//
// `tempo` is the tempo at the start; `tempoChanges` the tempo points after it (see
// TempoMap::changesToString; missing when the tempo is steady).
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
    MICFX_DAW_ID (kind) MICFX_DAW_ID (INSTRUMENT) MICFX_DAW_ID (plugin) MICFX_DAW_ID (format) MICFX_DAW_ID (vendor)
    MICFX_DAW_ID (description) MICFX_DAW_ID (state) MICFX_DAW_ID (NOTE) MICFX_DAW_ID (pitch) MICFX_DAW_ID (velocity)
    MICFX_DAW_ID (inGain) MICFX_DAW_ID (outGain)
    MICFX_DAW_ID (key) MICFX_DAW_ID (keyMinor) MICFX_DAW_ID (mixRole) MICFX_DAW_ID (ride)
    MICFX_DAW_ID (RETURN) MICFX_DAW_ID (SEND) MICFX_DAW_ID (target) MICFX_DAW_ID (level) MICFX_DAW_ID (pre)
    MICFX_DAW_ID (output) MICFX_DAW_ID (defaultReturns)
    MICFX_DAW_ID (stretch) MICFX_DAW_ID (formant) MICFX_DAW_ID (tempoChanges)
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
    void setMasterPan (float pan);

    //==============================================================================
    // Return tracks. Their ids share the track id space; the mixer setters above
    // (volume, pan, mute, solo), the chain gains, inserts and copyInserts take a
    // return's id too.
    juce::ValueTree addReturn (const juce::String& name, std::optional<fx::ModuleType> effect = std::nullopt);
    void removeReturn (int returnId); // also removes the sends to it, and routes its tracks back to the master
    juce::ValueTree getReturn (int returnId) const;
    juce::Array<juce::ValueTree> getReturns() const;
    void moveReturn (int returnId, int newIndex);

    // A track's send to a return, in dB (-inf / below minSendDb is off). Pre-fader sends
    // ignore the track's volume and pan (they still go quiet when it's muted).
    static constexpr float minSendDb = -60.0f, maxSendDb = 6.0f;
    void setSend (int trackId, int returnId, float decibels);
    void setSendPre (int trackId, int returnId, bool preFader);
    static juce::ValueTree getSend (const juce::ValueTree& track, int returnId);

    // Where a track plays: the master (masterId) or a return's id.
    void setTrackOutput (int trackId, int returnIdOrMaster);
    static int getTrackOutput (const juce::ValueTree& track) { return (int) track.getProperty (ids::output, masterId); }

    // A track, return or the master by id (whatever the mixer shows as a strip).
    juce::ValueTree getChannel (int id) const;

    // The insert chain's input and output gain (before and after its effects), in dB,
    // -24..24. On a track, or the master with masterId.
    static constexpr float maxChainGain = 24.0f;
    void setChainGain (int trackId, bool output, float decibels);

    // Recording: armed tracks get the take; monitored tracks hear the mic live.
    void setTrackArm (int trackId, bool);
    void setTrackMonitor (int trackId, bool);

    // The tempo at the start of the song (the whole song while it's steady).
    void setTempo (double bpm);
    double getTempo() const { return state.getProperty (ids::tempo, 120.0); }

    // The tempo over the song: points with steps or ramps (see TempoMap). Setting one
    // replaces the start tempo and every change, as one undoable edit.
    TempoMap getTempoMap() const;
    void setTempoMap (const TempoMap&);

    // Sets the tempo of the point in force at a time (the start tempo while it's steady).
    void setTempoAt (double seconds, double bpm);

    // Time signature: beats in a bar (2..12), for the click's accent, bar numbers and
    // the count-in. The tempo counts these beats.
    void setBeatsPerBar (int beats);
    int getBeatsPerBar() const { return juce::jlimit (2, 12, (int) state.getProperty (ids::beatsPerBar, 4)); }

    // The song's key (detected or picked): tonic 0 = C ... 11 = B, or -1 when unknown.
    void setKey (int tonic, bool minor);
    int getKey() const;
    bool isKeyMinor() const { return (bool) state.getProperty (ids::keyMinor, false); }

    //==============================================================================
    // Markers: named points on the timeline (verse, chorus...). Times in seconds.
    juce::ValueTree addMarker (double timeSeconds, const juce::String& name = {});
    void removeMarker (int markerId);
    void setMarkerTime (int markerId, double timeSeconds);
    void setMarkerName (int markerId, const juce::String& name);
    juce::Array<juce::ValueTree> getMarkers() const; // in time order

    //==============================================================================
    // MIDI tracks, instruments and notes.
    struct InstrumentInfo
    {
        juce::String id, name, format, vendor; // format: "builtin", "VST3", "AU"...
        juce::String description;               // the plugin's PluginDescription as XML (plugins)
        juce::String state;                     // base64 settings to start from (optional)
    };

    static constexpr const char* midiKind = "midi";
    static bool isMidiTrack (const juce::ValueTree& track) { return track[ids::kind].toString() == midiKind; }
    static bool isMidiClip (const juce::ValueTree& clip)   { return clip[ids::kind].toString() == midiKind; }

    juce::ValueTree addMidiTrack (const juce::String& name, const InstrumentInfo&, int index = -1);
    void setInstrument (int trackId, const InstrumentInfo&); // replaces the track's instrument (a MIDI track)
    juce::ValueTree getInstrument (int trackId) const;

    // An empty MIDI clip on a MIDI track. Returns it, or an invalid tree.
    juce::ValueTree addMidiClip (int trackId, double startSeconds, double lengthSeconds, const juce::String& name = {});

    // Notes in a MIDI clip; `time` is seconds from the start of the clip's content (so a
    // note at the clip's left edge has time == the clip's offset).
    juce::ValueTree addNote (int clipId, int pitch, double time, double length, int velocity = 100);
    void setNote (int noteId, int pitch, double time, double length, int velocity);
    void removeNote (int noteId);
    juce::ValueTree getNote (int noteId) const;

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

    // How the clip is stretched and transposed (see warp::Algorithm), and whether a
    // transpose keeps the voice's formants.
    void setClipStretch (int clipId, warp::Algorithm);
    void setClipFormant (int clipId, bool);
    void setTrackTranspose (int trackId, double semitones);

    //==============================================================================
    // Volume automation ("ride"): a gain in dB over time, on top of the track's fader,
    // after its inserts. Straight lines between the points (seconds, in time order);
    // before the first and after the last, their values hold. Stored on the TRACK as
    // text "time:dB;time:dB...". An empty list removes it. One undo step.
    struct RidePoint { double time = 0.0; float db = 0.0f; };
    static constexpr float maxRideDb = 24.0f;
    void setVolumeRide (int trackId, const std::vector<RidePoint>&);
    static std::vector<RidePoint> getVolumeRide (const juce::ValueTree& track);
    static float getRideDbAt (const std::vector<RidePoint>&, double time);
    void setClipWarp (int clipId, const warp::Map&);
    static warp::Map getClipWarp (const juce::ValueTree& clip);

    // What the clip's rendered copy needs: its warp map and its transpose plus its track's.
    static warp::Settings getWarpSettings (const juce::ValueTree& clip);

    // Replaces a clip with copies that play only the given parts of it (seconds from the
    // clip's start), each with short fades. For strip silence. Returns the new clip ids.
    juce::Array<int> keepClipParts (int clipId, const std::vector<std::pair<double, double>>& parts, double fadeSeconds = 0.01);

    // The nearest grid line on the tempo map, `beats` apart (1 = quarter notes,
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
    void renumber (juce::ValueTree&); // fresh ids for a copied tree and everything in it
    juce::ValueTree findById (const juce::Identifier& type, int id) const;
    juce::ValueTree getTrackOrMaster (int trackId) const { return getChannel (trackId); }
    void addDefaultReturns();

    juce::ValueTree state;
    juce::UndoManager undoManager;
    juce::UndoManager* undoTarget = &undoManager; // null while building parts that aren't edits
    int nextId = 1; // shared by tracks, clips and inserts; never reused

    JUCE_DECLARE_NON_COPYABLE (Edit)
};
}
