// Mallets: struck bars, tines and bells by modal synthesis. Each material is a few
// partials at its own (often inharmonic) ratios with their own decays; a harder mallet
// wakes the upper ones and adds a click. Vibes get their motor's tremolo.
#include "Fx.h"
#include "Registry.h"

#include <map>

namespace micfx::inst
{
namespace
{
    struct Material
    {
        float ratios[6];
        float levels[6];
        float decays[6]; // seconds at middle C
        float click;
        float gain;
    };

    constexpr Material materials[] = {
        // Marimba: tuned bars over resonator tubes (warm, the fundamental rings longest).
        { { 1.0f, 3.99f, 10.65f, 0, 0, 0 }, { 1.0f, 0.35f, 0.12f, 0, 0, 0 }, { 1.4f, 0.35f, 0.08f, 0, 0, 0 }, 0.5f, 1.0f },
        // Vibraphone: aluminium bars, long and pure.
        { { 1.0f, 4.0f, 10.0f, 0, 0, 0 }, { 1.0f, 0.3f, 0.08f, 0, 0, 0 }, { 5.0f, 1.0f, 0.25f, 0, 0, 0 }, 0.3f, 0.9f },
        // Glockenspiel: small steel bars, bright and inharmonic.
        { { 1.0f, 2.76f, 5.40f, 8.93f, 0, 0 }, { 1.0f, 0.5f, 0.3f, 0.15f, 0, 0 }, { 3.0f, 1.2f, 0.6f, 0.3f, 0, 0 }, 0.35f, 0.8f },
        // Kalimba: plucked metal tines on a box.
        { { 1.0f, 5.9f, 13.3f, 0, 0, 0 }, { 1.0f, 0.25f, 0.08f, 0, 0, 0 }, { 1.6f, 0.15f, 0.05f, 0, 0, 0 }, 0.6f, 1.0f },
        // Tubular bells: the strike note over a bell's spread of partials.
        { { 1.0f, 2.0f, 2.4f, 3.0f, 4.2f, 5.4f }, { 0.7f, 1.0f, 0.5f, 0.6f, 0.35f, 0.2f }, { 4.5f, 3.5f, 2.2f, 1.8f, 1.2f, 0.8f }, 0.25f, 0.45f },
    };

    struct MalletsBlock
    {
        int material = 0;
        float hardness = 0.5f, decay = 1.0f, damper = 0.0f, width = 0.5f;
    };

    class MalletsVoice
    {
    public:
        const MalletsBlock* block = nullptr;

        void prepare (double rate) { sampleRate = rate; }

        void start (int note, float velocity)
        {
            const auto& b = *block;
            const auto& m = materials[juce::jlimit (0, 4, b.material)];
            const auto rate = (float) sampleRate;
            const auto f = midiToHz ((float) note);
            const auto hard = juce::jlimit (0.0f, 1.0f, b.hardness * (0.4f + 0.6f * velocity));
            const auto pitchDecay = std::exp2 (-((float) note - 60.0f) / 24.0f);

            count = 0;
            for (int k = 0; k < 6 && m.ratios[k] > 0.0f; ++k)
            {
                const auto hz = f * m.ratios[k];
                if (hz > rate * 0.45f)
                    break;
                auto& mode = modes[count++];
                const auto w = juce::MathConstants<float>::twoPi * hz / rate;
                mode.cw = std::cos (w);
                mode.sw = std::sin (w);
                mode.c = 1.0f;
                mode.s = 0.0f;
                // Harder mallets: more of the upper partials.
                mode.level = m.levels[k] * (k == 0 ? 1.0f : std::pow (hard, 0.6f + 0.4f * (float) k) * 1.6f) * 0.18f
                           * (0.15f + 0.85f * velocity) * m.gain;
                mode.decay = decayCoefficient (m.decays[k] * b.decay * pitchDecay / 4.0f, sampleRate); // about -35 dB by then
                mode.damped = decayCoefficient (0.05f + 0.6f * (1.0f - b.damper), sampleRate);
            }

            click = m.click * hard * velocity * 0.25f;
            clickDecay = decayCoefficient (0.002f + 0.004f * (1.0f - hard), sampleRate);
            clickFilter.set (juce::jmin (12000.0f, f * 6.0f), 0.2f, sampleRate);
            clickFilter.reset();
            noise.seed ((uint32_t) note * 40503u + 7u);

            const auto pan = 0.5f + 0.5f * juce::jlimit (-1.0f, 1.0f, b.width * ((float) note - 66.0f) / 30.0f);
            gainL = std::cos (pan * juce::MathConstants<float>::halfPi) * 1.3f;
            gainR = std::sin (pan * juce::MathConstants<float>::halfPi) * 1.3f;
            attack = 0.0f;
            attackStep = 1.0f / (0.0008f * rate);
            active = count > 0;
        }

        void release()
        {
            if (block->damper <= 0.001f)
                return; // undamped bars ring on
            for (int k = 0; k < count; ++k)
                modes[k].decay = juce::jmin (modes[k].decay, modes[k].damped);
        }

        void kill() { active = false; }
        bool isActive() const { return active; }

        void render (float* left, float* right, int n)
        {
            for (int i = 0; i < n; ++i)
            {
                auto sum = 0.0f;
                for (int k = 0; k < count; ++k)
                {
                    auto& m = modes[k];
                    const auto c = m.c * m.cw - m.s * m.sw;
                    m.s = m.c * m.sw + m.s * m.cw;
                    m.c = c;
                    sum += m.s * m.level;
                    m.level *= m.decay;
                }
                if (click > 1.0e-6f)
                {
                    sum += clickFilter.bandpass (noise.next()) * click;
                    click *= clickDecay;
                }
                attack = juce::jmin (1.0f, attack + attackStep);
                sum *= attack;
                left[i] += sum * gainL;
                right[i] += sum * gainR;
            }

            auto loudest = click;
            for (int k = 0; k < count; ++k)
            {
                auto& m = modes[k];
                const auto fix = 1.5f - 0.5f * (m.c * m.c + m.s * m.s);
                m.c *= fix;
                m.s *= fix;
                loudest = juce::jmax (loudest, m.level);
            }
            if (loudest < 1.0e-5f)
                active = false;
        }

    private:
        struct Mode
        {
            float c = 1, s = 0, cw = 1, sw = 0, level = 0, decay = 1, damped = 1;
        };

        double sampleRate = 48000.0;
        Mode modes[6];
        int count = 0;
        float click = 0, clickDecay = 1, gainL = 1, gainR = 1, attack = 0, attackStep = 1;
        bool active = false;
        Svf clickFilter;
        Noise noise;
    };

    class MalletsEngine final : public VoiceEngine<MalletsVoice>
    {
    public:
        explicit MalletsEngine (Params p) : VoiceEngine (32)
        {
            for (const auto* id : { "material", "hardness", "decay", "damper", "tremRate", "tremDepth", "width", "echo", "room" })
                values[id] = p (id);
            for (auto& slot : voices)
                slot.voice.block = &block;
        }

        double getTailSeconds() const override { return 6.0; }

    private:
        float v (const char* id) const { return values.at (id)->load (std::memory_order_relaxed); }

        void prepareEngine (double rate, int) override { echo.prepare (rate); room.prepare (rate); }
        void resetEngine() override { echo.reset(); room.reset(); }

        void beginBlock (int) override
        {
            block.material = (int) std::round (v ("material"));
            block.hardness = v ("hardness");
            block.decay = v ("decay");
            block.damper = v ("damper");
            block.width = v ("width");
        }

        void endBlock (juce::AudioBuffer<float>& buffer) override
        {
            auto* l = buffer.getWritePointer (0);
            auto* r = buffer.getWritePointer (1);
            const auto n = buffer.getNumSamples();

            // The vibraphone's motor: rotating discs in the tubes make the level pulse.
            const auto depth = v ("tremDepth");
            const auto increment = v ("tremRate") / (float) sampleRate;
            if (depth > 0.001f)
                for (int i = 0; i < n; ++i)
                {
                    const auto g = 1.0f - depth * 0.5f * (1.0f + std::sin (juce::MathConstants<float>::twoPi * tremPhase));
                    l[i] *= g;
                    r[i] *= g;
                    tremPhase += increment;
                    tremPhase -= std::floor (tremPhase);
                }

            echo.process (l, r, n, v ("echo") * 0.6f, 0.36f, 0.4f);
            room.process (l, r, n, v ("room"), 0.78f, 0.4f);
        }

        std::map<juce::String, std::atomic<float>*> values;
        MalletsBlock block;
        float tremPhase = 0.0f;
        Echo echo;
        Room room;
    };
}

const Spec& getMalletsSpec()
{
    static const Spec spec = []
    {
        Spec s;
        s.key = "mallets";
        s.name = "Mallets";
        s.tagline = "Bars, tines and bells";
        s.category = "Percussion";
        s.accent = juce::Colour (0xff9d8cf0);
        s.lowestKey = 48;
        s.highestKey = 96;

        s.params = {
            choice ("material", "Instrument", { "Marimba", "Vibraphone", "Glockenspiel", "Kalimba", "Tubular bells" }, 0),
            knob ("hardness", "Mallet", 0.0f, 1.0f, 0.5f),
            knob ("decay", "Sustain", 0.2f, 3.0f, 1.0f, Unit::multiplier, 1.0f),
            knob ("damper", "Damper", 0.0f, 1.0f, 0.0f),
            knob ("tremRate", "Rate", 1.0f, 10.0f, 5.0f, Unit::hertz),
            knob ("tremDepth", "Depth", 0.0f, 1.0f, 0.0f),
            knob ("width", "Width", 0.0f, 1.0f, 0.6f),
            knob ("echo", "Echo", 0.0f, 1.0f, 0.0f),
            knob ("room", "Room", 0.0f, 1.0f, 0.25f),
        };

        s.rows = {
            { { "Instrument", { "material", "hardness", "decay", "damper" }, {} },
              { "Motor", { "tremRate", "tremDepth" }, {} } },
            { { "Stereo", { "width" }, {} },
              { "Space", { "echo", "room" }, {} } },
        };

        s.presets = {
            { "Marimba", {} },
            { "Vibraphone", { { "material", 1.0f }, { "hardness", 0.35f }, { "damper", 0.6f }, { "tremDepth", 0.45f }, { "room", 0.3f } } },
            { "Glockenspiel", { { "material", 2.0f }, { "hardness", 0.75f }, { "room", 0.3f } } },
            { "Kalimba", { { "material", 3.0f }, { "hardness", 0.6f }, { "width", 0.4f }, { "echo", 0.2f } } },
            { "Tubular Bells", { { "material", 4.0f }, { "hardness", 0.7f }, { "decay", 1.2f }, { "damper", 0.3f }, { "room", 0.4f } } },
            { "Music Box", { { "material", 2.0f }, { "hardness", 0.3f }, { "decay", 0.7f }, { "echo", 0.25f }, { "room", 0.35f } } },
        };

        s.makeEngine = [] (Params p) { return std::make_unique<MalletsEngine> (p); };
        return s;
    }();
    return spec;
}
}
