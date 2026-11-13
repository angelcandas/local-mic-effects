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
// Resonance suppressor: finds the narrow frequencies that stick out of the sound from
// moment to moment (a harsh note, a boxy room tone, a whistly "s", a ringing cymbal) and
// turns only those down, only while they stick out.
//
// The signal is split into frequency bins with a short-time FFT (frames overlap by 75%).
// Every frame:
//   1. Detection: each bin's power, smoothed over ~10 ms so noise doesn't flicker.
//   2. Envelope: a line fitted to the detection in dB over the bins around each one (a
//      window a fixed fraction of an octave wide, leaving out the bin's own main lobe),
//      raised to the window's mean power: the "shape" of the sound without its narrow
//      peaks, riding along the tops of a note's harmonics where the frame resolves them. Sharpness sets the window:
//      sharper is narrower, so only narrower peaks stand out from it.
//   3. Excess: how far each bin is over its envelope, minus the threshold (Focus tilts
//      the threshold towards the lows or the highs and limits it to a range of
//      frequencies). That excess is the cut, up to Depth, spread over the peak's main
//      lobe so a tone between two bins is caught whole.
//   4. Each bin's cut follows its target with its own attack and release.
// Mix blends with the untouched signal; Delta plays only what's taken out.
//
// Stereo: linked (one set of cuts from both sides, so the image stays put) or mid/side
// (the centre and the sides get their own). Up to two channels.
//
// Resolution sets the frame: fast (~5 ms), balanced (~11 ms) or quality (~21 ms, finer
// in frequency), the frame doubling at 88.2/96 kHz and again at 176.4/192 kHz so its
// length in time stays the same. Latency is one frame. All frame sizes are allocated in
// prepare(), so Resolution can change while running.
class ResonanceSuppressor
{
public:
    enum class Resolution { fast, balanced, quality };
    enum class Stereo { linked, midSide };

    static constexpr int numBands = 32; // resolution of the readings published to the UI
    static constexpr double bandLowHz = 40.0, bandHighHz = 18000.0;
    static constexpr float maxDepthDb = 24.0f;

    struct Settings
    {
        float depthDb = 9.0f;       // the deepest cut
        float thresholdDb = 6.0f;   // how far over the envelope a bin must be before it's cut
        float sharpness = 0.5f;     // 0..1: envelope window from 1 octave to 1/8 octave
        float tilt = 0.0f;          // -1..1: more sensitive in the lows (-) or highs (+), up to 6 dB
        float lowHz = 20.0f, highHz = 20000.0f; // only cut in this range (soft edges)
        float attackMs = 5.0f, releaseMs = 100.0f;
        float mix = 1.0f;           // 0..1
        bool delta = false;         // hear only what's removed
        Stereo stereo = Stereo::linked;
        Resolution resolution = Resolution::balanced;
    };

    static int getFrameSize (Resolution resolution, double rate) noexcept
    {
        const auto base = resolution == Resolution::fast ? 256 : resolution == Resolution::balanced ? 512 : 1024;
        return base * (rate > 128000.0 ? 4 : rate > 64000.0 ? 2 : 1);
    }

    // Not real-time safe (allocates). Call before processing starts.
    void prepare (const juce::dsp::ProcessSpec& spec)
    {
        sampleRate = spec.sampleRate;

        for (int r = 0; r < 3; ++r)
        {
            auto& frame = frames[(size_t) r];
            frame.size = getFrameSize ((Resolution) r, sampleRate);
            frame.fft = std::make_unique<juce::dsp::FFT> (juce::roundToInt (std::log2 (frame.size)));

            // sqrt-Hann on analysis and synthesis: their product is a Hann window, and four
            // of those spaced a quarter frame apart sum to exactly 2.
            frame.window.resize ((size_t) frame.size);
            auto windowSum = 0.0f;
            for (int i = 0; i < frame.size; ++i)
            {
                frame.window[(size_t) i] = std::sqrt (0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) frame.size));
                windowSum += frame.window[(size_t) i];
            }
            frame.fullScalePower = 0.25f * windowSum * windowSum; // bin power of a 0 dBFS sine
        }

        const auto maxSize = frames[2].size, maxBins = maxSize / 2 + 1;

        for (auto& channel : channels)
        {
            channel.input.assign ((size_t) maxSize, 0.0f);
            channel.accumulator.assign ((size_t) maxSize, 0.0f);
            channel.output.assign ((size_t) maxSize / 4, 0.0f);
            channel.spectrum.assign ((size_t) maxSize * 2, 0.0f);
        }

        for (auto& path : paths)
            for (auto* bins : { &path.detection, &path.cutDb, &path.gain })
                bins->assign ((size_t) maxBins, 0.0f);

        for (auto* bins : { &power, &levelDb, &envelopeDb, &targetDb, &spreadDb, &thresholdDb, &logBin })
            bins->assign ((size_t) maxBins + 1, 0.0f);

        for (auto* sums : { &sumX, &sumXX, &sumY, &sumXY, &sumP })
            sums->assign ((size_t) maxBins + 1, 0.0);

        logBin[0] = 0.0f;
        for (int k = 1; k <= maxBins; ++k)
            logBin[(size_t) k] = std::log2 ((float) k);

        windowLow.assign ((size_t) maxBins, 0);
        windowHigh.assign ((size_t) maxBins, 0);

        configured = -1;
        configure (settings.resolution);
    }

    // Clears the audio in flight and every cut.
    void reset() noexcept
    {
        for (auto& channel : channels)
        {
            std::fill (channel.input.begin(), channel.input.end(), 0.0f);
            std::fill (channel.accumulator.begin(), channel.accumulator.end(), 0.0f);
            std::fill (channel.output.begin(), channel.output.end(), 0.0f);
        }

        for (auto& path : paths)
        {
            std::fill (path.detection.begin(), path.detection.end(), 0.0f);
            std::fill (path.cutDb.begin(), path.cutDb.end(), 0.0f);
            std::fill (path.gain.begin(), path.gain.end(), 1.0f);
        }

        position = 0;
        framesSinceReset = 0;

        for (auto& band : reductionBandsDb)
            band.store (0.0f, std::memory_order_relaxed);
    }

    // Audio thread (or before processing starts).
    void setSettings (const Settings& newSettings) noexcept
    {
        const auto needsShape = newSettings.sharpness != settings.sharpness || newSettings.tilt != settings.tilt
                             || newSettings.lowHz != settings.lowHz || newSettings.highHz != settings.highHz
                             || newSettings.thresholdDb != settings.thresholdDb;
        const auto needsTimes = newSettings.attackMs != settings.attackMs || newSettings.releaseMs != settings.releaseMs;
        settings = newSettings;
        settings.depthDb = juce::jlimit (0.0f, maxDepthDb, settings.depthDb);
        settings.mix = juce::jlimit (0.0f, 1.0f, settings.mix);

        if (configured < 0)
            return;

        if ((int) settings.resolution != configured)
            configure (settings.resolution); // also works out the shape and the times
        else
        {
            if (needsShape) computeShape();
            if (needsTimes) computeTimes();
        }
    }

    const Settings& getSettings() const noexcept { return settings; }

    // One frame, for the current resolution.
    int getLatencySamples() const noexcept { return configured < 0 ? getFrameSize (settings.resolution, sampleRate) : fftSize; }

    void process (const juce::dsp::ProcessContextReplacing<float>& context) noexcept
    {
        auto& block = context.getOutputBlock();
        const auto numChannels = juce::jmin (block.getNumChannels(), channels.size());

        if (numChannels == 0 || configured < 0)
            return;

        const auto midSide = numChannels == 2 && settings.stereo == Stereo::midSide;
        auto* left = block.getChannelPointer (0);
        auto* right = numChannels > 1 ? block.getChannelPointer (1) : nullptr;
        auto& a = channels[0];
        auto& b = channels[1];
        const auto writeAt = (size_t) (fftSize - hop);

        for (size_t i = 0; i < block.getNumSamples(); ++i)
        {
            const auto p = (size_t) position;

            if (midSide)
            {
                const auto l = left[i], r = right[i];
                a.input[writeAt + p] = 0.5f * (l + r);
                b.input[writeAt + p] = 0.5f * (l - r);
                const auto m = a.output[p], s = b.output[p];
                left[i] = m + s;
                right[i] = m - s;
            }
            else
            {
                a.input[writeAt + p] = left[i];
                left[i] = a.output[p];

                if (right != nullptr)
                {
                    b.input[writeAt + p] = right[i];
                    right[i] = b.output[p];
                }
            }

            if (++position == hop)
            {
                position = 0;
                processFrame (numChannels, midSide);
            }
        }
    }

    // Written by the audio thread once per frame, read by the UI: the cut applied right now
    // (dB, the deepest bin in each band, log-spaced bandLowHz..bandHighHz) and the deepest
    // cut anywhere since takeDeepestCut() was last called.
    std::array<std::atomic<float>, numBands> reductionBandsDb {};

    float takeDeepestCut() noexcept { return deepestCutDb.exchange (0.0f, std::memory_order_relaxed); }

    // Bin frequency spacing of the current frame, for tests.
    double getBinHz() const noexcept { return sampleRate / juce::jmax (1, fftSize); }

private:
    struct Frame
    {
        int size = 512;
        std::unique_ptr<juce::dsp::FFT> fft;
        std::vector<float> window;
        float fullScalePower = 1.0f;
    };

    struct Channel
    {
        std::vector<float> input;       // newest fftSize samples; the last `hop` are being filled
        std::vector<float> accumulator; // overlap-add of the processed frames
        std::vector<float> output;      // finished samples handed out over the next hop
        std::vector<float> spectrum;    // FFT work buffer (2 * fftSize)
    };

    // One set of cuts: both channels when linked, mid or side in mid/side mode.
    struct Path
    {
        std::vector<float> detection; // smoothed power per bin
        std::vector<float> cutDb;     // current cut per bin (after attack/release)
        std::vector<float> gain;      // the gain applied, mix and delta included
    };

    void configure (Resolution resolution) noexcept
    {
        configured = (int) resolution;
        const auto& frame = frames[(size_t) configured];
        fftSize = frame.size;
        hop = fftSize / 4;
        numBins = fftSize / 2 + 1;

        const auto binHz = sampleRate / fftSize;
        for (int band = 0; band <= numBands; ++band)
        {
            const auto hz = bandLowHz * std::pow (bandHighHz / bandLowHz, (double) band / numBands);
            bandEdges[(size_t) band] = juce::jlimit (1, numBins - 1, (int) std::lround (hz / binHz));
        }

        computeShape();
        computeTimes();
        reset();
    }

    // Envelope windows and the threshold per bin (Sharpness, Focus).
    void computeShape() noexcept
    {
        const auto binHz = sampleRate / fftSize;
        const auto octaves = juce::jmap (juce::jlimit (0.0f, 1.0f, settings.sharpness), 1.0f, 0.125f);
        const auto ratio = std::exp2 (0.5f * octaves);
        const auto lowEdge = juce::jmax (1.0f, settings.lowHz), highEdge = juce::jmax (lowEdge * 1.01f, settings.highHz);
        constexpr float edgeOctaves = 1.0f / 3.0f;

        for (int k = 0; k < numBins; ++k)
        {
            // At least `minimumSide` bins on each side beyond the main lobe, so the low
            // bins (few per octave) still have an envelope to compare with.
            const auto low = juce::jmin (k - mainLobe - minimumSide, (int) std::floor ((float) k / ratio));
            const auto high = juce::jmax (k + mainLobe + minimumSide, (int) std::ceil ((float) k * ratio));
            windowLow[(size_t) k] = juce::jmax (1, low);
            windowHigh[(size_t) k] = juce::jmin (numBins - 1, high);

            const auto hz = juce::jmax (1.0f, (float) (k * binHz));
            const auto fromCentre = std::log2 (hz / 1000.0f) / 4.0f; // -1 at 62 Hz, +1 at 16 kHz
            auto threshold = settings.thresholdDb - maxTiltDb * settings.tilt * juce::jlimit (-1.0f, 1.0f, fromCentre);

            // Outside the range the threshold rises (soft edges over a third of an octave),
            // and far outside nothing is cut.
            const auto outside = juce::jmax (std::log2 (lowEdge / hz), std::log2 (hz / highEdge), 0.0f);
            const auto weight = juce::jlimit (0.0f, 1.0f, 1.0f - outside / edgeOctaves);
            thresholdDb[(size_t) k] = weight <= 0.0f ? 1000.0f : threshold + (1.0f - weight) * maxDepthDb * 2.0f;

            // The lowest bins are never cut: too few bins below them to tell a peak from
            // the slope of the bass (and the leakage of anything under the first bin).
            // That's from ~560 Hz at the balanced resolution, ~280 Hz at quality and
            // ~1.1 kHz at fast. Nor is the top bin.
            if (k < firstBin || k == numBins - 1)
                thresholdDb[(size_t) k] = 1000.0f;
        }
    }

    void computeTimes() noexcept
    {
        const auto frameRate = sampleRate / hop;
        const auto perFrame = [frameRate] (double seconds) { return (float) (1.0 - std::exp (-1.0 / (juce::jmax (1.0e-4, seconds) * frameRate))); };
        detectionCoefficient = perFrame (detectionSeconds);
        attackCoefficient = perFrame (settings.attackMs * 0.001);
        releaseCoefficient = perFrame (settings.releaseMs * 0.001);
    }

    void processFrame (size_t numChannels, bool midSide) noexcept
    {
        const auto& frame = frames[(size_t) configured];
        const auto& window = frame.window;
        const auto numPaths = midSide ? (size_t) 2 : (size_t) 1;
        ++framesSinceReset;

        for (size_t ch = 0; ch < numChannels; ++ch)
        {
            auto* spectrum = channels[ch].spectrum.data();
            for (int i = 0; i < fftSize; ++i)
                spectrum[i] = channels[ch].input[(size_t) i] * window[(size_t) i];

            std::fill (spectrum + fftSize, spectrum + 2 * fftSize, 0.0f);
            frame.fft->performRealOnlyForwardTransform (spectrum, true);
        }

        // The cuts, per path: linked paths hear the channels' average power.
        for (size_t p = 0; p < numPaths; ++p)
        {
            std::fill (power.begin(), power.begin() + numBins, 0.0f);
            const auto first = midSide ? p : 0, last = midSide ? p + 1 : numChannels;

            for (auto ch = first; ch < last; ++ch)
            {
                const auto* spectrum = channels[ch].spectrum.data();
                for (int k = 0; k < numBins; ++k)
                    power[(size_t) k] += (spectrum[2 * k] * spectrum[2 * k] + spectrum[2 * k + 1] * spectrum[2 * k + 1]) / (float) (last - first);
            }

            updateCuts (paths[p], frame.fullScalePower);
        }

        // Synthesis: apply the gains, inverse FFT, window and overlap-add.
        for (size_t ch = 0; ch < numChannels; ++ch)
        {
            auto& channel = channels[ch];
            auto* spectrum = channel.spectrum.data();
            const auto& gain = paths[midSide ? ch : 0].gain;

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

            frame.fft->performRealOnlyInverseTransform (spectrum);

            auto& accumulator = channel.accumulator;
            for (int i = 0; i < fftSize; ++i)
                accumulator[(size_t) i] += spectrum[i] * window[(size_t) i] * overlapScale;

            // The first hop is now complete: hand it out, then slide everything along.
            std::copy (accumulator.begin(), accumulator.begin() + hop, channel.output.begin());
            std::copy (accumulator.begin() + hop, accumulator.begin() + fftSize, accumulator.begin());
            std::fill (accumulator.begin() + fftSize - hop, accumulator.begin() + fftSize, 0.0f);
            std::copy (channel.input.begin() + hop, channel.input.begin() + fftSize, channel.input.begin());
        }

        publish (numPaths);
    }

    void updateCuts (Path& path, float fullScalePower) noexcept
    {
        auto& detection = path.detection;
        const auto quietPower = fullScalePower * juce::Decibels::decibelsToGain (quietDb * 2.0f, -400.0f); // a power ratio: 10^(dB/10)
        const auto warm = framesSinceReset > 1;

        // 1. Detection, in dB relative to a full-scale sine.
        for (int k = 0; k < numBins; ++k)
        {
            auto& d = detection[(size_t) k];
            d = warm ? d + detectionCoefficient * (power[(size_t) k] - d) : power[(size_t) k];
            levelDb[(size_t) k] = 10.0f * std::log10 ((d + tinyPower) / fullScalePower);
        }

        // 2. Envelope: a straight line (dB against log frequency) fitted to each bin's
        // window without its own main lobe, read at the bin. A line rather than a plain
        // mean, so a sloping spectrum (pink noise, a dark voice) and the lopsided windows
        // of the lowest bins don't read as peaks.
        for (int k = 0; k < numBins; ++k)
        {
            const auto x = logBin[(size_t) k], y = levelDb[(size_t) k];
            sumX[(size_t) k + 1] = sumX[(size_t) k] + x;
            sumXX[(size_t) k + 1] = sumXX[(size_t) k] + x * x;
            sumY[(size_t) k + 1] = sumY[(size_t) k] + y;
            sumXY[(size_t) k + 1] = sumXY[(size_t) k] + x * y;
            sumP[(size_t) k + 1] = sumP[(size_t) k] + (double) detection[(size_t) k] / fullScalePower;
        }

        const auto range = [] (const std::vector<double>& sums, int from, int to) { return sums[(size_t) to + 1] - sums[(size_t) from]; }; // inclusive

        for (int k = 0; k < numBins; ++k)
        {
            const auto low = windowLow[(size_t) k], high = windowHigh[(size_t) k];
            const auto lobeLow = juce::jlimit (low, high, k - mainLobe), lobeHigh = juce::jlimit (low, high, k + mainLobe);
            const auto n = (double) ((high - low + 1) - (lobeHigh - lobeLow + 1));

            if (n < 2.0 || k == 0)
            {
                envelopeDb[(size_t) k] = levelDb[(size_t) k];
                continue;
            }

            const auto sx = range (sumX, low, high) - range (sumX, lobeLow, lobeHigh);
            const auto sxx = range (sumXX, low, high) - range (sumXX, lobeLow, lobeHigh);
            const auto sy = range (sumY, low, high) - range (sumY, lobeLow, lobeHigh);
            const auto sxy = range (sumXY, low, high) - range (sumXY, lobeLow, lobeHigh);
            const auto meanX = sx / n, meanY = sy / n;
            const auto spread = sxx / n - meanX * meanX;
            const auto slope = spread > 1.0e-9 ? (sxy / n - meanX * meanY) / spread : 0.0;

            // Raised from the mean dB to the mean power: where the frame resolves a note's
            // harmonics, the envelope rides along their tops rather than through the gaps
            // between them, so only a harmonic louder than its neighbours stands out.
            const auto meanPower = (range (sumP, low, high) - range (sumP, lobeLow, lobeHigh)) / n;
            const auto lift = juce::jmax (0.0, 10.0 * std::log10 (juce::jmax (1.0e-30, meanPower)) - meanY);
            envelopeDb[(size_t) k] = (float) (meanY + lift + slope * ((double) logBin[(size_t) k] - meanX));
        }

        // 3. The target cut: the excess over envelope + threshold, up to Depth. Quiet bins
        // (near digital silence) are left alone.
        const auto depth = settings.depthDb;
        for (int k = 0; k < numBins; ++k)
        {
            const auto excess = levelDb[(size_t) k] - envelopeDb[(size_t) k] - thresholdDb[(size_t) k];
            targetDb[(size_t) k] = detection[(size_t) k] < quietPower ? 0.0f : juce::jlimit (0.0f, depth, excess);
        }

        // ... spread over the main lobe, so all of a peak's bins come down together.
        for (int k = 0; k < numBins; ++k)
        {
            auto spread = targetDb[(size_t) k];
            for (int offset = 1; offset <= mainLobe; ++offset)
            {
                const auto share = offset == 1 ? 1.0f : 0.6f;
                if (k - offset >= 0)      spread = std::max (spread, share * targetDb[(size_t) (k - offset)]);
                if (k + offset < numBins) spread = std::max (spread, share * targetDb[(size_t) (k + offset)]);
            }
            spreadDb[(size_t) k] = spread;
        }

        // 4. Attack and release per bin, then the gain with Mix and Delta.
        const auto mix = settings.mix;
        for (int k = 0; k < numBins; ++k)
        {
            auto& cut = path.cutDb[(size_t) k];
            const auto target = spreadDb[(size_t) k];
            cut += (target > cut ? attackCoefficient : releaseCoefficient) * (target - cut);

            const auto wet = cut < 0.001f ? 1.0f : std::exp (-cut * 0.11512925f); // dB to gain
            const auto blended = 1.0f - mix * (1.0f - wet);
            path.gain[(size_t) k] = settings.delta ? 1.0f - blended : blended;
        }
    }

    void publish (size_t numPaths) noexcept
    {
        auto deepest = 0.0f;
        for (int band = 0; band < numBands; ++band)
        {
            const auto first = bandEdges[(size_t) band];
            const auto last = juce::jmax (first + 1, bandEdges[(size_t) band + 1]);
            auto most = 0.0f;

            for (size_t p = 0; p < numPaths; ++p)
                for (int k = first; k < last; ++k)
                    most = std::max (most, paths[p].cutDb[(size_t) k]);

            reductionBandsDb[(size_t) band].store (most, std::memory_order_relaxed);
            deepest = std::max (deepest, most);
        }

        if (deepest > deepestCutDb.load (std::memory_order_relaxed))
            deepestCutDb.store (deepest, std::memory_order_relaxed);
    }

    static constexpr float tinyPower = 1.0e-20f;
    static constexpr float quietDb = -90.0f;           // dBFS: bins below this are never cut
    static constexpr float maxTiltDb = 6.0f;
    static constexpr float overlapScale = 0.5f;        // four sqrt-Hann² windows sum to 2
    static constexpr double detectionSeconds = 0.01;
    static constexpr int mainLobe = 2;                 // bins either side that belong to a peak (Hann)
    static constexpr int minimumSide = 4;
    static constexpr int firstBin = 6;

    double sampleRate = 48000.0;
    std::array<Frame, 3> frames;
    int configured = -1, fftSize = 512, hop = 128, numBins = 257, position = 0, framesSinceReset = 0;

    std::array<Channel, 2> channels;
    std::array<Path, 2> paths;
    std::vector<float> power, levelDb, envelopeDb, targetDb, spreadDb, thresholdDb, logBin;
    std::vector<double> sumX, sumXX, sumY, sumXY, sumP; // running sums for the envelope's line fits
    std::vector<int> windowLow, windowHigh;
    std::array<int, numBands + 1> bandEdges {};

    float detectionCoefficient = 1.0f, attackCoefficient = 1.0f, releaseCoefficient = 1.0f;
    Settings settings;
    std::atomic<float> deepestCutDb { 0.0f };
};
} // namespace fx
