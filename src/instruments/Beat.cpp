// Beat: a synthesized drum kit on the General MIDI drum notes (36 kick, 38 snare, 42
// closed hat...). Every sound is made from the same few parts: a tone that drops in
// pitch, filtered noise and a cluster of metallic square waves, each with its own decay.
#include "Fx.h"
#include "Registry.h"

#include <map>

namespace micfx::inst
{
namespace
{
    enum class Drum { none, kick, snare, rim, clap, closedHat, pedalHat, openHat, tom, crash, ride, tambourine, cowbell, shaker, claves };

    struct BeatBlock
    {
        float kickTune = 0, kickDecay = 0.5f, kickPunch = 0.5f, snareTune = 0, snappy = 0.6f, snareDecay = 0.2f;
        float hatTone = 0.5f, hatDecay = 0.4f, tomTune = 0, cymbalDecay = 1.5f;
    };

    struct Mapping
    {
        Drum drum = Drum::none;
        float pitch = 0.0f; // toms: Hz
        float pan = 0.0f;   // -1..1
    };

    Mapping mapNote (int note)
    {
        switch (note)
        {
            case 35: case 36: return { Drum::kick, 0, 0 };
            case 37:          return { Drum::rim, 0, -0.05f };
            case 38: case 40: return { Drum::snare, 0, -0.05f };
            case 39:          return { Drum::clap, 0, 0.05f };
            case 42:          return { Drum::closedHat, 0, 0.35f };
            case 44:          return { Drum::pedalHat, 0, 0.35f };
            case 46:          return { Drum::openHat, 0, 0.35f };
            case 41:          return { Drum::tom, 82, -0.5f };
            case 43:          return { Drum::tom, 96, -0.35f };
            case 45:          return { Drum::tom, 112, -0.15f };
            case 47:          return { Drum::tom, 132, 0.05f };
            case 48:          return { Drum::tom, 156, 0.2f };
            case 50:          return { Drum::tom, 184, 0.35f };
            case 49: case 52: case 55: case 57: return { Drum::crash, 0, note == 57 ? 0.5f : -0.45f };
            case 51: case 53: case 59:          return { Drum::ride, note == 53 ? 1.0f : 0.0f, 0.45f };
            case 54:          return { Drum::tambourine, 0, 0.3f };
            case 56:          return { Drum::cowbell, 0, 0.2f };
            case 60: case 61: case 62: case 63: case 64: case 65: case 66:
                              return { Drum::tom, 200.0f + 35.0f * (float) (66 - note), (float) (note - 63) * 0.15f };
            case 69: case 70: case 82: return { Drum::shaker, 0, 0.4f };
            case 75: case 76: case 77: return { Drum::claves, 0, -0.3f };
            default:          return {};
        }
    }

    class BeatVoice
    {
    public:
        const BeatBlock* block = nullptr;

        void prepare (double rate) { sampleRate = rate; }

        void start (int note, float velocity)
        {
            const auto& b = *block;
            const auto map = mapNote (note);
            drum = map.drum;
            const auto v = 0.2f + 0.8f * velocity;
            tone = tone2 = noiseLevel = metal = 0.0f;
            time = 0;
            clapTail = false;
            noise.seed ((uint32_t) note * 2654435761u + (uint32_t) (velocity * 3000.0f) + 1u);
            for (auto& p : metalPhases) p = noise.next() * 0.5f + 0.5f;

            const auto st = [] (float semitones) { return std::exp2 (semitones / 12.0f); };
            switch (drum)
            {
                case Drum::kick:
                    setTone (165.0f * st (b.kickTune) * (1.0f + 0.6f * b.kickPunch), 48.0f * st (b.kickTune), 0.035f, 1.0f * v, b.kickDecay / 4.0f);
                    setNoise (Mode::high, 3000.0f, 0.3f * b.kickPunch * v, 0.003f);
                    break;
                case Drum::snare:
                    setTone (190.0f * st (b.snareTune), 175.0f * st (b.snareTune), 0.05f, 0.45f * v, 0.06f + b.snareDecay * 0.2f);
                    setTone2 (330.0f * st (b.snareTune), 0.25f * v, 0.04f);
                    setNoise (Mode::high, 1800.0f, (0.25f + 0.55f * b.snappy) * v, b.snareDecay / 3.0f);
                    break;
                case Drum::rim:
                    setTone (1700.0f, 1650.0f, 0.01f, 0.4f * v, 0.008f);
                    setNoise (Mode::band, 3200.0f, 0.4f * v, 0.01f);
                    break;
                case Drum::clap:
                    setNoise (Mode::band, 1150.0f, 0.75f * v, 0.003f);
                    clapTail = true;
                    clapLevel = 0.75f * v;
                    break;
                case Drum::closedHat:
                case Drum::pedalHat:
                case Drum::openHat:
                {
                    const auto decay = drum == Drum::closedHat ? 0.035f : drum == Drum::pedalHat ? 0.06f : b.hatDecay / 3.0f;
                    setMetal (330.0f, 6500.0f + 4000.0f * b.hatTone, 0.45f * v, decay);
                    setNoise (Mode::high, 8000.0f, 0.12f * v, decay * 0.8f);
                    break;
                }
                case Drum::tom:
                {
                    const auto f = map.pitch * st (b.tomTune);
                    setTone (f * 1.35f, f, 0.08f, 0.8f * v, 0.18f + 30.0f / f);
                    setNoise (Mode::band, f * 4.0f, 0.08f * v, 0.04f);
                    break;
                }
                case Drum::crash:
                    setMetal (420.0f, 5000.0f, 0.3f * v, b.cymbalDecay / 3.0f);
                    setNoise (Mode::high, 5500.0f, 0.25f * v, b.cymbalDecay / 3.5f);
                    break;
                case Drum::ride:
                    setMetal (520.0f, 3500.0f, 0.18f * v, b.cymbalDecay / 2.2f);
                    setNoise (Mode::high, 7000.0f, 0.06f * v, b.cymbalDecay / 4.0f);
                    if (map.pitch > 0.0f) // the bell
                        setTone (2350.0f, 2350.0f, 1.0f, 0.25f * v, b.cymbalDecay / 3.0f);
                    break;
                case Drum::tambourine:
                    setMetal (1100.0f, 7000.0f, 0.3f * v, 0.12f);
                    setNoise (Mode::high, 9000.0f, 0.2f * v, 0.1f);
                    break;
                case Drum::cowbell:
                    setTone (560.0f, 560.0f, 1.0f, 0.3f * v, 0.12f, true);
                    setTone2 (835.0f, 0.3f * v, 0.12f, true);
                    break;
                case Drum::shaker:
                    setNoise (Mode::high, 6500.0f, 0.3f * v, 0.05f);
                    break;
                case Drum::claves:
                    setTone (2500.0f, 2500.0f, 1.0f, 0.4f * v, 0.025f);
                    break;
                case Drum::none:
                    break;
            }

            const auto pan = 0.5f + 0.5f * map.pan;
            gainL = std::cos (pan * juce::MathConstants<float>::halfPi) * 1.41f;
            gainR = std::sin (pan * juce::MathConstants<float>::halfPi) * 1.41f;
            active = drum != Drum::none;
        }

        // Drums ring out whatever the key does; a closed hat chokes an open one.
        void release() {}
        void choke() { if (drum == Drum::openHat) { metalDecay = noiseDecay = decayCoefficient (0.012f, sampleRate); } }
        void kill() { active = false; }
        bool isActive() const { return active; }

        void render (float* left, float* right, int n)
        {
            constexpr auto twoPi = juce::MathConstants<float>::twoPi;
            const auto rate = (float) sampleRate;
            for (int i = 0; i < n; ++i)
            {
                auto out = 0.0f;
                if (tone > 1.0e-6f)
                {
                    toneFrequency = toneEnd + (toneFrequency - toneEnd) * pitchDecay;
                    tonePhase += toneFrequency / rate;
                    tonePhase -= std::floor (tonePhase);
                    out += (toneSquare ? (tonePhase < 0.5f ? 0.6f : -0.6f) : std::sin (twoPi * tonePhase)) * tone;
                    tone *= toneDecay;
                }
                if (tone2 > 1.0e-6f)
                {
                    tone2Phase += tone2Frequency / rate;
                    tone2Phase -= std::floor (tone2Phase);
                    out += (tone2Square ? (tone2Phase < 0.5f ? 0.6f : -0.6f) : std::sin (twoPi * tone2Phase)) * tone2;
                    tone2 *= tone2Decay;
                }
                if (noiseLevel > 1.0e-6f || clapTail)
                {
                    float bp, hp;
                    const auto lp = noiseFilter.process (noise.next(), bp, hp);
                    juce::ignoreUnused (lp);
                    auto level = noiseLevel;
                    if (clapTail)
                    {
                        // Three quick claps, then the room's tail.
                        const auto ms = (float) time / rate * 1000.0f;
                        if (ms < 30.0f)
                            level = clapLevel * std::exp (-std::fmod (ms, 10.0f) / 2.5f);
                        else
                        {
                            clapTail = false;
                            noiseLevel = clapLevel * 0.6f;
                            noiseDecay = decayCoefficient (0.14f, sampleRate);
                            level = noiseLevel;
                        }
                    }
                    out += (noiseMode == Mode::high ? hp : bp * 2.0f) * level;
                    noiseLevel *= noiseDecay;
                }
                if (metal > 1.0e-6f)
                {
                    static constexpr float ratios[6] = { 1.0f, 1.342f, 1.2312f, 1.6532f, 1.9523f, 2.1523f };
                    auto sum = 0.0f;
                    for (int k = 0; k < 6; ++k)
                    {
                        metalPhases[k] += metalBase * ratios[k] / rate;
                        metalPhases[k] -= std::floor (metalPhases[k]);
                        sum += metalPhases[k] < 0.5f ? 1.0f : -1.0f;
                    }
                    out += metalFilter.highpass (metalFilter2.highpass (sum * 0.25f)) * metal;
                    metal *= metalDecay;
                }

                left[i] += out * gainL * 0.4f;
                right[i] += out * gainR * 0.4f;
                ++time;
            }

            if (tone < 1.0e-5f && tone2 < 1.0e-5f && noiseLevel < 1.0e-5f && metal < 1.0e-5f && ! clapTail)
                active = false;
        }

    private:
        enum class Mode { band, high };

        void setTone (float from, float to, float sweep, float level, float decay, bool square = false)
        {
            toneFrequency = from; toneEnd = to; tonePhase = 0.25f; tone = level; toneSquare = square;
            pitchDecay = decayCoefficient (sweep, sampleRate);
            toneDecay = decayCoefficient (decay, sampleRate);
        }

        void setTone2 (float frequency, float level, float decay, bool square = false)
        {
            tone2Frequency = frequency; tone2Phase = 0.0f; tone2 = level; tone2Square = square;
            tone2Decay = decayCoefficient (decay, sampleRate);
        }

        void setNoise (Mode mode, float frequency, float level, float decay)
        {
            noiseMode = mode;
            noiseFilter.set (frequency, mode == Mode::band ? 0.55f : 0.1f, sampleRate);
            noiseFilter.reset();
            noiseLevel = level;
            noiseDecay = decayCoefficient (decay, sampleRate);
        }

        void setMetal (float base, float highpass, float level, float decay)
        {
            metalBase = base;
            metalFilter.set (highpass, 0.2f, sampleRate);
            metalFilter2.set (highpass, 0.2f, sampleRate);
            metalFilter.reset();
            metalFilter2.reset();
            metal = level;
            metalDecay = decayCoefficient (decay, sampleRate);
        }

        double sampleRate = 48000.0;
        Drum drum = Drum::none;
        float tone = 0, toneFrequency = 0, toneEnd = 0, tonePhase = 0, pitchDecay = 1, toneDecay = 1;
        float tone2 = 0, tone2Frequency = 0, tone2Phase = 0, tone2Decay = 1;
        bool toneSquare = false, tone2Square = false;
        float noiseLevel = 0, noiseDecay = 1, clapLevel = 0;
        Mode noiseMode = Mode::high;
        bool clapTail = false;
        float metal = 0, metalDecay = 1, metalBase = 300, metalPhases[6] {};
        float gainL = 1, gainR = 1;
        int time = 0;
        bool active = false;
        Svf noiseFilter, metalFilter, metalFilter2;
        Noise noise;
    };

    class BeatEngine final : public VoiceEngine<BeatVoice>
    {
    public:
        explicit BeatEngine (Params p) : VoiceEngine (32)
        {
            for (const auto* id : { "kickTune", "kickDecay", "kickPunch", "snareTune", "snappy", "snareDecay", "hatTone", "hatDecay",
                                    "tomTune", "cymbalDecay", "drive", "room" })
                values[id] = p (id);
            for (auto& slot : voices)
                slot.voice.block = &block;
        }

        double getTailSeconds() const override { return 4.0; }

    private:
        float v (const char* id) const { return values.at (id)->load (std::memory_order_relaxed); }

        void prepareEngine (double rate, int) override { room.prepare (rate); }
        void resetEngine() override { room.reset(); }

        void beginBlock (int) override
        {
            block.kickTune = v ("kickTune");     block.kickDecay = v ("kickDecay");   block.kickPunch = v ("kickPunch");
            block.snareTune = v ("snareTune");   block.snappy = v ("snappy");         block.snareDecay = v ("snareDecay");
            block.hatTone = v ("hatTone");       block.hatDecay = v ("hatDecay");     block.tomTune = v ("tomTune");
            block.cymbalDecay = v ("cymbalDecay");
        }

        void noteOn (int note, float velocity) override
        {
            if (mapNote (note).drum == Drum::none)
                return;
            if (note == 42 || note == 44)
                for (auto& slot : voices)
                    slot.voice.choke();
            VoiceEngine::noteOn (note, velocity);
        }

        void endBlock (juce::AudioBuffer<float>& buffer) override
        {
            auto* l = buffer.getWritePointer (0);
            auto* r = buffer.getWritePointer (1);
            const auto n = buffer.getNumSamples();

            const auto drive = v ("drive");
            if (drive > 0.001f)
            {
                const auto g = 1.0f + 5.0f * drive;
                const auto makeup = 1.0f / std::tanh (g * 0.5f) * 0.5f;
                for (int i = 0; i < n; ++i)
                {
                    l[i] = std::tanh (l[i] * g) * makeup;
                    r[i] = std::tanh (r[i] * g) * makeup;
                }
            }
            room.process (l, r, n, v ("room"), 0.5f, 0.6f);
        }

        std::map<juce::String, std::atomic<float>*> values;
        BeatBlock block;
        Room room;
    };
}

const Spec& getBeatSpec()
{
    static const Spec spec = []
    {
        Spec s;
        s.key = "beat";
        s.name = "Beat";
        s.tagline = "Drum kit";
        s.category = "Drums";
        s.accent = juce::Colour (0xfff05f8a);
        s.lowestKey = 35;
        s.highestKey = 59;

        s.params = {
            knob ("kickTune", "Tune", -12.0f, 12.0f, 0.0f, Unit::semitones),
            knob ("kickDecay", "Decay", 0.1f, 1.6f, 0.5f, Unit::seconds, 0.5f),
            knob ("kickPunch", "Punch", 0.0f, 1.0f, 0.5f),
            knob ("snareTune", "Tune", -12.0f, 12.0f, 0.0f, Unit::semitones),
            knob ("snappy", "Snappy", 0.0f, 1.0f, 0.6f),
            knob ("snareDecay", "Decay", 0.05f, 0.6f, 0.2f, Unit::seconds, 0.2f),
            knob ("hatTone", "Tone", 0.0f, 1.0f, 0.5f),
            knob ("hatDecay", "Open", 0.1f, 1.5f, 0.45f, Unit::seconds, 0.45f),
            knob ("tomTune", "Toms", -12.0f, 12.0f, 0.0f, Unit::semitones),
            knob ("cymbalDecay", "Cymbals", 0.4f, 5.0f, 1.8f, Unit::seconds, 1.5f),
            knob ("drive", "Drive", 0.0f, 1.0f, 0.0f),
            knob ("room", "Room", 0.0f, 1.0f, 0.12f),
        };

        s.rows = {
            { { "Kick", { "kickTune", "kickDecay", "kickPunch" }, {} },
              { "Snare", { "snareTune", "snappy", "snareDecay" }, {} },
              { "Hats", { "hatTone", "hatDecay" }, {} } },
            { { "Toms and cymbals", { "tomTune", "cymbalDecay" }, {} },
              { "Bus", { "drive", "room" }, {} } },
        };

        s.presets = {
            { "Studio Kit", {} },
            { "Boom Kit", { { "kickDecay", 1.3f }, { "kickTune", -3.0f }, { "kickPunch", 0.3f }, { "snareDecay", 0.3f }, { "room", 0.08f } } },
            { "Tight Kit", { { "kickDecay", 0.3f }, { "kickPunch", 0.8f }, { "snareTune", 3.0f }, { "snareDecay", 0.12f },
                             { "hatDecay", 0.25f }, { "room", 0.05f } } },
            { "Lo-fi Kit", { { "drive", 0.55f }, { "hatTone", 0.15f }, { "snappy", 0.4f }, { "room", 0.0f }, { "kickTune", -2.0f } } },
            { "Big Room", { { "room", 0.45f }, { "snareDecay", 0.35f }, { "tomTune", -3.0f }, { "cymbalDecay", 3.0f } } },
        };

        s.keyName = [] (int note) -> juce::String
        {
            switch (note)
            {
                case 35: case 36: return "Kick";   case 37: return "Rim";    case 38: return "Snare";  case 39: return "Clap";
                case 40: return "Snare";           case 42: return "Hat";    case 44: return "Pedal";  case 46: return "Open";
                case 41: case 43: case 45: case 47: case 48: case 50: return "Tom";
                case 49: case 52: case 55: case 57: return "Crash";
                case 51: case 59: return "Ride";   case 53: return "Bell";   case 54: return "Tamb";   case 56: return "Cow";
                default: return {};
            }
        };
                s.makeEngine = [] (Params p) { return std::make_unique<BeatEngine> (p); };
        return s;
    }();
    return spec;
}
}
