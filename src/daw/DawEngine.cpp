#include "DawEngine.h"
#include "Patterns.h"
#include "RackInsert.h"

#include <functional>

namespace daw
{
namespace
{
    // Balance: the centre is unity on both sides; panning turns the other side down.
    std::pair<float, float> balanceGains (float pan) noexcept
    {
        const auto halfPi = juce::MathConstants<float>::halfPi;
        return { pan > 0.0f ? std::cos (pan * halfPi) : 1.0f, pan < 0.0f ? std::cos (-pan * halfPi) : 1.0f };
    }

    // A volume ride's level at a timeline sample (dB), straight lines between points.
    float rideDbAt (const std::vector<std::pair<juce::int64, float>>& ride, juce::int64 at) noexcept
    {
        if (at <= ride.front().first) return ride.front().second;
        if (at >= ride.back().first)  return ride.back().second;
        const auto next = std::upper_bound (ride.begin(), ride.end(), at, [] (juce::int64 t, const auto& p) { return t < p.first; });
        const auto& [t1, d1] = *next;
        const auto& [t0, d0] = *(next - 1);
        return t1 > t0 ? d0 + (d1 - d0) * (float) ((double) (at - t0) / (double) (t1 - t0)) : d1;
    }
}

namespace
{
    constexpr double smoothingSeconds = 0.02;

    float volumeToGain (const juce::var& decibels)
    {
        return juce::Decibels::decibelsToGain ((float) decibels, -100.0f);
    }

    void updatePeak (std::atomic<float>& peak, float value) noexcept
    {
        auto current = peak.load (std::memory_order_relaxed);
        while (value > current && ! peak.compare_exchange_weak (current, value, std::memory_order_relaxed)) {}
    }
}

DawEngine::DawEngine (Edit& e, bool isRealtime) : edit (e), realtime (isRealtime)
{
    pool.setSampleRate (sampleRate);
    pool.setSynchronous (! realtime);
    pool.onLoaded = [this] { requestRebuild(); };
    liveMidi.ensureSize (4096);

    edit.getState().addListener (this);
    updateTempo();

    // An offline engine builds its first graph when it's prepared (or on its first
    // block), at the rate it renders at, so nothing is decoded twice.
    if (realtime)
        rebuild();
    else
        rebuildPending = true;

    if (realtime)
        startTimerHz (10);
}

DawEngine::~DawEngine()
{
    *alive = false;
    stopTimer();
    cancelPendingUpdate();
    edit.getState().removeListener (this);

    delete incoming.exchange (nullptr);
    delete active;
    active = nullptr;
    collectGarbage();

    delete previewIncoming.exchange (nullptr);
    delete previewActive;
    previewActive = nullptr;
    collectPreviews();
}

//==============================================================================
void DawEngine::prepare (double newSampleRate, int maximumBlockSize)
{
    if (auto* mm = juce::MessageManager::getInstanceWithoutCreating(); realtime && mm != nullptr && ! mm->isThisTheMessageThread())
    {
        juce::MessageManager::callAsync ([this, newSampleRate, maximumBlockSize, weak = std::weak_ptr<bool> (alive)]
        {
            if (auto flag = weak.lock(); flag != nullptr && *flag)
                prepare (newSampleRate, maximumBlockSize);
        });
        return;
    }

    if (newSampleRate > 0.0 && ! juce::exactlyEqual (newSampleRate, sampleRate))
    {
        position = (juce::int64) ((double) position.load() * newSampleRate / sampleRate);
        sampleRate = newSampleRate;
        pool.setSampleRate (sampleRate);
        chordFollower = nullptr; // the next graph gets one at the new rate
    }

    blockSize = juce::jmax (1, maximumBlockSize);
    if ((int) silence.size() < blockSize)
        silence.assign ((size_t) blockSize, 0.0f);
    // Stops (and keeps) a take in progress.
    for (auto& take : takes)
    {
        take.on = false;
        if (take.recorder != nullptr)
            take.recorder->stop();
    }
    recordingAudio = false;
    countingIn = false;
    recordingMidi = false;
    punchEnd = -1;
    updateTempo();

    // Fresh strips: every chain (and instrument) is prepared for the new rate and block
    // size. The instruments are made again, from their current settings.
    captureInstrumentStates();
    strips.clear();
    rebuild();
}

void DawEngine::requestRebuild()
{
    // An offline engine lives on the thread that renders it (an export, Auto Mix, key
    // detection...) and is often deleted there: a rebuild posted to the message thread
    // would run there while it renders, or after it's gone. It rebuilds before its next
    // block instead.
    if (realtime)
        triggerAsyncUpdate();
    else
        rebuildPending = true;
}

void DawEngine::rebuildNow()
{
    cancelPendingUpdate();
    rebuild();
}

void DawEngine::loadAllNow()
{
    for (const auto& track : edit.getTracks())
        for (const auto& child : track)
        {
            if (child.hasType (ids::CLIP))
                pool.loadNow (juce::File (child[ids::file].toString()), getPlaybackSettings (child));
            else if (child.hasType (ids::TAKE) && (int) child[ids::id] == Edit::getListenTake (track))
                for (const auto& clip : child)
                    if (clip.hasType (ids::CLIP))
                        pool.loadNow (juce::File (clip[ids::file].toString()), getPlaybackSettings (clip));
        }

    rebuildNow();

    // Before the first block: start every fader, pan and mute where it's set, rather than
    // gliding there from unity (a muted track leaked into the first few ms of a render).
    for (auto& [id, strip] : strips)
    {
        strip->left.setCurrentAndTargetValue (strip->targetLeft.load());
        strip->right.setCurrentAndTargetValue (strip->targetRight.load());
        strip->direct.setCurrentAndTargetValue (strip->targetDirect.load());
    }
}

InstrumentPtr DawEngine::getInstrument (int trackId) const
{
    const auto it = strips.find (trackId);
    return it != strips.end() ? it->second->instrument : nullptr;
}

juce::String DawEngine::getInstrumentError (int trackId) const
{
    const auto it = strips.find (trackId);
    return it != strips.end() ? it->second->instrumentError : juce::String();
}

void DawEngine::captureInstrumentStates()
{
    for (auto& [id, strip] : strips)
    {
        if (strip->instrument == nullptr || ! strip->instrumentTree.isValid() || ! strip->instrumentTree.getParent().isValid())
            continue;

        const auto state = instruments::getState (*strip->instrument);
        if (state != strip->instrumentTree[ids::state].toString())
            strip->instrumentTree.setProperty (ids::state, state, nullptr);
    }

    // Effect plugins (one that didn't load keeps the state it was saved with).
    for (auto& [id, strip] : strips)
        for (auto& [insertId, slot] : strip->plugins)
        {
            if (slot.module == nullptr || ! slot.tree.isValid() || ! slot.tree.getParent().isValid())
                continue;

            const auto state = instruments::getState (*slot.module->getPlugin());
            if (state != slot.tree[ids::state].toString())
                slot.tree.setProperty (ids::state, state, nullptr);
        }
}

InstrumentPtr DawEngine::getInsertPlugin (int insertId) const
{
    for (const auto& [id, strip] : strips)
        if (const auto it = strip->plugins.find (insertId); it != strip->plugins.end())
            return it->second.module != nullptr ? it->second.module->getPlugin() : nullptr;
    return nullptr;
}

int DawEngine::getInsertLatencySamples (int insertId) const
{
    for (const auto& [id, strip] : strips)
        if (const auto it = strip->modulesByInsertId.find (insertId); it != strip->modulesByInsertId.end())
            return it->second->getLatencySamples();
    return 0;
}

juce::String DawEngine::getInsertPluginError (int insertId) const
{
    for (const auto& [id, strip] : strips)
        if (const auto it = strip->plugins.find (insertId); it != strip->plugins.end())
            return it->second.error;
    return {};
}

void DawEngine::retryMissingPlugins()
{
    auto any = false;
    for (auto& [id, strip] : strips)
        for (const auto& [insertId, slot] : strip->plugins)
            if (slot.module == nullptr)
            {
                strip->insertLayout = {}; // a new chain: the running plugins carry over, the others load again
                any = true;
            }

    if (any)
        requestRebuild();
}

void DawEngine::checkPluginLatency()
{
    for (const auto& [id, strip] : strips)
        if (! strip->plugins.empty() && strip->getLatency() != strip->latencyAtBuild)
        {
            requestRebuild();
            return;
        }
}

void DawEngine::addLiveMidi (const juce::MidiMessage& message) noexcept
{
    if (message.getRawDataSize() > 3)
        return;

    const auto* data = message.getRawData();
    juce::uint32 packed = 0;
    for (int i = 0; i < message.getRawDataSize(); ++i)
        packed |= (juce::uint32) data[i] << (8 * i);

    const auto scope = liveFifo.write (1);
    if (scope.blockSize1 == 1)
        liveFifoData[(size_t) scope.startIndex1] = packed;
}

void DawEngine::addLiveMidi (const juce::MidiMessage& message, int trackId) noexcept
{
    if (message.getRawDataSize() > 3)
        return;

    const auto* data = message.getRawData();
    juce::uint32 packed = 0;
    for (int i = 0; i < message.getRawDataSize(); ++i)
        packed |= (juce::uint32) data[i] << (8 * i);

    const auto scope = auditionFifo.write (1);
    if (scope.blockSize1 == 1)
        auditionFifoData[(size_t) scope.startIndex1] = { trackId, packed };
}

void DawEngine::setPatternMode (int patternId)
{
    if (patternId >= 0 && ! edit.getPattern (patternId).isValid())
        patternId = -1;

    if (patternId == patternMode)
        return;

    if (patternMode < 0)
    {
        songPosition = position.load();
        position = 0;
    }
    else if (patternId < 0)
    {
        patternLoopEnd = 0;
        position = songPosition;
    }
    else
    {
        position = 0; // another pattern: from its start
    }

    patternMode = patternId;
    rebuild();
}

std::bitset<128> DawEngine::getHeldNotes() const noexcept
{
    std::bitset<128> held;
    for (int i = 0; i < 128; ++i)
        held[(size_t) i] = ((heldNotes[(size_t) (i / 32)].load (std::memory_order_relaxed) >> (i % 32)) & 1u) != 0;
    return held;
}

std::pair<float, float> DawEngine::takeChainPeaks (int trackId)
{
    const auto it = strips.find (trackId);
    if (it == strips.end())
        return { 0.0f, 0.0f };

    return { it->second->chain.takeInputPeak(), it->second->chain.takeOutputPeak() };
}

void DawEngine::forEachInsert (int trackId, const std::function<void (int, fx::Module&)>& visit)
{
    if (const auto it = strips.find (trackId); it != strips.end())
        for (const auto& [insertId, module] : it->second->modulesByInsertId)
            visit (insertId, *module);
}

std::pair<float, float> DawEngine::takePeaks (int trackId)
{
    const auto it = strips.find (trackId);
    if (it == strips.end())
        return { 0.0f, 0.0f };

    return { it->second->peakLeft.exchange (0.0f), it->second->peakRight.exchange (0.0f) };
}

//==============================================================================
void DawEngine::valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier& property)
{
    if (tree.hasType (ids::INSTRUMENT))
    {
        // A different plugin needs a new instrument; its saved settings and names don't.
        if (property == ids::plugin)
            requestRebuild();
        return;
    }

    if (tree.hasType (ids::PATTERN) || tree.hasType (ids::PART) || tree.hasType (ids::PATTERNCLIP))
    {
        if (property != ids::name && property != ids::colour && property != ids::rows)
            requestRebuild();
        return;
    }

    if (tree.hasType (ids::NOTE))
    {
        requestRebuild();
        return;
    }

    if (tree.hasType (ids::INSERT))
    {
        // An effect plugin's saved settings and names are kept for saving, not played; a
        // different plugin needs a new one (its layout changes).
        if (property == ids::state || property == ids::name || property == ids::vendor || property == ids::description
            || property == ids::label || property == ids::ab || property == ids::abOther)
            return;
        if (property == ids::plugin)
        {
            requestRebuild();
            return;
        }

        // A new type, or automation added, changed or removed (or a value under
        // automation, whose own setting the lanes go back to): a rebuild.
        if (property == ids::type || property == ids::automation || tree.hasProperty (ids::automation)
            || Edit::followsAutomatedMacro (tree, property.toString()))
        {
            requestRebuild();
            if (property == ids::type || property == ids::automation)
                return;
        }

        // An insert value: straight into the running module, no rebuild.
        const auto owner = Edit::getChannelOf (tree);
        if (const auto it = strips.find ((int) owner[ids::id]); it != strips.end())
        {
            const auto& modules = it->second->modulesByInsertId;
            if (const auto module = modules.find ((int) tree[ids::id]); module != modules.end())
            {
                if (property == ids::file)
                    loadInsertFile (*module->second, tree);
                else if (auto* value = module->second->findValue (property.toString()))
                    value->store ((float) tree[property]);
            }

            // Switching autotune or pitch shift on or off changes the track's delay, so
            // the latency compensation is worked out again.
            if (it->second->getLatency() != it->second->latencyAtBuild)
                requestRebuild();
        }

        return;
    }

    if (tree.hasType (ids::PROJECT))
    {
        if (property == ids::tempo || property == ids::tempoChanges)
            requestRebuild(); // the click follows the new tempo map (and so will musical clips)
        else if (property == ids::beatsPerBar)
            updateTempo();
        else if (property == ids::loopOn || property == ids::loopStart || property == ids::loopEnd)
            updateLoop();

        return;
    }

    if (tree.hasType (ids::MACRO))
    {
        if (property == ids::automation)
            requestRebuild(); // the values it's mapped to follow a new lane
        return;
    }

    if (tree.hasType (ids::SEND))
    {
        if (property == ids::level)
            updateMix();
        else
            requestRebuild(); // pre/post or another return: a new graph

        return;
    }

    if (tree.hasType (ids::TRACK) && (property == ids::transpose || property == ids::ride || property == ids::panAutomation || property == ids::output || property == ids::listenTake
                                     || property == ids::polarity || property == ids::nudge))
    {
        requestRebuild(); // its clips play other rendered copies
        return;
    }

    if (tree.hasType (ids::TRACK) || tree.hasType (ids::MASTER) || tree.hasType (ids::RETURN))
    {
        if (property == ids::volume || property == ids::pan || property == ids::mute || property == ids::solo
            || property == ids::monitor || property == ids::monitorAuto || property == ids::arm || property == ids::inGain || property == ids::outGain
            || property == ids::inputLeft || property == ids::inputRight)
            updateMix();
        else if (property == ids::kind)
            requestRebuild();

        return;
    }

    if (tree.hasType (ids::CLIP))
    {
        // The real edit (or an undo) replaces what was being previewed.
        if (property == ids::warp || property == ids::pitchNotes || property == ids::musical || property == ids::clipTempo || property == ids::beatMap)
            previews.erase ((int) tree[ids::id]);
        requestRebuild();
    }
}

//==============================================================================
std::shared_ptr<std::atomic<bool>> DawEngine::getHold (int trackId, const juce::String& target)
{
    auto& flag = laneHolds[{ trackId, target }];
    if (flag == nullptr)
        flag = std::make_shared<std::atomic<bool>> (false);
    return flag;
}

void DawEngine::holdLane (int trackId, const juce::String& target, bool held)
{
    getHold (trackId, target)->store (held);
}

//==============================================================================
void DawEngine::setPreviewWarp (int clipId, const warp::Map& map)
{
    const auto clip = edit.getClip (clipId);
    if (! clip.isValid() || Edit::isMidiClip (clip))
        return;

    // Linked partners render together, so they all preview the same timing.
    auto clipIds = juce::Array<int> { clipId };
    for (const auto& partner : Edit::getLinkedClips (clip))
        clipIds.add ((int) partner[ids::id]);
    for (const auto id : clipIds)
    {
        auto& preview = previews[id];
        if (preview.map.has_value() && *preview.map == map)
            continue;
        preview.map = map;
        requestRebuild();
    }
}

void DawEngine::setPreviewNotes (int clipId, const std::vector<analysis::pitch::Note>& notes)
{
    const auto clip = edit.getClip (clipId);
    if (! clip.isValid() || Edit::isMidiClip (clip) || ! Edit::getLinkedClips (clip).isEmpty())
        return;
    previews[clipId].notes = notes;
    requestRebuild();
}

void DawEngine::cancelPreview (int clipId)
{
    if (clipId < 0)
    {
        if (previews.empty())
            return;
        previews.clear();
        requestRebuild();
        return;
    }

    const auto clip = edit.getClip (clipId);
    auto clipIds = juce::Array<int> { clipId };
    if (clip.isValid())
        for (const auto& partner : Edit::getLinkedClips (clip))
            clipIds.add ((int) partner[ids::id]);
    for (const auto id : clipIds)
        if (previews.erase (id) > 0)
            requestRebuild();
}

warp::Settings DawEngine::getPlaybackSettings (const juce::ValueTree& clip) const
{
    auto settings = Edit::getWarpSettings (clip);
    const auto it = previews.find ((int) clip[ids::id]);
    if (it == previews.end())
        return settings;

    const auto bypass = (bool) clip.getProperty (ids::warpBypass, false);
    if (it->second.map.has_value())
        settings.map = *it->second.map;
    if (it->second.notes.has_value() && ! bypass && settings.linked.isEmpty())
        settings.notes = *it->second.notes;

    // Neutral now (or no longer): linked partners render together only when warped.
    if (settings.isNeutral())
        settings.linked.clear();
    else if (settings.linked.isEmpty())
        if (const auto partners = Edit::getLinkedClips (clip); ! partners.isEmpty())
        {
            settings.linked.add (clip[ids::file].toString());
            for (const auto& partner : partners)
                settings.linked.addIfNotAlreadyThere (partner[ids::file].toString());
            settings.linked.sort (false);
        }
    return settings;
}

void DawEngine::setStripTargets (Strip& strip, const juce::ValueTree& tree, bool audible, bool jump)
{
    const auto gain = audible ? volumeToGain (tree[ids::volume]) : 0.0f;
    const auto pan = juce::jlimit (-1.0f, 1.0f, (float) tree.getProperty (ids::pan, 0.0f));
    const auto [left, right] = balanceGains (pan);

    strip.targetLeft = gain * left;
    strip.targetRight = gain * right;
    strip.targetGain = gain;

    strip.chain.setGains ((float) tree.getProperty (ids::inGain, 0.0f), (float) tree.getProperty (ids::outGain, 0.0f));

    if (jump)
    {
        strip.left.reset (sampleRate, smoothingSeconds);
        strip.right.reset (sampleRate, smoothingSeconds);
        strip.direct.reset (sampleRate, smoothingSeconds);
        strip.left.setCurrentAndTargetValue (strip.targetLeft);
        strip.right.setCurrentAndTargetValue (strip.targetRight);
        strip.direct.setCurrentAndTargetValue (strip.targetDirect);
    }
}

void DawEngine::updateMix()
{
    const auto tracks = edit.getTracks();
    const auto returns = edit.getReturns();
    const auto soloed = [] (const auto& t) { return (bool) t[ids::solo]; };
    const auto anyTrackSolo = std::any_of (tracks.begin(), tracks.end(), soloed);
    const auto anyReturnSolo = std::any_of (returns.begin(), returns.end(), soloed);

    // Solo: soloing tracks silences the other tracks but keeps the returns, so the
    // soloed tracks' reverb is still heard. Soloing a return silences the other returns
    // and what the tracks play straight to the master; their sends still feed it.
    for (const auto& track : tracks)
    {
        const auto trackId = (int) track[ids::id];
        const auto audible = ! (bool) track[ids::mute] && (! anyTrackSolo || (bool) track[ids::solo]);

        if (const auto it = strips.find (trackId); it != strips.end())
        {
            setStripTargets (*it->second, track, audible, false);
            it->second->targetDirect = Edit::getTrackOutput (track) != Edit::masterId || ! anyReturnSolo || (bool) track[ids::solo] ? 1.0f : 0.0f;
            // MIDI tracks play the MIDI input when armed too, so you hear what you record.
            // Auto monitoring hears the input only while the track is armed.
            it->second->monitoring = (bool) track[ids::monitor] || ((bool) track[ids::arm] && (Edit::isMidiTrack (track) || (bool) track.getProperty (ids::monitorAuto, false)));
            const auto input = Edit::getTrackInput (track);
            it->second->inputLeft = input.left;
            it->second->inputRight = input.right;
        }

        for (const auto& send : track)
        {
            if (! send.hasType (ids::SEND))
                continue;

            if (const auto it = sendLevels.find ({ trackId, (int) send[ids::target] }); it != sendLevels.end())
            {
                const auto db = (float) send.getProperty (ids::level, -100.0f);
                it->second->target = audible && db >= Edit::minSendDb ? juce::Decibels::decibelsToGain (db) : 0.0f;
                it->second->audible = audible;
            }
        }
    }

    for (const auto& bus : returns)
    {
        const auto audible = ! (bool) bus[ids::mute] && (! anyReturnSolo || (bool) bus[ids::solo]);

        if (const auto it = strips.find ((int) bus[ids::id]); it != strips.end())
            setStripTargets (*it->second, bus, audible, false);

        for (const auto& send : bus)
            if (send.hasType (ids::SEND))
                if (const auto it = sendLevels.find ({ (int) bus[ids::id], (int) send[ids::target] }); it != sendLevels.end())
                {
                    const auto db = (float) send.getProperty (ids::level, -100.0f);
                    it->second->target = audible && db >= Edit::minSendDb ? juce::Decibels::decibelsToGain (db) : 0.0f;
                }
    }

    if (const auto it = strips.find (Edit::masterId); it != strips.end())
        setStripTargets (*it->second, edit.getMaster(), true, false);
}

void DawEngine::loadInsertFile (fx::Module& module, const juce::ValueTree& insert)
{
    if (! module.usesFile())
        return;

    const auto path = insert[ids::file].toString();
    const auto file = juce::File::isAbsolutePath (path) ? juce::File (path) : juce::File();
    if (file == module.getFile())
        return;

    if (const auto error = module.loadFile (file); error.isNotEmpty())
        juce::Logger::writeToLog ("[MicFX DAW] " + error);
}

std::shared_ptr<fx::Module> DawEngine::takeRunningInsert (const juce::ValueTree& insert)
{
    const auto it = runningInserts.find ((int) insert[ids::id]);
    if (it == runningInserts.end() || it->second.tree != insert)
        return nullptr;

    auto module = std::move (it->second.module);
    runningInserts.erase (it);
    return module;
}

// An insert's type and id (a plug-in's too, a rack's chains and theirs): a different
// layout needs new modules.
juce::String DawEngine::describeLayout (const juce::ValueTree& insert)
{
    juce::String layout;
    layout << insert[ids::type].toString() << ":" << (int) insert[ids::id] << " ";
    if (Edit::isPluginInsert (insert))
        layout << insert[ids::plugin].toString() << " ";
    if (Edit::isRackInsert (insert))
    {
        layout << "[ ";
        for (const auto& chain : insert)
        {
            for (const auto& member : chain.getChildWithName (ids::INSERTS))
                layout << describeLayout (member);
            layout << "| ";
        }
        layout << "] ";
    }
    return layout;
}

std::shared_ptr<DawEngine::Strip> DawEngine::getStrip (const juce::ValueTree& trackOrMaster)
{
    const auto id = (int) trackOrMaster[ids::id];
    const auto inserts = Edit::getInserts (trackOrMaster);

    juce::String layout;
    for (const auto& insert : inserts)
        layout << describeLayout (insert);

    const auto isMidi = trackOrMaster.hasType (ids::TRACK) && Edit::isMidiTrack (trackOrMaster);
    const auto instrumentTree = isMidi ? trackOrMaster.getChildWithName (ids::INSTRUMENT) : juce::ValueTree();

    if (isMidi)
        layout << "| instrument:" << (int) instrumentTree[ids::id] << ":" << instrumentTree[ids::plugin].toString();

    if (const auto it = strips.find (id); it != strips.end() && it->second->insertLayout == layout)
        return it->second;

    // New track, or its inserts were added, removed or reordered: a new chain. (Insert
    // values alone never get here; they go straight into the running modules.)
    auto strip = std::make_shared<Strip>();
    strip->id = id;
    strip->insertLayout = layout;

    // The chain this one replaces, if any: an insert that's the same INSERT takes over its
    // running module (above); one only like it (same id and type, e.g. the project was
    // opened again) hands over what it has learnt.
    const auto previous = strips.find (id);
    const auto* previousModules = previous != strips.end() ? &previous->second->modulesByInsertId : nullptr;

    // Each insert's module into a chain (a rack's chains too, recursively). `members`: the
    // rack being built, which keeps its modules so a rebuild can keep the rack running.
    std::function<void (fx::FxChain&, const juce::ValueTree&, RackInsert*)> addInserts;
    const auto keep = [&] (int insertId, fx::FxChain& chain, fx::Module& module, const juce::ValueTree& insert, RackInsert* members)
    {
        strip->modulesByInsertId[insertId] = &module;
        strip->insertTrees[insertId] = insert;
        if (members != nullptr)
            members->members.push_back ({ insertId, chain.share (module), insert });
        if (auto* pitch = dynamic_cast<fx::modules::Pitch*> (&module))
            strip->pitches.push_back (pitch);
    };

    addInserts = [&] (fx::FxChain& chain, const juce::ValueTree& list, RackInsert* members)
    {
    for (const auto& insert : list)
    {
        const auto insertId = (int) insert[ids::id];

        if (Edit::isRackInsert (insert))
        {
            const auto rackLayout = describeLayout (insert);

            // The same rack, nothing changed inside: it carries on, chains and all.
            if (auto running = std::dynamic_pointer_cast<RackInsert> (takeRunningInsert (insert)); running != nullptr && running->layout == rackLayout)
            {
                auto& added = chain.add (running, true);
                keep (insertId, chain, added, insert, members);
                for (const auto& member : running->members)
                {
                    takeRunningInsert (member.tree); // it's spoken for
                    strip->modulesByInsertId[member.insertId] = member.module.get();
                    strip->insertTrees[member.insertId] = member.tree;
                    if (auto* pitch = dynamic_cast<fx::modules::Pitch*> (member.module.get()))
                        strip->pitches.push_back (pitch);
                    if (auto* plugin = dynamic_cast<PluginInsert*> (member.module.get()))
                        strip->plugins[member.insertId] = { plugin, member.tree, {} };
                    if (members != nullptr)
                        members->members.push_back (member);
                }
                continue;
            }

            auto rack = std::make_unique<RackInsert> (insert.getNumChildren());
            for (int v = 0; v < rack->getNumValues(); ++v)
                if (const auto& value = insert[juce::Identifier (rack->getValueId (v))]; ! value.isVoid())
                    rack->getValue (v).store ((float) value);
            rack->layout = rackLayout;
            for (int c = 0; c < rack->getNumChains(); ++c)
                addInserts (rack->getChain (c), insert.getChild (c).getChildWithName (ids::INSERTS), rack.get());
            auto* built = rack.get();
            auto& added = chain.add (std::move (rack));
            keep (insertId, chain, added, insert, members);
            if (members != nullptr)
                for (const auto& member : built->members)
                    members->members.push_back (member);
            continue;
        }

        if (Edit::isPluginInsert (insert))
        {
            // An effect plugin: the one already running for this insert if there is one (so
            // it keeps everything it has in memory), else loaded from its saved state. One
            // that can't load is left out of the chain and the signal goes on past it.
            auto& slot = strip->plugins[insertId];
            slot.tree = insert;

            // The very module running for this insert (on this track or, moved, on another),
            // so even its bypass delay goes on; else another module on the same plugin.
            if (auto running = std::dynamic_pointer_cast<PluginInsert> (takeRunningInsert (insert)); running != nullptr)
            {
                auto& added = chain.add (std::move (running), true);
                slot.module = static_cast<PluginInsert*> (&added);
                keep (insertId, chain, added, insert, members);
                continue;
            }

            std::unique_ptr<PluginInsert> module;

            if (previous != strips.end())
                if (const auto it = previous->second->plugins.find (insertId); it != previous->second->plugins.end() && it->second.module != nullptr)
                    module = PluginInsert::share (*it->second.module);

            if (module == nullptr)
                module = PluginInsert::create (insert, sampleRate, blockSize, realtime, slot.error);

            if (module == nullptr)
            {
                juce::Logger::writeToLog ("[MicFX DAW] effect plugin: " + slot.error);
                continue;
            }

            auto& added = chain.add (std::move (module));
            for (int v = 0; v < added.getNumValues(); ++v)
                if (const auto& value = insert[juce::Identifier (added.getValueId (v))]; ! value.isVoid())
                    added.getValue (v).store ((float) value);

            slot.module = static_cast<PluginInsert*> (&added);
            keep (insertId, chain, added, insert, members);
            continue;
        }

        const auto type = Edit::moduleTypeFromKey (insert[ids::type].toString());
        if (! type.has_value())
            continue; // a module from a newer version: skipped rather than refusing the project

        // Still there from the last graph: the same module carries on, mid-tail, with its
        // values already in (they're written straight into running modules).
        if (auto running = takeRunningInsert (insert); running != nullptr && running->getType() == *type)
        {
            auto& module = chain.add (std::move (running), true);
            keep (insertId, chain, module, insert, members);
            continue;
        }

        auto& module = chain.add (fx::createModule (*type));

        for (int v = 0; v < module.getNumValues(); ++v)
            if (const auto& value = insert[juce::Identifier (module.getValueId (v))]; ! value.isVoid())
                module.getValue (v).store ((float) value);

        if (previousModules != nullptr)
            if (const auto it = previousModules->find ((int) insert[ids::id]); it != previousModules->end() && it->second->getType() == *type)
                module.carryStateFrom (*it->second);

        if (module.usesFile())
            loadInsertFile (module, insert); // before prepare, so it plays from the first block

        keep (insertId, chain, module, insert, members);
    }
    };
    addInserts (strip->chain, inserts, nullptr);


    strip->chain.setGains ((float) trackOrMaster.getProperty (ids::inGain, 0.0f), (float) trackOrMaster.getProperty (ids::outGain, 0.0f));
    strip->chain.prepare ({ sampleRate, (juce::uint32) blockSize, 2 }); // starts at those gains
    strip->buffer.setSize (2, blockSize);
    strip->isMidi = isMidi;

    if (instrumentTree.isValid())
    {
        strip->instrumentTree = instrumentTree;
        strip->instrument = instruments::create (instrumentTree, sampleRate, blockSize, realtime, strip->instrumentError);

        if (auto* instrument = strip->instrument.get())
        {
            const auto channels = juce::jmax (2, instrument->getTotalNumInputChannels(), instrument->getTotalNumOutputChannels());
            strip->instrumentBuffer.setSize (channels, blockSize);
            strip->midi.ensureSize (4096);
            strip->audition.ensureSize (1024);
        }
        else
        {
            juce::Logger::writeToLog ("[MicFX DAW] instrument: " + strip->instrumentError);
        }
    }
    setStripTargets (*strip, trackOrMaster, true, true);

    strips[id] = strip;
    return strip;
}

void DawEngine::rebuild()
{
    auto graph = std::make_unique<Graph>();
    graph->blockSize = blockSize;

    if (chordFollower == nullptr)
    {
        chordFollower = std::make_shared<analysis::ChordFollower>();
        chordFollower->prepare (sampleRate);
    }
    graph->chords = chordFollower;
    graph->listen.setSize (2, blockSize);
    graph->keyMask = analysis::getKeyMask (edit.getKey(), edit.isKeyMinor());
    std::map<int, std::shared_ptr<Strip>> previous;
    std::swap (previous, strips);

    // Every running insert module, for the new chains to take over (see getStrip).
    runningInserts.clear();
    for (const auto& [stripId, strip] : previous)
    {
        for (const auto& [insertId, module] : strip->modulesByInsertId)
            if (const auto tree = strip->insertTrees.find (insertId); tree != strip->insertTrees.end())
                if (auto shared = strip->chain.share (*module); shared != nullptr)
                    runningInserts[insertId] = { std::move (shared), tree->second };
        // The modules inside racks (the strip's chain only holds the racks).
        for (int m = 0; m < strip->chain.size(); ++m)
            if (auto* rack = dynamic_cast<RackInsert*> (&strip->chain[m]))
                for (const auto& member : rack->members)
                    runningInserts[member.insertId] = { member.module, member.tree };
    }

    // Keep the strips that still match (moved back from `previous` as they're found).
    const auto reuse = [&] (const juce::ValueTree& tree)
    {
        const auto id = (int) tree[ids::id];
        if (const auto it = previous.find (id); it != previous.end())
            strips[id] = it->second;
        return getStrip (tree);
    };

    graph->master = reuse (edit.getMaster());
    graph->tempo = edit.getTempoMap();

    // Pattern mode: just the pattern, as a loop (a removed pattern goes back to the song).
    auto loopPattern = patternMode >= 0 ? edit.getPattern (patternMode) : juce::ValueTree();
    if (patternMode >= 0 && ! loopPattern.isValid())
    {
        patternMode = -1;
        patternLoopEnd = 0;
        position = songPosition;
    }

    std::map<int, int> busIndex; // return id -> index in graph->returns
    // Group tracks first: they send into the plain returns, which must come later.
    auto returnTrees = edit.getReturns();
    std::stable_partition (returnTrees.begin(), returnTrees.end(), [] (const auto& tree) { return Edit::isGroup (tree); });

    for (const auto& returnTree : returnTrees)
    {
        busIndex[(int) returnTree[ids::id]] = (int) graph->returns.size();
        graph->returns.push_back ({ reuse (returnTree), {}, {} });
    }

    std::map<std::pair<int, int>, std::shared_ptr<SendLevel>> previousSends;
    std::swap (previousSends, sendLevels);
    std::vector<std::shared_ptr<SendLevel>> newSends; // start at their level instead of fading in

    for (const auto& returnTree : returnTrees)
    {
        if (! Edit::isGroup (returnTree))
            continue;

        const auto groupId = (int) returnTree[ids::id];
        auto& bus = graph->returns[(size_t) busIndex[groupId]];

        for (const auto& sendTree : returnTree)
        {
            if (! sendTree.hasType (ids::SEND))
                continue;

            const auto target = (int) sendTree[ids::target];
            const auto found = busIndex.find (target);
            if (found == busIndex.end() || Edit::isGroup (edit.getReturn (target)))
                continue;

            const std::pair key { groupId, target };
            auto level = previousSends.count (key) > 0 ? previousSends[key] : nullptr;
            if (level == nullptr)
            {
                level = std::make_shared<SendLevel>();
                newSends.push_back (level);
            }

            sendLevels[key] = level;
            bus.sends.push_back ({ level, found->second, (bool) sendTree.getProperty (ids::pre, false) });
        }
    }

    juce::int64 end = 0;
    int maxLatency = 0;
    std::set<juce::String> warpsInUse;
    std::map<int, AudioPool::Audio> clipAudio;
    std::map<int, warp::Map> clipMaps;

    for (const auto& trackTree : edit.getTracks())
    {
        Track track;
        track.strip = reuse (trackTree);
        maxLatency = juce::jmax (maxLatency, track.strip->getLatency());

        const auto trackId = (int) trackTree[ids::id];

        if (const auto it = busIndex.find (Edit::getTrackOutput (trackTree)); it != busIndex.end())
            track.output = it->second;

        for (const auto& sendTree : trackTree)
        {
            if (! sendTree.hasType (ids::SEND))
                continue;

            const auto target = (int) sendTree[ids::target];
            const auto bus = busIndex.find (target);
            if (bus == busIndex.end() || bus->second == track.output)
                continue; // a missing return, or the one it already plays into

            const std::pair key { trackId, target };
            auto level = previousSends.count (key) > 0 ? previousSends[key] : nullptr;

            if (level == nullptr)
            {
                level = std::make_shared<SendLevel>();
                newSends.push_back (level);
            }

            sendLevels[key] = level;
            Send send { level, bus->second, (bool) sendTree.getProperty (ids::pre, false), {}, getHold (trackId, "send:" + juce::String (target)) };
            for (const auto& point : Edit::parsePoints (sendTree[ids::automation].toString()))
                send.points.emplace_back ((juce::int64) std::llround (point.time * sampleRate), point.value);
            track.sends.push_back (std::move (send));
        }

        if (track.strip->isMidi)
            buildMidiEvents (track, trackTree, sampleRate, end, graph->tempo, loopPattern);

        for (const auto& point : Edit::getVolumeRide (trackTree))
            track.ride.emplace_back ((juce::int64) std::llround (point.time * sampleRate), point.db);

        for (const auto& point : Edit::getLane (trackTree, "pan"))
            track.pan.emplace_back ((juce::int64) std::llround (point.time * sampleRate), point.value);
        if (! track.pan.empty())
            track.panHeld = getHold (trackId, "pan");

        // Effect automation: the values' own lanes, then the values that follow a macro
        // with a lane (unless they have their own).
        std::set<std::pair<int, juce::String>> automated;
        const auto addLane = [&] (const juce::ValueTree& insert, const juce::String& valueId, const auto& points, auto&& map, const juce::String& holdTarget)
        {
            const auto module = track.strip->modulesByInsertId.find ((int) insert[ids::id]);
            if (module == track.strip->modulesByInsertId.end())
                return;
            Track::Lane lane;
            lane.value = module->second->findValue (valueId);
            if (lane.value == nullptr)
                return;
            lane.base = (float) insert.getProperty (juce::Identifier (valueId), 0.0f);
            lane.held = getHold (trackId, holdTarget);
            for (const auto& point : points)
                lane.points.emplace_back ((juce::int64) std::llround (point.time * sampleRate), map (point.value));
            track.lanes.push_back (std::move (lane));
            automated.insert ({ (int) insert[ids::id], valueId });
        };
        const auto inserts = Edit::getAllInserts (trackTree);
        for (const auto& insert : inserts)
            for (const auto& [valueId, points] : Edit::getInsertAutomation (insert))
                addLane (insert, valueId, points, [] (float v) { return v; }, "insert:" + juce::String ((int) insert[ids::id]) + ":" + valueId);
        if (const auto macros = Edit::getMacros (trackTree); macros.isValid())
            for (const auto& macro : macros)
            {
                const auto points = Edit::parsePoints (macro[ids::automation].toString());
                if (points.empty())
                    continue;
                for (const auto& map : macro)
                {
                    const auto insertId = (int) map[ids::insert];
                    const auto param = map[ids::param].toString();
                    if (automated.count ({ insertId, param }) > 0)
                        continue;
                    for (const auto& insert : inserts)
                        if ((int) insert[ids::id] == insertId)
                            addLane (insert, param, points, [map] (float v) { return Edit::macroTarget (map, v); }, "macro:" + macro[ids::index].toString());
                }
            }

        // Auditioning a take: its lane plays instead of the comp (live only: exports and
        // Auto Mix always hear the comp).
        auto clipSource = trackTree;
        if (const auto listen = Edit::getListenTake (trackTree); listen >= 0 && realtime)
            for (const auto& child : trackTree)
                if (child.hasType (ids::TAKE) && (int) child[ids::id] == listen)
                    clipSource = child;

        for (const auto& clipTree : clipSource)
        {
            if (! clipTree.hasType (ids::CLIP) || Edit::isMidiClip (clipTree))
                continue;

            Clip clip;
            const juce::File file (clipTree[ids::file].toString());
            const auto warpSettings = getPlaybackSettings (clipTree);
            const auto clipId = (int) clipTree[ids::id];
            const auto toSeconds = [] (const juce::var& v) { return (double) v; };

            // Where the playhead is in this clip's copy, so what's heard next renders first;
            // and what it plays now, to stand in while a pitch change renders.
            AudioPool::Hint hint;
            if (realtime)
            {
                const auto now = (double) position.load() / sampleRate;
                const auto clipStart = toSeconds (clipTree[ids::start]), clipOffset = toSeconds (clipTree[ids::offset]);
                if (now < clipStart + toSeconds (clipTree[ids::length]))
                    hint.focus = juce::jmax (0.0, clipOffset + juce::jmax (0.0, now - clipStart));
                if (const auto it = lastClipMap.find (clipId); it != lastClipMap.end() && it->second == warpSettings.map)
                    if (const auto audio = lastClipAudio.find (clipId); audio != lastClipAudio.end())
                        hint.previous = audio->second;
            }
            clip.audio = realtime ? pool.getForPlayback (file, warpSettings, hint) : pool.get (file, warpSettings);
            auto playedMap = warpSettings.map;

            if (! warpSettings.isNeutral())
                warpsInUse.insert (AudioPool::getWarpKey (file, warpSettings));

            if (clip.audio == nullptr)
            {
                // A new warp or transpose is rendering: keep playing what the clip played
                // until now, rather than dropping out.
                if (const auto it = lastClipAudio.find (clipId); it != lastClipAudio.end())
                {
                    clip.audio = it->second;
                    if (const auto map = lastClipMap.find (clipId); map != lastClipMap.end())
                        playedMap = map->second;
                }
                else
                    continue; // still loading (rebuilds when it's ready) or unreadable
            }

            clipAudio[clipId] = clip.audio;
            clipMaps[clipId] = playedMap;

            const auto toSamples = [this] (const juce::var& seconds) { return (juce::int64) std::llround ((double) seconds * sampleRate); };
            clip.start   = toSamples (clipTree[ids::start]);
            clip.offset  = juce::jmax ((juce::int64) 0, toSamples (clipTree[ids::offset]));
            clip.length  = juce::jmin (toSamples (clipTree[ids::length]), (juce::int64) clip.audio->getNumSamples() - clip.offset);
            clip.fadeIn  = juce::jmax ((juce::int64) 0, toSamples (clipTree[ids::fadeIn]));
            clip.fadeOut = juce::jmax ((juce::int64) 0, toSamples (clipTree[ids::fadeOut]));
            clip.gain    = volumeToGain (clipTree.getProperty (ids::gain, 0.0f));

            // The track's phase: upside down, and/or a little later or earlier (an earlier
            // clip at the session's start loses the bit that would play before it).
            if (Edit::getPolarity (trackTree))
                clip.gain = -clip.gain;
            if (const auto nudge = (juce::int64) std::llround (Edit::getNudge (trackTree) / 1000.0 * sampleRate); nudge != 0)
            {
                clip.start += nudge;
                if (clip.start < 0)
                {
                    const auto cut = -clip.start;
                    clip.offset += cut;
                    clip.length -= cut;
                    clip.fadeIn = juce::jmax ((juce::int64) 0, clip.fadeIn - cut);
                    clip.start = 0;
                }
                clip.length = juce::jmin (clip.length, (juce::int64) clip.audio->getNumSamples() - clip.offset);
            }

            if (clip.length <= 0 || loopPattern.isValid())
                continue; // pattern mode: no clips (their audio stays loaded for the song)

            end = juce::jmax (end, clip.start + clip.length);

            if ((bool) clipTree.getProperty (ids::mute, false))
                continue; // muted: still part of the song's length, but silent

            track.clips.push_back (std::move (clip));
        }

        // Anti-click: a clip edge that cuts into the file's audio gets a short fade, unless
        // the next part of the same audio carries straight on there (a plain split stays
        // seamless).
        {
            const auto edgeFade = (juce::int64) std::llround (antiClickSeconds * sampleRate);
            auto& clips = track.clips;

            const auto continues = [&] (const Clip& a, const Clip& b) // b picks up where a stops
            {
                return a.audio == b.audio && b.start == a.start + a.length && b.offset == a.offset + a.length;
            };

            for (auto& clip : clips)
            {
                const auto cutsIn = clip.offset > 0
                    && std::none_of (clips.begin(), clips.end(), [&] (const Clip& other) { return continues (other, clip); });
                const auto cutsOut = clip.offset + clip.length < (juce::int64) clip.audio->getNumSamples()
                    && std::none_of (clips.begin(), clips.end(), [&] (const Clip& other) { return continues (clip, other); });
                const auto most = clip.length / 2;

                if (cutsIn)  clip.fadeIn  = juce::jmin (most, juce::jmax (clip.fadeIn, edgeFade));
                if (cutsOut) clip.fadeOut = juce::jmin (most, juce::jmax (clip.fadeOut, edgeFade));
            }
        }

        graph->tracks.push_back (std::move (track));
    }

    // Delay every track to line up with the slowest chain.
    for (auto& track : graph->tracks)
    {
        track.strip->latencyAtBuild = track.strip->getLatency();
        track.delay.setDelay (maxLatency - track.strip->latencyAtBuild);
    }

    // And every return with the slowest return; the tracks' direct sound waits for them.
    int maxReturnLatency = 0;
    for (auto& bus : graph->returns)
    {
        bus.strip->latencyAtBuild = bus.strip->getLatency();
        maxReturnLatency = juce::jmax (maxReturnLatency, bus.strip->latencyAtBuild);
    }

    for (auto& bus : graph->returns)
        bus.delay.setDelay (maxReturnLatency - bus.strip->latencyAtBuild);

    graph->directDelay.setDelay (maxReturnLatency);
    if (maxReturnLatency > 0)
        graph->direct.setSize (2, blockSize);

    lastClipAudio = std::move (clipAudio);
    lastClipMap = std::move (clipMaps);
    pool.releaseUnusedWarps (warpsInUse);

    if (loopPattern.isValid())
    {
        end = juce::jmax ((juce::int64) 1, (juce::int64) std::llround (patterns::getLoopSeconds (loopPattern, graph->tempo) * sampleRate));
        patternLoopEnd = end;
    }
    else
    {
        end = juce::jmax (end, (juce::int64) std::llround (patterns::getBlocksEnd (edit.getState(), graph->tempo) * sampleRate));
    }

    length = end;
    graph->master->latencyAtBuild = graph->master->chain.getLatencySamples();
    latency = maxLatency + maxReturnLatency + graph->master->latencyAtBuild;

    updateMix();

    for (auto& level : newSends)
    {
        level->gain.reset (sampleRate, smoothingSeconds);
        level->gain.setCurrentAndTargetValue (level->target.load());
    }

    runningInserts.clear(); // removed inserts: their modules go with the old graph
    publish (std::move (graph));
    ++rebuildCount;
    rebuildPending = false; // what loaded while it built is in this graph
}

void DawEngine::buildMidiEvents (Track& track, const juce::ValueTree& trackTree, double rate, juce::int64& end,
                                 const TempoMap& tempo, const juce::ValueTree& loopPattern)
{
    const auto toSamples = [rate] (double seconds) { return (juce::int64) std::llround (seconds * rate); };
    const auto transpose = (int) std::lround ((double) trackTree.getProperty (ids::transpose, 0.0));
    const auto trackId = (int) trackTree[ids::id];

    // Pattern notes (blocks on the timeline, or the pattern mode's loop).
    const auto patternEvents = loopPattern.isValid() ? patterns::getLoopEvents (loopPattern, trackId, tempo)
                                                     : patterns::getBlockEvents (trackTree.getParent(), trackId, tempo);
    for (const auto& e : patternEvents)
    {
        const auto on = toSamples (e.on);
        const auto pitch = juce::jlimit (0, 127, e.pitch + transpose);
        track.midiEvents.push_back ({ on, pitch, e.velocity });
        track.midiEvents.push_back ({ juce::jmax (on + 1, toSamples (e.off)), pitch, 0 });
    }

    for (const auto& clip : trackTree)
    {
        if (loopPattern.isValid())
            break; // pattern mode: the pattern only
        if (! clip.hasType (ids::CLIP) || ! Edit::isMidiClip (clip))
            continue;

        const auto start = (double) clip[ids::start], offset = (double) clip[ids::offset], length = (double) clip[ids::length];
        if (length <= 0.0)
            continue;

        end = juce::jmax (end, toSamples (start + length));

        if ((bool) clip.getProperty (ids::mute, false))
            continue;

        const auto clipEnd = toSamples (start + length);

        for (const auto& note : clip)
        {
            if (! note.hasType (ids::NOTE))
                continue;

            // Only notes that start inside the clip's window play; they end at its edge.
            const auto time = (double) note[ids::time];
            if (time < offset || time >= offset + length)
                continue;

            const auto on = toSamples (start + time - offset);
            const auto off = juce::jmin (clipEnd, toSamples (start + time - offset + (double) note[ids::length]));
            // The track's (and clip's) transpose moves MIDI notes too.
            const auto pitch = juce::jlimit (0, 127, (int) note[ids::pitch] + transpose + (int) std::lround ((double) clip.getProperty (ids::transpose, 0.0)));

            track.midiEvents.push_back ({ on, pitch, juce::jlimit (1, 127, (int) note.getProperty (ids::velocity, 100)) });
            track.midiEvents.push_back ({ juce::jmax (on + 1, off), pitch, 0 });
        }
    }

    // In time order; at the same time, note-offs first so a repeated note restarts.
    std::sort (track.midiEvents.begin(), track.midiEvents.end(), [] (const MidiEvent& a, const MidiEvent& b)
    {
        return a.time != b.time ? a.time < b.time : a.velocity < b.velocity;
    });
}

//==============================================================================
void DawEngine::publish (std::unique_ptr<Graph> graph)
{
    // A graph the audio thread hasn't picked up yet was never used: free it here.
    delete incoming.exchange (graph.release(), std::memory_order_acq_rel);
    collectGarbage();
}

//==============================================================================
void DawEngine::startPreview (juce::AudioBuffer<float>&& audio, float gain, bool loop, int delaySamples)
{
    collectPreviews();
    setPreviewGain (gain);
    previewStop = false;

    if (! realtime || audio.getNumSamples() == 0)
        return;

    previewPlaying = true;
    previewLoopIncoming = loop;
    previewDelayIncoming = juce::jmax (0, delaySamples);
    // One the audio thread hasn't picked up yet was never played: free it here.
    delete previewIncoming.exchange (new juce::AudioBuffer<float> (std::move (audio)), std::memory_order_acq_rel);
}

void DawEngine::stopPreview()
{
    delete previewIncoming.exchange (nullptr, std::memory_order_acq_rel);
    previewStop = true;
    previewPlaying = false;
    collectPreviews();
}

void DawEngine::collectPreviews()
{
    const auto scope = previewRetiredFifo.read (previewRetiredFifo.getNumReady());
    scope.forEach ([this] (int index)
    {
        delete previewRetired[(size_t) index];
        previewRetired[(size_t) index] = nullptr;
    });
}

void DawEngine::retirePreview (juce::AudioBuffer<float>* audio) noexcept
{
    if (audio == nullptr)
        return;

    const auto scope = previewRetiredFifo.write (1);
    jassert (scope.blockSize1 == 1); // the message thread isn't freeing old previews

    if (scope.blockSize1 == 1)
        previewRetired[(size_t) scope.startIndex1] = audio;
}

void DawEngine::renderPreview (juce::AudioBuffer<float>& output, int numChannels, int numSamples) noexcept
{
    if (auto* next = previewIncoming.exchange (nullptr, std::memory_order_acq_rel))
    {
        retirePreview (previewActive);
        previewActive = next;
        previewPosition = 0;
        previewLooping = previewLoopIncoming.load();
        previewDelay = previewDelayIncoming.load();
    }

    if (previewStop.exchange (false))
    {
        retirePreview (previewActive);
        previewActive = nullptr;
    }

    if (previewActive == nullptr)
        return;

    const auto gain = previewGain.load();
    const auto sourceChannels = previewActive->getNumChannels();
    const auto length = previewActive->getNumSamples();
    auto at = juce::jmin (previewDelay, numSamples);
    previewDelay -= at;

    while (at < numSamples && previewActive != nullptr)
    {
        const auto count = juce::jmin (numSamples - at, length - previewPosition);
        for (int ch = 0; ch < numChannels && count > 0; ++ch)
            output.addFrom (ch, at, *previewActive, juce::jmin (ch, sourceChannels - 1), previewPosition, count, gain);

        previewPosition += juce::jmax (0, count);
        at += juce::jmax (0, count);

        if (previewPosition >= length)
        {
            if (previewLooping && length > 0)
            {
                previewPosition = 0;
                continue;
            }
            retirePreview (previewActive);
            previewActive = nullptr;
            previewPlaying = false;
        }
    }
}

void DawEngine::collectGarbage()
{
    const auto scope = retiredFifo.read (retiredFifo.getNumReady());
    scope.forEach ([this] (int index)
    {
        delete retired[(size_t) index];
        retired[(size_t) index] = nullptr;
    });
}

//==============================================================================
void DawEngine::updateTempo()
{
    beatsPerBar = edit.getBeatsPerBar();
    updateLoop();
}

void DawEngine::updateLoop()
{
    const auto& project = edit.getState();
    const auto start = (juce::int64) std::llround ((double) project.getProperty (ids::loopStart, 0.0) * sampleRate);
    const auto end = (juce::int64) std::llround ((double) project.getProperty (ids::loopEnd, 0.0) * sampleRate);
    loopStart = start;
    loopEnd = end;
    loopOn = (bool) project.getProperty (ids::loopOn, false) && end > start;
}

juce::String DawEngine::startRecording (const juce::File& folder)
{
    if (isRecording())
        return "Already recording.";

    const auto tracks = edit.getTracks();
    const auto armedAudio = std::any_of (tracks.begin(), tracks.end(), [] (const auto& t) { return (bool) t[ids::arm] && ! Edit::isMidiTrack (t); });
    const auto armedMidi = std::any_of (tracks.begin(), tracks.end(), [] (const auto& t) { return (bool) t[ids::arm] && Edit::isMidiTrack (t); });

    if (! armedAudio && ! armedMidi)
        return "Arm a track to record on first.";

    // Pattern mode records MIDI into the pattern only: armed audio tracks sit it out.
    const auto inPattern = patternMode >= 0;
    if (inPattern && ! armedMidi)
        return "Arm a MIDI track to record into the pattern (audio records in Song mode).";

    if (armedAudio && ! inPattern)
    {
        // One take per input the armed tracks use: tracks on the same input share it.
        std::vector<Edit::Input> inputs;
        for (const auto& track : tracks)
            if ((bool) track[ids::arm] && ! Edit::isMidiTrack (track))
                if (const auto input = Edit::getTrackInput (track); input.left != Edit::noInput
                      && std::find (inputs.begin(), inputs.end(), input) == inputs.end())
                    inputs.push_back (input);

        if (inputs.empty())
            return "The armed tracks have no input. Pick one in the track's Input.";
        if ((int) inputs.size() > maxTakes)
            return "Too many different inputs to record at once (" + juce::String (maxTakes) + " at most).";

        for (size_t i = 0; i < inputs.size(); ++i)
        {
            auto& take = takes[i];
            if (take.recorder == nullptr)
                take.recorder = std::make_unique<Recorder>();

            take.input = inputs[i];
            take.recorder->prepare (sampleRate, inputs[i].isStereo() ? 2 : 1);

            const auto label = "In " + juce::String (inputs[i].left + 1) + (inputs[i].isStereo() ? "-" + juce::String (inputs[i].right + 1) : juce::String());
            if (const auto error = take.recorder->start (folder, false, inputs.size() > 1 ? label : juce::String()); error.isNotEmpty())
            {
                for (size_t j = 0; j < i; ++j)
                {
                    takes[j].on = false;
                    takes[j].recorder->stop().file.deleteFile();
                }
                return error;
            }
            take.on = true;
        }

        numTakes = (int) inputs.size();
        recordingAudio = true;
    }

    drainRecordedMidi();
    recordedMidi.clear();

    if (inPattern)
    {
        patternTracks.clear();
        for (const auto& track : tracks)
            if ((bool) track[ids::arm] && Edit::isMidiTrack (track))
                patternTracks.add ((int) track[ids::id]);

        for (auto& held : patternHeld)
            held = { -1.0, 0 };
        patternSustained.reset();
        patternPedal = false;
        patternRecorded.clear();
        patternRecording = true;
    }

    recordingMidi = armedMidi;

    // Punch: records only inside the loop range, from a pre-roll before it.
    const auto punching = punch.load() && ! inPattern && loopEnd.load() > loopStart.load();
    const auto start = punching ? loopStart.load() : position.load();
    recordStart = start;
    punchEnd = punching ? loopEnd.load() : (juce::int64) -1;

    // Loop recording: starting inside the loop with it on, playback wraps as usual and
    // every pass becomes a take (audio only: MIDI takes run straight on).
    loopRecording = ! punching && armedAudio && ! armedMidi && loopOn.load() && start >= loopStart.load() && start < loopEnd.load();

    if (const auto bars = punching ? preRollBars.load() : countInBars.load(); bars > 0)
    {
        countingIn = true;
        const auto tempo = edit.getTempoMap();
        const auto startBeat = tempo.toBeats ((double) start / sampleRate);
        const auto countIn = tempo.toSeconds (startBeat) - tempo.toSeconds (startBeat - bars * beatsPerBar.load());
        position = start - (juce::int64) std::llround (countIn * sampleRate);
    }

    playing = true;
    return {};
}

void DawEngine::drainRecordedMidi()
{
    const auto scope = midiFifo.read (midiFifo.getNumReady());
    scope.forEach ([this] (int index) { recordedMidi.push_back (midiFifoData[(size_t) index]); });
}

void DawEngine::addPatternNote (int pitch, double onBeat, double offBeat, int velocity)
{
    const auto pattern = edit.getPattern (patternMode);
    if (! pattern.isValid())
        return;

    const auto length = Edit::getPatternLength (pattern);
    const auto step = Edit::getPatternStep (pattern);
    auto duration = offBeat - onBeat;
    if (duration <= 0.0)
        duration += length; // held across the loop's end

    auto time = onBeat;
    if (patternSnap)
    {
        time = std::round (onBeat / step) * step;
        duration = juce::jmax (step, std::round (duration / step) * step);
    }
    if (time >= length - 1.0e-9)
        time -= length;
    time = juce::jlimit (0.0, length, time);

    const Edit::NotUndoable notUndoable (edit);
    const auto patternId = (int) pattern[ids::id];

    for (auto trackId : patternTracks)
    {
        auto part = edit.ensurePart (patternId, trackId);
        if (! part.isValid())
            continue;

        const auto drum = (bool) part.getProperty (ids::drum, false);
        const auto tolerance = patternSnap ? step * 0.5 : 1.0 / 64.0;
        const auto taken = std::any_of (part.begin(), part.end(), [&] (const juce::ValueTree& note)
        {
            return note.hasType (ids::NOTE) && (int) note[ids::pitch] == pitch && std::abs ((double) note[ids::time] - time) < tolerance;
        });

        if (! taken)
            patternRecorded.push_back ((int) edit.addPatternNote (patternId, trackId, pitch, time, drum ? step : juce::jmax (0.01, duration), velocity)[ids::id]);
    }
}

void DawEngine::collectPatternNotes()
{
    if (! patternRecording.load())
        return;

    drainRecordedMidi();
    const auto pattern = edit.getPattern (patternMode);
    if (! pattern.isValid() || recordedMidi.empty())
    {
        recordedMidi.clear();
        return;
    }

    // Timeline times are inside the loop (0..its length): moved earlier by the output
    // latency (keys are played to what's heard) and wrapped back into it.
    const auto tempo = edit.getTempoMap();
    const auto loop = juce::jmax ((juce::int64) 1, patternLoopEnd.load());
    const auto toBeat = [&] (juce::int64 time)
    {
        auto t = (time - midiRecordingLatency.load()) % loop;
        if (t < 0)
            t += loop;
        return tempo.toBeats ((double) t / sampleRate);
    };

    const auto end = [&] (int pitch, double beat)
    {
        auto& held = patternHeld[(size_t) pitch];
        if (held.first >= 0.0)
            addPatternNote (pitch, held.first, beat, held.second);
        held = { -1.0, 0 };
        patternSustained.reset ((size_t) pitch);
    };

    for (const auto& event : recordedMidi)
    {
        const auto type = event.status & 0xf0;
        const auto beat = toBeat (event.time);

        if (type == 0x90 && event.data2 > 0)
        {
            end (event.data1, beat);
            patternHeld[event.data1] = { beat, event.data2 };
        }
        else if (type == 0x80 || type == 0x90)
        {
            if (patternPedal)
                patternSustained.set (event.data1, patternHeld[event.data1].first >= 0.0);
            else
                end (event.data1, beat);
        }
        else if (type == 0xb0 && event.data1 == 64)
        {
            patternPedal = event.data2 >= 64;
            if (! patternPedal)
                for (int p = 0; p < 128; ++p)
                    if (patternSustained[(size_t) p])
                        end (p, beat);
        }
    }

    recordedMidi.clear();
}

int DawEngine::finishPatternRecording()
{
    if (! patternRecording.load())
        return 0;

    collectPatternNotes();
    recordingMidi = false;
    patternRecording = false;
    countingIn = false;

    // Notes still held end here.
    const auto tempo = edit.getTempoMap();
    const auto loop = juce::jmax ((juce::int64) 1, patternLoopEnd.load());
    const auto now = tempo.toBeats ((double) (juce::jmax ((juce::int64) 0, position.load()) % loop) / sampleRate);
    for (int p = 0; p < 128; ++p)
        if (patternHeld[(size_t) p].first >= 0.0)
            addPatternNote (p, patternHeld[(size_t) p].first, now, patternHeld[(size_t) p].second);

    // Everything recorded becomes one undo step: taken out (unrecorded) and put back.
    struct Recorded { int pattern, track, pitch, velocity; double time, length; };
    std::vector<Recorded> notes;
    {
        const Edit::NotUndoable notUndoable (edit);
        for (auto noteId : patternRecorded)
        {
            const auto note = edit.getNote (noteId);
            const auto part = note.getParent();
            if (! note.isValid() || ! part.hasType (ids::PART))
                continue;

            notes.push_back ({ (int) part.getParent()[ids::id], (int) part[ids::track], (int) note[ids::pitch], (int) note[ids::velocity],
                               (double) note[ids::time], (double) note[ids::length] });
            edit.removeNote (noteId);
        }
    }
    patternRecorded.clear();

    if (! notes.empty())
    {
        edit.getUndoManager().beginNewTransaction ("Record pattern");
        for (const auto& n : notes)
            edit.addPatternNote (n.pattern, n.track, n.pitch, n.time, n.length, n.velocity);
        edit.getUndoManager().beginNewTransaction();
    }

    return (int) notes.size();
}

void DawEngine::syncCapture()
{
    const auto seconds = captureLength.load();
    std::vector<Edit::Input> wanted;

    if (seconds > 0.0)
        for (const auto& track : edit.getTracks())
            if ((bool) track[ids::arm] && ! Edit::isMidiTrack (track))
                if (const auto input = Edit::getTrackInput (track); input.left != Edit::noInput
                      && std::find (wanted.begin(), wanted.end(), input) == wanted.end() && (int) wanted.size() < maxCaptureInputs)
                    wanted.push_back (input);

    const auto numChannelsOf = [] (const Edit::Input& input) { return input.isStereo() ? 2 : 1; };
    std::vector<bool> placed (wanted.size(), false);

    // Slots already on a wanted input stay (and keep their audio); the others are freed.
    for (auto& slot : captures)
    {
        const Edit::Input held { slot.left.load(), slot.right.load() };
        const auto found = slot.ring.isOn() ? std::find (wanted.begin(), wanted.end(), held) : wanted.end();

        if (found != wanted.end() && ! placed[(size_t) (found - wanted.begin())])
        {
            placed[(size_t) (found - wanted.begin())] = true;
            slot.ring.configure (sampleRate, numChannelsOf (held), seconds); // a change of length or rate
        }
        else if (slot.ring.isOn())
        {
            slot.ring.configure (sampleRate, 1, 0.0);
        }
    }

    for (size_t i = 0; i < wanted.size(); ++i)
    {
        if (placed[i])
            continue;

        for (auto& slot : captures)
        {
            if (slot.ring.isOn())
                continue;

            slot.left = wanted[i].left;
            slot.right = wanted[i].right;
            slot.ring.configure (sampleRate, numChannelsOf (wanted[i]), seconds);
            break;
        }
    }
}

double DawEngine::getCaptureAvailable() const noexcept
{
    auto best = -1.0;
    for (const auto& slot : captures)
        if (slot.ring.isOn())
            best = juce::jmax (best, slot.ring.getAvailableSeconds());
    return best;
}

DawEngine::CaptureResult DawEngine::capture (const juce::File& folder, double seconds)
{
    CaptureResult result;
    syncCapture(); // the armed tracks as they are now

    if (isRecording())
        return { "Stop recording first.", 0.0, 0 };

    const auto tracks = edit.getTracks();
    const auto anyArmed = std::any_of (tracks.begin(), tracks.end(), [] (const auto& t) { return (bool) t[ids::arm] && ! Edit::isMidiTrack (t); });

    if (! anyArmed)
        return { "Arm an audio track first: Capture keeps what comes into the armed tracks.", 0.0, 0 };

    if (captureLength.load() <= 0.0)
        return { "Capture is off. Turn it on to keep the last minutes.", 0.0, 0 };

    struct Taken { Edit::Input input; CaptureRing::Result audio; };
    std::vector<Taken> taken;

    for (auto& slot : captures)
        if (slot.ring.isOn())
            if (auto audio = slot.ring.take (seconds > 0.0 ? seconds : 1.0e9); audio.audio.getNumSamples() > 0)
                taken.push_back ({ { slot.left.load(), slot.right.load() }, std::move (audio) });

    if (taken.empty())
        return { "Nothing captured yet: Capture starts when a track is armed.", 0.0, 0 };

    const auto stamp = juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H-%M-%S");
    const auto latency = recordingLatency.load();
    const auto here = position.load();
    std::vector<std::pair<Edit::Input, std::pair<juce::File, std::vector<Edit::RecordedPass>>>> files;

    for (auto& item : taken)
    {
        const auto label = taken.size() > 1 ? " In " + juce::String (item.input.left + 1) + (item.input.isStereo() ? "-" + juce::String (item.input.right + 1) : juce::String()) : juce::String();
        const auto file = folder.getChildFile ("MicFX capture " + stamp + label + ".wav").getNonexistentSibling();

        if (const auto error = CaptureRing::writeWav (file, item.audio.audio, 0, item.audio.audio.getNumChannels(), sampleRate); error.isNotEmpty())
            return { error, 0.0, 0 };

        // Where it was played: to the music (minus the interface's round trip, as a recording),
        // or at the playhead when nothing was playing.
        auto start = item.audio.startPosition >= 0 ? item.audio.startPosition - latency : here;
        auto offset = juce::int64 (0);
        auto length = (juce::int64) item.audio.audio.getNumSamples();

        if (start < 0) // before the song's start: skip that part of the file
        {
            offset = -start;
            length += start;
            start = 0;
        }

        if (length <= 0)
            continue;

        files.push_back ({ item.input, { file, { Edit::RecordedPass { (double) start / sampleRate, (double) offset / sampleRate,
                                                                       (double) length / sampleRate } } } });
        result.seconds = juce::jmax (result.seconds, (double) length / sampleRate);
    }

    edit.getUndoManager().beginNewTransaction ("Capture");

    for (const auto& [input, placement] : files)
        for (const auto& track : edit.getTracks())
            if ((bool) track[ids::arm] && ! Edit::isMidiTrack (track) && Edit::getTrackInput (track) == input)
            {
                edit.addRecording ((int) track[ids::id], placement.first, placement.second);
                ++result.tracks;
            }

    edit.getUndoManager().beginNewTransaction();
    return result;
}

Recorder::Take DawEngine::stopRecording()
{
    if (patternRecording.load())
    {
        playing = false;
        countingIn = false;
        finishPatternRecording();
        return {};
    }

    playing = false;
    countingIn = false;
    const auto punchedAt = punchEnd.exchange (-1);
    const auto stopAt = punchedAt >= 0 ? juce::jmin (position.load(), punchedAt) : position.load();
    const auto wasRecordingMidi = recordingMidi.exchange (false);
    drainRecordedMidi();

    // The audio takes, by the input they recorded.
    const auto wasRecording = recordingAudio.exchange (false);
    std::vector<std::pair<Edit::Input, Recorder::Take>> audioTakes;
    Recorder::Take take;

    for (int i = 0; i < numTakes.load(); ++i)
    {
        auto& slot = takes[(size_t) i];
        const auto wasOn = slot.on.exchange (false);
        if (slot.recorder == nullptr)
            continue;

        const auto recorded = slot.recorder->isRecording();
        auto result = slot.recorder->stop();

        if (wasRecording && wasOn && recorded && result.file.existsAsFile() && result.seconds > 0.0)
        {
            if (audioTakes.empty())
                take = result;
            take.droppedAudio = take.droppedAudio || result.droppedAudio;
            audioTakes.emplace_back (slot.input, result);
        }
    }
    numTakes = 0;

    const auto hasAudio = ! audioTakes.empty();

    // The notes played, from the MIDI events: times in seconds from the record start,
    // moved earlier by the output latency (the keys were played to what was heard). Held
    // by the sustain pedal until it lifts; anything still held ends at the stop.
    struct Played { int pitch, velocity; double time, length; };
    std::vector<Played> notes;
    const auto takeStart = recordStart.load();

    if (wasRecordingMidi)
    {
        const auto seconds = [&] (juce::int64 t) { return juce::jmax (0.0, (double) (t - midiRecordingLatency.load() - takeStart) / sampleRate); };
        std::array<int, 128> open; // index into notes, or -1
        open.fill (-1);
        std::array<bool, 128> sustained {};
        bool pedal = false;

        const auto end = [&] (int pitch, double time)
        {
            if (const auto index = open[(size_t) pitch]; index >= 0)
                notes[(size_t) index].length = juce::jmax (0.01, time - notes[(size_t) index].time);
            open[(size_t) pitch] = -1;
            sustained[(size_t) pitch] = false;
        };

        for (const auto& event : recordedMidi)
        {
            const auto type = event.status & 0xf0;
            const auto time = seconds (event.time);

            if (type == 0x90 && event.data2 > 0)
            {
                end (event.data1, time);
                open[event.data1] = (int) notes.size();
                notes.push_back ({ event.data1, event.data2, time, 0.0 });
            }
            else if (type == 0x80 || type == 0x90)
            {
                if (pedal)
                    sustained[event.data1] = open[event.data1] >= 0;
                else
                    end (event.data1, time);
            }
            else if (type == 0xb0 && event.data1 == 64)
            {
                pedal = event.data2 >= 64;
                if (! pedal)
                    for (int p = 0; p < 128; ++p)
                        if (sustained[(size_t) p])
                            end (p, time);
            }
        }

        for (int p = 0; p < 128; ++p)
            end (p, seconds (stopAt));

        recordedMidi.clear();
    }

    if (! hasAudio && notes.empty())
        return take;

    edit.getUndoManager().beginNewTransaction ("Record");

    if (! notes.empty())
    {
        const auto start = (double) takeStart / sampleRate;
        const auto takeLength = juce::jmax (Edit::minClipSeconds, (double) (stopAt - takeStart) / sampleRate);

        for (const auto& track : edit.getTracks())
        {
            if (! (bool) track[ids::arm] || ! Edit::isMidiTrack (track))
                continue;

            const auto clip = edit.addMidiClip ((int) track[ids::id], start, takeLength, track[ids::name].toString());
            for (const auto& note : notes)
                edit.addNote ((int) clip[ids::id], note.pitch, note.time, note.length, note.velocity);
        }
    }

    if (! hasAudio)
    {
        edit.getUndoManager().beginNewTransaction();
        return take;
    }

    const auto wasLooping = loopRecording.exchange (false);

    for (const auto& [input, recorded] : audioTakes)
    {
        const auto passes = getRecordedPasses (recorded, wasLooping);

        for (const auto& track : edit.getTracks())
            if ((bool) track[ids::arm] && ! Edit::isMidiTrack (track) && Edit::getTrackInput (track) == input)
                edit.addRecording ((int) track[ids::id], recorded.file, passes);
    }

    edit.getUndoManager().beginNewTransaction();
    return take;
}

std::vector<Edit::RecordedPass> DawEngine::getRecordedPasses (const Recorder::Take& recorded, bool looped) const
{
    // A sample recorded at playhead position p was sung to what was heard at p minus the
    // interface's round trip, so it belongs there on the timeline.
    const auto latency = recordingLatency.load();
    const auto fileLength = (juce::int64) std::llround (recorded.seconds * sampleRate);
    const auto toSeconds = [this] (juce::int64 samples) { return (double) samples / sampleRate; };

    const auto pass = [&] (juce::int64 start, juce::int64 offset, juce::int64 length)
    {
        if (start < 0) // before the song's start: skip that part of the file
        {
            offset -= start;
            length += start;
            start = 0;
        }
        return Edit::RecordedPass { toSeconds (start), toSeconds (offset), toSeconds (juce::jmin (length, fileLength - offset)) };
    };

    const auto start = recorded.timelineStart;
    const auto end = loopEnd.load(), loopLength = end - loopStart.load();

    if (! looped || loopLength <= 0 || start >= end)
        return { pass (start - latency, 0, fileLength) };

    // The first pass runs from the record start to the loop end; each one after it from
    // the loop start, one loop long, from where it continues in the file.
    std::vector<Edit::RecordedPass> passes { pass (start - latency, 0, end - start + latency) };
    const auto shortest = (juce::int64) std::llround (minLoopPassSeconds * sampleRate);

    for (auto offset = end - start + latency; offset < fileLength; offset += loopLength)
        if (const auto length = juce::jmin (loopLength, fileLength - offset); length >= shortest)
            passes.push_back (pass (loopStart.load(), offset, length));

    passes.erase (std::remove_if (passes.begin(), passes.end(), [] (const auto& p) { return p.length < Edit::minClipSeconds; }), passes.end());
    return passes;
}

void DawEngine::renderClick (const TempoMap& tempo, juce::AudioBuffer<float>& output, int offset, int numChannels, int numSamples,
                             juce::int64 blockStart) noexcept
{
    const auto counting = countingIn.load();
    const auto start = recordStart.load();
    const auto clickOn = metronome.load();
    const auto onlyRecording = clickOnlyRecording.load();
    const auto recording = recordingAudio.load() || recordingMidi.load();
    const auto barLength = (juce::int64) beatsPerBar.load();
    const auto volume = metronomeVolume.load();

    // The beats that start in this block, on the tempo map.
    auto nextBeat = (juce::int64) std::ceil (tempo.toBeats ((double) blockStart / sampleRate) - 1.0e-9);
    auto nextAt = (juce::int64) std::llround (tempo.toSeconds ((double) nextBeat) * sampleRate);
    if (nextAt < blockStart)
        nextAt = (juce::int64) std::llround (tempo.toSeconds ((double) ++nextBeat) * sampleRate);

    for (int i = 0; i < numSamples; ++i)
    {
        const auto t = blockStart + i;

        if (t == nextAt)
        {
            if ((clickOn && (! onlyRecording || recording)) || (counting && t < start))
            {
                const auto downbeat = ((nextBeat % barLength) + barLength) % barLength == 0;
                clickPhase = 0.0f;
                clickIncrement = (downbeat ? 1500.0f : 1000.0f) / (float) sampleRate;
                clickLevel = (downbeat ? 0.35f : 0.22f) * volume;
                clickDecay = (float) std::exp (-1.0 / (0.012 * sampleRate));
            }

            nextAt = (juce::int64) std::llround (tempo.toSeconds ((double) ++nextBeat) * sampleRate);
        }

        if (clickLevel < 1.0e-4f)
            continue;

        const auto sample = clickLevel * std::sin (juce::MathConstants<float>::twoPi * clickPhase);
        clickPhase += clickIncrement;
        clickPhase -= std::floor (clickPhase);
        clickLevel *= clickDecay;

        for (int ch = 0; ch < numChannels; ++ch)
            output.addSample (ch, offset + i, sample);
    }
}

//==============================================================================
void DawEngine::renderClip (const Clip& clip, juce::AudioBuffer<float>& buffer, juce::int64 blockStart, int numSamples) noexcept
{
    const auto from = juce::jmax (blockStart, clip.start);
    const auto to = juce::jmin (blockStart + numSamples, clip.start + clip.length);

    if (from >= to)
        return;

    const auto& audio = *clip.audio;
    const auto sourceChannels = audio.getNumChannels();

    for (int ch = 0; ch < 2; ++ch)
    {
        const auto* source = audio.getReadPointer (juce::jmin (ch, sourceChannels - 1));
        auto* destination = buffer.getWritePointer (ch);

        for (auto t = from; t < to; ++t)
        {
            const auto intoClip = t - clip.start;
            auto gain = clip.gain;

            if (intoClip < clip.fadeIn)
                gain *= (float) intoClip / (float) clip.fadeIn;

            if (const auto remaining = clip.length - intoClip; remaining < clip.fadeOut)
                gain *= (float) remaining / (float) clip.fadeOut;

            destination[t - blockStart] += gain * source[clip.offset + intoClip];
        }
    }
}

void DawEngine::Delay::process (juce::AudioBuffer<float>& buffer, int numSamples) noexcept
{
    if (samples <= 0)
        return;

    for (int ch = 0; ch < 2; ++ch)
    {
        auto* data = buffer.getWritePointer (ch);
        auto* delayed = line.getWritePointer (ch);
        auto at = write;

        for (int i = 0; i < numSamples; ++i)
        {
            std::swap (data[i], delayed[at]);
            at = (at + 1) % samples;
        }

        if (ch == 1)
            write = at;
    }
}

void DawEngine::recordMidi (const juce::MidiBuffer& midi, int midiOffset, int numSamples, juce::int64 blockStart) noexcept
{
    const auto start = recordStart.load();

    for (const auto metadata : midi)
    {
        if (metadata.samplePosition < midiOffset || metadata.samplePosition >= midiOffset + numSamples)
            continue;

        const auto time = blockStart + (metadata.samplePosition - midiOffset);
        const auto message = metadata.getMessage();

        // Pattern recording: from anywhere in the loop (after the count-in).
        const auto early = patternRecording.load (std::memory_order_relaxed) ? countingIn.load() : time < start;
        if (early || ! (message.isNoteOnOrOff() || message.isSustainPedalOn() || message.isSustainPedalOff()))
            continue;

        const auto* data = message.getRawData();
        const auto scope = midiFifo.write (1);
        if (scope.blockSize1 == 1)
            midiFifoData[(size_t) scope.startIndex1] = { time, data[0], data[1], data[2] };
    }
}

void DawEngine::renderInstrument (Track& track, bool isPlaying, bool fresh, juce::int64 blockStart, int numSamples,
                                  const juce::MidiBuffer* input, int midiOffset) noexcept
{
    auto& strip = *track.strip;
    auto* instrument = strip.instrument.get();
    if (instrument == nullptr)
        return;

    auto& midi = strip.midi;
    midi.clear();

    // The timeline's notes are scheduled late by the track's compensation delay, so they
    // line up with the delayed audio tracks (the live input isn't delayed).
    const auto windowStart = blockStart - track.delay.samples;
    const auto jumped = isPlaying && strip.nextBlock != windowStart;

    if (strip.sounding.any() && (fresh || ! isPlaying || jumped))
    {
        // A new graph while playing on (an edit, a note added by pattern recording): the
        // notes it still has sounding at the playhead carry on; the rest stop.
        std::bitset<128> keep;
        if (fresh && isPlaying && ! jumped)
            for (const auto& e : track.midiEvents)
            {
                if (e.time >= windowStart)
                    break;
                keep.set ((size_t) e.note, e.velocity > 0);
            }

        for (int note = 0; note < 128; ++note)
            if (strip.sounding[(size_t) note] && ! keep[(size_t) note])
                midi.addEvent (juce::MidiMessage::noteOff (1, note), 0);

        strip.sounding &= keep;
    }

    if (isPlaying)
    {
        const auto& events = track.midiEvents;
        auto it = std::lower_bound (events.begin(), events.end(), windowStart,
                                    [] (const MidiEvent& e, juce::int64 t) { return e.time < t; });

        for (; it != events.end() && it->time < windowStart + numSamples; ++it)
        {
            const auto at = (int) (it->time - windowStart);

            if (it->velocity > 0)
            {
                midi.addEvent (juce::MidiMessage::noteOn (1, it->note, (juce::uint8) it->velocity), at);
                strip.sounding.set ((size_t) it->note);
            }
            else if (strip.sounding[(size_t) it->note])
            {
                midi.addEvent (juce::MidiMessage::noteOff (1, it->note), at);
                strip.sounding.reset ((size_t) it->note);
            }
        }

        strip.nextBlock = windowStart + numSamples;
    }
    else
    {
        strip.nextBlock = -1;
    }

    if (input != nullptr && strip.monitoring.load (std::memory_order_relaxed))
        midi.addEvents (*input, midiOffset, numSamples, -midiOffset);

    if (! strip.audition.isEmpty())
    {
        midi.addEvents (strip.audition, 0, -1, 0);
        strip.audition.clear();
    }

    const juce::ScopedLock lock (instrument->getCallbackLock());

    if (instrument->isSuspended())
        return;

    const auto channels = strip.instrumentBuffer.getNumChannels();
    juce::AudioBuffer<float> view (strip.instrumentBuffer.getArrayOfWritePointers(), channels, numSamples);
    view.clear();
    instrument->processBlock (view, midi);

    const auto outputs = juce::jlimit (1, channels, instrument->getTotalNumOutputChannels());
    strip.buffer.addFrom (0, 0, view, 0, 0, numSamples);
    strip.buffer.addFrom (1, 0, view, juce::jmin (1, outputs - 1), 0, numSamples);
}

void DawEngine::renderBlock (Graph& graph, juce::AudioBuffer<float>& output, int outputOffset, int numChannels,
                             int numSamples, const float* const* inputs, int numInputs, const juce::MidiBuffer* midi,
                             int midiOffset) noexcept
{
    const auto isPlaying = playing.load();
    const auto blockStart = position.load();
    const auto fresh = std::exchange (graph.fresh, false);

    if (isPlaying && countingIn.load() && blockStart + numSamples > recordStart.load())
        countingIn = false;

    // An input's samples, or silence for one the interface doesn't have.
    const auto inputChannel = [&] (int index) -> const float*
    {
        return index >= 0 && index < numInputs && inputs[index] != nullptr ? inputs[index] : nullptr;
    };

    // Capture: the raw inputs of the armed tracks, always, with the playhead while it runs.
    for (auto& slot : captures)
    {
        if (! slot.ring.isOn())
            continue;

        const auto* left = inputChannel (slot.left.load (std::memory_order_relaxed));
        const auto* right = inputChannel (slot.right.load (std::memory_order_relaxed));
        const float* channels[] = { left != nullptr ? left : silence.data(),
                                    right != nullptr ? right : silence.data() };
        slot.ring.write (channels, numSamples, isPlaying ? blockStart : -1);
    }

    // The dry takes: the raw inputs from the record start on (not during the count-in).
    if (isPlaying && recordingAudio.load())
    {
        const auto start = recordStart.load();
        const auto skip = (int) juce::jlimit ((juce::int64) 0, (juce::int64) numSamples, start - blockStart);
        const auto stopAt = punchEnd.load();
        const auto count = stopAt < 0 ? numSamples - skip
                                      : (int) juce::jlimit ((juce::int64) 0, (juce::int64) (numSamples - skip), stopAt - (blockStart + skip));

        if (count > 0)
        {
            for (int t = 0; t < numTakes.load (std::memory_order_acquire); ++t)
            {
                auto& take = takes[(size_t) t];
                if (! take.on.load (std::memory_order_acquire))
                    continue;

                const auto* left = inputChannel (take.input.left);
                const auto* right = inputChannel (take.input.right);
                const float* channels[] = { left != nullptr ? left + skip : silence.data(),
                                            right != nullptr ? right + skip : silence.data() };
                take.recorder->write (channels, nullptr, count, blockStart + skip);
            }
        }
    }

    if (isPlaying && midi != nullptr && recordingMidi.load())
        recordMidi (*midi, midiOffset, numSamples, blockStart);

    followChords (graph, isPlaying, blockStart, numSamples);

    // The tempo where the timeline is, for effects that sync to it (the echo).
    {
        const auto bpm = graph.tempo.getBpmAt ((double) std::max ((juce::int64) 0, blockStart) / sampleRate);
        for (auto& track : graph.tracks)
            track.strip->chain.setTempo (bpm);
        for (auto& bus : graph.returns)
            bus.strip->chain.setTempo (bpm);
        graph.master->chain.setTempo (bpm);
    }

    auto& master = *graph.master;
    master.buffer.clear (0, numSamples);

    for (auto& bus : graph.returns)
        bus.strip->buffer.clear (0, numSamples);

    const auto delayDirect = graph.directDelay.samples > 0;
    auto& direct = delayDirect ? graph.direct : master.buffer;
    if (delayDirect)
        direct.clear (0, numSamples);

    // A track's sends: its signal into the returns, gliding to new levels.
    const auto feedSendsFrom = [&graph, numSamples, isPlaying, blockStart] (std::vector<Send>& sends, Strip& source, bool pre)
    {
        for (auto& send : sends)
        {
            if (send.pre != pre)
                continue;

            auto& gain = send.level->gain;
            // Automated while playing: the lane's level where the timeline is.
            if (isPlaying && ! send.points.empty() && ! isHeld (send.held))
            {
                const auto db = rideDbAt (send.points, blockStart);
                gain.setTargetValue (send.level->audible.load (std::memory_order_relaxed) && db >= Edit::minSendDb ? juce::Decibels::decibelsToGain (db) : 0.0f);
            }
            else
                gain.setTargetValue (send.level->target.load (std::memory_order_relaxed));
            const auto from = gain.getCurrentValue();
            const auto to = gain.skip (numSamples);

            if (from == 0.0f && to == 0.0f)
                continue;

            auto& into = graph.returns[(size_t) send.bus].strip->buffer;
            into.addFromWithRamp (0, 0, source.buffer.getReadPointer (0), numSamples, from, to);
            into.addFromWithRamp (1, 0, source.buffer.getReadPointer (1), numSamples, from, to);
        }
    };

    const auto feedSends = [&] (Track& track, bool pre) { feedSendsFrom (track.sends, *track.strip, pre); };

    for (auto& track : graph.tracks)
    {
        auto& strip = *track.strip;
        strip.buffer.clear (0, numSamples);

        if (isPlaying)
            for (const auto& clip : track.clips)
                renderClip (clip, strip.buffer, blockStart, numSamples);

        track.delay.process (strip.buffer, numSamples);

        // The instrument after the delay line: its notes are scheduled late instead, so
        // what's played live isn't delayed.
        if (strip.isMidi)
            renderInstrument (track, isPlaying, fresh, blockStart, numSamples, midi, midiOffset);

        // Live input after the delay line: monitoring shouldn't wait for compensation. A
        // mono input plays on both sides, a stereo one left and right.
        else if (numInputs > 0 && strip.monitoring.load (std::memory_order_relaxed))
        {
            const auto* left = inputChannel (strip.inputLeft.load (std::memory_order_relaxed));
            const auto rightIndex = strip.inputRight.load (std::memory_order_relaxed);
            const auto* right = rightIndex == Edit::noInput ? left : inputChannel (rightIndex);

            if (left != nullptr)
                strip.buffer.addFrom (0, 0, left, numSamples);
            if (right != nullptr)
                strip.buffer.addFrom (1, 0, right, numSamples);
        }

        // Effect automation: each value where the timeline is (knob values glide, so a
        // block-rate update doesn't click); back to the insert's own when stopped.
        if (! track.lanes.empty())
        {
            if (isPlaying)
                for (auto& lane : track.lanes)
                    if (! isHeld (lane.held))
                        lane.value->store (rideDbAt (lane.points, blockStart), std::memory_order_relaxed);
            else if (track.automating)
                for (auto& lane : track.lanes)
                    lane.value->store (lane.base, std::memory_order_relaxed);
            track.automating = isPlaying;
        }

        auto block = juce::dsp::AudioBlock<float> (strip.buffer).getSubBlock (0, (size_t) numSamples);
        strip.chain.process (fx::Module::Context (block));

        if (! track.sends.empty())
            feedSends (track, true);

        // Pan automation, while playing: the lane's pan in place of the knob.
        if (isPlaying && ! track.pan.empty() && ! isHeld (track.panHeld))
        {
            const auto [gainLeft, gainRight] = balanceGains (juce::jlimit (-1.0f, 1.0f, rideDbAt (track.pan, blockStart)));
            const auto gain = strip.targetGain.load (std::memory_order_relaxed);
            strip.left.setTargetValue (gain * gainLeft);
            strip.right.setTargetValue (gain * gainRight);
        }
        else
        {
            strip.left.setTargetValue (strip.targetLeft.load (std::memory_order_relaxed));
            strip.right.setTargetValue (strip.targetRight.load (std::memory_order_relaxed));
        }

        auto* left = strip.buffer.getWritePointer (0);
        auto* right = strip.buffer.getWritePointer (1);

        for (int i = 0; i < numSamples; ++i)
        {
            left[i] *= strip.left.getNextValue();
            right[i] *= strip.right.getNextValue();
        }

        // Volume automation, while playing: a straight ramp over the block (points are a
        // tenth of a second apart or more, blocks a few milliseconds).
        if (isPlaying && ! track.ride.empty())
        {
            const auto from = juce::Decibels::decibelsToGain (rideDbAt (track.ride, blockStart));
            const auto to = juce::Decibels::decibelsToGain (rideDbAt (track.ride, blockStart + numSamples));
            strip.buffer.applyGainRamp (0, 0, numSamples, from, to);
            strip.buffer.applyGainRamp (1, 0, numSamples, from, to);
        }

        if (! track.sends.empty())
            feedSends (track, false);

        updatePeak (strip.peakLeft, strip.buffer.getMagnitude (0, 0, numSamples));
        updatePeak (strip.peakRight, strip.buffer.getMagnitude (1, 0, numSamples));

        auto& into = track.output >= 0 ? graph.returns[(size_t) track.output].strip->buffer : direct;
        strip.direct.setTargetValue (strip.targetDirect.load (std::memory_order_relaxed));
        const auto from = strip.direct.getCurrentValue();
        const auto to = strip.direct.skip (numSamples);

        if (from == 1.0f && to == 1.0f)
        {
            into.addFrom (0, 0, strip.buffer, 0, 0, numSamples);
            into.addFrom (1, 0, strip.buffer, 1, 0, numSamples);
        }
        else if (from != 0.0f || to != 0.0f)
        {
            into.addFromWithRamp (0, 0, strip.buffer.getReadPointer (0), numSamples, from, to);
            into.addFromWithRamp (1, 0, strip.buffer.getReadPointer (1), numSamples, from, to);
        }
    }

    // The returns: their effects on what the tracks sent, into the master.
    for (auto& bus : graph.returns)
    {
        auto& strip = *bus.strip;
        auto block = juce::dsp::AudioBlock<float> (strip.buffer).getSubBlock (0, (size_t) numSamples);
        strip.chain.process (fx::Module::Context (block));

        if (! bus.sends.empty())
            feedSendsFrom (bus.sends, strip, true);

        strip.left.setTargetValue (strip.targetLeft.load (std::memory_order_relaxed));
        strip.right.setTargetValue (strip.targetRight.load (std::memory_order_relaxed));
        strip.buffer.applyGainRamp (0, 0, numSamples, strip.left.getCurrentValue(), strip.left.skip (numSamples));
        strip.buffer.applyGainRamp (1, 0, numSamples, strip.right.getCurrentValue(), strip.right.skip (numSamples));

        if (! bus.sends.empty())
            feedSendsFrom (bus.sends, strip, false);

        updatePeak (strip.peakLeft, strip.buffer.getMagnitude (0, 0, numSamples));
        updatePeak (strip.peakRight, strip.buffer.getMagnitude (1, 0, numSamples));

        bus.delay.process (strip.buffer, numSamples);
        master.buffer.addFrom (0, 0, strip.buffer, 0, 0, numSamples);
        master.buffer.addFrom (1, 0, strip.buffer, 1, 0, numSamples);
    }

    if (delayDirect)
    {
        graph.directDelay.process (direct, numSamples);
        master.buffer.addFrom (0, 0, direct, 0, 0, numSamples);
        master.buffer.addFrom (1, 0, direct, 1, 0, numSamples);
    }

    auto block = juce::dsp::AudioBlock<float> (master.buffer).getSubBlock (0, (size_t) numSamples);
    master.chain.process (fx::Module::Context (block));

    master.left.setTargetValue (master.targetLeft.load (std::memory_order_relaxed));
    master.right.setTargetValue (master.targetRight.load (std::memory_order_relaxed));
    master.buffer.applyGainRamp (0, 0, numSamples, master.left.getCurrentValue(), master.left.skip (numSamples));
    master.buffer.applyGainRamp (1, 0, numSamples, master.right.getCurrentValue(), master.right.skip (numSamples));
    updatePeak (master.peakLeft, master.buffer.getMagnitude (0, 0, numSamples));
    updatePeak (master.peakRight, master.buffer.getMagnitude (1, 0, numSamples));

    if (numChannels == 1)
    {
        output.copyFrom (0, outputOffset, master.buffer.getReadPointer (0), numSamples, 0.5f);
        output.addFrom (0, outputOffset, master.buffer, 1, 0, numSamples, 0.5f);
    }
    else
    {
        for (int ch = 0; ch < numChannels; ++ch)
            output.copyFrom (ch, outputOffset, master.buffer, juce::jmin (ch, 1), 0, numSamples);
    }

    if (isPlaying)
    {
        renderClick (graph.tempo, output, outputOffset, numChannels, numSamples, blockStart);
        position = blockStart + numSamples;
    }
}

void DawEngine::followChords (Graph& graph, bool isPlaying, juce::int64 blockStart, int numSamples) noexcept
{
    // Every pitch insert, on tracks and returns.
    const auto forEachPitch = [&graph] (auto&& action)
    {
        for (auto& track : graph.tracks)
            for (auto* pitch : track.strip->pitches)
                action (*pitch);
        for (auto& bus : graph.returns)
            for (auto* pitch : bus.strip->pitches)
                action (*pitch);
    };

    auto following = false;
    forEachPitch ([&following] (fx::modules::Pitch& pitch) { following = following || pitch.isFollowingChords(); });

    if (! following || graph.chords == nullptr)
    {
        harmonyChord.store (-1, std::memory_order_relaxed);
        return;
    }

    auto& follower = *graph.chords;
    follower.setKeyMask (graph.keyMask);

    // Notes: what's played on the keyboard plus what the MIDI tracks are sounding.
    auto notes = getHeldNotes();
    for (auto& track : graph.tracks)
        if (track.strip->isMidi)
            notes |= track.strip->sounding;
    follower.setHeldNotes (notes);

    // Audio: the clips of the tracks without pitch inserts (the vocals have them), read
    // a little ahead so the chord has been heard by the time it plays.
    if (isPlaying && ! notes.any())
    {
        const auto lookAhead = (juce::int64) (0.12 * sampleRate);
        auto& listen = graph.listen;
        listen.clear (0, numSamples);

        for (auto& track : graph.tracks)
            if (! track.strip->isMidi && track.strip->pitches.empty())
                for (const auto& clip : track.clips)
                    renderClip (clip, listen, blockStart + lookAhead, numSamples);

        listen.addFrom (0, 0, listen, 1, 0, numSamples);
        follower.pushAudio (listen.getReadPointer (0), numSamples);
    }

    const auto chord = follower.getChord();
    harmonyChord.store (chord.pack(), std::memory_order_relaxed);

    forEachPitch ([chord] (fx::modules::Pitch& pitch) { pitch.setChord (chord); });
}

void DawEngine::process (juce::AudioBuffer<float>& output, int numChannels, int numSamples, const float* input,
                         const juce::MidiBuffer* midiIn) noexcept
{
    const float* inputs[] = { input };
    process (output, numChannels, numSamples, inputs, input != nullptr ? 1 : 0, midiIn);
}

void DawEngine::process (juce::AudioBuffer<float>& output, int numChannels, int numSamples, const float* const* inputs,
                         int numInputs, const juce::MidiBuffer* midiIn) noexcept
{
    numInputs = juce::jlimit (0, Edit::maxInputs, inputs != nullptr ? numInputs : 0);

    // The MIDI input plus anything from addLiveMidi(); held notes for the UI.
    const juce::MidiBuffer* midi = midiIn;

    if (liveFifo.getNumReady() > 0)
    {
        liveMidi.clear();
        if (midiIn != nullptr)
            liveMidi.addEvents (*midiIn, 0, numSamples, 0);

        const auto scope = liveFifo.read (liveFifo.getNumReady());
        scope.forEach ([this] (int index)
        {
            const auto packed = liveFifoData[(size_t) index];
            const juce::uint8 bytes[] = { (juce::uint8) packed, (juce::uint8) (packed >> 8), (juce::uint8) (packed >> 16) };
            const auto size = juce::MidiMessage::getMessageLengthFromFirstByte (bytes[0]);
            liveMidi.addEvent (bytes, size, 0);
        });

        midi = &liveMidi;
    }

    if (midi != nullptr)
    {
        for (const auto metadata : *midi)
        {
            const auto message = metadata.getMessage();
            if (! message.isNoteOnOrOff())
                continue;

            const auto note = message.getNoteNumber();
            const auto bit = 1u << (note % 32);
            auto& word = heldNotes[(size_t) (note / 32)];

            if (message.isNoteOn())
                word.fetch_or (bit, std::memory_order_relaxed);
            else
                word.fetch_and (~bit, std::memory_order_relaxed);
        }
    }

    if (! realtime && rebuildPending)
        rebuild(); // offline: its own thread (see requestRebuild)

    if (auto* next = incoming.exchange (nullptr, std::memory_order_acq_rel))
    {
        if (! realtime)
        {
            delete active;
        }
        else if (active != nullptr)
        {
            const auto scope = retiredFifo.write (1);
            jassert (scope.blockSize1 == 1); // the message thread isn't freeing old graphs

            if (scope.blockSize1 == 1)
                retired[(size_t) scope.startIndex1] = active;
        }

        active = next;
    }

    // Auditioned notes go to their track's strip (dropped when it isn't in the graph).
    if (auditionFifo.getNumReady() > 0)
    {
        const auto scope = auditionFifo.read (auditionFifo.getNumReady());
        scope.forEach ([this] (int index)
        {
            const auto [trackId, packed] = auditionFifoData[(size_t) index];
            if (active == nullptr)
                return;

            for (auto& track : active->tracks)
                if (track.strip->id == trackId && track.strip->isMidi)
                {
                    const juce::uint8 bytes[] = { (juce::uint8) packed, (juce::uint8) (packed >> 8), (juce::uint8) (packed >> 16) };
                    track.strip->audition.addEvent (bytes, juce::MidiMessage::getMessageLengthFromFirstByte (bytes[0]), 0);
                }
        });
    }

    numChannels = juce::jmin (numChannels, output.getNumChannels());

    if (active == nullptr || numChannels <= 0)
    {
        for (int ch = 0; ch < numChannels; ++ch)
            output.clear (ch, 0, numSamples);
        return;
    }

    for (int done = 0; done < numSamples;)
    {
        auto chunk = juce::jmin (active->blockSize, numSamples - done);

        // Looping: stop the chunk at the loop end, then jump back to the start. Pattern
        // mode always loops its pattern (from anywhere past its end too).
        const auto patternEnd = patternLoopEnd.load();
        const auto inPattern = patternEnd > 0;
        const auto looping = playing.load() && (inPattern || (loopOn.load() && (! isRecording() || loopRecording.load())));
        const auto end = inPattern ? patternEnd : loopEnd.load();
        const auto start = inPattern ? (juce::int64) 0 : loopStart.load();

        if (looping && (position.load() == end || (inPattern && position.load() > end)))
            position = start;

        if (const auto pos = position.load(); looping && pos < end && pos + chunk > end)
            chunk = (int) (end - pos);

        std::array<const float*, Edit::maxInputs> chunkInputs {};
        for (int i = 0; i < numInputs; ++i)
            chunkInputs[(size_t) i] = inputs[i] != nullptr ? inputs[i] + done : nullptr;

        renderBlock (*active, output, done, numChannels, chunk, chunkInputs.data(), numInputs, midi, done);

        if (looping && position.load() == end)
            position = start;

        done += chunk;
    }

    if (realtime)
        renderPreview (output, numChannels, numSamples);
}
}
