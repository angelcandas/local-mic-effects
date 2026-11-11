#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>

#include "Parameters.h"
#include "Multitrack.h"
#include "Recorder.h"
#include "daw/DawEngine.h"
#include "daw/Project.h"
#include "dsp/FxChain.h"

// Signal chain, in order (every module except gain/reverb/limiter is skipped when off):
//   input gain -> [effect chain] -> output gain -> safety limiter
// where the effect chain (fx::FxChain, every module type) runs:
//   high-pass -> gate -> autotune / pitch shift / harmonizer -> EQ -> compressor
//   -> de-esser -> exciter -> saturation -> radio -> bitcrusher -> robot -> filter
//   -> auto-wah -> tremolo -> vibrato -> chorus -> flanger -> phaser -> doubler
//   -> echo -> reverb -> stereo width
// The plugin parameters drive the chain's module values once per block.
class PluginProcessor final : public juce::AudioProcessor,
                              private juce::Timer
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
    const fx::PitchEngine& getPitchEngine() const noexcept  { return pitchModule.getEngine(); }

    // Records the processed output (and optionally the dry mic) to WAV. Start/stop it
    // from the message thread; processBlock feeds it.
    Recorder& getRecorder() noexcept                        { return recorder; }

    // Backing track + recorded takes, mixed in after the voice chain (so the music
    // isn't processed or recorded with the voice). Message thread only.
    Multitrack& getMultitrack() noexcept                    { return multitrack; }

    // DAW mode (standalone app): the mic feeds the DAW engine's tracks instead of the
    // Live chain. The session, its engine and its project file. Message thread.
    daw::Edit& getDawEdit() noexcept                         { return dawEdit; }
    daw::DawEngine& getDawEngine() noexcept                  { return dawEngine; }
    daw::Project& getDawProject() noexcept                   { return dawProject; }
    void setDawMode (bool on) noexcept                       { dawMode = on; updateLatency(); }
    bool isDawMode() const noexcept                          { return dawMode.load(); }

    // Peak levels (linear gain) since the UI last read them. Written by the audio
    // thread, drained with exchange (0) by the editor.
    std::atomic<float> inputPeak  { 0.0f };
    std::atomic<float> outputPeak { 0.0f };

    std::atomic<int> currentPreset { 0 };

    // Live mode performance controls (not saved, not parameters):
    // outputMuted silences the output (panic button); dryCompare skips the effect chain
    // so you hear the plain mic, to compare.
    std::atomic<bool> outputMuted { false };
    std::atomic<bool> dryCompare { false };

    // Tells the host (and the recorder's take alignment) how late the output is: the
    // Live chain's delay (autotune, pitch shift), or the DAW engine's in DAW mode.
    // Runs on a timer, since switching autotune on or off changes it. Message thread.
    void updateLatency();

private:
    explicit PluginProcessor (juce::AudioProcessorValueTreeState::ParameterLayout layout);

    void updateDspParameters();

    void timerCallback() override { updateLatency(); }

    Parameters params;
    juce::AudioProcessorValueTreeState state;

    juce::dsp::Gain<float> inputGain, outputGain;
    fx::FxChain chain { fx::FxChain::getAllTypes() };
    fx::modules::Pitch& pitchModule;
    juce::dsp::Limiter<float> limiter;

    Recorder recorder;
    Multitrack multitrack;

    daw::Edit dawEdit;
    daw::DawEngine dawEngine { dawEdit };
    daw::Project dawProject { dawEdit };
    std::atomic<bool> dawMode { false };

    void applyPeakGuard (juce::AudioBuffer<float>&, int numChannels, int numSamples) noexcept;
    // Transparent peak guard for the voice + playback sum: only acts above -1 dB, so
    // the music isn't coloured (juce::dsp::Limiter compresses from -10 dB).
    float mixGuardGain = 1.0f, mixGuardRelease = 0.0f;
    juce::AudioBuffer<float> dryInput; // the raw mic, kept for the recorder's dry file

    // Each chain value and the plugin parameter that drives it.
    struct Binding
    {
        enum class Kind { number, toggle, choice };

        fx::Module::Value* value;
        juce::RangedAudioParameter* parameter;
        Kind kind;
    };

    std::vector<Binding> bindings;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginProcessor)
};
