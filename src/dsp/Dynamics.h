#pragma once

#include "Biquad.h"
#include "FxModule.h"

#include <atomic>
#include <cmath>

// Dynamics that work on part of the sound: a transient shaper (the attack and the tail of
// each hit, louder or softer, whatever the level) and a one-band dynamic EQ (a bell that
// only cuts while its band is loud). Both stereo, linked across channels.
namespace fx::modules
{
//==============================================================================
// Compares envelopes of the level: a fast one against a slower-attacking one finds the
// start of each hit, a slow-releasing one against the fast one finds its tail. Attack and
// sustain (-100..100%) turn those differences, in dB, into gain: +100% adds half the
// difference again (up to 12 dB), -100% takes half of it away.
class Transient final : public Module
{
public:
    Transient() : Module (ModuleType::transient) {}

    Value& on      = addSwitch ("tsOn");
    Value& attack  = addValue ("tsAttack", 0.0f);  // %
    Value& sustain = addValue ("tsSustain", 0.0f); // %

    // The envelopes' times, seconds.
    static constexpr double fastAttack = 0.0005, slowAttack = 0.02, release = 0.08, tailRelease = 0.4;
    static constexpr float maxGainDb = 12.0f;

    void addReadings (juce::DynamicObject& readings) override
    {
        readings.setProperty ("gain", std::round (peakGainDb.exchange (0.0f, std::memory_order_relaxed) * 10.0f) / 10.0f);
    }

private:
    void prepareModule (const juce::dsp::ProcessSpec& spec) override
    {
        const auto coefficient = [rate = spec.sampleRate] (double seconds) { return (float) (1.0 - std::exp (-1.0 / (seconds * rate))); };
        fastA = coefficient (fastAttack);
        slowA = coefficient (slowAttack);
        rel = coefficient (release);
        tailRel = coefficient (tailRelease);
    }

    void resetModule() override { fast = slow = tail = 0.0f; }

    void update() noexcept override
    {
        attackAmount = fraction (attack);
        sustainAmount = fraction (sustain);
    }

    void render (const Context& c) noexcept override
    {
        auto& block = c.getOutputBlock();
        const auto channels = block.getNumChannels();
        constexpr float floor = 1.0e-5f; // -100 dB
        float loudest = 0.0f;

        for (size_t i = 0; i < block.getNumSamples(); ++i)
        {
            float x = 0.0f;
            for (size_t ch = 0; ch < channels; ++ch)
                x = std::max (x, std::abs (block.getSample ((int) ch, (int) i)));

            fast += (x > fast ? fastA : rel) * (x - fast);
            slow += (x > slow ? slowA : rel) * (x - slow);
            tail += (x > tail ? fastA : tailRel) * (x - tail);

            const auto onsetDb = 20.0f * std::log10 ((fast + floor) / (slow + floor));
            const auto tailDb = 20.0f * std::log10 ((tail + floor) / (fast + floor));
            const auto gainDb = juce::jlimit (-maxGainDb, maxGainDb, 0.5f * (attackAmount * onsetDb + sustainAmount * tailDb));
            loudest = std::abs (gainDb) > std::abs (loudest) ? gainDb : loudest;

            const auto gain = std::exp (gainDb * 0.11512925f); // dB to gain
            for (size_t ch = 0; ch < channels; ++ch)
                block.setSample ((int) ch, (int) i, block.getSample ((int) ch, (int) i) * gain);
        }

        if (std::abs (loudest) > std::abs (peakGainDb.load (std::memory_order_relaxed)))
            peakGainDb.store (loudest, std::memory_order_relaxed);
    }

    float fastA = 1.0f, slowA = 1.0f, rel = 1.0f, tailRel = 1.0f;
    float fast = 0.0f, slow = 0.0f, tail = 0.0f;
    float attackAmount = 0.0f, sustainAmount = 0.0f;
    std::atomic<float> peakGainDb { 0.0f };
};

//==============================================================================
// A bell at `freq` that cuts only while the band around it (a band-pass of the same Q)
// is over the threshold: 4:1 above it, by up to `range` dB. The detector is the band's
// mean square, so the threshold reads like the band levels Auto Mix measures.
class DynamicEq final : public Module
{
public:
    DynamicEq() : Module (ModuleType::dynamicEq) {}

    Value& on        = addSwitch ("deqOn");
    Value& freq      = addValue ("deqFreq", 3000.0f);
    Value& q         = addValue ("deqQ", 2.0f);
    Value& threshold = addValue ("deqThreshold", -30.0f); // dB
    Value& range     = addValue ("deqRange", 6.0f);       // dB, the deepest cut

    static constexpr float ratio = 4.0f;
    static constexpr double attackSeconds = 0.005, releaseSeconds = 0.08;
    static constexpr int chunk = 16; // samples between bell updates

    void addReadings (juce::DynamicObject& readings) override
    {
        readings.setProperty ("reduction", std::round (peakReduction.exchange (0.0f, std::memory_order_relaxed) * 10.0f) / 10.0f);
    }

private:
    void prepareModule (const juce::dsp::ProcessSpec& spec) override
    {
        rate = spec.sampleRate;
        attackA = (float) (1.0 - std::exp (-1.0 / (attackSeconds * rate)));
        releaseA = (float) (1.0 - std::exp (-1.0 / (releaseSeconds * rate)));
        lastFreq = lastQ = -1.0f;
        lastCut = -1.0f;
    }

    void resetModule() override
    {
        detector.reset();
        bell.reset();
        envelope = 0.0f;
        lastCut = -1.0f;
    }

    void update() noexcept override
    {
        const auto f = get (freq), width = juce::jmax (0.3f, get (q));
        if (f != lastFreq || width != lastQ)
        {
            detector.setup (Biquad::Type::bandPass, rate, f, width);
            lastFreq = f;
            lastQ = width;
            lastCut = -1.0f; // the bell follows
        }
        thresholdDb = get (threshold);
        rangeDb = juce::jmax (0.0f, get (range));
    }

    void render (const Context& c) noexcept override
    {
        auto& block = c.getOutputBlock();
        const auto channels = juce::jmin ((size_t) 2, block.getNumChannels());
        const auto n = (int) block.getNumSamples();
        float deepest = 0.0f;

        for (int start = 0; start < n; start += chunk)
        {
            const auto end = juce::jmin (n, start + chunk);

            for (int i = start; i < end; ++i)
            {
                float power = 0.0f;
                for (size_t ch = 0; ch < channels; ++ch)
                {
                    const auto y = detector.process (ch, block.getSample ((int) ch, i));
                    power = std::max (power, y * y);
                }
                envelope += (power > envelope ? attackA : releaseA) * (power - envelope);
            }

            const auto levelDb = 10.0f * std::log10 (envelope + 1.0e-12f);
            const auto cut = juce::jlimit (0.0f, rangeDb, (levelDb - thresholdDb) * (1.0f - 1.0f / ratio));
            if (std::abs (cut - lastCut) > 0.05f)
            {
                bell.setup (Biquad::Type::peak, rate, lastFreq, lastQ, -cut);
                lastCut = cut;
            }
            deepest = std::max (deepest, cut);

            for (int i = start; i < end; ++i)
                for (size_t ch = 0; ch < channels; ++ch)
                    block.setSample ((int) ch, i, bell.process (ch, block.getSample ((int) ch, i)));
        }

        if (deepest > peakReduction.load (std::memory_order_relaxed))
            peakReduction.store (deepest, std::memory_order_relaxed);
    }

    double rate = 48000.0;
    float attackA = 1.0f, releaseA = 1.0f;
    float lastFreq = -1.0f, lastQ = -1.0f, lastCut = -1.0f;
    float thresholdDb = -30.0f, rangeDb = 6.0f, envelope = 0.0f;
    Biquad detector, bell;
    std::atomic<float> peakReduction { 0.0f };
};
}
