// Pluck: plucked strings (guitars, harp, koto) from a waveguide: a burst of noise shaped
// like the pick, then a delay line one period long whose loop filter loses highs a
// little more every pass. The body's resonances, a chorus and a room follow.
#include "Fx.h"
#include "Registry.h"

#include <complex>
#include <map>

namespace micfx::inst
{
namespace
{
    struct PluckBlock
    {
        int body = 0;
        float pick = 0.5f, position = 0.2f, decay = 3.0f, tone = 0.5f, mute = 0.0f, width = 0.5f;
    };

    class PluckVoice
    {
    public:
        const PluckBlock* block = nullptr;

        void prepare (double rate)
        {
            sampleRate = rate;
            line.assign ((size_t) juce::nextPowerOfTwo ((int) (rate / 20.0) + 8), 0.0f);
            burst.assign (line.size(), 0.0f);
            mask = (int) line.size() - 1;
        }

        void start (int note, float velocity)
        {
            const auto& b = *block;
            const auto rate = (float) sampleRate;
            const auto f = midiToHz ((float) note);

            // The loop filter: a one-pole low-pass; brighter strings lose fewer highs.
            const auto brightness = juce::jlimit (0.0f, 1.0f, b.tone * (1.0f - 0.6f * b.mute) + 0.15f * velocity);
            pole = juce::jlimit (0.0f, 0.9f, 0.55f * (1.0f - brightness) + 0.02f);

            // Tune: the loop is the delay plus the filter's own delay at this pitch.
            const auto w = juce::MathConstants<float>::twoPi * f / rate;
            const std::complex<float> z = std::polar (1.0f, -w);
            const auto h = (1.0f - pole) / (1.0f - pole * z);
            const auto filterDelay = -std::arg (h) / w;
            period = juce::jlimit (2.0f, (float) mask - 4.0f, rate / f - filterDelay);

            // Loss per pass, so the note fades by 60 dB over the Decay time (shorter when muted).
            const auto seconds = juce::jmax (0.05f, b.decay * (1.0f - 0.85f * b.mute) * std::exp2 (-((float) note - 52.0f) / 30.0f));
            // (The filter passes the lowest frequencies whole, so the loop gain stays below 1.)
            gain = juce::jmin (0.9995f, std::pow (0.001f, 1.0f / (f * seconds)) / std::abs (h));
            releaseGain = juce::jmin (0.99f, std::pow (0.001f, 1.0f / (f * 0.08f)) / std::abs (h));

            // The pluck: noise, low-passed by the pick and combed by where it hits the string.
            std::fill (line.begin(), line.end(), 0.0f);
            noise.seed ((uint32_t) note * 2246822519u + (uint32_t) (velocity * 977.0f));
            const auto length = (int) std::ceil (period);
            const auto pickCutoff = juce::jlimit (0.05f, 0.95f, 0.25f + 0.7f * b.pick * (0.4f + 0.6f * velocity));
            auto smooth = 0.0f, mean = 0.0f;
            for (int i = 0; i < length; ++i)
            {
                smooth += pickCutoff * (noise.next() - smooth);
                burst[(size_t) i] = smooth;
                mean += smooth;
            }
            mean /= (float) length;
            const auto offset = juce::jlimit (1, juce::jmax (1, length - 1), (int) std::round (b.position * (float) length));
            auto peak = 1.0e-6f;
            for (int i = 0; i < length; ++i)
            {
                const auto x = burst[(size_t) i] - mean;
                const auto combed = x - (burst[(size_t) ((i + offset) % length)] - mean);
                line[(size_t) i] = combed;
                peak = juce::jmax (peak, std::abs (combed));
            }
            const auto level = (0.15f + 0.85f * velocity) * 0.32f / peak;
            for (int i = 0; i < length; ++i)
                line[(size_t) i] *= level;
            write = length;
            last = 0.0f;
            loopGain = gain;
            released = false;
            silentFor = 0;

            const auto pan = 0.5f + 0.5f * juce::jlimit (-1.0f, 1.0f, b.width * ((float) note - 60.0f) / 30.0f);
            gainL = std::cos (pan * juce::MathConstants<float>::halfPi) * 1.2f;
            gainR = std::sin (pan * juce::MathConstants<float>::halfPi) * 1.2f;
            active = true;
        }

        void release()
        {
            released = true;
            loopGain = juce::jmin (loopGain, releaseGain);
        }

        void kill() { active = false; }
        bool isActive() const { return active; }

        void render (float* left, float* right, int n)
        {
            for (int i = 0; i < n; ++i)
            {
                // Read one period back (interpolated), filter, feed back.
                const auto position = (float) write - period;
                const auto index = (int) std::floor (position);
                const auto frac = position - (float) index;
                const auto a = line[(size_t) (index & mask)];
                const auto b = line[(size_t) ((index + 1) & mask)];
                const auto delayed = a + frac * (b - a);
                last = (1.0f - pole) * delayed + pole * last;
                const auto y = last * loopGain;
                line[(size_t) write] = y;
                write = (write + 1) & mask;

                left[i] += y * gainL;
                right[i] += y * gainR;
                silentFor = std::abs (y) < 1.0e-5f ? silentFor + 1 : 0;
            }
            if (silentFor > (int) (period * 2.0f) + 64)
                active = false;
        }

    private:
        double sampleRate = 48000.0;
        std::vector<float> line, burst;
        int mask = 0, write = 0, silentFor = 0;
        float period = 100, pole = 0.3f, gain = 0.99f, releaseGain = 0.9f, loopGain = 0.99f, last = 0, gainL = 1, gainR = 1;
        bool active = false, released = false;
        Noise noise;
    };

    class PluckEngine final : public VoiceEngine<PluckVoice>
    {
    public:
        explicit PluckEngine (Params p) : VoiceEngine (24)
        {
            for (const auto* id : { "body", "pick", "position", "decay", "tone", "mute", "bodyAmount", "width", "chorus", "room" })
                values[id] = p (id);
            for (auto& slot : voices)
                slot.voice.block = &block;
        }

        double getTailSeconds() const override { return 4.0; }

    private:
        float v (const char* id) const { return values.at (id)->load (std::memory_order_relaxed); }

        void prepareEngine (double rate, int) override
        {
            chorus.prepare (rate);
            room.prepare (rate);
            shownBody = -1;
        }

        void resetEngine() override
        {
            chorus.reset();
            room.reset();
            for (auto& f : eq) f.reset();
        }

        void beginBlock (int) override
        {
            block.body = (int) std::round (v ("body"));
            block.pick = v ("pick");
            block.position = v ("position");
            block.decay = v ("decay");
            block.tone = v ("tone");
            block.mute = v ("mute");
            block.width = v ("width");
        }

        void endBlock (juce::AudioBuffer<float>& buffer) override
        {
            auto* l = buffer.getWritePointer (0);
            auto* r = buffer.getWritePointer (1);
            const auto n = buffer.getNumSamples();

            // Each body's resonances: (frequency, Q, dB) x 3.
            const auto body = block.body;
            const auto amount = v ("bodyAmount");
            if (body != shownBody || ! juce::exactlyEqual (amount, shownAmount))
            {
                shownBody = body;
                shownAmount = amount;
                static constexpr float shapes[4][3][3] = {
                    { { 105, 1.5f, 6 }, { 210, 1.8f, 4 }, { 2800, 0.8f, -2 } },  // nylon guitar
                    { { 98, 1.6f, 5 }, { 195, 1.6f, 4 }, { 3200, 0.9f, 3 } },    // steel guitar
                    { { 140, 0.9f, 3 }, { 400, 0.8f, 2 }, { 4000, 0.7f, -3 } },  // harp
                    { { 300, 1.2f, 4 }, { 1100, 1.4f, 5 }, { 3500, 1.0f, 3 } },  // koto
                };
                const auto& shape = shapes[juce::jlimit (0, 3, body)];
                for (int ch = 0; ch < 2; ++ch)
                    for (int k = 0; k < 3; ++k)
                        eq[ch * 3 + k].setPeak (shape[k][0], shape[k][1], shape[k][2] * amount, sampleRate);
            }
            for (int i = 0; i < n; ++i)
            {
                auto x = l[i], y = r[i];
                for (int k = 0; k < 3; ++k)
                {
                    x = eq[k].process (x);
                    y = eq[3 + k].process (y);
                }
                l[i] = x;
                r[i] = y;
            }

            chorus.process (l, r, n, v ("chorus"), 0.4f, 2.5f);
            room.process (l, r, n, v ("room"), 0.72f, 0.45f);
        }

        std::map<juce::String, std::atomic<float>*> values;
        PluckBlock block;
        Biquad eq[6];
        int shownBody = -1;
        float shownAmount = -1.0f;
        Chorus chorus;
        Room room;
    };
}

const Spec& getPluckSpec()
{
    static const Spec spec = []
    {
        Spec s;
        s.key = "pluck";
        s.name = "Pluck";
        s.tagline = "Plucked strings";
        s.category = "Strings";
        s.accent = juce::Colour (0xff8fd16a);
        s.lowestKey = 36;
        s.highestKey = 96;

        s.params = {
            choice ("body", "Body", { "Nylon", "Steel", "Harp", "Koto" }, 0),
            knob ("pick", "Pick", 0.0f, 1.0f, 0.5f),
            knob ("position", "Position", 0.04f, 0.5f, 0.18f),
            knob ("decay", "Sustain", 0.2f, 12.0f, 3.5f, Unit::seconds, 3.0f),
            knob ("tone", "Tone", 0.0f, 1.0f, 0.5f),
            knob ("mute", "Palm mute", 0.0f, 1.0f, 0.0f),
            knob ("bodyAmount", "Resonance", 0.0f, 1.0f, 0.7f),
            knob ("width", "Width", 0.0f, 1.0f, 0.5f),
            knob ("chorus", "Chorus", 0.0f, 1.0f, 0.0f),
            knob ("room", "Room", 0.0f, 1.0f, 0.2f),
        };

        s.rows = {
            { { "String", { "pick", "position", "decay", "tone", "mute" }, {} },
              { "Body", { "body", "bodyAmount" }, {} } },
            { { "Stereo", { "width" }, {} },
              { "Space", { "chorus", "room" }, {} } },
        };

        s.presets = {
            { "Nylon Guitar", {} },
            { "Steel Guitar", { { "body", 1.0f }, { "pick", 0.75f }, { "tone", 0.75f }, { "decay", 5.0f }, { "position", 0.12f } } },
            { "Harp", { { "body", 2.0f }, { "pick", 0.3f }, { "tone", 0.55f }, { "decay", 6.0f }, { "position", 0.3f },
                        { "width", 0.8f }, { "room", 0.4f } } },
            { "Koto", { { "body", 3.0f }, { "pick", 0.85f }, { "tone", 0.8f }, { "decay", 2.5f }, { "position", 0.08f }, { "room", 0.3f } } },
            { "Muted Pluck", { { "body", 1.0f }, { "mute", 0.75f }, { "pick", 0.6f }, { "tone", 0.5f }, { "room", 0.1f } } },
            { "Dream Strum", { { "body", 1.0f }, { "pick", 0.6f }, { "tone", 0.7f }, { "decay", 7.0f }, { "chorus", 0.5f },
                               { "room", 0.45f }, { "width", 0.8f } } },
        };

        s.makeEngine = [] (Params p) { return std::make_unique<PluckEngine> (p); };
        return s;
    }();
    return spec;
}
}
