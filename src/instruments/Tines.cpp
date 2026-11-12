// Tines: an electric piano. Each note is a sine phase-modulated by itself (the "bark"
// that grows with velocity) plus a short bell partial high above it, as a struck metal
// tine sounds; Reed mode clips it unevenly for the buzzier, reedy kind. Then drive, a
// stereo tremolo, chorus and a room.
#include "Fx.h"
#include "Registry.h"

#include <map>

namespace micfx::inst
{
namespace
{
    struct TinesBlock
    {
        float tone = 0.5f, bark = 0.5f, bell = 0.4f, decay = 1.0f, release = 0.12f;
        int character = 0;
    };

    class TinesVoice
    {
    public:
        const TinesBlock* block = nullptr;

        void prepare (double rate) { sampleRate = rate; }

        void start (int note, float velocity)
        {
            const auto& b = *block;
            const auto rate = (float) sampleRate;
            const auto f = midiToHz ((float) note);
            const auto reed = b.character == 1;

            carrierStep = f / rate;
            modulatorStep = carrierStep;
            const auto bellRatio = reed ? 7.0f : 14.0f - 0.04f * (float) (note - 60);
            const auto bellHz = f * bellRatio;
            bellStep = bellHz < rate * 0.45f ? bellHz / rate : 0.0f;
            carrierPhase = modulatorPhase = bellPhase = 0.0f;

            // Harder hits bark more; the Tone knob brightens everything.
            const auto v = juce::jlimit (0.0f, 1.0f, velocity);
            const auto keyScale = juce::jlimit (0.25f, 1.0f, 1.0f - ((float) note - 60.0f) / 60.0f);
            index = (0.15f + 1.2f * b.tone + 2.6f * b.bark * v * v) * keyScale;
            indexDecay = decayCoefficient (0.25f + 0.35f * (1.0f - b.bark), sampleRate);
            sustainIndex = 0.12f + 0.25f * b.tone;

            const auto tau = juce::jmax (0.4f, b.decay * 5.0f * std::exp2 (-((float) note - 48.0f) / 20.0f));
            level = (0.12f + 0.88f * std::pow (v, 1.4f)) * 0.22f;
            levelDecay = decayCoefficient (tau, sampleRate);
            damped = decayCoefficient (b.release, sampleRate);

            bellLevel = b.bell * v * (reed ? 0.15f : 0.35f) * keyScale;
            bellDecay = decayCoefficient (reed ? 0.03f : 0.06f + 0.06f * (1.0f - v), sampleRate);

            attack = 0.0f;
            attackStep = 1.0f / (0.0015f * rate);
            asymmetry = reed ? 0.6f : 0.0f;
            released = false;
            active = true;
        }

        void release()
        {
            if (! released)
            {
                released = true;
                levelDecay = juce::jmin (levelDecay, damped);
                bellDecay = juce::jmin (bellDecay, damped);
            }
        }

        void kill() { active = false; }
        bool isActive() const { return active; }

        void render (float* left, float* right, int n)
        {
            constexpr auto twoPi = juce::MathConstants<float>::twoPi;
            for (int i = 0; i < n; ++i)
            {
                const auto modulator = std::sin (twoPi * modulatorPhase);
                auto x = std::sin (twoPi * carrierPhase + (index + sustainIndex) * modulator);
                if (asymmetry > 0.0f)
                    x = std::tanh (1.8f * (x + asymmetry * x * x)) * 0.8f;
                auto out = x * level;
                if (bellStep > 0.0f)
                    out += std::sin (twoPi * bellPhase) * bellLevel;
                attack = juce::jmin (1.0f, attack + attackStep);
                out *= attack;

                left[i] += out;
                right[i] += out;

                carrierPhase += carrierStep;     carrierPhase -= std::floor (carrierPhase);
                modulatorPhase += modulatorStep; modulatorPhase -= std::floor (modulatorPhase);
                bellPhase += bellStep;           bellPhase -= std::floor (bellPhase);
                index *= indexDecay;
                level *= levelDecay;
                bellLevel *= bellDecay;
            }
            if (level < 1.0e-5f && bellLevel < 1.0e-5f)
                active = false;
        }

    private:
        double sampleRate = 48000.0;
        float carrierStep = 0, modulatorStep = 0, bellStep = 0, carrierPhase = 0, modulatorPhase = 0, bellPhase = 0;
        float index = 0, indexDecay = 1, sustainIndex = 0, level = 0, levelDecay = 1, damped = 1;
        float bellLevel = 0, bellDecay = 1, attack = 0, attackStep = 1, asymmetry = 0;
        bool released = false, active = false;
    };

    class TinesEngine final : public VoiceEngine<TinesVoice>
    {
    public:
        explicit TinesEngine (Params p) : VoiceEngine (32)
        {
            for (const auto* id : { "character", "tone", "bark", "bell", "decay", "release", "drive", "tremRate", "tremDepth",
                                    "chorus", "room" })
                values[id] = p (id);
            for (auto& slot : voices)
                slot.voice.block = &block;
        }

        double getTailSeconds() const override { return 3.0; }

    private:
        float v (const char* id) const { return values.at (id)->load (std::memory_order_relaxed); }

        void prepareEngine (double rate, int) override
        {
            chorus.prepare (rate);
            room.prepare (rate);
            for (auto& f : toneFilter) f.setLowpass (7000.0f, 0.6f, rate);
        }

        void resetEngine() override
        {
            chorus.reset();
            room.reset();
            for (auto& f : toneFilter) f.reset();
        }

        void beginBlock (int) override
        {
            block.character = (int) std::round (v ("character"));
            block.tone = v ("tone");
            block.bark = v ("bark");
            block.bell = v ("bell");
            block.decay = v ("decay");
            block.release = v ("release");
        }

        void endBlock (juce::AudioBuffer<float>& buffer) override
        {
            auto* l = buffer.getWritePointer (0);
            auto* r = buffer.getWritePointer (1);
            const auto n = buffer.getNumSamples();

            // Drive: an amp pushed into soft clipping.
            const auto drive = v ("drive");
            if (drive > 0.001f)
            {
                const auto g = 1.0f + 6.0f * drive;
                const auto makeup = 1.0f / std::tanh (0.5f * g) * 0.5f;
                for (int i = 0; i < n; ++i)
                {
                    l[i] = toneFilter[0].process (std::tanh (l[i] * g) * makeup);
                    r[i] = toneFilter[1].process (std::tanh (r[i] * g) * makeup);
                }
            }

            // The stereo tremolo: sound swings between the speakers.
            const auto depth = v ("tremDepth");
            const auto increment = v ("tremRate") / (float) sampleRate;
            for (int i = 0; i < n; ++i)
            {
                const auto swing = depth * std::sin (juce::MathConstants<float>::twoPi * tremPhase);
                l[i] *= 1.0f - 0.5f * depth + 0.5f * swing;
                r[i] *= 1.0f - 0.5f * depth - 0.5f * swing;
                tremPhase += increment;
                tremPhase -= std::floor (tremPhase);
            }

            chorus.process (l, r, n, v ("chorus"), 0.5f, 3.0f);
            room.process (l, r, n, v ("room"), 0.7f, 0.5f);
        }

        std::map<juce::String, std::atomic<float>*> values;
        TinesBlock block;
        Biquad toneFilter[2];
        float tremPhase = 0.0f;
        Chorus chorus;
        Room room;
    };
}

const Spec& getTinesSpec()
{
    static const Spec spec = []
    {
        Spec s;
        s.key = "tines";
        s.name = "Tines";
        s.tagline = "Electric piano";
        s.category = "Piano";
        s.accent = juce::Colour (0xffef7a5a);
        s.lowestKey = 28;
        s.highestKey = 103;

        s.params = {
            choice ("character", "Type", { "Tine", "Reed" }, 0),
            knob ("tone", "Tone", 0.0f, 1.0f, 0.4f),
            knob ("bark", "Bark", 0.0f, 1.0f, 0.5f),
            knob ("bell", "Bell", 0.0f, 1.0f, 0.45f),
            knob ("decay", "Sustain", 0.3f, 2.5f, 1.0f, Unit::multiplier, 1.0f),
            knob ("release", "Damping", 0.02f, 0.6f, 0.1f, Unit::seconds, 0.12f),
            knob ("drive", "Drive", 0.0f, 1.0f, 0.1f),
            knob ("tremRate", "Rate", 0.5f, 10.0f, 4.5f, Unit::hertz, 3.5f),
            knob ("tremDepth", "Depth", 0.0f, 1.0f, 0.0f),
            knob ("chorus", "Chorus", 0.0f, 1.0f, 0.0f),
            knob ("room", "Room", 0.0f, 1.0f, 0.2f),
        };

        s.rows = {
            { { "Pickup", { "character", "tone", "bark", "bell" }, {} },
              { "Keys", { "decay", "release" }, {} } },
            { { "Amp", { "drive" }, {} },
              { "Tremolo", { "tremRate", "tremDepth" }, {} },
              { "Space", { "chorus", "room" }, {} } },
        };

        s.presets = {
            { "Classic", { { "tremDepth", 0.35f } } },
            { "Ballad Bell", { { "bell", 0.8f }, { "bark", 0.25f }, { "tone", 0.3f }, { "chorus", 0.35f }, { "room", 0.35f } } },
            { "Funky Bark", { { "bark", 0.95f }, { "tone", 0.6f }, { "drive", 0.4f }, { "bell", 0.3f }, { "room", 0.1f } } },
            { "Stage Tremolo", { { "tremDepth", 0.8f }, { "tremRate", 5.5f }, { "bark", 0.6f } } },
            { "Reed", { { "character", 1.0f }, { "tone", 0.5f }, { "bark", 0.6f }, { "bell", 0.2f }, { "decay", 0.7f }, { "tremDepth", 0.25f } } },
            { "Lo-fi Keys", { { "tone", 0.2f }, { "bell", 0.2f }, { "drive", 0.55f }, { "chorus", 0.6f }, { "room", 0.3f } } },
        };

        s.makeEngine = [] (Params p) { return std::make_unique<TinesEngine> (p); };
        return s;
    }();
    return spec;
}
}
