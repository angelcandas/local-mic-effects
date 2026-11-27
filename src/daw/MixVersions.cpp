#include "Edit.h"

#include <set>

// Mix versions (declared in Edit.h): named snapshots of the mix, to switch between and compare.
namespace daw
{
namespace
{
    const juce::Identifier snapshotType { "MIXSNAP" };

    // What a version keeps of a TRACK: its mix properties (missing ones are removed on
    // recall), and its INSERTS, SENDs and MACROS.
    const juce::Identifier trackMixIds[] { ids::volume, ids::pan, ids::mute, ids::solo, ids::output, ids::group,
                                           ids::ride, ids::panAutomation, ids::polarity, ids::nudge, ids::inGain, ids::outGain };

    bool isTrackMixChild (const juce::ValueTree& child)
    {
        return child.hasType (ids::INSERTS) || child.hasType (ids::SEND) || child.hasType (ids::MACROS);
    }

    juce::String encode (const juce::ValueTree& snapshot)
    {
        juce::MemoryOutputStream out;
        snapshot.writeToStream (out);
        return out.getMemoryBlock().toBase64Encoding();
    }

    juce::ValueTree decode (const juce::String& text)
    {
        juce::MemoryBlock block;
        if (! block.fromBase64Encoding (text))
            return {};
        return juce::ValueTree::readFromData (block.getData(), block.getSize());
    }
}

//==============================================================================
juce::ValueTree Edit::captureMix() const
{
    juce::ValueTree snapshot (snapshotType);
    for (const auto& track : getTracks())
    {
        juce::ValueTree copy (ids::TRACK);
        copy.setProperty (ids::id, track[ids::id], nullptr);
        for (const auto& id : trackMixIds)
            if (track.hasProperty (id))
                copy.setProperty (id, track[id], nullptr);
        for (const auto& child : track)
            if (isTrackMixChild (child))
                copy.appendChild (child.createCopy(), nullptr);
        snapshot.appendChild (copy, nullptr);
    }
    for (const auto& bus : getReturns())
        snapshot.appendChild (bus.createCopy(), nullptr);
    if (const auto master = getMaster(); master.isValid())
        snapshot.appendChild (master.createCopy(), nullptr);
    return snapshot;
}

void Edit::applyMix (const juce::ValueTree& snapshot)
{
    if (! snapshot.hasType (snapshotType))
        return;

    // The returns and groups: as they were (in place when they're still here).
    std::set<int> returnIds;
    for (const auto& bus : snapshot)
        if (bus.hasType (ids::RETURN))
            returnIds.insert ((int) bus[ids::id]);

    for (auto live : getReturns())
        if (returnIds.count ((int) live[ids::id]) == 0)
            state.removeChild (live, undoTarget);

    for (const auto& bus : snapshot)
    {
        if (! bus.hasType (ids::RETURN))
            continue;
        if (auto live = getReturn ((int) bus[ids::id]); live.isValid())
        {
            live.copyPropertiesFrom (bus, undoTarget);
            live.removeAllChildren (undoTarget);
            for (const auto& child : bus)
                live.appendChild (child.createCopy(), undoTarget);
        }
        else
        {
            // After the last return there is (returns sit after the tracks).
            const auto returns = getReturns();
            const auto index = returns.isEmpty() ? -1 : state.indexOf (returns.getLast()) + 1;
            state.addChild (bus.createCopy(), index, undoTarget);
        }
    }

    // The master.
    if (const auto saved = snapshot.getChildWithName (ids::MASTER); saved.isValid())
    {
        auto master = getMaster();
        master.copyPropertiesFrom (saved, undoTarget);
        master.removeAllChildren (undoTarget);
        for (const auto& child : saved)
            master.appendChild (child.createCopy(), undoTarget);
    }

    // The tracks it knows; the others keep their own mix.
    for (auto track : getTracks())
    {
        const auto saved = snapshot.getChildWithProperty (ids::id, track[ids::id]);
        if (saved.hasType (ids::TRACK))
        {
            for (const auto& id : trackMixIds)
            {
                if (saved.hasProperty (id))
                    track.setProperty (id, saved[id], undoTarget);
                else
                    track.removeProperty (id, undoTarget);
            }

            auto insertsAt = -1;
            for (int i = track.getNumChildren(); --i >= 0;)
                if (const auto child = track.getChild (i); isTrackMixChild (child))
                {
                    if (child.hasType (ids::INSERTS))
                        insertsAt = i;
                    track.removeChild (i, undoTarget);
                }
            for (const auto& child : saved)
            {
                if (child.hasType (ids::INSERTS))
                    track.addChild (child.createCopy(), juce::jmax (0, insertsAt), undoTarget);
                else
                    track.appendChild (child.createCopy(), undoTarget);
            }
        }

        // Nothing may point at a return that isn't there now.
        if (const auto output = getTrackOutput (track); output != masterId && returnIds.count (output) == 0)
            track.removeProperty (ids::output, undoTarget);
        if (const auto group = getTrackGroup (track); group >= 0 && returnIds.count (group) == 0)
            track.removeProperty (ids::group, undoTarget);
        for (int i = track.getNumChildren(); --i >= 0;)
            if (const auto send = track.getChild (i); send.hasType (ids::SEND) && returnIds.count ((int) send[ids::target]) == 0)
                track.removeChild (i, undoTarget);
    }
}

//==============================================================================
juce::Array<juce::ValueTree> Edit::getMixVersions() const
{
    juce::Array<juce::ValueTree> versions;
    for (const auto& mix : state.getChildWithName (ids::MIXES))
        if (mix.hasType (ids::MIX))
            versions.add (mix);
    return versions;
}

int Edit::getCurrentMixVersion() const
{
    const auto mixes = state.getChildWithName (ids::MIXES);
    const auto id = (int) mixes.getProperty (ids::current, -1);
    return mixes.getChildWithProperty (ids::id, id).isValid() ? id : -1;
}

int Edit::getPreviousMixVersion() const
{
    const auto mixes = state.getChildWithName (ids::MIXES);
    const auto id = (int) mixes.getProperty (ids::previous, -1);
    return id != getCurrentMixVersion() && mixes.getChildWithProperty (ids::id, id).isValid() ? id : -1;
}

juce::ValueTree Edit::addMixVersion (const juce::String& name)
{
    auto mixes = state.getChildWithName (ids::MIXES);
    if (! mixes.isValid())
    {
        mixes = juce::ValueTree (ids::MIXES);
        state.appendChild (mixes, undoTarget);
    }

    const auto live = encode (captureMix());
    const auto make = [&] (const juce::String& wanted)
    {
        juce::ValueTree mix (ids::MIX);
        mix.setProperty (ids::id, takeNextId(), nullptr);
        mix.setProperty (ids::name, wanted.trim().isNotEmpty() ? wanted.trim().substring (0, 40)
                                                               : "Mix " + juce::String (mixes.getNumChildren() + 1), nullptr);
        mix.setProperty (ids::mix, live, nullptr);
        mixes.appendChild (mix, undoTarget);
        return mix;
    };

    // The mix so far stays as the first version, so there is something to compare with.
    const auto current = getCurrentMixVersion();
    if (current < 0 && getMixVersions().isEmpty())
        mixes.setProperty (ids::previous, (int) make ({})[ids::id], undoTarget);
    else if (current >= 0)
    {
        updateMixVersion (current);
        mixes.setProperty (ids::previous, current, undoTarget);
    }

    const auto made = make (name);
    mixes.setProperty (ids::current, (int) made[ids::id], undoTarget);
    return made;
}

juce::ValueTree Edit::addMixVersionAfterChange (const juce::ValueTree& before, const juce::String& name,
                                                const juce::String& beforeName)
{
    auto mixes = state.getChildWithName (ids::MIXES);
    if (! mixes.isValid())
    {
        mixes = juce::ValueTree (ids::MIXES);
        state.appendChild (mixes, undoTarget);
    }

    const auto make = [&] (const juce::String& wanted, const juce::String& mix)
    {
        juce::ValueTree version (ids::MIX);
        version.setProperty (ids::id, takeNextId(), nullptr);
        version.setProperty (ids::name, wanted.trim().isNotEmpty() ? wanted.trim().substring (0, 40)
                                                                  : "Mix " + juce::String (mixes.getNumChildren() + 1), nullptr);
        version.setProperty (ids::mix, mix, nullptr);
        mixes.appendChild (version, undoTarget);
        return version;
    };

    // The version you were on keeps the mix from before the change; with none, the mix
    // before becomes a version of its own.
    auto previous = getCurrentMixVersion();
    if (before.hasType (snapshotType))
    {
        if (previous >= 0)
        {
            auto version = mixes.getChildWithProperty (ids::id, previous);
            if (const auto text = encode (before); text != version[ids::mix].toString())
                version.setProperty (ids::mix, text, undoTarget);
        }
        else
            previous = (int) make (beforeName, encode (before))[ids::id];
    }
    const auto made = make (name, encode (captureMix()));
    mixes.setProperty (ids::previous, previous, undoTarget);
    mixes.setProperty (ids::current, (int) made[ids::id], undoTarget);
    return made;
}

bool Edit::recallMixVersion (int mixId)
{
    auto mixes = state.getChildWithName (ids::MIXES);
    const auto mix = mixes.getChildWithProperty (ids::id, mixId);
    if (! mix.hasType (ids::MIX))
        return false;

    const auto current = getCurrentMixVersion();
    if (current == mixId)
        return true;
    if (current >= 0)
        updateMixVersion (current);

    const auto snapshot = decode (mix[ids::mix].toString());
    if (! snapshot.isValid())
        return false;
    applyMix (snapshot);
    mixes.setProperty (ids::previous, current, undoTarget);
    mixes.setProperty (ids::current, mixId, undoTarget);
    return true;
}

void Edit::updateMixVersion (int mixId)
{
    auto mix = state.getChildWithName (ids::MIXES).getChildWithProperty (ids::id, mixId);
    if (! mix.hasType (ids::MIX))
        return;
    if (const auto live = encode (captureMix()); live != mix[ids::mix].toString())
        mix.setProperty (ids::mix, live, undoTarget);
}

void Edit::renameMixVersion (int mixId, const juce::String& name)
{
    auto mix = state.getChildWithName (ids::MIXES).getChildWithProperty (ids::id, mixId);
    if (mix.hasType (ids::MIX) && name.trim().isNotEmpty())
        mix.setProperty (ids::name, name.trim().substring (0, 40), undoTarget);
}

void Edit::removeMixVersion (int mixId)
{
    auto mixes = state.getChildWithName (ids::MIXES);
    const auto mix = mixes.getChildWithProperty (ids::id, mixId);
    if (! mix.hasType (ids::MIX))
        return;
    mixes.removeChild (mix, undoTarget);
    if (mixes.getNumChildren() == 0)
        state.removeChild (mixes, undoTarget);
}
}
