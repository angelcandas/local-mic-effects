#pragma once

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
// Frequency-domain noise reduction (in the spirit of iZotope's Voice De-noise): the
// signal is split into ~250 frequency bins with a short-time FFT, the noise power in
// every bin is estimated statistically, and each bin is turned down by how much of it
// looks like noise. Steady hiss, hum and fan noise disappear between and *under* the
// words, which a broadband gate can't do.
//
// Noise estimate, per bin:
//   - Adaptive: MCRA (Cohen & Berdugo, "minima controlled recursive averaging"). A bin
//     whose smoothed power sits well above its recent minimum is probably voice; the
//     noise estimate only follows the bins (and moments) that aren't. Its rise is rate
//     limited (4 dB/s) and the minimum is taken over 2.5-5 s, so a long sustained note
//     isn't learnt as noise.
//   - Starting point: with no profile yet, the first half second sets one from the
//     quietest moments of every bin (their minimum, not their average, so starting in
//     the middle of a phrase doesn't learn the voice as noise).
//   - Learn: while on, every frame is averaged into the noise profile (play a few
//     seconds of room noise only). When it's switched off the profile is kept, and with
//     Adaptive on it becomes the starting point the tracker refines.
//   - Adaptive off and not learning: the current profile is frozen.
//
// Gain, per bin: decision-directed Wiener filter (Ephraim & Malah), which keeps the
// "musical noise" of plain spectral subtraction down, smoothed across neighbouring bins
// and never lower than the reduction floor.
//
// Latency is one FFT frame: 512 samples (10.7 ms at 48 kHz, 11.6 ms at 44.1 kHz), 1024
// above 64 kHz so the frame stays ~11 ms. Frames overlap by 75%. Up to two channels;
// they share one set of gains (from their average power) so a stereo image stays put.
//
// The noise profile survives reset() and a prepare() at a rate with the same frame
// size, so switching the effect off and on doesn't make you learn the room again.
// copyProfile() / seedProfile() carry it over to a new instance (a DAW track's chain is
// rebuilt when its inserts change).
class SpectralDenoiser
{
public:
    static constexpr int numBands = 32; // resolution of the analysis published to the UI

    static constexpr int getFrameSize (double rate) noexcept { return rate > 64000.0 ? 1024 : 512; }

    // Not real-time safe (allocates). Call before processing starts.
    void prepare (const juce::dsp::ProcessSpec& spec)
    {
        sampleRate = spec.sampleRate;
        const auto size = getFrameSize (sampleRate);

        if (fft == nullptr || size != fftSize)
            allocate (size);

        // Per-frame time constants (frames arrive every `hop` samples).
        const auto frameRate = sampleRate / hop;
        const auto perFrame = [frameRate] (double seconds) { return (float) std::exp (-1.0 / (seconds * frameRate)); };
        smoothingCoefficient = perFrame (0.06);
        quickCoefficient     = perFrame (0.015);
        presenceCoefficient  = perFrame (0.02);
        noiseCoefficient     = perFrame (0.2);
        maxRisePerFrame      = juce::Decibels::decibelsToGain (maxRiseDbPerSecond / (float) frameRate, -200.0f);
        maxRisePerFrame     *= maxRisePerFrame; // power, not amplitude
        minimumWindowFrames  = juce::jmax (1, juce::roundToInt (2.5 * frameRate));
        warmupFrames         = juce::jmax (1, juce::roundToInt (0.5 * frameRate));
        settleFrames         = juce::jmax (1, juce::roundToInt (0.04 * frameRate));

        // Bins covered by each display band, log-spaced from 60 Hz to 16 kHz.
        const auto binHz = sampleRate / fftSize;
        for (int b = 0; b <= numBands; ++b)
        {
            const auto hz = bandLowHz * std::pow (bandHighHz / bandLowHz, (double) b / numBands);
            bandEdges[(size_t) b] = juce::jlimit (1, numBins - 1, (int) std::lround (hz / binHz));
        }

        // A profile handed over by seedProfile() (same frame size only: the bins must match).
        if (seed.size() == (size_t) numBins)
        {
            std::copy (seed.begin(), seed.end(), noise.begin());
            seedTrackerFromProfile();
            publishProfile();
        }

        seed.clear();
        reset();
    }

    // Clears the audio in flight but keeps the noise profile.
    void reset() noexcept
    {
        for (auto& channel : channels)
        {
            std::fill (channel.input.begin(), channel.input.end(), 0.0f);
            std::fill (channel.accumulator.begin(), channel.accumulator.end(), 0.0f);
            std::fill (channel.output.begin(), channel.output.end(), 0.0f);
        }

        std::fill (previousGain.begin(), previousGain.end(), 1.0f);
        std::fill (previousSnr.begin(), previousSnr.end(), 1.0f);
        position = 0;
        framesSinceReset = 0;
    }

    // Forgets the noise profile: the next half second sets a new one.
    void clearNoiseProfile() noexcept
    {
        hasNoiseEstimate = false;
        framesSinceMinimumReset = 0;
        learnedFrames = 0;
        warmupCount = 0;
        std::fill (noise.begin(), noise.end(), 0.0f);
        std::fill (presence.begin(), presence.end(), 0.0f);

        for (auto& band : noiseBandsDb)     band.store (-100.0f, std::memory_order_relaxed);
        for (auto& band : reductionBandsDb) band.store (0.0f, std::memory_order_relaxed);

        const juce::SpinLock::ScopedLockType lock (profileLock);
        profileValid = false;
    }

    // reductionDb: how far noise is turned down at most. thresholdDb: raises (more
    // aggressive) or lowers (gentler) the level below which a bin counts as noise,
    // relative to a default of 6 dB above the estimated noise power.
    void setParameters (float reductionDb, float thresholdDb, bool shouldAdapt, bool shouldLearn) noexcept
    {
        floorGain = juce::Decibels::decibelsToGain (-reductionDb, -200.0f);
        thresholdGain = std::pow (10.0f, (thresholdDb + noiseMarginDb) / 10.0f);
        adaptive = shouldAdapt;

        if (shouldLearn && ! learning)
        {
            learnedFrames = 0;
            learnedSeconds.store (0.0, std::memory_order_relaxed);
        }
        else if (! shouldLearn && learning && learnedFrames > 0)
            seedTrackerFromProfile();

        learning = shouldLearn;
    }

    int getLatencySamples() const noexcept { return fftSize; }
    bool isLearning() const noexcept       { return learning; }
    int getLearnedFrames() const noexcept  { return learnedFrames; }
    bool hasProfile() const noexcept       { return hasNoiseEstimate; }

    // Seconds of noise averaged in since Learn was switched on. Any thread (approximate).
    double getLearnedSeconds() const noexcept { return learnedSeconds.load (std::memory_order_relaxed); }

    // The current noise profile (power per bin), for handing over to a new instance.
    // False while there isn't one yet. Any thread; the audio thread refreshes it a few
    // times a second.
    bool copyProfile (std::vector<float>& destination) const
    {
        const juce::SpinLock::ScopedLockType lock (profileLock);

        if (! profileValid)
            return false;

        destination = profile;
        return true;
    }

    // Starts the next prepare() from this profile instead of a fresh estimate. Call before
    // the instance is shared with the audio thread.
    void seedProfile (std::vector<float> powerPerBin) { seed = std::move (powerPerBin); }

    void process (const juce::dsp::ProcessContextReplacing<float>& context) noexcept
    {
        auto& block = context.getOutputBlock();
        const auto numChannels = juce::jmin (block.getNumChannels(), channels.size());

        if (numChannels == 0)
            return;

        for (size_t i = 0; i < block.getNumSamples(); ++i)
        {
            for (size_t ch = 0; ch < numChannels; ++ch)
            {
                auto& channel = channels[ch];
                auto* data = block.getChannelPointer (ch);

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

    // Written by the audio thread once per frame, read by the UI: the noise profile (dBFS
    // of a sine with that power) and the gain reduction currently applied (dB), averaged
    // over each display band (log-spaced, bandLowHz..bandHighHz).
    std::array<std::atomic<float>, numBands> noiseBandsDb {};
    std::array<std::atomic<float>, numBands> reductionBandsDb {};

    static constexpr double bandLowHz = 60.0, bandHighHz = 16000.0;

private:
    struct Channel
    {
        std::vector<float> input;       // newest fftSize samples; the last `hop` are being filled
        std::vector<float> accumulator; // overlap-add of the processed frames
        std::vector<float> output;      // finished samples handed out over the next hop
        std::vector<float> spectrum;    // FFT work buffer (2 * fftSize)
    };

    void allocate (int size)
    {
        fft = std::make_unique<juce::dsp::FFT> (juce::roundToInt (std::log2 (size)));
        fftSize = size;
        hop = fftSize / 4;
        numBins = fftSize / 2 + 1;

        // sqrt-Hann on analysis and synthesis: their product is a Hann window, and four
        // of those spaced a quarter frame apart sum to exactly 2.
        window.resize ((size_t) fftSize);
        auto windowSum = 0.0f;
        for (int i = 0; i < fftSize; ++i)
        {
            window[(size_t) i] = std::sqrt (0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) fftSize));
            windowSum += window[(size_t) i];
        }
        overlapScale = 0.5f;
        fullScalePower = 0.25f * windowSum * windowSum; // bin power of a 0 dBFS sine

        for (auto& channel : channels)
        {
            channel.input.assign ((size_t) fftSize, 0.0f);
            channel.accumulator.assign ((size_t) fftSize, 0.0f);
            channel.output.assign ((size_t) hop, 0.0f);
            channel.spectrum.assign ((size_t) fftSize * 2, 0.0f);
        }

        for (auto* bins : { &power, &smoothed, &minimum, &candidate, &presence, &noise,
                            &previousGain, &previousSnr, &gain, &scratch, &quick, &quickMinimum })
            bins->assign ((size_t) numBins, 0.0f);

        std::fill (gain.begin(), gain.end(), 1.0f);

        {
            const juce::SpinLock::ScopedLockType lock (profileLock);
            profile.assign ((size_t) numBins, 0.0f);
        }

        clearNoiseProfile();
    }

    void processFrame (size_t numChannels) noexcept
    {
        // Analysis: windowed FFT of every channel, power averaged across them so both
        // sides of a stereo signal get the same gains.
        std::fill (power.begin(), power.end(), 0.0f);

        for (size_t ch = 0; ch < numChannels; ++ch)
        {
            auto& channel = channels[ch];
            auto* spectrum = channel.spectrum.data();

            for (int i = 0; i < fftSize; ++i)
                spectrum[i] = channel.input[(size_t) i] * window[(size_t) i];

            std::fill (spectrum + fftSize, spectrum + 2 * fftSize, 0.0f);
            fft->performRealOnlyForwardTransform (spectrum, true);

            for (int k = 0; k < numBins; ++k)
                power[(size_t) k] += (spectrum[2 * k] * spectrum[2 * k] + spectrum[2 * k + 1] * spectrum[2 * k + 1]) / (float) numChannels;
        }

        updateNoiseEstimate();
        updateGains();

        // Synthesis: apply the gains, inverse FFT, window and overlap-add.
        for (size_t ch = 0; ch < numChannels; ++ch)
        {
            auto& channel = channels[ch];
            auto* spectrum = channel.spectrum.data();

            for (int k = 0; k < numBins; ++k)
            {
                spectrum[2 * k]     *= gain[(size_t) k];
                spectrum[2 * k + 1] *= gain[(size_t) k];
            }

            // Mirror the negative frequencies (conjugate symmetric) for the real inverse.
            for (int k = numBins; k < fftSize; ++k)
            {
                spectrum[2 * k]     =  spectrum[2 * (fftSize - k)];
                spectrum[2 * k + 1] = -spectrum[2 * (fftSize - k) + 1];
            }

            fft->performRealOnlyInverseTransform (spectrum);

            auto& accumulator = channel.accumulator;
            for (int i = 0; i < fftSize; ++i)
                accumulator[(size_t) i] += spectrum[i] * window[(size_t) i] * overlapScale;

            // The first hop is now complete: hand it out, then slide everything along.
            std::copy (accumulator.begin(), accumulator.begin() + hop, channel.output.begin());
            std::copy (accumulator.begin() + hop, accumulator.end(), accumulator.begin());
            std::fill (accumulator.end() - hop, accumulator.end(), 0.0f);
            std::copy (channel.input.begin() + hop, channel.input.end(), channel.input.begin());
        }

        publishBands();

        if (++framesSincePublish >= publishEveryFrames && hasNoiseEstimate)
            publishProfile();
    }

    void updateNoiseEstimate() noexcept
    {
        // Until the input FIFO has filled up after a reset, frames are partly silence:
        // they'd drag the estimate down, so they're left out.
        if (framesSinceReset < fftSize / hop)
        {
            ++framesSinceReset;
            return;
        }

        // Power smoothed over neighbouring bins and time: the statistic MCRA tracks.
        for (int k = 0; k < numBins; ++k)
        {
            const auto below = power[(size_t) juce::jmax (0, k - 1)];
            const auto above = power[(size_t) juce::jmin (numBins - 1, k + 1)];
            scratch[(size_t) k] = 0.25f * below + 0.5f * power[(size_t) k] + 0.25f * above;
        }

        // Digital silence (a DAW track before its clip starts, a muted input) says nothing
        // about the noise: it would set a profile of zero that takes ages to climb back.
        if (*std::max_element (power.begin(), power.end()) < silentPower * fullScalePower)
            return;

        // A fresh estimate starts the smoothed power from the first frame rather than zero.
        const auto warmingUp = ! hasNoiseEstimate && ! learning;
        const auto starting = warmingUp && warmupCount == 0;

        for (int k = 0; k < numBins; ++k)
        {
            auto& s = smoothed[(size_t) k];
            s = starting ? scratch[(size_t) k] : smoothingCoefficient * s + (1.0f - smoothingCoefficient) * scratch[(size_t) k];
            minimum[(size_t) k]   = starting ? s : juce::jmin (minimum[(size_t) k], s);
            candidate[(size_t) k] = starting ? s : juce::jmin (candidate[(size_t) k], s);
        }

        // The starting profile is the minimum of a faster-smoothed power, which reaches
        // the noise floor in the short gaps between syllables. Its first few frames are
        // left out: a single frame's power can dip far below the mean.
        if (warmingUp)
        {
            const auto settling = warmupCount < settleFrames;

            for (int k = 0; k < numBins; ++k)
            {
                auto& q = quick[(size_t) k];
                q = starting ? scratch[(size_t) k] : quickCoefficient * q + (1.0f - quickCoefficient) * scratch[(size_t) k];
                quickMinimum[(size_t) k] = settling ? q : juce::jmin (quickMinimum[(size_t) k], q);
            }
        }

        // Minimum statistics over a 2.5-5 s window: every 2.5 s the running minimum
        // restarts from the minimum of the last stretch.
        if (++framesSinceMinimumReset >= minimumWindowFrames)
        {
            framesSinceMinimumReset = 0;
            for (int k = 0; k < numBins; ++k)
            {
                minimum[(size_t) k]   = juce::jmin (candidate[(size_t) k], smoothed[(size_t) k]);
                candidate[(size_t) k] = smoothed[(size_t) k];
            }
        }

        // Explicit learning: every frame is averaged into the profile.
        if (learning)
        {
            ++learnedFrames;
            learnedSeconds.store (learnedFrames * hop / sampleRate, std::memory_order_relaxed);

            for (int k = 0; k < numBins; ++k)
                noise[(size_t) k] += (power[(size_t) k] - noise[(size_t) k]) / (float) learnedFrames;

            hasNoiseEstimate = true;
            return;
        }

        // No profile yet: the minimum over the first half second (the quietest moments in
        // each bin) is the starting point.
        if (! hasNoiseEstimate)
        {
            if (++warmupCount >= warmupFrames)
            {
                for (int k = 0; k < numBins; ++k)
                    noise[(size_t) k] = quickMinimum[(size_t) k] * minimumBias;

                seedTrackerFromProfile();
            }

            return;
        }

        if (! adaptive)
            return;

        for (int k = 0; k < numBins; ++k)
        {
            // Voice is likely where the power is well above its recent minimum.
            const auto voiced = smoothed[(size_t) k] > voiceRatio * minimum[(size_t) k] ? 1.0f : 0.0f;
            auto& p = presence[(size_t) k];
            p = presenceCoefficient * p + (1.0f - presenceCoefficient) * voiced;

            // The more likely voice is present, the less the noise estimate moves.
            const auto coefficient = noiseCoefficient + (1.0f - noiseCoefficient) * p;
            auto& n = noise[(size_t) k];
            const auto updated = coefficient * n + (1.0f - coefficient) * power[(size_t) k];
            n = juce::jmin (updated, juce::jmax (n, tinyPower) * maxRisePerFrame);
        }
    }

    // Start the tracker from the current profile (after learning, or a handed-over one)
    // rather than from whatever it was following before.
    void seedTrackerFromProfile() noexcept
    {
        std::copy (noise.begin(), noise.end(), smoothed.begin());
        std::copy (noise.begin(), noise.end(), minimum.begin());
        std::copy (noise.begin(), noise.end(), candidate.begin());
        std::fill (presence.begin(), presence.end(), 0.0f);
        framesSinceMinimumReset = 0;
        hasNoiseEstimate = true;
    }

    void updateGains() noexcept
    {
        if (! hasNoiseEstimate)
        {
            std::fill (gain.begin(), gain.end(), 1.0f);
            return;
        }

        for (int k = 0; k < numBins; ++k)
        {
            const auto noisePower = juce::jmax (noise[(size_t) k] * thresholdGain, tinyPower);
            const auto snr = power[(size_t) k] / noisePower; // a posteriori SNR

            // Decision-directed a priori SNR: mostly last frame's clean-speech estimate,
            // which is what keeps isolated noise peaks from poking through as "chirps".
            const auto prior = juce::jmax (minimumPriorSnr,
                                           decisionDirected * previousGain[(size_t) k] * previousGain[(size_t) k] * previousSnr[(size_t) k]
                                               + (1.0f - decisionDirected) * juce::jmax (snr - 1.0f, 0.0f));

            scratch[(size_t) k] = prior / (1.0f + prior); // Wiener gain
            previousSnr[(size_t) k] = snr;
        }

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
            auto noiseSum = 0.0f, gainSum = 0.0f;

            for (int k = first; k < last; ++k)
            {
                noiseSum += noise[(size_t) k];
                gainSum += gain[(size_t) k];
            }

            const auto count = (float) (last - first);
            noiseBandsDb[(size_t) b].store (hasNoiseEstimate ? 10.0f * std::log10 (noiseSum / count / fullScalePower + 1.0e-10f) : -100.0f,
                                            std::memory_order_relaxed);
            reductionBandsDb[(size_t) b].store (-juce::Decibels::gainToDecibels (gainSum / count, -100.0f), std::memory_order_relaxed);
        }
    }

    // Copies the profile where copyProfile() can read it. Never waits: if another thread
    // holds the lock, it tries again next frame.
    void publishProfile() noexcept
    {
        const juce::SpinLock::ScopedTryLockType lock (profileLock);

        if (! lock.isLocked())
            return;

        std::copy (noise.begin(), noise.end(), profile.begin());
        profileValid = true;
        framesSincePublish = 0;
    }

    static constexpr float tinyPower = 1.0e-12f;
    static constexpr float voiceRatio = 5.0f;          // ~7 dB above the minimum counts as voice
    static constexpr float minimumBias = 1.5f;
    static constexpr float silentPower = 1.0e-14f;     // -140 dB, relative to a full-scale sine         // the minimum of a fluctuating power sits below its mean
    static constexpr float decisionDirected = 0.96f;
    static constexpr float minimumPriorSnr = 0.003f;   // -25 dB
    static constexpr float maxRiseDbPerSecond = 4.0f;
    static constexpr int publishEveryFrames = 32;      // ~85 ms at 48 kHz
    // Noise power fluctuates around its mean, so a bin is only treated as signal once
    // it's comfortably above it; without this margin a third of the hiss leaks through.
    static constexpr float noiseMarginDb = 6.0f;

    double sampleRate = 48000.0;
    std::unique_ptr<juce::dsp::FFT> fft;
    int fftSize = 512, hop = 128, numBins = 257, position = 0;

    std::vector<float> window;
    float overlapScale = 0.5f, fullScalePower = 1.0f;
    std::array<Channel, 2> channels;

    std::vector<float> power, smoothed, minimum, candidate, presence, noise, previousGain, previousSnr, gain, scratch;
    std::vector<float> quick, quickMinimum; // for the starting profile
    std::array<int, numBands + 1> bandEdges {};

    float smoothingCoefficient = 0.0f, quickCoefficient = 0.0f, presenceCoefficient = 0.0f, noiseCoefficient = 0.0f, maxRisePerFrame = 1.0f;
    int minimumWindowFrames = 1, framesSinceMinimumReset = 0, learnedFrames = 0;
    int warmupFrames = 1, settleFrames = 1, warmupCount = 0, framesSinceReset = 0, framesSincePublish = 0;
    bool hasNoiseEstimate = false, adaptive = true, learning = false;
    std::atomic<double> learnedSeconds { 0.0 };

    float floorGain = 0.25f, thresholdGain = 1.0f;

    // The profile as copyProfile() hands it out, and one waiting for prepare().
    mutable juce::SpinLock profileLock;
    std::vector<float> profile;
    bool profileValid = false;
    std::vector<float> seed;
};
} // namespace fx
