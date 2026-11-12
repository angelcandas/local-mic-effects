#include "DawEngine.h"

namespace daw
{
namespace
{
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
    pool.onLoaded = [this] { triggerAsyncUpdate(); };
    liveMidi.ensureSize (4096);

    edit.getState().addListener (this);
    updateTempo();
    rebuild();

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
    updateTempo();

    // Fresh strips: every chain (and instrument) is prepared for the new rate and block
    // size. The instruments are made again, from their current settings.
    captureInstrumentStates();
    strips.clear();
    rebuild();
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
                pool.loadNow (juce::File (child[ids::file].toString()), Edit::getWarpSettings (child));
            else if (child.hasType (ids::TAKE) && (int) child[ids::id] == Edit::getListenTake (track))
                for (const auto& clip : child)
                    if (clip.hasType (ids::CLIP))
                        pool.loadNow (juce::File (clip[ids::file].toString()), Edit::getWarpSettings (clip));
        }

    rebuildNow();
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
            triggerAsyncUpdate();
        return;
    }

    if (tree.hasType (ids::NOTE))
    {
        triggerAsyncUpdate();
        return;
    }

    if (tree.hasType (ids::INSERT))
    {
        // A new type, or automation added, changed or removed (or a value under
        // automation, whose own setting the lanes go back to): a rebuild.
        if (property == ids::type || property == ids::automation || tree.hasProperty (ids::automation))
        {
            triggerAsyncUpdate();
            if (property == ids::type || property == ids::automation)
                return;
        }

        // An insert value: straight into the running module, no rebuild.
        const auto owner = tree.getParent().getParent();
        if (const auto it = strips.find ((int) owner[ids::id]); it != strips.end())
        {
            const auto& modules = it->second->modulesByInsertId;
            if (const auto module = modules.find ((int) tree[ids::id]); module != modules.end())
                if (auto* value = module->second->findValue (property.toString()))
                    value->store ((float) tree[property]);

            // Switching autotune or pitch shift on or off changes the track's delay, so
            // the latency compensation is worked out again.
            if (it->second->getLatency() != it->second->latencyAtBuild)
                triggerAsyncUpdate();
        }

        return;
    }

    if (tree.hasType (ids::PROJECT))
    {
        if (property == ids::tempo || property == ids::tempoChanges)
            triggerAsyncUpdate(); // the click follows the new tempo map (and so will musical clips)
        else if (property == ids::beatsPerBar)
            updateTempo();
        else if (property == ids::loopOn || property == ids::loopStart || property == ids::loopEnd)
            updateLoop();

        return;
    }

    if (tree.hasType (ids::SEND))
    {
        if (property == ids::level)
            updateMix();
        else
            triggerAsyncUpdate(); // pre/post or another return: a new graph

        return;
    }

    if (tree.hasType (ids::TRACK) && (property == ids::transpose || property == ids::ride || property == ids::output || property == ids::listenTake))
    {
        triggerAsyncUpdate(); // its clips play other rendered copies
        return;
    }

    if (tree.hasType (ids::TRACK) || tree.hasType (ids::MASTER) || tree.hasType (ids::RETURN))
    {
        if (property == ids::volume || property == ids::pan || property == ids::mute || property == ids::solo
            || property == ids::monitor || property == ids::arm || property == ids::inGain || property == ids::outGain
            || property == ids::inputLeft || property == ids::inputRight)
            updateMix();
        else if (property == ids::kind)
            triggerAsyncUpdate();

        return;
    }

    if (tree.hasType (ids::CLIP))
        triggerAsyncUpdate();
}

void DawEngine::setStripTargets (Strip& strip, const juce::ValueTree& tree, bool audible, bool jump)
{
    const auto gain = audible ? volumeToGain (tree[ids::volume]) : 0.0f;
    const auto pan = juce::jlimit (-1.0f, 1.0f, (float) tree.getProperty (ids::pan, 0.0f));
    const auto halfPi = juce::MathConstants<float>::halfPi;

    // Balance: the centre is unity on both sides; panning turns the other side down.
    const auto left  = pan > 0.0f ? std::cos (pan * halfPi) : 1.0f;
    const auto right = pan < 0.0f ? std::cos (-pan * halfPi) : 1.0f;

    strip.targetLeft = gain * left;
    strip.targetRight = gain * right;

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
            it->second->monitoring = (bool) track[ids::monitor] || (Edit::isMidiTrack (track) && (bool) track[ids::arm]);
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
            }
        }
    }

    for (const auto& bus : returns)
        if (const auto it = strips.find ((int) bus[ids::id]); it != strips.end())
            setStripTargets (*it->second, bus, ! (bool) bus[ids::mute] && (! anyReturnSolo || (bool) bus[ids::solo]), false);

    if (const auto it = strips.find (Edit::masterId); it != strips.end())
        setStripTargets (*it->second, edit.getMaster(), true, false);
}

std::shared_ptr<DawEngine::Strip> DawEngine::getStrip (const juce::ValueTree& trackOrMaster)
{
    const auto id = (int) trackOrMaster[ids::id];
    const auto inserts = Edit::getInserts (trackOrMaster);

    juce::String layout;
    for (const auto& insert : inserts)
        layout << insert[ids::type].toString() << ":" << (int) insert[ids::id] << " ";

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

    // The chain this one replaces, if any: inserts that stay hand over what they've learnt.
    const auto previous = strips.find (id);
    const auto* previousModules = previous != strips.end() ? &previous->second->modulesByInsertId : nullptr;

    for (const auto& insert : inserts)
    {
        const auto type = Edit::moduleTypeFromKey (insert[ids::type].toString());
        if (! type.has_value())
            continue; // a module from a newer version: skipped rather than refusing the project

        auto& module = strip->chain.add (fx::createModule (*type));

        for (int v = 0; v < module.getNumValues(); ++v)
            if (const auto& value = insert[juce::Identifier (module.getValueId (v))]; ! value.isVoid())
                module.getValue (v).store ((float) value);

        if (previousModules != nullptr)
            if (const auto it = previousModules->find ((int) insert[ids::id]); it != previousModules->end() && it->second->getType() == *type)
                module.carryStateFrom (*it->second);

        strip->modulesByInsertId[(int) insert[ids::id]] = &module;
    }

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
    std::map<int, std::shared_ptr<Strip>> previous;
    std::swap (previous, strips);

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

    std::map<int, int> busIndex; // return id -> index in graph->returns
    for (const auto& returnTree : edit.getReturns())
    {
        busIndex[(int) returnTree[ids::id]] = (int) graph->returns.size();
        graph->returns.push_back ({ reuse (returnTree), {} });
    }

    std::map<std::pair<int, int>, std::shared_ptr<SendLevel>> previousSends;
    std::swap (previousSends, sendLevels);
    std::vector<std::shared_ptr<SendLevel>> newSends; // start at their level instead of fading in

    juce::int64 end = 0;
    int maxLatency = 0;
    std::set<juce::String> warpsInUse;
    std::map<int, AudioPool::Audio> clipAudio;

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
            track.sends.push_back ({ level, bus->second, (bool) sendTree.getProperty (ids::pre, false) });
        }

        if (track.strip->isMidi)
            buildMidiEvents (track, trackTree, sampleRate, end);

        for (const auto& point : Edit::getVolumeRide (trackTree))
            track.ride.emplace_back ((juce::int64) std::llround (point.time * sampleRate), point.db);

        for (const auto& insert : Edit::getInserts (trackTree))
            for (const auto& [valueId, points] : Edit::getInsertAutomation (insert))
            {
                const auto module = track.strip->modulesByInsertId.find ((int) insert[ids::id]);
                if (module == track.strip->modulesByInsertId.end())
                    continue;
                Track::Lane lane;
                lane.value = module->second->findValue (valueId);
                if (lane.value == nullptr)
                    continue;
                lane.base = (float) insert.getProperty (juce::Identifier (valueId), 0.0f);
                for (const auto& point : points)
                    lane.points.emplace_back ((juce::int64) std::llround (point.time * sampleRate), point.value);
                track.lanes.push_back (std::move (lane));
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
            const auto warpSettings = Edit::getWarpSettings (clipTree);
            const auto clipId = (int) clipTree[ids::id];
            clip.audio = pool.get (file, warpSettings);

            if (! warpSettings.isNeutral())
                warpsInUse.insert (AudioPool::getWarpKey (file, warpSettings));

            if (clip.audio == nullptr)
            {
                // A new warp or transpose is rendering: keep playing what the clip played
                // until now, rather than dropping out.
                if (const auto it = lastClipAudio.find (clipId); it != lastClipAudio.end())
                    clip.audio = it->second;
                else
                    continue; // still loading (rebuilds when it's ready) or unreadable
            }

            clipAudio[clipId] = clip.audio;

            const auto toSamples = [this] (const juce::var& seconds) { return (juce::int64) std::llround ((double) seconds * sampleRate); };
            clip.start   = toSamples (clipTree[ids::start]);
            clip.offset  = juce::jmax ((juce::int64) 0, toSamples (clipTree[ids::offset]));
            clip.length  = juce::jmin (toSamples (clipTree[ids::length]), (juce::int64) clip.audio->getNumSamples() - clip.offset);
            clip.fadeIn  = juce::jmax ((juce::int64) 0, toSamples (clipTree[ids::fadeIn]));
            clip.fadeOut = juce::jmax ((juce::int64) 0, toSamples (clipTree[ids::fadeOut]));
            clip.gain    = volumeToGain (clipTree.getProperty (ids::gain, 0.0f));

            if (clip.length <= 0)
                continue;

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
    pool.releaseUnusedWarps (warpsInUse);

    length = end;
    graph->master->latencyAtBuild = graph->master->chain.getLatencySamples();
    latency = maxLatency + maxReturnLatency + graph->master->latencyAtBuild;

    updateMix();

    for (auto& level : newSends)
    {
        level->gain.reset (sampleRate, smoothingSeconds);
        level->gain.setCurrentAndTargetValue (level->target.load());
    }

    publish (std::move (graph));
}

void DawEngine::buildMidiEvents (Track& track, const juce::ValueTree& trackTree, double rate, juce::int64& end)
{
    const auto toSamples = [rate] (double seconds) { return (juce::int64) std::llround (seconds * rate); };
    const auto transpose = (int) std::lround ((double) trackTree.getProperty (ids::transpose, 0.0));

    for (const auto& clip : trackTree)
    {
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

    if (armedAudio)
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
    recordingMidi = armedMidi;

    const auto start = position.load();
    recordStart = start;

    // Loop recording: starting inside the loop with it on, playback wraps as usual and
    // every pass becomes a take (audio only: MIDI takes run straight on).
    loopRecording = armedAudio && ! armedMidi && loopOn.load() && start >= loopStart.load() && start < loopEnd.load();

    if (const auto bars = countInBars.load(); bars > 0)
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

Recorder::Take DawEngine::stopRecording()
{
    playing = false;
    countingIn = false;
    const auto stopAt = position.load();
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
            if (clickOn || (counting && t < start))
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

        if (time < start || ! (message.isNoteOnOrOff() || message.isSustainPedalOn() || message.isSustainPedalOff()))
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
        for (int note = 0; note < 128; ++note)
            if (strip.sounding[(size_t) note])
                midi.addEvent (juce::MidiMessage::noteOff (1, note), 0);

        strip.sounding.reset();
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

    // The dry takes: the raw inputs from the record start on (not during the count-in).
    if (isPlaying && recordingAudio.load())
    {
        const auto start = recordStart.load();
        const auto skip = (int) juce::jlimit ((juce::int64) 0, (juce::int64) numSamples, start - blockStart);

        if (skip < numSamples)
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
                take.recorder->write (channels, nullptr, numSamples - skip, blockStart + skip);
            }
        }
    }

    if (isPlaying && midi != nullptr && recordingMidi.load())
        recordMidi (*midi, midiOffset, numSamples, blockStart);

    auto& master = *graph.master;
    master.buffer.clear (0, numSamples);

    for (auto& bus : graph.returns)
        bus.strip->buffer.clear (0, numSamples);

    const auto delayDirect = graph.directDelay.samples > 0;
    auto& direct = delayDirect ? graph.direct : master.buffer;
    if (delayDirect)
        direct.clear (0, numSamples);

    // A track's sends: its signal into the returns, gliding to new levels.
    const auto feedSends = [&graph, numSamples] (Track& track, bool pre)
    {
        for (auto& send : track.sends)
        {
            if (send.pre != pre)
                continue;

            auto& gain = send.level->gain;
            gain.setTargetValue (send.level->target.load (std::memory_order_relaxed));
            const auto from = gain.getCurrentValue();
            const auto to = gain.skip (numSamples);

            if (from == 0.0f && to == 0.0f)
                continue;

            auto& into = graph.returns[(size_t) send.bus].strip->buffer;
            into.addFromWithRamp (0, 0, track.strip->buffer.getReadPointer (0), numSamples, from, to);
            into.addFromWithRamp (1, 0, track.strip->buffer.getReadPointer (1), numSamples, from, to);
        }
    };

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

        strip.left.setTargetValue (strip.targetLeft.load (std::memory_order_relaxed));
        strip.right.setTargetValue (strip.targetRight.load (std::memory_order_relaxed));

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

        strip.left.setTargetValue (strip.targetLeft.load (std::memory_order_relaxed));
        strip.right.setTargetValue (strip.targetRight.load (std::memory_order_relaxed));
        strip.buffer.applyGainRamp (0, 0, numSamples, strip.left.getCurrentValue(), strip.left.skip (numSamples));
        strip.buffer.applyGainRamp (1, 0, numSamples, strip.right.getCurrentValue(), strip.right.skip (numSamples));
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

        // Looping: stop the chunk at the loop end, then jump back to the start.
        const auto looping = loopOn.load() && playing.load() && (! isRecording() || loopRecording.load());
        const auto end = loopEnd.load();

        if (looping && position.load() == end)
            position = loopStart.load();

        if (const auto pos = position.load(); looping && pos < end && pos + chunk > end)
            chunk = (int) (end - pos);

        std::array<const float*, Edit::maxInputs> chunkInputs {};
        for (int i = 0; i < numInputs; ++i)
            chunkInputs[(size_t) i] = inputs[i] != nullptr ? inputs[i] + done : nullptr;

        renderBlock (*active, output, done, numChannels, chunk, chunkInputs.data(), numInputs, midi, done);

        if (looping && position.load() == end)
            position = loopStart.load();

        done += chunk;
    }
}
}
