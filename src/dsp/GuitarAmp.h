#pragma once

#include <juce_dsp/juce_dsp.h>

#include "Biquad.h"

#include <array>
#include <cmath>
#include <complex>
#include <vector>

namespace fx
{
//==============================================================================
// A guitar (or bass) amplifier as plain DSP, so the amp module works with no file:
// a preamp of one to three gain stages that clip softly and a little unevenly (as
// valves do), with the coupling filters between them; the passive bass / mid / treble
// tone stack of classic amps (David Yeh and Julius Smith's analysis of the three-knob
// circuit, made digital with the bilinear transform); then a power stage with presence.
// Everything that clips runs 4x oversampled so high gain doesn't alias.
//
// Our own voicings (four models), not copies of particular amps. The level is kept
// roughly steady as Gain and Master move: each stage's growth for a typical guitar
// level is worked out and divided back out, so the knobs change the tone and grit
// more than the loudness.
class GuitarAmp
{
public:
    enum class Model { clean, crunch, lead, bass };
    static constexpr int numModels = 4;

    static juce::StringArray getModelNames() { return { "Clean", "Crunch", "Lead", "Bass" }; }

    struct Settings
    {
        Model model = Model::crunch;
        float gain = 5.0f;      // 0..10
        float bass = 5.0f;      // 0..10
        float mid = 5.0f;       // 0..10
        float treble = 5.0f;    // 0..10
        float presence = 5.0f;  // 0..10
        float master = 3.0f;    // 0..10, how hard the power stage is pushed
        bool operator== (const Settings&) const = default;
    };

    void prepare (const juce::dsp::ProcessSpec& spec)
    {
        baseRate = spec.sampleRate;
        numChannels = juce::jmin ((int) spec.numChannels, 2);
        oversampling = std::make_unique<juce::dsp::Oversampling<float>> ((size_t) juce::jmax (1, numChannels), oversamplingOrder,
                                                                        juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true, true);
        oversampling->initProcessing (spec.maximumBlockSize);
        designed = false;
        design();
        reset();
    }

    void reset()
    {
        if (oversampling != nullptr)
            oversampling->reset();
        for (auto& c : channels)
            c = {};
        inputHighPass.reset();
        bright.reset();
        presenceShelf.reset();
        toneStack.reset();
        lowSplit.reset();
    }

    // Delay of the oversampling filters, in samples at the session rate.
    int getLatencySamples() const noexcept
    {
        return oversampling != nullptr ? juce::roundToInt (oversampling->getLatencyInSamples()) : 0;
    }

    // Audio thread: recomputes the filters only when something changed.
    void setSettings (const Settings& s) noexcept
    {
        if (designed && s == settings)
            return;

        settings = s;
        design();
    }

    void process (const juce::dsp::ProcessContextReplacing<float>& context) noexcept
    {
        if (oversampling == nullptr)
            return;

        auto& block = context.getOutputBlock();
        auto up = oversampling->processSamplesUp (block);
        const auto n = up.getNumSamples();
        const auto chans = juce::jmin ((size_t) numChannels, up.getNumChannels());

        for (size_t ch = 0; ch < chans; ++ch)
        {
            auto* data = up.getChannelPointer (ch);
            auto& state = channels[ch];

            for (size_t i = 0; i < n; ++i)
            {
                auto x = inputHighPass.process (ch, data[i]);
                x = bright.process (ch, x);

                // Bass: the lows go round the preamp, so it grits without losing its bottom.
                auto low = 0.0f;
                if (cleanLows)
                {
                    low = lowSplit.process (ch, x);
                    x -= low;
                }

                for (int s = 0; s < numStages; ++s)
                {
                    const auto& stage = stages[(size_t) s];
                    auto y = clip (x * stage.gain + stage.bias) - clip (stage.bias);
                    // Coupling capacitor (one-pole high-pass) and the stage's own top end.
                    const auto hp = y - state.coupling[(size_t) s];
                    state.coupling[(size_t) s] += stage.couplingCoeff * hp;
                    state.smooth[(size_t) s] += stage.lowPassCoeff * (hp - state.smooth[(size_t) s]);
                    x = state.smooth[(size_t) s];
                }

                x = x * preampMakeUp + low * lowLevel;
                x = (float) toneStack.process (ch, (double) x) * toneMakeUp;
                x = presenceShelf.process (ch, x);
                x = powerSoftClip (x * powerDrive) * powerMakeUp;
                data[i] = x;
            }
        }

        oversampling->processSamplesDown (block);
    }

    // The tone stack's response (dB, relative to its knobs-at-noon level) for these
    // settings, for the card's picture. Any thread.
    static std::vector<float> getToneStackResponseDb (const Settings& s, const std::vector<double>& frequencies)
    {
        constexpr double rate = 192000.0;
        const auto v = getVoicing (s.model);
        ToneStack stack;
        stack.design (v.stack, 0.5, 0.5, bassTaper (0.5), rate);
        const auto makeUp = getAverageGain (stack, rate);
        stack.design (v.stack, knob (s.treble), knob (s.mid), bassTaper (knob (s.bass)), rate);

        std::vector<float> result;
        for (auto f : frequencies)
            result.push_back ((float) juce::Decibels::gainToDecibels (stack.getMagnitude (f, rate) / makeUp, -100.0));
        return result;
    }

private:
    static constexpr int oversamplingOrder = 2; // 4x
    static constexpr int maxStages = 3;
    static constexpr float nominalPeak = 0.35f; // a guitar played at about -18 dBFS RMS

    // Soft, slightly uneven clipping: harder on one side, like a valve stage.
    static float clip (float x) noexcept { return x >= 0.0f ? std::tanh (x) : std::tanh (1.25f * x) * 0.8f; }
    static float powerSoftClip (float x) noexcept { return std::tanh (x); }

    //==============================================================================
    // Third-order passive tone stack (Yeh & Smith, DAFx 2006): H(s) = B(s) / A(s), with
    // the three knobs in the coefficients, mapped to z by the bilinear transform.
    class ToneStack
    {
    public:
        struct Parts { double r1, r2, r3, r4, c1, c2, c3; };

        void design (const Parts& p, double t, double m, double l, double rate) noexcept
        {
            const auto [R1, R2, R3, R4, C1, C2, C3] = p;
            const auto b1 = t * C1 * R1 + m * C3 * R3 + l * (C1 * R2 + C2 * R2) + (C1 * R3 + C2 * R3);
            const auto b2 = t * (C1 * C2 * R1 * R4 + C1 * C3 * R1 * R4) - m * m * (C1 * C3 * R3 * R3 + C2 * C3 * R3 * R3)
                          + m * (C1 * C3 * R1 * R3 + C1 * C3 * R3 * R3 + C2 * C3 * R3 * R3)
                          + l * (C1 * C2 * R1 * R2 + C1 * C2 * R2 * R4 + C1 * C3 * R2 * R4)
                          + l * m * (C1 * C3 * R2 * R3 + C2 * C3 * R2 * R3) + (C1 * C2 * R1 * R3 + C1 * C2 * R3 * R4 + C1 * C3 * R3 * R4);
            const auto b3 = l * m * (C1 * C2 * C3 * R1 * R2 * R3 + C1 * C2 * C3 * R2 * R3 * R4)
                          - m * m * (C1 * C2 * C3 * R1 * R3 * R3 + C1 * C2 * C3 * R3 * R3 * R4)
                          + m * (C1 * C2 * C3 * R1 * R3 * R3 + C1 * C2 * C3 * R3 * R3 * R4)
                          + t * C1 * C2 * C3 * R1 * R3 * R4 - t * m * C1 * C2 * C3 * R1 * R3 * R4 + t * l * C1 * C2 * C3 * R1 * R2 * R4;
            const auto a1 = (C1 * R1 + C1 * R3 + C2 * R3 + C2 * R4 + C3 * R4) + m * C3 * R3 + l * (C1 * R2 + C2 * R2);
            const auto a2 = m * (C1 * C3 * R1 * R3 - C2 * C3 * R3 * R4 + C1 * C3 * R3 * R3 + C2 * C3 * R3 * R3)
                          + l * m * (C1 * C3 * R2 * R3 + C2 * C3 * R2 * R3) - m * m * (C1 * C3 * R3 * R3 + C2 * C3 * R3 * R3)
                          + l * (C1 * C2 * R2 * R4 + C1 * C2 * R1 * R2 + C1 * C3 * R2 * R4 + C2 * C3 * R2 * R4)
                          + (C1 * C2 * R1 * R4 + C1 * C3 * R1 * R4 + C1 * C2 * R3 * R4 + C1 * C2 * R1 * R3 + C1 * C3 * R3 * R4 + C2 * C3 * R3 * R4);
            const auto a3 = l * m * (C1 * C2 * C3 * R1 * R2 * R3 + C1 * C2 * C3 * R2 * R3 * R4)
                          - m * m * (C1 * C2 * C3 * R1 * R3 * R3 + C1 * C2 * C3 * R3 * R3 * R4)
                          + m * (C1 * C2 * C3 * R3 * R3 * R4 + C1 * C2 * C3 * R1 * R3 * R3 - C1 * C2 * C3 * R1 * R3 * R4)
                          + l * C1 * C2 * C3 * R1 * R2 * R4 + C1 * C2 * C3 * R1 * R3 * R4;

            // s = c (1 - z^-1) / (1 + z^-1); each s^k term spreads over z^0..z^-3.
            const auto c = 2.0 * rate;
            const std::array<double, 4> s { 1.0, c, c * c, c * c * c };
            static constexpr double spread[4][4] = { { 1, 3, 3, 1 }, { 1, 1, -1, -1 }, { 1, -1, -1, 1 }, { 1, -3, 3, -1 } };
            const std::array<double, 4> num { 0.0, b1, b2, b3 }, den { 1.0, a1, a2, a3 };

            std::array<double, 4> bz {}, az {};
            for (int k = 0; k < 4; ++k)
                for (int j = 0; j < 4; ++j)
                {
                    bz[(size_t) j] += num[(size_t) k] * s[(size_t) k] * spread[k][j];
                    az[(size_t) j] += den[(size_t) k] * s[(size_t) k] * spread[k][j];
                }

            for (int j = 0; j < 4; ++j)
            {
                b[(size_t) j] = bz[(size_t) j] / az[0];
                a[(size_t) j] = az[(size_t) j] / az[0];
            }
        }

        double process (size_t channel, double x) noexcept
        {
            auto& z = state[channel];
            const auto y = b[0] * x + z[0];
            z[0] = b[1] * x - a[1] * y + z[1];
            z[1] = b[2] * x - a[2] * y + z[2];
            z[2] = b[3] * x - a[3] * y;
            return y;
        }

        void reset() noexcept { state = {}; }

        double getMagnitude (double frequency, double rate) const noexcept
        {
            const auto w = juce::MathConstants<double>::twoPi * frequency / rate;
            std::complex<double> nz, dz;
            for (int k = 0; k < 4; ++k)
            {
                const auto e = std::polar (1.0, -w * k);
                nz += b[(size_t) k] * e;
                dz += a[(size_t) k] * e;
            }
            return std::abs (nz) / juce::jmax (1.0e-30, std::abs (dz));
        }

    private:
        std::array<double, 4> b {}, a { 1.0, 0.0, 0.0, 0.0 };
        std::array<std::array<double, 3>, 2> state {};
    };

    struct Voicing
    {
        int stages;
        float minGainDb, maxGainDb;  // whole preamp, Gain at 0 and 10
        float bias;                  // unevenness of the clipping
        float inputCutHz;            // tightens the lows before the gain
        float couplingHz;            // between stages
        float stageTopHz;            // each stage's own roll-off
        float brightDb;              // treble lift before the gain, at low Gain
        ToneStack::Parts stack;
        bool cleanLows;
        float levelDb;               // evens out the models' loudness
    };

    static Voicing getVoicing (Model model) noexcept
    {
        // Tone stack parts: R1 treble pot, R2 bass pot, R3 mid pot, R4 slope resistor.
        constexpr ToneStack::Parts american { 250e3, 1e6, 25e3, 56e3, 250e-12, 20e-9, 20e-9 };
        constexpr ToneStack::Parts british { 220e3, 1e6, 22e3, 33e3, 470e-12, 22e-9, 22e-9 };
        constexpr ToneStack::Parts modern { 250e3, 1e6, 25e3, 47e3, 500e-12, 22e-9, 22e-9 };

        switch (model)
        {
            case Model::clean:  return { 1, 0.0f, 24.0f, 0.10f, 40.0f, 20.0f, 14000.0f, 6.0f, american, false, -3.0f };
            case Model::crunch: return { 2, 6.0f, 38.0f, 0.20f, 80.0f, 90.0f, 10000.0f, 3.0f, british, false, -1.5f };
            case Model::lead:   return { 3, 14.0f, 58.0f, 0.15f, 120.0f, 180.0f, 7500.0f, 0.0f, modern, false, 3.0f };
            case Model::bass:   return { 1, 0.0f, 30.0f, 0.15f, 30.0f, 20.0f, 9000.0f, 0.0f, american, true, -2.5f };
        }
        return getVoicing (Model::crunch);
    }

    void design() noexcept
    {
        designed = true;
        osRate = baseRate * (double) (1 << oversamplingOrder);
        const auto v = getVoicing (settings.model);
        const auto g = juce::jlimit (0.0f, 1.0f, settings.gain / 10.0f);

        inputHighPass.setup (Biquad::Type::highPass, osRate, v.inputCutHz, 0.707);
        bright.setup (Biquad::Type::highShelf, osRate, 1200.0, 0.6, v.brightDb * (1.0f - g));
        cleanLows = v.cleanLows;
        lowSplit.setup (Biquad::Type::lowPass, osRate, 180.0, 0.5);
        lowLevel = 1.0f;

        // Gain spread evenly over the stages; follow a typical peak through them to
        // know how much the preamp grows it, and divide that back out.
        numStages = v.stages;
        const auto totalDb = v.minGainDb + (v.maxGainDb - v.minGainDb) * std::pow (g, 0.8f);
        const auto stageGain = juce::Decibels::decibelsToGain (totalDb / (float) numStages);
        auto peak = nominalPeak;
        for (int s = 0; s < numStages; ++s)
        {
            auto& stage = stages[(size_t) s];
            stage.gain = stageGain;
            stage.bias = v.bias;
            stage.couplingCoeff = onePole (s == 0 ? 10.0f : v.couplingHz);
            stage.lowPassCoeff = onePole (v.stageTopHz);
            peak = std::abs (clip (peak * stage.gain + stage.bias) - clip (stage.bias));
        }
        // A clipped wave carries more energy than its peak says (it squares up): the
        // harder the preamp is driven, the more of that is taken back too.
        const auto driven = nominalPeak * std::pow (stageGain, (float) numStages) / juce::jmax (1.0e-4f, peak);
        preampMakeUp = nominalPeak / juce::jmax (1.0e-4f, peak) / (1.0f + 0.15f * std::log2 (juce::jmax (1.0f, driven)));

        // Tone stack: audio taper for the bass, linear mid and treble. Its make-up is
        // set with the knobs at noon, so turning them still changes the level.
        toneStack.design (v.stack, 0.5, 0.5, bassTaper (0.5), osRate);
        toneMakeUp = (float) (1.0 / getAverageGain (toneStack, osRate));
        toneStack.design (v.stack, knob (settings.treble), knob (settings.mid), bassTaper (knob (settings.bass)), osRate);

        presenceShelf.setup (Biquad::Type::highShelf, osRate, 3500.0, 0.7, (settings.presence - 5.0f) * 1.5f);

        const auto masterNorm = juce::jlimit (0.0f, 1.0f, settings.master / 10.0f);
        powerDrive = juce::Decibels::decibelsToGain (-6.0f + 24.0f * masterNorm);
        powerMakeUp = nominalPeak / juce::jmax (1.0e-4f, powerSoftClip (nominalPeak * powerDrive)) * juce::Decibels::decibelsToGain (v.levelDb);
    }

    static double knob (float value) noexcept { return (double) juce::jlimit (0.0f, 1.0f, value / 10.0f); }
    static double bassTaper (double l) noexcept { return (std::exp (3.4 * l) - 1.0) / (std::exp (3.4) - 1.0); }

    // Average gain over 100 Hz - 4 kHz (log-spaced): the tone stack's own loss.
    static double getAverageGain (const ToneStack& stack, double rate) noexcept
    {
        double sum = 0.0;
        constexpr int points = 24;
        for (int i = 0; i < points; ++i)
            sum += stack.getMagnitude (100.0 * std::pow (40.0, (double) i / (points - 1)), rate);
        return juce::jmax (1.0e-6, sum / points);
    }

    float onePole (float hz) const noexcept
    {
        return 1.0f - std::exp (-juce::MathConstants<float>::twoPi * hz / (float) osRate);
    }

    struct Stage
    {
        float gain = 1.0f, bias = 0.0f, couplingCoeff = 0.0f, lowPassCoeff = 1.0f;
    };

    struct ChannelState
    {
        std::array<float, maxStages> coupling {}, smooth {};
    };

    std::unique_ptr<juce::dsp::Oversampling<float>> oversampling;
    double baseRate = 48000.0, osRate = 192000.0;
    int numChannels = 2;

    Settings settings;
    bool designed = false;
    std::array<Stage, maxStages> stages;
    int numStages = 1;
    std::array<ChannelState, 2> channels {};

    Biquad inputHighPass, bright, presenceShelf, lowSplit;
    ToneStack toneStack;
    bool cleanLows = false;
    float lowLevel = 1.0f, preampMakeUp = 1.0f, toneMakeUp = 1.0f, powerDrive = 1.0f, powerMakeUp = 1.0f;
};
} // namespace fx
