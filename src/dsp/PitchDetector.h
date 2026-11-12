#pragma once

#include <juce_core/juce_core.h>

#include <cmath>
#include <vector>

namespace fx
{
// YIN monophonic pitch detector (de Cheveigné & Kawahara, 2002).
// Analysis runs on a decimated copy of the input every ~5 ms, looking at the most
// recent ~14 ms, so it's cheap enough for the audio thread and adds no audio latency.
class PitchDetector
{
public:
    static constexpr float minFrequency = 70.0f;
    static constexpr float maxFrequency = 1000.0f;

    void prepare (double sampleRate)
    {
        decimation   = sampleRate > 60000.0 ? 4 : (sampleRate > 30000.0 ? 2 : 1);
        analysisRate = sampleRate / decimation;
        maxLag       = (int) std::ceil (analysisRate / minFrequency);
        minLag       = juce::jmax (2, (int) std::floor (analysisRate / maxFrequency));
        windowSize   = maxLag;
        hopSize      = juce::jmax (1, (int) (analysisRate * 0.005));

        history.assign ((size_t) (windowSize + maxLag), 0.0f);
        frame.assign (history.size(), 0.0f);
        difference.assign ((size_t) maxLag + 2, 0.0f);

        reset();
    }

    void reset()
    {
        std::fill (history.begin(), history.end(), 0.0f);
        writeIndex = 0;
        decimationSum = 0.0f;
        decimationCount = 0;
        samplesSinceAnalysis = 0;
        frequency = 0.0f;
    }

    void push (const float* samples, int numSamples) noexcept
    {
        for (int i = 0; i < numSamples; ++i)
        {
            decimationSum += samples[i];

            if (++decimationCount < decimation)
                continue;

            history[(size_t) writeIndex] = decimationSum / (float) decimation;
            writeIndex = (writeIndex + 1) % (int) history.size();
            decimationSum = 0.0f;
            decimationCount = 0;

            if (autoAnalyse && ++samplesSinceAnalysis >= hopSize)
            {
                samplesSinceAnalysis = 0;
                analyse();
            }
        }
    }

    // Off: push() only collects the input, and analyseNow() gives an estimate when the
    // caller wants one (offline analysis, once per 100 ms hop instead of every 5 ms).
    void setAutoAnalyse (bool shouldAnalyse) noexcept { autoAnalyse = shouldAnalyse; }
    void analyseNow() noexcept { analyse(); }

    // Most recent estimate in Hz, or 0 when the input is silent or unpitched.
    float getFrequency() const noexcept { return frequency; }

    // Pitch period in samples at the original sample rate, or 0 when unvoiced.
    float getPeriodInSamples() const noexcept
    {
        return frequency > 0.0f ? (float) (analysisRate * decimation) / frequency : 0.0f;
    }

private:
    void analyse() noexcept
    {
        // Unroll the ring buffer, oldest sample first.
        const auto size = (int) history.size();
        for (int i = 0; i < size; ++i)
            frame[(size_t) i] = history[(size_t) ((writeIndex + i) % size)];

        const auto* newest = frame.data() + maxLag; // the latest windowSize samples

        float energy = 0.0f;
        for (int j = 0; j < windowSize; ++j)
            energy += newest[j] * newest[j];

        if (std::sqrt (energy / (float) windowSize) < silenceThreshold)
        {
            frequency = 0.0f;
            return;
        }

        // Difference function: compare the newest window with itself tau samples earlier.
        for (int tau = 1; tau <= maxLag; ++tau)
        {
            const auto* lagged = newest - tau;
            float sum = 0.0f;

            for (int j = 0; j < windowSize; ++j)
            {
                const auto delta = newest[j] - lagged[j];
                sum += delta * delta;
            }

            difference[(size_t) tau] = sum;
        }

        // Cumulative mean normalised difference.
        difference[0] = 1.0f;
        float runningSum = 0.0f;

        for (int tau = 1; tau <= maxLag; ++tau)
        {
            runningSum += difference[(size_t) tau];
            difference[(size_t) tau] = runningSum > 0.0f ? difference[(size_t) tau] * (float) tau / runningSum : 1.0f;
        }

        // First dip below the threshold, then walk down to its local minimum.
        int tau = -1;

        for (int t = minLag; t < maxLag; ++t)
        {
            if (difference[(size_t) t] < yinThreshold)
            {
                while (t + 1 < maxLag && difference[(size_t) t + 1] < difference[(size_t) t])
                    ++t;

                tau = t;
                break;
            }
        }

        if (tau < 0)
        {
            frequency = 0.0f;
            return;
        }

        // Parabolic interpolation for sub-sample accuracy.
        const auto prev = difference[(size_t) tau - 1];
        const auto curr = difference[(size_t) tau];
        const auto next = difference[(size_t) tau + 1];
        const auto denominator = prev - 2.0f * curr + next;
        const auto refinedTau = (float) tau + (std::abs (denominator) > 1.0e-9f ? 0.5f * (prev - next) / denominator : 0.0f);

        frequency = (float) analysisRate / refinedTau;
    }

    static constexpr float yinThreshold = 0.15f;
    static constexpr float silenceThreshold = 0.003f; // about -50 dBFS RMS

    int decimation = 1, maxLag = 0, minLag = 2, windowSize = 0, hopSize = 1;
    double analysisRate = 44100.0;

    std::vector<float> history, frame, difference;
    int writeIndex = 0, decimationCount = 0, samplesSinceAnalysis = 0;
    float decimationSum = 0.0f;
    float frequency = 0.0f;
    bool autoAnalyse = true;
};
} // namespace fx
