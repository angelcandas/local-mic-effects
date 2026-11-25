#pragma once

#include "FxModule.h"

#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <memory>
#include <vector>

namespace fx
{
//==============================================================================
// Room echo reduction for a recorded voice: turns down the late reverb of the room it was
// recorded in, the boxy "bathroom" tail between and under the words.
//
// Short-time FFT (~21 ms frames, 75 % overlap). A room's late reverb is, per bin, the power
// of the sound some 50 ms earlier, decayed by the room: e^(-2 delta T) with
// delta = 3 ln 10 / RT60 (energy falls 60 dB in RT60 seconds). Each bin is turned down by
// how much of it that predicts (a decision-directed Wiener gain, as in the denoiser, which
// keeps "musical" warbles down), never further than Amount allows. The direct sound and
// the first reflections (the 50 ms) stay, so the voice keeps its body.
//
// RT60 is set by Decay, or found by itself (Auto): where the voice stops, the level falls
// at the room's rate. Every stretch of falling level (at least 60 ms and 10 dB, above the
// noise) gives a slope; the room is about the quickest of them (the voice itself can only
// fade slower than the room lets it), so the estimate is the lower quartile of the recent
// ones, gliding. The same falls tell how much of the sound is echo (how far the level drops
// the moment the voice stops), which scales the estimate, so a fairly dry voice is barely
// touched.
//
// Latency is one frame (1024 samples at 44.1/48 kHz, 2048 above 64 kHz): DAW only.
class Dereverb
{
public:
    static constexpr int maxChannels = 2;
    static constexpr int numBands = 32;          // the reduction published to the UI
    static constexpr double lateSeconds = 0.05;  // where the late reverb starts
    static constexpr float minDecay = 0.15f, maxDecay = 4.0f, startDecay = 0.6f;

    static constexpr int getFrameSize (double rate) noexcept { return rate > 64000.0 ? 2048 : 1024; }

    // Not real-time safe (allocates).
    void prepare (double rate, int numChannels)
    {
        sampleRate = rate;
        channels = juce::jlimit (1, maxChannels, numChannels);
        const auto size = getFrameSize (rate);

        if (fft == nullptr || size != fftSize)
        {
            fft = std::make_unique<juce::dsp::FFT> (juce::roundToInt (std::log2 (size)));
            fftSize = size;
            hop = fftSize / 4;
            numBins = fftSize / 2 + 1;

            window.resize ((size_t) fftSize);
            for (int i = 0; i < fftSize; ++i)
                window[(size_t) i] = std::sqrt (0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) fftSize));

            for (auto& channel : state)
            {
                channel.input.assign ((size_t) fftSize, 0.0f);
                channel.accumulator.assign ((size_t) fftSize, 0.0f);
                channel.output.assign ((size_t) hop, 0.0f);
                channel.spectrum.assign ((size_t) fftSize * 2, 0.0f);
            }

            for (auto* bins : { &power, &smoothed, &previousGain, &previousRatio, &gain, &scratch })
                bins->assign ((size_t) numBins, 0.0f);
        }

        const auto frameRate = rate / hop;
        lateFrames = juce::jmax (1, (int) std::lround (lateSeconds * frameRate));
        history.assign ((size_t) ((lateFrames + 1) * numBins), 0.0f);
        smoothingCoefficient = (float) std::exp (-1.0 / (0.012 * frameRate));
        decayGlide = (float) (1.0 - std::exp (-1.0 / (2.0 * frameRate)));

        const auto binHz = rate / fftSize;
        lowBin = juce::jmax (1, (int) std::lround (300.0 / binHz));
        highBin = juce::jmin (numBins - 1, (int) std::lround (4000.0 / binHz));
        for (int b = 0; b <= numBands; ++b)
        {
            const auto hz = 60.0 * std::pow (16000.0 / 60.0, (double) b / numBands);
            bandEdges[(size_t) b] = juce::jlimit (1, numBins - 1, (int) std::lround (hz / binHz));
        }

        minRunFrames = juce::jmax (4, (int) std::lround (0.06 * frameRate));
        skipFrames = fftSize / hop + 1; // the frame has to be clear of the direct sound
        floorWindowFrames = juce::jmax (8, (int) std::lround (2.0 * frameRate));
        reset();
    }

    void reset() noexcept
    {
        for (auto& channel : state)
        {
            std::fill (channel.input.begin(), channel.input.end(), 0.0f);
            std::fill (channel.accumulator.begin(), channel.accumulator.end(), 0.0f);
            std::fill (channel.output.begin(), channel.output.end(), 0.0f);
        }
        std::fill (history.begin(), history.end(), 0.0f);
        std::fill (smoothed.begin(), smoothed.end(), 0.0f);
        std::fill (previousGain.begin(), previousGain.end(), 1.0f);
        std::fill (previousRatio.begin(), previousRatio.end(), 0.0f);
        std::fill (gain.begin(), gain.end(), 1.0f);
        position = 0;
        historyFrame = 0;
        framesSeen = 0;
        runLength = 0;
        previousLevel = -200.0f;
        floorDb = 0.0f;
        floorCandidate = 0.0f;
        floorFrames = 0;
        estimateCount = 0;
        estimateNext = 0;
        // The decay found so far is kept: it belongs to the recording, not to the moment.
    }

    // amount 0..1: how hard the late reverb is turned down. decaySeconds: the room's RT60
    // when auto is off.
    void setParameters (float amount, bool automatic, float decaySeconds) noexcept
    {
        const auto a = juce::jlimit (0.0f, 1.0f, amount);
        overEstimate = 0.5f + 1.5f * a;
        floorGain = juce::Decibels::decibelsToGain (-(2.0f + 22.0f * a));
        autoDecay = automatic;
        manualDecay = juce::jlimit (minDecay, maxDecay, decaySeconds);
    }

    int getLatencySamples() const noexcept { return fftSize; }

    // The RT60 in use (seconds). Any thread.
    float getDecaySeconds() const noexcept { return decayInUse.load (std::memory_order_relaxed); }

    void process (juce::dsp::AudioBlock<float>& block) noexcept
    {
        const auto numChannels = juce::jmin ((int) block.getNumChannels(), channels);
        if (numChannels == 0)
            return;

        for (size_t i = 0; i < block.getNumSamples(); ++i)
        {
            for (int ch = 0; ch < numChannels; ++ch)
            {
                auto& channel = state[(size_t) ch];
                auto* data = block.getChannelPointer ((size_t) ch);
                channel.input[(size_t) (fftSize - hop + position)] = data[i];
                data[i] = channel.output[(size_t) position];
            }

            if (++position == hop)
            {
                position = 0;
                processFrame (numChannels);
            }
        }
    }

    std::array<std::atomic<float>, numBands> reductionBandsDb {};

private:
    struct Channel
    {
        std::vector<float> input, accumulator, output, spectrum;
    };

    void processFrame (int numChannels) noexcept
    {
        std::fill (power.begin(), power.end(), 0.0f);

        for (int ch = 0; ch < numChannels; ++ch)
        {
            auto& channel = state[(size_t) ch];
            auto* spectrum = channel.spectrum.data();
            for (int i = 0; i < fftSize; ++i)
                spectrum[i] = channel.input[(size_t) i] * window[(size_t) i];
            std::fill (spectrum + fftSize, spectrum + 2 * fftSize, 0.0f);
            fft->performRealOnlyForwardTransform (spectrum, true);

            for (int k = 0; k < numBins; ++k)
                power[(size_t) k] += (spectrum[2 * k] * spectrum[2 * k] + spectrum[2 * k + 1] * spectrum[2 * k + 1]) / (float) numChannels;
        }

        trackDecay();
        updateGains();

        for (int ch = 0; ch < numChannels; ++ch)
        {
            auto& channel = state[(size_t) ch];
            auto* spectrum = channel.spectrum.data();
            for (int k = 0; k < numBins; ++k)
            {
                spectrum[2 * k] *= gain[(size_t) k];
                spectrum[2 * k + 1] *= gain[(size_t) k];
            }
            for (int k = numBins; k < fftSize; ++k)
            {
                spectrum[2 * k] = spectrum[2 * (fftSize - k)];
                spectrum[2 * k + 1] = -spectrum[2 * (fftSize - k) + 1];
            }
            fft->performRealOnlyInverseTransform (spectrum);

            auto& accumulator = channel.accumulator;
            for (int i = 0; i < fftSize; ++i)
                accumulator[(size_t) i] += spectrum[i] * window[(size_t) i] * 0.5f;

            std::copy (accumulator.begin(), accumulator.begin() + hop, channel.output.begin());
            std::copy (accumulator.begin() + hop, accumulator.end(), accumulator.begin());
            std::fill (accumulator.end() - hop, accumulator.end(), 0.0f);
            std::copy (channel.input.begin() + hop, channel.input.end(), channel.input.begin());
        }

        publishBands();
    }

    //==========================================================================
    // RT60 from the falling stretches of the 300 Hz - 4 kHz level.
    void trackDecay() noexcept
    {
        auto sum = 0.0;
        for (int k = lowBin; k <= highBin; ++k)
            sum += power[(size_t) k];
        const auto level = (float) (10.0 * std::log10 (sum + 1.0e-20));
        ++framesSeen;

        // The noise floor: the lowest level over the last 2-4 s.
        if (framesSeen == 1)
            floorDb = floorCandidate = level;
        floorDb = juce::jmin (floorDb, level);
        floorCandidate = juce::jmin (floorCandidate, level);
        if (++floorFrames >= floorWindowFrames)
        {
            floorFrames = 0;
            floorDb = floorCandidate;
            floorCandidate = level;
        }

        // A stretch: from a peak on, while the level never climbs 2 dB over its lowest so far.
        const auto audible = level > floorDb + 6.0f;
        if (runLength > 0 && audible && level < runMin + 2.0f && runLength < (int) run.size())
        {
            run[(size_t) runLength++] = level;
            runMin = juce::jmin (runMin, level);
        }
        else
        {
            finishRun();
            runLength = 0;
            if (audible)
            {
                run[(size_t) runLength++] = level;
                runMin = level;
            }
        }
        previousLevel = level;

        if (autoDecay)
        {
            auto target = startDecay;
            if (estimateCount > 0)
                target = quantile (estimates, 0.35f);
            foundDecay += decayGlide * (target - foundDecay);
            decay = foundDecay;
        }
        else
            decay = manualDecay;

        echoShare += decayGlide * ((estimateCount > 0 ? quantile (shares, 0.5f) : startShare) - echoShare);

        decayInUse.store (decay, std::memory_order_relaxed);
    }

    float quantile (const std::array<float, 24>& values, float q) const noexcept
    {
        std::array<float, 24> sorted {};
        std::copy (values.begin(), values.begin() + estimateCount, sorted.begin());
        std::sort (sorted.begin(), sorted.begin() + estimateCount);
        return sorted[(size_t) juce::roundToInt (q * (float) (estimateCount - 1))];
    }

    void finishRun() noexcept
    {
        if (runLength < minRunFrames + skipFrames)
            return;

        // The fall starts where the level leaves its top (a held note comes first), and the
        // first frame's worth after that is left out: the direct sound stopping falls faster
        // than any room.
        auto top = 0;
        for (int i = 1; i < runLength; ++i)
            if (run[(size_t) i] > run[(size_t) top])
                top = i;
        auto start = top;
        for (int i = top; i < runLength; ++i)
            if (run[(size_t) i] >= run[(size_t) top] - 1.5f)
                start = i;

        const auto first = start + skipFrames, count = runLength - first;
        if (count < minRunFrames || run[(size_t) first] - run[(size_t) (runLength - 1)] < 10.0f)
            return;

        // Least-squares slope, dB per frame.
        auto meanX = 0.0, meanY = 0.0;
        for (int i = 0; i < count; ++i)
        {
            meanX += i;
            meanY += run[(size_t) (first + i)];
        }
        meanX /= count;
        meanY /= count;
        auto sxy = 0.0, sxx = 0.0;
        for (int i = 0; i < count; ++i)
        {
            sxy += (i - meanX) * (run[(size_t) (first + i)] - meanY);
            sxx += (i - meanX) * (i - meanX);
        }
        const auto slopePerSecond = sxy / juce::jmax (1.0e-9, sxx) * (sampleRate / hop);
        if (slopePerSecond >= -1.0)
            return;

        const auto rt60 = juce::jlimit (minDecay, maxDecay, (float) (-60.0 / slopePerSecond));

        // How much of the sound before the fall was echo: the fitted line, taken back to
        // where the fall started, against the level there (the direct sound has gone).
        const auto slopePerFrame = sxy / juce::jmax (1.0e-9, sxx);
        const auto echoAtStart = meanY + slopePerFrame * (-(double) skipFrames - meanX);
        const auto share = juce::jlimit (0.05f, 1.0f, (float) std::pow (10.0, (echoAtStart - run[(size_t) start]) / 10.0));

        shares[(size_t) estimateNext] = share;
        estimates[(size_t) estimateNext] = rt60;
        estimateNext = (estimateNext + 1) % maxEstimates;
        estimateCount = juce::jmin (estimateCount + 1, maxEstimates);
    }

    //==========================================================================
    void updateGains() noexcept
    {
        // The power a lateFrames ago, smoothed a little, decayed by the room over that time.
        const auto slot = [this] (juce::int64 frame) { return (size_t) (frame % (lateFrames + 1)) * (size_t) numBins; };
        const auto* old = history.data() + slot (historyFrame + 1); // frame t - lateFrames
        const auto delta = 3.0 * std::log (10.0) / juce::jmax (minDecay, decay);
        // Only the echo part of what was heard back then is still ringing (echoShare).
        const auto decayOver = (float) std::exp (-2.0 * delta * lateFrames * hop / sampleRate) * overEstimate * echoShare;
        const auto ready = historyFrame >= lateFrames;

        for (int k = 0; k < numBins; ++k)
        {
            auto& s = smoothed[(size_t) k];
            s = smoothingCoefficient * s + (1.0f - smoothingCoefficient) * power[(size_t) k];

            const auto late = ready ? juce::jmax (old[k] * decayOver, 1.0e-20f) : 1.0e-20f;
            const auto ratio = power[(size_t) k] / late; // direct sound to late reverb, a posteriori
            const auto prior = juce::jmax (0.003f, 0.96f * previousGain[(size_t) k] * previousGain[(size_t) k] * previousRatio[(size_t) k]
                                                     + 0.04f * juce::jmax (ratio - 1.0f, 0.0f));
            scratch[(size_t) k] = prior / (1.0f + prior);
            previousRatio[(size_t) k] = ratio;
        }

        auto* write = history.data() + slot (historyFrame); // frame t lives in slot t (mod lateFrames + 1)
        std::copy (smoothed.begin(), smoothed.end(), write);
        ++historyFrame;

        for (int k = 0; k < numBins; ++k)
        {
            const auto below = scratch[(size_t) juce::jmax (0, k - 1)];
            const auto above = scratch[(size_t) juce::jmin (numBins - 1, k + 1)];
            const auto wiener = 0.25f * below + 0.5f * scratch[(size_t) k] + 0.25f * above;
            previousGain[(size_t) k] = wiener;
            gain[(size_t) k] = floorGain + (1.0f - floorGain) * wiener;
        }
    }

    void publishBands() noexcept
    {
        for (int b = 0; b < numBands; ++b)
        {
            const auto first = bandEdges[(size_t) b];
            const auto last = juce::jmax (first + 1, bandEdges[(size_t) b + 1]);
            auto sum = 0.0f;
            for (int k = first; k < last; ++k)
                sum += gain[(size_t) k];
            reductionBandsDb[(size_t) b].store (-juce::Decibels::gainToDecibels (sum / (float) (last - first), -100.0f),
                                                std::memory_order_relaxed);
        }
    }

    static constexpr int maxEstimates = 24;

    double sampleRate = 48000.0;
    int channels = 1, fftSize = 0, hop = 256, numBins = 0, position = 0;
    std::unique_ptr<juce::dsp::FFT> fft;
    std::vector<float> window;
    std::array<Channel, maxChannels> state;
    std::vector<float> power, smoothed, previousGain, previousRatio, gain, scratch, history;
    std::array<int, numBands + 1> bandEdges {};
    int lateFrames = 9, lowBin = 6, highBin = 85;
    juce::int64 historyFrame = 0, framesSeen = 0;
    float smoothingCoefficient = 0.9f, overEstimate = 1.25f, floorGain = 0.25f;
    bool autoDecay = true;
    float manualDecay = startDecay, decay = startDecay, foundDecay = startDecay, decayGlide = 0.01f;

    std::array<float, 256> run {};
    int runLength = 0, minRunFrames = 11, skipFrames = 3, floorWindowFrames = 375, floorFrames = 0;
    float runMin = 0.0f, previousLevel = -200.0f, floorDb = 0.0f, floorCandidate = 0.0f;
    std::array<float, maxEstimates> estimates {}, shares {};
    static constexpr float startShare = 0.4f;
    float echoShare = startShare;
    int estimateCount = 0, estimateNext = 0;
    std::atomic<float> decayInUse { startDecay };
};

namespace modules
{
//==============================================================================
class DereverbModule final : public Module
{
public:
    DereverbModule() : Module (ModuleType::dereverb) {}

    Value& on        = addSwitch ("dereverbOn");
    Value& amount    = addValue ("dereverbAmount", 50.0f);  // %
    Value& automatic = addChoice ("dereverbAuto", 1.0f);
    Value& decay     = addValue ("dereverbDecay", 0.6f);    // s, used when Auto is off

    int getLatencySamples() const noexcept override { return flag (on) ? latency.load (std::memory_order_relaxed) : 0; }

    // reduction: how far each band (60 Hz-16 kHz, log-spaced) is turned down now (dB);
    // decay: the room's RT60 in use (s).
    void addReadings (juce::DynamicObject& readings) override
    {
        juce::Array<juce::var> cut;
        for (int b = 0; b < Dereverb::numBands; ++b)
            cut.add (std::round (dereverb.reductionBandsDb[(size_t) b].load (std::memory_order_relaxed) * 10.0f) / 10.0f);
        readings.setProperty ("reduction", cut);
        readings.setProperty ("decay", std::round (dereverb.getDecaySeconds() * 100.0f) / 100.0f);
    }

    const Dereverb& getDereverb() const noexcept { return dereverb; }

private:
    void prepareModule (const juce::dsp::ProcessSpec& spec) override
    {
        dereverb.prepare (spec.sampleRate, (int) spec.numChannels);
        latency.store (dereverb.getLatencySamples(), std::memory_order_relaxed);
    }

    void resetModule() override { dereverb.reset(); }
    void update() noexcept override { dereverb.setParameters (fraction (amount), flag (automatic), get (decay)); }

    void render (const Context& c) noexcept override
    {
        auto block = c.getOutputBlock();
        dereverb.process (block);
    }

    Dereverb dereverb;
    std::atomic<int> latency { Dereverb::getFrameSize (48000.0) };
};
}
}
