#pragma once

#include "Biquad.h"

#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

// Space for the voice: a plate reverb, and the ducking the plate and the echo share.
namespace fx
{
//==============================================================================
// How much the singer is singing right now, 0..1, without caring how loud they sing:
// a fast envelope of the dry voice against a slow-falling memory of its recent peaks.
// Near the recent peaks it reads 1; 12 dB or more under them (the gaps between
// phrases) it reads 0. The reverb or echo is turned down by duck x presence, so the
// effect dips under the words and blooms in the gaps.
class Ducker
{
public:
    void prepare (double sampleRate) noexcept
    {
        fastAttack = pole (sampleRate, 0.005);
        fastRelease = pole (sampleRate, 0.06);
        memoryRelease = pole (sampleRate, 2.0);
        gainAttack = pole (sampleRate, 0.02);
        gainRelease = pole (sampleRate, 0.25);
        reset();
    }

    void reset() noexcept { fast = 0.0f; memory = 0.0f; gain = 1.0f; }

    // One sample of the dry level (peak over channels); returns the gain for the wet.
    float next (float dryPeak, float duck) noexcept
    {
        fast = dryPeak + (dryPeak > fast ? fastAttack : fastRelease) * (fast - dryPeak);
        memory = fast > memory ? fast : fast + memoryRelease * (memory - fast);

        // Quieter than -70 dBFS is not singing, whatever the memory says.
        const auto fastDb = 20.0f * std::log10 (std::max (fast, 1.0e-6f));
        const auto memoryDb = 20.0f * std::log10 (std::max (memory, 1.0e-6f));
        const auto presence = fastDb < -70.0f ? 0.0f : juce::jlimit (0.0f, 1.0f, (fastDb - (memoryDb - 12.0f)) / 9.0f);

        // Up to about -20 dB at 100 %.
        const auto target = 1.0f - 0.9f * juce::jlimit (0.0f, 1.0f, duck) * presence;
        gain = target + (target < gain ? gainAttack : gainRelease) * (gain - target);
        return gain;
    }

private:
    static float pole (double sampleRate, double seconds) noexcept { return (float) std::exp (-1.0 / (seconds * sampleRate)); }

    float fastAttack = 0.0f, fastRelease = 0.0f, memoryRelease = 0.0f, gainAttack = 0.0f, gainRelease = 0.0f;
    float fast = 0.0f, memory = 0.0f, gain = 1.0f;
};

//==============================================================================
// A plate reverb on the classic figure-eight tank topology (Dattorro, "Effect Design
// Part 1", JAES 1997): pre-delay, a band limit, four input diffusers, then two
// cross-coupled halves, each a modulated all-pass, a delay, damping, a second all-pass
// and a delay, read at seven taps per side for decorrelated stereo. Delays are scaled
// from the paper's 29.761 kHz to the running rate.
//
// Decay is the RT60 in seconds (the tank gain is worked out from the loop length);
// damping darkens the tail as it decays; low and high cut shape only the reverb; width
// is the wet signal's stereo spread; duck dips it while the voice is present.
//
// The input stops while `feeding` is false, but the tail keeps ringing: the module
// always runs so switching it off doesn't cut the tail.
class PlateReverb
{
public:
    struct Settings
    {
        float mix = 0.2f;          // 0..1
        float decaySeconds = 1.8f;
        float preDelayMs = 20.0f;
        float damping = 0.4f;      // 0..1
        float lowCutHz = 150.0f;
        float highCutHz = 9000.0f;
        float modulation = 0.3f;   // 0..1
        float width = 1.0f;        // 0..1
        float duck = 0.0f;         // 0..1
        bool feeding = true;
    };

    static constexpr double paperRate = 29761.0;
    static constexpr float maxPreDelayMs = 200.0f;

    void prepare (double newSampleRate)
    {
        sampleRate = newSampleRate;
        const auto scale = sampleRate / paperRate;
        const auto scaled = [scale] (int n) { return std::max (1, (int) std::lround (n * scale)); };

        preDelay.resize ((size_t) (maxPreDelayMs * 0.001 * sampleRate) + 2);

        constexpr int inputLengths[] = { 142, 107, 379, 277 };
        for (size_t i = 0; i < 4; ++i)
            inputDiffusers[i].resize (scaled (inputLengths[i]));

        excursion = (float) (16.0 * scale);
        for (size_t side = 0; side < 2; ++side)
        {
            auto& h = halves[side];
            h.modulated.resize (scaled (side == 0 ? 672 : 908) + (int) std::ceil (excursion) + 2);
            h.modulatedLength = (float) (side == 0 ? 672 : 908) * (float) scale;
            h.delay1.resize (scaled (side == 0 ? 4453 : 4217));
            h.allpass.resize (scaled (side == 0 ? 1800 : 2656));
            h.delay2.resize (scaled (side == 0 ? 3720 : 3163));
        }

        // Output taps (left from the paper; right mirrored on the other half).
        constexpr int left[7][2] = { { 266, 0 }, { 2974, 0 }, { 1913, 1 }, { 1996, 2 }, { 1990, 3 }, { 187, 4 }, { 1066, 5 } };
        constexpr int right[7][2] = { { 353, 3 }, { 3627, 3 }, { 1228, 4 }, { 2673, 5 }, { 2111, 0 }, { 335, 1 }, { 121, 2 } };
        for (size_t t = 0; t < 7; ++t)
        {
            leftTaps[t] = { scaled (left[t][0]), left[t][1] };
            rightTaps[t] = { scaled (right[t][0]), right[t][1] };
        }

        // The loop the signal goes round (both halves), for the decay gain.
        loopSeconds = 0.0;
        for (const auto& h : halves)
            loopSeconds += (h.modulatedLength + (double) h.delay1.size() + (double) h.allpass.size() + (double) h.delay2.size()) / sampleRate;

        ducker.prepare (sampleRate);
        lfoIncrement = (float) (juce::MathConstants<double>::twoPi * 0.9 / sampleRate);
        wetGain.reset (sampleRate, 0.05);
        last.decaySeconds = last.lowCutHz = last.highCutHz = -1.0f;
        setSettings (settings);
        reset();
    }

    void reset() noexcept
    {
        std::fill (preDelay.begin(), preDelay.end(), 0.0f);
        preDelayWrite = 0;
        for (auto& d : inputDiffusers) d.clear();
        for (auto& h : halves)
        {
            h.modulated.clear(); h.delay1.clear(); h.allpass.clear(); h.delay2.clear();
            h.damp = 0.0f;
        }
        bandwidthState = 0.0f;
        for (auto& f : lowCut) f.reset();
        for (auto& f : highCut) f.reset();
        ducker.reset();
        silentSamples = 0;
        wetGain.setCurrentAndTargetValue (settings.mix);
    }

    void setSettings (const Settings& s) noexcept
    {
        settings = s;

        if (s.decaySeconds != last.decaySeconds)
        {
            // The tank gain is applied four times per loop: 60 dB down after decay seconds.
            const auto seconds = std::max (0.1f, s.decaySeconds);
            decayGain = (float) std::pow (10.0, -3.0 * loopSeconds / (4.0 * seconds));
            decayGain = std::min (decayGain, 0.9995f);
        }

        if (s.lowCutHz != last.lowCutHz)
            for (auto& f : lowCut) f.setup (Biquad::Type::highPass, sampleRate, s.lowCutHz, 0.707);
        if (s.highCutHz != last.highCutHz)
            for (auto& f : highCut) f.setup (Biquad::Type::lowPass, sampleRate, s.highCutHz, 0.707);

        dampingCoefficient = 0.05f + 0.75f * juce::jlimit (0.0f, 1.0f, s.damping);
        preDelaySamples = juce::jlimit (0.0f, (float) preDelay.size() - 2.0f, s.preDelayMs * 0.001f * (float) sampleRate);
        wetGain.setTargetValue (s.mix);
        last = s;
    }

    // True once the tail has died away (below about -100 dB for a while) with no input:
    // the caller may then skip the plate.
    bool isSilent() const noexcept { return silentSamples > (int) sampleRate / 10; }

    void process (juce::dsp::AudioBlock<float>& block) noexcept
    {
        juce::ScopedNoDenormals noDenormals;
        const auto numChannels = std::min<size_t> (block.getNumChannels(), 2);
        const auto stereo = numChannels == 2;
        const auto feeding = settings.feeding;
        const auto width = juce::jlimit (0.0f, 1.0f, settings.width);
        const auto modDepth = excursion * juce::jlimit (0.0f, 1.0f, settings.modulation);
        constexpr float bandwidth = 0.9995f;
        constexpr float inputDiffusion1 = 0.75f, inputDiffusion2 = 0.625f, decayDiffusion1 = 0.7f, decayDiffusion2 = 0.5f;

        for (size_t i = 0; i < block.getNumSamples(); ++i)
        {
            const auto dryL = block.getSample (0, (int) i);
            const auto dryR = stereo ? block.getSample (1, (int) i) : dryL;
            const auto input = feeding ? 0.5f * (dryL + dryR) : 0.0f;

            // Pre-delay (fractional, so it can glide).
            preDelay[(size_t) preDelayWrite] = input;
            const auto readPosition = (float) preDelayWrite - preDelaySamples + (float) preDelay.size();
            const auto r0 = (int) readPosition;
            const auto fractionPart = readPosition - (float) r0;
            const auto size = (int) preDelay.size();
            const auto a = preDelay[(size_t) (r0 % size)], b = preDelay[(size_t) ((r0 + 1) % size)];
            const auto delayed = a + fractionPart * (b - a);
            preDelayWrite = (preDelayWrite + 1) % size;

            bandwidthState += bandwidth * (delayed - bandwidthState);
            auto x = bandwidthState;
            x = inputDiffusers[0].process (x, inputDiffusion1);
            x = inputDiffusers[1].process (x, inputDiffusion1);
            x = inputDiffusers[2].process (x, inputDiffusion2);
            x = inputDiffusers[3].process (x, inputDiffusion2);

            lfoPhase += lfoIncrement;
            if (lfoPhase > juce::MathConstants<float>::twoPi)
                lfoPhase -= juce::MathConstants<float>::twoPi;

            // The two halves, each fed by the other's end.
            const auto fromRight = halves[1].delay2.back(), fromLeft = halves[0].delay2.back();
            for (size_t side = 0; side < 2; ++side)
            {
                auto& h = halves[side];
                auto v = x + decayGain * (side == 0 ? fromRight : fromLeft);
                const auto lfo = std::sin (lfoPhase + (side == 0 ? 0.0f : juce::MathConstants<float>::halfPi));
                v = h.modulated.processModulated (v, -decayDiffusion1, h.modulatedLength + modDepth * lfo);
                h.delay1.push (v);
                v = h.delay1.back();
                h.damp += (1.0f - dampingCoefficient) * (v - h.damp);
                v = h.damp * decayGain;
                v = h.allpass.process (v, decayDiffusion2);
                h.delay2.push (v);
            }

            // Taps: buffer 0 delay1 L, 1 allpass L, 2 delay2 L, 3 delay1 R, 4 allpass R, 5 delay2 R.
            const auto tap = [this] (const Tap& t)
            {
                const auto& h = halves[t.buffer < 3 ? 0 : 1];
                switch (t.buffer % 3)
                {
                    case 0:  return h.delay1.at (t.delay);
                    case 1:  return h.allpass.at (t.delay);
                    default: return h.delay2.at (t.delay);
                }
            };
            const auto signs = std::array<float, 7> { 1.0f, 1.0f, -1.0f, 1.0f, -1.0f, -1.0f, -1.0f };
            float wetL = 0.0f, wetR = 0.0f;
            for (size_t t = 0; t < 7; ++t)
            {
                wetL += signs[t] * tap (leftTaps[t]);
                wetR += signs[t] * tap (rightTaps[t]);
            }
            wetL *= 0.6f;
            wetR *= 0.6f;

            // Width (mid/side), then the cuts on the reverb only.
            const auto mid = 0.5f * (wetL + wetR), side = 0.5f * (wetL - wetR) * width;
            wetL = lowCut[0].process (0, highCut[0].process (0, mid + side));
            wetR = lowCut[1].process (0, highCut[1].process (0, mid - side));

            const auto duckGain = ducker.next (std::max (std::abs (dryL), std::abs (dryR)), settings.duck);
            const auto mix = wetGain.getNextValue();

            // Equal-power crossfade, so the voice doesn't drop as the mix rises; while
            // switched off the dry is untouched and the tail rings out over it.
            const auto angle = mix * juce::MathConstants<float>::halfPi;
            const auto dryGain = feeding ? std::cos (angle) : 1.0f;
            const auto wet = std::sin (angle) * duckGain;

            if (stereo)
            {
                block.setSample (0, (int) i, dryL * dryGain + wetL * wet);
                block.setSample (1, (int) i, dryR * dryGain + wetR * wet);
            }
            else
            {
                block.setSample (0, (int) i, dryL * dryGain + 0.5f * (wetL + wetR) * wet);
            }

            const auto energy = wetL * wetL + wetR * wetR + input * input;
            silentSamples = energy < 1.0e-10f ? silentSamples + 1 : 0;
        }
    }

private:
    // A delay line with a single write position; at (n) reads n samples back.
    struct Delay
    {
        void resize (int length) { buffer.assign ((size_t) std::max (2, length), 0.0f); write = 0; }
        void clear() { std::fill (buffer.begin(), buffer.end(), 0.0f); write = 0; }
        size_t size() const noexcept { return buffer.size(); }

        void push (float x) noexcept
        {
            buffer[(size_t) write] = x;
            write = (write + 1) % (int) buffer.size();
        }

        // n in 1..size: n samples ago (the newest is 1).
        float at (int n) const noexcept
        {
            const auto length = (int) buffer.size();
            n = juce::jlimit (1, length, n);
            return buffer[(size_t) ((write - n + length) % length)];
        }

        float back() const noexcept { return buffer[(size_t) write]; } // the oldest sample

        // A Schroeder all-pass around the whole delay.
        float process (float x, float g) noexcept
        {
            const auto delayed = back();
            const auto v = x + g * delayed;
            push (v);
            return delayed - g * v;
        }

        // The same with a fractional, moving length (linear interpolation).
        float processModulated (float x, float g, float length) noexcept
        {
            const auto maxLength = (float) buffer.size() - 1.0f;
            length = juce::jlimit (1.0f, maxLength, length);
            const auto whole = (int) length;
            const auto f = length - (float) whole;
            const auto delayed = at (whole) + f * (at (whole + 1) - at (whole));
            const auto v = x + g * delayed;
            push (v);
            return delayed - g * v;
        }

        std::vector<float> buffer;
        int write = 0;
    };

    struct Half
    {
        Delay modulated, delay1, allpass, delay2;
        float modulatedLength = 0.0f;
        float damp = 0.0f;
    };

    struct Tap
    {
        int delay = 1;
        int buffer = 0;
    };

    double sampleRate = 48000.0, loopSeconds = 1.0;
    Settings settings, last;
    std::vector<float> preDelay;
    int preDelayWrite = 0;
    float preDelaySamples = 0.0f;
    std::array<Delay, 4> inputDiffusers;
    std::array<Half, 2> halves;
    std::array<Tap, 7> leftTaps, rightTaps;
    std::array<Biquad, 2> lowCut, highCut;
    Ducker ducker;
    juce::SmoothedValue<float> wetGain;
    float bandwidthState = 0.0f, decayGain = 0.5f, dampingCoefficient = 0.3f, excursion = 16.0f;
    float lfoPhase = 0.0f, lfoIncrement = 0.0f;
    int silentSamples = 0;
};
}
