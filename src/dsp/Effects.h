#pragma once

#include <juce_dsp/juce_dsp.h>

#include "Biquad.h"
#include "Space.h"

#include <array>
#include <cmath>

// Small effects that aren't in juce::dsp. Each one follows the juce::dsp processor
// shape (prepare / reset / process) so the chain can treat them all alike. Blocks have
// at most two channels (see PluginProcessor::isBusesLayoutSupported).
namespace fx
{
using Context = juce::dsp::ProcessContextReplacing<float>;

namespace detail
{
    inline constexpr float twoPi = juce::MathConstants<float>::twoPi;

    // One-pole smoothing coefficient for a time constant in seconds.
    inline float timeCoefficient (double sampleRate, double seconds)
    {
        return (float) std::exp (-1.0 / (seconds * sampleRate));
    }

    inline float advancePhase (float& phase, float increment) noexcept
    {
        phase += increment;
        if (phase >= 1.0f)
            phase -= 1.0f;
        return phase;
    }
}

//==============================================================================
// Low shelf (150 Hz), sweepable mid peak and high shelf (7 kHz).
class Equalizer
{
public:
    void prepare (const juce::dsp::ProcessSpec& spec) { sampleRate = spec.sampleRate; reset(); }
    void reset() { low.reset(); mid.reset(); high.reset(); }

    void setParameters (float lowDb, float midHz, float midDb, float highDb) noexcept
    {
        low.setup  (Biquad::Type::lowShelf,  sampleRate, 150.0,  0.707, lowDb);
        mid.setup  (Biquad::Type::peak,      sampleRate, midHz,  1.0,   midDb);
        high.setup (Biquad::Type::highShelf, sampleRate, 7000.0, 0.707, highDb);
    }

    void process (const Context& context) noexcept
    {
        auto& block = context.getOutputBlock();

        for (size_t ch = 0; ch < block.getNumChannels(); ++ch)
        {
            auto* data = block.getChannelPointer (ch);
            for (size_t i = 0; i < block.getNumSamples(); ++i)
                data[i] = high.process (ch, mid.process (ch, low.process (ch, data[i])));
        }
    }

private:
    double sampleRate = 44100.0;
    Biquad low, mid, high;
};

//==============================================================================
// Split-band de-esser: when the sibilance band exceeds the threshold, only that
// band is turned down (4:1), leaving the rest of the voice untouched.
class DeEsser
{
public:
    void prepare (const juce::dsp::ProcessSpec& spec)
    {
        sampleRate = spec.sampleRate;
        attack  = detail::timeCoefficient (sampleRate, 0.001);
        release = detail::timeCoefficient (sampleRate, 0.06);
        reset();
    }

    void reset() { split.reset(); envelope = 0.0f; }

    void setParameters (float newThresholdDb, float frequency) noexcept
    {
        thresholdDb = newThresholdDb;
        split.setup (Biquad::Type::highPass, sampleRate, frequency, 0.707);
    }

    void process (const Context& context) noexcept
    {
        auto& block = context.getOutputBlock();
        const auto numChannels = block.getNumChannels();
        std::array<float, 2> highs {};

        for (size_t i = 0; i < block.getNumSamples(); ++i)
        {
            auto level = 0.0f;

            for (size_t ch = 0; ch < numChannels; ++ch)
            {
                highs[ch] = split.process (ch, block.getSample ((int) ch, (int) i));
                level = juce::jmax (level, std::abs (highs[ch]));
            }

            const auto coefficient = level > envelope ? attack : release;
            envelope = coefficient * envelope + (1.0f - coefficient) * level;

            const auto overDb = juce::Decibels::gainToDecibels (envelope, -100.0f) - thresholdDb;
            const auto gain = overDb > 0.0f ? juce::Decibels::decibelsToGain (-0.75f * overDb) : 1.0f;

            for (size_t ch = 0; ch < numChannels; ++ch)
                block.addSample ((int) ch, (int) i, -highs[ch] * (1.0f - gain));
        }
    }

private:
    double sampleRate = 44100.0;
    Biquad split;
    float attack = 0.0f, release = 0.0f, envelope = 0.0f, thresholdDb = -30.0f;
};

//==============================================================================
// Adds saturated harmonics of the top end for "air" and presence.
class Exciter
{
public:
    void prepare (const juce::dsp::ProcessSpec& spec) { sampleRate = spec.sampleRate; reset(); }
    void reset() { highPass.reset(); }

    void setParameters (float frequency, float newAmount) noexcept
    {
        highPass.setup (Biquad::Type::highPass, sampleRate, frequency, 0.707);
        amount = newAmount;
    }

    void process (const Context& context) noexcept
    {
        auto& block = context.getOutputBlock();

        for (size_t ch = 0; ch < block.getNumChannels(); ++ch)
        {
            auto* data = block.getChannelPointer (ch);
            for (size_t i = 0; i < block.getNumSamples(); ++i)
                data[i] += amount * 0.6f * std::tanh (2.5f * highPass.process (ch, data[i]));
        }
    }

private:
    double sampleRate = 44100.0;
    Biquad highPass;
    float amount = 0.0f;
};

//==============================================================================
// tanh drive with loudness compensation and a gentle top-end roll-off.
class Saturation
{
public:
    void prepare (const juce::dsp::ProcessSpec& spec)
    {
        toneCoefficient = 1.0f - detail::timeCoefficient (spec.sampleRate, 1.0 / (juce::MathConstants<double>::twoPi * 7500.0));
        reset();
    }

    void reset() { tone = {}; }

    void setParameters (float driveDb, float newMix) noexcept
    {
        drive  = juce::Decibels::decibelsToGain (driveDb);
        makeup = 1.0f / std::pow (drive, 0.7f);
        mix    = newMix;
    }

    void process (const Context& context) noexcept
    {
        auto& block = context.getOutputBlock();

        for (size_t ch = 0; ch < block.getNumChannels(); ++ch)
        {
            auto* data = block.getChannelPointer (ch);

            for (size_t i = 0; i < block.getNumSamples(); ++i)
            {
                const auto wet = std::tanh (drive * data[i]) * makeup;
                tone[ch] += toneCoefficient * (wet - tone[ch]);
                data[i] += mix * (tone[ch] - data[i]);
            }
        }
    }

private:
    std::array<float, 2> tone {};
    float toneCoefficient = 0.5f, drive = 1.0f, makeup = 1.0f, mix = 1.0f;
};

//==============================================================================
// Steep band-pass with a nasal mid bump: telephone, radio, walkie-talkie.
class Radio
{
public:
    void prepare (const juce::dsp::ProcessSpec& spec) { sampleRate = spec.sampleRate; reset(); }
    void reset() { for (auto* f : { &hp1, &hp2, &lp1, &lp2, &honk }) f->reset(); }

    void setParameters (float lowHz, float highHz) noexcept
    {
        highHz = juce::jmax (highHz, lowHz * 1.5f);
        hp1.setup (Biquad::Type::highPass, sampleRate, lowHz, 0.54);
        hp2.setup (Biquad::Type::highPass, sampleRate, lowHz, 1.31);
        lp1.setup (Biquad::Type::lowPass,  sampleRate, highHz, 0.54);
        lp2.setup (Biquad::Type::lowPass,  sampleRate, highHz, 1.31);
        honk.setup (Biquad::Type::peak,    sampleRate, std::sqrt (lowHz * highHz), 1.2, 5.0);
    }

    void process (const Context& context) noexcept
    {
        auto& block = context.getOutputBlock();

        for (size_t ch = 0; ch < block.getNumChannels(); ++ch)
        {
            auto* data = block.getChannelPointer (ch);
            for (size_t i = 0; i < block.getNumSamples(); ++i)
                data[i] = honk.process (ch, lp2.process (ch, lp1.process (ch, hp2.process (ch, hp1.process (ch, data[i])))));
        }
    }

private:
    double sampleRate = 44100.0;
    Biquad hp1, hp2, lp1, lp2, honk;
};

//==============================================================================
// Bit-depth reduction plus sample-and-hold downsampling.
class Bitcrusher
{
public:
    void prepare (const juce::dsp::ProcessSpec& spec) { sampleRate = spec.sampleRate; reset(); }
    void reset() { held = {}; phase = 1.0f; }

    void setParameters (float bits, float rateHz, float newMix) noexcept
    {
        levels = std::exp2 (bits - 1.0f);
        step   = juce::jmin (1.0f, rateHz / (float) sampleRate);
        mix    = newMix;
    }

    void process (const Context& context) noexcept
    {
        auto& block = context.getOutputBlock();

        for (size_t i = 0; i < block.getNumSamples(); ++i)
        {
            phase += step;
            const auto takeSample = phase >= 1.0f;
            if (takeSample)
                phase -= 1.0f;

            for (size_t ch = 0; ch < block.getNumChannels(); ++ch)
            {
                const auto x = block.getSample ((int) ch, (int) i);
                if (takeSample)
                    held[ch] = std::round (x * levels) / levels;

                block.setSample ((int) ch, (int) i, x + mix * (held[ch] - x));
            }
        }
    }

private:
    double sampleRate = 44100.0;
    std::array<float, 2> held {};
    float phase = 1.0f, step = 1.0f, levels = 128.0f, mix = 1.0f;
};

//==============================================================================
// Ring modulator: multiplies the voice by a sine wave for classic robot voices.
class RingModulator
{
public:
    void prepare (const juce::dsp::ProcessSpec& spec) { sampleRate = spec.sampleRate; reset(); }
    void reset() { phase = 0.0f; }

    void setParameters (float frequency, float newMix) noexcept
    {
        increment = frequency / (float) sampleRate;
        mix = newMix;
    }

    void process (const Context& context) noexcept
    {
        auto& block = context.getOutputBlock();

        for (size_t i = 0; i < block.getNumSamples(); ++i)
        {
            const auto carrier = std::sin (detail::twoPi * detail::advancePhase (phase, increment));

            for (size_t ch = 0; ch < block.getNumChannels(); ++ch)
            {
                const auto x = block.getSample ((int) ch, (int) i);
                block.setSample ((int) ch, (int) i, x + mix * (x * carrier - x));
            }
        }
    }

private:
    double sampleRate = 44100.0;
    float phase = 0.0f, increment = 0.0f, mix = 1.0f;
};

//==============================================================================
// Resonant 24 dB/oct Moog-style ladder filter.
class ResonantFilter
{
public:
    void prepare (const juce::dsp::ProcessSpec& spec) { ladder.prepare (spec); }
    void reset() { ladder.reset(); }

    void setParameters (int type, float cutoffHz, float resonance) noexcept
    {
        using Mode = juce::dsp::LadderFilterMode;
        ladder.setMode (type == 1 ? Mode::HPF24 : (type == 2 ? Mode::BPF24 : Mode::LPF24));
        ladder.setCutoffFrequencyHz (cutoffHz);
        ladder.setResonance (juce::jlimit (0.0f, 0.95f, resonance));
    }

    void process (const Context& context) noexcept { ladder.process (context); }

private:
    juce::dsp::LadderFilter<float> ladder;
};

//==============================================================================
// Envelope-following band-pass: the louder you speak, the higher the filter opens.
class AutoWah
{
public:
    void prepare (const juce::dsp::ProcessSpec& spec)
    {
        sampleRate = spec.sampleRate;
        filter.prepare (spec);
        filter.setType (juce::dsp::StateVariableTPTFilterType::bandpass);
        filter.setResonance (resonance);
        attack  = detail::timeCoefficient (sampleRate, 0.004);
        release = detail::timeCoefficient (sampleRate, 0.12);
        reset();
    }

    void reset() { filter.reset(); envelope = 0.0f; }

    void setParameters (float newSensitivity, float newBaseHz, float newMix) noexcept
    {
        sensitivity = newSensitivity;
        baseHz = newBaseHz;
        mix = newMix;
    }

    void process (const Context& context) noexcept
    {
        auto& block = context.getOutputBlock();
        const auto maxCutoff = (float) sampleRate * 0.45f;

        for (size_t i = 0; i < block.getNumSamples(); ++i)
        {
            auto level = 0.0f;
            for (size_t ch = 0; ch < block.getNumChannels(); ++ch)
                level = juce::jmax (level, std::abs (block.getSample ((int) ch, (int) i)));

            const auto coefficient = level > envelope ? attack : release;
            envelope = coefficient * envelope + (1.0f - coefficient) * level;

            if (i % 16 == 0)
            {
                const auto octaves = juce::jmin (4.0f, envelope * sensitivity * 20.0f);
                filter.setCutoffFrequency (juce::jmin (maxCutoff, baseHz * std::exp2 (octaves)));
            }

            for (size_t ch = 0; ch < block.getNumChannels(); ++ch)
            {
                const auto x = block.getSample ((int) ch, (int) i);
                // The band-pass peaks at `resonance`; scale it back to a little above unity.
                const auto wet = filter.processSample ((int) ch, x) * (1.4f / resonance);
                block.setSample ((int) ch, (int) i, x + mix * (wet - x));
            }
        }
    }

private:
    static constexpr float resonance = 4.0f;

    juce::dsp::StateVariableTPTFilter<float> filter;
    double sampleRate = 44100.0;
    float attack = 0.0f, release = 0.0f, envelope = 0.0f;
    float sensitivity = 0.5f, baseHz = 400.0f, mix = 1.0f;
};

//==============================================================================
class Tremolo
{
public:
    void prepare (const juce::dsp::ProcessSpec& spec) { sampleRate = spec.sampleRate; reset(); }
    void reset() { phase = 0.0f; }

    void setParameters (float rateHz, float newDepth) noexcept
    {
        increment = rateHz / (float) sampleRate;
        depth = newDepth;
    }

    void process (const Context& context) noexcept
    {
        auto& block = context.getOutputBlock();

        for (size_t i = 0; i < block.getNumSamples(); ++i)
        {
            const auto lfo = 0.5f - 0.5f * std::cos (detail::twoPi * detail::advancePhase (phase, increment));
            const auto gain = 1.0f - depth * lfo;

            for (size_t ch = 0; ch < block.getNumChannels(); ++ch)
                block.setSample ((int) ch, (int) i, block.getSample ((int) ch, (int) i) * gain);
        }
    }

private:
    double sampleRate = 44100.0;
    float phase = 0.0f, increment = 0.0f, depth = 0.5f;
};

//==============================================================================
// Pitch wobble from a sine-modulated delay line.
class Vibrato
{
public:
    void prepare (const juce::dsp::ProcessSpec& spec)
    {
        sampleRate = spec.sampleRate;
        line.setMaximumDelayInSamples ((int) std::ceil (sampleRate * 0.01));
        line.prepare (spec);
        reset();
    }

    void reset() { line.reset(); phase = 0.0f; }

    void setParameters (float rateHz, float depth) noexcept
    {
        increment = rateHz / (float) sampleRate;
        depthSamples = depth * maxDepthMs * 0.001f * (float) sampleRate;
    }

    void process (const Context& context) noexcept
    {
        auto& block = context.getOutputBlock();
        const auto centre = (maxDepthMs + 0.5f) * 0.001f * (float) sampleRate;

        for (size_t i = 0; i < block.getNumSamples(); ++i)
        {
            const auto delay = centre + depthSamples * std::sin (detail::twoPi * detail::advancePhase (phase, increment));

            for (size_t ch = 0; ch < block.getNumChannels(); ++ch)
            {
                line.pushSample ((int) ch, block.getSample ((int) ch, (int) i));
                block.setSample ((int) ch, (int) i, line.popSample ((int) ch, delay));
            }
        }
    }

private:
    static constexpr float maxDepthMs = 1.5f;

    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Lagrange3rd> line;
    double sampleRate = 44100.0;
    float phase = 0.0f, increment = 0.0f, depthSamples = 0.0f;
};

//==============================================================================
// Two slightly delayed, slowly drifting copies of the voice spread left and right,
// like a singer double-tracking a vocal.
class Doubler
{
public:
    void prepare (const juce::dsp::ProcessSpec& spec)
    {
        sampleRate = spec.sampleRate;
        line.setMaximumDelayInSamples ((int) std::ceil (sampleRate * 0.05));
        line.prepare ({ spec.sampleRate, spec.maximumBlockSize, 1 });
        reset();
    }

    void reset() { line.reset(); phaseA = 0.0f; phaseB = 0.37f; }

    void setParameters (float newMix, float spread) noexcept
    {
        mix = newMix;
        pan = 0.5f + 0.5f * spread;
    }

    void process (const Context& context) noexcept
    {
        auto& block = context.getOutputBlock();
        const auto numChannels = block.getNumChannels();
        const auto msToSamples = 0.001f * (float) sampleRate;

        for (size_t i = 0; i < block.getNumSamples(); ++i)
        {
            auto mid = 0.0f;
            for (size_t ch = 0; ch < numChannels; ++ch)
                mid += block.getSample ((int) ch, (int) i);
            mid /= (float) numChannels;

            const auto delayA = (18.0f + 1.3f * std::sin (detail::twoPi * detail::advancePhase (phaseA, 0.31f / (float) sampleRate))) * msToSamples;
            const auto delayB = (27.0f + 1.3f * std::sin (detail::twoPi * detail::advancePhase (phaseB, 0.43f / (float) sampleRate))) * msToSamples;

            line.pushSample (0, mid);
            const auto a = line.popSample (0, delayA, false);
            const auto b = line.popSample (0, delayB, true);

            if (numChannels == 2)
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
    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Linear> line;
    double sampleRate = 44100.0;
    float phaseA = 0.0f, phaseB = 0.37f, mix = 0.5f, pan = 0.9f;
};

//==============================================================================
// Feedback delay with darkening repeats and an optional ping-pong mode.
//
// The repeats darken through a one-pole low-pass at the high cut (5 kHz by default, as
// it always was). Moving either cut off its default also filters the repeats you hear
// (a low cut from 20 Hz up, a high cut away from 5 kHz); duck dips them while you sing.
// At the defaults the output is exactly the original echo's.
class Echo
{
public:
    static constexpr float defaultHighCutHz = 5000.0f, defaultLowCutHz = 20.0f;

    void prepare (const juce::dsp::ProcessSpec& spec)
    {
        sampleRate = spec.sampleRate;
        line.setMaximumDelayInSamples ((int) std::ceil (sampleRate * 1.6));
        line.prepare (spec);
        delaySamples.reset (sampleRate, 0.15);
        lastHighCut = lastLowCut = -1.0f;
        setTone (lowCutHz, highCutHz);
        ducker.prepare (sampleRate);
        reset();
    }

    void reset()
    {
        line.reset();
        feedbackState = {};
        for (auto& f : toneLow) f.reset();
        for (auto& f : toneHigh) f.reset();
        ducker.reset();
        delaySamples.setCurrentAndTargetValue (delaySamples.getTargetValue());
    }

    void setParameters (float timeMs, float newFeedback, float newMix, bool newPingPong) noexcept
    {
        delaySamples.setTargetValue (timeMs * 0.001f * (float) sampleRate);
        feedback = newFeedback;
        mix = newMix;
        pingPong = newPingPong;
    }

    void setTone (float newLowCutHz, float newHighCutHz) noexcept
    {
        lowCutHz = newLowCutHz;
        highCutHz = newHighCutHz;
        filtered = lowCutHz > defaultLowCutHz + 0.5f || std::abs (highCutHz - defaultHighCutHz) > 0.5f;

        if (highCutHz != lastHighCut)
        {
            lastHighCut = highCutHz;
            damping = 1.0f - detail::timeCoefficient (sampleRate, 1.0 / (juce::MathConstants<double>::twoPi * (double) highCutHz));
            for (auto& f : toneHigh) f.setup (Biquad::Type::lowPass, sampleRate, highCutHz, 0.707);
        }

        if (lowCutHz != lastLowCut)
        {
            lastLowCut = lowCutHz;
            for (auto& f : toneLow) f.setup (Biquad::Type::highPass, sampleRate, lowCutHz, 0.707);
        }
    }

    void setDuck (float newDuck) noexcept { duck = newDuck; }

    void process (const Context& context) noexcept
    {
        auto& block = context.getOutputBlock();
        const auto numChannels = block.getNumChannels();
        const auto stereoPingPong = pingPong && numChannels == 2;

        for (size_t i = 0; i < block.getNumSamples(); ++i)
        {
            const auto delay = delaySamples.getNextValue();
            std::array<float, 2> dry {}, echoes {};

            for (size_t ch = 0; ch < numChannels; ++ch)
            {
                dry[ch] = block.getSample ((int) ch, (int) i);
                echoes[ch] = line.popSample ((int) ch, delay);
                feedbackState[ch] += damping * (echoes[ch] - feedbackState[ch]);
            }

            if (stereoPingPong)
            {
                line.pushSample (0, 0.5f * (dry[0] + dry[1]) + feedback * feedbackState[1]);
                line.pushSample (1, feedback * feedbackState[0]);
            }
            else
            {
                for (size_t ch = 0; ch < numChannels; ++ch)
                    line.pushSample ((int) ch, dry[ch] + feedback * feedbackState[ch]);
            }

            if (filtered)
                for (size_t ch = 0; ch < numChannels; ++ch)
                    echoes[ch] = toneLow[ch].process (0, toneHigh[ch].process (0, echoes[ch]));

            auto wet = mix;
            if (duck > 0.0f)
                wet *= ducker.next (std::max (std::abs (dry[0]), std::abs (dry[numChannels > 1 ? 1 : 0])), duck);

            for (size_t ch = 0; ch < numChannels; ++ch)
                block.setSample ((int) ch, (int) i, dry[ch] + wet * echoes[ch]);
        }
    }

private:
    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Linear> line;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> delaySamples;
    std::array<float, 2> feedbackState {};
    std::array<Biquad, 2> toneLow, toneHigh;
    Ducker ducker;
    double sampleRate = 44100.0;
    float damping = 0.5f, feedback = 0.35f, mix = 0.25f, duck = 0.0f;
    float lowCutHz = defaultLowCutHz, highCutHz = defaultHighCutHz, lastLowCut = -1.0f, lastHighCut = -1.0f;
    bool pingPong = false, filtered = false;
};

//==============================================================================
// Mid/side width: 0% folds to mono, 100% leaves it alone, 200% doubles the sides.
class StereoWidth
{
public:
    void prepare (const juce::dsp::ProcessSpec&) {}
    void reset() {}

    void setParameters (float newWidth) noexcept { width = newWidth; }

    void process (const Context& context) noexcept
    {
        auto& block = context.getOutputBlock();
        if (block.getNumChannels() != 2)
            return;

        auto* left  = block.getChannelPointer (0);
        auto* right = block.getChannelPointer (1);

        for (size_t i = 0; i < block.getNumSamples(); ++i)
        {
            const auto mid  = 0.5f * (left[i] + right[i]);
            const auto side = 0.5f * (left[i] - right[i]) * width;
            left[i]  = mid + side;
            right[i] = mid - side;
        }
    }

private:
    float width = 1.0f;
};
} // namespace fx
