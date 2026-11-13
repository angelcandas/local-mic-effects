#pragma once

#include "Biquad.h"

#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <vector>

// Colour and doubling for the voice: saturation curves that run oversampled, a tilt
// tone control, and the doubler's micro pitch shift and second-take modes.
namespace fx
{
//==============================================================================
// A tilt EQ around 800 Hz: positive brightens (highs up, lows down), negative darkens.
// tilt is -1..1, up to +/-6 dB at the ends of the range. Off (and untouched) at 0.
class Tilt
{
public:
    void prepare (double newSampleRate) noexcept { sampleRate = newSampleRate; lastTilt = 2.0f; reset(); }
    void reset() noexcept { for (auto& f : low) f.reset(); for (auto& f : high) f.reset(); }

    void setTilt (float tilt) noexcept
    {
        tilt = juce::jlimit (-1.0f, 1.0f, tilt);
        active = std::abs (tilt) > 0.005f;
        if (tilt == lastTilt)
            return;

        lastTilt = tilt;
        for (auto& f : low)  f.setup (Biquad::Type::lowShelf, sampleRate, 800.0, 0.5, -6.0 * tilt);
        for (auto& f : high) f.setup (Biquad::Type::highShelf, sampleRate, 800.0, 0.5, 6.0 * tilt);
    }

    float process (size_t channel, float x) noexcept
    {
        return active ? high[channel].process (0, low[channel].process (0, x)) : x;
    }

private:
    double sampleRate = 48000.0;
    std::array<Biquad, 2> low, high;
    float lastTilt = 2.0f;
    bool active = false;
};

//==============================================================================
// The Saturation effect's Tape, Tube and Clip types (Warm stays on fx::Saturation).
// All three run 4x oversampled through polyphase IIR half-band filters (a few samples
// of delay, reported as latency; the dry is delayed to match for Mix), and are
// loudness-compensated: the output follows the input's level over ~300 ms, so turning
// up drive changes the colour, not the volume.
//
//   Tape: soft, slightly asymmetric, with high-frequency pre-emphasis before the curve
//         and de-emphasis after (so the highs saturate a little sooner) and a small
//         low bump.
//   Tube: asymmetric, mostly even harmonics; DC is blocked after the curve.
//   Clip: a firm cubic soft clip, for rap and rock edge.
class ColourSaturation
{
public:
    enum class Type { tape = 1, tube = 2, clip = 3 };

    struct Settings
    {
        Type type = Type::tape;
        float driveDb = 12.0f;
        float mix = 1.0f;   // 0..1
        float tone = 0.0f;  // -1..1 tilt
    };

    static constexpr size_t oversamplingOrder = 2; // 4x

    void prepare (const juce::dsp::ProcessSpec& spec)
    {
        sampleRate = spec.sampleRate;
        numChannels = juce::jmin ((int) spec.numChannels, 2);
        oversampling = std::make_unique<juce::dsp::Oversampling<float>> ((size_t) juce::jmax (1, numChannels), oversamplingOrder,
                                                                        juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true, true);
        oversampling->initProcessing (spec.maximumBlockSize);
        latency = juce::roundToInt (oversampling->getLatencyInSamples());
        for (auto& d : dryDelay)
            d.assign ((size_t) latency + 1, 0.0f);

        const auto osRate = sampleRate * (double) (1 << oversamplingOrder);
        for (auto& f : preEmphasis)  f.setup (Biquad::Type::highShelf, osRate, 3000.0, 0.7, 5.0);
        for (auto& f : deEmphasis)   f.setup (Biquad::Type::highShelf, osRate, 3000.0, 0.7, -5.0);
        for (auto& f : lowBump)      f.setup (Biquad::Type::peak, sampleRate, 90.0, 0.8, 1.5);
        for (auto& f : dcBlock)      f.setup (Biquad::Type::highPass, sampleRate, 12.0, 0.707);
        tilt.prepare (sampleRate);
        followPole = (float) std::exp (-1.0 / (0.3 * sampleRate));
        dryScratch.setSize (2, (int) spec.maximumBlockSize);
        reset();
    }

    void reset()
    {
        if (oversampling != nullptr)
            oversampling->reset();
        for (auto& f : preEmphasis) f.reset();
        for (auto& f : deEmphasis) f.reset();
        for (auto& f : lowBump) f.reset();
        for (auto& f : dcBlock) f.reset();
        for (auto& d : dryDelay) std::fill (d.begin(), d.end(), 0.0f);
        delayWrite = 0;
        tilt.reset();
        inPower = outPower = 0.0f;
        compensation = 1.0f;
    }

    int getLatencySamples() const noexcept { return latency; }

    void setSettings (const Settings& s) noexcept
    {
        settings = s;
        tilt.setTilt (s.tone);
    }

    static float shape (Type type, float x) noexcept
    {
        switch (type)
        {
            case Type::tape:
                // A tanh with a little bias: soft, with some even harmonics.
                return std::tanh (x + 0.12f) - std::tanh (0.12f);

            case Type::tube:
                // Softer on one side than the other: mostly even harmonics.
                return x >= 0.0f ? std::tanh (1.3f * x) / 1.3f : std::tanh (0.75f * x) / 0.75f;

            case Type::clip:
            default:
            {
                // Cubic soft clip: slope 1 at 0, flat at +/-2/3.
                const auto t = juce::jlimit (-1.0f, 1.0f, x / 1.5f);
                return t - t * t * t / 3.0f;
            }
        }
    }

    void process (juce::dsp::AudioBlock<float>& block) noexcept
    {
        if (oversampling == nullptr)
            return;

        const auto n = block.getNumSamples();
        const auto chans = juce::jmin ((size_t) numChannels, block.getNumChannels());
        const auto drive = juce::Decibels::decibelsToGain (settings.driveDb);
        const auto mix = juce::jlimit (0.0f, 1.0f, settings.mix);
        const auto type = settings.type;
        const auto staticGain = 1.0f / std::sqrt (drive); // the loudness match does the rest

        // Keep the dry (for Mix and the level match) before the block is overwritten.
        for (size_t ch = 0; ch < chans; ++ch)
            dryScratch.copyFrom ((int) ch, 0, block.getChannelPointer (ch), (int) n);

        auto up = oversampling->processSamplesUp (block);
        for (size_t ch = 0; ch < chans; ++ch)
        {
            auto* data = up.getChannelPointer (ch);
            for (size_t i = 0; i < up.getNumSamples(); ++i)
            {
                auto x = data[i] * drive;
                if (type == Type::tape)
                    x = preEmphasis[ch].process (0, x);
                x = shape (type, x);
                if (type == Type::tape)
                    x = deEmphasis[ch].process (0, x);
                data[i] = x * staticGain;
            }
        }
        oversampling->processSamplesDown (block);

        const auto size = (int) dryDelay[0].size();
        for (size_t i = 0; i < n; ++i)
        {
            // Level in (as heard, delayed like the wet) and out, for the loudness match.
            std::array<float, 2> dry {}, wet {};
            auto inSquared = 0.0f, outSquared = 0.0f;
            for (size_t ch = 0; ch < chans; ++ch)
            {
                auto& line = dryDelay[ch];
                line[(size_t) delayWrite] = dryScratch.getSample ((int) ch, (int) i);
                dry[ch] = line[(size_t) ((delayWrite + 1) % size)];

                auto x = block.getSample ((int) ch, (int) i);
                if (type == Type::tape) x = lowBump[ch].process (0, x);
                if (type == Type::tube) x = dcBlock[ch].process (0, x);
                x = tilt.process (ch, x);
                wet[ch] = x;
                inSquared += dry[ch] * dry[ch];
                outSquared += x * x;
            }
            delayWrite = (delayWrite + 1) % size;

            inPower = inSquared + followPole * (inPower - inSquared);
            outPower = outSquared + followPole * (outPower - outSquared);
            if (outPower > 1.0e-9f)
                compensation = juce::jlimit (0.0625f, 16.0f, std::sqrt (inPower / outPower));

            for (size_t ch = 0; ch < chans; ++ch)
                block.setSample ((int) ch, (int) i, dry[ch] + mix * (wet[ch] * compensation - dry[ch]));
        }
    }

private:
    double sampleRate = 48000.0;
    int numChannels = 2, latency = 0, delayWrite = 0;
    Settings settings;
    std::unique_ptr<juce::dsp::Oversampling<float>> oversampling;
    std::array<Biquad, 2> preEmphasis, deEmphasis, lowBump, dcBlock;
    std::array<std::vector<float>, 2> dryDelay;
    juce::AudioBuffer<float> dryScratch;
    Tilt tilt;
    float followPole = 0.0f, inPower = 0.0f, outPower = 0.0f, compensation = 1.0f;
};

//==============================================================================
// The Doubler's Micro shift and Double track modes (Classic stays on fx::Doubler).
//
//   Micro shift: two copies, one detuned a few cents up and one down, made with two
//                crossfading read heads each (no pitch detection: no latency, and it
//                works on chords and groups), the second 10 ms later than the first.
//                Because the copies' pitch differs from the dry, they don't comb in
//                mono the way fixed short delays do.
//   Double track: one copy whose timing and pitch drift slowly and randomly, like a
//                second take sung along.
// Spread pans the copies apart; Mix is how loud they are next to the dry.
class VoiceDoubler
{
public:
    enum class Mode { microShift = 1, doubleTrack = 2 };

    struct Settings
    {
        Mode mode = Mode::microShift;
        float mix = 0.5f;          // 0..1
        float spread = 0.8f;       // 0..1
        float detuneCents = 8.0f;
        float delayMs = 12.0f;
    };

    static constexpr float windowMs = 30.0f, maxDelayMs = 80.0f;

    void prepare (double newSampleRate)
    {
        sampleRate = newSampleRate;
        line.assign ((size_t) ((maxDelayMs + windowMs + 20.0f) * 0.001f * (float) sampleRate) + 4, 0.0f);
        driftPole = (float) std::exp (-1.0 / (0.6 * sampleRate));
        reset();
    }

    void reset() noexcept
    {
        std::fill (line.begin(), line.end(), 0.0f);
        write = 0;
        phaseUp = 0.0f; phaseDown = 0.25f;
        drift = driftTarget = 0.0f;
        samplesToNextTarget = 0;
    }

    void setSettings (const Settings& s) noexcept { settings = s; }

    void process (juce::dsp::AudioBlock<float>& block) noexcept
    {
        const auto numChannels = std::min<size_t> (block.getNumChannels(), 2);
        const auto ms = 0.001f * (float) sampleRate;
        const auto window = windowMs * ms;
        const auto base = juce::jlimit (1.0f, maxDelayMs, settings.delayMs) * ms;
        const auto ratio = std::exp2 (juce::jlimit (0.0f, 50.0f, settings.detuneCents) / 1200.0f);
        const auto step = std::abs (ratio - 1.0f) / window; // read-head phase per sample
        const auto pan = 0.5f + 0.5f * juce::jlimit (0.0f, 1.0f, settings.spread);
        const auto mix = settings.mix;

        for (size_t i = 0; i < block.getNumSamples(); ++i)
        {
            auto mid = 0.0f;
            for (size_t ch = 0; ch < numChannels; ++ch)
                mid += block.getSample ((int) ch, (int) i);
            mid /= (float) numChannels;

            line[(size_t) write] = mid;
            float a, b;

            if (settings.mode == Mode::microShift)
            {
                // Up: the delay shrinks across the window; down: it grows. Two heads half
                // a window apart, each faded in and out (Hann), so the sum stays level.
                phaseUp = wrap (phaseUp + step);
                phaseDown = wrap (phaseDown + step);
                a = heads (base + window * 0.5f, window, phaseUp, true);
                b = heads (base + 10.0f * ms + window * 0.5f, window, phaseDown, false);
            }
            else
            {
                // A slowly wandering delay: a new random target every 0.4-1 s, glided to.
                if (--samplesToNextTarget <= 0)
                {
                    driftTarget = random.nextFloat() * 2.0f - 1.0f;
                    samplesToNextTarget = (int) ((0.4f + 0.6f * random.nextFloat()) * (float) sampleRate);
                }
                drift = driftTarget + driftPole * (drift - driftTarget);
                const auto wander = 0.5f * settings.detuneCents * ms * drift; // 8 cents ~ +/-4 ms
                a = read (base + 12.0f * ms + wander);
                b = a;
            }

            write = (write + 1) % (int) line.size();

            if (numChannels == 2 && settings.mode == Mode::doubleTrack)
            {
                // One copy, pushed towards the right by Spread.
                block.addSample (0, (int) i, mix * a * (1.0f - pan));
                block.addSample (1, (int) i, mix * a * pan);
            }
            else if (numChannels == 2)
            {
                block.addSample (0, (int) i, mix * (a * pan + b * (1.0f - pan)));
                block.addSample (1, (int) i, mix * (b * pan + a * (1.0f - pan)));
            }
            else
            {
                block.addSample (0, (int) i, mix * 0.5f * (a + b));
            }
        }
    }

private:
    static float wrap (float p) noexcept { return p >= 1.0f ? p - 1.0f : p; }

    float read (float delaySamples) const noexcept
    {
        const auto size = (int) line.size();
        delaySamples = juce::jlimit (1.0f, (float) size - 2.0f, delaySamples);
        const auto position = (float) write - delaySamples + (float) size;
        const auto whole = (int) position;
        const auto f = position - (float) whole;
        const auto x0 = line[(size_t) (whole % size)], x1 = line[(size_t) ((whole + 1) % size)];
        return x0 + f * (x1 - x0);
    }

    // Two read heads sweeping a window around `centre`, half a window apart.
    float heads (float centre, float window, float phase, bool up) const noexcept
    {
        float sum = 0.0f;
        for (const auto offset : { 0.0f, 0.5f })
        {
            const auto p = wrap (phase + offset);
            const auto sweep = up ? (0.5f - p) : (p - 0.5f); // up: delay falls as p rises
            const auto gain = std::sin (juce::MathConstants<float>::pi * p);
            sum += gain * gain * read (centre + window * sweep);
        }
        return sum;
    }

    double sampleRate = 48000.0;
    Settings settings;
    std::vector<float> line;
    int write = 0, samplesToNextTarget = 0;
    float phaseUp = 0.0f, phaseDown = 0.25f, drift = 0.0f, driftTarget = 0.0f, driftPole = 0.0f;
    juce::Random random { 1234 };
};
}
