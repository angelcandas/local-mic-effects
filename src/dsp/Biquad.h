#pragma once

#include <juce_core/juce_core.h>

#include <array>
#include <cmath>

namespace fx
{
// RBJ "Audio EQ Cookbook" biquad in transposed direct form II, for up to two channels.
// Recomputing coefficients never allocates, so setup() is safe on the audio thread.
class Biquad
{
public:
    enum class Type { lowPass, highPass, bandPass, peak, lowShelf, highShelf };

    void setup (Type type, double sampleRate, double frequency, double q, double gainDb = 0.0) noexcept
    {
        const auto w0    = juce::MathConstants<double>::twoPi * juce::jlimit (10.0, sampleRate * 0.49, frequency) / sampleRate;
        const auto cosW  = std::cos (w0);
        const auto alpha = std::sin (w0) / (2.0 * q);
        const auto a     = std::pow (10.0, gainDb / 40.0);
        const auto shelf = 2.0 * std::sqrt (a) * alpha;

        double b0 = 1, b1 = 0, b2 = 0, a0 = 1, a1 = 0, a2 = 0;

        switch (type)
        {
            case Type::lowPass:
                b0 = (1 - cosW) / 2; b1 = 1 - cosW; b2 = b0;
                a0 = 1 + alpha; a1 = -2 * cosW; a2 = 1 - alpha;
                break;

            case Type::highPass:
                b0 = (1 + cosW) / 2; b1 = -(1 + cosW); b2 = b0;
                a0 = 1 + alpha; a1 = -2 * cosW; a2 = 1 - alpha;
                break;

            case Type::bandPass: // constant 0 dB peak gain
                b0 = alpha; b1 = 0; b2 = -alpha;
                a0 = 1 + alpha; a1 = -2 * cosW; a2 = 1 - alpha;
                break;

            case Type::peak:
                b0 = 1 + alpha * a; b1 = -2 * cosW; b2 = 1 - alpha * a;
                a0 = 1 + alpha / a; a1 = -2 * cosW; a2 = 1 - alpha / a;
                break;

            case Type::lowShelf:
                b0 = a * ((a + 1) - (a - 1) * cosW + shelf);
                b1 = 2 * a * ((a - 1) - (a + 1) * cosW);
                b2 = a * ((a + 1) - (a - 1) * cosW - shelf);
                a0 = (a + 1) + (a - 1) * cosW + shelf;
                a1 = -2 * ((a - 1) + (a + 1) * cosW);
                a2 = (a + 1) + (a - 1) * cosW - shelf;
                break;

            case Type::highShelf:
                b0 = a * ((a + 1) + (a - 1) * cosW + shelf);
                b1 = -2 * a * ((a - 1) + (a + 1) * cosW);
                b2 = a * ((a + 1) + (a - 1) * cosW - shelf);
                a0 = (a + 1) - (a - 1) * cosW + shelf;
                a1 = 2 * ((a - 1) - (a + 1) * cosW);
                a2 = (a + 1) - (a - 1) * cosW - shelf;
                break;
        }

        c = { (float) (b0 / a0), (float) (b1 / a0), (float) (b2 / a0), (float) (a1 / a0), (float) (a2 / a0) };
    }

    float process (size_t channel, float x) noexcept
    {
        auto& s = state[channel];
        const auto y = c[0] * x + s[0];
        s[0] = c[1] * x - c[3] * y + s[1];
        s[1] = c[2] * x - c[4] * y;
        return y;
    }

    void reset() noexcept { state = {}; }

    // Gain of the current coefficients at `frequency`, in dB.
    double getMagnitudeDb (double frequency, double sampleRate) const noexcept
    {
        const auto w = juce::MathConstants<double>::twoPi * frequency / sampleRate;
        const auto c1 = std::cos (w), s1 = std::sin (w), c2 = std::cos (2 * w), s2 = std::sin (2 * w);
        const auto nr = c[0] + c[1] * c1 + c[2] * c2, ni = -(c[1] * s1 + c[2] * s2);
        const auto dr = 1.0 + c[3] * c1 + c[4] * c2,  di = -(c[3] * s1 + c[4] * s2);
        return 10.0 * std::log10 ((nr * nr + ni * ni) / juce::jmax (1.0e-30, dr * dr + di * di));
    }

private:
    std::array<float, 5> c { 1.0f, 0.0f, 0.0f, 0.0f, 0.0f }; // b0 b1 b2 a1 a2
    std::array<std::array<float, 2>, 2> state {};
};
} // namespace fx
