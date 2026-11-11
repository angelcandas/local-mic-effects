// A tiny VST3 instrument for the tests: a sine per held note, with one parameter
// ("level") that its saved state keeps. The tests scan it, load it on a MIDI track and
// check it plays, so plugin hosting is tested without a third-party plugin.
#include <juce_audio_processors/juce_audio_processors.h>

namespace
{
class TestSynth final : public juce::AudioProcessor
{
public:
    TestSynth()
        : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true))
    {
        addParameter (level = new juce::AudioParameterFloat (juce::ParameterID { "level", 1 }, "Level", 0.0f, 1.0f, 0.5f));
    }

    const juce::String getName() const override { return "MicFX Test Synth"; }
    void prepareToPlay (double rate, int) override { sampleRate = rate; }
    void releaseResources() override {}

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override
    {
        buffer.clear();
        int done = 0;

        for (const auto metadata : midi)
        {
            render (buffer, done, metadata.samplePosition);
            done = metadata.samplePosition;
            const auto message = metadata.getMessage();

            if (message.isNoteOn())
            {
                note = message.getNoteNumber();
                phase = 0.0;
            }
            else if (message.isNoteOff() && message.getNoteNumber() == note)
            {
                note = -1;
            }
            else if (message.isAllNotesOff())
            {
                note = -1;
            }
        }

        render (buffer, done, buffer.getNumSamples());
    }
    using AudioProcessor::processBlock;

    double getTailLengthSeconds() const override { return 0.0; }
    bool acceptsMidi() const override { return true; }
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
        juce::MemoryOutputStream (data, false).writeFloat (level->get());
    }

    void setStateInformation (const void* data, int size) override
    {
        if (size >= 4)
            *level = juce::MemoryInputStream (data, (size_t) size, false).readFloat();
    }

private:
    void render (juce::AudioBuffer<float>& buffer, int from, int to)
    {
        if (note < 0)
            return;

        const auto increment = juce::MidiMessage::getMidiNoteInHertz (note) / sampleRate;

        for (int i = from; i < to; ++i)
        {
            const auto sample = level->get() * (float) std::sin (juce::MathConstants<double>::twoPi * phase);
            phase += increment;
            phase -= std::floor (phase);

            for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
                buffer.setSample (ch, i, sample);
        }
    }

    juce::AudioParameterFloat* level = nullptr;
    double sampleRate = 48000.0, phase = 0.0;
    int note = -1;
};
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new TestSynth(); }
