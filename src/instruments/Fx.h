#pragma once

#include "InstrumentCore.h"

// Effects the instruments put after their voices: EQ, chorus, delay and a room.
namespace micfx::inst
{
// RBJ biquad, one channel (transposed direct form II).
struct Biquad
{
    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f, z1 = 0.0f, z2 = 0.0f;

    void setPeak (float frequency, float q, float gainDb, double rate) noexcept
    {
        const auto a = std::pow (10.0f, gainDb / 40.0f);
        const auto w = juce::MathConstants<float>::twoPi * frequency / (float) rate;
        const auto alpha = std::sin (w) / (2.0f * q);
        const auto c = std::cos (w);
        set (1.0f + alpha * a, -2.0f * c, 1.0f - alpha * a, 1.0f + alpha / a, -2.0f * c, 1.0f - alpha / a);
    }

    void setLowShelf (float frequency, float gainDb, double rate) noexcept  { setShelf (frequency, gainDb, rate, true); }
    void setHighShelf (float frequency, float gainDb, double rate) noexcept { setShelf (frequency, gainDb, rate, false); }

    void setLowpass (float frequency, float q, double rate) noexcept
    {
        const auto w = juce::MathConstants<float>::twoPi * juce::jmin (frequency, (float) rate * 0.49f) / (float) rate;
        const auto alpha = std::sin (w) / (2.0f * q);
        const auto c = std::cos (w);
        set ((1.0f - c) * 0.5f, 1.0f - c, (1.0f - c) * 0.5f, 1.0f + alpha, -2.0f * c, 1.0f - alpha);
    }

    void setHighpass (float frequency, float q, double rate) noexcept
    {
        const auto w = juce::MathConstants<float>::twoPi * frequency / (float) rate;
        const auto alpha = std::sin (w) / (2.0f * q);
        const auto c = std::cos (w);
        set ((1.0f + c) * 0.5f, -(1.0f + c), (1.0f + c) * 0.5f, 1.0f + alpha, -2.0f * c, 1.0f - alpha);
    }

    float process (float x) noexcept
    {
        const auto y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }

    void reset() noexcept { z1 = z2 = 0.0f; }

private:
    void setShelf (float frequency, float gainDb, double rate, bool low) noexcept
    {
        const auto a = std::pow (10.0f, gainDb / 40.0f);
        const auto w = juce::MathConstants<float>::twoPi * frequency / (float) rate;
        const auto c = std::cos (w);
        const auto alpha = std::sin (w) / 2.0f * std::sqrt (2.0f);
        const auto sa = 2.0f * std::sqrt (a) * alpha;
        if (low)
            set (a * ((a + 1) - (a - 1) * c + sa), 2 * a * ((a - 1) - (a + 1) * c), a * ((a + 1) - (a - 1) * c - sa),
                 (a + 1) + (a - 1) * c + sa, -2 * ((a - 1) + (a + 1) * c), (a + 1) + (a - 1) * c - sa);
        else
            set (a * ((a + 1) + (a - 1) * c + sa), -2 * a * ((a - 1) + (a + 1) * c), a * ((a + 1) + (a - 1) * c - sa),
                 (a + 1) - (a - 1) * c + sa, 2 * ((a - 1) - (a + 1) * c), (a + 1) - (a - 1) * c - sa);
    }

    void set (float nb0, float nb1, float nb2, float na0, float na1, float na2) noexcept
    {
        b0 = nb0 / na0; b1 = nb1 / na0; b2 = nb2 / na0; a1 = na1 / na0; a2 = na2 / na0;
    }
};

// A fractional delay line.
class DelayLine
{
public:
    void prepare (int maxSamples)
    {
        buffer.assign ((size_t) juce::nextPowerOfTwo (maxSamples + 4), 0.0f);
        mask = (int) buffer.size() - 1;
        write = 0;
    }

    void reset() { std::fill (buffer.begin(), buffer.end(), 0.0f); }

    void push (float x) noexcept { buffer[(size_t) write] = x; write = (write + 1) & mask; }

    // `delay` samples ago (>= 1), interpolated.
    float read (float delay) const noexcept
    {
        const auto position = (float) write - delay;
        const auto index = (int) std::floor (position);
        const auto frac = position - (float) index;
        const auto a = buffer[(size_t) (index & mask)];
        const auto b = buffer[(size_t) ((index + 1) & mask)];
        return a + frac * (b - a);
    }

private:
    std::vector<float> buffer;
    int mask = 0, write = 0;
};

// Two modulated delays per side: a wide, smooth chorus.
class Chorus
{
public:
    void prepare (double rate)
    {
        sampleRate = rate;
        for (auto& d : delays)
            d.prepare ((int) (rate * 0.05));
    }

    void reset() { for (auto& d : delays) d.reset(); }

    void process (float* left, float* right, int n, float mix, float rateHz = 0.6f, float depthMs = 3.5f) noexcept
    {
        if (mix <= 0.0001f)
            return;
        const auto increment = rateHz / (float) sampleRate;
        const auto base = 0.012f * (float) sampleRate;
        const auto depth = depthMs * 0.001f * (float) sampleRate;
        for (int i = 0; i < n; ++i)
        {
            delays[0].push (left[i]);
            delays[1].push (right[i]);
            const auto p = juce::MathConstants<float>::twoPi * phase;
            const auto wetL = 0.6f * delays[0].read (base + depth * (1.0f + std::sin (p)) * 0.5f)
                            + 0.4f * delays[1].read (base * 1.3f + depth * (1.0f + std::sin (p + 2.1f)) * 0.5f);
            const auto wetR = 0.6f * delays[1].read (base + depth * (1.0f + std::cos (p)) * 0.5f)
                            + 0.4f * delays[0].read (base * 1.3f + depth * (1.0f + std::cos (p + 2.1f)) * 0.5f);
            left[i] += mix * (wetL - 0.5f * left[i]);
            right[i] += mix * (wetR - 0.5f * right[i]);
            phase += increment;
            phase -= std::floor (phase);
        }
    }

private:
    DelayLine delays[2];
    double sampleRate = 48000.0;
    float phase = 0.0f;
};

// A ping-pong echo with darkening repeats.
class Echo
{
public:
    void prepare (double rate)
    {
        sampleRate = rate;
        for (auto& d : delays)
            d.prepare ((int) (rate * 2.1));
    }

    void reset() { for (auto& d : delays) d.reset(); toneL = toneR = 0.0f; }

    void process (float* left, float* right, int n, float mix, float seconds, float feedback) noexcept
    {
        if (mix <= 0.0001f)
        {
            if (! cleared) { reset(); cleared = true; }
            return;
        }
        cleared = false;
        const auto target = juce::jlimit (1.0f, (float) sampleRate * 2.0f, seconds * (float) sampleRate);
        const auto damp = 0.35f;
        for (int i = 0; i < n; ++i)
        {
            time += (target - time) * 0.0005f; // glides when the time changes
            const auto echoL = delays[0].read (time);
            const auto echoR = delays[1].read (time);
            toneL += damp * (echoL - toneL);
            toneR += damp * (echoR - toneR);
            const auto in = 0.5f * (left[i] + right[i]);
            delays[0].push (in + feedback * toneR); // crossed: ping-pong
            delays[1].push (feedback * toneL);
            left[i] += mix * echoL;
            right[i] += mix * echoR;
        }
    }

private:
    DelayLine delays[2];
    double sampleRate = 48000.0;
    float time = 0.0f, toneL = 0.0f, toneR = 0.0f;
    bool cleared = true;
};

// A room/hall reverb (Freeverb) as a send.
class Room
{
public:
    void prepare (double rate) { reverb.setSampleRate (rate); scratchL.resize (8192); scratchR.resize (8192); }
    void reset() { reverb.reset(); }

    void process (float* left, float* right, int n, float mix, float size, float damping = 0.5f, float width = 1.0f) noexcept
    {
        if (mix <= 0.0001f)
        {
            if (! cleared) { reverb.reset(); cleared = true; }
            return;
        }
        cleared = false;
        juce::Reverb::Parameters p;
        p.roomSize = juce::jlimit (0.0f, 1.0f, size);
        p.damping = damping;
        p.wetLevel = 1.0f;
        p.dryLevel = 0.0f;
        p.width = width;
        reverb.setParameters (p);

        for (int start = 0; start < n; start += 8192)
        {
            const auto count = juce::jmin (8192, n - start);
            std::copy (left + start, left + start + count, scratchL.begin());
            std::copy (right + start, right + start + count, scratchR.begin());
            reverb.processStereo (scratchL.data(), scratchR.data(), count);
            for (int i = 0; i < count; ++i)
            {
                left[start + i] += mix * 0.6f * scratchL[(size_t) i];
                right[start + i] += mix * 0.6f * scratchR[(size_t) i];
            }
        }
    }

private:
    juce::Reverb reverb;
    std::vector<float> scratchL, scratchR;
    bool cleared = true;
};
}
