#include "Instruments.h"

namespace daw
{
namespace
{
    //==============================================================================
    // The built-in synths: one simple subtractive voice (two oscillators, a low-pass
    // filter and an envelope) with a sound per instrument.
    struct Sound
    {
        const char* key;
        const char* name;
        float saw, square, sine, sub;     // oscillator mix
        float detuneCents;                // second saw, for width
        float cutoff, cutoffEnvelope;     // Hz, and how far the envelope opens it (octaves)
        float keyTracking;                // 0..1: the cutoff follows the note
        float attack, decay, sustain, release; // seconds / level
        float filterDecay;                // seconds
        float level;
    };

    constexpr Sound sounds[] = {
        // key     name           saw   sq    sine  sub   det   cutoff  env   kt    A      D     S     R     fD    level
        { "keys",  "Keys",        0.25f, 0.0f, 0.8f, 0.0f, 0.0f, 1800.0f, 2.0f, 0.6f, 0.002f, 1.2f, 0.25f, 0.35f, 0.4f, 0.5f },
        { "piano", "Soft Piano",  0.35f, 0.0f, 0.6f, 0.0f, 3.0f, 1200.0f, 3.0f, 0.8f, 0.001f, 2.5f, 0.0f, 0.4f, 0.8f, 0.55f },
        { "pad",   "Warm Pad",    0.6f, 0.0f, 0.0f, 0.2f, 9.0f,  900.0f, 1.0f, 0.3f, 0.6f,  1.0f, 0.8f, 1.2f, 1.5f, 0.35f },
        { "strings","Strings",    0.7f, 0.0f, 0.0f, 0.0f, 12.0f, 2500.0f, 0.5f, 0.4f, 0.25f, 0.5f, 0.85f, 0.6f, 1.0f, 0.3f },
        { "bass",  "Synth Bass",  0.7f, 0.0f, 0.0f, 0.7f, 0.0f,  350.0f, 2.5f, 0.3f, 0.003f, 0.4f, 0.6f, 0.12f, 0.25f, 0.55f },
        { "lead",  "Lead",        0.3f, 0.6f, 0.0f, 0.0f, 6.0f, 3000.0f, 1.5f, 0.5f, 0.005f, 0.3f, 0.7f, 0.2f, 0.3f, 0.35f },
        { "organ", "Organ",       0.0f, 0.15f, 0.8f, 0.4f, 0.0f, 6000.0f, 0.0f, 0.0f, 0.004f, 0.05f, 1.0f, 0.06f, 0.1f, 0.4f },
    };

    const Sound* findSound (const juce::String& key)
    {
        for (const auto& s : sounds)
            if (key == s.key)
                return &s;
        return nullptr;
    }

    // Band-limited step correction (PolyBLEP) for the saw and square.
    float polyBlep (float t, float dt) noexcept
    {
        if (t < dt)        { t /= dt; return t + t - t * t - 1.0f; }
        if (t > 1.0f - dt) { t = (t - 1.0f) / dt; return t * t + t + t + 1.0f; }
        return 0.0f;
    }

    struct SynthSound final : juce::SynthesiserSound
    {
        bool appliesToNote (int) override    { return true; }
        bool appliesToChannel (int) override { return true; }
    };

    class SynthVoice final : public juce::SynthesiserVoice
    {
    public:
        explicit SynthVoice (const Sound& s) : sound (s) {}

        bool canPlaySound (juce::SynthesiserSound* s) override { return dynamic_cast<SynthSound*> (s) != nullptr; }

        void startNote (int note, float velocity, juce::SynthesiserSound*, int pitchWheel) override
        {
            noteNumber = note;
            gain = sound.level * (0.25f + 0.75f * velocity);
            setPitchWheel (pitchWheel);
            phase1 = phase2 = phaseSub = 0.0f;
            filterState1 = filterState2 = 0.0f;

            juce::ADSR::Parameters p { sound.attack, sound.decay, sound.sustain, sound.release };
            envelope.setSampleRate (getSampleRate());
            envelope.setParameters (p);
            envelope.noteOn();
            filterEnvelope = 1.0f;
            filterEnvelopeDecay = (float) std::exp (-1.0 / (sound.filterDecay * getSampleRate()));
        }

        void stopNote (float, bool allowTailOff) override
        {
            if (allowTailOff)
            {
                envelope.noteOff();
            }
            else
            {
                envelope.reset();
                clearCurrentNote();
            }
        }

        void pitchWheelMoved (int value) override { setPitchWheel (value); }
        void controllerMoved (int, int) override {}

        using SynthesiserVoice::renderNextBlock;

        void renderNextBlock (juce::AudioBuffer<float>& output, int start, int numSamples) override
        {
            if (! isVoiceActive())
                return;

            const auto rate = (float) getSampleRate();
            const auto dt1 = frequency / rate;
            const auto dt2 = frequency * detuneRatio / rate;
            const auto baseCutoff = sound.cutoff * std::pow (frequency / 261.6f, sound.keyTracking);

            for (int i = 0; i < numSamples; ++i)
            {
                // Oscillators.
                auto saw1 = 2.0f * phase1 - 1.0f - polyBlep (phase1, dt1);
                auto saw2 = 2.0f * phase2 - 1.0f - polyBlep (phase2, dt2);
                auto square = (phase1 < 0.5f ? 1.0f : -1.0f) + polyBlep (phase1, dt1)
                            - polyBlep (std::fmod (phase1 + 0.5f, 1.0f), dt1);
                auto sine = std::sin (juce::MathConstants<float>::twoPi * phase1);
                auto sub = std::sin (juce::MathConstants<float>::twoPi * phaseSub);

                auto value = sound.saw * (sound.detuneCents > 0.0f ? 0.5f * (saw1 + saw2) : saw1)
                           + sound.square * square + sound.sine * sine + sound.sub * sub;

                phase1 += dt1;  phase1 -= std::floor (phase1);
                phase2 += dt2;  phase2 -= std::floor (phase2);
                phaseSub += dt1 * 0.5f; phaseSub -= std::floor (phaseSub);

                // Two one-pole low-passes, opened by the filter envelope.
                const auto cutoff = juce::jlimit (30.0f, rate * 0.45f,
                                                  baseCutoff * std::exp2 (sound.cutoffEnvelope * filterEnvelope));
                const auto coefficient = 1.0f - std::exp (-juce::MathConstants<float>::twoPi * cutoff / rate);
                filterState1 += coefficient * (value - filterState1);
                filterState2 += coefficient * (filterState1 - filterState2);
                filterEnvelope *= filterEnvelopeDecay;

                const auto sample = filterState2 * gain * envelope.getNextSample();

                for (int ch = 0; ch < output.getNumChannels(); ++ch)
                    output.addSample (ch, start + i, sample);

                if (! envelope.isActive())
                {
                    clearCurrentNote();
                    break;
                }
            }
        }

    private:
        void setPitchWheel (int value)
        {
            const auto bend = ((float) value - 8192.0f) / 8192.0f * 2.0f; // +-2 semitones
            frequency = (float) juce::MidiMessage::getMidiNoteInHertz (noteNumber) * std::exp2 (bend / 12.0f);
            detuneRatio = std::exp2 (sound.detuneCents / 1200.0f);
        }

        const Sound& sound;
        int noteNumber = 60;
        float frequency = 440.0f, detuneRatio = 1.0f, gain = 0.0f;
        float phase1 = 0.0f, phase2 = 0.0f, phaseSub = 0.0f;
        float filterState1 = 0.0f, filterState2 = 0.0f, filterEnvelope = 0.0f, filterEnvelopeDecay = 0.0f;
        juce::ADSR envelope;
    };

    class BuiltinSynth final : public juce::AudioProcessor
    {
    public:
        explicit BuiltinSynth (const Sound& s)
            : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
              sound (s)
        {
            for (int i = 0; i < 16; ++i)
                synth.addVoice (new SynthVoice (sound));
            synth.addSound (new SynthSound());
        }

        const juce::String getName() const override { return sound.name; }
        void prepareToPlay (double rate, int) override { synth.setCurrentPlaybackSampleRate (rate); }
        void releaseResources() override {}

        void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override
        {
            buffer.clear();
            synth.renderNextBlock (buffer, midi, 0, buffer.getNumSamples());
        }
        using AudioProcessor::processBlock;

        double getTailLengthSeconds() const override { return sound.release; }
        bool acceptsMidi() const override { return true; }
        bool producesMidi() const override { return false; }
        juce::AudioProcessorEditor* createEditor() override { return nullptr; }
        bool hasEditor() const override { return false; }
        int getNumPrograms() override { return 1; }
        int getCurrentProgram() override { return 0; }
        void setCurrentProgram (int) override {}
        const juce::String getProgramName (int) override { return sound.name; }
        void changeProgramName (int, const juce::String&) override {}
        void getStateInformation (juce::MemoryBlock&) override {}
        void setStateInformation (const void*, int) override {}

    private:
        const Sound& sound;
        juce::Synthesiser synth;
    };

    instruments::PluginLoader& getLoader()
    {
        static instruments::PluginLoader loader;
        return loader;
    }

    bool onMessageThread()
    {
        auto* mm = juce::MessageManager::getInstanceWithoutCreating();
        return mm == nullptr || mm->isThisTheMessageThread();
    }
}

namespace instruments
{
    juce::Array<Edit::InstrumentInfo> getBuiltins()
    {
        juce::Array<Edit::InstrumentInfo> list;
        for (const auto& s : sounds)
            list.add ({ juce::String ("builtin:") + s.key, s.name, builtinFormat, "MicFX", {}, {} });
        return list;
    }

    std::optional<Edit::InstrumentInfo> findBuiltin (const juce::String& id)
    {
        for (const auto& info : getBuiltins())
            if (info.id == id)
                return info;
        return std::nullopt;
    }

    Edit::InstrumentInfo getDefault() { return getBuiltins().getFirst(); }

    InstrumentPtr create (const juce::ValueTree& instrument, double sampleRate, int blockSize, bool realtime,
                          juce::String& error)
    {
        const auto id = instrument[ids::plugin].toString();
        std::unique_ptr<juce::AudioProcessor> processor;

        if (id.startsWith ("builtin:"))
        {
            if (const auto* s = findSound (id.fromFirstOccurrenceOf (":", false, false)))
                processor = std::make_unique<BuiltinSynth> (*s);
            else
                error = "Unknown built-in instrument " + id + ".";
        }
        else if (id.isNotEmpty())
        {
            const auto load = [&]
            {
                if (auto& loader = getLoader())
                    processor = loader (instrument, sampleRate, blockSize, error);
                else
                    error = "Instrument plugins can't be loaded here.";
            };

            if (onMessageThread())
                load();
            else
                juce::MessageManager::getInstance()->callFunctionOnMessageThread ([] (void* f) -> void*
                                                                                 { (*static_cast<decltype (load)*> (f))(); return nullptr; },
                                                                                 (void*) &load);

            if (processor == nullptr && error.isEmpty())
                error = "Couldn't load " + instrument[ids::name].toString() + ".";
        }

        if (processor == nullptr)
            return {};

        if (const auto state = instrument[ids::state].toString(); state.isNotEmpty())
        {
            juce::MemoryBlock block;
            if (block.fromBase64Encoding (state) && block.getSize() > 0)
                processor->setStateInformation (block.getData(), (int) block.getSize());
        }

        processor->setNonRealtime (! realtime);
        processor->setRateAndBufferSizeDetails (sampleRate, blockSize);
        processor->prepareToPlay (sampleRate, blockSize);

        // Plugins are released and deleted on the message thread, where they were made.
        const auto isPlugin = dynamic_cast<BuiltinSynth*> (processor.get()) == nullptr;

        return InstrumentPtr (processor.release(), [isPlugin] (juce::AudioProcessor* p)
        {
            const auto destroy = [p] { p->releaseResources(); delete p; };

            if (! isPlugin || onMessageThread())
                destroy();
            else
                juce::MessageManager::getInstance()->callFunctionOnMessageThread ([] (void* f) -> void*
                                                                                 { (*static_cast<decltype (destroy)*> (f))(); return nullptr; },
                                                                                 (void*) &destroy);
        });
    }

    juce::String getState (juce::AudioProcessor& processor)
    {
        juce::MemoryBlock block;
        processor.getStateInformation (block);
        return block.getSize() > 0 ? block.toBase64Encoding() : juce::String();
    }

    void setPluginLoader (PluginLoader loader) { getLoader() = std::move (loader); }
}
}
