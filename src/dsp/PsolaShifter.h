#pragma once

#include <juce_core/juce_core.h>

#include <array>
#include <cmath>
#include <vector>

namespace fx
{
// Real-time TD-PSOLA pitch shifter for a monophonic voice.
//
// Hann-windowed grains are cut from the input at spacing equal to the detected pitch
// period and laid back down at spacing period / pitchRatio, which changes the pitch
// while keeping the vocal formants in place. Reading the grains faster or slower
// (formantRatio) moves the formants independently, for "chipmunk" or "monster" voices.
//
// Grain source positions always step by whole periods, so consecutive grains stay in
// phase. Latency adapts to the voice: roughly half a pitch period (~3-6 ms).
class PsolaShifter
{
public:
    void prepare (double sampleRate)
    {
        buffer.assign ((size_t) juce::nextPowerOfTwo ((int) (sampleRate * 0.5)), 0.0f);
        mask = (juce::int64) buffer.size() - 1;
        unvoicedPeriod = (float) (sampleRate * 0.008);
        maxHalfWidth = (float) (sampleRate * 0.03);

        for (size_t i = 0; i < window.size(); ++i)
            window[i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) (window.size() - 1));

        reset();
    }

    // The delay to report for a typical singing voice (around 200 Hz), in samples. The
    // real delay follows the voice: 4 samples plus half a pitch period, wandering by up to
    // half a period either way while the pitch is being changed.
    static int getNominalLatency (double sampleRate) noexcept
    {
        return 4 + juce::roundToInt (0.5 * sampleRate / nominalVoiceHz);
    }

    static constexpr double nominalVoiceHz = 200.0;

    void reset()
    {
        std::fill (buffer.begin(), buffer.end(), 0.0f);
        for (auto& grain : grains)
            grain.active = false;

        now = 0;
        nextMark = 0.0;
        hasLastSource = false;
    }

    // pitchRatio: output / input frequency. period: input pitch period in samples,
    // or <= 0 when unvoiced (then the pitch is left alone and only formants move).
    void setParameters (float newPitchRatio, float newFormantRatio, float newPeriod) noexcept
    {
        const auto voiced = newPeriod > 0.0f;
        period       = voiced ? juce::jlimit (8.0f, maxHalfWidth, newPeriod) : unvoicedPeriod;
        pitchRatio   = voiced ? juce::jlimit (0.25f, 4.0f, newPitchRatio) : 1.0f;
        formantRatio = juce::jlimit (0.5f, 2.0f, newFormantRatio);
        isVoiced     = voiced;
    }

    float processSample (float input) noexcept
    {
        buffer[(size_t) (now & mask)] = input;

        // Schedule each grain as its window opens (half a grain before its centre).
        while (nextMark - halfWidth() <= (double) now)
            spawnGrain();

        float sum = 0.0f, weight = 0.0f;

        for (auto& grain : grains)
        {
            if (! grain.active)
                continue;

            const auto age = (double) now - grain.start;

            if (age >= grain.length)
            {
                grain.active = false;
                continue;
            }

            if (age < 0.0)
                continue; // scheduled but its window hasn't opened yet

            const auto w = windowAt ((float) (age / grain.length));
            sum += w * read (grain.sourceStart + age * grain.readRate);
            weight += w;
        }

        ++now;

        // Raising the pitch overlaps grains more densely, so divide by the summed window
        // to keep the level steady. Lowering it leaves gaps, which are kept (they are the
        // new pitch) and made up for with a little gain instead.
        const auto gapCompensation = pitchRatio < 1.0f ? std::sqrt (1.0f / pitchRatio) : 1.0f;
        return sum / juce::jmax (weight, 1.0f) * gapCompensation;
    }

private:
    struct Grain
    {
        bool active = false;
        double start = 0.0, length = 1.0, sourceStart = 0.0;
        float readRate = 1.0f;
    };

    // Classic PSOLA grains span two input periods. When lowering the pitch they are
    // spaced further apart than that, and the gaps between them are what create the
    // new, longer period, so they must not be stretched to fill them.
    float halfWidth() const noexcept
    {
        return juce::jmin (maxHalfWidth, period);
    }

    void spawnGrain() noexcept
    {
        const auto hw = halfWidth();
        const auto centre = nextMark;
        nextMark += period / pitchRatio;

        // How far behind the output the source must be so a grain never reads samples
        // that haven't arrived yet.
        const auto safety = (double) hw * std::abs (formantRatio - 1.0f) + 4.0;
        const auto target = centre - safety - 0.5 * period;

        auto sourceCentre = target;

        if (isVoiced && hasLastSource)
            sourceCentre = lastSourceCentre + std::round ((target - lastSourceCentre) / period) * period;

        // Never read into the future (possible right after a large parameter change).
        while (sourceCentre > centre - safety)
            sourceCentre -= period;

        lastSourceCentre = sourceCentre;
        hasLastSource = true;

        for (auto& grain : grains)
        {
            if (grain.active)
                continue;

            grain.active      = true;
            grain.start       = centre - hw;
            grain.length      = 2.0 * hw;
            grain.readRate    = formantRatio;
            grain.sourceStart = sourceCentre - hw * formantRatio;
            return;
        }
    }

    float windowAt (float position) const noexcept
    {
        const auto index = position * (float) (window.size() - 1);
        const auto i = (size_t) index;
        const auto frac = index - (float) i;
        return i + 1 < window.size() ? window[i] + frac * (window[i + 1] - window[i]) : window.back();
    }

    // 4-point cubic Hermite interpolation.
    float read (double position) const noexcept
    {
        const auto i = (juce::int64) std::floor (position);
        const auto f = (float) (position - (double) i);

        const auto xm1 = buffer[(size_t) ((i - 1) & mask)];
        const auto x0  = buffer[(size_t) (i & mask)];
        const auto x1  = buffer[(size_t) ((i + 1) & mask)];
        const auto x2  = buffer[(size_t) ((i + 2) & mask)];

        const auto c1 = 0.5f * (x1 - xm1);
        const auto c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
        const auto c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);

        return ((c3 * f + c2) * f + c1) * f + x0;
    }

    std::vector<float> buffer;
    juce::int64 mask = 0, now = 0;

    std::array<Grain, 24> grains {};
    std::array<float, 1024> window {};

    double nextMark = 0.0, lastSourceCentre = 0.0;
    bool hasLastSource = false;
    float period = 350.0f, unvoicedPeriod = 350.0f, maxHalfWidth = 1300.0f;
    float pitchRatio = 1.0f, formantRatio = 1.0f;
    bool isVoiced = false;
};
} // namespace fx
