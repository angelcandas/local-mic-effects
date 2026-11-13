#include "PluginProcessor.h"
#include "Presets.h"

namespace
{
    // Lock-free "max since last read" so short peaks aren't missed between UI frames.
    void updatePeak (std::atomic<float>& peak, float value) noexcept
    {
        auto current = peak.load (std::memory_order_relaxed);
        while (value > current && ! peak.compare_exchange_weak (current, value, std::memory_order_relaxed)) {}
    }
}

//==============================================================================
PluginProcessor::PluginProcessor()
    : PluginProcessor (juce::AudioProcessorValueTreeState::ParameterLayout{})
{
}

PluginProcessor::PluginProcessor (juce::AudioProcessorValueTreeState::ParameterLayout layout)
    : AudioProcessor (BusesProperties()
                          // In the app, every input of the audio interface (up to maxAppInputs):
                          // DAW tracks each pick theirs. Live mode uses the first.
                          .withInput  ("Input",  juce::JUCEApplicationBase::isStandaloneApp()
                                                     ? juce::AudioChannelSet::discreteChannels (maxAppInputs)
                                                     : juce::AudioChannelSet::mono(),   true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      params (layout),
      state (*this, nullptr, "MicFX", std::move (layout)),
      pitchModule (*chain.find<fx::modules::Pitch>()),
      autoLevelModule (*chain.find<fx::modules::AutoLevel>())
{
    using Kind = Binding::Kind;

    for (int m = 0; m < chain.size(); ++m)
    {
        auto& module = chain[m];

        for (int v = 0; v < module.getNumValues(); ++v)
        {
            auto* parameter = state.getParameter (module.getValueId (v));
            jassert (parameter != nullptr); // every module value must have a plugin parameter

            if (parameter == nullptr)
                continue;

            const auto kind = dynamic_cast<juce::AudioParameterBool*> (parameter) != nullptr   ? Kind::toggle
                            : dynamic_cast<juce::AudioParameterChoice*> (parameter) != nullptr ? Kind::choice
                                                                                               : Kind::number;
            bindings.push_back ({ &module.getValue (v), parameter, kind });
        }
    }

    liveHistory.getPreset = [this] { return currentPreset.load(); };
    liveHistory.setPreset = [this] (int index) { currentPreset = index; };

    // MIDI learn's own actions: step through the factory presets, the panic mute.
    midiMap.onPresetStep = [this] (int delta)
    {
        const auto count = getNumPrograms();
        if (count > 0)
            setCurrentProgram (((currentPreset.load() + delta) % count + count) % count);
    };
    midiMap.onMuteToggle = [this] { outputMuted = ! outputMuted.load(); };
    midiMap.onScene = [this] (const juce::String& action)
    {
        if (action == "scene.next")          scenes.step (1);
        else if (action == "scene.previous") scenes.step (-1);
        else                                 scenes.recall (action.fromFirstOccurrenceOf ("scene.", false, false).getIntValue() - 1);
    };

    // Saves and backups keep the instruments' current settings (plugin knobs, sounds).
    dawProject.onBeforeWrite = [this] { dawEngine.captureInstrumentStates(); };

    startTimerHz (5);
}

void PluginProcessor::updateLatency()
{
    const auto latency = dawMode.load() ? dawEngine.getLatencySamples() : chain.getLatencySamples();

    if (latency != getLatencySamples())
        setLatencySamples (latency);
}

//==============================================================================
bool PluginProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto in  = layouts.getMainInputChannelSet();
    const auto out = layouts.getMainOutputChannelSet();

    const auto isMonoOrStereo = [] (const juce::AudioChannelSet& set)
    {
        return set == juce::AudioChannelSet::mono() || set == juce::AudioChannelSet::stereo();
    };

    // The app takes any number of inputs (as many as the interface has open).
    if (juce::JUCEApplicationBase::isStandaloneApp())
        return in.size() <= maxAppInputs && isMonoOrStereo (out);

    return isMonoOrStereo (in) && isMonoOrStereo (out) && in.size() <= out.size();
}

void PluginProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    const juce::dsp::ProcessSpec spec { sampleRate,
                                        (juce::uint32) samplesPerBlock,
                                        (juce::uint32) juce::jmax (1, getTotalNumOutputChannels()) };

    inputGain.prepare (spec);
    outputGain.prepare (spec);
    limiter.prepare (spec);

    inputGain.setRampDurationSeconds (0.05);
    outputGain.setRampDurationSeconds (0.05);

    // Brick-wall-ish protection for your ears (and your listeners').
    limiter.setThreshold (-1.0f);
    limiter.setRelease (100.0f);

    updateDspParameters();
    chain.prepare (spec); // starts every module from its current values

    recorder.prepare (sampleRate, (int) spec.numChannels);
    liveCapture.configure (sampleRate, 3, captureSeconds.load());
    multitrack.prepare (sampleRate);
    looper.prepare (sampleRate);
    pitchTap.prepare (sampleRate);
    soundCheck.prepare (sampleRate);
    loopInput.setSize (2, juce::jmax (samplesPerBlock, 4096));
    loopInput.clear();
    backingBuffer.setSize (juce::jmax (2, getTotalNumOutputChannels()), juce::jmax (samplesPerBlock, 4096));
    backingMeter.prepare (sampleRate);
    backingWasPlaying = false;
    dawEngine.prepare (sampleRate, samplesPerBlock);
    chordFollower.prepare (sampleRate);
    mixGuardGain = 1.0f;
    mixGuardRelease = 1.0f - std::exp (-1.0f / (0.1f * (float) sampleRate)); // ~100 ms
    // Room for blocks a bit larger than announced; hosts occasionally send them.
    dryInput.setSize (1, juce::jmax (samplesPerBlock, 4096));
    dryInput.clear();

    // Start from the current values instead of ramping from the defaults.
    inputGain.reset();
    outputGain.reset();
    limiter.reset();

    updateLatency();
}

void PluginProcessor::updateDspParameters()
{
    inputGain.setGainDecibels (params.inputGain.get());
    outputGain.setGainDecibels (params.outputGain.get());

    using Kind = Binding::Kind;

    for (const auto& binding : bindings)
    {
        const auto value = binding.kind == Kind::toggle ? (static_cast<juce::AudioParameterBool*> (binding.parameter)->get() ? 1.0f : 0.0f)
                         : binding.kind == Kind::choice ? (float) static_cast<juce::AudioParameterChoice*> (binding.parameter)->getIndex()
                                                        : static_cast<juce::AudioParameterFloat*> (binding.parameter)->get();
        binding.value->store (value, std::memory_order_relaxed);
    }
}

void PluginProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;

    const auto numIn      = getTotalNumInputChannels();
    const auto numOut     = getTotalNumOutputChannels();
    const auto numSamples = buffer.getNumSamples();

    if (dawMode.load())
    {
        // The raw mic goes to the DAW engine (recorded dry, monitored through the
        // armed track's inserts); the engine writes the mix over the buffer.
        auto inputLevel = 0.0f;
        for (int ch = 0; ch < numIn; ++ch)
            inputLevel = juce::jmax (inputLevel, buffer.getMagnitude (ch, 0, numSamples));
        updatePeak (inputPeak, inputLevel);
        inputChannels = numIn;

        // The standalone app passes the MIDI input here (MIDI tracks play and record it).
        dawEngine.process (buffer, numOut, numSamples, buffer.getArrayOfReadPointers(), numIn, &midi);
        midi.clear();
        applyPeakGuard (buffer, numOut, numSamples);
        updatePeak (outputPeak, buffer.getMagnitude (0, numSamples));
        return;
    }

    // Mapped pedals, knobs and keys (MIDI learn). Looper pedals act before this block's audio.
    midiMap.process (midi);

    // Keep the raw mic for the recorder before anything touches it.
    const auto recording = recorder.isRecording();
    const auto dryLength = juce::jmin (numSamples, dryInput.getNumSamples());

    const auto capturing = liveCapture.isOn();

    if ((recording || capturing) && numIn > 0)
    {
        dryInput.copyFrom (0, 0, buffer, 0, 0, dryLength);
        if (dryLength < numSamples)
            dryInput.clear(); // oversized block: the dry file gets silence rather than garbage
    }
    else if (capturing)
        dryInput.clear(); // no input: Capture keeps silence for the dry channel

    updateDspParameters();
    followChords (buffer, numIn, numSamples, midi); // before the mic is copied over the other inputs

    // A mono mic feeds every output channel so stereo effects have signal on both sides.
    // The app has the interface's every input: Live mode uses the first, as a mono mic.
    if (numIn == 1 || (numIn > 1 && juce::JUCEApplicationBase::isStandaloneApp()))
        for (int ch = 1; ch < numOut; ++ch)
            buffer.copyFrom (ch, 0, buffer, 0, 0, numSamples);
    else
        for (auto ch = numIn; ch < numOut; ++ch)
            buffer.clear (ch, 0, numSamples);

    auto block = juce::dsp::AudioBlock<float> (buffer).getSubsetChannelBlock (0, (size_t) numOut);
    const fx::Context context (block);

    inputGain.process (context);
    updatePeak (inputPeak, buffer.getMagnitude (0, numSamples));

    // The Practice view follows the singer's own pitch, before any effect touches it.
    pitchTap.process (buffer.getReadPointer (0), numSamples, multitrack.getPosition());
    soundCheck.capture (buffer.getReadPointer (0), numSamples); // the same raw mic, for Sound check

    if (! dryCompare.load())
        chain.process (context);

    outputGain.process (context);

    // The looper hears the voice before the limiter; its layers are added after it.
    const auto looping = looper.getState() != Looper::State::empty || looper.hasPendingCommands();
    const auto loopChunk = loopInput.getNumSamples();
    if (looping)
        for (int ch = 0; ch < juce::jmin (2, numOut); ++ch)
            loopInput.copyFrom (ch, 0, buffer, ch, 0, juce::jmin (numSamples, loopChunk));

    limiter.process (context);

    if (looping)
    {
        for (int start = 0; start < numSamples; start += loopChunk)
        {
            const auto length = juce::jmin (loopChunk, numSamples - start);
            if (start > 0)
                for (int ch = 0; ch < juce::jmin (2, numOut); ++ch)
                    loopInput.copyFrom (ch, 0, buffer, ch, start, length); // oversized block: limited, but rare

            juce::AudioBuffer<float> output (buffer.getArrayOfWritePointers(), juce::jmin (2, numOut), start, length);
            looper.process (loopInput, output, juce::jmin (2, numOut), length);
        }
        applyPeakGuard (buffer, numOut, numSamples);
    }

    // Where this block sits on the multitrack timeline. The playback is added after the
    // voice is recorded, so takes contain only the voice.
    const auto timelinePosition = multitrack.getPosition();

    if (recording || capturing)
    {
        // Write in chunks the dry buffer can cover so both files stay sample-aligned.
        const auto playingBacking = multitrack.isPlaying();

        for (int start = 0; start < numSamples; start += dryInput.getNumSamples())
        {
            const auto length = juce::jmin (dryInput.getNumSamples(), numSamples - start);
            std::array<const float*, 2> output { buffer.getReadPointer (0, start),
                                                 buffer.getReadPointer (juce::jmin (1, numOut - 1), start) };

            if (recording)
                recorder.write (output.data(), dryInput.getReadPointer (0), length, timelinePosition + start);

            if (capturing)
            {
                // The voice after the limiter and the looper, and the raw mic. Capture keeps the
                // timeline position while the backing plays so a capture can sit against it.
                const std::array<const float*, 3> kept { output[0], output[1], dryInput.getReadPointer (0) };
                liveCapture.write (kept.data(), length, playingBacking ? timelinePosition + start : -1);
            }
        }
    }

    if (multitrack.isPlaying())
    {
        // The backing on its own first, measured for auto level (the voice's next block
        // aims over it), then added under the voice.
        const auto chunk = backingBuffer.getNumSamples();
        const auto channels = juce::jmin (numOut, backingBuffer.getNumChannels());
        for (int start = 0; start < numSamples; start += chunk)
        {
            const auto length = juce::jmin (chunk, numSamples - start);
            backingBuffer.clear();
            multitrack.process (backingBuffer, channels, length);
            for (int ch = 0; ch < channels; ++ch)
                buffer.addFrom (ch, start, backingBuffer, ch, 0, length);
            backingMeter.process (juce::dsp::AudioBlock<float> (backingBuffer).getSubsetChannelBlock (0, (size_t) channels)
                                                                               .getSubBlock (0, (size_t) length));
        }
        autoLevelModule.setReference (backingMeter.getMomentary());
        backingWasPlaying = true;
        applyPeakGuard (buffer, numOut, numSamples);
    }
    else if (backingWasPlaying)
    {
        backingMeter.reset();
        autoLevelModule.setReference (fx::modules::AutoLevel::silence);
        backingWasPlaying = false;
    }

    if (outputMuted.load())
        buffer.clear();

    updatePeak (outputPeak, buffer.getMagnitude (0, numSamples));
}

void PluginProcessor::followChords (const juce::AudioBuffer<float>& buffer, int numIn, int numSamples,
                                    const juce::MidiBuffer& midi) noexcept
{
    // The keys down, kept up to date even while nothing follows them.
    for (const auto metadata : midi)
    {
        const auto message = metadata.getMessage();
        if (message.isNoteOn())
            heldKeys.set ((size_t) message.getNoteNumber());
        else if (message.isNoteOff())
            heldKeys.reset ((size_t) message.getNoteNumber());
        else if (message.isAllNotesOff() || message.isAllSoundOff())
            heldKeys.reset();
    }

    if (! pitchModule.isFollowingChords())
        return;

    using Pitch = fx::modules::Pitch;

    // The autotune key, when it's a major or minor one, as a hint for the chords.
    const auto scale = juce::roundToInt (pitchModule.tuneScale.load (std::memory_order_relaxed));
    const auto key = juce::roundToInt (pitchModule.tuneKey.load (std::memory_order_relaxed));
    chordFollower.setKeyMask (scale == 1 || scale == 2 ? analysis::getKeyMask (key, scale == 2) : 0);

    if (heldKeys.any() || pitchModule.getHarmonySource() == Pitch::midiKeys)
    {
        // Held keys decide; released, the last chord they made stays.
        chordFollower.setHeldNotes (heldKeys);
    }
    else if (pitchModule.getHarmonySource() == Pitch::instrumentInput)
    {
        // The second input (a guitar or keyboard on input 2 of the interface).
        if (numIn > 1)
            chordFollower.pushAudio (buffer.getReadPointer (1), numSamples);
    }
    else if (const auto chord = multitrack.getBackingChord (multitrack.getPosition()); chord.isValid())
    {
        chordFollower.setChord (chord);
    }

    pitchModule.setChord (chordFollower.getChord());
}

analysis::Chord PluginProcessor::getHarmonyChord() const noexcept
{
    if (dawMode.load())
        return dawEngine.getHarmonyChord();

    return analysis::Chord::unpack (pitchModule.getEngine().harmonyChord.load (std::memory_order_relaxed));
}

void PluginProcessor::applyPeakGuard (juce::AudioBuffer<float>& buffer, int numChannels, int numSamples) noexcept
{
    constexpr auto ceiling = 0.89125f; // -1 dB

    for (int i = 0; i < numSamples; ++i)
    {
        float peak = 0.0f;
        for (int ch = 0; ch < numChannels; ++ch)
            peak = juce::jmax (peak, std::abs (buffer.getSample (ch, i)));

        if (peak * mixGuardGain > ceiling)
            mixGuardGain = ceiling / peak; // instant attack: never overshoots
        else
            mixGuardGain += (1.0f - mixGuardGain) * mixGuardRelease;

        if (mixGuardGain < 0.9999f)
            for (int ch = 0; ch < numChannels; ++ch)
                buffer.setSample (ch, i, buffer.getSample (ch, i) * mixGuardGain);
    }
}

//==============================================================================
int PluginProcessor::getNumPrograms()
{
    return (int) getPresets().size();
}

void PluginProcessor::setCurrentProgram (int index)
{
    const auto& presets = getPresets();

    if (! juce::isPositiveAndBelow (index, (int) presets.size()))
        return;

    if (juce::MessageManager::existsAndIsCurrentThread())
        liveHistory.label (juce::String ("Load preset ") + presets[(size_t) index].name);
    currentPreset = index;
    applyPreset (*this, presets[(size_t) index]);
}

const juce::String PluginProcessor::getProgramName (int index)
{
    const auto& presets = getPresets();
    return juce::isPositiveAndBelow (index, (int) presets.size()) ? presets[(size_t) index].name : "";
}

void PluginProcessor::setCaptureSeconds (double seconds)
{
    captureSeconds = juce::jmax (0.0, seconds);
    dawEngine.setCaptureSeconds (captureSeconds.load()); // the DAW's armed inputs follow the same setting
    if (getSampleRate() > 0.0)
        liveCapture.configure (getSampleRate(), 3, captureSeconds.load());
}

juce::String PluginProcessor::setModuleFile (fx::ModuleType type, const juce::File& file)
{
    auto* module = chain.find (type);
    if (module == nullptr || ! module->usesFile())
        return "This effect doesn't use files.";

    if (const auto error = module->loadFile (file); error.isNotEmpty())
        return error;

    if (file != juce::File())
        if (const auto choice = module->getFileChoice(); choice.valueId.isNotEmpty())
            if (auto* parameter = state.getParameter (choice.valueId))
                parameter->setValueNotifyingHost (parameter->convertTo0to1 (choice.index));

    return {};
}

juce::File PluginProcessor::getModuleFile (fx::ModuleType type) const
{
    const auto* module = const_cast<fx::FxChain&> (chain).find (type);
    return module != nullptr ? module->getFile() : juce::File();
}

void PluginProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto tree = state.copyState();
    tree.setProperty ("preset", currentPreset.load(), nullptr);

    // The effects' files, by module key.
    juce::ValueTree files ("MODULE_FILES");
    for (int i = 0; i < chain.size(); ++i)
        if (const auto& module = chain[i]; module.usesFile() && module.getFile() != juce::File())
            files.setProperty (fx::getModuleKey (module.getType()), module.getFile().getFullPathName(), nullptr);
    if (files.getNumProperties() > 0)
        tree.appendChild (files, nullptr);
    if (juce::JUCEApplicationBase::isStandaloneApp() && (setlist.size() > 0 || setlist.getFile() != juce::File()))
        tree.appendChild (setlist.getSnapshot(), nullptr); // the app's setlist survives a restart
    if (juce::JUCEApplicationBase::isStandaloneApp() && juce::MessageManager::existsAndIsCurrentThread())
        if (auto singAlong = multitrack.saveSession(); singAlong.getNumChildren() > 0)
            tree.appendChild (singAlong, nullptr); // the sing-along rows survive a restart

    if (auto xml = tree.createXml())
        copyXmlToBinary (*xml, destData);
}

void PluginProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
    {
        if (xml->hasTagName (state.state.getType()))
        {
            auto tree = juce::ValueTree::fromXml (*xml);
            currentPreset = (int) tree.getProperty ("preset", 0);
            if (const auto saved = tree.getChildWithName ("SETLIST"); saved.isValid())
            {
                if (juce::JUCEApplicationBase::isStandaloneApp())
                    setlist.restore (saved);
                tree.removeChild (saved, nullptr);
            }

            if (const auto singAlong = tree.getChildWithName ("SINGALONG"); singAlong.isValid())
            {
                if (juce::JUCEApplicationBase::isStandaloneApp() && juce::MessageManager::existsAndIsCurrentThread())
                    multitrack.restoreSession (singAlong);
                tree.removeChild (singAlong, nullptr);
            }

            const auto files = tree.getChildWithName ("MODULE_FILES");
            tree.removeChild (files, nullptr);
            for (int i = 0; i < chain.size(); ++i)
                if (auto& module = chain[i]; module.usesFile())
                {
                    const auto path = files.getProperty (fx::getModuleKey (module.getType())).toString();
                    const auto error = module.loadFile (juce::File::isAbsolutePath (path) ? juce::File (path) : juce::File());
                    if (error.isNotEmpty())
                        juce::Logger::writeToLog ("[MicFX] " + error);
                }

            state.replaceState (tree);
            liveHistory.clear(); // the loaded sound is where undo starts
        }
    }
}

//==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new PluginProcessor();
}
