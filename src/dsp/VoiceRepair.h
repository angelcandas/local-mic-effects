#pragma once

#include "Biquad.h"
#include "FxModule.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <vector>

// Voice repair for recorded vocals and speech (DAW inserts): a breath tamer that turns
// breaths down, and a mouth click remover that rebuilds the few samples of each click.
// Both look ahead, so they add a fixed delay the DAW makes up for.
namespace fx
{
//==============================================================================
// Breaths are quiet, unpitched and noisy, with little energy in the lows; the voice
// around them is pitched and louder. Every 5 ms frame of the (mono) input gets:
//   - its level, against the level of the recent pitched voice (a peak that falls 3 dB/s),
//   - how pitched it is (normalised autocorrelation at 80-800 Hz, at ~12 kHz),
//   - how much of it is below 400 Hz and how much above 5 kHz (an "s" lives up there).
// A frame that is unpitched, well below the voice, not low-heavy and not "s"-like is a
// breath candidate. A run of candidates long enough (40-70 ms, shorter when more sensitive)
// is a breath: the whole run, from its first frame, is turned down by Reduction. The audio
// is delayed by enough frames for that decision to be made before the breath plays.
class BreathTamer
{
public:
    static constexpr int maxChannels = 2;
    static constexpr double frameSeconds = 0.005;
    static constexpr int longestRun = 14, shortestRun = 8; // frames needed at sensitivity 0 and 100 %
    static constexpr int delayFrames = longestRun + 2;
    static constexpr double rampSeconds = 0.006;

    // Not real-time safe (allocates).
    void prepare (double rate, int numChannels)
    {
        sampleRate = rate;
        channels = juce::jlimit (1, maxChannels, numChannels);
        frameSize = juce::jmax (16, (int) std::lround (frameSeconds * rate));
        latency = delayFrames * frameSize;

        auto size = 1;
        while (size < latency + frameSize + 8)
            size <<= 1;
        mask = size - 1;
        for (auto& ring : rings)
            ring.assign ((size_t) size, 0.0f);

        low.setup (Biquad::Type::lowPass, rate, 400.0, 0.707);
        high.setup (Biquad::Type::highPass, rate, 5000.0, 0.707);
        decimateFilter.setup (Biquad::Type::lowPass, rate, 3000.0, 0.707);
        decimation = juce::jmax (1, (int) std::lround (rate / 12000.0));
        const auto lowRate = rate / decimation;
        minLag = juce::jmax (2, (int) std::lround (lowRate / 800.0));
        maxLag = juce::jmin (historySize - windowSize - 1, (int) std::lround (lowRate / 80.0));

        rampCoefficient = (float) (1.0 - std::exp (-1.0 / (rampSeconds * rate)));
        voiceFallPerFrame = 3.0f * (float) frameSeconds;
        reset();
    }

    void reset() noexcept
    {
        for (auto& ring : rings)
            std::fill (ring.begin(), ring.end(), 0.0f);
        marks.fill (false);
        history.fill (0.0f);
        low.reset();
        high.reset();
        decimateFilter.reset();
        written = 0;
        inFrame = 0;
        frameIndex = 0;
        decimatePhase = 0;
        historyPosition = 0;
        eTotal = eLow = eHigh = 0.0;
        run = 0;
        sinceCandidate = 1000;
        confirmed = false;
        voiceDb = -24.0f;
        gain = 1.0f;
    }

    // reductionDb: how far a breath is turned down. sensitivity 0..1: how readily a sound
    // counts as a breath (quieter gap to the voice, shorter breaths, less strict shape).
    void setParameters (float reductionDb, float sensitivity) noexcept
    {
        floorGain = juce::Decibels::decibelsToGain (-juce::jmax (0.0f, reductionDb), -120.0f);
        const auto s = juce::jlimit (0.0f, 1.0f, sensitivity);
        neededGapDb = 22.0f - 14.0f * s;
        maxPitched = 0.35f + 0.2f * s;
        maxLowShare = 0.25f + 0.2f * s;
        neededRun = juce::roundToInt ((float) longestRun - (float) (longestRun - shortestRun) * s);
    }

    int getLatencySamples() const noexcept { return latency; }

    void process (juce::dsp::AudioBlock<float>& block) noexcept
    {
        const auto numChannels = juce::jmin ((int) block.getNumChannels(), channels);
        if (numChannels == 0)
            return;

        for (size_t i = 0; i < block.getNumSamples(); ++i)
        {
            auto mono = 0.0f;
            for (int ch = 0; ch < numChannels; ++ch)
            {
                const auto x = block.getSample (ch, (int) i);
                rings[(size_t) ch][(size_t) (written & mask)] = x;
                mono += x;
            }
            mono /= (float) numChannels;
            analyse (mono);

            // The sample leaving now is `latency` behind; its frame's mark (and the next
            // one's, so the ramp down starts a little early) are final by now.
            const auto out = written - latency;
            auto target = 1.0f;
            if (out >= 0)
            {
                const auto frame = out / frameSize;
                if (marks[(size_t) (frame % marksSize)] || marks[(size_t) ((frame + 1) % marksSize)])
                    target = floorGain;
            }
            gain += rampCoefficient * (target - gain);
            reductionNow = juce::jmax (reductionNow, -juce::Decibels::gainToDecibels (gain, -120.0f));

            for (int ch = 0; ch < numChannels; ++ch)
                block.setSample (ch, (int) i, out >= 0 ? rings[(size_t) ch][(size_t) (out & mask)] * gain : 0.0f);

            ++written;
        }
    }

    // The most it turned down since the last call (dB), and breaths found. Any thread.
    float takeReductionDb() noexcept { return reductionPeak.exchange (0.0f, std::memory_order_relaxed); }
    int takeBreaths() noexcept       { return breathsFound.exchange (0, std::memory_order_relaxed); }

    // The latest frame's features, for tests.
    struct Features { float levelDb = -120.0f, gapDb = 0.0f, pitched = 0.0f, lowShare = 0.0f, highShare = 0.0f; bool candidate = false; };
    Features getLastFeatures() const noexcept { return last; }

private:
    static constexpr int marksSize = 64;
    static constexpr int historySize = 512; // decimated samples kept for the pitch check
    static constexpr int windowSize = 128;

    void analyse (float mono) noexcept
    {
        const auto l = low.process (0, mono), h = high.process (0, mono);
        eTotal += (double) mono * mono;
        eLow += (double) l * l;
        eHigh += (double) h * h;

        const auto d = decimateFilter.process (0, mono);
        if (++decimatePhase >= decimation)
        {
            decimatePhase = 0;
            history[(size_t) historyPosition] = d;
            historyPosition = (historyPosition + 1) % historySize;
        }

        if (++inFrame < frameSize)
            return;

        inFrame = 0;
        classifyFrame();
        eTotal = eLow = eHigh = 0.0;
        reductionPeak.store (juce::jmax (reductionPeak.load (std::memory_order_relaxed), reductionNow), std::memory_order_relaxed);
        reductionNow = 0.0f;
        ++frameIndex;
    }

    // How periodic the last ~10 ms are: the best normalised autocorrelation between the
    // newest window and the one a lag earlier, for pitches 80-800 Hz.
    float pitchedness() const noexcept
    {
        const auto at = [this] (int back) { return history[(size_t) ((historyPosition - 1 - back + 2 * historySize) % historySize)]; };
        auto energy = 0.0;
        for (int n = 0; n < windowSize; ++n)
            energy += (double) at (n) * at (n);
        if (energy < 1.0e-12)
            return 0.0f;

        auto best = 0.0;
        for (int lag = minLag; lag <= maxLag; ++lag)
        {
            auto cross = 0.0, other = 0.0;
            for (int n = 0; n < windowSize; ++n)
            {
                const auto y = at (n + lag);
                cross += (double) at (n) * y;
                other += (double) y * y;
            }
            if (other > 1.0e-12)
                best = std::max (best, cross / std::sqrt (energy * other));
        }
        return (float) best;
    }

    void classifyFrame() noexcept
    {
        const auto power = eTotal / frameSize;
        const auto levelDb = (float) (10.0 * std::log10 (power + 1.0e-14));
        const auto lowShare = (float) (eLow / (eTotal + 1.0e-14));
        const auto highShare = (float) (eHigh / (eTotal + 1.0e-14));
        const auto pitched = levelDb > -80.0f ? pitchedness() : 0.0f;

        // The voice's level: the peak of pitched frames, falling slowly in between.
        voiceDb = juce::jmax (-40.0f, voiceDb - voiceFallPerFrame);
        if (pitched > 0.7f && levelDb > -60.0f)
            voiceDb = juce::jmax (voiceDb, levelDb);

        const auto gap = voiceDb - levelDb;
        const auto candidate = levelDb > -85.0f && gap > neededGapDb && pitched < maxPitched
                                && lowShare < maxLowShare && highShare < 0.6f;
        last = { levelDb, gap, pitched, lowShare, highShare, candidate };

        const auto slot = [] (juce::int64 frame) { return (size_t) (frame % marksSize); };
        marks[slot (frameIndex)] = false;

        if (candidate)
        {
            sinceCandidate = 0;
            ++run;
            if (confirmed)
                marks[slot (frameIndex)] = true;
            else if (run >= neededRun)
            {
                // Long enough: it's a breath from its first frame.
                confirmed = true;
                breathsFound.fetch_add (1, std::memory_order_relaxed);
                for (int back = 0; back < run && back < marksSize - 2; ++back)
                    marks[slot (frameIndex - back)] = true;
            }
        }
        else if (++sinceCandidate > 1)
        {
            run = 0;
            confirmed = false;
        }
        else if (confirmed)
            marks[slot (frameIndex)] = true; // one odd frame inside a breath doesn't end it
    }

    double sampleRate = 48000.0;
    int channels = 1, frameSize = 240, latency = delayFrames * 240, mask = 0;
    std::array<std::vector<float>, maxChannels> rings;
    std::array<bool, marksSize> marks {};
    std::array<float, historySize> history {};
    Biquad low, high, decimateFilter;
    int decimation = 4, decimatePhase = 0, historyPosition = 0, minLag = 15, maxLag = 150;
    juce::int64 written = 0, frameIndex = 0;
    int inFrame = 0, run = 0, sinceCandidate = 1000;
    bool confirmed = false;
    double eTotal = 0.0, eLow = 0.0, eHigh = 0.0;
    float voiceDb = -24.0f, voiceFallPerFrame = 0.015f;
    float floorGain = 0.25f, neededGapDb = 15.0f, maxPitched = 0.45f, maxLowShare = 0.35f;
    int neededRun = 11;
    float gain = 1.0f, rampCoefficient = 0.004f, reductionNow = 0.0f;
    Features last;
    std::atomic<float> reductionPeak { 0.0f };
    std::atomic<int> breathsFound { 0 };
};

//==============================================================================
// Mouth clicks (lip smacks, tongue ticks, saliva crackle) are tiny, sharp spikes: a
// fraction of a millisecond, much brighter than the voice under them. The detector
// listens to the second difference of the (mono) signal, which lifts the highs, against
// its own recent level: a jump k times over it (k 12..4 with sensitivity) that ends
// within 2 ms, isn't matched by a peak in the 2-24 ms before it (a voice's sharp edges
// come back every pitch period) and is followed by the level it had before is a click. Anything longer
// (a consonant, a note starting) is left alone.
//
// Repair: the samples of the click are rebuilt from both sides. An all-pole model is
// fitted (Burg's method) to the 4 ms before and, backwards, to the 4 ms after; each
// predicts across the gap and the two are crossfaded. The audio is delayed 8 ms so the
// "after" side is there in time. If the rebuilt samples aren't calmer than the click, the
// original is kept.
class ClickRemover
{
public:
    static constexpr int maxChannels = 2;
    static constexpr double latencySeconds = 0.010, contextSeconds = 0.006;
    static constexpr double longestClickSeconds = 0.002, marginSeconds = 0.00025, gapSeconds = 0.0003;

    // Not real-time safe (allocates).
    void prepare (double rate, int numChannels)
    {
        sampleRate = rate;
        channels = juce::jlimit (1, maxChannels, numChannels);
        latency = (int) std::lround (latencySeconds * rate);
        context = juce::jmax (32, (int) std::lround (contextSeconds * rate));
        longest = juce::jmax (4, (int) std::lround (longestClickSeconds * rate));
        margin = juce::jmax (2, (int) std::lround (marginSeconds * rate));
        gap = juce::jmax (2, (int) std::lround (gapSeconds * rate));
        order = juce::jlimit (12, 48, (int) std::lround (24.0 * rate / 48000.0));

        auto size = 1;
        while (size < latency + context + longest + 4 * margin + 16)
            size <<= 1;
        mask = size - 1;
        for (auto& ring : rings)
            ring.assign ((size_t) size, 0.0f);

        maxLength = longest + 2 * margin + 4;
        for (auto* v : { &burgF, &burgB, &before, &after })
            v->assign ((size_t) context, 0.0);
        for (auto* v : { &segment, &residual })
            v->assign ((size_t) (maxLength + 2 * order), 0.0);
        for (auto* v : { &coefficients, &coefficientsAfter, &autocorrelation })
            v->assign ((size_t) order + 1, 0.0);
        solution.assign ((size_t) maxLength, 0.0);
        matrix.assign ((size_t) (maxLength * maxLength), 0.0);
        saved.assign ((size_t) (maxLength * maxChannels), 0.0f);

        const auto perSample = [rate] (double seconds) { return (float) (1.0 - std::exp (-1.0 / (seconds * rate))); };
        backgroundA = perSample (0.008);
        peakBlock = juce::jmax (8, (int) std::lround (0.002 * rate));
        fastA = perSample (0.001);
        reset();
    }

    void reset() noexcept
    {
        for (auto& ring : rings)
            std::fill (ring.begin(), ring.end(), 0.0f);
        written = 0;
        m1 = m2 = 0.0f;
        background = fast = 0.0f;
        inSpan = false;
        pendingCount = 0;
        warm = 0;
        peaks.fill (0.0f);
        peakSlot = 0;
        inPeakBlock = 0;
        recentPeak = 0.0f;
    }

    // sensitivity 0..1: how far over the background a spike must jump (12x .. 4x), and
    // over the sharpest peak of the 2-24 ms before it (3x .. 1.5x).
    void setSensitivity (float sensitivity) noexcept
    {
        const auto s = juce::jlimit (0.0f, 1.0f, sensitivity);
        threshold = 12.0f - 8.0f * s;
        peakRatio = 3.0f - 1.5f * s;
    }

    int getLatencySamples() const noexcept { return latency; }

    void process (juce::dsp::AudioBlock<float>& block) noexcept
    {
        const auto numChannels = juce::jmin ((int) block.getNumChannels(), channels);
        if (numChannels == 0)
            return;
        active = numChannels;

        for (size_t i = 0; i < block.getNumSamples(); ++i)
        {
            auto mono = 0.0f;
            for (int ch = 0; ch < numChannels; ++ch)
            {
                const auto x = block.getSample (ch, (int) i);
                rings[(size_t) ch][(size_t) (written & mask)] = x;
                mono += x;
            }
            detect (mono / (float) numChannels);

            if (pendingCount > 0 && written >= pending[0].end + context)
            {
                repair (pending[0]);
                std::copy (pending.begin() + 1, pending.begin() + pendingCount, pending.begin());
                --pendingCount;
            }

            const auto out = written - latency;
            for (int ch = 0; ch < numChannels; ++ch)
                block.setSample (ch, (int) i, out >= 0 ? rings[(size_t) ch][(size_t) (out & mask)] : 0.0f);

            ++written;
        }
    }

    // Clicks fixed since the last call. Any thread.
    int takeClicksFixed() noexcept { return clicksFixed.exchange (0, std::memory_order_relaxed); }

private:
    struct Span { juce::int64 start = 0, end = 0; float background = 0.0f; };

    void detect (float mono) noexcept
    {
        const auto h = mono - 2.0f * m1 + m2;
        m2 = m1;
        m1 = mono;
        const auto e = h * h;
        fast += fastA * (e - fast);

        // The sharpest peak of each 2 ms, kept for 24 ms. A voice's own sharp edges come
        // back every pitch period, so they're in there; a click is on its own.
        peaks[(size_t) peakSlot] = juce::jmax (peaks[(size_t) peakSlot], std::abs (h));
        if (++inPeakBlock >= peakBlock)
        {
            inPeakBlock = 0;
            peakSlot = (peakSlot + 1) % (int) peaks.size();
            peaks[(size_t) peakSlot] = 0.0f;
            // The slot now filling and the one just finished (it may hold this very click)
            // don't count.
            const auto justDone = (peakSlot + (int) peaks.size() - 1) % (int) peaks.size();
            recentPeak = 0.0f;
            for (int i = 0; i < (int) peaks.size(); ++i)
                if (i != peakSlot && i != justDone)
                    recentPeak = juce::jmax (recentPeak, peaks[(size_t) i]);
        }

        if (warm < context)
        {
            ++warm;
            background += backgroundA * (e - background);
            return;
        }

        const auto exceeds = std::abs (h) > threshold * std::sqrt (background) && std::abs (h) > peakRatio * recentPeak
                             && std::abs (h) > absoluteFloor;

        if (! inSpan)
        {
            if (exceeds)
            {
                inSpan = true;
                spanStart = lastExceed = written;
                spanBackground = background;
            }
            else
                background += backgroundA * (e - background);
            return;
        }

        if (exceeds)
            lastExceed = written;

        if (written - spanStart > longest)
        {
            // Too long for a click: a consonant or a note starting. Follow it.
            inSpan = false;
            background = fast;
        }
        else if (written - lastExceed > gap)
        {
            inSpan = false;
            if (pendingCount < (int) pending.size())
                pending[(size_t) pendingCount++] = { spanStart - margin - 2, lastExceed + margin, spanBackground };
            background += backgroundA * (e - background);
        }
    }

    float ringAt (int ch, juce::int64 index) const noexcept { return rings[(size_t) ch][(size_t) (index & mask)]; }

    // Mean square of the second difference over [from, to] of the mono signal in the ring.
    double brightness (juce::int64 from, juce::int64 to) const noexcept
    {
        const auto monoAt = [this] (juce::int64 index)
        {
            auto sum = 0.0f;
            for (int ch = 0; ch < active; ++ch)
                sum += ringAt (ch, index);
            return sum / (float) active;
        };
        auto total = 0.0;
        for (auto n = from; n <= to; ++n)
        {
            const auto h = monoAt (n) - 2.0f * monoAt (n - 1) + monoAt (n - 2);
            total += (double) h * h;
        }
        return total / (double) std::max<juce::int64> (1, to - from + 1);
    }

    void repair (const Span& span) noexcept
    {
        const auto length = (int) (span.end - span.start + 1);
        if (length <= 0 || length > maxLength)
            return;

        // A click is a spike on its own: afterwards the sound is back to what it was.
        const auto afterwards = brightness (span.end + 1, span.end + context);
        const auto limit = 0.25 * threshold * threshold * juce::jmax (1.0e-12, (double) span.background);
        if (afterwards > limit)
            return;
        // ...and the sound doesn't just stop there (a cut to silence isn't a click).
        if (afterwards < 0.02 * span.background && span.background > absoluteFloor * absoluteFloor)
            return;

        const auto clickBrightness = brightness (span.start, span.end);
        for (int ch = 0; ch < active; ++ch)
        {
            for (int n = 0; n < length; ++n)
                saved[(size_t) (ch * length + n)] = ringAt (ch, span.start + n);
            interpolate (ch, span.start, length);
        }

        if (brightness (span.start, span.end) >= clickBrightness)
        {
            for (int ch = 0; ch < active; ++ch)
                for (int n = 0; n < length; ++n)
                    rings[(size_t) ch][(size_t) ((span.start + n) & mask)] = saved[(size_t) (ch * length + n)];
            return;
        }

        clicksFixed.fetch_add (1, std::memory_order_relaxed);
    }

    // Fits an all-pole model to x[0..n) with Burg's method; a[0] = 1, a[1..order].
    void fit (const double* x, int n, double* a) noexcept
    {
        std::fill (a, a + order + 1, 0.0);
        a[0] = 1.0;

        std::copy (x, x + n, burgF.begin());
        std::copy (x, x + n, burgB.begin());

        auto denominator = 0.0;
        for (int j = 0; j < n; ++j)
            denominator += 2.0 * x[j] * x[j];
        denominator -= x[0] * x[0] + x[n - 1] * x[n - 1];

        for (int k = 0; k < order && k < n - 2 && denominator > 1.0e-18; ++k)
        {
            auto mu = 0.0;
            for (int j = 0; j < n - k - 1; ++j)
                mu += burgF[(size_t) (j + k + 1)] * burgB[(size_t) j];
            mu *= -2.0 / denominator;

            for (int j = 0; j <= (k + 1) / 2; ++j)
            {
                const auto t1 = a[j] + mu * a[k + 1 - j];
                const auto t2 = a[k + 1 - j] + mu * a[j];
                a[j] = t1;
                a[k + 1 - j] = t2;
            }

            for (int j = 0; j < n - k - 1; ++j)
            {
                const auto t1 = burgF[(size_t) (j + k + 1)] + mu * burgB[(size_t) j];
                const auto t2 = burgB[(size_t) j] + mu * burgF[(size_t) (j + k + 1)];
                burgF[(size_t) (j + k + 1)] = t1;
                burgB[(size_t) j] = t2;
            }

            denominator = (1.0 - mu * mu) * denominator - burgF[(size_t) (k + 1)] * burgF[(size_t) (k + 1)]
                          - burgB[(size_t) (n - k - 2)] * burgB[(size_t) (n - k - 2)];
        }
    }

    // Rebuilds ring samples [start, start + length) of one channel: least-squares AR
    // interpolation (the missing samples that make the model's prediction error smallest
    // across the gap and `order` samples either side). The model is the average of one fitted
    // before the gap and one fitted (backwards) after it.
    void interpolate (int ch, juce::int64 start, int length) noexcept
    {
        for (int n = 0; n < context; ++n)
        {
            before[(size_t) n] = ringAt (ch, start - context + n);
            after[(size_t) n] = ringAt (ch, start + length - 1 + context - n); // reversed
        }
        fit (before.data(), context, coefficients.data());
        fit (after.data(), context, coefficientsAfter.data());
        for (int k = 0; k <= order; ++k)
            coefficients[(size_t) k] = 0.5 * (coefficients[(size_t) k] + coefficientsAfter[(size_t) k]);
        const auto* a = coefficients.data();

        // The segment: `order` known samples, the gap, `order` known samples.
        const auto p = order, total = length + 2 * p;
        auto* x = segment.data();
        for (int n = 0; n < total; ++n)
            x[n] = (n >= p && n < p + length) ? 0.0 : ringAt (ch, start - p + n);

        // Prediction error with the gap at zero, rows t = p .. total-1.
        auto* r = residual.data();
        for (int t = p; t < total; ++t)
        {
            auto e = 0.0;
            for (int k = 0; k <= p; ++k)
                e += a[k] * x[t - k];
            r[t] = e;
        }

        // Normal equations R u = -b, R[i][j] = sum_k a_k a_(k+|i-j|) (banded Toeplitz).
        for (int k = 0; k <= p; ++k)
        {
            auto sum = 0.0;
            for (int j = 0; j + k <= p; ++j)
                sum += a[j] * a[j + k];
            autocorrelation[(size_t) k] = sum;
        }

        auto* m = matrix.data();
        auto* u = solution.data();
        for (int i = 0; i < length; ++i)
        {
            for (int j = 0; j < length; ++j)
            {
                const auto lag = std::abs (i - j);
                m[i * length + j] = lag <= p ? autocorrelation[(size_t) lag] : 0.0;
            }
            m[i * length + i] += 1.0e-9; // keeps it positive definite on silence

            auto b = 0.0;
            const auto column = p + i;
            for (int k = 0; k <= p; ++k)
                b += a[k] * r[column + k];
            u[i] = -b;
        }

        // Cholesky, in place (lower triangle), then the two triangular solves.
        for (int j = 0; j < length; ++j)
        {
            auto d = m[j * length + j];
            for (int k = 0; k < j; ++k)
                d -= m[j * length + k] * m[j * length + k];
            if (d <= 0.0)
                return; // leave the samples as they were
            d = std::sqrt (d);
            m[j * length + j] = d;
            for (int i = j + 1; i < length; ++i)
            {
                auto v = m[i * length + j];
                for (int k = 0; k < j; ++k)
                    v -= m[i * length + k] * m[j * length + k];
                m[i * length + j] = v / d;
            }
        }
        for (int i = 0; i < length; ++i)
        {
            auto v = u[i];
            for (int k = 0; k < i; ++k)
                v -= m[i * length + k] * u[k];
            u[i] = v / m[i * length + i];
        }
        for (int i = length - 1; i >= 0; --i)
        {
            auto v = u[i];
            for (int k = i + 1; k < length; ++k)
                v -= m[k * length + i] * u[k];
            u[i] = v / m[i * length + i];
        }

        for (int n = 0; n < length; ++n)
            rings[(size_t) ch][(size_t) ((start + n) & mask)] = (float) u[n];
    }

    static constexpr float absoluteFloor = 3.0e-4f; // about -70 dB

    double sampleRate = 48000.0;
    int channels = 1, active = 1, latency = 384, context = 192, longest = 96, margin = 12, gap = 14, order = 16, mask = 0;
    std::array<std::vector<float>, maxChannels> rings;
    std::vector<double> burgF, burgB, before, after, segment, residual, coefficients, coefficientsAfter, autocorrelation, solution, matrix;
    std::vector<float> saved;
    int maxLength = 124;
    juce::int64 written = 0, spanStart = 0, lastExceed = 0;
    float m1 = 0.0f, m2 = 0.0f, background = 0.0f, fast = 0.0f, spanBackground = 0.0f;
    float backgroundA = 0.003f, fastA = 0.02f, threshold = 8.0f, peakRatio = 2.25f, recentPeak = 0.0f;
    std::array<float, 12> peaks {};
    int peakSlot = 0, inPeakBlock = 0, peakBlock = 96;
    bool inSpan = false;
    int warm = 0;
    std::array<Span, 8> pending {};
    int pendingCount = 0;
    std::atomic<int> clicksFixed { 0 };
};

namespace modules
{
//==============================================================================
class Breath final : public Module
{
public:
    Breath() : Module (ModuleType::breath) {}

    Value& on          = addSwitch ("breathOn");
    Value& reduction   = addValue ("breathReduction", 12.0f);  // dB
    Value& sensitivity = addValue ("breathSensitivity", 50.0f); // %

    int getLatencySamples() const noexcept override { return flag (on) ? latency.load (std::memory_order_relaxed) : 0; }

    // reduction: the most it turned a breath down since the last reading (dB);
    // breaths: how many it found since then.
    void addReadings (juce::DynamicObject& readings) override
    {
        readings.setProperty ("reduction", std::round (tamer.takeReductionDb() * 10.0f) / 10.0f);
        readings.setProperty ("breaths", tamer.takeBreaths());
    }

private:
    void prepareModule (const juce::dsp::ProcessSpec& spec) override
    {
        tamer.prepare (spec.sampleRate, (int) spec.numChannels);
        latency.store (tamer.getLatencySamples(), std::memory_order_relaxed);
    }

    void resetModule() override { tamer.reset(); }
    void update() noexcept override { tamer.setParameters (get (reduction), fraction (sensitivity)); }

    void render (const Context& c) noexcept override
    {
        auto block = c.getOutputBlock();
        tamer.process (block);
    }

    BreathTamer tamer;
    std::atomic<int> latency { BreathTamer::delayFrames * 240 };
};

//==============================================================================
class Declick final : public Module
{
public:
    Declick() : Module (ModuleType::declick) {}

    Value& on          = addSwitch ("declickOn");
    Value& sensitivity = addValue ("declickSensitivity", 50.0f); // %

    int getLatencySamples() const noexcept override { return flag (on) ? latency.load (std::memory_order_relaxed) : 0; }

    // clicks: how many it fixed since the last reading.
    void addReadings (juce::DynamicObject& readings) override
    {
        readings.setProperty ("clicks", remover.takeClicksFixed());
    }

private:
    void prepareModule (const juce::dsp::ProcessSpec& spec) override
    {
        remover.prepare (spec.sampleRate, (int) spec.numChannels);
        latency.store (remover.getLatencySamples(), std::memory_order_relaxed);
    }

    void resetModule() override { remover.reset(); }
    void update() noexcept override { remover.setSensitivity (fraction (sensitivity)); }

    void render (const Context& c) noexcept override
    {
        auto block = c.getOutputBlock();
        remover.process (block);
    }

    ClickRemover remover;
    std::atomic<int> latency { 384 };
};
}
}
