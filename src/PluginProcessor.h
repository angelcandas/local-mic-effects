#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>

#include "Parameters.h"
#include "Multitrack.h"
#include "Recorder.h"
#include "dsp/Effects.h"
#include "dsp/PitchEngine.h"

// Signal chain, in order (every module except gain/reverb/limiter is skipped when off):
//   input gain -> high-pass -> gate -> autotune / pitch shift / harmonizer -> EQ
//   -> compressor -> de-esser -> exciter -> saturation -> radio -> bitcrusher -> robot
//   -> filter -> auto-wah -> tremolo -> vibrato -> chorus -> flanger -> phaser
//   -> doubler -> echo -> reverb -> stereo width -> output gain -> safety limiter
class PluginProcessor final : public juce::AudioProcessor
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

    // Records the processed output (and optionally the dry mic) to WAV. Start/stop it
    // from the message thread; processBlock feeds it.
    Recorder& getRecorder() noexcept                        { return recorder; }

    // Backing track + recorded takes, mixed in after the voice chain (so the music
    // isn't processed or recorded with the voice). Message thread only.
    Multitrack& getMultitrack() noexcept                    { return multitrack; }

    // Peak levels (linear gain) since the UI last read them. Written by the audio
    // thread, drained with exchange (0) by the editor.
    std::atomic<float> inputPeak  { 0.0f };
    std::atomic<float> outputPeak { 0.0f };

    std::atomic<int> currentPreset { 0 };

private:
    explicit PluginProcessor (juce::AudioProcessorValueTreeState::ParameterLayout layout);

    void updateDspParameters();

    Parameters params;
    juce::AudioProcessorValueTreeState state;

    juce::dsp::Gain<float> inputGain, outputGain;
    juce::dsp::StateVariableTPTFilter<float> highPass;
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

    Recorder recorder;
    Multitrack multitrack;
    // Transparent peak guard for the voice + playback sum: only acts above -1 dB, so
    // the music isn't coloured (juce::dsp::Limiter compresses from -10 dB).
    float mixGuardGain = 1.0f, mixGuardRelease = 0.0f;
    juce::AudioBuffer<float> dryInput; // the raw mic, kept for the recorder's dry file

    // Which switchable modules ran last block, so one switched back on starts clean.
    std::array<bool, 32> moduleWasOn {};
    bool pitchWasOn = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginProcessor)
};
