#include "Beats.h"

#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <cmath>
#include <numeric>

namespace analysis::rhythm
{
namespace
{
    std::vector<float> mixToMono (const juce::AudioBuffer<float>& audio)
    {
        std::vector<float> mono ((size_t) audio.getNumSamples(), 0.0f);
        const auto channels = audio.getNumChannels();

        for (int ch = 0; ch < channels; ++ch)
        {
            const auto* data = audio.getReadPointer (ch);
            for (size_t i = 0; i < mono.size(); ++i)
                mono[i] += data[i] / (float) channels;
        }

        return mono;
    }

    double fitLine (const std::vector<double>& y, double& intercept) // slope of y[i] against i
    {
        const auto n = (double) y.size();
        const auto meanX = (n - 1.0) / 2.0;
        const auto meanY = std::accumulate (y.begin(), y.end(), 0.0) / n;
        double sxy = 0.0, sxx = 0.0;

        for (size_t i = 0; i < y.size(); ++i)
        {
            sxy += ((double) i - meanX) * (y[i] - meanY);
            sxx += ((double) i - meanX) * ((double) i - meanX);
        }

        const auto slope = sxx > 0.0 ? sxy / sxx : 0.0;
        intercept = meanY - slope * meanX;
        return slope;
    }
}

//==============================================================================
OnsetEnvelope getOnsetEnvelope (const juce::AudioBuffer<float>& audio, double sampleRate)
{
    OnsetEnvelope envelope;
    const auto hop = juce::jmax (1, (int) std::round (sampleRate * 0.005));
    envelope.hopSeconds = hop / sampleRate;

    const auto mono = mixToMono (audio);
    const auto order = juce::jlimit (8, 12, (int) std::round (std::log2 (sampleRate * 0.023)));
    const int size = 1 << order, half = size / 2;
    const auto maxBin = juce::jmin (half, (int) std::ceil (11000.0 * size / sampleRate));

    juce::dsp::FFT fft (order);
    juce::dsp::WindowingFunction<float> window ((size_t) size, juce::dsp::WindowingFunction<float>::hann, false);
    std::vector<float> data ((size_t) size * 2), previous ((size_t) maxBin, 0.0f);

    const auto frames = (int) (mono.size() / (size_t) hop) + 1;
    envelope.values.assign ((size_t) frames, 0.0f);

    for (int f = 0; f < frames; ++f)
    {
        const auto start = f * hop - half;
        for (int i = 0; i < size; ++i)
        {
            const auto n = start + i;
            data[(size_t) i] = n >= 0 && n < (int) mono.size() ? mono[(size_t) n] : 0.0f;
        }

        window.multiplyWithWindowingTable (data.data(), (size_t) size);
        fft.performFrequencyOnlyForwardTransform (data.data(), true);

        float flux = 0.0f;
        for (int k = 1; k < maxBin; ++k)
        {
            const auto level = std::log1p (100.0f * data[(size_t) k] / (float) (size / 4));
            if (f > 0)
                flux += juce::jmax (0.0f, level - previous[(size_t) k]);
            previous[(size_t) k] = level;
        }

        envelope.values[(size_t) f] = flux;
    }

    return envelope;
}

std::vector<Onset> findOnsets (const juce::AudioBuffer<float>& audio, double sampleRate, double from, double to, float sensitivity)
{
    std::vector<Onset> onsets;
    const auto envelope = getOnsetEnvelope (audio, sampleRate);
    const auto& e = envelope.values;
    const auto hop = envelope.hopSeconds;
    const auto numFrames = (int) e.size();

    if (to < 0.0)
        to = audio.getNumSamples() / sampleRate;

    const auto first = juce::jlimit (0, numFrames, (int) std::floor (from / hop));
    const auto last = juce::jlimit (0, numFrames, (int) std::ceil (to / hop) + 1);
    if (last - first < 3)
        return onsets;

    const auto peak = *std::max_element (e.begin() + first, e.begin() + last);
    if (peak <= 1.0e-6f)
        return onsets;

    const auto delta = peak * (0.02f + 0.2f * (1.0f - juce::jlimit (0.0f, 1.0f, sensitivity)));
    const auto spacing = (int) std::round (0.05 / hop);
    const auto before = (int) std::round (0.1 / hop), after = (int) std::round (0.05 / hop);

    // Level in 1 ms blocks, to place each onset at the start of its attack.
    const auto mono = mixToMono (audio);
    const auto block = juce::jmax (1, (int) std::round (sampleRate * 0.001));
    const auto blockPeak = [&] (int b)
    {
        float m = 0.0f;
        for (int i = b * block; i < juce::jmin ((int) mono.size(), (b + 1) * block); ++i)
            m = juce::jmax (m, std::abs (mono[(size_t) i]));
        return m;
    };

    int lastPicked = -spacing;

    for (int f = first; f < last; ++f)
    {
        const auto value = e[(size_t) f];
        bool isPeak = value > delta;

        for (int g = juce::jmax (0, f - spacing); isPeak && g <= juce::jmin (numFrames - 1, f + spacing); ++g)
            if (e[(size_t) g] > value || (e[(size_t) g] == value && g < f))
                isPeak = false;

        if (! isPeak || f - lastPicked < spacing)
            continue;

        float mean = 0.0f;
        int count = 0;
        for (int g = juce::jmax (0, f - before); g <= juce::jmin (numFrames - 1, f + after); ++g, ++count)
            mean += e[(size_t) g];

        if (value < mean / (float) juce::jmax (1, count) + delta)
            continue;

        // The attack: the loudest millisecond just after the flux peak, then back to where
        // the level first rises towards it.
        const auto centre = f * hop * sampleRate;
        const auto searchFrom = juce::jmax (0, (int) ((centre - 0.025 * sampleRate) / block));
        const auto searchTo = juce::jmin ((int) mono.size() / block, (int) ((centre + 0.04 * sampleRate) / block));
        int loudest = searchFrom;
        float loudestLevel = 0.0f;

        for (int b = searchFrom; b < searchTo; ++b)
            if (const auto level = blockPeak (b); level > loudestLevel)
            {
                loudest = b;
                loudestLevel = level;
            }

        if (loudestLevel < 0.002f) // below about -54 dBFS: noise, not a note
            continue;

        auto attack = loudest;
        while (attack > searchFrom && blockPeak (attack - 1) > 0.15f * loudestLevel)
            --attack;

        const auto time = attack * block / sampleRate;
        if (time < from || time > to)
            continue;

        onsets.push_back ({ time, value / peak });
        lastPicked = f;
    }

    return onsets;
}

//==============================================================================
Tempo detectTempo (const juce::AudioBuffer<float>& audio, double sampleRate, int beatsPerBar)
{
    Tempo result;
    const auto envelope = getOnsetEnvelope (audio, sampleRate);
    const auto hop = envelope.hopSeconds;
    const auto n = (int) envelope.values.size();

    // Take away the slow-moving level (1 s average), so only the pulses remain.
    std::vector<double> e ((size_t) n, 0.0);
    {
        const auto radius = (int) std::round (0.5 / hop);
        double sum = 0.0;
        int count = 0, lo = 0, hi = 0;

        for (int i = 0; i < n; ++i)
        {
            while (hi < n && hi <= i + radius) { sum += envelope.values[(size_t) hi++]; ++count; }
            while (lo < i - radius)            { sum -= envelope.values[(size_t) lo++]; --count; }
            e[(size_t) i] = juce::jmax (0.0, envelope.values[(size_t) i] - sum / count);
        }
    }

    const auto minLag = (int) std::floor (60.0 / 200.0 / hop), maxLag = (int) std::ceil (60.0 / 60.0 / hop);
    if (n < maxLag * 4)
        return result; // under about 4 seconds: not enough to tell

    const auto energy = std::inner_product (e.begin(), e.end(), e.begin(), 0.0);
    if (energy <= 0.0)
        return result;

    std::vector<double> acf ((size_t) (2 * maxLag + 2), 0.0);
    for (int lag = 1; lag < (int) acf.size(); ++lag)
    {
        double sum = 0.0;
        for (int i = lag; i < n; ++i)
            sum += e[(size_t) i] * e[(size_t) (i - lag)];
        acf[(size_t) lag] = sum / energy;
    }

    // Favour tempos around 120 BPM, and periods whose double also repeats.
    const auto preferred = 60.0 / 120.0 / hop;
    double best = -1.0;
    int bestLag = 0;

    for (int lag = minLag; lag <= maxLag; ++lag)
    {
        const auto octaves = std::log2 (lag / preferred);
        const auto weight = std::exp (-0.5 * octaves * octaves / (0.9 * 0.9));
        const auto score = weight * (acf[(size_t) lag] + 0.5 * acf[(size_t) (2 * lag)]);

        if (score > best)
        {
            best = score;
            bestLag = lag;
        }
    }

    double period = bestLag;
    {
        const auto a = acf[(size_t) bestLag - 1], b = acf[(size_t) bestLag], c = acf[(size_t) bestLag + 1];
        if (const auto d = a - 2.0 * b + c; d < 0.0)
            period += juce::jlimit (-0.5, 0.5, 0.5 * (a - c) / d);
    }

    result.confidence = juce::jlimit (0.0, 1.0, acf[(size_t) bestLag] * 4.0);

    // Follow the beats (dynamic programming, after Ellis 2007): each beat is placed where
    // there's an onset and about one period after the last, so the tempo may drift.
    const auto tightness = 200.0;
    std::vector<double> score ((size_t) n), local ((size_t) n);
    std::vector<int> previous ((size_t) n, -1);
    {
        const auto mean = std::accumulate (e.begin(), e.end(), 0.0) / n;
        double variance = 0.0;
        for (auto v : e) variance += (v - mean) * (v - mean);
        const auto deviation = juce::jmax (1.0e-9, std::sqrt (variance / n));
        for (int i = 0; i < n; ++i)
            local[(size_t) i] = e[(size_t) i] / deviation;
    }

    for (int t = 0; t < n; ++t)
    {
        double bestPrevious = 0.0;
        int from = -1;

        for (auto tau = t - (int) std::round (2.0 * period); tau <= t - (int) std::round (period / 2.0); ++tau)
        {
            if (tau < 0)
                continue;

            const auto ratio = std::log ((t - tau) / period);
            const auto candidate = score[(size_t) tau] - tightness * ratio * ratio;

            if (from < 0 || candidate > bestPrevious)
            {
                bestPrevious = candidate;
                from = tau;
            }
        }

        score[(size_t) t] = local[(size_t) t] + (from >= 0 ? juce::jmax (0.0, bestPrevious) : 0.0);
        previous[(size_t) t] = from >= 0 && bestPrevious > 0.0 ? from : -1;
    }

    auto end = n - 1;
    for (auto t = juce::jmax (0, n - (int) std::round (period)); t < n; ++t)
        if (score[(size_t) t] > score[(size_t) end])
            end = t;

    std::vector<int> frames;
    for (auto t = end; t >= 0; t = previous[(size_t) t])
        frames.push_back (t);
    std::reverse (frames.begin(), frames.end());

    // Drop beats before the music starts and after it ends (the chain carries on through
    // silence at both ends).
    while (frames.size() > 1 && local[(size_t) frames.front()] < 0.5)
        frames.erase (frames.begin());
    while (frames.size() > 1 && local[(size_t) frames.back()] < 0.5)
        frames.pop_back();

    if (frames.size() < 4)
        return result;

    for (auto f : frames)
        result.beats.push_back (f * hop);

    double intercept = 0.0;
    const auto slope = fitLine (result.beats, intercept);
    if (slope <= 0.0)
        return result;

    result.bpm = 60.0 / slope;
    result.gridStart = intercept;

    // The downbeat: the beat in the bar that carries the most onset energy on average.
    const auto bar = juce::jlimit (2, 12, beatsPerBar);
    double strongest = -1.0;
    for (int phase = 0; phase < bar && phase < (int) frames.size(); ++phase)
    {
        double sum = 0.0;
        int count = 0;
        for (auto i = (size_t) phase; i < frames.size(); i += (size_t) bar, ++count)
            sum += local[(size_t) frames[i]];

        if (count > 0 && sum / count > strongest)
        {
            strongest = sum / count;
            result.firstDownbeat = phase;
        }
    }

    return result;
}

//==============================================================================
std::vector<double> makeGrid (double bpm, double beats, double from, double to, double origin, double swing)
{
    std::vector<double> grid;
    if (bpm <= 0.0 || beats <= 0.0 || to < from)
        return grid;

    const auto step = beats * 60.0 / bpm;
    const auto delay = beats < 1.0 ? juce::jlimit (0.0, 1.0, swing) * step / 3.0 : 0.0;

    for (auto k = (juce::int64) std::floor ((from - origin) / step) - 1; ; ++k)
    {
        const auto line = origin + (double) k * step + ((k & 1) != 0 ? delay : 0.0);
        if (line > to + step)
            break;
        grid.push_back (line);
    }

    return grid;
}

std::vector<double> makeGroove (const std::vector<double>& beats, double steps, double from, double to, double swing)
{
    std::vector<double> grid;
    if (beats.size() < 2 || steps <= 0.0 || to < from)
        return grid;

    // Every played beat, carried on at the edge spacing to cover from .. to.
    std::vector<double> lines (beats.begin(), beats.end());
    const auto firstGap = lines[1] - lines[0], lastGap = lines.back() - lines[lines.size() - 2];
    if (firstGap <= 0.0 || lastGap <= 0.0)
        return grid;

    while (lines.front() > from - 2.0 * firstGap)
        lines.insert (lines.begin(), lines.front() - firstGap);
    while (lines.back() < to + 2.0 * lastGap)
        lines.push_back (lines.back() + lastGap);

    if (steps >= 1.0)
    {
        const auto every = (size_t) juce::jmax (1, (int) std::round (steps));
        for (size_t i = 0; i < lines.size(); i += every)
            grid.push_back (lines[i]);
        return grid;
    }

    const auto divisions = juce::jmax (1, (int) std::round (1.0 / steps));
    for (size_t i = 0; i + 1 < lines.size(); ++i)
    {
        const auto gap = (lines[i + 1] - lines[i]) / divisions;
        for (int d = 0; d < divisions; ++d)
            grid.push_back (lines[i] + gap * d + ((d & 1) != 0 ? juce::jlimit (0.0, 1.0, swing) * gap / 3.0 : 0.0));
    }

    grid.push_back (lines.back());
    return grid;
}
}
