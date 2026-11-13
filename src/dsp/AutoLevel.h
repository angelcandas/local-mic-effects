#pragma once

#include "FxModule.h"
#include "Mastering.h"

#include <array>
#include <atomic>
#include <cmath>

namespace fx::modules
{
//==============================================================================
// Auto level: a rider that keeps a voice at a steady loudness, the way an engineer rides
// the vocal fader. It measures the input's loudness (K-weighted, BS.1770) over the last
// 400 ms of singing, and moves a gain within +/- range dB towards the target, gliding over
// `speed` ms so it never pumps on single syllables. No look-ahead (it's for Live).
//
// The target is a fixed loudness (LUFS), or "over the backing": the backing track's
// momentary loudness plus N LU, handed in by the host with setReference() (Live mode
// measures the Multitrack's output). With no backing playing, the fixed target holds.
//
// While nothing is sung (the input stays far under the target, under where a gate would
// close) the gain freezes where it was, so the room's noise isn't brought up between
// phrases.
class AutoLevel final : public Module
{
public:
    AutoLevel() : Module (ModuleType::autoLevel) {}

    Value& on          = addSwitch ("alOn");
    Value& target      = addValue ("alTarget", -18.0f);     // LUFS
    Value& range       = addValue ("alRange", 6.0f);        // dB, each way
    Value& speed       = addValue ("alSpeed", 400.0f);      // ms
    Value& mode        = addChoice ("alMode", 0.0f);        // 0 fixed level, 1 over the backing
    Value& overBacking = addValue ("alOverBacking", 3.0f);  // LU over the backing

    static constexpr double chunkSeconds = 0.01;
    static constexpr int windowChunks = 40;      // 400 ms of singing
    static constexpr float silence = -100.0f;
    static constexpr float freezeUnderDb = 18.0f; // under target - range, it's not singing
    static constexpr float noBackingLufs = -50.0f; // a quieter backing counts as none

    // The backing's momentary loudness (LUFS), or silence when there's none. Any thread;
    // the next block uses it.
    void setReference (float lufs) noexcept { reference.store (lufs, std::memory_order_relaxed); }

    // The loudness it aims at now (LUFS).
    float getEffectiveTarget() const noexcept
    {
        const auto backing = reference.load (std::memory_order_relaxed);
        if (juce::roundToInt (mode.load (std::memory_order_relaxed)) == 1 && backing > noBackingLufs)
            return juce::jlimit (-40.0f, -6.0f, backing + overBacking.load (std::memory_order_relaxed));
        return target.load (std::memory_order_relaxed);
    }

    float getRideDb() const noexcept    { return rideReading.load (std::memory_order_relaxed); }
    float getLoudness() const noexcept  { return loudnessReading.load (std::memory_order_relaxed); }

    // ride: the gain now (dB), loudness: the voice over the last 400 ms of singing (LUFS),
    // target: what it aims at, frozen: holding (nothing sung), backing: its loudness.
    void addReadings (juce::DynamicObject& readings) override
    {
        readings.setProperty ("ride", std::round (getRideDb() * 10.0f) / 10.0f);
        readings.setProperty ("loudness", std::round (getLoudness() * 10.0f) / 10.0f);
        readings.setProperty ("target", std::round (getEffectiveTarget() * 10.0f) / 10.0f);
        readings.setProperty ("frozen", frozenReading.load (std::memory_order_relaxed));
        readings.setProperty ("backing", std::round (reference.load (std::memory_order_relaxed) * 10.0f) / 10.0f);
    }

private:
    void prepareModule (const juce::dsp::ProcessSpec& spec) override
    {
        rate = spec.sampleRate;
        chunkLength = juce::jmax (1, (int) std::lround (rate * chunkSeconds));
        weighting.setup (rate);
    }

    void resetModule() override
    {
        weighting.reset();
        chunks.fill (0.0);
        chunkIndex = chunksFilled = chunkFill = 0;
        chunkSum = 0.0;
        gainDb = desiredDb = 0.0f;
        rideReading.store (0.0f, std::memory_order_relaxed);
        loudnessReading.store (silence, std::memory_order_relaxed);
    }

    void update() noexcept override
    {
        rangeDb = juce::jlimit (0.0f, 24.0f, get (range));
        glide = (float) (1.0 - std::exp (-1.0 / (juce::jmax (10.0f, get (speed)) * 0.001 * rate)));
        aim = getEffectiveTarget();
    }

    void render (const Context& c) noexcept override
    {
        auto& block = c.getOutputBlock();
        const auto channels = juce::jmin ((size_t) 2, block.getNumChannels());
        const auto n = block.getNumSamples();

        for (size_t i = 0; i < n; ++i)
        {
            double power = 0.0;
            for (size_t ch = 0; ch < channels; ++ch)
                power += juce::square (weighting.process (ch, block.getSample ((int) ch, (int) i)));
            // Channels add up, as in BS.1770 (and the Live backing's meter).
            chunkSum += power;

            if (++chunkFill >= chunkLength)
                endChunk();

            gainDb += (desiredDb - gainDb) * glide;
            const auto gain = juce::Decibels::decibelsToGain (gainDb);
            for (size_t ch = 0; ch < block.getNumChannels(); ++ch)
                block.setSample ((int) ch, (int) i, block.getSample ((int) ch, (int) i) * gain);
        }

        rideReading.store (gainDb, std::memory_order_relaxed);
    }

    static float toLufs (double power) noexcept { return power > 0.0 ? (float) (-0.691 + 10.0 * std::log10 (power)) : silence; }

    void endChunk() noexcept
    {
        const auto level = toLufs (chunkSum / chunkLength);
        chunkSum = 0.0;
        chunkFill = 0;

        const auto frozen = level < juce::jmax (-70.0f, aim - rangeDb - freezeUnderDb);
        frozenReading.store (frozen, std::memory_order_relaxed);
        if (frozen)
            return; // hold: desiredDb stays where it was

        chunks[(size_t) chunkIndex] = std::pow (10.0, ((double) level + 0.691) / 10.0);
        chunkIndex = (chunkIndex + 1) % windowChunks;
        chunksFilled = juce::jmin (chunksFilled + 1, windowChunks);

        double sum = 0.0;
        for (int k = 0; k < chunksFilled; ++k)
            sum += chunks[(size_t) k];
        const auto loudness = toLufs (sum / chunksFilled);
        loudnessReading.store (loudness, std::memory_order_relaxed);

        // Wait for 100 ms of singing before the first move.
        if (chunksFilled >= windowChunks / 4)
            desiredDb = juce::jlimit (-rangeDb, rangeDb, aim - loudness);
    }

    double rate = 48000.0;
    int chunkLength = 480;
    KWeighting weighting;
    std::array<double, windowChunks> chunks {}; // mean square of the last sung 10 ms chunks
    int chunkIndex = 0, chunksFilled = 0, chunkFill = 0;
    double chunkSum = 0.0;
    float gainDb = 0.0f, desiredDb = 0.0f, rangeDb = 6.0f, glide = 0.001f, aim = -18.0f;

    std::atomic<float> reference { silence };
    std::atomic<float> rideReading { 0.0f }, loudnessReading { silence };
    std::atomic<bool> frozenReading { false };
};
} // namespace fx::modules
