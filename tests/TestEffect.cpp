// A tiny VST3 effect for the tests: the signal delayed by a fixed 64 samples (it reports
// that as its latency) and scaled by twice its one parameter ("gain", 0.5 = unity), which
// its saved state keeps. The tests scan it, put it on a track as an insert and check the
// sound, the latency compensation and the saved settings, without a third-party plugin.
#include <juce_audio_processors/juce_audio_processors.h>

namespace
{
class TestEffect final : public juce::AudioProcessor
{
public:
    static constexpr int latency = 64;

    TestEffect()
        : AudioProcessor (BusesProperties().withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                           .withOutput ("Output", juce::AudioChannelSet::stereo(), true))
    {
        addParameter (gain = new juce::AudioParameterFloat (juce::ParameterID { "gain", 1 }, "Gain", 0.0f, 1.0f, 0.5f));
        setLatencySamples (latency);
    }

    const juce::String getName() const override { return "MicFX Test Effect"; }

    void prepareToPlay (double, int) override
    {
        line.setSize (2, latency);
        line.clear();
        position = 0;
    }

    void releaseResources() override {}

    bool isBusesLayoutSupported (const BusesLayout& layout) const override
    {
        const auto in = layout.getMainInputChannelSet(), out = layout.getMainOutputChannelSet();
        return in == out && (in == juce::AudioChannelSet::stereo() || in == juce::AudioChannelSet::mono());
    }

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        const auto scale = 2.0f * gain->get();
        const auto channels = juce::jmin (2, buffer.getNumChannels());

        for (int i = 0; i < buffer.getNumSamples(); ++i)
        {
            for (int ch = 0; ch < channels; ++ch)
            {
                const auto in = buffer.getSample (ch, i);
                buffer.setSample (ch, i, scale * line.getSample (ch, position));
                line.setSample (ch, position, in);
            }
            position = (position + 1) % latency;
        }
    }
    using AudioProcessor::processBlock;

    double getTailLengthSeconds() const override { return 0.0; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return new juce::GenericAudioProcessorEditor (*this); }
    bool hasEditor() const override { return true; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& data) override
    {
        juce::MemoryOutputStream (data, false).writeFloat (gain->get());
    }

    void setStateInformation (const void* data, int size) override
    {
        if (size >= 4)
            *gain = juce::MemoryInputStream (data, (size_t) size, false).readFloat();
    }

private:
    juce::AudioParameterFloat* gain = nullptr;
    juce::AudioBuffer<float> line { 2, latency };
    int position = 0;
};
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new TestEffect(); }
