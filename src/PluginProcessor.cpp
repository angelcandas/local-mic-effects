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

    float fraction (const juce::AudioParameterFloat& percent) noexcept
    {
        return percent.get() / 100.0f;
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
      state (*this, nullptr, "MicFX", std::move (layout))
{
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
    highPass.prepare (spec);
    denoiser.prepare (spec);
    gate.prepare (spec);
    pitchEngine.prepare (spec);
    eq.prepare (spec);
    compressor.prepare (spec);
    deEsser.prepare (spec);
    exciter.prepare (spec);
    saturation.prepare (spec);
    radio.prepare (spec);
    bitcrusher.prepare (spec);
    robot.prepare (spec);
    filter.prepare (spec);
    autoWah.prepare (spec);
    tremolo.prepare (spec);
    vibrato.prepare (spec);
    chorus.prepare (spec);
    flanger.prepare (spec);
    phaser.prepare (spec);
    doubler.prepare (spec);
    echo.prepare (spec);
    reverb.prepare (spec);
    width.prepare (spec);
    outputGain.prepare (spec);
    limiter.prepare (spec);

    inputGain.setRampDurationSeconds (0.05);
    outputGain.setRampDurationSeconds (0.05);

    highPass.setType (juce::dsp::StateVariableTPTFilterType::highpass);

    gate.setRatio (10.0f);
    gate.setAttack (1.0f);
    gate.setRelease (150.0f);

    compressor.setAttack (10.0f);
    compressor.setRelease (120.0f);

    chorus.setCentreDelay (7.0f);
    chorus.setFeedback (0.0f);
    flanger.setCentreDelay (2.5f);
    phaser.setCentreFrequency (1300.0f);

    // Brick-wall-ish protection for your ears (and your listeners').
    limiter.setThreshold (-1.0f);
    limiter.setRelease (100.0f);

    updateDspParameters();

    // Start from the current values instead of ramping from the defaults.
    inputGain.reset();
    outputGain.reset();
    reverb.reset();
    limiter.reset();
    moduleWasOn.fill (false);
    pitchWasOn = false;

    denoiserLatencyActive = params.denoise.on.get();
    setLatencySamples (denoiserLatencyActive ? denoiser.getLatencySamples() : 0);
}

void PluginProcessor::handleAsyncUpdate()
{
    setLatencySamples (denoiserLatencyActive ? denoiser.getLatencySamples() : 0);
}

void PluginProcessor::updateDspParameters()
{
    inputGain.setGainDecibels (params.inputGain.get());
    outputGain.setGainDecibels (params.outputGain.get());

    highPass.setCutoffFrequency (params.highPass.freq.get());
    denoiser.setParameters (params.denoise.reduction.get(), params.denoise.threshold.get(),
                            params.denoise.adaptive.get(), params.denoise.learn.get());
    gate.setThreshold (params.gate.threshold.get());

    pitchSettings.tuneOn           = params.autotune.on.get();
    pitchSettings.key              = params.autotune.key.getIndex();
    pitchSettings.scale            = params.autotune.scale.getIndex();
    pitchSettings.retuneMs         = params.autotune.speed.get();
    pitchSettings.amount           = fraction (params.autotune.amount);
    pitchSettings.shiftOn          = params.pitchShift.on.get();
    pitchSettings.semitones        = params.pitchShift.semitones.get();
    pitchSettings.formantSemitones = params.pitchShift.formant.get();
    pitchSettings.harmonyOn        = params.harmonizer.on.get();
    pitchSettings.interval1        = params.harmonizer.voice1.get();
    pitchSettings.interval2        = params.harmonizer.voice2.get();
    pitchSettings.harmonyMix       = fraction (params.harmonizer.mix);

    eq.setParameters (params.eq.low.get(), params.eq.midFreq.get(), params.eq.midGain.get(), params.eq.high.get());

    compressor.setThreshold (params.compressor.threshold.get());
    compressor.setRatio (params.compressor.ratio.get());

    deEsser.setParameters (params.deEsser.threshold.get(), params.deEsser.freq.get());
    exciter.setParameters (params.exciter.freq.get(), fraction (params.exciter.amount));
    saturation.setParameters (params.saturation.drive.get(), fraction (params.saturation.mix));
    radio.setParameters (params.radio.low.get(), params.radio.high.get());
    bitcrusher.setParameters (params.bitcrusher.bits.get(), params.bitcrusher.rate.get(), fraction (params.bitcrusher.mix));
    robot.setParameters (params.robot.freq.get(), fraction (params.robot.mix));
    filter.setParameters (params.filter.type.getIndex(), params.filter.cutoff.get(), fraction (params.filter.resonance));
    autoWah.setParameters (fraction (params.autoWah.sensitivity), params.autoWah.freq.get(), fraction (params.autoWah.mix));
    tremolo.setParameters (params.tremolo.rate.get(), fraction (params.tremolo.depth));
    vibrato.setParameters (params.vibrato.rate.get(), fraction (params.vibrato.depth));

    chorus.setRate (params.chorus.rate.get());
    chorus.setDepth (fraction (params.chorus.depth));
    chorus.setMix (fraction (params.chorus.mix));

    // juce::dsp::Chorus with a short delay and feedback is a flanger; depth 1 would
    // sweep +/-10 ms, so it's scaled to a flanger-sized +/-2 ms.
    flanger.setRate (params.flanger.rate.get());
    flanger.setDepth (0.2f * fraction (params.flanger.depth));
    flanger.setFeedback (fraction (params.flanger.feedback));
    flanger.setMix (fraction (params.flanger.mix));

    phaser.setRate (params.phaser.rate.get());
    phaser.setDepth (fraction (params.phaser.depth));
    phaser.setFeedback (fraction (params.phaser.feedback));
    phaser.setMix (fraction (params.phaser.mix));

    doubler.setParameters (fraction (params.doubler.mix), fraction (params.doubler.spread));
    echo.setParameters (params.echo.time.get(), fraction (params.echo.feedback), fraction (params.echo.mix), params.echo.pingPong.get());
    width.setParameters (fraction (params.width.amount));

    // Turning the reverb off fades the wet signal out (juce::Reverb smooths gain
    // changes) instead of cutting the tail abruptly.
    const auto mix = params.reverb.on.get() ? fraction (params.reverb.mix) : 0.0f;
    const auto angle = mix * juce::MathConstants<float>::halfPi;

    juce::dsp::Reverb::Parameters reverbParams;
    reverbParams.roomSize = fraction (params.reverb.size);
    reverbParams.damping  = fraction (params.reverb.damping);
    reverbParams.width    = fraction (params.reverb.width);
    // juce::Reverb scales dry by 2 and wet by 3 internally; dividing those out gives
    // an equal-power crossfade where 0% is the untouched dry signal.
    reverbParams.dryLevel = std::cos (angle) / 2.0f;
    reverbParams.wetLevel = std::sin (angle) / 3.0f;
    reverb.setParameters (reverbParams);
}

void PluginProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const auto numIn      = getTotalNumInputChannels();
    const auto numOut     = getTotalNumOutputChannels();
    const auto numSamples = buffer.getNumSamples();

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

    // Runs a module only while it's switched on, clearing its state when it comes back
    // on so stale samples from before the bypass don't click.
    size_t slot = 0;
    const auto run = [&] (auto& processor, const juce::AudioParameterBool& on)
    {
        auto& wasOn = moduleWasOn[slot++];
        const auto enabled = on.get();

        if (enabled && ! wasOn)
            processor.reset();

        wasOn = enabled;

        if (enabled)
            processor.process (context);
    };

    inputGain.process (context);
    updatePeak (inputPeak, buffer.getMagnitude (0, numSamples));

    run (highPass, params.highPass.on);
    run (denoiser, params.denoise.on);

    // The denoiser delays the signal by one FFT frame only while it's on; tell the
    // host (from the message thread) whenever that changes.
    if (params.denoise.on.get() != denoiserLatencyActive.load())
    {
        denoiserLatencyActive = params.denoise.on.get();
        triggerAsyncUpdate();
    }

    run (gate, params.gate.on);

    if (pitchSettings.isActive())
    {
        if (! pitchWasOn)
            pitchEngine.reset();

        pitchEngine.process (context, pitchSettings);
    }
    pitchWasOn = pitchSettings.isActive();

    run (eq,         params.eq.on);
    run (compressor, params.compressor.on);
    run (deEsser,    params.deEsser.on);
    run (exciter,    params.exciter.on);
    run (saturation, params.saturation.on);
    run (radio,      params.radio.on);
    run (bitcrusher, params.bitcrusher.on);
    run (robot,      params.robot.on);
    run (filter,     params.filter.on);
    run (autoWah,    params.autoWah.on);
    run (tremolo,    params.tremolo.on);
    run (vibrato,    params.vibrato.on);
    run (chorus,     params.chorus.on);
    run (flanger,    params.flanger.on);
    run (phaser,     params.phaser.on);
    run (doubler,    params.doubler.on);
    run (echo,       params.echo.on);

    reverb.process (context); // always runs; see updateDspParameters()

    run (width, params.width.on);

    outputGain.process (context);
    limiter.process (context);

    updatePeak (outputPeak, buffer.getMagnitude (0, numSamples));
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
