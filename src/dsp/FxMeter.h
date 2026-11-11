#pragma once

#include <juce_dsp/juce_dsp.h>

#include <array>
#include <atomic>
#include <cmath>

// What the UI needs to animate an effect from the signal going through it: peak levels
// in and out, and (only while the UI is looking) the latest samples before and after
// it, for a spectrum. The audio thread writes with relaxed atomics and never waits;
// the UI reads at its own pace (about 30 times a second).
namespace fx
{
// Lock-free "max since last read", so short peaks aren't missed between UI frames.
inline void raisePeak (std::atomic<float>& peak, float value) noexcept
{
    auto current = peak.load (std::memory_order_relaxed);
    while (value > current && ! peak.compare_exchange_weak (current, value, std::memory_order_relaxed)) {}
}

// Highest absolute sample over every channel of a block.
inline float blockPeak (const juce::dsp::AudioBlock<float>& block) noexcept
{
    float peak = 0.0f;

    for (size_t ch = 0; ch < block.getNumChannels(); ++ch)
    {
        const auto range = juce::FloatVectorOperations::findMinAndMax (block.getChannelPointer (ch), (int) block.getNumSamples());
        peak = juce::jmax (peak, std::abs (range.getStart()), std::abs (range.getEnd()));
    }

    return peak;
}

inline float peakToDecibels (float gain) noexcept
{
    return std::round (juce::Decibels::gainToDecibels (gain, -100.0f) * 10.0f) / 10.0f;
}

//==============================================================================
// The latest samples (first channel) going into and out of an effect. Capturing costs
// nothing until someone asks: request() switches it on for about a second, so it stops
// by itself when the UI goes away or stops looking.
class Scope
{
public:
    static constexpr int size = 2048; // a power of two

    // Any thread: keep capturing for a while longer.
    void request() noexcept { wanted.store (48000, std::memory_order_relaxed); }

    bool isWanted() const noexcept { return wanted.load (std::memory_order_relaxed) > 0; }

    // Audio thread. `before` and `after` are numSamples long.
    void push (const float* before, const float* after, int numSamples) noexcept
    {
        if (! isWanted())
            return;

        wanted.fetch_sub (numSamples, std::memory_order_relaxed);
        auto write = writeIndex.load (std::memory_order_relaxed);

        for (int i = 0; i < numSamples; ++i)
        {
            samplesIn[(size_t) write].store (before[i], std::memory_order_relaxed);
            samplesOut[(size_t) write].store (after[i], std::memory_order_relaxed);
            write = (write + 1) & (size - 1);
        }

        writeIndex.store (write, std::memory_order_release);
    }

    // Any thread: the latest `size` samples, oldest first. A block written meanwhile
    // can tear the copy; for a picture that doesn't matter.
    void read (std::array<float, size>& in, std::array<float, size>& out) const noexcept
    {
        const auto start = writeIndex.load (std::memory_order_acquire);

        for (int i = 0; i < size; ++i)
        {
            const auto index = (size_t) ((start + i) & (size - 1));
            in[(size_t) i] = samplesIn[index].load (std::memory_order_relaxed);
            out[(size_t) i] = samplesOut[index].load (std::memory_order_relaxed);
        }
    }

private:
    std::array<std::atomic<float>, size> samplesIn {}, samplesOut {};
    std::atomic<int> writeIndex { 0 };
    std::atomic<int> wanted { 0 }; // samples left to capture
};

//==============================================================================
// Spectrum bands for the UI: `numBands` log-spaced bands from 20 Hz to 20 kHz, in dB
// (0 dB = a full-scale sine). Message thread; allocates.
class SpectrumAnalyser
{
public:
    static constexpr int numBands = 48;
    static constexpr float floorDb = -100.0f;

    // -> { pre: [dB...], post: [dB...] }
    juce::var analyse (const Scope& scope, double sampleRate)
    {
        scope.read (in, out);
        auto* result = new juce::DynamicObject();
        result->setProperty ("pre", bands (in, sampleRate));
        result->setProperty ("post", bands (out, sampleRate));
        return juce::var (result);
    }

private:
    static constexpr int order = 11;
    static constexpr int fftSize = 1 << order;
    static_assert (fftSize == Scope::size);

    juce::var bands (const std::array<float, Scope::size>& samples, double sampleRate)
    {
        std::copy (samples.begin(), samples.end(), fftData.begin());
        std::fill (fftData.begin() + fftSize, fftData.end(), 0.0f);
        window.multiplyWithWindowingTable (fftData.data(), (size_t) fftSize);
        fft.performFrequencyOnlyForwardTransform (fftData.data(), true);

        // A full-scale sine through a Hann window peaks at fftSize / 4.
        const auto scale = 4.0f / (float) fftSize;
        const auto binHz = sampleRate / fftSize;
        juce::Array<juce::var> list;
        list.ensureStorageAllocated (numBands);

        for (int b = 0; b < numBands; ++b)
        {
            const auto f0 = 20.0 * std::pow (1000.0, (double) b / numBands);
            const auto f1 = 20.0 * std::pow (1000.0, (double) (b + 1) / numBands);
            if (f0 >= sampleRate * 0.5)
            {
                list.add (floorDb); // above Nyquist at low sample rates
                continue;
            }

            auto first = (int) std::ceil (f0 / binHz), last = (int) std::floor (f1 / binHz);

            // Low bands narrower than a bin take the nearest one.
            if (last < first)
                first = last = (int) std::round (std::sqrt (f0 * f1) / binHz);

            first = juce::jlimit (1, fftSize / 2 - 1, first);
            last = juce::jlimit (first, fftSize / 2 - 1, last);

            float peak = 0.0f;
            for (int i = first; i <= last; ++i)
                peak = juce::jmax (peak, fftData[(size_t) i]);

            list.add (std::round (juce::jmax (floorDb, juce::Decibels::gainToDecibels (peak * scale, floorDb))));
        }

        return list;
    }

    juce::dsp::FFT fft { order };
    juce::dsp::WindowingFunction<float> window { (size_t) fftSize, juce::dsp::WindowingFunction<float>::hann, false };
    std::array<float, fftSize * 2> fftData {};
    std::array<float, Scope::size> in {}, out {};
};
}
