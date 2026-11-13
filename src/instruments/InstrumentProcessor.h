#pragma once

#include "InstrumentCore.h"

namespace micfx::inst
{
// The audio processor for one instrument: its parameters (plus "volume"), factory
// presets as programs, the on-screen keyboard's notes, and a copy of the output for
// the window's scope. The same class is the built-in instrument of a MIDI track and
// the VST3/AU plugin.
class InstrumentProcessor final : public juce::AudioProcessor
{
public:
    explicit InstrumentProcessor (const Spec&);
    ~InstrumentProcessor() override;

    const Spec& getSpec() const noexcept { return spec; }
    juce::AudioProcessorValueTreeState& getState() noexcept { return state; }
    juce::MidiKeyboardState& getKeyboardState() noexcept { return keyboard; }

    // Loads a factory preset by name (false when there's none by that name).
    bool loadPreset (const juce::String& name);
    juce::String getPresetName() const;

    // The engine (the Kit's samples are reached through it), and the name of a key on this instance.
    Engine& getEngine() noexcept { return *engine; }
    juce::String getKeyName (int note) const;

    // The last `count` output samples (mono), oldest first. Any thread.
    void readScope (float* destination, int count) const;

    //==============================================================================
    const juce::String getName() const override { return spec.name; }
    void prepareToPlay (double sampleRate, int maximumBlockSize) override;
    void releaseResources() override {}
    void reset() override;
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    double getTailLengthSeconds() const override;
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram (int) override;
    const juce::String getProgramName (int) override;
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout makeLayout (const Spec&);

    const Spec& spec;
    juce::AudioProcessorValueTreeState state;
    std::unique_ptr<Engine> engine;
    juce::MidiKeyboardState keyboard;
    std::atomic<float>* volume = nullptr;
    juce::LinearSmoothedValue<float> gain;
    std::atomic<int> program { 0 };
    juce::AudioBuffer<float> work;

    static constexpr int scopeSize = 4096;
    std::array<float, scopeSize> scope {};
    std::atomic<int> scopeWrite { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (InstrumentProcessor)
};
}
