#include "VocalAlign.h"

#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace analysis::align
{
namespace
{
    constexpr int numBands = 24;
    constexpr double lowestBand = 120.0, highestBand = 7000.0;

    // What a step of the path costs (per frame): changing the shift by one frame, and a
    // little for every frame of shift (so of two equally good shifts the smaller wins).
    constexpr float changeCost = 0.08f, shiftCost = 0.03f;
    constexpr float matchedCost = 0.15f; // a sung frame "found the same sound" under this

    float percentile (std::vector<float> values, double share)
    {
        if (values.empty())
            return 0.0f;
        const auto index = (size_t) juce::jlimit (0, (int) values.size() - 1, (int) std::floor (share * (double) (values.size() - 1)));
        std::nth_element (values.begin(), values.begin() + (std::ptrdiff_t) index, values.end());
        return values[index];
    }

    // Douglas-Peucker on (time, shift): the frames to keep so that straight lines between
    // them stay within `tolerance` of every frame's shift.
    void simplify (const std::vector<double>& t, const std::vector<double>& s, int first, int last, double tolerance, std::vector<int>& keep)
    {
        if (last - first < 2)
            return;
        auto worst = -1;
        auto worstError = tolerance;
        for (int i = first + 1; i < last; ++i)
        {
            const auto along = (t[(size_t) i] - t[(size_t) first]) / juce::jmax (1.0e-9, t[(size_t) last] - t[(size_t) first]);
            const auto line = s[(size_t) first] + along * (s[(size_t) last] - s[(size_t) first]);
            if (const auto error = std::abs (s[(size_t) i] - line); error > worstError)
            {
                worstError = error;
                worst = i;
            }
        }
        if (worst < 0)
            return;
        simplify (t, s, first, worst, tolerance, keep);
        keep.push_back (worst);
        simplify (t, s, worst, last, tolerance, keep);
    }
}

float Features::loudLevel() const
{
    return level.empty() ? -200.0f : percentile (level, 0.9);
}

Features analyse (const juce::AudioBuffer<float>& audio, double sampleRate)
{
    Features features;
    features.bands = numBands;
    const auto length = audio.getNumSamples(), channels = audio.getNumChannels();
    if (length <= 0 || channels <= 0 || sampleRate < 8000.0)
        return features;

    std::vector<float> mono ((size_t) length, 0.0f);
    for (int ch = 0; ch < channels; ++ch)
    {
        const auto* data = audio.getReadPointer (ch);
        for (int i = 0; i < length; ++i)
            mono[(size_t) i] += data[i] / (float) channels;
    }

    // About 32 ms or more of window, a frame every 10 ms.
    int order = 8;
    while ((1 << order) < (int) (0.032 * sampleRate) && order < 14)
        ++order;
    const auto size = 1 << order;
    const auto hopSamples = juce::jmax (1, (int) std::lround (features.hop * sampleRate));
    features.hop = hopSamples / sampleRate;
    const auto frames = length / hopSamples + 1;

    juce::dsp::FFT fft (order);
    std::vector<float> window ((size_t) size), buffer ((size_t) size * 2), cepstrum ((size_t) size * 2);
    const auto lifter = juce::jmax (4, (int) std::lround (sampleRate / 800.0));
    for (int i = 0; i < size; ++i)
        window[(size_t) i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) size);

    // Bands spaced evenly in pitch, as FFT bins.
    std::vector<int> edges (numBands + 1);
    const auto top = juce::jmin (highestBand, 0.45 * sampleRate);
    for (int b = 0; b <= numBands; ++b)
    {
        const auto hz = lowestBand * std::pow (top / lowestBand, (double) b / numBands);
        edges[(size_t) b] = juce::jlimit (1, size / 2, (int) std::lround (hz * size / sampleRate));
    }
    for (int b = 1; b <= numBands; ++b)
        edges[(size_t) b] = juce::jmax (edges[(size_t) b], edges[(size_t) b - 1] + 1);

    features.shape.assign ((size_t) frames * numBands, 0.0f);
    features.onset.assign ((size_t) frames, 0.0f);
    features.level.assign ((size_t) frames, -200.0f);
    std::vector<float> logBands ((size_t) frames * numBands, -100.0f);

    for (int f = 0; f < frames; ++f)
    {
        const auto start = f * hopSamples - size / 2;
        double power = 0.0;
        std::fill (buffer.begin(), buffer.end(), 0.0f);
        for (int i = 0; i < size; ++i)
        {
            const auto at = start + i;
            const auto x = at >= 0 && at < length ? mono[(size_t) at] : 0.0f;
            buffer[(size_t) i] = x * window[(size_t) i];
            power += (double) x * x;
        }
        features.level[(size_t) f] = (float) (10.0 * std::log10 (power / size + 1.0e-20));
        fft.performFrequencyOnlyForwardTransform (buffer.data(), true);

        auto* bands = logBands.data() + (size_t) f * numBands;
        for (int b = 0; b < numBands; ++b)
        {
            double sum = 0.0;
            for (int k = edges[(size_t) b]; k < edges[(size_t) b + 1]; ++k)
                sum += (double) buffer[(size_t) k] * buffer[(size_t) k];
            bands[b] = (float) (10.0 * std::log10 (sum / (edges[(size_t) b + 1] - edges[(size_t) b]) + 1.0e-12));
        }

        // The spectrum's envelope, without the comb of the sung pitch's harmonics (so a
        // harmony on another note, or a lower voice, still has the same vowel): the log
        // spectrum through its cepstrum, keeping only the slow ripples (under 1.25 ms).
        for (int k = 0; k <= size / 2; ++k)
        {
            const auto value = std::log (buffer[(size_t) k] + 1.0e-6f);
            cepstrum[(size_t) k * 2] = value;
            cepstrum[(size_t) k * 2 + 1] = 0.0f;
            if (k > 0 && k < size / 2)
            {
                cepstrum[(size_t) (size - k) * 2] = value;
                cepstrum[(size_t) (size - k) * 2 + 1] = 0.0f;
            }
        }
        fft.performRealOnlyInverseTransform (cepstrum.data());
        for (int n = lifter + 1; n < size - lifter; ++n)
            cepstrum[(size_t) n] = 0.0f;
        std::fill (cepstrum.begin() + size, cepstrum.end(), 0.0f);
        fft.performRealOnlyForwardTransform (cepstrum.data());

        float envelope[numBands];
        for (int b = 0; b < numBands; ++b)
        {
            double sum = 0.0;
            for (int k = edges[(size_t) b]; k < edges[(size_t) b + 1]; ++k)
                sum += cepstrum[(size_t) k * 2];
            envelope[b] = (float) (sum / (edges[(size_t) b + 1] - edges[(size_t) b]));
        }

        // The spectrum's shape: its envelope less its mean, at length 1.
        float mean = 0.0f;
        for (int b = 0; b < numBands; ++b)
            mean += envelope[b] / numBands;
        auto* shape = features.shape.data() + (size_t) f * numBands;
        double norm = 0.0;
        for (int b = 0; b < numBands; ++b)
        {
            shape[b] = envelope[b] - mean;
            norm += (double) shape[b] * shape[b];
        }
        if (norm > 1.0e-6)
            for (int b = 0; b < numBands; ++b)
                shape[b] = (float) (shape[b] / std::sqrt (norm));
        else
            std::fill (shape, shape + numBands, 0.0f);
    }

    // New sound arriving: the rise of the log bands since the frame before, over all bands,
    // relative to the strong attacks of the recording. Nothing in near silence.
    const auto loud = features.loudLevel();
    // Sound dying away (a note's end) the same way, from the fall of the log bands.
    std::vector<float> flux ((size_t) frames, 0.0f), fall ((size_t) frames, 0.0f);
    for (int f = 1; f < frames; ++f)
    {
        float rise = 0.0f, drop = 0.0f;
        for (int b = 0; b < numBands; ++b)
        {
            const auto now = juce::jmax (logBands[(size_t) f * numBands + (size_t) b], loud - 60.0f);
            const auto before = juce::jmax (logBands[(size_t) (f - 1) * numBands + (size_t) b], loud - 60.0f);
            rise += juce::jmax (0.0f, now - before);
            drop += juce::jmax (0.0f, before - now);
        }
        flux[(size_t) f] = rise;
        fall[(size_t) f] = drop;
    }
    const auto strong = juce::jmax (1.0f, percentile (flux, 0.98)), strongFall = juce::jmax (1.0f, percentile (fall, 0.98));
    features.fall.assign ((size_t) frames, 0.0f);
    for (int f = 0; f < frames; ++f)
    {
        features.onset[(size_t) f] = juce::jmin (1.0f, flux[(size_t) f] / strong);
        features.fall[(size_t) f] = juce::jmin (1.0f, fall[(size_t) f] / strongFall);
    }

    return features;
}

Result align (const Features& guide, const Features& dub, const Options& options, double from, double to)
{
    Result result;
    if (guide.size() == 0 || dub.size() == 0 || guide.bands != dub.bands || std::abs (guide.hop - dub.hop) > 1.0e-6)
        return result;

    const auto hop = dub.hop;
    const auto bands = dub.bands;
    const auto first = juce::jlimit (0, dub.size(), (int) std::floor (juce::jmax (0.0, from) / hop));
    const auto last = to < 0.0 ? dub.size() : juce::jlimit (first, dub.size(), (int) std::ceil (to / hop) + 1);
    const auto count = last - first;
    if (count < 3)
        return result;

    const auto reach = juce::jmax (1, (int) std::lround (juce::jlimit (0.01, 2.0, options.maxShift) / hop));
    const auto states = 2 * reach + 1;

    // Gaps: frames far under the recording's loud level, or close to its noise (the
    // quiet tenth of it), and anything under -70 dBFS.
    const auto gapLevel = [&] (const Features& f, float loud)
    {
        const auto noise = percentile (f.level, 0.1);
        return juce::jmax (-70.0f, loud - options.silenceDb, juce::jmin (loud - 12.0f, noise + 10.0f));
    };
    const auto dubLoud = dub.loudLevel(), guideLoud = guide.loudLevel();
    const auto dubFloor = gapLevel (dub, dubLoud), guideFloor = gapLevel (guide, guideLoud);
    std::vector<bool> active ((size_t) count);
    for (int i = 0; i < count; ++i)
        active[(size_t) i] = dub.level[(size_t) (first + i)] >= dubFloor;

    // How different the dub's frame i sounds from the guide's frame j.
    const auto cost = [&] (int i, int j) -> float
    {
        if (! active[(size_t) (i - first)])
            return 0.0f;
        if (j < 0 || j >= guide.size() || guide.level[(size_t) j] < guideFloor)
            return 1.0f;
        const auto* a = dub.shape.data() + (size_t) i * (size_t) bands;
        const auto* b = guide.shape.data() + (size_t) j * (size_t) bands;
        float dot = 0.0f;
        for (int k = 0; k < bands; ++k)
            dot += a[k] * b[k];
        const auto spectrum = juce::jmin (1.0f, (1.0f - dot));
        const auto levels = std::abs (juce::jlimit (-30.0f, 0.0f, dub.level[(size_t) i] - dubLoud) - juce::jlimit (-30.0f, 0.0f, guide.level[(size_t) j] - guideLoud)) / 30.0f;
        return 0.55f * spectrum + 0.2f * std::abs (dub.onset[(size_t) i] - guide.onset[(size_t) j])
             + 0.1f * std::abs (dub.fall[(size_t) i] - guide.fall[(size_t) j]) + 0.15f * levels;
    };

    // The cheapest path through every shift, frame by frame (the shift moves at most one
    // frame per frame, so the dub plays between standing still and twice as fast). With a
    // lag, against the guide moved that far round its own span: how well the dub matches
    // a part of the guide it doesn't sing, which says what "matching" is worth by chance.
    std::vector<float> total ((size_t) states), next ((size_t) states);
    std::vector<int8_t> back ((size_t) count * (size_t) states, 0);
    const auto prior = [&] (int s) { return shiftCost * (float) std::abs (s - reach) / (float) reach; };
    const auto spanStart = juce::jmax (0, first - reach), spanEnd = juce::jmin (guide.size(), last + reach);
    const auto span = spanEnd - spanStart;

    const auto findPath = [&] (int lag, int& sung, int& matched)
    {
        const auto at = [&] (int j) { return lag == 0 ? j : spanStart + ((j - spanStart + lag) % span + span) % span; };
        for (int s = 0; s < states; ++s)
            total[(size_t) s] = cost (first, at (first + s - reach)) + prior (s);

        for (int i = 1; i < count; ++i)
        {
            const auto frame = first + i;
            auto* steps = back.data() + (size_t) i * (size_t) states;
            for (int s = 0; s < states; ++s)
            {
                auto best = total[(size_t) s];
                int8_t step = 0;
                if (s > 0 && total[(size_t) s - 1] + changeCost < best)
                {
                    best = total[(size_t) s - 1] + changeCost;
                    step = -1;
                }
                if (s + 1 < states && total[(size_t) s + 1] + changeCost < best)
                {
                    best = total[(size_t) s + 1] + changeCost;
                    step = 1;
                }
                next[(size_t) s] = best + cost (frame, at (frame + s - reach)) + prior (s);
                steps[s] = step;
            }
            std::swap (total, next);
        }

        std::vector<int> found ((size_t) count);
        found.back() = (int) (std::min_element (total.begin(), total.end()) - total.begin());
        for (int i = count - 1; i > 0; --i)
            found[(size_t) i - 1] = found[(size_t) i] + back[(size_t) i * (size_t) states + (size_t) found[(size_t) i]];

        // How much of what the dub sings found the same sound in the guide.
        sung = matched = 0;
        for (int i = 0; i < count; ++i)
            if (active[(size_t) i])
            {
                ++sung;
                if (cost (first + i, at (first + i + found[(size_t) i] - reach)) < matchedCost)
                    ++matched;
            }
        return found;
    };

    int sung = 0, matched = 0;
    const auto path = findPath (0, sung, matched);
    result.match = sung > 0 ? (float) matched / (float) sung : 0.0f;
    result.related = sung > 0 && result.match >= minMatch;

    // By chance (the guide half its span round, when that's well past the reach).
    if (result.related && span / 2 > 4 * reach)
    {
        int chanceSung = 0, chanceMatched = 0;
        findPath (span / 2, chanceSung, chanceMatched);
        result.chance = chanceSung > 0 ? (float) chanceMatched / (float) chanceSung : 0.0f;
        result.related = result.match - result.chance >= minLead;
    }
    result.shift.assign ((size_t) count, 0.0f);
    if (! result.related)
        return result;

    // Phrases: the sung frames, with gaps under 60 ms closed and blips under 40 ms left out.
    struct Region { int start, end; }; // frames within [0, count), end exclusive
    std::vector<Region> regions;
    for (int i = 0; i < count;)
    {
        if (! active[(size_t) i]) { ++i; continue; }
        auto end = i;
        while (end < count && active[(size_t) end])
            ++end;
        if (! regions.empty() && (i - regions.back().end) * hop < 0.06)
            regions.back().end = end;
        else
            regions.push_back ({ i, end });
        i = end;
    }
    regions.erase (std::remove_if (regions.begin(), regions.end(), [hop] (const Region& r) { return (r.end - r.start) * hop < 0.04; }),
                   regions.end());

    // The path's shift in each phrase, smoothed (a median, then an average over 70 ms), as
    // markers a straight line apart.
    std::vector<Marker> markers;
    std::vector<bool> inPhrase; // the stretch from the marker before to this one is inside a phrase
    double shiftSum = 0.0;
    int shiftCount = 0;
    for (const auto& region : regions)
    {
        const auto n = region.end - region.start;
        std::vector<double> raw ((size_t) n), smooth ((size_t) n), times ((size_t) n);
        for (int k = 0; k < n; ++k)
            raw[(size_t) k] = (path[(size_t) (region.start + k)] - reach) * hop;
        std::vector<double> median ((size_t) n);
        for (int k = 0; k < n; ++k)
        {
            double window[5];
            int w = 0;
            for (int d = -2; d <= 2; ++d)
                window[w++] = raw[(size_t) juce::jlimit (0, n - 1, k + d)];
            std::nth_element (window, window + 2, window + 5);
            median[(size_t) k] = window[2];
        }
        for (int k = 0; k < n; ++k)
        {
            double sum = 0.0;
            for (int d = -3; d <= 3; ++d)
                sum += median[(size_t) juce::jlimit (0, n - 1, k + d)];
            smooth[(size_t) k] = sum / 7.0;
            times[(size_t) k] = (first + region.start + k) * hop;
            result.shift[(size_t) (region.start + k)] = (float) smooth[(size_t) k];
            shiftSum += std::abs (smooth[(size_t) k]);
            result.largestShift = juce::jmax (result.largestShift, std::abs (smooth[(size_t) k]));
            ++shiftCount;
        }

        std::vector<int> keep { 0 };
        simplify (times, smooth, 0, n - 1, juce::jmax (0.001, options.tolerance), keep);
        if (n > 1)
            keep.push_back (n - 1);
        std::sort (keep.begin(), keep.end());
        for (size_t k = 0; k < keep.size(); ++k)
        {
            const auto at = (size_t) keep[k];
            markers.push_back ({ times[at], times[at] + juce::jlimit (0.0, 1.0, options.strength) * smooth[at] });
            inPhrase.push_back (k > 0);
        }
    }
    result.meanShift = shiftCount > 0 ? shiftSum / shiftCount : 0.0;

    // Within the stretch limits: each marker lands no closer to, or farther from, the one
    // before than the limit lets the time between them play.
    const auto voiceLimit = juce::jlimit (1.05, 4.0, options.maxStretch);
    const auto gapLimit = juce::jlimit (voiceLimit, 8.0, options.gapStretch);
    for (size_t k = 1; k < markers.size(); ++k)
    {
        const auto limit = inPhrase[k] ? voiceLimit : gapLimit;
        const auto span = markers[k].source - markers[k - 1].source;
        markers[k].target = juce::jlimit (markers[k - 1].target + span / limit, markers[k - 1].target + span * limit, markers[k].target);
    }

    // Nothing to move: no markers.
    const auto moves = std::any_of (markers.begin(), markers.end(), [] (const Marker& m) { return std::abs (m.target - m.source) > 0.0005; });
    if (moves)
        result.markers = std::move (markers);
    return result;
}

Result align (const juce::AudioBuffer<float>& guide, const juce::AudioBuffer<float>& dub, double sampleRate, const Options& options)
{
    return align (analyse (guide, sampleRate), analyse (dub, sampleRate), options);
}

double mapTime (const std::vector<Marker>& markers, double seconds)
{
    if (markers.empty())
        return seconds;
    if (seconds <= markers.front().source)
        return seconds + markers.front().target - markers.front().source;
    if (seconds >= markers.back().source)
        return seconds + markers.back().target - markers.back().source;
    const auto after = std::upper_bound (markers.begin(), markers.end(), seconds, [] (double t, const Marker& m) { return t < m.source; });
    const auto& b = *after;
    const auto& a = *(after - 1);
    const auto along = (seconds - a.source) / juce::jmax (1.0e-9, b.source - a.source);
    return a.target + along * (b.target - a.target);
}
}
