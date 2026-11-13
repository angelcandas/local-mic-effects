#include "Hitpoints.h"

#include <algorithm>
#include <cmath>

namespace daw::hitpoints
{
bool Filter::operator== (const Filter& o) const noexcept
{
    return std::abs (threshold - o.threshold) < 1.0e-6 && std::abs (minLength - o.minLength) < 1.0e-6 && std::abs (beats - o.beats) < 1.0e-6;
}

Filter parseFilter (const juce::String& text)
{
    Filter filter;
    const auto parts = juce::StringArray::fromTokens (text, ":", {});
    if (parts.size() >= 1 && parts[0].isNotEmpty()) filter.threshold = juce::jlimit (0.0, 1.0, parts[0].getDoubleValue());
    if (parts.size() >= 2 && parts[1].isNotEmpty()) filter.minLength = juce::jlimit (0.0, 2.0, parts[1].getDoubleValue() / 1000.0);
    if (parts.size() >= 3 && parts[2].isNotEmpty()) filter.beats = juce::jlimit (0.0, 4.0, parts[2].getDoubleValue());
    return filter;
}

juce::String toString (const Filter& filter)
{
    return juce::String (filter.threshold, 3) + ":" + juce::String (filter.minLength * 1000.0, 1) + ":" + juce::String (filter.beats, 4);
}

std::vector<HandEdit> parseEdits (const juce::String& text)
{
    std::vector<HandEdit> edits;
    for (const auto& part : juce::StringArray::fromTokens (text, ";", {}))
    {
        const auto time = part.upToFirstOccurrenceOf (":", false, false);
        const auto flag = part.fromFirstOccurrenceOf (":", false, false).trim();
        if (time.isEmpty() || flag.isEmpty())
            continue;
        const auto state = flag == "-" ? State::disabled : flag == "=" ? State::locked : State::added;
        edits.push_back ({ time.getDoubleValue(), state });
    }
    std::sort (edits.begin(), edits.end(), [] (const HandEdit& a, const HandEdit& b) { return a.time < b.time; });
    return edits;
}

juce::String toString (const std::vector<HandEdit>& edits)
{
    juce::StringArray parts;
    for (const auto& e : edits)
        if (e.state != State::automatic)
            parts.add (juce::String (e.time, 6) + ":" + (e.state == State::disabled ? "-" : e.state == State::locked ? "=" : "+"));
    return parts.joinIntoString (";");
}

std::vector<Hit> select (const std::vector<analysis::rhythm::OnsetCandidate>& candidates, double from, double to, const Filter& filter,
                         const std::vector<HandEdit>& edits, const std::function<double (double)>& beatOf)
{
    // 1. Clear enough, and in range.
    const auto margin = analysis::rhythm::marginForThreshold (filter.threshold);
    std::vector<Hit> picked;
    for (const auto& c : candidates)
        if (c.time >= from - 1.0e-9 && c.time <= to + 1.0e-9 && c.margin >= margin - 1.0e-6f)
            picked.push_back ({ c.time, c.strength, c.peak, State::automatic });

    // 2. Beats: only those near the grid of that note value (as the clip plays now).
    if (filter.beats > 0.0 && beatOf)
    {
        const auto tolerance = filter.beats * 0.12;
        picked.erase (std::remove_if (picked.begin(), picked.end(), [&] (const Hit& h)
        {
            const auto beat = beatOf (h.time);
            const auto off = beat - std::round (beat / filter.beats) * filter.beats;
            return std::abs (off) > tolerance;
        }), picked.end());
    }

    // 3. Minimum length: of two closer than that, the stronger stays.
    if (filter.minLength > 0.0)
    {
        auto byStrength = picked;
        std::stable_sort (byStrength.begin(), byStrength.end(), [] (const Hit& a, const Hit& b) { return a.strength > b.strength; });
        std::vector<Hit> kept;
        for (const auto& h : byStrength)
            if (std::none_of (kept.begin(), kept.end(), [&] (const Hit& k) { return std::abs (k.time - h.time) < filter.minLength - 1.0e-9; }))
                kept.push_back (h);
        std::sort (kept.begin(), kept.end(), [] (const Hit& a, const Hit& b) { return a.time < b.time; });
        picked = std::move (kept);
    }

    // 4. Hand edits win over the filter.
    const auto nearest = [] (std::vector<Hit>& list, double time) -> Hit*
    {
        Hit* best = nullptr;
        auto distance = matchSeconds;
        for (auto& h : list)
            if (const auto d = std::abs (h.time - time); d <= distance)
            {
                distance = d;
                best = &h;
            }
        return best;
    };
    const auto candidateAt = [&] (double time) -> const analysis::rhythm::OnsetCandidate*
    {
        const analysis::rhythm::OnsetCandidate* best = nullptr;
        auto distance = matchSeconds;
        for (const auto& c : candidates)
            if (const auto d = std::abs (c.time - time); d <= distance)
            {
                distance = d;
                best = &c;
            }
        return best;
    };

    for (const auto& edit : edits)
    {
        if (edit.time < from - 1.0e-9 || edit.time > to + 1.0e-9)
            continue;

        if (edit.state == State::disabled)
        {
            if (auto* h = nearest (picked, edit.time))
                h->state = State::disabled;
            continue;
        }

        // Added or locked: there whatever the filter says, exactly where it was put.
        if (auto* h = nearest (picked, edit.time))
        {
            h->time = edit.time;
            h->state = edit.state;
            continue;
        }
        Hit hit { edit.time, 0.5f, 0.0f, edit.state };
        if (const auto* c = candidateAt (edit.time))
        {
            hit.strength = c->strength;
            hit.peak = c->peak;
        }
        picked.push_back (hit);
    }

    std::sort (picked.begin(), picked.end(), [] (const Hit& a, const Hit& b) { return a.time < b.time; });
    return picked;
}

bool hasStored (const juce::ValueTree& clip)
{
    return clip.hasProperty (ids::hitFilter) || clip.hasProperty (ids::hitpoints);
}

bool hasEdits (const juce::ValueTree& clip)
{
    return ! parseEdits (clip[ids::hitpoints].toString()).empty();
}

Filter getFilter (const juce::ValueTree& clip)
{
    return clip.hasProperty (ids::hitFilter) ? parseFilter (clip[ids::hitFilter].toString()) : Filter();
}

std::vector<HandEdit> getEdits (const juce::ValueTree& clip)
{
    return parseEdits (clip[ids::hitpoints].toString());
}

std::function<std::vector<Hit>()> prepare (const juce::ValueTree& clip, AudioPool& pool, std::optional<Filter> filter)
{
    if (! clip.isValid() || Edit::isMidiClip (clip))
        return {};

    auto source = pool.loadNow (juce::File (clip[ids::file].toString()));
    if (source == nullptr)
        return {};

    const auto map = Edit::getEffectiveWarp (clip);
    const auto offset = (double) clip[ids::offset], length = (double) clip[ids::length];
    const auto timelineOffset = (double) clip[ids::start] - offset;
    const auto tempo = Edit::getTempoMapFor (clip);
    const auto rate = pool.getSampleRate();
    const auto chosen = filter.value_or (getFilter (clip));
    const auto edits = getEdits (clip);

    return [source, map, offset, length, timelineOffset, tempo, rate, chosen, edits]
    {
        const auto candidates = analysis::rhythm::findOnsetCandidatesCached (*source, rate);
        const auto from = juce::jmax (0.0, map.toSource (offset)), to = map.toSource (offset + length);
        return select (*candidates, from, to, chosen, edits,
                       [&] (double s) { return tempo.toBeats (map.toTarget (s) + timelineOffset); });
    };
}

std::vector<Hit> get (const juce::ValueTree& clip, AudioPool& pool, std::optional<Filter> filter)
{
    const auto find = prepare (clip, pool, filter);
    return find ? find() : std::vector<Hit>();
}

std::vector<double> enabledTimes (const std::vector<Hit>& hits)
{
    std::vector<double> times;
    for (const auto& h : hits)
        if (h.isEnabled())
            times.push_back (h.time);
    return times;
}

//==============================================================================
Groove makeGroove (const juce::String& name, const std::vector<double>& timelineHits, const TempoMap& tempo, int beatsPerBar, int bars)
{
    Groove groove;
    groove.name = name;
    groove.bars = juce::jlimit (1, 8, bars);
    const auto span = (double) groove.bars * beatsPerBar;

    std::vector<double> offsets;
    for (const auto t : timelineHits)
    {
        const auto beat = tempo.toBeats (t);
        offsets.push_back (beat - std::floor (beat / span) * span);
    }
    std::sort (offsets.begin(), offsets.end());

    // Hits of different bars at about the same place become one (their average).
    std::vector<std::pair<double, int>> merged;
    for (const auto o : offsets)
    {
        if (! merged.empty() && o - merged.back().first / merged.back().second < 0.12)
        {
            merged.back().first += o;
            ++merged.back().second;
        }
        else
            merged.push_back ({ o, 1 });
    }
    for (const auto& [sum, count] : merged)
        groove.offsets.push_back (sum / count);

    // One near the bar's end is the next bar's first.
    if (groove.offsets.size() > 1 && span - groove.offsets.back() < 0.12)
        groove.offsets.pop_back();
    return groove;
}

std::vector<Groove> parseGrooves (const juce::String& text)
{
    std::vector<Groove> grooves;
    for (const auto& part : juce::StringArray::fromTokens (text, "|", {}))
    {
        Groove g;
        g.name = part.upToFirstOccurrenceOf ("=", false, false).trim();
        const auto rest = part.fromFirstOccurrenceOf ("=", false, false);
        g.bars = juce::jlimit (1, 8, rest.upToFirstOccurrenceOf (":", false, false).getIntValue());
        for (const auto& o : juce::StringArray::fromTokens (rest.fromFirstOccurrenceOf (":", false, false), ",", {}))
            if (o.isNotEmpty())
                g.offsets.push_back (o.getDoubleValue());
        if (g.name.isNotEmpty() && ! g.offsets.empty())
            grooves.push_back (std::move (g));
    }
    return grooves;
}

juce::String toString (const std::vector<Groove>& grooves)
{
    juce::StringArray parts;
    for (const auto& g : grooves)
    {
        juce::StringArray offsets;
        for (const auto o : g.offsets)
            offsets.add (juce::String (o, 4));
        parts.add (g.name.removeCharacters ("=|:") + "=" + juce::String (g.bars) + ":" + offsets.joinIntoString (","));
    }
    return parts.joinIntoString ("|");
}

std::vector<double> makeGrid (const Groove& groove, const TempoMap& tempo, int beatsPerBar, double from, double to)
{
    std::vector<double> grid;
    if (groove.offsets.empty())
        return grid;
    const auto span = (double) groove.bars * beatsPerBar;
    const auto first = std::floor (tempo.toBeats (juce::jmax (0.0, from)) / span) - 1.0;
    for (auto cycle = juce::jmax (0.0, first); ; cycle += 1.0)
    {
        auto past = false;
        for (const auto o : groove.offsets)
        {
            const auto t = tempo.toSeconds (cycle * span + o);
            if (t > to) { past = true; break; }
            if (t >= from) grid.push_back (t);
        }
        if (past)
            break;
    }
    return grid;
}
}
