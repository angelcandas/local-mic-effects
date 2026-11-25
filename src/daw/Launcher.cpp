#include "Edit.h"

#include <algorithm>
#include <cmath>

// The clip launcher part of Edit (declared in Edit.h): slots and scenes. How slots play is
// in DawEngine (its launcher).
namespace daw
{
namespace
{
    constexpr double epsilon = 1.0e-6;

    struct QuantizeKey
    {
        const char* key;
        double bars, beats; // one of them
    };
    constexpr QuantizeKey quantizeKeys[] { { "none", 0.0, 0.0 }, { "beat", 0.0, 1.0 }, { "bar", 1.0, 0.0 },
                                           { "2bars", 2.0, 0.0 }, { "4bars", 4.0, 0.0 } };

    double clampBeats (double beats)
    {
        return juce::jlimit (Edit::minSlotBeats, Edit::maxSlotBeats, std::isfinite (beats) ? beats : 4.0);
    }

    // A clip's length in beats, made whole when it's within 2 % of whole beats (a loop
    // cut a hair long or short still sits on the grid).
    double toLoopBeats (double beats)
    {
        const auto whole = std::round (beats);
        if (whole >= 1.0 && std::abs (beats - whole) <= 0.02 * whole)
            return whole;
        return clampBeats (std::round (beats * 64.0) / 64.0);
    }
}

//==============================================================================
juce::ValueTree Edit::getSlot (int slotId) const
{
    return findById (ids::SLOT, slotId);
}

juce::ValueTree Edit::getSlotAt (int trackId, int row) const
{
    for (const auto& child : getTrack (trackId))
        if (child.hasType (ids::SLOT) && (int) child[ids::row] == row)
            return child;
    return {};
}

juce::Array<juce::ValueTree> Edit::getSlots (int trackId) const
{
    juce::Array<juce::ValueTree> slots;
    for (const auto& child : getTrack (trackId))
        if (child.hasType (ids::SLOT))
            slots.add (child);

    std::sort (slots.begin(), slots.end(), [] (const juce::ValueTree& a, const juce::ValueTree& b) { return (int) a[ids::row] < (int) b[ids::row]; });
    return slots;
}

juce::ValueTree Edit::getSlotClip (const juce::ValueTree& slot)
{
    return slot.getChildWithName (ids::CLIP);
}

double Edit::getSlotBeats (const juce::ValueTree& slot)
{
    return clampBeats ((double) slot.getProperty (ids::beats, 4.0));
}

juce::ValueTree Edit::getSlotOf (const juce::ValueTree& clip)
{
    const auto parent = clip.getParent();
    return parent.hasType (ids::SLOT) ? parent : juce::ValueTree();
}

//==============================================================================
int Edit::addSlotFromClip (int clipId, int trackId, int row)
{
    const auto source = getClip (clipId);
    auto track = getTrack (trackId);
    if (! source.isValid() || ! track.isValid() || isMidiClip (source) != isMidiTrack (track)
        || row < 0 || row >= maxScenes || getSlotOf (source).isValid())
        return -1;

    const auto start = (double) source[ids::start], length = (double) source[ids::length];
    const auto tempo = getTempoMapFor (source);
    const auto beats = toLoopBeats (tempo.toBeats (start + length) - tempo.toBeats (start));

    if (auto old = getSlotAt (trackId, row); old.isValid())
        track.removeChild (old, undoTarget);

    auto clip = source.createCopy();
    renumber (clip);
    clip.setProperty (ids::start, 0.0, nullptr);
    clip.removeProperty (ids::take, nullptr);
    clip.removeProperty (ids::mute, nullptr);

    juce::ValueTree slot (ids::SLOT);
    slot.setProperty (ids::id, takeNextId(), nullptr);
    slot.setProperty (ids::row, row, nullptr);
    slot.setProperty (ids::beats, beats, nullptr);
    slot.setProperty (ids::loop, true, nullptr);
    slot.appendChild (clip, nullptr);
    track.appendChild (slot, undoTarget);

    if (row >= getSceneCount())
        setSceneCount (row + 1);
    return (int) slot[ids::id];
}

int Edit::addMidiSlot (int trackId, int row, double beats)
{
    auto track = getTrack (trackId);
    if (! track.isValid() || ! isMidiTrack (track) || row < 0 || row >= maxScenes)
        return -1;

    beats = clampBeats (beats > 0.0 ? beats : (double) getBeatsPerBar());
    const auto tempo = getTempoMap();

    if (auto old = getSlotAt (trackId, row); old.isValid())
        track.removeChild (old, undoTarget);

    // Made on the track (addMidiClip sets it up as any MIDI clip), then moved into the slot.
    juce::ValueTree clip;
    {
        const NotUndoable building (*this);
        clip = addMidiClip (trackId, 0.0, tempo.toSeconds (beats), track[ids::name].toString() + " " + juce::String (row + 1));
        track.removeChild (clip, nullptr);
    }

    juce::ValueTree slot (ids::SLOT);
    slot.setProperty (ids::id, takeNextId(), nullptr);
    slot.setProperty (ids::row, row, nullptr);
    slot.setProperty (ids::beats, beats, nullptr);
    slot.setProperty (ids::loop, true, nullptr);
    slot.appendChild (clip, nullptr);
    track.appendChild (slot, undoTarget);

    if (row >= getSceneCount())
        setSceneCount (row + 1);
    return (int) slot[ids::id];
}

int Edit::moveSlot (int slotId, int trackId, int row, bool copy)
{
    auto slot = getSlot (slotId);
    auto track = getTrack (trackId);
    if (! slot.isValid() || ! track.isValid() || row < 0 || row >= maxScenes
        || isMidiTrack (track) != isMidiTrack (slot.getParent()))
        return -1;

    if (! copy && slot.getParent() == track && (int) slot[ids::row] == row)
        return slotId;

    if (auto old = getSlotAt (trackId, row); old.isValid() && old != slot)
        track.removeChild (old, undoTarget);

    if (copy)
    {
        auto made = slot.createCopy();
        renumber (made);
        made.setProperty (ids::row, row, nullptr);
        track.appendChild (made, undoTarget);
        slot = made;
    }
    else if (slot.getParent() != track)
    {
        slot.getParent().removeChild (slot, undoTarget);
        slot.setProperty (ids::row, row, nullptr);
        track.appendChild (slot, undoTarget);
    }
    else
    {
        slot.setProperty (ids::row, row, undoTarget);
    }

    if (row >= getSceneCount())
        setSceneCount (row + 1);
    return (int) slot[ids::id];
}

void Edit::removeSlot (int slotId)
{
    if (auto slot = getSlot (slotId); slot.isValid())
        slot.getParent().removeChild (slot, undoTarget);
}

void Edit::setSlotBeats (int slotId, double beats)
{
    if (auto slot = getSlot (slotId); slot.isValid())
        slot.setProperty (ids::beats, clampBeats (beats), undoTarget);
}

void Edit::setSlotLoop (int slotId, bool loop)
{
    if (auto slot = getSlot (slotId); slot.isValid())
        slot.setProperty (ids::loop, loop, undoTarget);
}

void Edit::setSlotName (int slotId, const juce::String& name)
{
    if (auto clip = getSlotClip (getSlot (slotId)); clip.isValid())
        clip.setProperty (ids::name, name.trim().substring (0, 60), undoTarget);
}

//==============================================================================
int Edit::getSceneCount() const
{
    auto count = juce::jlimit (1, maxScenes, (int) state.getProperty (ids::sceneCount, defaultScenes));
    for (const auto& track : getTracks())
        for (const auto& child : track)
            if (child.hasType (ids::SLOT))
                count = juce::jmax (count, (int) child[ids::row] + 1);
    return juce::jmin (count, maxScenes);
}

void Edit::setSceneCount (int count)
{
    count = juce::jlimit (1, maxScenes, count);

    for (auto track : getTracks())
        for (int i = track.getNumChildren(); --i >= 0;)
            if (const auto child = track.getChild (i); child.hasType (ids::SLOT) && (int) child[ids::row] >= count)
                track.removeChild (i, undoTarget);

    for (int i = state.getNumChildren(); --i >= 0;)
        if (const auto child = state.getChild (i); child.hasType (ids::SCENE) && (int) child[ids::row] >= count)
            state.removeChild (i, undoTarget);

    state.setProperty (ids::sceneCount, count, undoTarget);
}

void Edit::insertScene (int row)
{
    const auto count = getSceneCount();
    row = juce::jlimit (0, count, row);
    if (count >= maxScenes)
        setSceneCount (maxScenes - 1); // the last row makes way

    const auto shift = [this, row] (juce::ValueTree tree)
    {
        if ((int) tree[ids::row] >= row)
            tree.setProperty (ids::row, (int) tree[ids::row] + 1, undoTarget);
    };

    for (const auto& track : getTracks())
        for (const auto& child : track)
            if (child.hasType (ids::SLOT))
                shift (child);
    for (const auto& child : state)
        if (child.hasType (ids::SCENE))
            shift (child);

    state.setProperty (ids::sceneCount, juce::jmin (maxScenes, getSceneCount() + 1), undoTarget);
}

void Edit::removeScene (int row)
{
    const auto count = getSceneCount();
    if (row < 0 || row >= count)
        return;

    const auto apply = [this, row] (juce::ValueTree parent, const juce::Identifier& type)
    {
        for (int i = parent.getNumChildren(); --i >= 0;)
        {
            auto child = parent.getChild (i);
            if (! child.hasType (type))
                continue;
            if ((int) child[ids::row] == row)
                parent.removeChild (i, undoTarget);
            else if ((int) child[ids::row] > row)
                child.setProperty (ids::row, (int) child[ids::row] - 1, undoTarget);
        }
    };

    for (const auto& track : getTracks())
        apply (track, ids::SLOT);
    apply (state, ids::SCENE);

    state.setProperty (ids::sceneCount, juce::jmax (1, count - 1), undoTarget);
}

void Edit::duplicateScene (int row)
{
    if (row < 0 || row >= getSceneCount())
        return;

    insertScene (row + 1);

    for (auto track : getTracks())
        if (const auto slot = getSlotAt ((int) track[ids::id], row); slot.isValid())
        {
            auto copy = slot.createCopy();
            renumber (copy);
            copy.setProperty (ids::row, row + 1, nullptr);
            track.appendChild (copy, undoTarget);
        }

    if (const auto name = getSceneName (row); name.isNotEmpty())
        setSceneName (row + 1, name + " copy");
}

juce::String Edit::getSceneName (int row) const
{
    for (const auto& child : state)
        if (child.hasType (ids::SCENE) && (int) child[ids::row] == row)
            return child[ids::name].toString();
    return {};
}

void Edit::setSceneName (int row, const juce::String& name)
{
    if (row < 0 || row >= maxScenes)
        return;

    const auto trimmed = name.trim().substring (0, 40);
    for (int i = state.getNumChildren(); --i >= 0;)
    {
        auto child = state.getChild (i);
        if (! child.hasType (ids::SCENE) || (int) child[ids::row] != row)
            continue;

        if (trimmed.isEmpty())
            state.removeChild (i, undoTarget);
        else
            child.setProperty (ids::name, trimmed, undoTarget);
        return;
    }

    if (trimmed.isEmpty())
        return;

    juce::ValueTree scene (ids::SCENE);
    scene.setProperty (ids::row, row, nullptr);
    scene.setProperty (ids::name, trimmed, nullptr);
    state.appendChild (scene, undoTarget);
}

//==============================================================================
juce::StringArray Edit::getLaunchQuantizeKeys()
{
    juce::StringArray keys;
    for (const auto& q : quantizeKeys)
        keys.add (q.key);
    return keys;
}

juce::String Edit::getLaunchQuantize() const
{
    const auto key = state.getProperty (ids::launchQuantize, "bar").toString();
    return getLaunchQuantizeKeys().contains (key) ? key : juce::String ("bar");
}

void Edit::setLaunchQuantize (const juce::String& key)
{
    if (getLaunchQuantizeKeys().contains (key))
        state.setProperty (ids::launchQuantize, key, undoTarget);
}

double Edit::getLaunchQuantizeBeats() const
{
    const auto key = getLaunchQuantize();
    for (const auto& q : quantizeKeys)
        if (key == q.key)
            return q.beats > 0.0 ? q.beats : q.bars * getBeatsPerBar();
    return (double) getBeatsPerBar();
}

//==============================================================================
juce::Array<int> Edit::writeSlotToTimeline (int slotId, double startSeconds, double lengthSeconds, double phaseSeconds)
{
    juce::Array<int> made;
    const auto slot = getSlot (slotId);
    const auto source = getSlotClip (slot);
    auto track = slot.getParent();
    if (! source.isValid() || ! track.isValid() || lengthSeconds < minClipSeconds)
        return made;

    startSeconds = juce::jmax (0.0, startSeconds);
    const auto endSeconds = startSeconds + lengthSeconds;
    const auto tempo = getTempoMap();
    const auto pass = juce::jmax (minClipSeconds, tempo.toSeconds (tempo.toBeats (startSeconds) + getSlotBeats (slot)) - startSeconds);
    const auto clipLength = (double) source[ids::length];
    const auto loop = getSlotLoop (slot);
    phaseSeconds = loop ? std::fmod (juce::jmax (0.0, phaseSeconds), pass) : juce::jmax (0.0, phaseSeconds);
    if (phaseSeconds >= pass - minClipSeconds)
        return made; // a one-shot that had played

    // What the track had there makes way (audio crossfades with what stays; MIDI is cut).
    if (isMidiTrack (track))
    {
        juce::Array<juce::ValueTree> clips;
        for (const auto& child : track)
            if (child.hasType (ids::CLIP))
                clips.add (child);

        for (const auto& clip : clips)
        {
            const auto s = (double) clip[ids::start], e = s + (double) clip[ids::length];
            const auto id = (int) clip[ids::id];
            if (e <= startSeconds + epsilon || s >= endSeconds - epsilon)
                continue;
            if (s >= startSeconds - epsilon && e <= endSeconds + epsilon)
                removeClip (id);
            else if (s < startSeconds && e > endSeconds)
            {
                const auto right = splitClip (id, endSeconds);
                juce::ignoreUnused (right);
                trimClipEnd (id, startSeconds);
            }
            else if (s < startSeconds)
                trimClipEnd (id, startSeconds);
            else
                trimClipStart (id, endSeconds);
        }
    }
    else
    {
        cutComp (track, startSeconds, endSeconds);
    }

    // Pass by pass: the first picks up `phaseSeconds` into the loop.
    for (auto at = startSeconds, into = phaseSeconds; at < endSeconds - minClipSeconds; into = 0.0)
    {
        const auto passLeft = pass - into;
        const auto heard = juce::jmin (passLeft, endSeconds - at, juce::jmax (0.0, clipLength - into));

        if (heard >= minClipSeconds)
        {
            auto piece = source.createCopy();
            renumber (piece);
            piece.setProperty (ids::start, at, nullptr);
            piece.setProperty (ids::offset, (double) source[ids::offset] + into, nullptr);
            piece.setProperty (ids::length, heard, nullptr);
            if (into > 0.0)
                piece.setProperty (ids::fadeIn, juce::jmax ((double) source.getProperty (ids::fadeIn, 0.0), compFadeSeconds), nullptr);
            track.appendChild (piece, undoTarget);
            made.add ((int) piece[ids::id]);
        }

        at += passLeft;
        if (! loop)
            break;
    }

    return made;
}
}
