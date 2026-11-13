#pragma once

#include "Biquad.h"
#include "Mastering.h"

#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>

// Dynamics made for the voice: a compressor with three characters (Clean, Punch, Glue),
// a slow optical-style compressor, a gate that can be a gentle expander, and a de-esser
// that judges sibilance against the rest of the voice instead of an absolute level.
//
// All of them run per sample on a linked stereo level, add no latency (no look-ahead)
// and never allocate on the audio thread. Each keeps the deepest gain reduction since
// the last reading, for the effect cards.
namespace fx::vocal
{
// One-pole coefficient for a time constant in ms (0 = instant).
inline float poleFor (double sampleRate, double ms) noexcept
{
    return ms <= 0.0 ? 0.0f : (float) std::exp (-1000.0 / (ms * sampleRate));
}

inline float toDb (float linear) noexcept { return 20.0f * std::log10 (std::max (linear, 1.0e-6f)); }
inline float toGain (float decibels) noexcept { return std::pow (10.0f, decibels * 0.05f); }

// Gain reduction (dB, >= 0) for a level, with a soft knee `kneeDb` wide centred on the
// threshold. `slope` is 1 - 1/ratio for a feed-forward compressor.
inline float reductionFor (float levelDb, float thresholdDb, float slope, float kneeDb) noexcept
{
    const auto over = levelDb - thresholdDb;

    if (kneeDb > 0.0f && 2.0f * std::abs (over) < kneeDb)
    {
        const auto t = over + 0.5f * kneeDb;
        return slope * t * t / (2.0f * kneeDb);
    }

    return over > 0.0f ? slope * over : 0.0f;
}

// The deepest reduction since the last take(), shared between the audio thread (push)
// and the message thread (take).
class ReductionMeter
{
public:
    void push (float reductionDb) noexcept
    {
        if (reductionDb > pending)
            pending = reductionDb;
    }

    // Once per block, from the audio thread.
    void publish() noexcept
    {
        auto current = deepest.load (std::memory_order_relaxed);
        while (pending > current && ! deepest.compare_exchange_weak (current, pending, std::memory_order_relaxed)) {}
        pending = 0.0f;
    }

    float take() noexcept { return deepest.exchange (0.0f, std::memory_order_relaxed); }

private:
    std::atomic<float> deepest { 0.0f };
    float pending = 0.0f; // audio thread
};

//==============================================================================
// The Compressor's own engine for its Punch and Glue styles, and for Clean once the knee,
// mix or auto make-up are used (at their defaults Clean stays on juce::dsp::Compressor,
// so old sessions sound exactly as before).
//
//   Clean: peak level, feed-forward, the user's attack and release (like the juce one,
//          but stereo-linked and with a knee).
//   Punch: peak level, feedback (the detector hears the output, so it grabs fast and
//          lets go naturally), a release that is quick after short peaks and slower
//          after long squeezes, and a touch of level-matched harmonic colour while
//          it works.
//   Glue:  RMS level, a wide soft knee (at least 10 dB) and the same two-speed release,
//          longer: the slow, even compression of a bus.
class VoiceCompressor
{
public:
    enum class Style { clean, punch, glue };

    struct Settings
    {
        Style style = Style::clean;
        float thresholdDb = -18.0f, ratio = 3.0f, attackMs = 10.0f, releaseMs = 120.0f;
        float kneeDb = 0.0f;
        float mix = 1.0f;        // 0..1, below 1 is parallel compression
        float makeupDb = 0.0f;
        bool autoMakeup = false;
    };

    // The make-up auto make-up adds: half of what the curve takes off a 0 dBFS peak.
    static float autoMakeupDb (float thresholdDb, float ratio) noexcept
    {
        return 0.5f * std::max (0.0f, -thresholdDb) * (1.0f - 1.0f / std::max (1.0f, ratio));
    }

    void prepare (double newSampleRate) noexcept
    {
        sampleRate = newSampleRate;
        rmsPole = poleFor (sampleRate, 10.0);
        lastSettings = {};
        setSettings (settings);
        reset();
    }

    void reset() noexcept
    {
        envelope = 0.0f;
        meanSquare = 0.0f;
        fastDb = slowDb = 0.0f;
        lastGain = 1.0f;
    }

    void setSettings (const Settings& s) noexcept
    {
        settings = s;

        if (s.style == lastSettings.style && s.attackMs == lastSettings.attackMs && s.releaseMs == lastSettings.releaseMs)
            return;

        lastSettings = s;
        const auto attack = std::max (0.05f, s.attackMs);
        const auto release = std::max (1.0f, s.releaseMs);

        switch (s.style)
        {
            case Style::clean:
                attackPole = poleFor (sampleRate, attack);
                fastReleasePole = slowReleasePole = poleFor (sampleRate, release);
                slowAttackPole = attackPole;
                break;

            case Style::punch:
                attackPole = poleFor (sampleRate, attack);
                fastReleasePole = poleFor (sampleRate, release * 0.4f);
                slowAttackPole = poleFor (sampleRate, 120.0);
                slowReleasePole = poleFor (sampleRate, release * 2.0f);
                break;

            case Style::glue:
                attackPole = poleFor (sampleRate, attack);
                fastReleasePole = poleFor (sampleRate, release * 0.5f);
                slowAttackPole = poleFor (sampleRate, 250.0);
                slowReleasePole = poleFor (sampleRate, release * 2.5f);
                break;
        }
    }

    void process (juce::dsp::AudioBlock<float>& block) noexcept
    {
        const auto numChannels = std::min<size_t> (block.getNumChannels(), 2);
        const auto numSamples = block.getNumSamples();
        const auto ratio = std::max (1.0f, settings.ratio);
        const auto slope = 1.0f - 1.0f / ratio;
        const auto knee = settings.style == Style::glue ? std::max (10.0f, settings.kneeDb) : std::max (0.0f, settings.kneeDb);
        const auto makeup = toGain (settings.makeupDb + (settings.autoMakeup ? autoMakeupDb (settings.thresholdDb, ratio) : 0.0f));
        const auto mix = juce::jlimit (0.0f, 1.0f, settings.mix);
        const auto threshold = settings.thresholdDb;

        // In feedback the detector hears the already-reduced level: for the same static
        // curve it needs the slope s / (1 - s), which is ratio - 1.
        const auto feedbackSlope = ratio - 1.0f;

        for (size_t i = 0; i < numSamples; ++i)
        {
            auto peak = 0.0f;
            for (size_t ch = 0; ch < numChannels; ++ch)
                peak = std::max (peak, std::abs (block.getSample ((int) ch, (int) i)));

            float target = 0.0f;

            switch (settings.style)
            {
                case Style::clean:
                {
                    // Linear envelope with the attack and release, like juce's compressor.
                    const auto pole = peak > envelope ? attackPole : fastReleasePole;
                    envelope = peak + pole * (envelope - peak);
                    target = reductionFor (toDb (envelope), threshold, slope, knee);
                    break;
                }

                case Style::punch:
                    target = reductionFor (toDb (peak * lastGain), threshold, feedbackSlope, knee);
                    target = std::min (target, 40.0f);
                    break;

                case Style::glue:
                    meanSquare = peak * peak + rmsPole * (meanSquare - peak * peak);
                    target = reductionFor (0.5f * toDb (meanSquare) + 3.0f, threshold, slope, knee);
                    break;
            }

            float reductionDb;

            if (settings.style == Style::clean)
            {
                reductionDb = target; // the envelope already has the timing
            }
            else
            {
                fastDb = target + (target > fastDb ? attackPole : fastReleasePole) * (fastDb - target);
                slowDb = target + (target > slowDb ? slowAttackPole : slowReleasePole) * (slowDb - target);
                reductionDb = std::max (fastDb, slowDb);
            }

            const auto gain = toGain (-reductionDb);
            lastGain = gain;
            meter.push (reductionDb);

            // Punch colours a little while it squeezes: a soft curve blended in by the
            // reduction (at most 12 %), which leaves small signals at the same level.
            const auto colour = settings.style == Style::punch ? 0.12f * std::min (1.0f, reductionDb / 12.0f) : 0.0f;

            for (size_t ch = 0; ch < numChannels; ++ch)
            {
                const auto dry = block.getSample ((int) ch, (int) i);
                auto wet = dry * gain;

                if (colour > 0.0f)
                    wet = (1.0f - colour) * wet + colour * std::tanh (wet);

                wet *= makeup;
                block.setSample ((int) ch, (int) i, dry + mix * (wet - dry));
            }
        }

        meter.publish();
    }

    float takeReductionDb() noexcept { return meter.take(); }

private:
    double sampleRate = 48000.0;
    Settings settings, lastSettings;
    float attackPole = 0.0f, fastReleasePole = 0.0f, slowAttackPole = 0.0f, slowReleasePole = 0.0f, rmsPole = 0.0f;
    float envelope = 0.0f, meanSquare = 0.0f, fastDb = 0.0f, slowDb = 0.0f, lastGain = 1.0f;
    ReductionMeter meter;
};

//==============================================================================
// A slow, smooth compressor in the manner of an optical leveller, for evening out a
// whole line rather than catching peaks. Amount sets how hard it is pushed (the
// threshold falls as it rises); the ratio grows with level (about 2.5:1 rising to 4.5:1
// in Compress, 8:1 to 16:1 in Limit) over a 12 dB soft knee.
//
// Timing: an averaging detector (10 ms attack, 15 ms release), then a two-stage release: half
// of the reduction lets go in about 60 ms, the other half over a tail of 0.5 to 3 s that
// grows the longer and harder it has been working.
class OptoCompressor
{
public:
    enum class Mode { compress, limit };

    struct Settings
    {
        float amount = 0.3f;   // 0..1
        Mode mode = Mode::compress;
        float outputDb = 0.0f;
        float mix = 1.0f;      // 0..1
    };

    static float thresholdFor (float amount) noexcept { return -4.0f - 34.0f * juce::jlimit (0.0f, 1.0f, amount); }

    void prepare (double newSampleRate) noexcept
    {
        sampleRate = newSampleRate;
        detectorAttack = poleFor (sampleRate, 10.0);
        detectorRelease = poleFor (sampleRate, 15.0);
        attackPole = poleFor (sampleRate, 10.0);
        fastReleasePole = poleFor (sampleRate, 60.0);
        memoryRise = poleFor (sampleRate, 1500.0);
        memoryFall = poleFor (sampleRate, 4000.0);
        reset();
    }

    void reset() noexcept { power = 0.0f; fastDb = slowDb = 0.0f; memory = 0.0f; }

    void setSettings (const Settings& s) noexcept { settings = s; }

    void process (juce::dsp::AudioBlock<float>& block) noexcept
    {
        const auto numChannels = std::min<size_t> (block.getNumChannels(), 2);
        const auto threshold = thresholdFor (settings.amount);
        const auto limit = settings.mode == Mode::limit;
        const auto makeup = toGain (settings.outputDb);
        const auto mix = juce::jlimit (0.0f, 1.0f, settings.mix);

        for (size_t i = 0; i < block.getNumSamples(); ++i)
        {
            auto squared = 0.0f;
            for (size_t ch = 0; ch < numChannels; ++ch)
            {
                const auto x = block.getSample ((int) ch, (int) i);
                squared = std::max (squared, x * x);
            }

            power = squared + (squared > power ? detectorAttack : detectorRelease) * (power - squared);
            const auto levelDb = 10.0f * std::log10 (std::max (power, 1.0e-12f)) + 3.0f; // RMS of a sine reads as its peak

            const auto over = juce::jlimit (0.0f, 1.0f, (levelDb - threshold) / 20.0f);
            const auto ratio = limit ? 8.0f + 8.0f * over : 2.5f + 2.0f * over;
            const auto target = reductionFor (levelDb, threshold, 1.0f - 1.0f / ratio, 12.0f);

            // The tail stretches with how hard and how long it has been working.
            const auto busy = std::min (1.0f, 0.5f * (fastDb + slowDb) / 10.0f);
            memory = busy + (busy > memory ? memoryRise : memoryFall) * (memory - busy);
            const auto tailSeconds = 0.5f + 2.5f * memory;
            const auto tailPole = 1.0f - 1.0f / (tailSeconds * (float) sampleRate);

            fastDb = target + (target > fastDb ? attackPole : fastReleasePole) * (fastDb - target);
            slowDb = target + (target > slowDb ? attackPole : tailPole) * (slowDb - target);

            const auto reductionDb = 0.5f * (fastDb + slowDb);
            meter.push (reductionDb);
            const auto gain = toGain (-reductionDb) * makeup;

            for (size_t ch = 0; ch < numChannels; ++ch)
            {
                const auto dry = block.getSample ((int) ch, (int) i);
                block.setSample ((int) ch, (int) i, dry + mix * (dry * gain - dry));
            }
        }

        meter.publish();
    }

    float takeReductionDb() noexcept { return meter.take(); }

private:
    double sampleRate = 48000.0;
    Settings settings;
    float detectorAttack = 0.0f, detectorRelease = 0.0f, attackPole = 0.0f, fastReleasePole = 0.0f;
    float memoryRise = 0.0f, memoryFall = 0.0f;
    float power = 0.0f, fastDb = 0.0f, slowDb = 0.0f, memory = 0.0f;
    ReductionMeter meter;
};

//==============================================================================
// The Gate's own engine once its range, hold, release or Focus are used (at their
// defaults it stays on juce::dsp::NoiseGate, as before).
//
// It opens when the level passes the threshold and closes 3 dB below it (so a voice
// hovering at the threshold doesn't chatter), after the hold time. Closed, it turns
// down by Range instead of muting: 8-12 dB is a soft expander for speech and stage
// mics. Focus makes the detector hear only 150 Hz-4 kHz, so kick drum and cymbal bleed
// don't open it.
class ExpanderGate
{
public:
    struct Settings
    {
        float thresholdDb = -55.0f;
        float rangeDb = 80.0f;
        float holdMs = 0.0f;
        float releaseMs = 150.0f;
        bool focus = false;
    };

    static constexpr float hysteresisDb = 3.0f;

    void prepare (double newSampleRate) noexcept
    {
        sampleRate = newSampleRate;
        rmsRelease = poleFor (sampleRate, 20.0);
        openPole = poleFor (sampleRate, 1.0);
        focusLow.setup (Biquad::Type::highPass, sampleRate, 150.0, 0.707);
        focusHigh.setup (Biquad::Type::lowPass, sampleRate, 4000.0, 0.707);
        lastReleaseMs = -1.0f;
        setSettings (settings);
        reset();
    }

    void reset() noexcept
    {
        focusLow.reset();
        focusHigh.reset();
        power = 0.0f;
        gainDb = -settings.rangeDb;
        open = false;
        holdLeft = 0;
    }

    void setSettings (const Settings& s) noexcept
    {
        settings = s;
        if (s.releaseMs != lastReleaseMs)
        {
            lastReleaseMs = s.releaseMs;
            closePole = poleFor (sampleRate, std::max (1.0f, s.releaseMs));
        }
    }

    bool isOpen() const noexcept { return open; }

    void process (juce::dsp::AudioBlock<float>& block) noexcept
    {
        const auto numChannels = std::min<size_t> (block.getNumChannels(), 2);
        const auto holdSamples = (int) (settings.holdMs * 0.001f * (float) sampleRate);
        const auto closedDb = -std::max (0.0f, settings.rangeDb);

        for (size_t i = 0; i < block.getNumSamples(); ++i)
        {
            auto squared = 0.0f;
            for (size_t ch = 0; ch < numChannels; ++ch)
            {
                auto x = block.getSample ((int) ch, (int) i);
                if (settings.focus)
                    x = focusHigh.process (ch, focusLow.process (ch, x));
                squared = std::max (squared, x * x);
            }

            // Instant rise, 20 ms fall (a little quicker than the original gate, so Hold and
            // Release set the timing).
            power = squared > power ? squared : squared + rmsRelease * (power - squared);
            const auto levelDb = 10.0f * std::log10 (std::max (power, 1.0e-12f));

            if (levelDb > settings.thresholdDb)
            {
                open = true;
                holdLeft = holdSamples;
            }
            else if (levelDb < settings.thresholdDb - hysteresisDb)
            {
                if (holdLeft > 0)
                    --holdLeft;
                else
                    open = false;
            }

            const auto target = open ? 0.0f : closedDb;
            gainDb = target + (target > gainDb ? openPole : closePole) * (gainDb - target);
            meter.push (-gainDb);
            const auto gain = toGain (gainDb);

            for (size_t ch = 0; ch < numChannels; ++ch)
                block.setSample ((int) ch, (int) i, block.getSample ((int) ch, (int) i) * gain);
        }

        meter.publish();
    }

    float takeReductionDb() noexcept { return meter.take(); }

private:
    double sampleRate = 48000.0;
    Settings settings;
    Biquad focusLow, focusHigh;
    float rmsRelease = 0.0f, openPole = 0.0f, closePole = 0.0f, lastReleaseMs = -1.0f;
    float power = 0.0f, gainDb = 0.0f;
    bool open = false;
    int holdLeft = 0;
    ReductionMeter meter;
};

//==============================================================================
// The De-esser's own engine for Smart mode, and for Classic once range, Wide, or Listen
// are used (Classic at its defaults stays on fx::DeEsser, as before).
//
//   Classic: the level of the highs against an absolute threshold, 4:1 above it.
//   Smart:   the level of the highs against the level of the whole voice (both
//            smoothed over a few ms), so it cuts the same on a whisper and a belt and
//            leaves bright vowels alone. Amount sets how far above their usual share
//            the highs may go.
//
// Split turns down only the highs (Smart splits with a Linkwitz-Riley crossover, so the
// bands add up flat); Wide dips the whole voice, which sounds more natural on dark
// voices. Range is the most it may cut. Listen plays only what is taken out.
class SmartDeEsser
{
public:
    enum class Mode { classic, smart };
    enum class Band { split, wide };

    struct Settings
    {
        Mode mode = Mode::classic;
        float thresholdDb = -30.0f; // Classic
        float amount = 0.5f;        // Smart, 0..1
        float frequency = 6000.0f;
        float rangeDb = 24.0f;
        Band band = Band::split;
        bool listen = false;
    };

    // Smart: how far below the whole voice the highs may sit before it cuts (dB).
    static float relativeThresholdFor (float amount) noexcept { return -2.0f - 16.0f * juce::jlimit (0.0f, 1.0f, amount); }

    // Below this the voice is too quiet to judge (room noise): Smart leaves it alone.
    static constexpr float floorDb = -65.0f;

    void prepare (double newSampleRate) noexcept
    {
        sampleRate = newSampleRate;
        attackPole = poleFor (sampleRate, 1.0);
        releasePole = poleFor (sampleRate, 60.0);
        lastFrequency = -1.0f;
        setSettings (settings);
        reset();
    }

    void reset() noexcept
    {
        classicSplit.reset();
        crossover.reset();
        highEnvelope = fullEnvelope = 0.0f;
    }

    void setSettings (const Settings& s) noexcept
    {
        settings = s;
        if (s.frequency != lastFrequency)
        {
            lastFrequency = s.frequency;
            classicSplit.setup (Biquad::Type::highPass, sampleRate, s.frequency, 0.707);
            crossover.setup (sampleRate, s.frequency);
        }
    }

    void process (juce::dsp::AudioBlock<float>& block) noexcept
    {
        const auto numChannels = std::min<size_t> (block.getNumChannels(), 2);
        const auto smart = settings.mode == Mode::smart;
        const auto wide = settings.band == Band::wide;
        const auto range = std::max (0.0f, settings.rangeDb);
        const auto relativeThreshold = relativeThresholdFor (settings.amount);
        std::array<float, 2> dry {}, low {}, high {};

        for (size_t i = 0; i < block.getNumSamples(); ++i)
        {
            auto highPeak = 0.0f, fullPeak = 0.0f;

            for (size_t ch = 0; ch < numChannels; ++ch)
            {
                dry[ch] = block.getSample ((int) ch, (int) i);

                if (smart)
                {
                    crossover.split (ch, dry[ch], low[ch], high[ch]);
                }
                else
                {
                    high[ch] = classicSplit.process (ch, dry[ch]);
                    low[ch] = dry[ch] - high[ch];
                }

                highPeak = std::max (highPeak, std::abs (high[ch]));
                fullPeak = std::max (fullPeak, std::abs (dry[ch]));
            }

            highEnvelope = highPeak + (highPeak > highEnvelope ? attackPole : releasePole) * (highEnvelope - highPeak);
            fullEnvelope = fullPeak + (fullPeak > fullEnvelope ? attackPole : releasePole) * (fullEnvelope - fullPeak);

            float cut;
            if (smart)
            {
                const auto fullDb = toDb (fullEnvelope);
                cut = fullDb > floorDb ? std::max (0.0f, toDb (highEnvelope) - fullDb - relativeThreshold) : 0.0f;
            }
            else
            {
                cut = 0.75f * std::max (0.0f, toDb (highEnvelope) - settings.thresholdDb);
            }

            cut = std::min (cut, range);
            meter.push (cut);
            const auto gain = toGain (-cut);

            for (size_t ch = 0; ch < numChannels; ++ch)
            {
                const auto out = wide ? dry[ch] * gain : low[ch] + high[ch] * gain;
                // Listen: what is taken out (for Split in Smart, the highs' share, so the
                // crossover's phase doesn't show up as "removed").
                const auto removed = wide ? dry[ch] - out : high[ch] * (1.0f - gain);
                block.setSample ((int) ch, (int) i, settings.listen ? removed : out);
            }
        }

        meter.publish();
    }

    float takeReductionDb() noexcept { return meter.take(); }

private:
    double sampleRate = 48000.0;
    Settings settings;
    Biquad classicSplit;
    Crossover crossover;
    float attackPole = 0.0f, releasePole = 0.0f, lastFrequency = -1.0f;
    float highEnvelope = 0.0f, fullEnvelope = 0.0f;
    ReductionMeter meter;
};
}
