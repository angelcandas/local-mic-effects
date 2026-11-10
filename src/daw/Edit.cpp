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
