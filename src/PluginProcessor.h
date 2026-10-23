#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>

#include "Parameters.h"
#include "dsp/Effects.h"
#include "dsp/PitchEngine.h"
#include "dsp/SpectralDenoiser.h"

// Signal chain, in order (every module except gain/reverb/limiter is skipped when off):
//   input gain -> high-pass -> denoise -> gate -> autotune / pitch shift / harmonizer -> EQ
//   -> compressor -> de-esser -> exciter -> saturation -> radio -> bitcrusher -> robot
//   -> filter -> auto-wah -> tremolo -> vibrato -> chorus -> flanger -> phaser
//   -> doubler -> echo -> reverb -> stereo width -> output gain -> safety limiter
class PluginProcessor final : public juce::AudioProcessor,
                              private juce::AsyncUpdater
{
public:
    PluginProcessor();

    //==============================================================================
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;

    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    //==============================================================================
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override                       { return true; }

    const juce::String getName() const override           { return JucePlugin_Name; }
    bool acceptsMidi() const override                     { return false; }
    bool producesMidi() const override                    { return false; }
    bool isMidiEffect() const override                    { return false; }
    double getTailLengthSeconds() const override          { return 5.0; }

    // Programs are the factory voice presets (see Presets.cpp).
    int getNumPrograms() override;
    int getCurrentProgram() override                      { return currentPreset; }
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    //==============================================================================
    juce::AudioProcessorValueTreeState& getState() noexcept { return state; }
    const fx::PitchEngine& getPitchEngine() const noexcept  { return pitchEngine; }
    const fx::SpectralDenoiser& getDenoiser() const noexcept { return denoiser; }

    // Peak levels (linear gain) since the UI last read them. Written by the audio
    // thread, drained with exchange (0) by the editor.
    std::atomic<float> inputPeak  { 0.0f };
    std::atomic<float> outputPeak { 0.0f };

    std::atomic<int> currentPreset { 0 };

private:
    explicit PluginProcessor (juce::AudioProcessorValueTreeState::ParameterLayout layout);

    void updateDspParameters();
    void handleAsyncUpdate() override; // reports the denoiser's latency to the host

    Parameters params;
    juce::AudioProcessorValueTreeState state;

    juce::dsp::Gain<float> inputGain, outputGain;
    juce::dsp::StateVariableTPTFilter<float> highPass;
    fx::SpectralDenoiser denoiser;
    juce::dsp::NoiseGate<float> gate;
    fx::PitchEngine pitchEngine;
    fx::PitchEngine::Settings pitchSettings;
    fx::Equalizer eq;
    juce::dsp::Compressor<float> compressor;
    fx::DeEsser deEsser;
    fx::Exciter exciter;
    fx::Saturation saturation;
    fx::Radio radio;
    fx::Bitcrusher bitcrusher;
    fx::RingModulator robot;
    fx::ResonantFilter filter;
    fx::AutoWah autoWah;
    fx::Tremolo tremolo;
    fx::Vibrato vibrato;
    juce::dsp::Chorus<float> chorus, flanger;
    juce::dsp::Phaser<float> phaser;
    fx::Doubler doubler;
    fx::Echo echo;
    juce::dsp::Reverb reverb;
    fx::StereoWidth width;
    juce::dsp::Limiter<float> limiter;

    // Which switchable modules ran last block, so one switched back on starts clean.
    std::array<bool, 32> moduleWasOn {};
    bool pitchWasOn = false;
    std::atomic<bool> denoiserLatencyActive { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginProcessor)
};
