#include "TempoMap.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace daw
{
TempoMap::TempoMap (double bpm) : TempoMap (std::vector<Point> { { 0.0, bpm, false } }) {}

TempoMap::TempoMap (std::vector<Point> input)
{
    std::stable_sort (input.begin(), input.end(), [] (const Point& a, const Point& b) { return a.beat < b.beat; });

    for (auto p : input)
    {
        if (! std::isfinite (p.beat) || ! std::isfinite (p.bpm))
            continue;

        p.bpm = juce::jlimit (minBpm, maxBpm, p.bpm);
        p.beat = juce::jmax (0.0, p.beat);

        if (! points.empty() && p.beat - points.back().beat < 1.0 / 64.0)
            points.back() = { points.back().beat, p.bpm, points.size() > 1 && p.ramp };
        else
            points.push_back (p);
    }

    if (points.empty())
        points.push_back ({ 0.0, 120.0, false });

    points.front().beat = 0.0;
    points.front().ramp = false;

    startSeconds.assign (points.size(), 0.0);
    for (size_t i = 1; i < points.size(); ++i)
        startSeconds[i] = startSeconds[i - 1] + secondsWithin (i - 1, points[i].beat);
}

double TempoMap::slopeAfter (size_t i) const noexcept
{
    if (i + 1 >= points.size() || ! points[i + 1].ramp)
        return 0.0;
    return (points[i + 1].bpm - points[i].bpm) / (points[i + 1].beat - points[i].beat);
}

// With the tempo T(b) = T0 + k (b - b0), the time is the integral of 60 / T(b):
// 60 / k * ln (T(b) / T0), which is 60 (b - b0) / T0 when k is 0.
double TempoMap::secondsWithin (size_t i, double beat) const noexcept
{
    const auto& p = points[i];
    const auto k = slopeAfter (i), db = beat - p.beat;

    if (std::abs (k) < 1.0e-9 || db <= 0.0)
        return 60.0 * db / p.bpm;

    return 60.0 / k * std::log ((p.bpm + k * db) / p.bpm);
}

double TempoMap::beatsWithin (size_t i, double seconds) const noexcept
{
    const auto& p = points[i];
    const auto k = slopeAfter (i);

    if (std::abs (k) < 1.0e-9 || seconds <= 0.0)
        return seconds * p.bpm / 60.0;

    return p.bpm * (std::exp (k * seconds / 60.0) - 1.0) / k;
}

int TempoMap::getPointIndexAt (double beat) const noexcept
{
    const auto next = std::upper_bound (points.begin(), points.end(), beat, [] (double b, const Point& p) { return b < p.beat; });
    return (int) juce::jmax ((std::ptrdiff_t) 0, (next - points.begin()) - 1);
}

double TempoMap::toSeconds (double beat) const noexcept
{
    const auto i = (size_t) getPointIndexAt (beat);
    return startSeconds[i] + secondsWithin (i, beat);
}

double TempoMap::toBeats (double seconds) const noexcept
{
    const auto next = std::upper_bound (startSeconds.begin(), startSeconds.end(), seconds);
    const auto i = (size_t) juce::jmax ((std::ptrdiff_t) 0, (next - startSeconds.begin()) - 1);
    return points[i].beat + beatsWithin (i, seconds - startSeconds[i]);
}

double TempoMap::getBpmAtBeat (double beat) const noexcept
{
    const auto i = (size_t) getPointIndexAt (beat);
    return points[i].bpm + slopeAfter (i) * juce::jmax (0.0, beat - points[i].beat);
}

std::vector<double> TempoMap::makeGrid (double steps, double fromSeconds, double toSecondsValue, double swing) const
{
    std::vector<double> lines;
    steps = juce::jmax (1.0 / 64.0, steps);
    swing = steps < 1.0 ? juce::jlimit (0.0, 1.0, swing) : 0.0;

    // One line past each end, so a time near either end still has lines on both sides.
    const auto first = (long long) std::floor (toBeats (fromSeconds) / steps) - 1;
    const auto last = (long long) std::ceil (toBeats (toSecondsValue) / steps) + 1;

    for (auto n = first; n <= last && lines.size() < 1000000; ++n)
    {
        auto beat = (double) n * steps;
        if ((n & 1) != 0)
            beat += swing * steps / 3.0; // at 1, the off line sits two thirds through the pair
        lines.push_back (toSeconds (beat));
    }

    return lines;
}

double TempoMap::snap (double seconds, double steps) const noexcept
{
    steps = juce::jmax (1.0e-3, steps);
    return toSeconds (std::round (toBeats (seconds) / steps) * steps);
}

juce::String TempoMap::changesToString() const
{
    juce::StringArray parts;
    for (size_t i = 1; i < points.size(); ++i)
        parts.add (juce::String (points[i].beat, 6) + ":" + juce::String (points[i].bpm, 4) + ":" + (points[i].ramp ? "1" : "0"));
    return parts.joinIntoString (";");
}

TempoMap TempoMap::fromProject (double tempo, const juce::String& changes)
{
    std::vector<Point> parsed { { 0.0, tempo, false } };

    for (const auto& part : juce::StringArray::fromTokens (changes, ";", {}))
    {
        const auto fields = juce::StringArray::fromTokens (part, ":", {});
        if (fields.size() >= 2)
            parsed.push_back ({ fields[0].getDoubleValue(), fields[1].getDoubleValue(), fields.size() > 2 && fields[2].getIntValue() != 0 });
    }

    return TempoMap (std::move (parsed));
}

TempoMap TempoMap::fromDownbeats (std::vector<double> downbeats, int beatsPerBar, int* leadInBars)
{
    if (leadInBars != nullptr)
        *leadInBars = 0;

    downbeats.erase (std::remove_if (downbeats.begin(), downbeats.end(), [] (double t) { return ! std::isfinite (t); }), downbeats.end());
    std::sort (downbeats.begin(), downbeats.end());
    downbeats.erase (std::unique (downbeats.begin(), downbeats.end(), [] (double a, double b) { return b - a < 1.0e-3; }), downbeats.end());

    if (downbeats.size() < 2)
        return TempoMap (120.0);

    const auto bar = (double) juce::jlimit (1, 16, beatsPerBar);

    // Misplaced bar lines: bar k runs long and bar k + 1 short (or the other way round) by
    // more than 12 % of the local bar length, while the two together keep the pace.
    for (int pass = 0; pass < 2; ++pass)
        for (size_t k = 0; k + 2 < downbeats.size(); ++k)
        {
            std::vector<double> nearby;
            for (auto j = (size_t) juce::jmax (0, (int) k - 2); j <= juce::jmin (downbeats.size() - 2, k + 3); ++j)
                nearby.push_back (downbeats[j + 1] - downbeats[j]);
            std::nth_element (nearby.begin(), nearby.begin() + (long) nearby.size() / 2, nearby.end());
            const auto typical = nearby[nearby.size() / 2];

            const auto first = downbeats[k + 1] - downbeats[k], second = downbeats[k + 2] - downbeats[k + 1];
            if (std::abs (first - typical) > 0.12 * typical && std::abs (second - typical) > 0.12 * typical
                && std::abs (first + second - 2.0 * typical) < 0.08 * typical)
                downbeats[k + 1] = downbeats[k] + 0.5 * (first + second);
        }

    const auto bpmOf = [bar] (double seconds) { return juce::jlimit (minBpm, maxBpm, 60.0 * bar / seconds); };

    // The lead-in: whole bars from 0 s to the first downbeat, at the tempo nearest the first
    // bar's. When the music starts too soon after 0 s for a whole bar at a sensible pace
    // (the lead-in bar would run over 1.5 times the first bar's tempo), the first bar of
    // music joins the lead-in and the bar lines follow from the second downbeat on.
    int lead = 0;
    auto firstBarBpm = bpmOf (downbeats[1] - downbeats[0]);
    auto leadBpm = firstBarBpm;
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        lead = 0;
        firstBarBpm = bpmOf (downbeats[1] - downbeats[0]);
        leadBpm = firstBarBpm;
        const auto t0 = downbeats.front();
        if (t0 <= 1.0e-3)
            break;

        const auto barSeconds = 60.0 * bar / firstBarBpm;
        lead = juce::jmax (1, (int) std::lround (t0 / barSeconds));
        const auto exact = 60.0 * bar * lead / t0;
        leadBpm = juce::jlimit (minBpm, maxBpm, exact);
        if (exact <= 1.5 * firstBarBpm || downbeats.size() < 3 || attempt > 0)
            break;
        downbeats.erase (downbeats.begin());
    }
    const auto numBars = downbeats.size() - 1;

    std::vector<Point> points { { 0.0, leadBpm, false } };
    for (size_t k = 0; k < numBars; ++k)
    {
        const auto bpm = bpmOf (downbeats[k + 1] - downbeats[k]);
        if (std::abs (bpm - points.back().bpm) > 0.01)
            points.push_back ({ (lead + (double) k) * bar, bpm, false });
    }

    if (leadInBars != nullptr)
        *leadInBars = lead;
    return TempoMap (std::move (points));
}
}
