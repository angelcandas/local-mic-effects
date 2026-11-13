#pragma once

#include <juce_data_structures/juce_data_structures.h>

#include "dsp/FxModule.h"
#include "TempoMap.h"
#include "Warp.h"

#include <map>
#include <utility>
#include <optional>

// The DAW session as a ValueTree, so every change can be undone and the whole thing
// saves to a file as it is:
//
//   PROJECT  version, tempo, tempoChanges, beatsPerBar, loopOn, loopStart, loopEnd, viewState
//     MARKER  id, name, time
//     MASTER  volume (dB)
//       INSERTS
//         INSERT  type ("eq", "echo", ...), id, plus one property per module value
//         INSERT  type "plugin", id, pluginOn, pluginInGain, pluginOutGain, plugin, name, format,
//                 vendor, description, state   (a VST3/AU effect, see PluginInsert.h)
//     TRACK  id, name, colour, volume (dB), pan (-1..1), mute, solo, arm, monitor, transpose,
//            kind ("midi" for MIDI tracks; audio when missing), mixRole (Auto Mix's tag:
//            "voice", "drums"... see analysis::getRoleKey; missing when not tagged),
//            ride (volume automation, see RidePoint; missing when none),
//            output (the RETURN id the track plays into instead of the master; missing = master),
//            takesShown (its take lanes are unfolded), listenTake (a TAKE id heard instead of the comp),
//            polarity (true: its audio clips play upside down), nudge (ms its audio clips play later; < 0 earlier)
//       INSERTS ...
//       SEND  id, target (a RETURN's id), level (dB), pre (true = before the fader)
//       INSTRUMENT  id, plugin, name, format, vendor, description, state   (MIDI tracks)
//       CLIP  id, name, file, start, offset, length, gain (dB), fadeIn, fadeOut, mute,
//             transpose (semitones), warp (warp::Map as text),
//             stretch (warp::Algorithm key; mix when missing), formant (keep formants),
//             musical (follows the tempo map), clipTempo (the file's own tempo, BPM)
//             pitchNotes (the sung notes and their edits, see analysis::pitch::toString;
//                         missing when the take's notes aren't edited)
//             take (the TAKE this piece of the comp came from; missing when none)
//       CLIP  kind "midi", id, name, start, offset, length, mute          (MIDI clips)
//         NOTE  id, pitch (0..127), time, length, velocity (1..127)
//       TAKE  id, name                    (take lanes, see "Takes and comping" below)
//         CLIP ...
//       (a track in a group also has `group`, the group's RETURN id, and `output` = that id)
//     PATTERN  id, name, colour, length (beats), step (beats per step), swing (0..1)
//       PART  track (a TRACK id), drum (a row per note), rows ("36,38,42": rows shown, in order),
//             stepNote (the note a one-row part's steps play), mute
//         NOTE  id, pitch, time (BEATS from the pattern's start), length (beats), velocity
//     PATTERNCLIP  id, pattern (a PATTERN id), start (beats), length (beats), row (0..3), mute
//     RETURN  id, name, colour, volume, pan, mute, solo, inGain, outGain,
//             group (true: a group track, see below), folded (a group's tracks are hidden)
//       INSERTS ...
//       SEND ... (group tracks only: their own sends into the other returns)
//
// Return tracks (aux buses) hold shared effects: tracks send some of their signal to
// them with SENDs, or play into one entirely with `output` (a group bus). Returns play
// into the master. A group track is a return with `group` set: the tracks in it (their
// `group` is its id) play into it, so it has its own fader, pan, effects and sends; the UI
// shows it as a foldable header above its tracks. Groups don't nest. A new project starts with a Reverb return and a Chorus return
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
// A clip in musical mode is tied to bars and beats: its file plays at `clipTempo` bent
// onto the project's tempo map, so it follows tempo changes and ramps. Its `warp` is then
// the free warp in the file's own time, laid under that (see getEffectiveWarp). When the
// tempo map changes, musical clips keep their bars and beats (start, window and length).
//
// A clip with a warp map or a transpose (its own plus its track's) plays a rendered copy
// of its file (see Warp.h); its offset and length are then measured in that copy. The
// file itself is never changed.
//
// `tempo` is the tempo at the start; `tempoChanges` the tempo points after it (see
// TempoMap::changesToString; missing when the tempo is steady).
//
// Patterns (the step sequencer, see "Patterns" below) are the one part of the model kept
// in BEATS, so they follow the tempo map as they are.
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
    MICFX_DAW_ID (monitorAuto) // monitor mode Auto: hear the input while armed
    MICFX_DAW_ID (viewState) // the page's view of the project (zoom, scroll...), JSON; not an edit
    MICFX_DAW_ID (beatsPerBar) MICFX_DAW_ID (MARKER) MICFX_DAW_ID (time)
    MICFX_DAW_ID (transpose) MICFX_DAW_ID (warp) MICFX_DAW_ID (warpBypass)
    MICFX_DAW_ID (kind) MICFX_DAW_ID (INSTRUMENT) MICFX_DAW_ID (plugin) MICFX_DAW_ID (format) MICFX_DAW_ID (vendor)
    MICFX_DAW_ID (description) MICFX_DAW_ID (state) MICFX_DAW_ID (NOTE) MICFX_DAW_ID (pitch) MICFX_DAW_ID (velocity)
    MICFX_DAW_ID (inGain) MICFX_DAW_ID (outGain)
    MICFX_DAW_ID (key) MICFX_DAW_ID (keyMinor) MICFX_DAW_ID (mixRole) MICFX_DAW_ID (ride) MICFX_DAW_ID (automation)
    MICFX_DAW_ID (RETURN) MICFX_DAW_ID (SEND) MICFX_DAW_ID (target) MICFX_DAW_ID (level) MICFX_DAW_ID (pre)
    MICFX_DAW_ID (output) MICFX_DAW_ID (defaultReturns) MICFX_DAW_ID (group) MICFX_DAW_ID (folded)
    MICFX_DAW_ID (stretch) MICFX_DAW_ID (formant) MICFX_DAW_ID (tempoChanges) MICFX_DAW_ID (musical) MICFX_DAW_ID (clipTempo) MICFX_DAW_ID (link)
    MICFX_DAW_ID (pitchNotes) MICFX_DAW_ID (rebalance) MICFX_DAW_ID (rebalanceFrom)
    MICFX_DAW_ID (inputLeft) MICFX_DAW_ID (inputRight) MICFX_DAW_ID (polarity) MICFX_DAW_ID (nudge)
    MICFX_DAW_ID (TAKE) MICFX_DAW_ID (take) MICFX_DAW_ID (takesShown) MICFX_DAW_ID (listenTake)
    MICFX_DAW_ID (PATTERN) MICFX_DAW_ID (PART) MICFX_DAW_ID (PATTERNCLIP) MICFX_DAW_ID (track) MICFX_DAW_ID (drum)
    MICFX_DAW_ID (rows) MICFX_DAW_ID (stepNote) MICFX_DAW_ID (step) MICFX_DAW_ID (swing) MICFX_DAW_ID (pattern) MICFX_DAW_ID (row)
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

    // Phase between tracks that recorded the same sound (two mics on one source): the
    // track's audio clips play upside down (polarity) and/or shifted by `nudge` ms
    // (+ later, - earlier), before its effects. Auto Mix and "Align to..." set them
    // (analysis::measureAlignment). MIDI tracks and the live input aren't affected.
    static constexpr float maxNudgeMs = 50.0f;
    void setTrackPolarity (int trackId, bool inverted);
    void setTrackNudge (int trackId, float milliseconds);
    static bool getPolarity (const juce::ValueTree& track) { return (bool) track.getProperty (ids::polarity, false); }
    static float getNudge (const juce::ValueTree& track)   { return (float) track.getProperty (ids::nudge, 0.0f); }

    //==============================================================================
    // Return tracks. Their ids share the track id space; the mixer setters above
    // (volume, pan, mute, solo), the chain gains, inserts and copyInserts take a
    // return's id too.
    juce::ValueTree addReturn (const juce::String& name, std::optional<fx::ModuleType> effect = std::nullopt);
    void removeReturn (int returnId); // also removes the sends to it, and routes its tracks back to the master
    juce::ValueTree getReturn (int returnId) const;
    juce::Array<juce::ValueTree> getReturns() const;
    void moveReturn (int returnId, int newIndex);

    // Group tracks (returns with `group`). groupTracks makes a group of the given tracks
    // (moved next to each other, at the first one's place) and returns its id, or -1.
    // Tracks already in a group leave it first. ungroup removes the group, its tracks stay.
    int groupTracks (const juce::Array<int>& trackIds, const juce::String& name = "Group");
    void ungroup (int groupId);
    void setTrackGroup (int trackId, int groupIdOrNone); // -1: out of its group
    void setGroupFolded (int groupId, bool folded);      // view state, not an edit
    static bool isGroup (const juce::ValueTree& bus) { return bus.hasType (ids::RETURN) && (bool) bus.getProperty (ids::group, false); }
    static int getTrackGroup (const juce::ValueTree& track) { return (int) track.getProperty (ids::group, -1); }

    // A track's send to a return, in dB (-inf / below minSendDb is off). Pre-fader sends
    // ignore the track's volume and pan (they still go quiet when it's muted).
    static constexpr float minSendDb = -60.0f, maxSendDb = 6.0f;
    // A group track can send too, but only into plain returns.
    void setSend (int trackId, int returnId, float decibels);
    void setSendPre (int trackId, int returnId, bool preFader);
    static juce::ValueTree getSend (const juce::ValueTree& trackOrGroup, int returnId);

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
    // "in" (always hear the input), "auto" (while armed) or "off". Not undoable, like arm.
    void setTrackMonitorMode (int trackId, const juce::String& mode);
    static juce::String getTrackMonitorMode (const juce::ValueTree& track);

    // Which audio interface inputs feed an audio track (what it records and monitors),
    // counted from 0. Mono: one input (right is noInput). Stereo: a left and a right,
    // any two inputs (a stereo pair, or two mics). Left noInput: no input at all.
    // A track without the properties takes input 1, mono.
    static constexpr int noInput = -1, maxInputs = 32;
    struct Input
    {
        int left = 0, right = noInput;
        bool isStereo() const noexcept { return left != noInput && right != noInput; }
        bool operator== (const Input&) const = default;
    };
    void setTrackInput (int trackId, Input);
    static Input getTrackInput (const juce::ValueTree& track);

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
    int copyClipTo (int clipId, int trackId);          // the same clip, at the same time, on another track; returns its id or -1
    void setClipGain (int clipId, float decibels);

    // Rebalance: the clip plays `mixed`, a re-mix of its stems at other levels (same length as the
    // file it replaces, so windows, warps and fades stay as they are). `original` is the file it came
    // from and `settings` (JSON: quality and the level of each stem in dB) says how it was mixed.
    // Undoable. resetClipRebalance puts the original file back.
    void setClipRebalance (int clipId, const juce::File& mixed, const juce::File& original, const juce::String& settings);
    void resetClipRebalance (int clipId);
    static juce::File getRebalanceOriginal (const juce::ValueTree& clip); // the clip's own file when not rebalanced
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

    // The note pitch editor: the take's sung notes with their edits (in the file's time, see
    // analysis/Pitch.h). One undo step; no edited note removes them. Every clip showing the
    // same take renders with them. Ignored on linked clips.
    void setClipNotes (int clipId, const std::vector<analysis::pitch::Note>&);
    static std::vector<analysis::pitch::Note> getClipNotes (const juce::ValueTree& clip);
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

    // Effect automation: an insert value over time (an echo's mix rising at the end of a
    // phrase...), while playing and in exports. Straight lines between the points, held
    // before the first and after the last; when playback stops the value goes back to
    // the insert's own. Stored on the INSERT as "valueId=time:value;...|valueId=...".
    // An empty list removes that value's lane. One undo step.
    struct AutomationPoint { double time = 0.0; float value = 0.0f; };
    using Automation = std::map<juce::String, std::vector<AutomationPoint>>; // by value ID
    void setInsertAutomation (int insertId, const juce::String& valueId, const std::vector<AutomationPoint>&);
    static Automation getInsertAutomation (const juce::ValueTree& insert);
    static float getAutomationValueAt (const std::vector<AutomationPoint>&, double time);
    void clearTrackAutomation (int trackId); // every insert's automation on the track
    void setClipWarp (int clipId, const warp::Map&);
    static warp::Map getClipWarp (const juce::ValueTree& clip);

    // Musical mode. Switching it or changing the clip's tempo keeps the clip's start and
    // the stretch of the file it shows (the window follows the new timing). clipTempo <= 0
    // keeps the tempo it has (or the project tempo at the clip, the first time).
    static constexpr double minClipTempo = 30.0, maxClipTempo = 300.0;
    void setClipMusical (int clipId, bool on, double clipTempo = 0.0);
    static bool isMusical (const juce::ValueTree& clip) { return (bool) clip.getProperty (ids::musical, false); }
    static double getClipTempo (const juce::ValueTree& clip);

    // The tempo map of the project a clip belongs to.
    static TempoMap getTempoMapFor (const juce::ValueTree& clip);

    // A musical clip's bend: its file's own time (straight, at clipTempo) to its rendered
    // copy's time, through the tempo map. The identity for other clips.
    static warp::Map getMusicalMap (const juce::ValueTree& clip);

    // The whole map from file time to rendered time: the free warp, then the musical bend.
    // Quantize and the warp tool work on this; setting it on a musical clip keeps the
    // bend and stores the rest as the free warp.
    static warp::Map getEffectiveWarp (const juce::ValueTree& clip);
    void setEffectiveWarp (int clipId, const warp::Map&);

    // The free warp's markers with their targets in rendered time, for the warp tool.
    static std::vector<warp::Marker> getShownWarp (const juce::ValueTree& clip);

    //==============================================================================
    // Linked tracks warp as one, for recordings made together (the mics of a drum kit, a
    // stereo pair on two tracks). Tracks with the same `link` number are linked. A clip's
    // partners are the clips on its linked tracks that start their file at the same moment
    // (within 1 ms) and overlap it. Warping, quantizing, musical mode, the algorithm and
    // moving a clip apply to its partners too, and they render together so every mic gets
    // the same timing and phase decisions (see warp::Settings::linked).
    void linkTracks (const juce::Array<int>& trackIds); // joins any group one of them is in
    void unlinkTrack (int trackId);
    static int getLink (const juce::ValueTree& track) { return (int) track.getProperty (ids::link, 0); }
    static juce::Array<juce::ValueTree> getLinkedClips (const juce::ValueTree& clip);

    // What the clip's rendered copy needs: its warp map and its transpose plus its track's.
    static warp::Settings getWarpSettings (const juce::ValueTree& clip);

    // Replaces a clip with copies that play only the given parts of it (seconds from the
    // clip's start), each with short fades. For strip silence. Returns the new clip ids.
    juce::Array<int> keepClipParts (int clipId, const std::vector<std::pair<double, double>>& parts, double fadeSeconds = 0.01);

    //==============================================================================
    // Takes and comping. An audio track's TAKEs are lanes under it holding the
    // recordings made over the same stretch of the song (each pass of a loop recording,
    // or recording again over a part). They don't play: the track plays its own clips,
    // the comp, made of pieces copied from the takes (each piece's `take` says which).
    // Comping a range from a take replaces whatever the track plays there with that
    // take's audio. Take clips are never edited in place; the comp's pieces are ordinary
    // clips (move, trim, fade them as any other).
    //
    // `listenTake` makes the track play one take instead of its comp, to audition it.
    // It and `takesShown` are view settings: they don't go into the undo history.
    juce::Array<juce::ValueTree> getTakes (int trackId) const;
    juce::ValueTree getTake (int takeId) const;
    static juce::ValueTree getOwnerTrack (const juce::ValueTree& clipOrTake); // the TRACK a clip or take is on

    // A new empty take lane at the bottom (named "Take N" when no name is given).
    juce::ValueTree addTake (int trackId, const juce::String& name = {});
    void removeTake (int takeId);    // the comp keeps the pieces it took from it
    void setTakeName (int takeId, const juce::String&);

    // What one recording (or one loop pass) put on a track: seconds on the timeline, and
    // where in the file it starts.
    struct RecordedPass { double start = 0.0, offset = 0.0, length = 0.0; };

    // Adds a recording to an audio track. On an empty stretch of a track without takes
    // it's a plain clip, as before. Over audio already there (or with several loop
    // passes, or on a track that has takes) every pass becomes a new take, what was
    // there becomes the first take (once), and the comp plays the newest passes.
    void addRecording (int trackId, const juce::File&, const std::vector<RecordedPass>&);

    // Comps a range (seconds on the timeline) from a take: the track's clips there are
    // cut away and the take's audio in the range copied in. Its pieces get short fades
    // where they meet the rest of the comp.
    static constexpr double compFadeSeconds = 0.008;
    void compTake (int takeId, double start, double end);

    // Comps from the take the stretch of the comp at `time` (the comp piece there; in a
    // gap, the take's clip there between the comp's neighbouring pieces). Returns the
    // range comped, or an empty range when there's nothing to take.
    std::pair<double, double> compTakeAt (int takeId, double time);

    // A copy of a comp clip (or any clip on the track) as a new take lane, e.g. to keep
    // a part before recording over it. Returns the new take.
    juce::ValueTree addTakeFromClip (int clipId);

    void setTakesShown (int trackId, bool);
    void setListenTake (int trackId, int takeIdOrNone); // -1: the comp
    static int getListenTake (const juce::ValueTree& track);

    // The nearest grid line on the tempo map, `beats` apart (1 = quarter notes,
    // 4 = bars in 4/4, 0.5 = eighths).
    double snapToGrid (double seconds, double beats) const;

    // Loop range: while on, playback wraps from the end back to the start.
    void setLoop (bool on, double startSeconds, double endSeconds);

    //==============================================================================
    // Patterns: the step sequencer (see Patterns.h for how they play). A pattern is a loop
    // of notes for any number of MIDI tracks, one PART per track, all in beats from the
    // pattern's start. Pattern blocks (PATTERNCLIP) place a pattern on the timeline: the
    // block plays every track the pattern has a part for, repeating the pattern to fill its
    // length. Every block of a pattern plays the same notes; makePatternUnique gives one
    // block a copy of its own. Each call is one undoable edit.
    static constexpr double minPatternBeats = 0.25, maxPatternBeats = 256.0;
    static constexpr double minStepBeats = 1.0 / 32.0, maxStepBeats = 1.0;
    static constexpr int maxPatternRows = 4; // block lanes for overlapping blocks
    static constexpr int defaultStepNote = 60;

    juce::ValueTree addPattern (const juce::String& name = {}, double lengthBeats = 0.0); // 0: a bar
    int duplicatePattern (int patternId);  // a copy with its parts and notes; returns its id, or -1
    void removePattern (int patternId);    // and its blocks
    juce::ValueTree getPattern (int patternId) const;
    juce::Array<juce::ValueTree> getPatterns() const;
    void setPatternName (int patternId, const juce::String&);
    void setPatternColour (int patternId, const juce::String&);
    void setPatternLength (int patternId, double beats);
    void setPatternStep (int patternId, double beats);
    void setPatternSwing (int patternId, double swing);

    static double getPatternLength (const juce::ValueTree& pattern);
    static double getPatternStep (const juce::ValueTree& pattern);

    // A pattern's part for a track (invalid when it has none); ensurePart makes one (a MIDI
    // track only).
    juce::ValueTree getPart (int patternId, int trackId) const;
    juce::ValueTree ensurePart (int patternId, int trackId);
    void setPartDrum (int patternId, int trackId, bool drum);
    void setPartRows (int patternId, int trackId, const juce::Array<int>& notes);
    void setPartStepNote (int patternId, int trackId, int note);
    void setPartMute (int patternId, int trackId, bool mute);
    static juce::Array<int> getPartRows (const juce::ValueTree& part);

    // Notes in a part (times in beats from the pattern's start). setNote and removeNote
    // above take pattern notes too.
    juce::ValueTree addPatternNote (int patternId, int trackId, int pitch, double time, double length, int velocity = 100);

    // Step painting: for each (step index, velocity), the note of `pitch` on that step is
    // removed, then (velocity > 0) a one-step note added. Times are index x the step.
    void setSteps (int patternId, int trackId, int pitch, const std::vector<std::pair<int, int>>& steps);

    // Pattern blocks. Length <= 0: one pass of the pattern.
    juce::ValueTree addPatternClip (int patternId, double startBeats, double lengthBeats = 0.0, int row = 0);
    void setPatternClip (int clipId, double startBeats, double lengthBeats, int row);
    void setPatternClipMute (int clipId, bool);
    void removePatternClip (int clipId);
    juce::ValueTree getPatternClip (int clipId) const;
    juce::Array<juce::ValueTree> getPatternClips() const;

    // Gives a block its own copy of its pattern. Returns the new pattern's id, or -1.
    int makePatternUnique (int clipId);

    // Writes a block out as ordinary MIDI clips, one on each track its pattern has notes
    // for (swing included), and removes the block. Returns the new clips' ids.
    juce::Array<int> patternClipToMidiClips (int clipId);

    // The other way: a new pattern from a MIDI clip's notes (as long as the clip, rounded
    // up to whole beats), and a block of it in the clip's place. Returns the pattern's id,
    // or -1.
    int midiClipToPattern (int clipId);

    // Edits made while one of these is alive don't go into the undo history (pattern
    // recording adds notes as they're played, then makes them one undo step at the end).
    class NotUndoable
    {
    public:
        explicit NotUndoable (Edit& e) : edit (e), saved (std::exchange (e.undoTarget, nullptr)) {}
        ~NotUndoable() { edit.undoTarget = saved; }
    private:
        Edit& edit;
        juce::UndoManager* saved;
        JUCE_DECLARE_NON_COPYABLE (NotUndoable)
    };

    //==============================================================================
    // Inserts on a track, or on the master bus with trackId == masterId. A new insert
    // starts from the module's defaults and switched on.
    static constexpr int masterId = 0;

    juce::ValueTree addInsert (int trackId, fx::ModuleType, int index = -1);
    // A third-party effect plugin as an insert (type "plugin", see PluginInsert.h), on, at
    // unity gain, starting from the info's state (none: the plugin's own defaults).
    juce::ValueTree addPluginInsert (int trackId, const InstrumentInfo& plugin, int index = -1);
    static bool isPluginInsert (const juce::ValueTree& insert) { return insert[ids::type].toString() == "plugin"; }
    void removeInsert (int insertId);
    void moveInsert (int insertId, int newIndex);
    void setInsertValue (int insertId, const juce::String& valueId, float value);
    juce::ValueTree getInsert (int insertId) const;

    static juce::ValueTree getInserts (const juce::ValueTree& trackOrMaster);

    // Every module type by its project-file key ("eq", "echo", ...).
    static std::optional<fx::ModuleType> moduleTypeFromKey (const juce::String&);

private:
    int takeNextId();

    // Runs `apply` for each of a clip's linked partners (not again from inside it).
    void forPartners (int clipId, const std::function<void (int)>& apply);
    bool propagating = false;

    // Moves and resizes a clip so its window shows the same stretch of the file after its
    // effective map changes from `before`, keeping where the file's start sits.
    void keepWindow (juce::ValueTree& clip, const warp::Map& before);

    // A musical clip placed by beats: its file's start at anchorBeat, its window from
    // startBeat to endBeat.
    void placeByBeats (juce::ValueTree& clip, const TempoMap&, double anchorBeat, double startBeat, double endBeat);
    // Cuts the comp (a track's own audio clips) away between two times, leaving half a
    // comp fade either side for the piece that goes in to crossfade with.
    void cutComp (juce::ValueTree track, double start, double end);
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
