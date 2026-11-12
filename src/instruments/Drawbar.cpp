// Drawbar: a tonewheel organ. Nine drawbars mix sine "wheels" at the classic footages,
// with key click, the one-shot percussion, a scanner-style vibrato/chorus, an overdriven
// amp and a rotary speaker (horn and drum spinning up and down at their own speeds).
#include "Fx.h"
#include "Registry.h"

#include <map>

namespace micfx::inst
{
namespace
{
    // 16', 5 1/3', 8', 4', 2 2/3', 2', 1 3/5', 1 1/3', 1'
    constexpr float footages[9] = { 0.5f, 1.5f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 8.0f };
    constexpr const char* drawbarIds[9] = { "db16", "db513", "db8", "db4", "db223", "db2", "db135", "db113", "db1" };

    struct DrawbarBlock
    {
        float levels[9] {};
        float click = 0.3f, norm = 1.0f;
        bool percussion = false;
        int percussionHarmonic = 1;   // 0: second, 1: third
        float percussionLevel = 0.6f, percussionDecay = 0.2f;
        bool percussionNow = false;   // set by the engine for the note being started
    };

    class DrawbarVoice
    {
    public:
        const DrawbarBlock* block = nullptr;

        void prepare (double rate) { sampleRate = rate; }

        void start (int note, float)
        {
            const auto& b = *block;
            const auto rate = (float) sampleRate;
            const auto f = midiToHz ((float) note);
            for (int d = 0; d < 9; ++d)
            {
                auto hz = f * footages[d];
                while (hz > 5900.0f) // the top wheels fold back an octave
                    hz *= 0.5f;
                steps[d] = hz / rate;
                phases[d] = 0.17f * (float) d + 0.031f * (float) note; // tonewheels never stop turning
                phases[d] -= std::floor (phases[d]);
            }
            percussionStep = f * (b.percussionHarmonic == 0 ? 2.0f : 3.0f) / rate;
            percussionPhase = 0.0f;
            percussion = b.percussion && b.percussionNow ? b.percussionLevel * 0.5f : 0.0f;
            percussionDecay = decayCoefficient (b.percussionDecay, sampleRate);

            click = b.click * 0.12f;
            clickDecay = decayCoefficient (0.004f, sampleRate);
            clickFilter.set (2500.0f + 25.0f * (float) note, 0.3f, sampleRate);
            clickFilter.reset();
            noise.seed ((uint32_t) note * 7919u + 3u);

            gain = 0.0f;
            target = 1.0f;
            ramp = 1.0f / (0.004f * rate);
            active = true;
        }

        void release() { target = 0.0f; ramp = 1.0f / (0.012f * (float) sampleRate); }
        void kill()    { active = false; }
        bool isActive() const { return active; }

        void render (float* left, float* right, int n)
        {
            const auto& b = *block;
            constexpr auto twoPi = juce::MathConstants<float>::twoPi;
            for (int i = 0; i < n; ++i)
            {
                auto sum = 0.0f;
                for (int d = 0; d < 9; ++d)
                {
                    if (b.levels[d] > 0.0f)
                        sum += b.levels[d] * std::sin (twoPi * phases[d]);
                    phases[d] += steps[d];
                    phases[d] -= std::floor (phases[d]);
                }

                if (percussion > 1.0e-5f)
                {
                    sum += percussion * std::sin (twoPi * percussionPhase);
                    percussionPhase += percussionStep;
                    percussionPhase -= std::floor (percussionPhase);
                    percussion *= percussionDecay;
                }

                if (click > 1.0e-5f)
                {
                    sum += clickFilter.bandpass (noise.next()) * click * 6.0f;
                    click *= clickDecay;
                }

                gain += juce::jlimit (-ramp, ramp, target - gain);
                const auto out = sum * gain * 0.1f * b.norm;
                left[i] += out;
                right[i] += out;

                if (target <= 0.0f && gain <= 0.0f)
                {
                    active = false;
                    break;
                }
            }
        }

    private:
        double sampleRate = 48000.0;
        float steps[9] {}, phases[9] {};
        float percussion = 0, percussionDecay = 1, percussionStep = 0, percussionPhase = 0;
        float click = 0, clickDecay = 1, gain = 0, target = 0, ramp = 0.01f;
        Svf clickFilter;
        Noise noise;
        bool active = false;
    };

    // A rotary speaker: the highs go to a horn and the lows to a drum, each spinning
    // (pitch and level moving as it turns) and slowly reaching its new speed.
    class Rotary
    {
    public:
        void prepare (double rate)
        {
            sampleRate = rate;
            for (auto& d : delays)
                d.prepare ((int) (rate * 0.01));
            split.setLowpass (800.0f, 0.707f, rate);
            splitHigh.setHighpass (800.0f, 0.707f, rate);
        }

        void reset()
        {
            for (auto& d : delays) d.reset();
            split.reset();
            splitHigh.reset();
        }

        void process (float* left, float* right, int n, bool fast, float mix)
        {
            if (mix <= 0.001f)
                return;
            const auto hornTarget = fast ? 6.8f : 0.8f, drumTarget = fast ? 5.9f : 0.65f;
            const auto hornSpeed = 1.0f - std::exp (-1.0f / (0.6f * (float) sampleRate));
            const auto drumSpeed = 1.0f - std::exp (-1.0f / (2.5f * (float) sampleRate));
            const auto depth = 0.00035f * (float) sampleRate; // samples of Doppler swing

            for (int i = 0; i < n; ++i)
            {
                hornRate += (hornTarget - hornRate) * hornSpeed;
                drumRate += (drumTarget - drumRate) * drumSpeed;
                hornAngle += hornRate / (float) sampleRate; hornAngle -= std::floor (hornAngle);
                drumAngle += drumRate / (float) sampleRate; drumAngle -= std::floor (drumAngle);

                const auto mono = 0.5f * (left[i] + right[i]);
                const auto low = split.process (mono);
                const auto high = splitHigh.process (mono);
                delays[0].push (high);
                delays[1].push (low);

                const auto h = juce::MathConstants<float>::twoPi * hornAngle;
                const auto d = juce::MathConstants<float>::twoPi * drumAngle;
                // Two microphones, left and right of the cabinet.
                const auto hornL = delays[0].read (2.0f + depth * (1.0f + std::sin (h))) * (0.75f + 0.25f * std::cos (h));
                const auto hornR = delays[0].read (2.0f + depth * (1.0f - std::sin (h))) * (0.75f - 0.25f * std::cos (h));
                const auto drumL = delays[1].read (2.0f + 0.3f * depth * (1.0f + std::sin (d))) * (0.85f + 0.15f * std::cos (d));
                const auto drumR = delays[1].read (2.0f + 0.3f * depth * (1.0f - std::sin (d))) * (0.85f - 0.15f * std::cos (d));

                left[i] += mix * (hornL + drumL - left[i]);
                right[i] += mix * (hornR + drumR - right[i]);
            }
        }

    private:
        double sampleRate = 48000.0;
        DelayLine delays[2];
        Biquad split, splitHigh;
        float hornRate = 0.8f, drumRate = 0.65f, hornAngle = 0.0f, drumAngle = 0.25f;
    };

    class DrawbarEngine final : public VoiceEngine<DrawbarVoice>
    {
    public:
        explicit DrawbarEngine (Params p) : VoiceEngine (32)
        {
            for (const auto* id : drawbarIds)
                values[id] = p (id);
            for (const auto* id : { "percussion", "percHarmonic", "percDecay", "percLevel", "click", "vibrato", "vibratoDepth",
                                    "drive", "rotary", "rotarySpeed", "room" })
                values[id] = p (id);
            for (auto& slot : voices)
                slot.voice.block = &block;
        }

        double getTailSeconds() const override { return 2.0; }

    private:
        float v (const char* id) const { return values.at (id)->load (std::memory_order_relaxed); }

        void prepareEngine (double rate, int) override
        {
            for (auto& d : vibratoDelays) d.prepare ((int) (rate * 0.01));
            rotary.prepare (rate);
            room.prepare (rate);
            for (auto& f : ampFilter) f.setLowpass (5500.0f, 0.7f, rate);
        }

        void resetEngine() override
        {
            for (auto& d : vibratoDelays) d.reset();
            rotary.reset();
            room.reset();
            for (auto& f : ampFilter) f.reset();
        }

        void beginBlock (int) override
        {
            for (int d = 0; d < 9; ++d)
            {
                const auto position = std::round (v (drawbarIds[d]));
                block.levels[d] = position <= 0.0f ? 0.0f : juce::Decibels::decibelsToGain (-3.0f * (8.0f - position));
            }
            // Many drawbars out: a little quieter each, as the organ's own loudness pedal would.
            auto total = 0.0f;
            for (auto level : block.levels)
                total += level;
            block.norm = std::sqrt (3.0f / juce::jmax (3.0f, total));
            block.click = v ("click");
            block.percussion = v ("percussion") >= 0.5f;
            block.percussionHarmonic = (int) std::round (v ("percHarmonic"));
            block.percussionDecay = std::round (v ("percDecay")) < 0.5f ? 0.2f : 0.9f;
            block.percussionLevel = v ("percLevel");
        }

        // Percussion only sounds on notes played with no other key down (single trigger).
        void noteOn (int note, float velocity) override
        {
            auto anyHeld = false;
            for (const auto& slot : voices)
                anyHeld = anyHeld || (slot.held && slot.voice.isActive());
            block.percussionNow = ! anyHeld;
            VoiceEngine::noteOn (note, velocity);
        }

        void endBlock (juce::AudioBuffer<float>& buffer) override
        {
            auto* l = buffer.getWritePointer (0);
            auto* r = buffer.getWritePointer (1);
            const auto n = buffer.getNumSamples();

            // Scanner vibrato / chorus: a wobbling delay, alone or mixed with the dry sound.
            const auto vibratoMode = (int) std::round (v ("vibrato"));
            if (vibratoMode > 0)
            {
                const auto depth = (0.15f + 0.85f * v ("vibratoDepth")) * 0.0012f * (float) sampleRate;
                const auto increment = 6.9f / (float) sampleRate;
                const auto wet = vibratoMode == 1 ? 1.0f : 0.5f;
                for (int i = 0; i < n; ++i)
                {
                    vibratoDelays[0].push (l[i]);
                    vibratoDelays[1].push (r[i]);
                    const auto wobble = 0.5f * (1.0f + std::sin (juce::MathConstants<float>::twoPi * vibratoPhase));
                    const auto delayed0 = vibratoDelays[0].read (2.0f + depth * wobble);
                    const auto delayed1 = vibratoDelays[1].read (2.0f + depth * wobble);
                    l[i] = (1.0f - wet) * l[i] + wet * delayed0;
                    r[i] = (1.0f - wet) * r[i] + wet * delayed1;
                    vibratoPhase += increment;
                    vibratoPhase -= std::floor (vibratoPhase);
                }
            }

            // The amp, from clean to growling.
            const auto drive = v ("drive");
            const auto g = 1.0f + 8.0f * drive * drive + 0.5f;
            const auto makeup = 1.0f / std::tanh (0.6f * g) * 0.6f;
            for (int i = 0; i < n; ++i)
            {
                l[i] = ampFilter[0].process (std::tanh (l[i] * g) * makeup);
                r[i] = ampFilter[1].process (std::tanh (r[i] * g) * makeup);
            }

            // The mod wheel switches the rotor to fast, like the half-moon switch.
            const auto fast = std::round (v ("rotarySpeed")) >= 1.0f || modWheel > 0.5f;
            rotary.process (l, r, n, fast, v ("rotary"));
            room.process (l, r, n, v ("room"), 0.6f, 0.5f);
        }

        std::map<juce::String, std::atomic<float>*> values;
        DrawbarBlock block;
        DelayLine vibratoDelays[2];
        float vibratoPhase = 0.0f;
        Biquad ampFilter[2];
        Rotary rotary;
        Room room;
    };

    Preset registration (const char* name, const char* bars, std::vector<std::pair<const char*, float>> more)
    {
        Preset p { name, {} };
        for (int d = 0; d < 9; ++d)
            p.values.push_back ({ drawbarIds[d], (float) (bars[d] - '0') });
        p.values.insert (p.values.end(), more.begin(), more.end());
        return p;
    }
}

const Spec& getDrawbarSpec()
{
    static const Spec spec = []
    {
        Spec s;
        s.key = "drawbar";
        s.name = "Drawbar";
        s.tagline = "Tonewheel organ";
        s.category = "Organ";
        s.accent = juce::Colour (0xffd9a441);
        s.lowestKey = 36;
        s.highestKey = 96;

        s.params = {
            drawbar ("db16", "16'", 8.0f),  drawbar ("db513", "5\xe2\x85\x93'", 8.0f), drawbar ("db8", "8'", 8.0f),
            drawbar ("db4", "4'", 0.0f),    drawbar ("db223", "2\xe2\x85\x94'", 0.0f), drawbar ("db2", "2'", 0.0f),
            drawbar ("db135", "1\xe2\x85\x97'", 0.0f), drawbar ("db113", "1\xe2\x85\x93'", 0.0f), drawbar ("db1", "1'", 0.0f),
            toggle ("percussion", "Percussion", true),
            choice ("percHarmonic", "Harmonic", { "Second", "Third" }, 1),
            choice ("percDecay", "Decay", { "Fast", "Slow" }, 0),
            knob ("percLevel", "Level", 0.0f, 1.0f, 0.6f),
            knob ("click", "Click", 0.0f, 1.0f, 0.35f),
            choice ("vibrato", "Scanner", { "Off", "Vibrato", "Chorus" }, 2),
            knob ("vibratoDepth", "Depth", 0.0f, 1.0f, 0.5f),
            knob ("drive", "Drive", 0.0f, 1.0f, 0.2f),
            knob ("rotary", "Rotary", 0.0f, 1.0f, 0.8f),
            choice ("rotarySpeed", "Speed", { "Slow", "Fast" }, 0),
            knob ("room", "Room", 0.0f, 1.0f, 0.15f),
        };

        s.rows = {
            { { "Drawbars", { "db16", "db513", "db8", "db4", "db223", "db2", "db135", "db113", "db1" }, {} },
              { "Percussion", { "percussion", "percHarmonic", "percDecay", "percLevel" }, {} } },
            { { "Keys", { "click" }, {} },
              { "Vibrato", { "vibrato", "vibratoDepth" }, {} },
              { "Amp", { "drive" }, {} },
              { "Rotary speaker", { "rotary", "rotarySpeed", "room" }, {} } },
        };

        s.presets = {
            registration ("Jazz Trio", "888000000", {}),
            registration ("Gospel", "888800000", { { "rotarySpeed", 1.0f }, { "drive", 0.35f }, { "percussion", 0.0f } }),
            registration ("Full Organ", "888888888", { { "drive", 0.4f }, { "percussion", 0.0f }, { "click", 0.2f } }),
            registration ("Soft Ballad", "008600000", { { "percussion", 0.0f }, { "click", 0.1f }, { "drive", 0.05f }, { "room", 0.3f } }),
            registration ("Rock Drive", "888000000", { { "drive", 0.75f }, { "rotarySpeed", 1.0f }, { "percHarmonic", 0.0f } }),
            registration ("Bright Pop", "808000008", { { "percLevel", 0.8f }, { "vibrato", 0.0f }, { "drive", 0.1f } }),
            registration ("Cathedral", "846606046", { { "percussion", 0.0f }, { "vibrato", 0.0f }, { "rotary", 0.3f }, { "drive", 0.0f },
                                                      { "click", 0.05f }, { "room", 0.45f } }),
        };

        s.makeEngine = [] (Params p) { return std::make_unique<DrawbarEngine> (p); };
        return s;
    }();
    return spec;
}
}
