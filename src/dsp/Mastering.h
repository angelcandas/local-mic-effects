#pragma once

#include "Biquad.h"
#include "FxModule.h"

#include <array>
#include <atomic>
#include <cmath>
#include <vector>

// Mastering effects for the master bus (and any track): a three-band compressor, a
// look-ahead brickwall limiter, a broad mastering EQ, a stereo imager with mono bass and
// a loudness meter (BS.1770 LUFS). All stereo, all linked across channels.
namespace fx
{
//==============================================================================
// A Linkwitz-Riley 24 dB/octave crossover: low + high sum back to an all-pass, so the
// bands of the multiband compressor add up flat.
class Crossover
{
public:
    void setup (double sampleRate, double frequency) noexcept
    {
        if (frequency == lastFrequency && sampleRate == lastRate)
            return;

        lastFrequency = frequency;
        lastRate = sampleRate;
        constexpr auto butterworth = 0.70710678118654752;
        for (auto& f : low)  f.setup (Biquad::Type::lowPass, sampleRate, frequency, butterworth);
        for (auto& f : high) f.setup (Biquad::Type::highPass, sampleRate, frequency, butterworth);
    }

    void reset() noexcept
    {
        for (auto& f : low)  f.reset();
        for (auto& f : high) f.reset();
    }

    // Splits one sample of a channel into its low and high parts.
    void split (size_t channel, float x, float& lowOut, float& highOut) noexcept
    {
        lowOut = low[1].process (channel, low[0].process (channel, x));
        highOut = high[1].process (channel, high[0].process (channel, x));
    }

private:
    std::array<Biquad, 2> low, high;
    double lastFrequency = -1.0, lastRate = -1.0;
};

//==============================================================================
// A feed-forward compressor on a linked stereo level, soft knee, gain reduction readout.
class BandCompressor
{
public:
    void prepare (double sampleRate) noexcept { rate = sampleRate; reset(); }
    void reset() noexcept { envelopeDb = -120.0f; }

    void setParameters (float thresholdDb, float ratioValue, float attackMs, float releaseMs) noexcept
    {
        threshold = thresholdDb;
        ratio = juce::jmax (1.0f, ratioValue);
        attack = std::exp (-1.0f / (juce::jmax (0.1f, attackMs) * 0.001f * (float) rate));
        release = std::exp (-1.0f / (juce::jmax (1.0f, releaseMs) * 0.001f * (float) rate));
    }

    // The gain (linear) for one sample of the linked level |x| (the larger channel).
    float process (float level) noexcept
    {
        const auto db = juce::Decibels::gainToDecibels (level, -120.0f);
        const auto coefficient = db > envelopeDb ? attack : release;
        envelopeDb = db + coefficient * (envelopeDb - db);

        const auto over = envelopeDb - threshold;
        constexpr float knee = 6.0f;
        float reduction = 0.0f;

        if (over >= knee / 2)
            reduction = over * (1.0f - 1.0f / ratio);
        else if (over > -knee / 2)
            reduction = (1.0f - 1.0f / ratio) * juce::square (over + knee / 2) / (2.0f * knee);

        maxReduction = juce::jmax (maxReduction, reduction);
        return juce::Decibels::decibelsToGain (-reduction);
    }

    // The most gain reduction (dB) since the last call. Any thread.
    float takeReduction() noexcept { return reductionReading.exchange (0.0f, std::memory_order_relaxed); }
    void publish() noexcept
    {
        raisePeak (reductionReading, maxReduction);
        maxReduction = 0.0f;
    }

private:
    double rate = 48000.0;
    float threshold = -18.0f, ratio = 2.5f, attack = 0.0f, release = 0.0f;
    float envelopeDb = -120.0f, maxReduction = 0.0f;
    std::atomic<float> reductionReading { 0.0f };
};

//==============================================================================
// Look-ahead brickwall limiter. Each sample's needed gain (to stay under the ceiling) is
// held over the look-ahead window and then smoothed by a moving average of the same
// length, so the gain is already down when the peak arrives: the output never passes the
// ceiling, without clipping. Releases exponentially.
class LookaheadLimiter
{
public:
    static constexpr double lookaheadSeconds = 0.0015;

    void prepare (double sampleRate)
    {
        rate = sampleRate;
        window = juce::jmax (2, (int) std::lround (lookaheadSeconds * sampleRate) + 1);
        needed.assign ((size_t) window, 1.0f);
        held.assign ((size_t) window, 1.0f);
        delay.setSize (2, window);
        reset();
    }

    void reset() noexcept
    {
        std::fill (needed.begin(), needed.end(), 1.0f);
        std::fill (held.begin(), held.end(), 1.0f);
        heldSum = (double) window;
        delay.clear();
        position = 0;
        gain = 1.0f;
    }

    int getLatencySamples() const noexcept { return window - 1; }

    void setParameters (float ceilingDb, float releaseMs) noexcept
    {
        ceiling = juce::Decibels::decibelsToGain (ceilingDb);
        release = std::exp (-1.0f / (juce::jmax (1.0f, releaseMs) * 0.001f * (float) rate));
    }

    void process (juce::dsp::AudioBlock<float>& block) noexcept
    {
        const auto channels = juce::jmin ((size_t) 2, block.getNumChannels());
        auto* left = block.getChannelPointer (0);
        auto* right = channels > 1 ? block.getChannelPointer (1) : nullptr;
        auto* delayLeft = delay.getWritePointer (0);
        auto* delayRight = delay.getWritePointer (1);

        for (size_t i = 0; i < block.getNumSamples(); ++i)
        {
            const auto l = left[i], r = right != nullptr ? right[i] : l;
            const auto level = juce::jmax (std::abs (l), std::abs (r));
            needed[(size_t) position] = level > ceiling ? ceiling / level : 1.0f;

            float lowest = 1.0f;
            for (const auto g : needed)
                lowest = juce::jmin (lowest, g);

            heldSum += lowest - held[(size_t) position];
            held[(size_t) position] = lowest;
            const auto target = (float) (heldSum / window);

            gain = target < gain ? target : target + release * (gain - target);

            // The sample from window - 1 samples ago leaves the delay line now.
            const auto outIndex = (position + 1) % window;
            const auto outLeft = delayLeft[outIndex] * gain;
            const auto outRight = delayRight[outIndex] * gain;
            delayLeft[position] = l;
            delayRight[position] = r;

            left[i] = juce::jlimit (-ceiling, ceiling, outLeft); // rounding safety only
            if (right != nullptr)
                right[i] = juce::jlimit (-ceiling, ceiling, outRight);

            maxReduction = juce::jmax (maxReduction, -juce::Decibels::gainToDecibels (gain, -60.0f));
            position = (position + 1) % window;
        }

        raisePeak (reductionReading, maxReduction);
        maxReduction = 0.0f;
    }

    float takeReduction() noexcept { return reductionReading.exchange (0.0f, std::memory_order_relaxed); }

private:
    double rate = 48000.0;
    int window = 2, position = 0;
    std::vector<float> needed, held;
    double heldSum = 0.0;
    juce::AudioBuffer<float> delay;
    float ceiling = 1.0f, release = 0.0f, gain = 1.0f, maxReduction = 0.0f;
    std::atomic<float> reductionReading { 0.0f };
};

//==============================================================================
// Loudness as in ITU-R BS.1770 / EBU R128: momentary (400 ms), short-term (3 s) and
// integrated (gated, since the meter was switched on), in LUFS, plus the sample peak.
class LoudnessMeter
{
public:
    static constexpr float silence = -100.0f;

    void prepare (double sampleRate)
    {
        hopSize = juce::jmax (1, (int) std::lround (sampleRate * 0.1));
        setupWeighting (sampleRate);
        reset();
    }

    void reset() noexcept
    {
        state = {};
        hops.fill (0.0);
        hopIndex = hopsFilled = hopFill = 0;
        hopSum = 0.0;
        for (auto& bin : histogram)
            bin.store (0, std::memory_order_relaxed);
        momentary.store (silence, std::memory_order_relaxed);
        shortTerm.store (silence, std::memory_order_relaxed);
        peak.store (0.0f, std::memory_order_relaxed);
    }

    // Audio thread: measures, never changes the audio.
    void process (const juce::dsp::AudioBlock<float>& block) noexcept
    {
        const auto channels = juce::jmin ((size_t) 2, block.getNumChannels());

        for (size_t i = 0; i < block.getNumSamples(); ++i)
        {
            for (size_t ch = 0; ch < channels; ++ch)
            {
                const auto x = block.getSample ((int) ch, (int) i);
                hopSum += juce::square (weight (ch, x));
                samplePeak = juce::jmax (samplePeak, std::abs (x));
            }

            if (++hopFill >= hopSize)
                endHop();
        }

        raisePeak (peak, samplePeak);
        samplePeak = 0.0f;
    }

    float getMomentary() const noexcept { return momentary.load (std::memory_order_relaxed); }
    float getShortTerm() const noexcept { return shortTerm.load (std::memory_order_relaxed); }
    float takePeak() noexcept           { return peak.exchange (0.0f, std::memory_order_relaxed); }

    // Gated loudness of every 400 ms block so far (absolute gate -70 LUFS, relative gate
    // 10 LU under the ungated level). Any thread; reads the block histogram.
    float getIntegrated() const noexcept
    {
        double power = 0.0, count = 0.0;
        for (int b = 0; b < numBins; ++b)
        {
            const auto n = (double) histogram[(size_t) b].load (std::memory_order_relaxed);
            power += n * binPower (b);
            count += n;
        }

        if (count <= 0.0)
            return silence;

        const auto relativeGate = toLufs (power / count) - 10.0f;
        power = count = 0.0;

        for (int b = 0; b < numBins; ++b)
        {
            if (binLufs (b) < relativeGate)
                continue;
            const auto n = (double) histogram[(size_t) b].load (std::memory_order_relaxed);
            power += n * binPower (b);
            count += n;
        }

        return count > 0.0 ? toLufs (power / count) : silence;
    }

private:
    static constexpr int numBins = 800; // 0.1 LU each, from -70 LUFS
    static constexpr float binLow = -70.0f, binStep = 0.1f;
    static float binLufs (int b) noexcept    { return binLow + ((float) b + 0.5f) * binStep; }
    static double binPower (int b) noexcept  { return std::pow (10.0, ((double) binLufs (b) + 0.691) / 10.0); }
    static float toLufs (double power) noexcept { return power > 0.0 ? (float) (-0.691 + 10.0 * std::log10 (power)) : silence; }

    void endHop() noexcept
    {
        hops[(size_t) hopIndex] = hopSum / hopSize;
        hopIndex = (hopIndex + 1) % (int) hops.size();
        hopsFilled = juce::jmin (hopsFilled + 1, (int) hops.size());
        hopSum = 0.0;
        hopFill = 0;

        const auto meanOf = [this] (int count)
        {
            double sum = 0.0;
            for (int h = 1; h <= count; ++h)
                sum += hops[(size_t) ((hopIndex - h + (int) hops.size()) % (int) hops.size())];
            return sum / count;
        };

        const auto block = toLufs (meanOf (4));
        momentary.store (hopsFilled >= 4 ? block : silence, std::memory_order_relaxed);
        shortTerm.store (hopsFilled >= 30 ? toLufs (meanOf (30)) : silence, std::memory_order_relaxed);

        if (hopsFilled >= 4 && block >= binLow)
        {
            const auto bin = juce::jlimit (0, numBins - 1, (int) ((block - binLow) / binStep));
            histogram[(size_t) bin].fetch_add (1, std::memory_order_relaxed);
        }
    }

    // BS.1770 K-weighting (a high shelf, then a high-pass), designed for any sample rate.
    void setupWeighting (double sampleRate) noexcept
    {
        {
            const auto f0 = 1681.974450955533, gain = 3.999843853973347, q = 0.7071752369554196;
            const auto k = std::tan (juce::MathConstants<double>::pi * f0 / sampleRate);
            const auto vh = std::pow (10.0, gain / 20.0);
            const auto vb = std::pow (vh, 0.4996667741545416);
            const auto a0 = 1.0 + k / q + k * k;
            shelf = { (vh + vb * k / q + k * k) / a0, 2.0 * (k * k - vh) / a0, (vh - vb * k / q + k * k) / a0,
                      2.0 * (k * k - 1.0) / a0, (1.0 - k / q + k * k) / a0 };
        }
        {
            const auto f0 = 38.13547087602444, q = 0.5003270373238773;
            const auto k = std::tan (juce::MathConstants<double>::pi * f0 / sampleRate);
            const auto a0 = 1.0 + k / q + k * k;
            highPass = { 1.0, -2.0, 1.0, 2.0 * (k * k - 1.0) / a0, (1.0 - k / q + k * k) / a0 };
        }
    }

    double weight (size_t channel, double x) noexcept
    {
        auto& s = state[channel];
        const auto y1 = shelf[0] * x + s[0];
        s[0] = shelf[1] * x - shelf[3] * y1 + s[1];
        s[1] = shelf[2] * x - shelf[4] * y1;
        const auto y2 = highPass[0] * y1 + s[2];
        s[2] = highPass[1] * y1 - highPass[3] * y2 + s[3];
        s[3] = highPass[2] * y1 - highPass[4] * y2;
        return y2;
    }

    std::array<double, 5> shelf {}, highPass {};
    std::array<std::array<double, 4>, 2> state {};
    int hopSize = 4800, hopFill = 0, hopIndex = 0, hopsFilled = 0;
    double hopSum = 0.0;
    std::array<double, 30> hops {}; // mean square of the last 30 hops of 100 ms
    std::array<std::atomic<juce::uint32>, numBins> histogram {};
    std::atomic<float> momentary { silence }, shortTerm { silence }, peak { 0.0f };
    float samplePeak = 0.0f;
};

namespace modules
{
//==============================================================================
class Multiband final : public Module
{
public:
    Multiband() : Module (ModuleType::multiband) {}

    Value& on       = addSwitch ("mbOn");
    Value& lowFreq  = addValue ("mbLowFreq", 200.0f);
    Value& highFreq = addValue ("mbHighFreq", 3000.0f);
    Value& lowThreshold  = addValue ("mbLowThreshold", -18.0f);
    Value& midThreshold  = addValue ("mbMidThreshold", -18.0f);
    Value& highThreshold = addValue ("mbHighThreshold", -18.0f);
    Value& ratio    = addValue ("mbRatio", 2.5f);
    Value& attack   = addValue ("mbAttack", 15.0f);
    Value& release  = addValue ("mbRelease", 180.0f);
    Value& makeup   = addValue ("mbMakeup", 0.0f);

    void addReadings (juce::DynamicObject& readings) override
    {
        juce::Array<juce::var> reductions;
        for (auto& band : bands)
            reductions.add (std::round (band.takeReduction() * 10.0f) / 10.0f);
        readings.setProperty ("bands", reductions);
    }

private:
    void prepareModule (const juce::dsp::ProcessSpec& spec) override
    {
        rate = spec.sampleRate;
        for (auto& band : bands)
            band.prepare (rate);
    }

    void resetModule() override
    {
        lowSplit.reset();
        highSplit.reset();
        lowAllPass.reset();
        for (auto& band : bands)
            band.reset();
    }

    void update() noexcept override
    {
        const auto low = juce::jlimit (20.0f, 18000.0f, get (lowFreq));
        lowSplit.setup (rate, low);
        lowAllPass.setup (rate, juce::jmax (low * 1.25f, get (highFreq)));
        highSplit.setup (rate, juce::jmax (low * 1.25f, get (highFreq)));
        bands[0].setParameters (get (lowThreshold), get (ratio), get (attack), get (release));
        bands[1].setParameters (get (midThreshold), get (ratio), get (attack), get (release));
        bands[2].setParameters (get (highThreshold), get (ratio), get (attack), get (release));
        makeupGain = juce::Decibels::decibelsToGain (get (makeup));
    }

    void render (const Context& c) noexcept override
    {
        auto& block = c.getOutputBlock();
        const auto channels = juce::jmin ((size_t) 2, block.getNumChannels());

        for (size_t i = 0; i < block.getNumSamples(); ++i)
        {
            // Split each channel into low, mid and high (the low band goes through the
            // upper crossover's all-pass, so the three sum back flat).
            std::array<std::array<float, 3>, 2> parts {};
            std::array<float, 3> level {};

            for (size_t ch = 0; ch < channels; ++ch)
            {
                float low, rest, lowLow, lowHigh, mid, high;
                lowSplit.split (ch, block.getSample ((int) ch, (int) i), low, rest);
                lowAllPass.split (ch, low, lowLow, lowHigh);
                highSplit.split (ch, rest, mid, high);
                parts[ch] = { lowLow + lowHigh, mid, high };

                for (size_t b = 0; b < 3; ++b)
                    level[b] = juce::jmax (level[b], std::abs (parts[ch][b]));
            }

            std::array<float, 3> gains {};
            for (size_t b = 0; b < 3; ++b)
                gains[b] = bands[b].process (level[b]);

            for (size_t ch = 0; ch < channels; ++ch)
                block.setSample ((int) ch, (int) i,
                                 makeupGain * (parts[ch][0] * gains[0] + parts[ch][1] * gains[1] + parts[ch][2] * gains[2]));
        }

        for (auto& band : bands)
            band.publish();
    }

    double rate = 48000.0;
    Crossover lowSplit, highSplit, lowAllPass;
    std::array<BandCompressor, 3> bands;
    float makeupGain = 1.0f;
};

//==============================================================================
class Limiter final : public Module
{
public:
    Limiter() : Module (ModuleType::limiter) {}

    Value& on      = addSwitch ("limOn");
    Value& drive   = addValue ("limDrive", 0.0f);
    Value& ceiling = addValue ("limCeiling", -1.0f);
    Value& release = addValue ("limRelease", 120.0f);

    // The look-ahead delays the signal while it's on.
    int getLatencySamples() const noexcept override { return flag (on) ? latency : 0; }

    void addReadings (juce::DynamicObject& readings) override
    {
        readings.setProperty ("reduction", std::round (limiter.takeReduction() * 10.0f) / 10.0f);
    }

private:
    void prepareModule (const juce::dsp::ProcessSpec& spec) override
    {
        limiter.prepare (spec.sampleRate);
        latency = limiter.getLatencySamples();
    }

    void resetModule() override { limiter.reset(); }

    void update() noexcept override
    {
        limiter.setParameters (get (ceiling), get (release));
        driveGain = juce::Decibels::decibelsToGain (get (drive));
    }

    void render (const Context& c) noexcept override
    {
        auto& block = c.getOutputBlock();
        if (driveGain != 1.0f)
            block.multiplyBy (driveGain);
        limiter.process (block);
    }

    LookaheadLimiter limiter;
    int latency = 0;
    float driveGain = 1.0f;
};

//==============================================================================
// Broad strokes for a whole mix: a subsonic low cut, low shelf, one wide bell, high
// shelf and an "air" shelf at 16 kHz. The UI draws the same curve (ui/fxviews.js).
class MasterEq final : public Module
{
public:
    MasterEq() : Module (ModuleType::masterEq) {}

    static constexpr float midQ = 0.7f, airHz = 16000.0f;

    Value& on       = addSwitch ("meqOn");
    Value& lowCut   = addValue ("meqLowCut", 20.0f);
    Value& lowFreq  = addValue ("meqLowFreq", 100.0f);
    Value& lowGain  = addValue ("meqLowGain", 0.0f);
    Value& midFreq  = addValue ("meqMidFreq", 1000.0f);
    Value& midGain  = addValue ("meqMidGain", 0.0f);
    Value& highFreq = addValue ("meqHighFreq", 8000.0f);
    Value& highGain = addValue ("meqHighGain", 0.0f);
    Value& airGain  = addValue ("meqAirGain", 0.0f);

private:
    void prepareModule (const juce::dsp::ProcessSpec& spec) override { rate = spec.sampleRate; last = {}; }
    void resetModule() override
    {
        for (auto& f : filters)
            f.reset();
    }

    void update() noexcept override
    {
        const std::array<float, 8> now { get (lowCut), get (lowFreq), get (lowGain), get (midFreq), get (midGain),
                                         get (highFreq), get (highGain), get (airGain) };
        if (now == last)
            return;

        last = now;
        constexpr auto butterworth = 0.70710678118654752;
        filters[0].setup (Biquad::Type::highPass, rate, now[0], butterworth);
        filters[1].setup (Biquad::Type::lowShelf, rate, now[1], butterworth, now[2]);
        filters[2].setup (Biquad::Type::peak, rate, now[3], midQ, now[4]);
        filters[3].setup (Biquad::Type::highShelf, rate, now[5], butterworth, now[6]);
        filters[4].setup (Biquad::Type::highShelf, rate, juce::jmin ((double) airHz, rate * 0.45), butterworth, now[7]);
    }

    void render (const Context& c) noexcept override
    {
        auto& block = c.getOutputBlock();
        for (size_t ch = 0; ch < juce::jmin ((size_t) 2, block.getNumChannels()); ++ch)
        {
            auto* data = block.getChannelPointer (ch);
            for (size_t i = 0; i < block.getNumSamples(); ++i)
            {
                auto x = data[i];
                for (auto& f : filters)
                    x = f.process (ch, x);
                data[i] = x;
            }
        }
    }

    double rate = 48000.0;
    std::array<Biquad, 5> filters;
    std::array<float, 8> last {};
};

//==============================================================================
// Mid/side width (0% mono, 100% as is, 200% twice the sides), with the sides below a
// frequency taken out so the low end stays mono and centred.
class Imager final : public Module
{
public:
    Imager() : Module (ModuleType::imager) {}

    Value& on        = addSwitch ("imgOn");
    Value& width     = addValue ("imgWidth", 120.0f);
    Value& monoBelow = addValue ("imgMonoBelow", 120.0f); // Hz; below 20 = off

    void addReadings (juce::DynamicObject& readings) override
    {
        const auto m = midPower.exchange (0.0f, std::memory_order_relaxed);
        const auto s = sidePower.exchange (0.0f, std::memory_order_relaxed);
        // -1 (out of phase) .. 0 (wide) .. 1 (mono), as a correlation meter shows it.
        readings.setProperty ("correlation", m + s > 1.0e-9f ? std::round ((m - s) / (m + s) * 100.0f) / 100.0f : 1.0f);
    }

private:
    void prepareModule (const juce::dsp::ProcessSpec& spec) override { rate = spec.sampleRate; }
    void resetModule() override { sideFilter.reset(); }

    void update() noexcept override
    {
        amount = get (width) / 100.0f;
        monoOn = get (monoBelow) >= 20.0f;
        if (monoOn)
            sideFilter.setup (rate, get (monoBelow));
    }

    void render (const Context& c) noexcept override
    {
        auto& block = c.getOutputBlock();
        if (block.getNumChannels() < 2)
            return;

        auto* left = block.getChannelPointer (0);
        auto* right = block.getChannelPointer (1);
        float mids = 0.0f, sides = 0.0f;

        for (size_t i = 0; i < block.getNumSamples(); ++i)
        {
            const auto mid = 0.5f * (left[i] + right[i]);
            auto side = 0.5f * (left[i] - right[i]);

            if (monoOn)
            {
                float low, high;
                sideFilter.split (0, side, low, high);
                side = high;
            }

            side *= amount;
            left[i] = mid + side;
            right[i] = mid - side;
            mids += mid * mid;
            sides += side * side;
        }

        addTo (midPower, mids);
        addTo (sidePower, sides);
    }

    double rate = 48000.0;
    Crossover sideFilter;
    float amount = 1.2f;
    bool monoOn = true;
    std::atomic<float> midPower { 0.0f }, sidePower { 0.0f };

    // std::atomic<float>::fetch_add is missing from Apple's libc++.
    static void addTo (std::atomic<float>& total, float value) noexcept
    {
        auto old = total.load (std::memory_order_relaxed);
        while (! total.compare_exchange_weak (old, old + value, std::memory_order_relaxed)) {}
    }
};

//==============================================================================
// Measures, never changes the sound. Switching it off and on starts the integrated
// reading again.
class Loudness final : public Module
{
public:
    Loudness() : Module (ModuleType::loudness) {}

    Value& on     = addSwitch ("loudOn");
    Value& target = addValue ("loudTarget", -14.0f); // LUFS the readout compares against

    void addReadings (juce::DynamicObject& readings) override
    {
        readings.setProperty ("momentary", std::round (meter.getMomentary() * 10.0f) / 10.0f);
        readings.setProperty ("shortTerm", std::round (meter.getShortTerm() * 10.0f) / 10.0f);
        readings.setProperty ("integrated", std::round (meter.getIntegrated() * 10.0f) / 10.0f);
        readings.setProperty ("peak", peakToDecibels (meter.takePeak()));
        readings.setProperty ("target", get (target));
    }

    const LoudnessMeter& getMeter() const noexcept { return meter; }

private:
    void prepareModule (const juce::dsp::ProcessSpec& spec) override { meter.prepare (spec.sampleRate); }
    void resetModule() override { meter.reset(); }
    void update() noexcept override {}
    void render (const Context& c) noexcept override { meter.process (c.getOutputBlock()); }

    LoudnessMeter meter;
};
} // namespace modules
} // namespace fx
