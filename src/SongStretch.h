#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

// A live tempo and pitch change for a song's track (Practice's key and speed): no rendering,
// no extra tracks, the change is heard at once.
//
// The track is read at the pitch ratio (which changes its pitch and its speed together),
// then WSOLA (overlap-add of Hann-windowed grains whose starts are nudged to the spot that
// continues the last grain best) puts the speed right without moving the pitch again.
// So speed s and pitch ratio p give a tempo factor q = s / p on top of the resampling.
//
// Audio thread only, after prepare() on another thread. The track's own start must be 0.
// Positions are timeline samples as the transport counts them (playing time): output
// sample t is the song at t * s.
class SongStretcher
{
public:
    static constexpr int grain = 2048, hop = grain / 2, search = 256, searchStep = 2, corrStride = 4;

    SongStretcher()
    {
        for (auto& a : acc)
            a.assign ((size_t) grain, 0.0f);
        for (auto& f : fifo)
            f.assign ((size_t) fifoSize, 0.0f);
        window.resize ((size_t) grain);
        for (int i = 0; i < grain; ++i)
            window[(size_t) i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) grain);
        chunk.assign ((size_t) (hop + 2 * search + 2), 0.0f);
        target.assign ((size_t) hop, 0.0f);
    }

    // Writes `n` samples of the track as heard at timeline sample `t` into out[ch][0..n).
    // `speed` 0.5..1.5, `pitch` the frequency ratio (2^(semitones/12)).
    void pull (const juce::AudioBuffer<float>& source, double speed, double pitch, juce::int64 t, float* const* out, int numChannels, int n) noexcept
    {
        if (t != expected || speed != lastSpeed || pitch != lastPitch)
            reset (speed, pitch, t);

        const auto channels = juce::jmin (numChannels, 2);
        int done = 0;
        while (done < n)
        {
            if (avail == 0)
                makeHop (source);
            const auto count = juce::jmin (n - done, avail);
            for (int ch = 0; ch < channels; ++ch)
                for (int i = 0; i < count; ++i)
                    out[ch][done + i] = fifo[(size_t) ch][(size_t) ((readAt + i) % fifoSize)];
            readAt = (readAt + count) % fifoSize;
            avail -= count;
            done += count;
        }
        expected = t + n;
    }

private:
    static constexpr int fifoSize = hop * 2;

    void reset (double speed, double pitch, juce::int64 t) noexcept
    {
        lastSpeed = speed;
        lastPitch = pitch;
        q = speed / pitch;
        ideal = (double) t * q;
        first = true;
        avail = 0;
        readAt = 0;
        for (auto& a : acc)
            std::fill (a.begin(), a.end(), 0.0f);
    }

    // The track at r-domain position m (the track read at the pitch ratio), channel `ch`.
    static float sampleAt (const juce::AudioBuffer<float>& src, int ch, double x) noexcept
    {
        const auto size = src.getNumSamples();
        const auto i = (int) std::floor (x);
        if (i < 0 || i + 1 >= size)
            return 0.0f;
        const auto* d = src.getReadPointer (juce::jmin (ch, src.getNumChannels() - 1));
        const auto f = (float) (x - (double) i);
        return d[i] + (d[i + 1] - d[i]) * f;
    }

    float mono (const juce::AudioBuffer<float>& src, juce::int64 m) const noexcept
    {
        return sampleAt (src, 0, (double) m * lastPitch);
    }

    void makeHop (const juce::AudioBuffer<float>& src) noexcept
    {
        juce::int64 start;
        if (first)
        {
            start = (juce::int64) std::llround (ideal);
            first = false;
        }
        else
        {
            ideal += (double) hop * q;
            const auto center = (juce::int64) std::llround (ideal);
            // What the last grain would have gone on to play, and where nearby that is best matched.
            const auto natural = lastStart + hop;
            for (int i = 0; i < hop; ++i)
                target[(size_t) i] = mono (src, natural + i);
            const auto base = center - search;
            for (int i = 0; i < hop + 2 * search; ++i)
                chunk[(size_t) i] = mono (src, base + i);

            auto best = (juce::int64) 0;
            auto bestCost = std::numeric_limits<float>::max();
            for (int d = -search; d <= search; d += searchStep)
            {
                const auto* c = chunk.data() + (d + search);
                auto cost = 0.0f;
                for (int i = 0; i < hop; i += corrStride)
                    cost += std::abs (c[i] - target[(size_t) i]);
                // (a slight pull towards the ideal spot, so ties don't drift)
                cost += (float) std::abs (d) * 1.0e-4f;
                if (cost < bestCost)
                {
                    bestCost = cost;
                    best = d;
                }
            }
            start = center + best;
        }
        lastStart = start;

        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < grain; ++i)
                acc[(size_t) ch][(size_t) i] += window[(size_t) i] * sampleAt (src, ch, (double) (start + i) * lastPitch);

        for (int ch = 0; ch < 2; ++ch)
        {
            for (int i = 0; i < hop; ++i)
                fifo[(size_t) ch][(size_t) i] = acc[(size_t) ch][(size_t) i];
            std::copy (acc[(size_t) ch].begin() + hop, acc[(size_t) ch].end(), acc[(size_t) ch].begin());
            std::fill (acc[(size_t) ch].begin() + hop, acc[(size_t) ch].end(), 0.0f);
        }
        readAt = 0;
        avail = hop;
    }

    std::vector<float> acc[2], fifo[2], window, chunk, target;
    double q = 1.0, ideal = 0.0, lastSpeed = 0.0, lastPitch = 0.0;
    juce::int64 expected = -1, lastStart = 0;
    int avail = 0, readAt = 0;
    bool first = true;
};
