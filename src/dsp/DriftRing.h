#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <vector>

// Stereo audio from one clock to another: a capture thread (the computer's sound, another
// audio device) pushes at its own rate and the audio thread pulls at the interface's.
// The two clocks never agree exactly (and the rates may differ, 44.1 against 48 kHz), so
// the pull side reads at a ratio that follows the rates and then nudges itself, a few
// hundredths of a percent, to keep the fill near a target. Too little audio: silence
// until there's enough again. Too much (a stall on our side): it skips ahead.
//
// One producer thread and one consumer thread, lock-free. prepare() runs while neither
// is using it.
namespace fx
{
class DriftRing
{
public:
    // The most the pull ratio moves away from sourceRate / destRate to follow the fill.
    static constexpr double maxCorrection = 0.005;

    void prepare (double newSourceRate, double newDestRate, double seconds = 1.0)
    {
        sourceRate = newSourceRate;
        destRate = newDestRate;
        baseRatio = sourceRate > 0.0 && destRate > 0.0 ? sourceRate / destRate : 1.0;

        auto size = 1024;
        while (size < (int) (seconds * sourceRate))
            size *= 2;

        capacity = size;
        mask = size - 1;
        left.assign ((size_t) size, 0.0f);
        right.assign ((size_t) size, 0.0f);
        written.store (0);
        consumed.store (0);
        maxChunk.store (0);
        overflows.store (0);
        underruns.store (0);
        readPosition = 0.0;
        waitFrom = 0;
        running = false;
        smoothedFill = 0.0;
        ratio = baseRatio;
    }

    // Producer. `r` may be null (mono: both sides get `l`).
    void push (const float* l, const float* r, int numFrames) noexcept
    {
        if (capacity == 0 || numFrames <= 0)
            return;

        const auto w = written.load (std::memory_order_relaxed);

        // Full: drop the new audio rather than write over what the consumer is reading.
        // (It falls behind, notices the fill and skips ahead.)
        if (w + numFrames - consumed.load (std::memory_order_acquire) > capacity - 8)
        {
            overflows.fetch_add (1, std::memory_order_relaxed);
            return;
        }

        for (int i = 0; i < numFrames; ++i)
        {
            const auto index = (size_t) ((w + i) & mask);
            left[index] = l[i];
            right[index] = r != nullptr ? r[i] : l[i];
        }

        if (numFrames > maxChunk.load (std::memory_order_relaxed))
            maxChunk.store (numFrames, std::memory_order_relaxed);

        written.store (w + numFrames, std::memory_order_release);
    }

    // Consumer. Writes numFrames to l and r; false (and silence) while it's waiting for
    // enough audio.
    bool pull (float* l, float* r, int numFrames) noexcept
    {
        if (capacity == 0 || numFrames <= 0)
        {
            std::fill (l, l + std::max (0, numFrames), 0.0f);
            std::fill (r, r + std::max (0, numFrames), 0.0f);
            return false;
        }

        const auto w = written.load (std::memory_order_acquire);
        const auto target = getTargetFill (numFrames);

        if (! running)
        {
            // Only audio pushed since it last ran dry counts (what's before it was played).
            if ((double) (w - waitFrom) < target + 4.0)
                return silence (l, r, numFrames);

            start (w, target);
        }

        auto fill = (double) w - readPosition;
        const auto needed = numFrames * baseRatio * (1.0 + maxCorrection) + 3.0;

        if (fill < needed)
        {
            // Ran dry: wait for a full target again (a click is better than a stutter).
            underruns.fetch_add (1, std::memory_order_relaxed);
            running = false;
            waitFrom = w;
            consumed.store (w, std::memory_order_release);
            return silence (l, r, numFrames);
        }

        if (fill > std::max (target * 4.0, target + 0.25 * sourceRate) || fill > capacity / 2)
        {
            // Far behind (we stalled, or the source sped ahead): skip to the newest audio.
            start (w, target);
            fill = (double) w - readPosition;
        }

        // Follow the fill: a slow average (the fill saw-tooths with the two block sizes),
        // then a small proportional correction to the ratio.
        const auto coefficient = 1.0 - std::exp (-numFrames / (0.5 * destRate));
        smoothedFill += coefficient * (fill - smoothedFill);
        const auto error = (smoothedFill - target) / std::max (1.0, target);
        ratio = baseRatio * (1.0 + std::clamp (0.005 * error, -maxCorrection, maxCorrection));

        for (int i = 0; i < numFrames; ++i)
        {
            const auto position = readPosition + i * ratio;
            const auto index = (std::int64_t) position;
            const auto fraction = (float) (position - (double) index);
            l[i] = interpolate (left, index, fraction);
            r[i] = interpolate (right, index, fraction);
        }

        readPosition += numFrames * ratio;
        // Keep a couple of frames behind the read position for the interpolation.
        consumed.store ((std::int64_t) readPosition - 2, std::memory_order_release);
        return true;
    }

    // Consumer. Back to waiting (the source stopped or is about to change).
    void reset() noexcept
    {
        running = false;
        waitFrom = written.load (std::memory_order_acquire);
        consumed.store (written.load (std::memory_order_acquire), std::memory_order_release);
    }

    bool isRunning() const noexcept          { return running; }
    double getRatio() const noexcept         { return ratio; }
    double getBaseRatio() const noexcept     { return baseRatio; }
    double getFill() const noexcept          { return (double) written.load() - readPosition; }
    double getTargetFill (int pullFrames) const noexcept
    {
        // Room for one of the producer's chunks and two of ours, plus a little.
        return 1.5 * maxChunk.load (std::memory_order_relaxed) + 2.0 * pullFrames * baseRatio + 32.0;
    }
    int getOverflows() const noexcept        { return overflows.load(); }
    int getUnderruns() const noexcept        { return underruns.load(); }

private:
    bool silence (float* l, float* r, int numFrames) noexcept
    {
        std::fill (l, l + numFrames, 0.0f);
        std::fill (r, r + numFrames, 0.0f);
        return false;
    }

    void start (std::int64_t w, double target) noexcept
    {
        readPosition = (double) w - target;
        smoothedFill = target;
        running = true;
        consumed.store ((std::int64_t) readPosition - 2, std::memory_order_release);
    }

    // 4-point Hermite: keeps the top end that straight-line interpolation dulls.
    float interpolate (const std::vector<float>& data, std::int64_t index, float t) const noexcept
    {
        const auto x0 = data[(size_t) ((index - 1) & mask)];
        const auto x1 = data[(size_t) (index & mask)];
        const auto x2 = data[(size_t) ((index + 1) & mask)];
        const auto x3 = data[(size_t) ((index + 2) & mask)];
        const auto c1 = 0.5f * (x2 - x0);
        const auto c2 = x0 - 2.5f * x1 + 2.0f * x2 - 0.5f * x3;
        const auto c3 = 0.5f * (x3 - x0) + 1.5f * (x1 - x2);
        return ((c3 * t + c2) * t + c1) * t + x1;
    }

    std::vector<float> left, right;
    std::int64_t capacity = 0, mask = 0;
    double sourceRate = 48000.0, destRate = 48000.0, baseRatio = 1.0;

    std::atomic<std::int64_t> written { 0 }, consumed { 0 };
    std::atomic<int> maxChunk { 0 }, overflows { 0 }, underruns { 0 };

    // Consumer only.
    double readPosition = 0.0, smoothedFill = 0.0, ratio = 1.0;
    std::int64_t waitFrom = 0;
    std::atomic<bool> running { false };
};
} // namespace fx
