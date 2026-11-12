// Grand: a modelled acoustic piano. Each note is a set of slightly inharmonic partials
// (a stiff string's), the hammer shaping their levels by how hard it hits. The lowest
// partials come from two detuned strings, so they beat and decay in two stages like a
// real piano's. Hammer and damper noise, a soundboard EQ and a room follow.
#include "Fx.h"
#include "Registry.h"

#include <map>

namespace micfx::inst
{
namespace
{
    struct GrandBlock
    {
        float hammer = 0.5f, tone = 0.0f, dynamics = 0.5f, mechanics = 0.3f, decay = 1.0f, damper = 0.2f;
        float detune = 0.2f, stretch = 0.5f, width = 0.6f;
    };

    class GrandVoice
    {
    public:
        const GrandBlock* block = nullptr;

        void prepare (double rate) { sampleRate = rate; }

        void start (int note, float velocity)
        {
            const auto& b = *block;
            const auto rate = (float) sampleRate;
            noteNumber = note;
            released = false;
            noise.seed ((uint32_t) note * 2654435761u + (uint32_t) (velocity * 1000.0f));

            // Tuning: stretched octaves, a stiff string's inharmonicity.
            const auto fromMiddle = ((float) note - 64.5f) / 43.5f;
            const auto stretchCents = b.stretch * 28.0f * fromMiddle * fromMiddle * fromMiddle;
            const auto f0 = midiToHz ((float) note + stretchCents * 0.01f);
            const auto inharmonicity = 1.2e-4f * std::exp2 (((float) note - 60.0f) / 12.0f * 0.9f)
                                     + 2.0e-4f * juce::jlimit (0.0f, 1.0f, (45.0f - (float) note) / 24.0f);

            // The hammer: how hard (velocity and the Hardness knob) sets how bright.
            const auto hardness = juce::jlimit (0.02f, 1.0f, b.hammer * (0.3f + 0.7f * velocity));
            const auto brightness = (2.5f + 22.0f * std::pow (hardness, 1.5f)) * std::exp2 (b.tone * 1.2f);
            const auto cutoffHz = juce::jmin (12000.0f, f0 * brightness + 300.0f * hardness);
            const auto strikePoint = 1.0f / 8.3f;

            // How long the strings ring.
            const auto fundamentalTau = juce::jmax (0.35f, b.decay * 14.0f * std::exp2 (-((float) note - 21.0f) / 17.0f));
            const auto detuneCents = 0.15f + b.detune * 6.0f;
            const auto spread = 1.0f + (0.6f + 0.4f * std::abs (noise.next())) * detuneCents / 1731.0f; // cents -> ratio

            count = 0;
            auto energy = 0.0f;
            const auto maxPartials = note < 48 ? 40 : 28;
            for (int k = 1; k <= maxPartials; ++k)
            {
                const auto fk = (float) k * f0 * std::sqrt (1.0f + inharmonicity * (float) (k * k));
                if (fk > rate * 0.45f || fk > 16000.0f)
                    break;

                const auto comb = 0.35f + 0.65f * std::abs (std::sin (juce::MathConstants<float>::pi * (float) k * strikePoint));
                const auto ratio = fk / cutoffHz;
                auto level = comb / (float) k / std::sqrt (1.0f + ratio * ratio * ratio * ratio);
                if (k == 1 && note < 40)
                    level *= 0.5f + 0.5f * (float) (note - 21) / 19.0f; // bass strings: weak fundamentals
                energy += level * level;

                const auto tau = fundamentalTau / (1.0f + 0.5f * std::pow (fk / 1000.0f, 1.4f) + 0.02f * (float) (k - 1));
                const auto dampTau = b.damper / (1.0f + 0.06f * (float) k);
                const auto strings = k <= 8 ? 2 : 1;
                for (int s = 0; s < strings && count < maxOscillators; ++s)
                {
                    auto& o = oscillators[count++];
                    const auto f = strings == 2 ? fk * (s == 0 ? spread : 1.0f / spread) : fk;
                    const auto w = juce::MathConstants<float>::twoPi * f / rate;
                    o.cw = std::cos (w);
                    o.sw = std::sin (w);
                    o.c = 1.0f;
                    o.s = 0.0f;
                    const auto share = level / (float) strings;
                    o.prompt = share * 0.65f;
                    o.after = share * 0.35f;
                    o.promptDecay = decayCoefficient (tau / (s == 0 ? 5.0f : 7.0f), sampleRate);
                    o.afterDecay = decayCoefficient (tau * (s == 0 ? 1.0f : 0.85f), sampleRate);
                    o.damped = decayCoefficient (dampTau, sampleRate);
                }
            }

            // Loudness: the Dynamics knob sets how much quieter soft notes are.
            const auto range = 18.0f + 24.0f * b.dynamics;
            const auto loudness = juce::Decibels::decibelsToGain (-range * std::pow (1.0f - velocity, 1.3f));
            const auto norm = loudness * 0.13f / std::sqrt (juce::jmax (1.0e-6f, energy));
            for (int i = 0; i < count; ++i)
            {
                oscillators[i].prompt *= norm;
                oscillators[i].after *= norm;
            }

            // Hammer knock and thump.
            knock = b.mechanics * loudness * (0.05f + 0.12f * hardness);
            knockDecay = decayCoefficient (0.004f + 0.004f * (1.0f - hardness), sampleRate);
            thump = b.mechanics * loudness * 0.12f;
            thumpDecay = decayCoefficient (0.02f, sampleRate);
            knockFilter.set (400.0f + 3000.0f * hardness, 0.2f, sampleRate);
            thumpFilter.set (140.0f, 0.1f, sampleRate);
            knockFilter.reset();
            thumpFilter.reset();

            const auto pan = 0.5f + 0.5f * juce::jlimit (-1.0f, 1.0f, b.width * 0.85f * ((float) note - 64.0f) / 44.0f);
            gainL = std::cos (pan * juce::MathConstants<float>::halfPi) * 1.41f;
            gainR = std::sin (pan * juce::MathConstants<float>::halfPi) * 1.41f;
            fade = 1.0f;
            fadeDecay = 1.0f;
            active = count > 0;
        }

        void release()
        {
            if (released || ! active)
                return;
            released = true;
            if (noteNumber >= 89)
                return; // the top keys have no dampers
            for (int i = 0; i < count; ++i)
            {
                auto& o = oscillators[i];
                o.promptDecay = juce::jmin (o.promptDecay, o.damped);
                o.afterDecay = juce::jmin (o.afterDecay, o.damped);
            }
            // The damper's felt landing.
            thump += block->mechanics * 0.02f;
        }

        // A struck key's old note fades fast instead of cutting off.
        void kill() { active = false; count = 0; }
        bool isActive() const { return active; }

        void render (float* left, float* right, int n)
        {
            for (int i = 0; i < n; ++i)
            {
                auto sum = 0.0f;
                for (int p = 0; p < count; ++p)
                {
                    auto& o = oscillators[p];
                    const auto c = o.c * o.cw - o.s * o.sw;
                    o.s = o.c * o.sw + o.s * o.cw;
                    o.c = c;
                    sum += o.s * (o.prompt + o.after);
                    o.prompt *= o.promptDecay;
                    o.after *= o.afterDecay;
                }

                if (knock > 1.0e-6f || thump > 1.0e-6f)
                {
                    const auto white = noise.next();
                    sum += knockFilter.bandpass (white) * knock + thumpFilter.lowpass (white) * thump * 3.0f;
                    knock *= knockDecay;
                    thump *= thumpDecay;
                }

                left[i] += sum * gainL;
                right[i] += sum * gainR;
            }

            // Every block: keep the rotations exact and drop partials that died away.
            for (int p = 0; p < count;)
            {
                auto& o = oscillators[p];
                const auto length = o.c * o.c + o.s * o.s;
                const auto fix = 1.5f - 0.5f * length; // one Newton step towards 1
                o.c *= fix;
                o.s *= fix;
                if (o.prompt + o.after < 2.0e-6f)
                    o = oscillators[--count];
                else
                    ++p;
            }
            if (count == 0 && knock < 1.0e-6f && thump < 1.0e-6f)
                active = false;
        }

    private:
        struct Oscillator
        {
            float c = 1, s = 0, cw = 1, sw = 0;
            float prompt = 0, after = 0, promptDecay = 1, afterDecay = 1, damped = 1;
        };

        static constexpr int maxOscillators = 48;
        Oscillator oscillators[maxOscillators];
        int count = 0, noteNumber = 60;
        bool active = false, released = false;
        double sampleRate = 48000.0;
        float knock = 0, knockDecay = 0, thump = 0, thumpDecay = 0, gainL = 1, gainR = 1, fade = 1, fadeDecay = 1;
        Svf knockFilter, thumpFilter;
        Noise noise;
    };

    class GrandEngine final : public VoiceEngine<GrandVoice>
    {
    public:
        explicit GrandEngine (Params p) : VoiceEngine (40)
        {
            for (const auto* id : { "hammer", "tone", "dynamics", "mechanics", "decay", "damper", "detune", "stretch",
                                    "resonance", "body", "width", "room", "roomSize" })
                values[id] = p (id);
            for (auto& slot : voices)
                slot.voice.block = &block;
        }

        double getTailSeconds() const override { return 4.0; }

    private:
        float v (const char* id) const { return values.at (id)->load (std::memory_order_relaxed); }

        void prepareEngine (double rate, int) override
        {
            room.prepare (rate);
            shownBody = -1.0f;
        }

        void resetEngine() override
        {
            room.reset();
            for (auto& f : eq) f.reset();
        }

        void beginBlock (int) override
        {
            block.hammer = v ("hammer");       block.tone = v ("tone");
            block.dynamics = v ("dynamics");   block.mechanics = v ("mechanics");
            block.decay = v ("decay");         block.damper = v ("damper");
            block.detune = v ("detune");       block.stretch = v ("stretch");
            block.width = v ("width");
        }

        void endBlock (juce::AudioBuffer<float>& buffer) override
        {
            auto* l = buffer.getWritePointer (0);
            auto* r = buffer.getWritePointer (1);
            const auto n = buffer.getNumSamples();

            // The soundboard: warmth and body around 100-250 Hz, a little air from the tone.
            const auto body = v ("body"), tone = v ("tone");
            if (! juce::exactlyEqual (body, shownBody) || ! juce::exactlyEqual (tone, shownTone))
            {
                shownBody = body;
                shownTone = tone;
                for (int ch = 0; ch < 2; ++ch)
                {
                    eq[ch * 4 + 0].setHighpass (28.0f, 0.7f, sampleRate);
                    eq[ch * 4 + 1].setPeak (115.0f, 0.9f, -1.0f + 4.0f * body, sampleRate);
                    eq[ch * 4 + 2].setPeak (260.0f, 1.2f, 2.5f * body - 0.5f, sampleRate);
                    eq[ch * 4 + 3].setHighShelf (5000.0f, 3.0f * tone, sampleRate);
                }
            }
            for (int i = 0; i < n; ++i)
            {
                auto x = l[i], y = r[i];
                for (int f = 0; f < 4; ++f)
                {
                    x = eq[f].process (x);
                    y = eq[4 + f].process (y);
                }
                l[i] = x;
                r[i] = y;
            }

            // With the pedal down the other strings ring along: more room.
            const auto send = v ("room") + (sustain ? v ("resonance") * 0.35f : 0.0f);
            room.process (l, r, n, send, 0.55f + 0.42f * v ("roomSize"), 0.55f);
        }

        std::map<juce::String, std::atomic<float>*> values;
        GrandBlock block;
        Biquad eq[8];
        float shownBody = -1.0f, shownTone = 0.0f;
        Room room;
    };
}

const Spec& getGrandSpec()
{
    static const Spec spec = []
    {
        Spec s;
        s.key = "grand";
        s.name = "Grand";
        s.tagline = "Modelled grand piano";
        s.category = "Piano";
        s.accent = juce::Colour (0xffe8b86b);
        s.lowestKey = 21;
        s.highestKey = 108;

        s.params = {
            knob ("hammer", "Hardness", 0.0f, 1.0f, 0.55f),
            knob ("tone", "Tone", -1.0f, 1.0f, 0.0f, Unit::bipolar),
            knob ("dynamics", "Dynamics", 0.0f, 1.0f, 0.5f),
            knob ("mechanics", "Mechanics", 0.0f, 1.0f, 0.3f),
            knob ("decay", "Sustain", 0.3f, 2.5f, 1.0f, Unit::multiplier, 1.0f),
            knob ("damper", "Damping", 0.03f, 1.0f, 0.2f, Unit::seconds, 0.2f),
            knob ("detune", "Unison", 0.0f, 1.0f, 0.2f),
            knob ("stretch", "Stretch", 0.0f, 1.0f, 0.5f),
            knob ("resonance", "Pedal res", 0.0f, 1.0f, 0.4f),
            knob ("body", "Body", 0.0f, 1.0f, 0.5f),
            knob ("width", "Width", 0.0f, 1.0f, 0.6f),
            knob ("room", "Room", 0.0f, 1.0f, 0.25f),
            knob ("roomSize", "Size", 0.0f, 1.0f, 0.55f),
        };

        s.rows = {
            { { "Hammer", { "hammer", "tone", "dynamics", "mechanics" }, {} },
              { "Strings", { "decay", "damper", "detune", "stretch", "resonance" }, {} } },
            { { "Soundboard", { "body", "width" }, {} },
              { "Room", { "room", "roomSize" }, {} } },
        };

        s.presets = {
            { "Concert Grand", {} },
            { "Bright Pop", { { "hammer", 0.8f }, { "tone", 0.45f }, { "room", 0.15f }, { "body", 0.4f } } },
            { "Felt Piano", { { "hammer", 0.1f }, { "tone", -0.65f }, { "mechanics", 0.75f }, { "dynamics", 0.3f }, { "room", 0.4f },
                              { "roomSize", 0.6f }, { "damper", 0.35f } } },
            { "Upright", { { "decay", 0.6f }, { "body", 0.25f }, { "width", 0.3f }, { "mechanics", 0.5f }, { "detune", 0.35f },
                           { "tone", 0.1f }, { "roomSize", 0.35f }, { "room", 0.2f } } },
            { "Honky-Tonk", { { "detune", 1.0f }, { "tone", 0.3f }, { "hammer", 0.7f }, { "body", 0.3f }, { "width", 0.4f }, { "room", 0.15f } } },
            { "Ballad Hall", { { "tone", -0.2f }, { "room", 0.5f }, { "roomSize", 0.85f }, { "decay", 1.4f }, { "resonance", 0.7f } } },
        };

        s.makeEngine = [] (Params p) { return std::make_unique<GrandEngine> (p); };
        return s;
    }();
    return spec;
}
}
