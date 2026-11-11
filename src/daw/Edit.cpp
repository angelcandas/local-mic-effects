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

    // Older or hand-edited projects can miss parts the editing code relies on: give every
    // track and the master an effects list, and anything without an id a fresh one.
    for (auto child : state)
    {
        if (! child.hasType (ids::TRACK) && ! child.hasType (ids::MASTER))
            continue;

        if (! child.getChildWithName (ids::INSERTS).isValid())
            child.appendChild (juce::ValueTree (ids::INSERTS), nullptr);

        if (child.hasType (ids::TRACK))
            for (auto item : child)
                if (item.hasType (ids::CLIP) && ! item.hasProperty (ids::id))
                    item.setProperty (ids::id, takeNextId(), nullptr);

        if (child.hasType (ids::TRACK) && ! child.hasProperty (ids::id))
            child.setProperty (ids::id, takeNextId(), nullptr);
    }

    undoManager.clearUndoHistory();
}

int Edit::takeNextId()
{
    return nextId++;
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

    // Tracks come after MASTER; index counts tracks only.
    const auto numTracks = getTracks().size();
    const auto position = index < 0 || index > numTracks ? -1 : state.indexOf (getMaster()) + 1 + index;
    state.addChild (track, position, &undoManager);
    return track;
}

void Edit::removeTrack (int trackId)
{
    if (auto track = getTrack (trackId); track.isValid())
        state.removeChild (track, &undoManager);
}

void Edit::moveTrack (int trackId, int newIndex)
{
    auto track = getTrack (trackId);
    if (! track.isValid())
        return;

    const auto numTracks = getTracks().size();
    const auto first = state.indexOf (getMaster()) + 1;
    state.moveChild (state.indexOf (track), first + juce::jlimit (0, numTracks - 1, newIndex), &undoManager);
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
    std::function<void (juce::ValueTree&)> renumber = [&] (juce::ValueTree& tree)
    {
        if (tree.hasProperty (ids::id))
            tree.setProperty (ids::id, takeNextId(), nullptr);
        for (auto child : tree)
            renumber (child);
    };
    renumber (copy);
    copy.setProperty (ids::name, (track[ids::name].toString() + " copy").substring (0, 60), nullptr);
    copy.setProperty (ids::arm, false, nullptr);
    copy.setProperty (ids::monitor, false, nullptr);

    state.addChild (copy, state.indexOf (track) + 1, &undoManager);
    return (int) copy[ids::id];
}

void Edit::copyInserts (int fromTrackId, int toTrackId)
{
    const auto from = getInserts (getTrackOrMaster (fromTrackId));
    auto to = getInserts (getTrackOrMaster (toTrackId));
    if (! from.isValid() || ! to.isValid() || from == to)
        return;

    to.removeAllChildren (&undoManager);
    for (const auto& insert : from)
    {
        auto copy = insert.createCopy();
        copy.setProperty (ids::id, takeNextId(), nullptr);
        to.appendChild (copy, &undoManager);
    }
}

void Edit::soloExclusive (int trackId)
{
    for (auto track : getTracks())
        track.setProperty (ids::solo, (int) track[ids::id] == trackId, &undoManager);
}

void Edit::clearMuteAndSolo()
{
    for (auto track : getTracks())
    {
        track.setProperty (ids::solo, false, &undoManager);
        track.setProperty (ids::mute, false, &undoManager);
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

juce::ValueTree Edit::getTrackOrMaster (int trackId) const
{
    return trackId == masterId ? getMaster() : getTrack (trackId);
}

void Edit::setTrackVolume (int trackId, float decibels) { getTrack (trackId).setProperty (ids::volume, decibels, &undoManager); }
void Edit::setTrackPan (int trackId, float pan)         { getTrack (trackId).setProperty (ids::pan, juce::jlimit (-1.0f, 1.0f, pan), &undoManager); }
void Edit::setTrackMute (int trackId, bool mute)        { getTrack (trackId).setProperty (ids::mute, mute, &undoManager); }
void Edit::setTrackSolo (int trackId, bool solo)        { getTrack (trackId).setProperty (ids::solo, solo, &undoManager); }
void Edit::setMasterVolume (float decibels)             { getMaster().setProperty (ids::volume, decibels, &undoManager); }

void Edit::setChainGain (int trackId, bool output, float decibels)
{
    auto tree = trackId == masterId ? getMaster() : getTrack (trackId);
    if (tree.isValid())
        tree.setProperty (output ? ids::outGain : ids::inGain, juce::jlimit (-maxChainGain, maxChainGain, decibels), &undoManager);
}

// Arm and monitor are recording state, not edits: not undoable.
void Edit::setTrackArm (int trackId, bool arm)          { getTrack (trackId).setProperty (ids::arm, arm, nullptr); }
void Edit::setTrackMonitor (int trackId, bool monitor)  { getTrack (trackId).setProperty (ids::monitor, monitor, nullptr); }
void Edit::setTempo (double bpm)                        { state.setProperty (ids::tempo, juce::jlimit (20.0, 400.0, bpm), &undoManager); }

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
    track.appendChild (clip, &undoManager);
    return clip;
}

void Edit::removeClip (int clipId)
{
    if (auto clip = getClip (clipId); clip.isValid())
        clip.getParent().removeChild (clip, &undoManager);
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

    clip.setProperty (ids::start, juce::jmax (0.0, newStart), &undoManager);

    if (auto target = getTrack (toTrackId); target.isValid() && target != clip.getParent())
    {
        clip.getParent().removeChild (clip, &undoManager);
        target.appendChild (clip, &undoManager);
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

    clip.setProperty (ids::start, clamped, &undoManager);
    clip.setProperty (ids::offset, offset + delta, &undoManager);
    clip.setProperty (ids::length, length - delta, &undoManager);
}

void Edit::trimClipEnd (int clipId, double newEnd)
{
    auto clip = getClip (clipId);
    if (! clip.isValid())
        return;

    const auto start = (double) clip[ids::start];
    clip.setProperty (ids::length, juce::jmax (minClipSeconds, newEnd - start), &undoManager);
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
    const auto id = takeNextId();
    right.setProperty (ids::id, id, nullptr);
    right.setProperty (ids::start, time, nullptr);
    right.setProperty (ids::offset, offset + (time - start), nullptr);
    right.setProperty (ids::length, start + length - time, nullptr);
    right.setProperty (ids::fadeIn, 0.0, nullptr);

    clip.setProperty (ids::length, time - start, &undoManager);
    clip.setProperty (ids::fadeOut, 0.0, &undoManager);
    clip.getParent().addChild (right, clip.getParent().indexOf (clip) + 1, &undoManager);
    return id;
}

int Edit::duplicateClip (int clipId)
{
    auto clip = getClip (clipId);
    if (! clip.isValid())
        return -1;

    auto copy = clip.createCopy();
    const auto id = takeNextId();
    copy.setProperty (ids::id, id, nullptr);
    copy.setProperty (ids::start, (double) clip[ids::start] + (double) clip[ids::length], nullptr);
    clip.getParent().addChild (copy, clip.getParent().indexOf (clip) + 1, &undoManager);
    return id;
}

void Edit::setClipGain (int clipId, float decibels)
{
    getClip (clipId).setProperty (ids::gain, juce::jlimit (-60.0f, 24.0f, decibels), &undoManager);
}

void Edit::setClipName (int clipId, const juce::String& name)
{
    if (auto clip = getClip (clipId); clip.isValid() && name.trim().isNotEmpty())
        clip.setProperty (ids::name, name.trim().substring (0, 80), &undoManager);
}

void Edit::setClipMute (int clipId, bool mute)
{
    if (auto clip = getClip (clipId); clip.isValid())
        clip.setProperty (ids::mute, mute, &undoManager);
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
        const auto id = takeNextId();
        part.setProperty (ids::id, id, nullptr);
        part.setProperty (ids::start, start + a, nullptr);
        part.setProperty (ids::offset, offset + a, nullptr);
        part.setProperty (ids::length, b - a, nullptr);
        const auto fade = juce::jmin (fadeSeconds, (b - a) / 2.0);
        const auto half = (b - a) / 2.0;
        part.setProperty (ids::fadeIn, a > 0.0 ? fade : juce::jmin (half, (double) clip[ids::fadeIn]), nullptr);
        part.setProperty (ids::fadeOut, b < length ? fade : juce::jmin (half, (double) clip[ids::fadeOut]), nullptr);
        parent.addChild (part, ++index, &undoManager);
        created.add (id);
    }

    if (! created.isEmpty())
        parent.removeChild (clip, &undoManager);

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
    clip.setProperty (ids::fadeIn, fadeIn, &undoManager);
    clip.setProperty (ids::fadeOut, juce::jlimit (0.0, length - fadeIn, fadeOutSeconds), &undoManager);
}

double Edit::snapToGrid (double seconds, double beats) const
{
    const auto step = juce::jmax (1.0e-3, beats) * 60.0 / getTempo();
    return std::round (seconds / step) * step;
}

void Edit::setBeatsPerBar (int beats)
{
    state.setProperty (ids::beatsPerBar, juce::jlimit (2, 12, beats), &undoManager);
}

void Edit::setKey (int tonic, bool minor)
{
    if (tonic < 0 || tonic > 11)
    {
        state.removeProperty (ids::key, &undoManager);
        state.removeProperty (ids::keyMinor, &undoManager);
        return;
    }

    state.setProperty (ids::key, tonic, &undoManager);
    state.setProperty (ids::keyMinor, minor, &undoManager);
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
    state.appendChild (marker, &undoManager);
    return marker;
}

void Edit::removeMarker (int markerId)
{
    if (auto marker = findById (ids::MARKER, markerId); marker.isValid())
        state.removeChild (marker, &undoManager);
}

void Edit::setMarkerTime (int markerId, double timeSeconds)
{
    if (auto marker = findById (ids::MARKER, markerId); marker.isValid())
        marker.setProperty (ids::time, juce::jmax (0.0, timeSeconds), &undoManager);
}

void Edit::setMarkerName (int markerId, const juce::String& name)
{
    if (auto marker = findById (ids::MARKER, markerId); marker.isValid() && name.trim().isNotEmpty())
        marker.setProperty (ids::name, name.trim().substring (0, 60), &undoManager);
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
    state.setProperty (ids::loopStart, start, &undoManager);
    state.setProperty (ids::loopEnd, end, &undoManager);
    state.setProperty (ids::loopOn, on && end - start >= minClipSeconds, &undoManager);
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

    inserts.addChild (insert, index, &undoManager);
    return insert;
}

void Edit::removeInsert (int insertId)
{
    if (auto insert = getInsert (insertId); insert.isValid())
        insert.getParent().removeChild (insert, &undoManager);
}

void Edit::moveInsert (int insertId, int newIndex)
{
    auto insert = getInsert (insertId);
    if (! insert.isValid())
        return;

    auto inserts = insert.getParent();
    inserts.moveChild (inserts.indexOf (insert), juce::jlimit (0, inserts.getNumChildren() - 1, newIndex), &undoManager);
}

void Edit::setInsertValue (int insertId, const juce::String& valueId, float value)
{
    if (auto insert = getInsert (insertId); insert.isValid() && insert.hasProperty (juce::Identifier (valueId)))
        insert.setProperty (juce::Identifier (valueId), value, &undoManager);
}

juce::ValueTree Edit::getInsert (int insertId) const
{
    return findById (ids::INSERT, insertId);
}
}
