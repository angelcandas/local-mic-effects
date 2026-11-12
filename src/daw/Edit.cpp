#include "Edit.h"

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
    if (auto track = getTrack (trackId); track.isValid())
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
        for (const auto* wet : { "reverbMix", "chorusMix", "flangerMix", "phaserMix", "delayMix", "doublerMix" })
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
    }

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

void Edit::setSend (int trackId, int returnId, float decibels)
{
    auto track = getTrack (trackId);
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
    auto track = getTrack (trackId);
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

void Edit::setTrackOutput (int trackId, int returnIdOrMaster)
{
    auto track = getTrack (trackId);
    if (! track.isValid())
        return;

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
void Edit::setTrackMonitor (int trackId, bool monitor)  { getTrack (trackId).setProperty (ids::monitor, monitor, nullptr); }
void Edit::setTempo (double bpm)                        { state.setProperty (ids::tempo, juce::jlimit (20.0, 400.0, bpm), undoTarget); }

TempoMap Edit::getTempoMap() const
{
    return TempoMap::fromProject (getTempo(), state[ids::tempoChanges].toString());
}

void Edit::setTempoMap (const TempoMap& map)
{
    setTempo (map.getPoints().front().bpm);

    if (const auto changes = map.changesToString(); changes.isEmpty())
        state.removeProperty (ids::tempoChanges, &undoManager);
    else
        state.setProperty (ids::tempoChanges, changes, &undoManager);
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

    clip.setProperty (ids::start, juce::jmax (0.0, newStart), undoTarget);

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
    if (auto clip = getClip (clipId); clip.isValid())
    {
        if (keep)
            clip.setProperty (ids::formant, true, undoTarget);
        else
            clip.removeProperty (ids::formant, undoTarget);
    }
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

warp::Settings Edit::getWarpSettings (const juce::ValueTree& clip)
{
    warp::Settings settings;
    settings.map = getClipWarp (clip);
    settings.semitones = (double) clip.getProperty (ids::transpose, 0.0) + (double) clip.getParent().getProperty (ids::transpose, 0.0);
    settings.semitones = juce::jlimit (-warp::Settings::maxSemitones, warp::Settings::maxSemitones, settings.semitones);
    settings.algorithm = warp::algorithmFromKey (clip[ids::stretch].toString());
    settings.formant = (bool) clip.getProperty (ids::formant, false);
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

void Edit::setClipGain (int clipId, float decibels)
{
    getClip (clipId).setProperty (ids::gain, juce::jlimit (-60.0f, 24.0f, decibels), undoTarget);
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

    inserts.addChild (insert, index, undoTarget);
    return insert;
}

void Edit::removeInsert (int insertId)
{
    if (auto insert = getInsert (insertId); insert.isValid())
        insert.getParent().removeChild (insert, undoTarget);
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

juce::ValueTree Edit::getInsert (int insertId) const
{
    return findById (ids::INSERT, insertId);
}
}
