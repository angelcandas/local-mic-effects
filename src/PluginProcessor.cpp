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
                          .withInput  ("Input",  juce::AudioChannelSet::mono(),   true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      params (layout),
      state (*this, nullptr, "MicFX", std::move (layout)),
      pitchModule (*chain.find<fx::modules::Pitch>())
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
    multitrack.prepare (sampleRate);
    dawEngine.prepare (sampleRate, samplesPerBlock);
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
        updatePeak (inputPeak, numIn > 0 ? buffer.getMagnitude (0, 0, numSamples) : 0.0f);
        // The standalone app passes the MIDI input here (MIDI tracks play and record it).
        dawEngine.process (buffer, numOut, numSamples, numIn > 0 ? buffer.getReadPointer (0) : nullptr, &midi);
        midi.clear();
        applyPeakGuard (buffer, numOut, numSamples);
        updatePeak (outputPeak, buffer.getMagnitude (0, numSamples));
        return;
    }

    // Keep the raw mic for the recorder before anything touches it.
    const auto recording = recorder.isRecording();
    const auto dryLength = juce::jmin (numSamples, dryInput.getNumSamples());

    if (recording && numIn > 0)
    {
        dryInput.copyFrom (0, 0, buffer, 0, 0, dryLength);
        if (dryLength < numSamples)
            dryInput.clear(); // oversized block: the dry file gets silence rather than garbage
    }

    // A mono mic feeds every output channel so stereo effects have signal on both sides.
    if (numIn == 1)
        for (int ch = 1; ch < numOut; ++ch)
            buffer.copyFrom (ch, 0, buffer, 0, 0, numSamples);
    else
        for (auto ch = numIn; ch < numOut; ++ch)
            buffer.clear (ch, 0, numSamples);

    updateDspParameters();

    auto block = juce::dsp::AudioBlock<float> (buffer).getSubsetChannelBlock (0, (size_t) numOut);
    const fx::Context context (block);

    inputGain.process (context);
    updatePeak (inputPeak, buffer.getMagnitude (0, numSamples));

    if (! dryCompare.load())
        chain.process (context);

    outputGain.process (context);
    limiter.process (context);

    // Where this block sits on the multitrack timeline. The playback is added after the
    // voice is recorded, so takes contain only the voice.
    const auto timelinePosition = multitrack.getPosition();

    if (recording)
    {
        // Write in chunks the dry buffer can cover so both files stay sample-aligned.
        for (int start = 0; start < numSamples; start += dryInput.getNumSamples())
        {
            const auto length = juce::jmin (dryInput.getNumSamples(), numSamples - start);
            std::array<const float*, 2> output { buffer.getReadPointer (0, start),
                                                 buffer.getReadPointer (juce::jmin (1, numOut - 1), start) };
            recorder.write (output.data(), dryInput.getReadPointer (0), length, timelinePosition + start);
        }
    }

    if (multitrack.isPlaying())
    {
        multitrack.process (buffer, numOut, numSamples);
        applyPeakGuard (buffer, numOut, numSamples);
    }

    if (outputMuted.load())
        buffer.clear();

    updatePeak (outputPeak, buffer.getMagnitude (0, numSamples));
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

    currentPreset = index;
    applyPreset (*this, presets[(size_t) index]);
}

const juce::String PluginProcessor::getProgramName (int index)
{
    const auto& presets = getPresets();
    return juce::isPositiveAndBelow (index, (int) presets.size()) ? presets[(size_t) index].name : "";
}

void PluginProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto tree = state.copyState();
    tree.setProperty ("preset", currentPreset.load(), nullptr);

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
            state.replaceState (tree);
        }
    }
}

//==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new PluginProcessor();
}
