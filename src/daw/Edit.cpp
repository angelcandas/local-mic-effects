#include "Edit.h"
#include "RackInsert.h"
#include "PluginInsert.h"

#include "dsp/FxModules.h"

namespace daw
{
namespace
{
    juce::ValueTree findRecursive (const juce::ValueTree& parent, const juce::Identifier& type, int id)
    {
        for (const auto& child : parent)
        {
            if (child.hasType (type) && (int) child[ids::id] == id)
                return child;

            if (auto found = findRecursive (child, type, id); found.isValid())
                return found;
        }

        return {};
    }

    int maxId (const juce::ValueTree& tree)
    {
        auto result = (int) tree.getProperty (ids::id, 0);
        for (const auto& child : tree)
            result = juce::jmax (result, maxId (child));
        return result;
    }

    juce::ValueTree makeMaster()
    {
        juce::ValueTree master (ids::MASTER);
        master.setProperty (ids::id, Edit::masterId, nullptr);
        master.setProperty (ids::volume, 0.0f, nullptr);
        master.appendChild (juce::ValueTree (ids::INSERTS), nullptr);
        return master;
    }
}

Edit::Edit() : state (ids::PROJECT)
{
    state.setProperty (ids::version, formatVersion, nullptr);
    state.setProperty (ids::tempo, 120.0, nullptr);
    state.appendChild (makeMaster(), nullptr);
    addDefaultReturns();
}

void Edit::addDefaultReturns()
{
    // Not undoable: part of a new (or upgraded) project, not an edit.
    if ((bool) state.getProperty (ids::defaultReturns, false))
        return;

    state.setProperty (ids::defaultReturns, true, nullptr);

    if (! getReturns().isEmpty())
        return;

    juce::UndoManager* none = nullptr;
    std::swap (none, undoTarget);
    addReturn ("Reverb", fx::ModuleType::reverb);
    addReturn ("Chorus", fx::ModuleType::chorus);
    std::swap (none, undoTarget);
}

void Edit::replaceState (const juce::ValueTree& newState)
{
    jassert (newState.hasType (ids::PROJECT));

    // Copy property by property and child by child, so listeners on `state` (the
    // engine) stay attached and see the change.
    state.copyPropertiesFrom (newState, nullptr);
    state.removeAllChildren (nullptr);

    for (const auto& child : newState)
        state.appendChild (child.createCopy(), nullptr);

    if (! getMaster().isValid())
        state.addChild (makeMaster(), 0, nullptr);

    nextId = maxId (state) + 1;

    // Projects from before return tracks get the default ones once.
    addDefaultReturns();

    // Older or hand-edited projects can miss parts the editing code relies on: give every
    // track and the master an effects list, and anything without an id a fresh one.
    for (auto child : state)
    {
        if (! child.hasType (ids::TRACK) && ! child.hasType (ids::MASTER) && ! child.hasType (ids::RETURN))
            continue;

        if (! child.getChildWithName (ids::INSERTS).isValid())
            child.appendChild (juce::ValueTree (ids::INSERTS), nullptr);

        if (child.hasType (ids::TRACK))
            for (auto item : child)
                if (item.hasType (ids::CLIP) && ! item.hasProperty (ids::id))
                    item.setProperty (ids::id, takeNextId(), nullptr);

        if ((child.hasType (ids::TRACK) || child.hasType (ids::RETURN)) && ! child.hasProperty (ids::id))
            child.setProperty (ids::id, takeNextId(), nullptr);
    }

    undoManager.clearUndoHistory();
}

int Edit::takeNextId()
{
    return nextId++;
}

void Edit::renumber (juce::ValueTree& tree)
{
    if (tree.hasProperty (ids::id))
        tree.setProperty (ids::id, takeNextId(), nullptr);

    for (auto child : tree)
        renumber (child);
}

juce::ValueTree Edit::findById (const juce::Identifier& type, int id) const
{
    return findRecursive (state, type, id);
}

//==============================================================================
juce::ValueTree Edit::addTrack (const juce::String& name, int index)
{
    juce::ValueTree track (ids::TRACK);
    track.setProperty (ids::id, takeNextId(), nullptr);
    track.setProperty (ids::name, name, nullptr);
    track.setProperty (ids::volume, 0.0f, nullptr);
    track.setProperty (ids::pan, 0.0f, nullptr);
    track.setProperty (ids::mute, false, nullptr);
    track.setProperty (ids::solo, false, nullptr);
    track.setProperty (ids::arm, false, nullptr);
    track.setProperty (ids::monitor, false, nullptr);
    track.appendChild (juce::ValueTree (ids::INSERTS), nullptr);

    // Index counts tracks only (markers and returns sit among them in the tree). A new
    // last track goes right after the last one, so returns stay after the tracks.
    const auto tracks = getTracks();
    const auto position = juce::isPositiveAndBelow (index, tracks.size()) ? state.indexOf (tracks[index])
                        : state.indexOf (tracks.isEmpty() ? getMaster() : tracks.getLast()) + 1;
    state.addChild (track, position, undoTarget);
    return track;
}

void Edit::removeTrack (int trackId)
{
    auto track = getTrack (trackId);
    if (! track.isValid())
        return;

    // Its parts in the patterns go with it (same undo step).
    for (auto pattern : getPatterns())
        if (auto part = getPart ((int) pattern[ids::id], trackId); part.isValid())
            pattern.removeChild (part, undoTarget);

    state.removeChild (track, undoTarget);
}

void Edit::moveTrack (int trackId, int newIndex)
{
    auto track = getTrack (trackId);
    if (! track.isValid())
        return;

    // Take the place of the track now at newIndex (moving down, it lands right after it).
    const auto tracks = getTracks();
    const auto target = tracks[juce::jlimit (0, tracks.size() - 1, newIndex)];
    state.moveChild (state.indexOf (track), state.indexOf (target), undoTarget);
}

juce::ValueTree Edit::getTrack (int trackId) const
{
    auto track = state.getChildWithProperty (ids::id, trackId);
    return track.hasType (ids::TRACK) ? track : juce::ValueTree();
}

int Edit::duplicateTrack (int trackId)
{
    const auto track = getTrack (trackId);
    if (! track.isValid())
        return -1;

    auto copy = track.createCopy();
    renumber (copy);

    // The comp's pieces point at the copy's own takes.
    std::map<int, int> takeIds;
    for (int i = 0; i < track.getNumChildren(); ++i)
        if (track.getChild (i).hasType (ids::TAKE))
            takeIds[(int) track.getChild (i)[ids::id]] = (int) copy.getChild (i)[ids::id];
    for (auto clip : copy)
        if (clip.hasType (ids::CLIP) && clip.hasProperty (ids::take))
        {
            if (const auto it = takeIds.find ((int) clip[ids::take]); it != takeIds.end())
                clip.setProperty (ids::take, it->second, nullptr);
            else
                clip.removeProperty (ids::take, nullptr);
        }
    copy.removeProperty (ids::listenTake, nullptr);

    copy.setProperty (ids::name, (track[ids::name].toString() + " copy").substring (0, 60), nullptr);
    copy.setProperty (ids::arm, false, nullptr);
    copy.setProperty (ids::monitor, false, nullptr);

    state.addChild (copy, state.indexOf (track) + 1, undoTarget);
    return (int) copy[ids::id];
}

void Edit::copyInserts (int fromTrackId, int toTrackId)
{
    const auto from = getInserts (getTrackOrMaster (fromTrackId));
    auto to = getInserts (getTrackOrMaster (toTrackId));
    if (! from.isValid() || ! to.isValid() || from == to)
        return;

    to.removeAllChildren (undoTarget);
    for (const auto& insert : from)
    {
        auto copy = insert.createCopy();
        copy.setProperty (ids::id, takeNextId(), nullptr);
        to.appendChild (copy, undoTarget);
    }
}

void Edit::soloExclusive (int trackId)
{
    auto channels = getTracks();
    channels.addArray (getReturns());

    for (auto track : channels)
        track.setProperty (ids::solo, (int) track[ids::id] == trackId, undoTarget);
}

void Edit::clearMuteAndSolo()
{
    auto channels = getTracks();
    channels.addArray (getReturns());

    for (auto track : channels)
    {
        track.setProperty (ids::solo, false, undoTarget);
        track.setProperty (ids::mute, false, undoTarget);
    }
}

juce::Array<juce::ValueTree> Edit::getTracks() const
{
    juce::Array<juce::ValueTree> tracks;
    for (const auto& child : state)
        if (child.hasType (ids::TRACK))
            tracks.add (child);
    return tracks;
}

juce::ValueTree Edit::getChannel (int id) const
{
    if (id == masterId)
        return getMaster();

    auto channel = state.getChildWithProperty (ids::id, id);
    return channel.hasType (ids::TRACK) || channel.hasType (ids::RETURN) ? channel : juce::ValueTree();
}

// Tracks and returns (not the master: it has no mute or solo).
void Edit::setTrackVolume (int trackId, float decibels) { getChannel (trackId).setProperty (ids::volume, decibels, undoTarget); }
void Edit::setTrackPan (int trackId, float pan)         { getChannel (trackId).setProperty (ids::pan, juce::jlimit (-1.0f, 1.0f, pan), undoTarget); }

void Edit::setTrackPolarity (int trackId, bool inverted)
{
    if (auto track = getTrack (trackId); track.isValid())
    {
        if (inverted)
            track.setProperty (ids::polarity, true, undoTarget);
        else
            track.removeProperty (ids::polarity, undoTarget);
    }
}

void Edit::setTrackNudge (int trackId, float milliseconds)
{
    if (auto track = getTrack (trackId); track.isValid())
    {
        milliseconds = juce::jlimit (-maxNudgeMs, maxNudgeMs, milliseconds);
        if (std::abs (milliseconds) < 1.0e-4f)
            track.removeProperty (ids::nudge, undoTarget);
        else
            track.setProperty (ids::nudge, milliseconds, undoTarget);
    }
}
void Edit::setTrackMute (int trackId, bool mute)        { if (trackId != masterId) getChannel (trackId).setProperty (ids::mute, mute, undoTarget); }
void Edit::setTrackSolo (int trackId, bool solo)        { if (trackId != masterId) getChannel (trackId).setProperty (ids::solo, solo, undoTarget); }
void Edit::setMasterVolume (float decibels)             { getMaster().setProperty (ids::volume, decibels, undoTarget); }
void Edit::setMasterPan (float pan)                     { getMaster().setProperty (ids::pan, juce::jlimit (-1.0f, 1.0f, pan), undoTarget); }

//==============================================================================
juce::ValueTree Edit::addReturn (const juce::String& name, std::optional<fx::ModuleType> effect)
{
    juce::ValueTree bus (ids::RETURN);
    bus.setProperty (ids::id, takeNextId(), nullptr);
    bus.setProperty (ids::name, name, nullptr);
    bus.setProperty (ids::volume, 0.0f, nullptr);
    bus.setProperty (ids::pan, 0.0f, nullptr);
    bus.setProperty (ids::mute, false, nullptr);
    bus.setProperty (ids::solo, false, nullptr);
    bus.appendChild (juce::ValueTree (ids::INSERTS), nullptr);

    // After the last return (or the last track, for the first one).
    const auto returns = getReturns();
    const auto tracks = getTracks();
    const auto after = ! returns.isEmpty() ? returns.getLast() : ! tracks.isEmpty() ? tracks.getLast() : getMaster();
    state.addChild (bus, state.indexOf (after) + 1, undoTarget);

    if (effect.has_value())
    {
        auto insert = addInsert ((int) bus[ids::id], *effect);

        // A return carries only the effect: fully wet, the dry signal stays on the track.
        for (const auto* wet : { "reverbMix", "irvMix", "chorusMix", "flangerMix", "phaserMix", "delayMix", "doublerMix" })
            if (insert.hasProperty (wet))
                insert.setProperty (wet, 100.0f, nullptr);
    }

    return bus;
}

void Edit::removeReturn (int returnId)
{
    auto bus = getReturn (returnId);
    if (! bus.isValid())
        return;

    for (auto track : getTracks())
    {
        if (auto send = getSend (track, returnId); send.isValid())
            track.removeChild (send, undoTarget);

        if (getTrackOutput (track) == returnId)
            track.removeProperty (ids::output, undoTarget);

        if (getTrackGroup (track) == returnId)
            track.removeProperty (ids::group, undoTarget);
    }

    for (auto other : getReturns())
        if (auto send = getSend (other, returnId); send.isValid())
            other.removeChild (send, undoTarget);

    state.removeChild (bus, undoTarget);
}

juce::ValueTree Edit::getReturn (int returnId) const
{
    auto bus = state.getChildWithProperty (ids::id, returnId);
    return bus.hasType (ids::RETURN) ? bus : juce::ValueTree();
}

juce::Array<juce::ValueTree> Edit::getReturns() const
{
    juce::Array<juce::ValueTree> returns;
    for (const auto& child : state)
        if (child.hasType (ids::RETURN))
            returns.add (child);
    return returns;
}

void Edit::moveReturn (int returnId, int newIndex)
{
    const auto returns = getReturns();
    const auto from = returns.indexOf (getReturn (returnId));
    if (from < 0)
        return;

    const auto to = returns[juce::jlimit (0, returns.size() - 1, newIndex)];
    state.moveChild (state.indexOf (returns[from]), state.indexOf (to), undoTarget);
}

juce::ValueTree Edit::getSend (const juce::ValueTree& track, int returnId)
{
    for (const auto& child : track)
        if (child.hasType (ids::SEND) && (int) child[ids::target] == returnId)
            return child;
    return {};
}

// The source of a send: a track, or a group track (which can only send to plain returns).
static juce::ValueTree getSendSource (const Edit& edit, int sourceId, int returnId)
{
    auto source = edit.getChannel (sourceId);
    if (! source.isValid() || (! source.hasType (ids::TRACK) && ! Edit::isGroup (source)))
        return {};
    if (Edit::isGroup (source) && (Edit::isGroup (edit.getReturn (returnId)) || sourceId == returnId))
        return {};
    return source;
}

void Edit::setSend (int trackId, int returnId, float decibels)
{
    auto track = getSendSource (*this, trackId, returnId);
    if (! track.isValid() || ! getReturn (returnId).isValid())
        return;

    const auto level = std::isfinite (decibels) ? juce::jlimit (minSendDb - 1.0f, maxSendDb, decibels) : minSendDb - 1.0f;
    auto send = getSend (track, returnId);

    if (! send.isValid())
    {
        if (level < minSendDb)
            return; // nothing to turn off

        send = juce::ValueTree (ids::SEND);
        send.setProperty (ids::id, takeNextId(), nullptr);
        send.setProperty (ids::target, returnId, nullptr);
        send.setProperty (ids::level, level, nullptr);
        send.setProperty (ids::pre, false, nullptr);
        track.appendChild (send, undoTarget);
        return;
    }

    send.setProperty (ids::level, level, undoTarget);
}

void Edit::setSendPre (int trackId, int returnId, bool preFader)
{
    auto track = getSendSource (*this, trackId, returnId);
    if (! track.isValid() || ! getReturn (returnId).isValid())
        return;

    auto send = getSend (track, returnId);
    if (! send.isValid())
    {
        send = juce::ValueTree (ids::SEND);
        send.setProperty (ids::id, takeNextId(), nullptr);
        send.setProperty (ids::target, returnId, nullptr);
        send.setProperty (ids::level, minSendDb - 1.0f, nullptr);
        send.setProperty (ids::pre, preFader, nullptr);
        track.appendChild (send, undoTarget);
        return;
    }

    send.setProperty (ids::pre, preFader, undoTarget);
}

//==============================================================================
int Edit::groupTracks (const juce::Array<int>& trackIds, const juce::String& name)
{
    juce::Array<juce::ValueTree> members;
    for (const auto& track : getTracks()) // in track order
        if (trackIds.contains ((int) track[ids::id]))
            members.add (track);

    if (members.isEmpty())
        return -1;

    auto group = addReturn (name);
    group.setProperty (ids::group, true, nullptr);
    const auto groupId = (int) group[ids::id];

    // Next to each other, where the first one is.
    const auto first = state.indexOf (members.getFirst());
    for (int i = 1; i < members.size(); ++i)
        state.moveChild (state.indexOf (members[i]), first + i, undoTarget);

    for (auto track : members)
        setTrackGroup ((int) track[ids::id], groupId);

    return groupId;
}

void Edit::ungroup (int groupId)
{
    if (isGroup (getReturn (groupId)))
        removeReturn (groupId);
}

void Edit::setTrackGroup (int trackId, int groupId)
{
    auto track = getTrack (trackId);
    if (! track.isValid())
        return;

    if (groupId < 0 || ! isGroup (getReturn (groupId)))
    {
        if (const auto old = getTrackGroup (track); old >= 0)
        {
            track.removeProperty (ids::group, undoTarget);
            if (getTrackOutput (track) == old)
                track.removeProperty (ids::output, undoTarget);
        }
        return;
    }

    track.setProperty (ids::group, groupId, undoTarget);
    track.setProperty (ids::output, groupId, undoTarget);

    // Right after the group's other tracks.
    int last = -1;
    for (const auto& other : getTracks())
        if (other != track && getTrackGroup (other) == groupId)
            last = state.indexOf (other);
    if (last >= 0 && state.indexOf (track) != last + 1 && state.indexOf (track) != last)
        state.moveChild (state.indexOf (track), state.indexOf (track) < last ? last : last + 1, undoTarget);
}

void Edit::setGroupFolded (int groupId, bool folded)
{
    if (auto group = getReturn (groupId); isGroup (group))
        group.setProperty (ids::folded, folded, nullptr);
}

void Edit::setTrackOutput (int trackId, int returnIdOrMaster)
{
    auto track = getTrack (trackId);
    if (! track.isValid())
        return;

    if (getTrackGroup (track) >= 0 && getTrackGroup (track) != returnIdOrMaster)
        track.removeProperty (ids::group, undoTarget); // routed elsewhere by hand: out of its group

    if (returnIdOrMaster == masterId || ! getReturn (returnIdOrMaster).isValid())
        track.removeProperty (ids::output, undoTarget);
    else
        track.setProperty (ids::output, returnIdOrMaster, undoTarget);
}

void Edit::setChainGain (int trackId, bool output, float decibels)
{
    auto tree = getChannel (trackId);
    if (tree.isValid())
        tree.setProperty (output ? ids::outGain : ids::inGain, juce::jlimit (-maxChainGain, maxChainGain, decibels), undoTarget);
}

// Arm and monitor are recording state, not edits: not undoable.
void Edit::setTrackArm (int trackId, bool arm)          { getTrack (trackId).setProperty (ids::arm, arm, nullptr); }
void Edit::setTrackMonitor (int trackId, bool monitor)
{
    auto track = getTrack (trackId);
    track.setProperty (ids::monitorAuto, false, nullptr);
    track.setProperty (ids::monitor, monitor, nullptr);
}

void Edit::setTrackMonitorMode (int trackId, const juce::String& mode)
{
    auto track = getTrack (trackId);
    track.setProperty (ids::monitorAuto, mode == "auto", nullptr);
    track.setProperty (ids::monitor, mode == "in", nullptr);
}

juce::String Edit::getTrackMonitorMode (const juce::ValueTree& track)
{
    return (bool) track[ids::monitor] ? "in" : (bool) track.getProperty (ids::monitorAuto, false) ? "auto" : "off";
}

void Edit::setTrackInput (int trackId, Input input)
{
    auto track = getTrack (trackId);
    if (! track.isValid())
        return;

    const auto clampInput = [] (int i) { return i < 0 ? noInput : juce::jmin (i, maxInputs - 1); };
    input.left = clampInput (input.left);
    input.right = input.left == noInput || input.right == input.left ? noInput : clampInput (input.right);

    track.setProperty (ids::inputLeft, input.left, undoTarget);
    track.setProperty (ids::inputRight, input.right, undoTarget);
}

Edit::Input Edit::getTrackInput (const juce::ValueTree& track)
{
    Input input;
    input.left = (int) track.getProperty (ids::inputLeft, 0);
    input.right = input.left == noInput ? noInput : (int) track.getProperty (ids::inputRight, noInput);
    return input;
}
void Edit::setTempo (double bpm)
{
    auto points = getTempoMap().getPoints();
    points.front().bpm = bpm;
    setTempoMap (TempoMap (std::move (points)));
}

TempoMap Edit::getTempoMap() const
{
    return TempoMap::fromProject (getTempo(), state[ids::tempoChanges].toString());
}

void Edit::setTempoMap (const TempoMap& map)
{
    const auto before = getTempoMap();

    // Where each musical clip sits in bars and beats, before the tempo moves under it.
    struct Placement { juce::ValueTree clip; double anchor, start, end; };
    std::vector<Placement> musicalClips;
    for (const auto& track : getTracks())
        for (const auto& clip : track)
            if (clip.hasType (ids::CLIP) && isMusical (clip))
            {
                const auto start = (double) clip[ids::start], offset = (double) clip[ids::offset], length = (double) clip[ids::length];
                musicalClips.push_back ({ clip, before.toBeats (start - offset), before.toBeats (start), before.toBeats (start + length) });
            }

    state.setProperty (ids::tempo, juce::jlimit (TempoMap::minBpm, TempoMap::maxBpm, map.getPoints().front().bpm), undoTarget);

    if (const auto changes = map.changesToString(); changes.isEmpty())
        state.removeProperty (ids::tempoChanges, undoTarget);
    else
        state.setProperty (ids::tempoChanges, changes, undoTarget);

    const auto after = getTempoMap();
    for (auto& placed : musicalClips)
        placeByBeats (placed.clip, after, placed.anchor, placed.start, placed.end);
}

int Edit::setTempoMapFromBeats (const std::vector<double>& beats, int firstDownbeat)
{
    const auto bar = getBeatsPerBar();
    std::vector<double> downbeats;
    for (auto i = (size_t) juce::jmax (0, firstDownbeat); i < beats.size(); i += (size_t) bar)
        downbeats.push_back (beats[i]);

    if (downbeats.size() < 2)
        return -1;

    int leadIn = 0;
    setTempoMap (TempoMap::fromDownbeats (std::move (downbeats), bar, &leadIn));
    return leadIn;
}

void Edit::placeByBeats (juce::ValueTree& clip, const TempoMap& tempo, double anchorBeat, double startBeat, double endBeat)
{
    const auto anchor = tempo.toSeconds (anchorBeat);
    const auto start = juce::jmax (0.0, tempo.toSeconds (startBeat));
    const auto end = juce::jmax (start + minClipSeconds, tempo.toSeconds (endBeat));
    clip.setProperty (ids::start, start, undoTarget);
    clip.setProperty (ids::offset, start - anchor, undoTarget);
    clip.setProperty (ids::length, end - start, undoTarget);
}

void Edit::setTempoAt (double seconds, double bpm)
{
    const auto map = getTempoMap();
    auto points = map.getPoints();
    points[(size_t) map.getPointIndexAt (map.toBeats (seconds))].bpm = bpm;
    setTempoMap (TempoMap (std::move (points)));
}

//==============================================================================
namespace
{
    juce::ValueTree makeInstrument (const Edit::InstrumentInfo& info, int id)
    {
        juce::ValueTree instrument (ids::INSTRUMENT);
        instrument.setProperty (ids::id, id, nullptr);
        instrument.setProperty (ids::plugin, info.id, nullptr);
        instrument.setProperty (ids::name, info.name, nullptr);
        instrument.setProperty (ids::format, info.format, nullptr);
        instrument.setProperty (ids::vendor, info.vendor, nullptr);
        instrument.setProperty (ids::description, info.description, nullptr);
        instrument.setProperty (ids::state, info.state, nullptr);
        return instrument;
    }
}

juce::ValueTree Edit::addMidiTrack (const juce::String& name, const InstrumentInfo& info, int index)
{
    auto track = addTrack (name, index);
    track.setProperty (ids::kind, midiKind, undoTarget);
    track.appendChild (makeInstrument (info, takeNextId()), undoTarget);
    return track;
}

void Edit::setInstrument (int trackId, const InstrumentInfo& info)
{
    auto track = getTrack (trackId);
    if (! track.isValid())
        return;

    if (! isMidiTrack (track))
        track.setProperty (ids::kind, midiKind, undoTarget);

    // A new INSTRUMENT (with a new id), so the engine loads a fresh one.
    const auto index = track.indexOf (track.getChildWithName (ids::INSTRUMENT));
    if (index >= 0)
        track.removeChild (index, undoTarget);

    track.addChild (makeInstrument (info, takeNextId()), index, undoTarget);
}

juce::ValueTree Edit::getInstrument (int trackId) const
{
    return getTrack (trackId).getChildWithName (ids::INSTRUMENT);
}

juce::ValueTree Edit::addMidiClip (int trackId, double startSeconds, double lengthSeconds, const juce::String& name)
{
    auto track = getTrack (trackId);
    if (! track.isValid() || ! isMidiTrack (track))
        return {};

    juce::ValueTree clip (ids::CLIP);
    clip.setProperty (ids::id, takeNextId(), nullptr);
    clip.setProperty (ids::kind, midiKind, nullptr);
    clip.setProperty (ids::name, name.isNotEmpty() ? name : track[ids::name].toString(), nullptr);
    clip.setProperty (ids::start, juce::jmax (0.0, startSeconds), nullptr);
    clip.setProperty (ids::offset, 0.0, nullptr);
    clip.setProperty (ids::length, juce::jmax (minClipSeconds, lengthSeconds), nullptr);
    clip.setProperty (ids::gain, 0.0f, nullptr);
    clip.setProperty (ids::fadeIn, 0.0, nullptr);
    clip.setProperty (ids::fadeOut, 0.0, nullptr);
    track.appendChild (clip, undoTarget);
    return clip;
}

namespace
{
    void setNoteProperties (juce::ValueTree& note, int pitch, double time, double length, int velocity, juce::UndoManager* undo)
    {
        note.setProperty (ids::pitch, juce::jlimit (0, 127, pitch), undo);
        note.setProperty (ids::time, juce::jmax (0.0, time), undo);
        note.setProperty (ids::length, juce::jmax (0.001, length), undo);
        note.setProperty (ids::velocity, juce::jlimit (1, 127, velocity), undo);
    }
}

juce::ValueTree Edit::addNote (int clipId, int pitch, double time, double length, int velocity)
{
    auto clip = getClip (clipId);
    if (! clip.isValid() || ! isMidiClip (clip))
        return {};

    juce::ValueTree note (ids::NOTE);
    note.setProperty (ids::id, takeNextId(), nullptr);
    setNoteProperties (note, pitch, time, length, velocity, nullptr);
    clip.appendChild (note, undoTarget);
    return note;
}

void Edit::setNote (int noteId, int pitch, double time, double length, int velocity)
{
    if (auto note = getNote (noteId); note.isValid())
        setNoteProperties (note, pitch, time, length, velocity, undoTarget);
}

void Edit::removeNote (int noteId)
{
    if (auto note = getNote (noteId); note.isValid())
        note.getParent().removeChild (note, undoTarget);
}

juce::ValueTree Edit::getNote (int noteId) const
{
    return findById (ids::NOTE, noteId);
}

//==============================================================================
juce::ValueTree Edit::addClip (int trackId, const juce::File& file, double startSeconds, double lengthSeconds,
                               double offsetSeconds)
{
    auto track = getTrack (trackId);
    if (! track.isValid())
        return {};

    juce::ValueTree clip (ids::CLIP);
    clip.setProperty (ids::id, takeNextId(), nullptr);
    clip.setProperty (ids::name, file.getFileNameWithoutExtension(), nullptr);
    clip.setProperty (ids::file, file.getFullPathName(), nullptr);
    clip.setProperty (ids::start, startSeconds, nullptr);
    clip.setProperty (ids::offset, juce::jmax (0.0, offsetSeconds), nullptr);
    clip.setProperty (ids::length, juce::jmax (0.0, lengthSeconds), nullptr);
    clip.setProperty (ids::gain, 0.0f, nullptr);
    clip.setProperty (ids::fadeIn, 0.0, nullptr);
    clip.setProperty (ids::fadeOut, 0.0, nullptr);
    track.appendChild (clip, undoTarget);
    return clip;
}

void Edit::removeClip (int clipId)
{
    if (auto clip = getClip (clipId); clip.isValid())
        clip.getParent().removeChild (clip, undoTarget);
}

juce::ValueTree Edit::getClip (int clipId) const
{
    return findById (ids::CLIP, clipId);
}

void Edit::moveClip (int clipId, double newStart, int toTrackId)
{
    auto clip = getClip (clipId);
    if (! clip.isValid())
        return;

    const auto delta = juce::jmax (0.0, newStart) - (double) clip[ids::start];
    forPartners (clipId, [&] (int partner) { moveClip (partner, (double) getClip (partner)[ids::start] + delta); });

    if (isMusical (clip))
    {
        // Keep its length in beats where it lands (the tempo may differ there).
        const auto tempo = getTempoMapFor (clip);
        const auto start = (double) clip[ids::start], offset = (double) clip[ids::offset], length = (double) clip[ids::length];
        const auto shift = tempo.toBeats (juce::jmax (0.0, newStart)) - tempo.toBeats (start);
        placeByBeats (clip, tempo, tempo.toBeats (start - offset) + shift, tempo.toBeats (start) + shift, tempo.toBeats (start + length) + shift);
    }
    else
    {
        clip.setProperty (ids::start, juce::jmax (0.0, newStart), undoTarget);
    }

    if (auto target = getTrack (toTrackId); target.isValid() && target != clip.getParent())
    {
        clip.getParent().removeChild (clip, undoTarget);
        target.appendChild (clip, undoTarget);
    }
}

void Edit::trimClipStart (int clipId, double newStart)
{
    auto clip = getClip (clipId);
    if (! clip.isValid())
        return;

    const auto start = (double) clip[ids::start], offset = (double) clip[ids::offset], length = (double) clip[ids::length];
    const auto end = start + length;
    const auto clamped = juce::jlimit (juce::jmax (0.0, start - offset), end - minClipSeconds, newStart);
    const auto delta = clamped - start;

    clip.setProperty (ids::start, clamped, undoTarget);
    clip.setProperty (ids::offset, offset + delta, undoTarget);
    clip.setProperty (ids::length, length - delta, undoTarget);
}

void Edit::trimClipEnd (int clipId, double newEnd)
{
    auto clip = getClip (clipId);
    if (! clip.isValid())
        return;

    const auto start = (double) clip[ids::start];
    clip.setProperty (ids::length, juce::jmax (minClipSeconds, newEnd - start), undoTarget);
}

int Edit::splitClip (int clipId, double time)
{
    auto clip = getClip (clipId);
    if (! clip.isValid())
        return -1;

    const auto start = (double) clip[ids::start], offset = (double) clip[ids::offset], length = (double) clip[ids::length];

    if (time <= start + minClipSeconds || time >= start + length - minClipSeconds)
        return -1;

    auto right = clip.createCopy();
    renumber (right);
    const auto id = (int) right[ids::id];
    right.setProperty (ids::start, time, nullptr);
    right.setProperty (ids::offset, offset + (time - start), nullptr);
    right.setProperty (ids::length, start + length - time, nullptr);
    right.setProperty (ids::fadeIn, 0.0, nullptr);

    clip.setProperty (ids::length, time - start, undoTarget);
    clip.setProperty (ids::fadeOut, 0.0, undoTarget);
    clip.getParent().addChild (right, clip.getParent().indexOf (clip) + 1, undoTarget);
    return id;
}

void Edit::setClipTranspose (int clipId, double semitones)
{
    if (auto clip = getClip (clipId); clip.isValid())
        clip.setProperty (ids::transpose, juce::jlimit (-warp::Settings::maxSemitones, warp::Settings::maxSemitones, semitones), undoTarget);
}

void Edit::setClipStretch (int clipId, warp::Algorithm algorithm)
{
    forPartners (clipId, [&] (int partner) { setClipStretch (partner, algorithm); });
    if (auto clip = getClip (clipId); clip.isValid())
    {
        if (algorithm == warp::Algorithm::mix)
            clip.removeProperty (ids::stretch, undoTarget);
        else
            clip.setProperty (ids::stretch, warp::getAlgorithmKey (algorithm), undoTarget);
    }
}

void Edit::setClipFormant (int clipId, bool keep)
{
    forPartners (clipId, [&] (int partner) { setClipFormant (partner, keep); });
    if (auto clip = getClip (clipId); clip.isValid())
    {
        if (keep)
            clip.setProperty (ids::formant, true, undoTarget);
        else
            clip.removeProperty (ids::formant, undoTarget);
    }
}

void Edit::setClipNotes (int clipId, const std::vector<analysis::pitch::Note>& notes)
{
    if (auto clip = getClip (clipId); clip.isValid() && ! isMidiClip (clip))
    {
        if (analysis::pitch::anyEdited (notes))
            clip.setProperty (ids::pitchNotes, analysis::pitch::toString (notes), undoTarget);
        else
            clip.removeProperty (ids::pitchNotes, undoTarget);
    }
}

std::vector<analysis::pitch::Note> Edit::getClipNotes (const juce::ValueTree& clip)
{
    return analysis::pitch::fromString (clip[ids::pitchNotes].toString());
}

void Edit::setTrackTranspose (int trackId, double semitones)
{
    if (auto track = getTrack (trackId); track.isValid())
        track.setProperty (ids::transpose, juce::jlimit (-warp::Settings::maxSemitones, warp::Settings::maxSemitones, semitones), undoTarget);
}

void Edit::setVolumeRide (int trackId, const std::vector<RidePoint>& points)
{
    auto track = getTrack (trackId);
    if (! track.isValid())
        return;

    if (points.empty())
    {
        track.removeProperty (ids::ride, undoTarget);
        return;
    }

    auto sorted = points;
    std::stable_sort (sorted.begin(), sorted.end(), [] (const RidePoint& a, const RidePoint& b) { return a.time < b.time; });

    juce::StringArray parts;
    for (const auto& point : sorted)
        parts.add (juce::String (juce::jmax (0.0, point.time), 3) + ":" + juce::String (juce::jlimit (-maxRideDb, maxRideDb, point.db), 2));
    track.setProperty (ids::ride, parts.joinIntoString (";"), undoTarget);
}

void Edit::setInsertAutomation (int insertId, const juce::String& valueId, const std::vector<AutomationPoint>& points)
{
    auto insert = getInsert (insertId);
    if (! insert.isValid() || ! insert.hasProperty (juce::Identifier (valueId)))
        return;

    auto lanes = getInsertAutomation (insert);
    if (points.empty())
        lanes.erase (valueId);
    else
    {
        auto sorted = points;
        std::stable_sort (sorted.begin(), sorted.end(), [] (const auto& a, const auto& b) { return a.time < b.time; });
        lanes[valueId] = sorted;
    }

    if (lanes.empty())
    {
        insert.removeProperty (ids::automation, undoTarget);
        return;
    }

    juce::StringArray text;
    for (const auto& [id, lane] : lanes)
    {
        juce::StringArray parts;
        for (const auto& point : lane)
            parts.add (juce::String (juce::jmax (0.0, point.time), 3) + ":" + juce::String (point.value, 3));
        text.add (id + "=" + parts.joinIntoString (";"));
    }
    insert.setProperty (ids::automation, text.joinIntoString ("|"), undoTarget);
}

Edit::Automation Edit::getInsertAutomation (const juce::ValueTree& insert)
{
    Automation lanes;
    for (const auto& lane : juce::StringArray::fromTokens (insert[ids::automation].toString(), "|", ""))
    {
        const auto id = lane.upToFirstOccurrenceOf ("=", false, false).trim();
        if (id.isEmpty() || ! lane.containsChar ('='))
            continue;
        std::vector<AutomationPoint> points;
        for (const auto& part : juce::StringArray::fromTokens (lane.fromFirstOccurrenceOf ("=", false, false), ";", ""))
            if (part.containsChar (':'))
                points.push_back ({ juce::jmax (0.0, part.upToFirstOccurrenceOf (":", false, false).getDoubleValue()),
                                    (float) part.fromFirstOccurrenceOf (":", false, false).getDoubleValue() });
        std::stable_sort (points.begin(), points.end(), [] (const auto& a, const auto& b) { return a.time < b.time; });
        if (! points.empty())
            lanes[id] = points;
    }
    return lanes;
}

float Edit::getAutomationValueAt (const std::vector<AutomationPoint>& points, double time)
{
    if (points.empty())
        return 0.0f;
    if (time <= points.front().time)
        return points.front().value;
    if (time >= points.back().time)
        return points.back().value;
    const auto next = std::upper_bound (points.begin(), points.end(), time, [] (double t, const AutomationPoint& p) { return t < p.time; });
    const auto prev = next - 1;
    const auto x = (time - prev->time) / juce::jmax (1.0e-9, next->time - prev->time);
    return (float) (prev->value + x * (next->value - prev->value));
}

void Edit::clearTrackAutomation (int trackId)
{
    for (auto insert : getAllInserts (getChannel (trackId)))
        if (insert.hasProperty (ids::automation))
            insert.removeProperty (ids::automation, undoTarget);
}

void Edit::clearLanes (int trackId)
{
    clearTrackAutomation (trackId);
    auto track = getTrack (trackId);
    if (! track.isValid())
        return;
    track.removeProperty (ids::ride, undoTarget);
    track.removeProperty (ids::panAutomation, undoTarget);
    for (auto child : track)
        if (child.hasType (ids::SEND) && child.hasProperty (ids::automation))
            child.removeProperty (ids::automation, undoTarget);
    if (auto macros = getMacros (track); macros.isValid())
        for (auto macro : macros)
            if (macro.hasProperty (ids::automation))
                macro.removeProperty (ids::automation, undoTarget);
}

juce::Array<juce::ValueTree> Edit::getAllInserts (const juce::ValueTree& channel)
{
    juce::Array<juce::ValueTree> list;
    std::function<void (const juce::ValueTree&)> add = [&] (const juce::ValueTree& inserts)
    {
        for (const auto& insert : inserts)
        {
            if (! insert.hasType (ids::INSERT))
                continue;
            list.add (insert);
            for (const auto& chain : insert)
                if (chain.hasType (ids::CHAIN))
                    add (chain.getChildWithName (ids::INSERTS));
        }
    };
    add (getInserts (channel));
    return list;
}

//==============================================================================
std::vector<Edit::AutomationPoint> Edit::parsePoints (const juce::String& text)
{
    std::vector<AutomationPoint> points;
    for (const auto& part : juce::StringArray::fromTokens (text, ";", ""))
        if (part.containsChar (':'))
        {
            const auto time = part.upToFirstOccurrenceOf (":", false, false).getDoubleValue();
            const auto value = part.fromFirstOccurrenceOf (":", false, false).getDoubleValue();
            if (std::isfinite (time) && std::isfinite (value))
                points.push_back ({ juce::jmax (0.0, time), (float) value });
        }
    std::stable_sort (points.begin(), points.end(), [] (const auto& a, const auto& b) { return a.time < b.time; });
    return points;
}

juce::String Edit::pointsToString (const std::vector<AutomationPoint>& points, int valueDecimals)
{
    juce::StringArray parts;
    for (const auto& point : points)
        parts.add (juce::String (juce::jmax (0.0, point.time), 3) + ":" + juce::String (point.value, valueDecimals));
    return parts.joinIntoString (";");
}

namespace
{
    juce::ValueTree findMacro (juce::ValueTree channel, int index, juce::UndoManager* undo, bool create); // below

    // A lane target split up: "send:3" -> { "send", 3, "" }, "insert:5:delayMix" -> { "insert", 5, "delayMix" }.
    struct LaneTarget
    {
        juce::String kind, value;
        int id = -1;
    };

    LaneTarget parseTarget (const juce::String& target)
    {
        LaneTarget t;
        t.kind = target.upToFirstOccurrenceOf (":", false, false);
        const auto rest = target.fromFirstOccurrenceOf (":", false, false);
        if (t.kind == "send" || t.kind == "macro")
            t.id = rest.containsOnly ("0123456789") && rest.isNotEmpty() ? rest.getIntValue() : -1;
        else if (t.kind == "insert")
        {
            const auto id = rest.upToFirstOccurrenceOf (":", false, false);
            t.id = id.containsOnly ("0123456789") && id.isNotEmpty() ? id.getIntValue() : -1;
            t.value = rest.fromFirstOccurrenceOf (":", false, false);
        }
        return t;
    }

    juce::ValueTree findTrackInsert (const juce::ValueTree& track, int insertId)
    {
        for (const auto& insert : Edit::getAllInserts (track))
            if ((int) insert[ids::id] == insertId)
                return insert;
        return {};
    }

    juce::ValueTree findTrackMacro (const juce::ValueTree& track, int index)
    {
        const auto macros = Edit::getMacros (track);
        return macros.isValid() ? macros.getChildWithProperty (ids::index, index) : juce::ValueTree();
    }
}

std::pair<float, float> Edit::getLaneRange (const juce::ValueTree&, const juce::String& target)
{
    const auto t = parseTarget (target);
    if (t.kind == "volume") return { -maxRideDb, maxRideDb };
    if (t.kind == "pan")    return { -1.0f, 1.0f };
    if (t.kind == "send")   return { minSendDb - 1.0f, maxSendDb };
    if (t.kind == "macro")  return { 0.0f, 1.0f };
    return { -std::numeric_limits<float>::max(), std::numeric_limits<float>::max() };
}

bool Edit::setLane (int trackId, const juce::String& target, const std::vector<AutomationPoint>& input)
{
    auto track = getTrack (trackId);
    if (! track.isValid())
        return false;

    const auto t = parseTarget (target);
    const auto [lo, hi] = getLaneRange (track, target);
    std::vector<AutomationPoint> points;
    for (const auto& point : input)
        if (std::isfinite (point.time) && std::isfinite (point.value))
            points.push_back ({ juce::jmax (0.0, point.time), juce::jlimit (lo, hi, point.value) });
    std::stable_sort (points.begin(), points.end(), [] (const auto& a, const auto& b) { return a.time < b.time; });
    if (points.size() > (size_t) maxLanePoints)
        points.resize ((size_t) maxLanePoints);

    if (t.kind == "volume")
    {
        std::vector<RidePoint> ride;
        for (const auto& point : points)
            ride.push_back ({ point.time, point.value });
        setVolumeRide (trackId, ride);
        return true;
    }

    const auto store = [&] (juce::ValueTree tree, const juce::Identifier& property)
    {
        if (points.empty())
            tree.removeProperty (property, undoTarget);
        else
            tree.setProperty (property, pointsToString (points), undoTarget);
    };

    if (t.kind == "pan")
    {
        store (track, ids::panAutomation);
        return true;
    }

    if (t.kind == "send")
    {
        if (! getReturn (t.id).isValid() || isGroup (getReturn (t.id)) || t.id == getTrackOutput (track))
            return false;
        auto send = getSend (track, t.id);
        if (! send.isValid())
        {
            if (points.empty())
                return true;
            send = juce::ValueTree (ids::SEND);
            send.setProperty (ids::id, takeNextId(), nullptr);
            send.setProperty (ids::target, t.id, nullptr);
            send.setProperty (ids::level, minSendDb - 1.0f, nullptr);
            send.setProperty (ids::pre, false, nullptr);
            track.appendChild (send, undoTarget);
        }
        store (send, ids::automation);
        return true;
    }

    if (t.kind == "insert")
    {
        const auto insert = findTrackInsert (track, t.id);
        if (! insert.isValid() || t.value.isEmpty() || ! insert.hasProperty (juce::Identifier (t.value)))
            return false;
        setInsertAutomation (t.id, t.value, points);
        return true;
    }

    if (t.kind == "macro")
    {
        if (t.id < 0 || t.id >= numMacros)
            return false;
        auto macro = findTrackMacro (track, t.id);
        if (! macro.isValid())
        {
            if (points.empty())
                return true;
            macro = findMacro (track, t.id, undoTarget, true);
        }
        store (macro, ids::automation);
        return true;
    }

    return false;
}

std::vector<Edit::AutomationPoint> Edit::getLane (const juce::ValueTree& track, const juce::String& target)
{
    const auto t = parseTarget (target);
    if (t.kind == "volume")
    {
        std::vector<AutomationPoint> points;
        for (const auto& point : getVolumeRide (track))
            points.push_back ({ point.time, point.db });
        return points;
    }
    if (t.kind == "pan")
        return parsePoints (track[ids::panAutomation].toString());
    if (t.kind == "send")
        return parsePoints (getSend (track, t.id)[ids::automation].toString());
    if (t.kind == "macro")
        return parsePoints (findTrackMacro (track, t.id)[ids::automation].toString());
    if (t.kind == "insert")
    {
        const auto lanes = getInsertAutomation (findTrackInsert (track, t.id));
        const auto it = lanes.find (t.value);
        return it != lanes.end() ? it->second : std::vector<AutomationPoint>();
    }
    return {};
}

juce::StringArray Edit::getLaneTargets (const juce::ValueTree& track)
{
    juce::StringArray targets;
    if (track.hasProperty (ids::ride))          targets.add ("volume");
    if (track.hasProperty (ids::panAutomation)) targets.add ("pan");
    for (const auto& send : track)
        if (send.hasType (ids::SEND) && send.hasProperty (ids::automation))
            targets.add ("send:" + juce::String ((int) send[ids::target]));
    for (const auto& insert : getAllInserts (track))
        for (const auto& [valueId, points] : getInsertAutomation (insert))
            targets.add ("insert:" + juce::String ((int) insert[ids::id]) + ":" + valueId);
    if (const auto macros = getMacros (track); macros.isValid())
        for (int index = 0; index < numMacros; ++index)
            if (macros.getChildWithProperty (ids::index, index).hasProperty (ids::automation))
                targets.add ("macro:" + juce::String (index));
    return targets;
}

bool Edit::followsAutomatedMacro (const juce::ValueTree& insert, const juce::String& param)
{
    const auto macros = getMacros (getChannelOf (insert));
    if (! macros.isValid())
        return false;
    const auto insertId = (int) insert[ids::id];
    for (const auto& macro : macros)
        if (macro.hasProperty (ids::automation))
            for (const auto& map : macro)
                if ((int) map[ids::insert] == insertId && map[ids::param].toString() == param)
                    return true;
    return false;
}

std::vector<Edit::RidePoint> Edit::getVolumeRide (const juce::ValueTree& track)
{
    std::vector<RidePoint> points;
    for (const auto& part : juce::StringArray::fromTokens (track[ids::ride].toString(), ";", ""))
    {
        if (! part.containsChar (':'))
            continue;
        const auto time = part.upToFirstOccurrenceOf (":", false, false).getDoubleValue();
        const auto db = (float) part.fromFirstOccurrenceOf (":", false, false).getDoubleValue();
        points.push_back ({ juce::jmax (0.0, time), juce::jlimit (-maxRideDb, maxRideDb, db) });
    }
    std::stable_sort (points.begin(), points.end(), [] (const RidePoint& a, const RidePoint& b) { return a.time < b.time; });
    return points;
}

float Edit::getRideDbAt (const std::vector<RidePoint>& points, double time)
{
    if (points.empty())
        return 0.0f;
    if (time <= points.front().time)
        return points.front().db;
    if (time >= points.back().time)
        return points.back().db;

    const auto next = std::upper_bound (points.begin(), points.end(), time, [] (double t, const RidePoint& p) { return t < p.time; });
    const auto& b = *next;
    const auto& a = *(next - 1);
    const auto span = b.time - a.time;
    return span > 0.0 ? a.db + (b.db - a.db) * (float) ((time - a.time) / span) : b.db;
}

void Edit::setClipWarp (int clipId, const warp::Map& map)
{
    auto clip = getClip (clipId);
    if (! clip.isValid())
        return;

    // Partners get the same timing: the same free warp when they're musical alike,
    // otherwise the same whole map.
    forPartners (clipId, [&] (int partnerId)
    {
        const auto partner = getClip (partnerId);
        if (isMusical (partner) == isMusical (clip) && (! isMusical (clip) || sameBend (partner, clip)))
            setClipWarp (partnerId, map);
        else
            setEffectiveWarp (partnerId, isMusical (clip) ? warp::compose (map, getMusicalMap (clip)) : map);
    });

    const auto text = map.toString();
    if (text.isEmpty())
        clip.removeProperty (ids::warp, undoTarget);
    else
        clip.setProperty (ids::warp, text, undoTarget);
}

warp::Map Edit::getClipWarp (const juce::ValueTree& clip)
{
    return warp::Map::fromString (clip[ids::warp].toString());
}

//==============================================================================
TempoMap Edit::getTempoMapFor (const juce::ValueTree& clip)
{
    const auto project = getOwnerTrack (clip).getParent();
    if (! project.hasType (ids::PROJECT))
        return TempoMap();
    return TempoMap::fromProject (project.getProperty (ids::tempo, 120.0), project[ids::tempoChanges].toString());
}

double Edit::getClipTempo (const juce::ValueTree& clip)
{
    if (const auto map = getBeatMap (clip); map.size() >= 2)
        return juce::jlimit (minClipTempo, maxClipTempo, 60.0 * (map[1].beat - map[0].beat) / (map[1].seconds - map[0].seconds));
    if (const auto bpm = (double) clip.getProperty (ids::clipTempo, 0.0); bpm > 0.0)
        return juce::jlimit (minClipTempo, maxClipTempo, bpm);
    const auto start = (double) clip.getProperty (ids::start, 0.0);
    return juce::jlimit (minClipTempo, maxClipTempo, getTempoMapFor (clip).getBpmAt (start));
}

warp::Map Edit::getMusicalMap (const juce::ValueTree& clip)
{
    if (! isMusical (clip))
        return {};

    const auto tempo = getTempoMapFor (clip);
    const auto anchorSeconds = (double) clip[ids::start] - (double) clip[ids::offset];
    const auto anchor = tempo.toBeats (anchorSeconds);

    // The file's own beat at straight time x: its tempo definition, or steady at clipTempo
    // (the maths of clips without a definition is kept exactly as it was).
    const auto definition = getBeatMap (clip);
    const auto beatsPerSecond = getClipTempo (clip) / 60.0;
    const auto origin = definition.empty() ? 0.0 : beatAt (definition, 0.0);
    const auto ownBeat = [&] (double x) { return definition.empty() ? x * beatsPerSecond : beatAt (definition, x) - origin; };
    const auto ownSeconds = [&] (double beat) { return definition.empty() ? beat / beatsPerSecond : secondsAt (definition, beat + origin); };

    // Straight time x (file time 0 at the clip's anchor) plays at timeline beat anchor + its own beat.
    const auto rendered = [&] (double x) { return tempo.toSeconds (anchor + ownBeat (x)) - anchorSeconds; };
    const auto straight = [&] (double beat) { return ownSeconds (beat - anchor); };

    // Corners: far before and after (the map runs on at its last slope only through
    // markers), every tempo point, and every half beat along ramps.
    constexpr double far = 1.0e5;
    std::vector<double> xs { -far, 0.0, far };
    for (const auto& point : definition)
        xs.push_back (point.seconds);
    const auto& points = tempo.getPoints();
    for (size_t i = 0; i < points.size(); ++i)
    {
        xs.push_back (straight (points[i].beat));
        if (i > 0 && points[i].ramp)
            for (auto b = points[i - 1].beat + 0.5; b < points[i].beat; b += 0.5)
                xs.push_back (straight (b));
    }
    std::sort (xs.begin(), xs.end());

    std::vector<warp::Marker> markers;
    for (auto x : xs)
        if (x >= -far && x <= far && (markers.empty() || x - markers.back().source > 1.0e-6))
            markers.push_back ({ x, rendered (x) });

    return warp::Map (std::move (markers));
}

warp::Map Edit::getEffectiveWarp (const juce::ValueTree& clip)
{
    const auto free = getClipWarp (clip);
    return isMusical (clip) ? warp::compose (free, getMusicalMap (clip)) : free;
}

void Edit::setEffectiveWarp (int clipId, const warp::Map& map)
{
    const auto clip = getClip (clipId);
    if (! clip.isValid() || ! isMusical (clip))
    {
        setClipWarp (clipId, map);
        return;
    }

    const auto bend = getMusicalMap (clip);
    std::vector<warp::Marker> free;
    for (const auto& m : map.getMarkers())
        free.push_back ({ m.source, bend.toSource (m.target) });
    setClipWarp (clipId, warp::Map (std::move (free)));
}

std::vector<warp::Marker> Edit::getShownWarp (const juce::ValueTree& clip)
{
    auto markers = getClipWarp (clip).getMarkers();
    if (isMusical (clip))
    {
        const auto bend = getMusicalMap (clip);
        for (auto& m : markers)
            m.target = bend.toTarget (m.target);
    }
    return markers;
}

void Edit::keepWindow (juce::ValueTree& clip, const warp::Map& before)
{
    const auto after = getEffectiveWarp (clip);
    const auto start = (double) clip[ids::start], offset = (double) clip[ids::offset], length = (double) clip[ids::length];
    const auto from = before.toSource (offset), to = before.toSource (offset + length);
    const auto newOffset = after.toTarget (from);
    clip.setProperty (ids::offset, newOffset, undoTarget);
    clip.setProperty (ids::start, juce::jmax (0.0, start - offset + newOffset), undoTarget);
    clip.setProperty (ids::length, juce::jmax (minClipSeconds, after.toTarget (to) - newOffset), undoTarget);
}

void Edit::setClipMusical (int clipId, bool on, double clipTempo)
{
    auto clip = getClip (clipId);
    if (! clip.isValid() || isMidiClip (clip))
        return;

    forPartners (clipId, [&] (int partner) { setClipMusical (partner, on, clipTempo > 0.0 ? clipTempo : getClipTempo (clip)); });

    const auto before = getEffectiveWarp (clip);
    const auto bpm = clipTempo > 0.0 ? juce::jlimit (minClipTempo, maxClipTempo, clipTempo) : getClipTempo (clip);

    // The free warp stays as it is: while musical it's read in the file's own time. A tempo
    // definition scales around its downbeat to the new tempo.
    if (auto map = getBeatMap (clip); map.size() >= 2 && clipTempo > 0.0)
        clip.setProperty (ids::beatMap, beatMapToString (scaleBeatMap (map, bpm / getClipTempo (clip))), undoTarget);
    clip.setProperty (ids::clipTempo, bpm, undoTarget);
    if (on)
        clip.setProperty (ids::musical, true, undoTarget);
    else
        clip.removeProperty (ids::musical, undoTarget);

    keepWindow (clip, before);
}

bool Edit::sameBend (const juce::ValueTree& a, const juce::ValueTree& b)
{
    return juce::approximatelyEqual (getClipTempo (a), getClipTempo (b)) && a[ids::beatMap].toString() == b[ids::beatMap].toString();
}

//==============================================================================
Edit::BeatMap Edit::parseBeatMap (const juce::String& text)
{
    BeatMap map;
    for (const auto& item : juce::StringArray::fromTokens (text, ";", ""))
    {
        if (! item.containsChar (':'))
            continue;
        const BeatPoint point { item.upToFirstOccurrenceOf (":", false, false).getDoubleValue(),
                                item.fromFirstOccurrenceOf (":", false, false).getDoubleValue() };
        if (! std::isfinite (point.seconds) || ! std::isfinite (point.beat))
            continue;
        if (! map.empty())
        {
            const auto& last = map.back();
            const auto seconds = point.seconds - last.seconds, beats = point.beat - last.beat;
            if (seconds <= 1.0e-4 || beats <= 1.0e-4)
                continue;
            if (const auto bpm = 60.0 * beats / seconds; bpm < minSegmentTempo || bpm > maxSegmentTempo)
                continue;
        }
        map.push_back (point);
    }
    if (map.size() < 2)
        map.clear();
    return map;
}

juce::String Edit::beatMapToString (const BeatMap& map)
{
    juce::StringArray items;
    for (const auto& p : map)
        items.add (juce::String (p.seconds, 6) + ":" + juce::String (p.beat, 6));
    return map.size() < 2 ? juce::String() : items.joinIntoString (";");
}

Edit::BeatMap Edit::getBeatMap (const juce::ValueTree& clip)
{
    return clip.hasProperty (ids::beatMap) ? parseBeatMap (clip[ids::beatMap].toString()) : BeatMap();
}

Edit::BeatMap Edit::getDefinition (const juce::ValueTree& clip)
{
    if (auto map = getBeatMap (clip); ! map.empty())
        return map;
    return steadyBeatMap (0.0, getClipTempo (clip));
}

Edit::BeatMap Edit::steadyBeatMap (double downbeatSeconds, double bpm)
{
    bpm = juce::jlimit (minClipTempo, maxClipTempo, bpm);
    return { { downbeatSeconds, 0.0 }, { downbeatSeconds + 240.0 / bpm, 4.0 } };
}

Edit::BeatMap Edit::scaleBeatMap (const BeatMap& map, double factor)
{
    auto scaled = map;
    if (factor > 0.0)
        for (auto& p : scaled)
            p.beat *= factor;
    return scaled;
}

double Edit::beatAt (const BeatMap& map, double seconds)
{
    if (map.size() < 2)
        return seconds * 2.0; // 120 BPM: never asked for in practice
    size_t i = 1;
    while (i + 1 < map.size() && seconds > map[i].seconds)
        ++i;
    const auto& a = map[i - 1];
    const auto& b = map[i];
    return a.beat + (seconds - a.seconds) * (b.beat - a.beat) / (b.seconds - a.seconds);
}

double Edit::secondsAt (const BeatMap& map, double beat)
{
    if (map.size() < 2)
        return beat * 0.5;
    size_t i = 1;
    while (i + 1 < map.size() && beat > map[i].beat)
        ++i;
    const auto& a = map[i - 1];
    const auto& b = map[i];
    return a.seconds + (beat - a.beat) * (b.seconds - a.seconds) / (b.beat - a.beat);
}

void Edit::setBeatMap (int clipId, const BeatMap& map)
{
    auto clip = getClip (clipId);
    if (! clip.isValid() || isMidiClip (clip))
        return;

    const auto text = beatMapToString (parseBeatMap (beatMapToString (map)));
    forPartners (clipId, [&] (int partner) { setBeatMap (partner, parseBeatMap (text)); });

    const auto before = getEffectiveWarp (clip);
    const auto tempo = getClipTempo (clip); // kept when the definition goes
    if (text.isEmpty())
    {
        clip.removeProperty (ids::beatMap, undoTarget);
        clip.setProperty (ids::clipTempo, tempo, undoTarget);
    }
    else
    {
        clip.setProperty (ids::beatMap, text, undoTarget);
        clip.setProperty (ids::clipTempo, getClipTempo (clip), undoTarget);
    }
    keepWindow (clip, before);
}

void Edit::placeWindow (juce::ValueTree& clip, double from, double to, double start)
{
    clip.setProperty (ids::start, juce::jmax (0.0, start), undoTarget);
    // A musical clip's bend depends on where it sits, so settle it twice (exact on a steady tempo).
    for (int pass = 0; pass < 2; ++pass)
    {
        const auto map = getEffectiveWarp (clip);
        const auto offset = map.toTarget (from);
        clip.setProperty (ids::offset, offset, undoTarget);
        clip.setProperty (ids::length, juce::jmax (minClipSeconds, map.toTarget (to) - offset), undoTarget);
    }
}

void Edit::stretchClip (int clipId, double start, double length)
{
    auto clip = getClip (clipId);
    if (! clip.isValid() || isMidiClip (clip))
        return;

    const auto oldStart = (double) clip[ids::start], offset = (double) clip[ids::offset], oldLength = (double) clip[ids::length];
    if (oldLength < minClipSeconds || length <= 0.0)
        return;
    const auto factor = juce::jlimit (minEdgeStretch, maxEdgeStretch, length / oldLength);
    length = oldLength * factor;

    forPartners (clipId, [&] (int partner)
    {
        const auto other = getClip (partner);
        stretchClip (partner, (double) other[ids::start] + (start - oldStart), (double) other[ids::length] * factor);
    });

    const auto before = getEffectiveWarp (clip);
    const auto from = before.toSource (offset), to = before.toSource (offset + oldLength);

    if (isMusical (clip))
    {
        // Longer at the same project tempo = more of the audio's own beats in it: they're
        // shorter than it was told, so its tempo goes up by the same factor.
        const auto map = getDefinition (clip);
        clip.setProperty (ids::beatMap, beatMapToString (scaleBeatMap (map, factor)), undoTarget);
        clip.setProperty (ids::clipTempo, getClipTempo (clip), undoTarget);
        placeWindow (clip, from, to, start);
        return;
    }

    // The whole map scaled about rendered time 0, pinned at the file's start and at the
    // window's two ends so nothing outside the markers runs on at the old speed.
    std::vector<warp::Marker> markers { { 0.0, before.toTarget (0.0) * factor },
                                        { from, before.toTarget (from) * factor },
                                        { to, before.toTarget (to) * factor } };
    for (const auto& m : before.getMarkers())
        markers.push_back ({ m.source, m.target * factor });
    std::sort (markers.begin(), markers.end(), [] (auto& a, auto& b) { return a.source < b.source; });
    std::vector<warp::Marker> unique;
    for (const auto& m : markers)
        if (m.source >= 0.0 && (unique.empty() || m.source - unique.back().source > 1.0e-6))
            unique.push_back (m);

    setClipWarp (clipId, warp::Map (std::move (unique)));
    placeWindow (clip, from, to, start);
}

warp::Settings Edit::getFlattenSettings (const juce::ValueTree& clip)
{
    // What the clip plays, without the compare switches and without its track's transpose
    // (that stays on the track and keeps applying).
    warp::Settings settings;
    settings.map = getEffectiveWarp (clip);
    settings.semitones = juce::jlimit (-warp::Settings::maxSemitones, warp::Settings::maxSemitones, (double) clip.getProperty (ids::transpose, 0.0));
    settings.algorithm = warp::algorithmFromKey (clip[ids::stretch].toString());
    settings.formant = (bool) clip.getProperty (ids::formant, false);
    const auto partners = getLinkedClips (clip);
    if (partners.isEmpty())
        settings.notes = getClipNotes (clip);
    if (! settings.isNeutral() && ! partners.isEmpty())
    {
        settings.linked.add (clip[ids::file].toString());
        for (const auto& partner : partners)
            settings.linked.addIfNotAlreadyThere (partner[ids::file].toString());
        settings.linked.sort (false);
    }
    return settings;
}

void Edit::flattenClip (int clipId, const juce::File& file)
{
    auto clip = getClip (clipId);
    if (! clip.isValid() || isMidiClip (clip))
        return;

    // Offset and length were already in the rendered copy's time, which is the new file's.
    clip.setProperty (ids::file, file.getFullPathName(), undoTarget);
    for (const auto& id : { ids::warp, ids::transpose, ids::pitchNotes, ids::musical, ids::clipTempo, ids::beatMap,
                            ids::timeBypass, ids::warpBypass, ids::hitpoints })
        clip.removeProperty (id, undoTarget);
}

//==============================================================================
void Edit::linkTracks (const juce::Array<int>& trackIds)
{
    auto group = 0, highest = 0;
    for (const auto& track : getTracks())
    {
        highest = juce::jmax (highest, getLink (track));
        if (group == 0 && trackIds.contains ((int) track[ids::id]))
            group = getLink (track);
    }

    if (group == 0)
        group = highest + 1;

    for (auto id : trackIds)
        if (auto track = getTrack (id); track.isValid() && ! isMidiTrack (track))
            track.setProperty (ids::link, group, undoTarget);
}

void Edit::unlinkTrack (int trackId)
{
    if (auto track = getTrack (trackId); track.isValid())
        track.removeProperty (ids::link, undoTarget);
}

juce::Array<juce::ValueTree> Edit::getLinkedClips (const juce::ValueTree& clip)
{
    juce::Array<juce::ValueTree> partners;
    const auto track = clip.getParent(); // a take's clip has no partners: its TAKE has no link
    const auto group = getLink (track);
    const auto project = track.getParent();
    if (group == 0 || isMidiClip (clip) || ! project.isValid())
        return partners;

    const auto start = (double) clip[ids::start], end = start + (double) clip[ids::length];
    const auto anchor = start - (double) clip[ids::offset];

    for (const auto& other : project)
    {
        if (! other.hasType (ids::TRACK) || other == track || getLink (other) != group)
            continue;

        for (const auto& candidate : other)
        {
            if (! candidate.hasType (ids::CLIP) || isMidiClip (candidate))
                continue;
            const auto otherStart = (double) candidate[ids::start], otherEnd = otherStart + (double) candidate[ids::length];
            const auto otherAnchor = otherStart - (double) candidate[ids::offset];
            if (std::abs (otherAnchor - anchor) < 0.001 && otherStart < end && otherEnd > start)
                partners.add (candidate);
        }
    }

    return partners;
}

void Edit::forPartners (int clipId, const std::function<void (int)>& apply)
{
    if (propagating)
        return;

    const auto partners = getLinkedClips (getClip (clipId));
    if (partners.isEmpty())
        return;

    propagating = true;
    for (const auto& partner : partners)
        apply ((int) partner[ids::id]);
    propagating = false;
}

void Edit::setClipHitpoints (int clipId, const juce::String& filter, const juce::String& edits)
{
    auto clip = getClip (clipId);
    if (! clip.isValid() || isMidiClip (clip))
        return;
    for (const auto& [id, text] : { std::pair { ids::hitFilter, filter }, std::pair { ids::hitpoints, edits } })
    {
        if (text.isEmpty())
            clip.removeProperty (id, undoTarget);
        else
            clip.setProperty (id, text, undoTarget);
    }
}

warp::Map Edit::getTimeBypassMap (const juce::ValueTree& clip)
{
    const auto free = getClipWarp (clip);
    const auto effective = getEffectiveWarp (clip);
    if (free.getMarkers().empty())
        return effective;

    const auto offset = (double) clip[ids::offset], length = (double) clip[ids::length];
    const auto from = effective.toSource (offset), to = effective.toSource (offset + length);
    if (to - from < 1.0e-3)
        return effective;

    const warp::Map straight ({ { from, free.toTarget (from) }, { to, free.toTarget (to) } });
    return isMusical (clip) ? warp::compose (straight, getMusicalMap (clip)) : straight;
}

warp::Settings Edit::getWarpSettings (const juce::ValueTree& clip)
{
    warp::Settings settings;
    settings.map = getEffectiveWarp (clip);
    settings.semitones = (double) clip.getProperty (ids::transpose, 0.0) + (double) getOwnerTrack (clip).getProperty (ids::transpose, 0.0);
    settings.semitones = juce::jlimit (-warp::Settings::maxSemitones, warp::Settings::maxSemitones, settings.semitones);
    settings.algorithm = warp::algorithmFromKey (clip[ids::stretch].toString());
    settings.formant = (bool) clip.getProperty (ids::formant, false);
    if (getLinkedClips (clip).isEmpty())
        settings.notes = getClipNotes (clip);

    // Compare switch: the clip plays without its pitch changes (transpose and note edits),
    // but keeps its timing, so its length and place don't move.
    if ((bool) clip.getProperty (ids::warpBypass, false))
    {
        settings.semitones = 0.0;
        settings.notes.clear();
    }

    // Time compare: the free warp is replaced by the straight line between the window's two
    // ends, so the clip keeps its place and length (a musical clip keeps its bend).
    if ((bool) clip.getProperty (ids::timeBypass, false))
        settings.map = getTimeBypassMap (clip);

    // Linked: rendered with its partners' files, so they all stay in phase.
    if (! settings.isNeutral())
        if (const auto partners = getLinkedClips (clip); ! partners.isEmpty())
        {
            settings.linked.add (clip[ids::file].toString());
            for (const auto& partner : partners)
                settings.linked.addIfNotAlreadyThere (partner[ids::file].toString());
            settings.linked.sort (false);
        }

    return settings;
}

int Edit::duplicateClip (int clipId)
{
    auto clip = getClip (clipId);
    if (! clip.isValid())
        return -1;

    auto copy = clip.createCopy();
    renumber (copy);
    const auto id = (int) copy[ids::id];
    copy.setProperty (ids::start, (double) clip[ids::start] + (double) clip[ids::length], nullptr);
    clip.getParent().addChild (copy, clip.getParent().indexOf (clip) + 1, undoTarget);
    return id;
}

int Edit::copyClipTo (int clipId, int trackId)
{
    const auto clip = getClip (clipId);
    auto track = getTrack (trackId);
    if (! clip.isValid() || ! track.isValid() || isMidiClip (clip) != isMidiTrack (track))
        return -1;

    auto copy = clip.createCopy();
    renumber (copy);
    copy.removeProperty (ids::take, nullptr); // a comp's piece: the copy isn't on that track's takes
    track.appendChild (copy, undoTarget);
    return (int) copy[ids::id];
}

void Edit::setClipGain (int clipId, float decibels)
{
    getClip (clipId).setProperty (ids::gain, juce::jlimit (-60.0f, 24.0f, decibels), undoTarget);
}

void Edit::setClipRebalance (int clipId, const juce::File& mixed, const juce::File& original, const juce::String& settings)
{
    if (auto clip = getClip (clipId); clip.isValid())
    {
        clip.setProperty (ids::rebalanceFrom, original.getFullPathName(), undoTarget);
        clip.setProperty (ids::rebalance, settings, undoTarget);
        clip.setProperty (ids::file, mixed.getFullPathName(), undoTarget);
    }
}

void Edit::resetClipRebalance (int clipId)
{
    if (auto clip = getClip (clipId); clip.isValid() && clip.hasProperty (ids::rebalanceFrom))
    {
        const auto original = clip[ids::rebalanceFrom].toString();
        clip.setProperty (ids::file, original, undoTarget);
        clip.removeProperty (ids::rebalanceFrom, undoTarget);
        clip.removeProperty (ids::rebalance, undoTarget);
    }
}

juce::File Edit::getRebalanceOriginal (const juce::ValueTree& clip)
{
    return juce::File (clip.hasProperty (ids::rebalanceFrom) ? clip[ids::rebalanceFrom].toString() : clip[ids::file].toString());
}

void Edit::setClipName (int clipId, const juce::String& name)
{
    if (auto clip = getClip (clipId); clip.isValid() && name.trim().isNotEmpty())
        clip.setProperty (ids::name, name.trim().substring (0, 80), undoTarget);
}

void Edit::setClipMute (int clipId, bool mute)
{
    if (auto clip = getClip (clipId); clip.isValid())
        clip.setProperty (ids::mute, mute, undoTarget);
}

juce::Array<int> Edit::keepClipParts (int clipId, const std::vector<std::pair<double, double>>& parts, double fadeSeconds)
{
    juce::Array<int> created;
    auto clip = getClip (clipId);
    if (! clip.isValid() || parts.empty())
        return created;

    const auto start = (double) clip[ids::start], offset = (double) clip[ids::offset], length = (double) clip[ids::length];
    auto parent = clip.getParent();
    auto index = parent.indexOf (clip);

    for (const auto& [from, to] : parts)
    {
        const auto a = juce::jlimit (0.0, length, from), b = juce::jlimit (0.0, length, to);
        if (b - a < minClipSeconds)
            continue;

        auto part = clip.createCopy();
        renumber (part);
        const auto id = (int) part[ids::id];
        part.setProperty (ids::start, start + a, nullptr);
        part.setProperty (ids::offset, offset + a, nullptr);
        part.setProperty (ids::length, b - a, nullptr);
        const auto fade = juce::jmin (fadeSeconds, (b - a) / 2.0);
        const auto half = (b - a) / 2.0;
        part.setProperty (ids::fadeIn, a > 0.0 ? fade : juce::jmin (half, (double) clip[ids::fadeIn]), nullptr);
        part.setProperty (ids::fadeOut, b < length ? fade : juce::jmin (half, (double) clip[ids::fadeOut]), nullptr);
        parent.addChild (part, ++index, undoTarget);
        created.add (id);
    }

    if (! created.isEmpty())
        parent.removeChild (clip, undoTarget);

    return created;
}

void Edit::setClipFades (int clipId, double fadeInSeconds, double fadeOutSeconds)
{
    auto clip = getClip (clipId);
    if (! clip.isValid())
        return;

    // The two fades together never exceed the clip.
    const auto length = (double) clip[ids::length];
    const auto fadeIn = juce::jlimit (0.0, length, fadeInSeconds);
    clip.setProperty (ids::fadeIn, fadeIn, undoTarget);
    clip.setProperty (ids::fadeOut, juce::jlimit (0.0, length - fadeIn, fadeOutSeconds), undoTarget);
}

//==============================================================================
namespace
{
    bool isAudioClip (const juce::ValueTree& clip) { return clip.hasType (ids::CLIP) && ! Edit::isMidiClip (clip); }
    double clipStart (const juce::ValueTree& clip)  { return (double) clip[ids::start]; }
    double clipEnd (const juce::ValueTree& clip)    { return (double) clip[ids::start] + (double) clip[ids::length]; }
    constexpr double timeEpsilon = 1.0e-6;
}

juce::ValueTree Edit::getOwnerTrack (const juce::ValueTree& clipOrTake)
{
    for (auto tree = clipOrTake; tree.isValid(); tree = tree.getParent())
        if (tree.hasType (ids::TRACK))
            return tree;
    return {};
}

juce::Array<juce::ValueTree> Edit::getTakes (int trackId) const
{
    juce::Array<juce::ValueTree> takes;
    for (const auto& child : getTrack (trackId))
        if (child.hasType (ids::TAKE))
            takes.add (child);
    return takes;
}

juce::ValueTree Edit::getTake (int takeId) const
{
    return findById (ids::TAKE, takeId);
}

juce::ValueTree Edit::addTake (int trackId, const juce::String& name)
{
    auto track = getTrack (trackId);
    if (! track.isValid() || isMidiTrack (track))
        return {};

    // "Take N" counts on from the highest number so far (deleting a take doesn't reuse it).
    int number = 0;
    for (const auto& take : getTakes (trackId))
    {
        const auto text = take[ids::name].toString();
        number = juce::jmax (number, text.startsWith ("Take ") ? text.substring (5).getIntValue() : 0);
    }
    number = juce::jmax (number, getTakes (trackId).size()) + 1;

    juce::ValueTree take (ids::TAKE);
    take.setProperty (ids::id, takeNextId(), nullptr);
    take.setProperty (ids::name, name.trim().isNotEmpty() ? name.trim().substring (0, 60) : "Take " + juce::String (number), nullptr);
    track.appendChild (take, undoTarget);
    return take;
}

void Edit::removeTake (int takeId)
{
    auto take = getTake (takeId);
    if (! take.isValid())
        return;

    auto track = take.getParent();
    for (auto clip : track)
        if (clip.hasType (ids::CLIP) && (int) clip.getProperty (ids::take, -1) == takeId)
            clip.removeProperty (ids::take, undoTarget);

    if (getListenTake (track) == takeId)
        track.removeProperty (ids::listenTake, nullptr);

    track.removeChild (take, undoTarget);
}

void Edit::setTakeName (int takeId, const juce::String& name)
{
    if (auto take = getTake (takeId); take.isValid() && name.trim().isNotEmpty())
        take.setProperty (ids::name, name.trim().substring (0, 60), undoTarget);
}

void Edit::setTakesShown (int trackId, bool shown)
{
    if (auto track = getTrack (trackId); track.isValid())
        track.setProperty (ids::takesShown, shown, nullptr);
}

void Edit::setListenTake (int trackId, int takeId)
{
    auto track = getTrack (trackId);
    if (! track.isValid())
        return;

    if (const auto take = getTake (takeId); take.isValid() && take.getParent() == track)
        track.setProperty (ids::listenTake, takeId, nullptr);
    else
        track.removeProperty (ids::listenTake, nullptr);
}

int Edit::getListenTake (const juce::ValueTree& track)
{
    if (! track.hasProperty (ids::listenTake))
        return -1;

    const auto id = (int) track[ids::listenTake];
    for (const auto& child : track)
        if (child.hasType (ids::TAKE) && (int) child[ids::id] == id)
            return id;
    return -1;
}

void Edit::cutComp (juce::ValueTree track, double start, double end)
{
    // The new piece crossfades with what stays: it reaches half a fade past each end,
    // and what's left on either side reaches half a fade in.
    const auto half = compFadeSeconds * 0.5;

    juce::Array<juce::ValueTree> clips;
    for (const auto& child : track)
        if (isAudioClip (child))
            clips.add (child);

    for (auto clip : clips)
    {
        const auto s = clipStart (clip), e = clipEnd (clip);
        if (e <= start + timeEpsilon || s >= end - timeEpsilon)
            continue;

        const auto id = (int) clip[ids::id];
        const auto keepLeft = s < start - half - timeEpsilon;
        const auto keepRight = e > end + half + timeEpsilon;

        if (! keepLeft && ! keepRight)
        {
            removeClip (id);
            continue;
        }

        if (keepRight)
        {
            const auto rightStart = juce::jmax (s, end - half);
            auto right = clip;
            if (keepLeft)
                right = getClip (splitClip (id, rightStart));
            else
                trimClipStart (id, rightStart);

            if (right.isValid())
                right.setProperty (ids::fadeIn, compFadeSeconds, undoTarget);
        }

        if (keepLeft)
        {
            trimClipEnd (id, juce::jmin (clipEnd (clip), start + half));
            clip.setProperty (ids::fadeOut, compFadeSeconds, undoTarget);
        }
    }
}

void Edit::compTake (int takeId, double start, double end)
{
    const auto take = getTake (takeId);
    if (! take.isValid() || end - start < minClipSeconds)
        return;

    auto track = take.getParent();
    const auto half = compFadeSeconds * 0.5;
    start = juce::jmax (0.0, start);

    cutComp (track, start, end);

    for (const auto& source : take)
    {
        if (! isAudioClip (source))
            continue;

        const auto s = clipStart (source), e = clipEnd (source);
        const auto a = juce::jmax (s, start - half), b = juce::jmin (e, end + half);
        if (b - a < minClipSeconds)
            continue;

        auto piece = source.createCopy();
        renumber (piece);
        piece.setProperty (ids::start, a, nullptr);
        piece.setProperty (ids::offset, (double) source[ids::offset] + (a - s), nullptr);
        piece.setProperty (ids::length, b - a, nullptr);
        piece.setProperty (ids::fadeIn, a > s + timeEpsilon ? compFadeSeconds : (double) source.getProperty (ids::fadeIn, 0.0), nullptr);
        piece.setProperty (ids::fadeOut, b < e - timeEpsilon ? compFadeSeconds : (double) source.getProperty (ids::fadeOut, 0.0), nullptr);
        piece.setProperty (ids::take, takeId, nullptr);
        track.appendChild (piece, undoTarget);
    }
}

std::pair<double, double> Edit::compTakeAt (int takeId, double time)
{
    const auto take = getTake (takeId);
    if (! take.isValid())
        return {};

    const auto track = take.getParent();
    const auto half = compFadeSeconds * 0.5;

    // The comp piece playing at `time` (in a crossfade, the one that started last).
    juce::ValueTree piece;
    for (const auto& clip : track)
        if (isAudioClip (clip) && clipStart (clip) <= time && clipEnd (clip) > time
              && (! piece.isValid() || clipStart (clip) > clipStart (piece)))
            piece = clip;

    double start = 0.0, end = 0.0;

    if (piece.isValid())
    {
        // Its stretch between the crossfades' middles.
        const auto isJoint = [] (const juce::var& fade) { return std::abs ((double) fade - compFadeSeconds) < 1.0e-4; };
        start = clipStart (piece) + (isJoint (piece.getProperty (ids::fadeIn, 0.0)) ? half : 0.0);
        end = clipEnd (piece) - (isJoint (piece.getProperty (ids::fadeOut, 0.0)) ? half : 0.0);
    }
    else
    {
        // A gap in the comp: the take's clip there, up to the comp's pieces either side.
        juce::ValueTree source;
        for (const auto& clip : take)
            if (isAudioClip (clip) && clipStart (clip) <= time && clipEnd (clip) > time)
                source = clip;
        if (! source.isValid())
            return {};

        start = clipStart (source);
        end = clipEnd (source);
        for (const auto& clip : track)
        {
            if (! isAudioClip (clip))
                continue;
            if (clipEnd (clip) <= time)
                start = juce::jmax (start, clipEnd (clip));
            else if (clipStart (clip) >= time)
                end = juce::jmin (end, clipStart (clip));
        }
    }

    if (end - start < minClipSeconds)
        return {};

    compTake (takeId, start, end);
    return { start, end };
}

juce::ValueTree Edit::addTakeFromClip (int clipId)
{
    auto clip = getClip (clipId);
    auto track = clip.getParent();
    if (! isAudioClip (clip) || ! track.hasType (ids::TRACK))
        return {};

    auto take = addTake ((int) track[ids::id]);
    auto copy = clip.createCopy();
    renumber (copy);
    copy.removeProperty (ids::take, nullptr);
    take.appendChild (copy, undoTarget);
    clip.setProperty (ids::take, (int) take[ids::id], undoTarget);
    return take;
}

void Edit::addRecording (int trackId, const juce::File& file, const std::vector<RecordedPass>& passes)
{
    auto track = getTrack (trackId);
    if (! track.isValid() || isMidiTrack (track) || passes.empty())
        return;

    auto low = passes.front().start, high = low;
    for (const auto& pass : passes)
    {
        low = juce::jmin (low, pass.start);
        high = juce::jmax (high, pass.start + pass.length);
    }

    // Everything the track plays now (the whole first take, not only what the recording overlaps).
    juce::Array<juce::ValueTree> covered;
    for (const auto& clip : track)
        if (isAudioClip (clip) && ! clip.hasProperty (ids::take))
            covered.add (clip);
    const auto overlapsRecording = [&] (const juce::ValueTree& clip)
    {
        return juce::jmin (clipEnd (clip), high) - juce::jmax (clipStart (clip), low) > minClipSeconds;
    };
    const auto overlapping = std::any_of (covered.begin(), covered.end(), overlapsRecording);

    auto takes = getTakes (trackId);

    if (passes.size() == 1 && takes.isEmpty() && covered.isEmpty())
    {
        addClip (trackId, file, passes.front().start, passes.front().length, passes.front().offset);
        return;
    }

    // The first time, what was there becomes the first take, so it can still be picked.
    if (takes.isEmpty() && ! covered.isEmpty() && (overlapping || passes.size() > 1))
    {
        auto first = addTake (trackId);
        for (auto clip : covered)
        {
            auto copy = clip.createCopy();
            renumber (copy);
            copy.removeProperty (ids::take, nullptr);
            first.appendChild (copy, undoTarget);
            clip.setProperty (ids::take, (int) first[ids::id], undoTarget);
        }
    }

    // Every pass is a take, and the comp plays them in order: the newest wins.
    for (const auto& pass : passes)
    {
        auto take = addTake (trackId);
        auto clip = addClip (trackId, file, pass.start, pass.length, pass.offset);
        clip.setProperty (ids::name, take[ids::name], undoTarget);
        track.removeChild (clip, undoTarget);
        take.appendChild (clip, undoTarget);
        compTake ((int) take[ids::id], pass.start, pass.start + pass.length);
    }

    setTakesShown (trackId, true);
}

double Edit::snapToGrid (double seconds, double beats) const
{
    return getTempoMap().snap (seconds, beats);
}

void Edit::setBeatsPerBar (int beats)
{
    state.setProperty (ids::beatsPerBar, juce::jlimit (2, 12, beats), undoTarget);
}

void Edit::setKey (int tonic, bool minor)
{
    if (tonic < 0 || tonic > 11)
    {
        state.removeProperty (ids::key, undoTarget);
        state.removeProperty (ids::keyMinor, undoTarget);
        return;
    }

    state.setProperty (ids::key, tonic, undoTarget);
    state.setProperty (ids::keyMinor, minor, undoTarget);
}

int Edit::getKey() const
{
    if (! state.hasProperty (ids::key))
        return -1;
    const auto tonic = (int) state.getProperty (ids::key);
    return tonic >= 0 && tonic < 12 ? tonic : -1;
}

//==============================================================================
juce::ValueTree Edit::addMarker (double timeSeconds, const juce::String& name)
{
    juce::ValueTree marker (ids::MARKER);
    marker.setProperty (ids::id, takeNextId(), nullptr);
    marker.setProperty (ids::name, name.isNotEmpty() ? name : "Marker " + juce::String (getMarkers().size() + 1), nullptr);
    marker.setProperty (ids::time, juce::jmax (0.0, timeSeconds), nullptr);
    state.appendChild (marker, undoTarget);
    return marker;
}

void Edit::removeMarker (int markerId)
{
    if (auto marker = findById (ids::MARKER, markerId); marker.isValid())
        state.removeChild (marker, undoTarget);
}

void Edit::setMarkerTime (int markerId, double timeSeconds)
{
    if (auto marker = findById (ids::MARKER, markerId); marker.isValid())
        marker.setProperty (ids::time, juce::jmax (0.0, timeSeconds), undoTarget);
}

void Edit::setMarkerName (int markerId, const juce::String& name)
{
    if (auto marker = findById (ids::MARKER, markerId); marker.isValid() && name.trim().isNotEmpty())
        marker.setProperty (ids::name, name.trim().substring (0, 60), undoTarget);
}

juce::Array<juce::ValueTree> Edit::getMarkers() const
{
    juce::Array<juce::ValueTree> markers;
    for (const auto& child : state)
        if (child.hasType (ids::MARKER))
            markers.add (child);

    std::stable_sort (markers.begin(), markers.end(), [] (const auto& a, const auto& b)
                      { return (double) a[ids::time] < (double) b[ids::time]; });
    return markers;
}

void Edit::setSongSections (const std::vector<SectionMarker>& sections)
{
    for (const auto& marker : getSongSections())
        state.removeChild (marker, undoTarget);

    for (const auto& s : sections)
    {
        if (s.length <= 0.0)
            continue;
        juce::ValueTree marker (ids::MARKER);
        marker.setProperty (ids::id, takeNextId(), nullptr);
        marker.setProperty (ids::name, s.name.isNotEmpty() ? s.name.substring (0, 60) : juce::String ("Section"), nullptr);
        marker.setProperty (ids::time, juce::jmax (0.0, s.start), nullptr);
        marker.setProperty (ids::length, s.length, nullptr);
        marker.setProperty (ids::section, s.kind.isNotEmpty() ? s.kind : juce::String ("part"), nullptr);
        marker.setProperty (ids::letter, s.letter, nullptr);
        if (s.colour.isNotEmpty())
            marker.setProperty (ids::colour, s.colour, nullptr);
        state.appendChild (marker, undoTarget);
    }
}

juce::Array<juce::ValueTree> Edit::getSongSections() const
{
    juce::Array<juce::ValueTree> sections;
    for (const auto& marker : getMarkers())
        if (isSection (marker))
            sections.add (marker);
    return sections;
}

void Edit::setLoop (bool on, double startSeconds, double endSeconds)
{
    const auto start = juce::jmax (0.0, juce::jmin (startSeconds, endSeconds));
    const auto end = juce::jmax (startSeconds, endSeconds);
    state.setProperty (ids::loopStart, start, undoTarget);
    state.setProperty (ids::loopEnd, end, undoTarget);
    state.setProperty (ids::loopOn, on && end - start >= minClipSeconds, undoTarget);
}

//==============================================================================
juce::ValueTree Edit::getInserts (const juce::ValueTree& trackOrMaster)
{
    return trackOrMaster.getChildWithName (ids::INSERTS);
}

std::optional<fx::ModuleType> Edit::moduleTypeFromKey (const juce::String& key)
{
    for (int i = 0; i < fx::numModuleTypes; ++i)
        if (key == fx::getModuleKey ((fx::ModuleType) i))
            return (fx::ModuleType) i;

    return std::nullopt;
}

juce::ValueTree Edit::addInsert (int trackId, fx::ModuleType type, int index)
{
    auto inserts = getInserts (getTrackOrMaster (trackId));
    if (! inserts.isValid())
        return {};

    auto insert = createInsertTree (type);
    inserts.addChild (insert, index, undoTarget);
    return insert;
}

juce::ValueTree Edit::createInsertTree (fx::ModuleType type)
{
    juce::ValueTree insert (ids::INSERT);
    insert.setProperty (ids::id, takeNextId(), nullptr);
    insert.setProperty (ids::type, fx::getModuleKey (type), nullptr);

    // Start from the module's defaults, switched on (the first switch for the pitch
    // module, which is autotune).
    auto module = fx::createModule (type);
    bool switchedOn = false;

    for (int v = 0; v < module->getNumValues(); ++v)
    {
        auto value = module->getValue (v).load();

        if (module->isSwitch (v) && ! switchedOn)
        {
            value = 1.0f;
            switchedOn = true;
        }

        insert.setProperty (juce::Identifier (module->getValueId (v)), value, nullptr);
    }

    return insert;
}

//==============================================================================
juce::ValueTree Edit::getChannelOf (const juce::ValueTree& insert)
{
    for (auto tree = insert.getParent(); tree.isValid(); tree = tree.getParent())
        if (tree.hasType (ids::TRACK) || tree.hasType (ids::RETURN) || tree.hasType (ids::MASTER))
            return tree;
    return {};
}

namespace
{
    void setRackChainValues (juce::ValueTree rack, int chain, juce::UndoManager* undo)
    {
        rack.setProperty (juce::Identifier (RackInsert::gainId (chain)), 0.0f, undo);
        rack.setProperty (juce::Identifier (RackInsert::panId (chain)), 0.0f, undo);
        rack.setProperty (juce::Identifier (RackInsert::onIdOf (chain)), 1.0f, undo);
        rack.setProperty (juce::Identifier (RackInsert::soloId (chain)), 0.0f, undo);
    }
}

juce::ValueTree Edit::groupInserts (const juce::Array<int>& insertIds)
{
    juce::Array<juce::ValueTree> members;
    for (const auto insertId : insertIds)
        if (const auto insert = getInsert (insertId); insert.isValid() && ! members.contains (insert))
            members.add (insert);
    if (members.isEmpty())
        return {};

    // In the first one's chain, at its place; the others join from wherever they are on
    // the same channel (inserts from another channel stay where they are).
    auto parent = members.getFirst().getParent();
    const auto channel = getChannelOf (members.getFirst());
    const auto at = parent.indexOf (members.getFirst());

    juce::ValueTree rack (ids::INSERT);
    rack.setProperty (ids::id, takeNextId(), nullptr);
    rack.setProperty (ids::type, fx::getModuleKey (fx::ModuleType::rack), nullptr);
    rack.setProperty (juce::Identifier (RackInsert::onId), 1.0f, nullptr);
    rack.setProperty ("rackInGain", 0.0f, nullptr);
    rack.setProperty ("rackOutGain", 0.0f, nullptr);
    juce::ValueTree chain (ids::CHAIN);
    chain.setProperty (ids::name, "Chain 1", nullptr);
    chain.appendChild (juce::ValueTree (ids::INSERTS), nullptr);
    rack.appendChild (chain, nullptr);
    setRackChainValues (rack, 0, nullptr);

    parent.addChild (rack, at, undoTarget);
    auto chainInserts = rack.getChild (0).getChild (0);
    for (auto insert : members)
        if (getChannelOf (insert) == channel && insert != rack)
        {
            insert.getParent().removeChild (insert, undoTarget);
            chainInserts.addChild (insert, -1, undoTarget);
        }
    return rack;
}

void Edit::ungroupInsert (int rackId)
{
    auto rack = getInsert (rackId);
    if (! rack.isValid() || ! isRackInsert (rack))
        return;
    auto parent = rack.getParent();
    auto at = parent.indexOf (rack);
    // Macros mapped to the rack's own values go; its effects keep theirs.
    if (auto macros = getMacros (getChannelOf (rack)); macros.isValid())
        for (auto macro : macros)
            for (int i = macro.getNumChildren(); --i >= 0;)
                if ((int) macro.getChild (i)[ids::insert] == rackId)
                    macro.removeChild (i, undoTarget);
    for (int c = 0; c < rack.getNumChildren(); ++c)
    {
        auto inserts = rack.getChild (c).getChildWithName (ids::INSERTS);
        while (inserts.getNumChildren() > 0)
        {
            auto insert = inserts.getChild (0);
            inserts.removeChild (0, undoTarget);
            parent.addChild (insert, at++, undoTarget);
        }
    }
    parent.removeChild (rack, undoTarget);
}

int Edit::addRackChain (int rackId, const juce::String& name)
{
    auto rack = getInsert (rackId);
    if (! rack.isValid() || ! isRackInsert (rack) || rack.getNumChildren() >= RackInsert::maxChains)
        return -1;
    const auto index = rack.getNumChildren();
    juce::ValueTree chain (ids::CHAIN);
    chain.setProperty (ids::name, name.isNotEmpty() ? name : "Chain " + juce::String (index + 1), nullptr);
    chain.appendChild (juce::ValueTree (ids::INSERTS), nullptr);
    setRackChainValues (rack, index, undoTarget);
    rack.appendChild (chain, undoTarget);
    return index;
}

void Edit::removeRackChain (int rackId, int chainIndex)
{
    auto rack = getInsert (rackId);
    if (! rack.isValid() || ! isRackInsert (rack) || ! juce::isPositiveAndBelow (chainIndex, rack.getNumChildren()))
        return;
    for (const auto& insert : rack.getChild (chainIndex).getChildWithName (ids::INSERTS))
        unmapInsert (insert); // its effects' macro mappings go with them

    // The values of the chains after it move down one.
    const auto count = rack.getNumChildren();
    for (int c = chainIndex; c < count - 1; ++c)
        for (const auto& id : { RackInsert::gainId, RackInsert::panId, RackInsert::onIdOf, RackInsert::soloId })
            rack.setProperty (juce::Identifier (id (c)), rack[juce::Identifier (id (c + 1))], undoTarget);
    for (const auto& id : { RackInsert::gainId, RackInsert::panId, RackInsert::onIdOf, RackInsert::soloId })
        rack.removeProperty (juce::Identifier (id (count - 1)), undoTarget);
    rack.removeChild (chainIndex, undoTarget);
}

void Edit::renameRackChain (int rackId, int chainIndex, const juce::String& name)
{
    auto inserts = getRackChainInserts (rackId, chainIndex);
    if (inserts.isValid())
        inserts.getParent().setProperty (ids::name, name.trim().substring (0, 40), undoTarget);
}

juce::ValueTree Edit::getRackChainInserts (int rackId, int chainIndex) const
{
    const auto rack = getInsert (rackId);
    if (! rack.isValid() || ! isRackInsert (rack) || ! juce::isPositiveAndBelow (chainIndex, rack.getNumChildren()))
        return {};
    return rack.getChild (chainIndex).getChildWithName (ids::INSERTS);
}

bool Edit::moveInsertToRack (int insertId, int rackId, int chainIndex, int index)
{
    auto insert = getInsert (insertId);
    auto target = getRackChainInserts (rackId, chainIndex);
    if (! insert.isValid() || ! target.isValid() || insertId == rackId)
        return false;
    for (auto tree = target; tree.isValid(); tree = tree.getParent())
        if (tree == insert)
            return false; // a rack into itself
    if (getChannelOf (insert) != getChannelOf (target))
        unmapInsert (insert);

    auto source = insert.getParent();
    if (source == target)
    {
        const auto last = source.getNumChildren() - 1;
        source.moveChild (source.indexOf (insert), index < 0 ? last : juce::jlimit (0, last, index), undoTarget);
        return true;
    }
    source.removeChild (insert, undoTarget);
    target.addChild (insert, index < 0 ? -1 : juce::jmin (index, target.getNumChildren()), undoTarget);
    return true;
}

juce::ValueTree Edit::addRackInsert (int rackId, int chainIndex, fx::ModuleType type, int index)
{
    auto inserts = getRackChainInserts (rackId, chainIndex);
    if (! inserts.isValid())
        return {};
    auto insert = createInsertTree (type);
    inserts.addChild (insert, index, undoTarget);
    return insert;
}

juce::ValueTree Edit::addInsert (int trackId, fx::ModuleType type, int index, const std::map<juce::String, float>& values)
{
    auto insert = addInsert (trackId, type, index);
    if (! insert.isValid())
        return {};

    // Straight onto the new insert: still one undo step (its adding).
    for (const auto& [valueId, value] : values)
        if (const juce::Identifier name (valueId); insert.hasProperty (name) && name != ids::id && name != ids::type && std::isfinite (value))
            insert.setProperty (name, value, nullptr);

    return insert;
}

juce::StringArray Edit::getInsertValueIds (const juce::ValueTree& insert)
{
    juce::StringArray result;

    if (isPluginInsert (insert))
    {
        for (const auto* id : { "pluginOn", "pluginInGain", "pluginOutGain" })
            if (insert.hasProperty (id))
                result.add (id);
        for (const auto index : PluginInsert::getPanel (insert)) // its panel's parameters
            if (insert.hasProperty (juce::Identifier (PluginInsert::panelValueId (index))))
                result.add (PluginInsert::panelValueId (index));
        return result;
    }

    if (isRackInsert (insert))
    {
        juce::StringArray rackIds { RackInsert::onId, "rackInGain", "rackOutGain" };
        for (int c = 0; c < insert.getNumChildren(); ++c)
            rackIds.addArray ({ RackInsert::gainId (c), RackInsert::panId (c), RackInsert::onIdOf (c), RackInsert::soloId (c) });
        for (const auto& id : rackIds)
            if (insert.hasProperty (juce::Identifier (id)))
                result.add (id);
        return result;
    }

    if (const auto type = moduleTypeFromKey (insert[ids::type].toString()))
    {
        const auto module = fx::createModule (*type);
        for (int v = 0; v < module->getNumValues(); ++v)
            if (insert.hasProperty (juce::Identifier (module->getValueId (v))))
                result.add (module->getValueId (v));
    }

    return result;
}

juce::ValueTree Edit::copyInsertTo (int insertId, int channelId, int index)
{
    const auto insert = getInsert (insertId);
    auto inserts = getInserts (getChannel (channelId));
    if (! insert.isValid() || ! inserts.isValid())
        return {};

    auto copy = insert.createCopy();
    copy.setProperty (ids::id, takeNextId(), nullptr);
    for (auto child : copy) renumber (child); // a rack's effects too
    inserts.addChild (copy, index, undoTarget);
    return copy;
}

juce::ValueTree Edit::duplicateInsert (int insertId)
{
    const auto insert = getInsert (insertId);
    if (! insert.isValid())
        return {};

    auto inserts = insert.getParent();
    auto copy = insert.createCopy();
    copy.setProperty (ids::id, takeNextId(), nullptr);
    for (auto child : copy) renumber (child); // a rack's effects too
    inserts.addChild (copy, inserts.indexOf (insert) + 1, undoTarget);
    return copy;
}

bool Edit::moveInsertTo (int insertId, int channelId, int index)
{
    auto insert = getInsert (insertId);
    auto target = getInserts (getChannel (channelId));
    if (! insert.isValid() || ! target.isValid())
        return false;

    auto source = insert.getParent();
    if (source == target)
    {
        const auto last = source.getNumChildren() - 1;
        source.moveChild (source.indexOf (insert), index < 0 ? last : juce::jlimit (0, last, index), undoTarget);
        return true;
    }

    if (getChannelOf (insert) != getChannelOf (target))
        unmapInsert (insert); // macros turn values of their own channel only
    source.removeChild (insert, undoTarget);
    target.addChild (insert, index < 0 ? -1 : juce::jmin (index, target.getNumChildren()), undoTarget);
    return true;
}

juce::Array<juce::ValueTree> Edit::pasteInserts (const juce::Array<int>& insertIds, int channelId, int index)
{
    // Copy them all first, so pasting a chain into itself copies what was there.
    juce::Array<juce::ValueTree> sources;
    for (const auto insertId : insertIds)
        if (const auto insert = getInsert (insertId); insert.isValid())
            sources.add (insert);
    return pasteInsertTrees (sources, channelId, index);
}

juce::Array<juce::ValueTree> Edit::pasteInsertTrees (const juce::Array<juce::ValueTree>& trees, int channelId, int index)
{
    return pasteInsertTreesInto (trees, getInserts (getChannel (channelId)), index);
}

juce::Array<juce::ValueTree> Edit::pasteInsertTreesInto (const juce::Array<juce::ValueTree>& trees, juce::ValueTree inserts, int index)
{
    juce::Array<juce::ValueTree> copies;
    if (! inserts.isValid() || ! inserts.hasType (ids::INSERTS))
        return copies;

    juce::Array<juce::ValueTree> sources;
    for (const auto& tree : trees)
        if (tree.hasType (ids::INSERT))
            sources.add (tree.createCopy());

    auto at = index < 0 ? inserts.getNumChildren() : juce::jmin (index, inserts.getNumChildren());
    for (auto copy : sources)
    {
        copy.setProperty (ids::id, takeNextId(), nullptr);
        for (auto child : copy) renumber (child); // a rack's effects too
        inserts.addChild (copy, at++, undoTarget);
        copies.add (copy);
    }

    return copies;
}

juce::ValueTree Edit::replaceInsert (int insertId, fx::ModuleType type)
{
    const auto insert = getInsert (insertId);
    if (! insert.isValid())
        return {};

    auto inserts = insert.getParent();
    const auto index = inserts.indexOf (insert);
    const auto ownerId = (int) inserts.getParent()[ids::id];
    inserts.removeChild (insert, undoTarget);
    return addInsert (ownerId, type, index);
}

juce::ValueTree Edit::replaceInsertWithPlugin (int insertId, const InstrumentInfo& info)
{
    const auto insert = getInsert (insertId);
    if (! insert.isValid() || info.id.isEmpty())
        return {};

    auto inserts = insert.getParent();
    const auto index = inserts.indexOf (insert);
    const auto ownerId = (int) inserts.getParent()[ids::id];
    inserts.removeChild (insert, undoTarget);
    return addPluginInsert (ownerId, info, index);
}

void Edit::setInsertName (int insertId, const juce::String& name)
{
    auto insert = getInsert (insertId);
    if (! insert.isValid())
        return;

    const auto trimmed = name.trim().substring (0, 60);
    if (trimmed.isEmpty())
        insert.removeProperty (ids::label, undoTarget);
    else
        insert.setProperty (ids::label, trimmed, undoTarget);
}

// A value set as text ("id=value;id=value"), for the set that isn't playing.
static juce::String writeValueSet (const juce::ValueTree& insert, const juce::StringArray& valueIds)
{
    juce::StringArray parts;
    for (const auto& valueId : valueIds)
        parts.add (valueId + "=" + juce::String ((float) insert[juce::Identifier (valueId)]));
    return parts.joinIntoString (";");
}

void Edit::switchInsertAB (int insertId)
{
    auto insert = getInsert (insertId);
    if (! insert.isValid() || isPluginInsert (insert))
        return;

    const auto valueIds = getInsertValueIds (insert);
    const auto playing = writeValueSet (insert, valueIds);
    const auto other = hasAB (insert) ? insert[ids::abOther].toString() : playing;

    for (const auto& part : juce::StringArray::fromTokens (other, ";", ""))
    {
        const auto valueId = part.upToFirstOccurrenceOf ("=", false, false);
        if (valueIds.contains (valueId) && part.containsChar ('='))
            insert.setProperty (juce::Identifier (valueId), part.fromFirstOccurrenceOf ("=", false, false).getFloatValue(), undoTarget);
    }

    insert.setProperty (ids::abOther, playing, undoTarget);
    insert.setProperty (ids::ab, isShowingB (insert) ? 0 : 1, undoTarget);
}

void Edit::copyInsertAB (int insertId)
{
    auto insert = getInsert (insertId);
    if (insert.isValid() && ! isPluginInsert (insert))
        insert.setProperty (ids::abOther, writeValueSet (insert, getInsertValueIds (insert)), undoTarget);
}

juce::ValueTree Edit::addPluginInsert (int trackId, const InstrumentInfo& info, int index)
{
    auto inserts = getInserts (getTrackOrMaster (trackId));
    if (! inserts.isValid() || info.id.isEmpty())
        return {};

    // Like an INSTRUMENT, plus the switch and gains every insert has.
    juce::ValueTree tree (ids::INSERT);
    tree.copyPropertiesFrom (makeInstrument (info, takeNextId()), nullptr);
    tree.setProperty (ids::type, fx::getModuleKey (fx::ModuleType::plugin), nullptr);
    tree.setProperty ("pluginOn", 1.0f, nullptr);
    tree.setProperty ("pluginInGain", 0.0f, nullptr);
    tree.setProperty ("pluginOutGain", 0.0f, nullptr);

    inserts.addChild (tree, index, undoTarget);
    return tree;
}

void Edit::removeInsert (int insertId)
{
    if (auto insert = getInsert (insertId); insert.isValid())
    {
        unmapInsert (insert); // its macro mappings go with it
        insert.getParent().removeChild (insert, undoTarget);
    }
}

//==============================================================================
namespace
{
    juce::ValueTree findMacro (juce::ValueTree channel, int index, juce::UndoManager* undo, bool create)
    {
        if (! channel.isValid() || index < 0 || index >= Edit::numMacros)
            return {};
        auto macros = Edit::getMacros (channel);
        if (! macros.isValid())
        {
            if (! create)
                return {};
            macros = juce::ValueTree (ids::MACROS);
            channel.appendChild (macros, undo);
        }
        for (auto macro : macros)
            if ((int) macro[ids::index] == index)
                return macro;
        if (! create)
            return {};
        juce::ValueTree macro (ids::MACRO);
        macro.setProperty (ids::index, index, nullptr);
        macro.setProperty (ids::value, 0.0f, nullptr);
        macros.appendChild (macro, undo);
        return macro;
    }
}

float Edit::macroTarget (const juce::ValueTree& map, float macroValue)
{
    const auto lo = (float) map[ids::min], hi = (float) map[ids::max];
    return lo + (hi - lo) * juce::jlimit (0.0f, 1.0f, macroValue);
}

void Edit::setMacro (int channelId, int index, float value)
{
    auto macro = findMacro (getChannel (channelId), index, undoTarget, true);
    if (! macro.isValid())
        return;
    value = juce::jlimit (0.0f, 1.0f, value);
    macro.setProperty (ids::value, value, undoTarget);
    for (const auto& map : macro)
        setInsertValue ((int) map[ids::insert], map[ids::param].toString(), macroTarget (map, value));
}

bool Edit::mapMacro (int channelId, int index, int insertId, const juce::String& param, float min, float max)
{
    const auto channel = getChannel (channelId);
    const auto insert = getInsert (insertId);
    if (! channel.isValid() || ! insert.isValid() || ! insert.hasProperty (juce::Identifier (param))
        || getChannelOf (insert) != channel || index < 0 || index >= numMacros)
        return false;

    // A value follows one macro: off the others first.
    if (auto macros = getMacros (channel); macros.isValid())
        for (auto other : macros)
            for (int i = other.getNumChildren(); --i >= 0;)
                if ((int) other.getChild (i)[ids::insert] == insertId && other.getChild (i)[ids::param].toString() == param)
                    other.removeChild (i, undoTarget);

    auto macro = findMacro (channel, index, undoTarget, true);
    juce::ValueTree map (ids::MAP);
    map.setProperty (ids::insert, insertId, nullptr);
    map.setProperty (ids::param, param, nullptr);
    map.setProperty (ids::min, min, nullptr);
    map.setProperty (ids::max, max, nullptr);
    macro.appendChild (map, undoTarget);
    setInsertValue (insertId, param, macroTarget (map, (float) macro[ids::value]));
    return true;
}

void Edit::unmapInsert (const juce::ValueTree& insert)
{
    const auto insertId = (int) insert[ids::id];
    if (auto macros = getMacros (getChannelOf (insert)); macros.isValid())
        for (auto macro : macros)
            for (int i = macro.getNumChildren(); --i >= 0;)
                if ((int) macro.getChild (i)[ids::insert] == insertId)
                    macro.removeChild (i, undoTarget);
    for (const auto& chain : insert) // a rack's effects too
        for (const auto& member : chain.getChildWithName (ids::INSERTS))
            unmapInsert (member);
}

juce::ValueTree Edit::getChainMacros (int channelId) const
{
    const auto channel = getChannel (channelId);
    const auto inserts = getInserts (channel);
    const auto macros = getMacros (channel);
    if (! macros.isValid() || ! inserts.isValid())
        return {};

    juce::ValueTree result (ids::MACROS);
    for (const auto& macro : macros)
    {
        auto copy = macro.createCopy();
        for (int i = copy.getNumChildren(); --i >= 0;)
        {
            auto map = copy.getChild (i);
            const auto slot = inserts.indexOf (getInsert ((int) map[ids::insert]));
            if (slot < 0)
                copy.removeChild (i, nullptr);
            else
            {
                map.removeProperty (ids::insert, nullptr);
                map.setProperty (ids::slot, slot, nullptr);
            }
        }
        if (copy.getNumChildren() > 0 || copy[ids::name].toString().isNotEmpty())
            result.appendChild (copy, nullptr);
    }
    return result.getNumChildren() > 0 ? result : juce::ValueTree();
}

void Edit::setChainMacros (int channelId, const juce::ValueTree& macros, const juce::Array<juce::ValueTree>& inserts)
{
    auto channel = getChannel (channelId);
    if (! channel.isValid())
        return;
    if (auto old = getMacros (channel); old.isValid())
        channel.removeChild (old, undoTarget);
    if (! macros.hasType (ids::MACROS))
        return;

    for (const auto& macro : macros)
    {
        const auto index = (int) macro[ids::index];
        if (index < 0 || index >= numMacros)
            continue;
        if (macro[ids::name].toString().isNotEmpty())
            nameMacro (channelId, index, macro[ids::name].toString());
        setMacro (channelId, index, (float) macro[ids::value]);
        for (const auto& map : macro)
            if (const auto slot = (int) map[ids::slot]; juce::isPositiveAndBelow (slot, inserts.size()))
                mapMacro (channelId, index, (int) inserts[slot][ids::id], map[ids::param].toString(), (float) map[ids::min], (float) map[ids::max]);
    }
}

void Edit::unmapMacro (int channelId, int index, int insertId, const juce::String& param)
{
    if (auto macro = findMacro (getChannel (channelId), index, undoTarget, false); macro.isValid())
        for (int i = macro.getNumChildren(); --i >= 0;)
            if ((int) macro.getChild (i)[ids::insert] == insertId && macro.getChild (i)[ids::param].toString() == param)
                macro.removeChild (i, undoTarget);
}

bool Edit::addPluginPanel (int insertId, int parameterIndex, float value)
{
    auto insert = getInsert (insertId);
    if (! isPluginInsert (insert) || parameterIndex < 0)
        return false;
    auto panel = PluginInsert::getPanel (insert);
    if (panel.contains (parameterIndex) || panel.size() >= maxPanelParameters)
        return false;
    panel.add (parameterIndex);

    juce::StringArray text;
    for (const auto index : panel)
        text.add (juce::String (index));
    insert.setProperty (juce::Identifier (PluginInsert::panelValueId (parameterIndex)), juce::jlimit (0.0f, 1.0f, value), undoTarget);
    insert.setProperty ("panel", text.joinIntoString (","), undoTarget);
    return true;
}

bool Edit::removePluginPanel (int insertId, int parameterIndex)
{
    auto insert = getInsert (insertId);
    auto panel = PluginInsert::getPanel (insert);
    if (! isPluginInsert (insert) || ! panel.contains (parameterIndex))
        return false;
    panel.removeFirstMatchingValue (parameterIndex);

    const auto valueId = PluginInsert::panelValueId (parameterIndex);
    if (auto macros = getMacros (getChannelOf (insert)); macros.isValid())
        for (auto macro : macros)
            for (int i = macro.getNumChildren(); --i >= 0;)
                if ((int) macro.getChild (i)[ids::insert] == insertId && macro.getChild (i)[ids::param].toString() == valueId)
                    macro.removeChild (i, undoTarget);
    setInsertAutomation (insertId, valueId, {});
    insert.removeProperty (juce::Identifier (valueId), undoTarget);

    juce::StringArray text;
    for (const auto index : panel)
        text.add (juce::String (index));
    if (text.isEmpty())
        insert.removeProperty ("panel", undoTarget);
    else
        insert.setProperty ("panel", text.joinIntoString (","), undoTarget);
    return true;
}

void Edit::nameMacro (int channelId, int index, const juce::String& name)
{
    if (auto macro = findMacro (getChannel (channelId), index, undoTarget, true); macro.isValid())
        macro.setProperty (ids::name, name.trim().substring (0, 30), undoTarget);
}

void Edit::moveInsert (int insertId, int newIndex)
{
    auto insert = getInsert (insertId);
    if (! insert.isValid())
        return;

    auto inserts = insert.getParent();
    inserts.moveChild (inserts.indexOf (insert), juce::jlimit (0, inserts.getNumChildren() - 1, newIndex), undoTarget);
}

void Edit::setInsertValue (int insertId, const juce::String& valueId, float value)
{
    if (auto insert = getInsert (insertId); insert.isValid() && insert.hasProperty (juce::Identifier (valueId)))
        insert.setProperty (juce::Identifier (valueId), value, undoTarget);
}

void Edit::setInsertFile (int insertId, const juce::File& file)
{
    if (auto insert = getInsert (insertId); insert.isValid())
    {
        if (file == juce::File())
            insert.removeProperty (ids::file, undoTarget);
        else
            insert.setProperty (ids::file, file.getFullPathName(), undoTarget);
    }
}

juce::ValueTree Edit::getInsert (int insertId) const
{
    return findById (ids::INSERT, insertId);
}
}
