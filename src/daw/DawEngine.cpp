#include "DawEngine.h"

namespace daw
{
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
    recorder.prepare (sampleRate, 1);

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
    recorder.prepare (sampleRate, 1); // stops (and keeps) a take in progress
    countingIn = false;
    updateTempo();

    // Fresh strips: every chain is prepared for the new rate and block size.
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
        for (const auto& clip : track)
            if (clip.hasType (ids::CLIP))
                pool.loadNow (juce::File (clip[ids::file].toString()));

    rebuildNow();
}

float DawEngine::takePeak (int trackId)
{
    const auto it = strips.find (trackId);
    return it != strips.end() ? it->second->peak.exchange (0.0f) : 0.0f;
}

//==============================================================================
void DawEngine::valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier& property)
{
    if (tree.hasType (ids::INSERT))
    {
        if (property == ids::type)
        {
            triggerAsyncUpdate();
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
            if (it->second->chain.getLatencySamples() != it->second->latencyAtBuild)
                triggerAsyncUpdate();
        }

        return;
    }

    if (tree.hasType (ids::PROJECT))
    {
        if (property == ids::tempo)
            updateTempo();
        else if (property == ids::loopOn || property == ids::loopStart || property == ids::loopEnd)
            updateLoop();

        return;
    }

    if (tree.hasType (ids::TRACK) || tree.hasType (ids::MASTER))
    {
        if (property == ids::volume || property == ids::pan || property == ids::mute || property == ids::solo
            || property == ids::monitor)
            updateMix();

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

    if (jump)
    {
        strip.left.reset (sampleRate, smoothingSeconds);
        strip.right.reset (sampleRate, smoothingSeconds);
        strip.left.setCurrentAndTargetValue (strip.targetLeft);
        strip.right.setCurrentAndTargetValue (strip.targetRight);
    }
}

void DawEngine::updateMix()
{
    const auto tracks = edit.getTracks();
    const auto anySolo = std::any_of (tracks.begin(), tracks.end(), [] (const auto& t) { return (bool) t[ids::solo]; });

    for (const auto& track : tracks)
    {
        if (const auto it = strips.find ((int) track[ids::id]); it != strips.end())
        {
            setStripTargets (*it->second, track, ! (bool) track[ids::mute] && (! anySolo || (bool) track[ids::solo]), false);
            it->second->monitoring = (bool) track[ids::monitor];
        }
    }

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

    if (const auto it = strips.find (id); it != strips.end() && it->second->insertLayout == layout)
        return it->second;

    // New track, or its inserts were added, removed or reordered: a new chain. (Insert
    // values alone never get here; they go straight into the running modules.)
    auto strip = std::make_shared<Strip>();
    strip->id = id;
    strip->insertLayout = layout;

    for (const auto& insert : inserts)
    {
        const auto type = Edit::moduleTypeFromKey (insert[ids::type].toString());
        if (! type.has_value())
            continue; // a module from a newer version: skipped rather than refusing the project

        auto& module = strip->chain.add (fx::createModule (*type));

        for (int v = 0; v < module.getNumValues(); ++v)
            if (const auto& value = insert[juce::Identifier (module.getValueId (v))]; ! value.isVoid())
                module.getValue (v).store ((float) value);

        strip->modulesByInsertId[(int) insert[ids::id]] = &module;
    }

    strip->chain.prepare ({ sampleRate, (juce::uint32) blockSize, 2 });
    strip->buffer.setSize (2, blockSize);
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

    juce::int64 end = 0;
    int maxLatency = 0;

    for (const auto& trackTree : edit.getTracks())
    {
        Track track;
        track.strip = reuse (trackTree);
        maxLatency = juce::jmax (maxLatency, track.strip->chain.getLatencySamples());

        for (const auto& clipTree : trackTree)
        {
            if (! clipTree.hasType (ids::CLIP))
                continue;

            Clip clip;
            clip.audio = pool.get (juce::File (clipTree[ids::file].toString()));
            if (clip.audio == nullptr)
                continue; // still loading (rebuilds when it's ready) or unreadable

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
            track.clips.push_back (std::move (clip));
        }

        graph->tracks.push_back (std::move (track));
    }

    // Delay every track to line up with the slowest chain.
    for (auto& track : graph->tracks)
    {
        track.strip->latencyAtBuild = track.strip->chain.getLatencySamples();
        track.delaySamples = maxLatency - track.strip->chain.getLatencySamples();

        if (track.delaySamples > 0)
        {
            track.delayBuffer.setSize (2, track.delaySamples);
            track.delayBuffer.clear();
        }
    }

    length = end;
    graph->master->latencyAtBuild = graph->master->chain.getLatencySamples();
    latency = maxLatency + graph->master->latencyAtBuild;

    updateMix();
    publish (std::move (graph));
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
    const auto bpm = juce::jlimit (20.0, 400.0, (double) edit.getState().getProperty (ids::tempo, 120.0));
    samplesPerBeat = sampleRate * 60.0 / bpm;
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
    if (std::none_of (tracks.begin(), tracks.end(), [] (const auto& t) { return (bool) t[ids::arm]; }))
        return "Arm a track to record on first.";

    if (const auto error = recorder.start (folder, false); error.isNotEmpty())
        return error;

    const auto start = position.load();
    recordStart = start;

    if (const auto bars = countInBars.load(); bars > 0)
    {
        countingIn = true;
        position = start - (juce::int64) std::llround (bars * 4 * samplesPerBeat.load());
    }

    playing = true;
    return {};
}

Recorder::Take DawEngine::stopRecording()
{
    playing = false;
    countingIn = false;

    const auto wasRecording = recorder.isRecording();
    auto take = recorder.stop();

    if (! wasRecording || ! take.file.existsAsFile() || take.seconds <= 0.0)
        return take;

    // Where the first sample was heard: the playhead minus the interface's round trip.
    auto start = (double) (take.timelineStart - recordingLatency.load()) / sampleRate;
    auto offset = 0.0;

    if (start < 0.0)
    {
        offset = -start;
        start = 0.0;
    }

    edit.getUndoManager().beginNewTransaction ("Record");

    for (const auto& track : edit.getTracks())
        if ((bool) track[ids::arm])
            edit.addClip ((int) track[ids::id], take.file, start, take.seconds - offset, offset);

    edit.getUndoManager().beginNewTransaction();
    return take;
}

void DawEngine::renderClick (juce::AudioBuffer<float>& output, int offset, int numChannels, int numSamples,
                             juce::int64 blockStart) noexcept
{
    const auto spb = samplesPerBeat.load();
    const auto counting = countingIn.load();
    const auto start = recordStart.load();
    const auto clickOn = metronome.load();

    for (int i = 0; i < numSamples; ++i)
    {
        const auto t = blockStart + i;

        if (clickOn || (counting && t < start))
        {
            const auto beat = (juce::int64) std::floor ((double) t / spb);

            if ((double) t - (double) beat * spb < 1.0) // a beat starts on this sample
            {
                const auto downbeat = ((beat % 4) + 4) % 4 == 0;
                clickPhase = 0.0f;
                clickIncrement = (downbeat ? 1500.0f : 1000.0f) / (float) sampleRate;
                clickLevel = downbeat ? 0.35f : 0.22f;
                clickDecay = (float) std::exp (-1.0 / (0.012 * sampleRate));
            }
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

void DawEngine::applyDelay (Track& track, int numSamples) noexcept
{
    if (track.delaySamples <= 0)
        return;

    auto& buffer = track.strip->buffer;

    for (int ch = 0; ch < 2; ++ch)
    {
        auto* data = buffer.getWritePointer (ch);
        auto* line = track.delayBuffer.getWritePointer (ch);
        auto write = track.delayWrite;

        for (int i = 0; i < numSamples; ++i)
        {
            std::swap (data[i], line[write]);
            write = (write + 1) % track.delaySamples;
        }

        if (ch == 1)
            track.delayWrite = write;
    }
}

void DawEngine::renderBlock (Graph& graph, juce::AudioBuffer<float>& output, int outputOffset, int numChannels,
                             int numSamples, const float* input) noexcept
{
    const auto isPlaying = playing.load();
    const auto blockStart = position.load();

    // The dry take: the raw mic from the record start on (not during the count-in).
    if (isPlaying && input != nullptr && recorder.isRecording())
    {
        const auto start = recordStart.load();
        const auto skip = (int) juce::jlimit ((juce::int64) 0, (juce::int64) numSamples, start - blockStart);

        if (skip < numSamples)
        {
            const float* channels[] = { input + skip };
            recorder.write (channels, nullptr, numSamples - skip, blockStart + skip);
        }

        if (blockStart + numSamples > start)
            countingIn = false;
    }

    auto& master = *graph.master;
    master.buffer.clear (0, numSamples);

    for (auto& track : graph.tracks)
    {
        auto& strip = *track.strip;
        strip.buffer.clear (0, numSamples);

        if (isPlaying)
            for (const auto& clip : track.clips)
                renderClip (clip, strip.buffer, blockStart, numSamples);

        applyDelay (track, numSamples);

        // Live mic after the delay line: monitoring shouldn't wait for compensation.
        if (input != nullptr && strip.monitoring.load (std::memory_order_relaxed))
        {
            strip.buffer.addFrom (0, 0, input, numSamples);
            strip.buffer.addFrom (1, 0, input, numSamples);
        }

        auto block = juce::dsp::AudioBlock<float> (strip.buffer).getSubBlock (0, (size_t) numSamples);
        strip.chain.process (fx::Module::Context (block));

        strip.left.setTargetValue (strip.targetLeft.load (std::memory_order_relaxed));
        strip.right.setTargetValue (strip.targetRight.load (std::memory_order_relaxed));

        auto* left = strip.buffer.getWritePointer (0);
        auto* right = strip.buffer.getWritePointer (1);

        for (int i = 0; i < numSamples; ++i)
        {
            left[i] *= strip.left.getNextValue();
            right[i] *= strip.right.getNextValue();
        }

        updatePeak (strip.peak, strip.buffer.getMagnitude (0, numSamples));
        master.buffer.addFrom (0, 0, strip.buffer, 0, 0, numSamples);
        master.buffer.addFrom (1, 0, strip.buffer, 1, 0, numSamples);
    }

    auto block = juce::dsp::AudioBlock<float> (master.buffer).getSubBlock (0, (size_t) numSamples);
    master.chain.process (fx::Module::Context (block));

    master.left.setTargetValue (master.targetLeft.load (std::memory_order_relaxed));
    master.right.setTargetValue (master.targetRight.load (std::memory_order_relaxed));
    master.buffer.applyGainRamp (0, 0, numSamples, master.left.getCurrentValue(), master.left.skip (numSamples));
    master.buffer.applyGainRamp (1, 0, numSamples, master.right.getCurrentValue(), master.right.skip (numSamples));
    updatePeak (master.peak, master.buffer.getMagnitude (0, numSamples));

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
        renderClick (output, outputOffset, numChannels, numSamples, blockStart);
        position = blockStart + numSamples;
    }
}

void DawEngine::process (juce::AudioBuffer<float>& output, int numChannels, int numSamples, const float* input) noexcept
{
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
        const auto looping = loopOn.load() && playing.load() && ! isRecording();
        const auto end = loopEnd.load();

        if (looping && position.load() == end)
            position = loopStart.load();

        if (const auto pos = position.load(); looping && pos < end && pos + chunk > end)
            chunk = (int) (end - pos);

        renderBlock (*active, output, done, numChannels, chunk, input != nullptr ? input + done : nullptr);

        if (looping && position.load() == end)
            position = loopStart.load();

        done += chunk;
    }
}
}
