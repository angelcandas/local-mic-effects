#include "Edit.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <set>

// The arranger part of Edit (declared in Edit.h): rearranging the song by stretches of
// musical time (moving, duplicating and deleting sections, Delete time, Insert silence),
// and hand-made sections.
namespace daw
{
namespace
{
    constexpr double beatEpsilon = 1.0e-6;
    constexpr double edgeSeconds = 0.001; // a lane's value just before a piece ends

    // The pieces whose source holds `beat`, in the order given.
    std::vector<size_t> piecesAt (const std::vector<Edit::ArrangePiece>& pieces, double beat)
    {
        std::vector<size_t> found;
        for (size_t i = 0; i < pieces.size(); ++i)
            if (beat >= pieces[i].fromBeat - beatEpsilon && beat < pieces[i].toBeat - beatEpsilon)
                found.push_back (i);
        return found;
    }

    juce::String capitalised (const juce::String& word)
    {
        return word.substring (0, 1).toUpperCase() + word.substring (1);
    }

    const juce::StringArray& sectionKinds()
    {
        static const juce::StringArray kinds { "intro", "verse", "chorus", "bridge", "outro", "part" };
        return kinds;
    }
}

//==============================================================================
void Edit::rearrange (const std::vector<ArrangePiece>& input)
{
    std::vector<ArrangePiece> pieces;
    for (auto piece : input)
    {
        piece.fromBeat = juce::jmax (0.0, piece.fromBeat);
        piece.atBeat = juce::jmax (0.0, piece.atBeat);
        if (std::isfinite (piece.fromBeat) && std::isfinite (piece.atBeat) && piece.toBeat > piece.fromBeat + beatEpsilon)
            pieces.push_back (piece);
    }
    if (pieces.empty())
        return;

    const auto old = getTempoMap();
    const auto isEnd = [] (double beat) { return beat >= songEndBeat * 0.5; };

    //---------------------------------------------------------------- cut at every edge
    std::vector<double> cuts;
    for (const auto& piece : pieces)
    {
        cuts.push_back (piece.fromBeat);
        if (! isEnd (piece.toBeat))
            cuts.push_back (piece.toBeat);
    }
    std::sort (cuts.begin(), cuts.end());
    cuts.erase (std::unique (cuts.begin(), cuts.end(), [] (double a, double b) { return std::abs (a - b) < beatEpsilon; }), cuts.end());

    // Where clips live on the timeline: a track's own (the comp) and its take lanes.
    const auto containers = [this]
    {
        juce::Array<juce::ValueTree> found;
        for (const auto& track : getTracks())
        {
            found.add (track);
            for (const auto& child : track)
                if (child.hasType (ids::TAKE))
                    found.add (child);
        }
        return found;
    };
    const auto clipsIn = [] (const juce::ValueTree& container)
    {
        juce::Array<juce::ValueTree> clips;
        for (const auto& child : container)
            if (child.hasType (ids::CLIP))
                clips.add (child);
        return clips;
    };

    for (const auto beat : cuts)
    {
        const auto at = old.toSeconds (beat);
        for (const auto& container : containers())
            for (const auto& clip : clipsIn (container))
            {
                const auto start = (double) clip[ids::start], end = start + (double) clip[ids::length];
                if (start < at - minClipSeconds && end > at + minClipSeconds)
                    splitClip ((int) clip[ids::id], at);
            }
        for (const auto& block : getPatternClips())
            splitPatternClip ((int) block[ids::id], beat);
    }

    //---------------------------------------------------------------- the new tempo map
    // Each piece takes the tempo it had: a step to the tempo at its start, then its own points.
    auto byPlace = pieces;
    std::stable_sort (byPlace.begin(), byPlace.end(), [] (const auto& a, const auto& b) { return a.atBeat < b.atBeat; });
    std::vector<TempoMap::Point> tempoPoints;
    for (const auto& piece : byPlace)
    {
        tempoPoints.push_back ({ piece.atBeat, old.getBpmAtBeat (piece.fromBeat), false });
        for (const auto& point : old.getPoints())
            if (point.beat > piece.fromBeat + beatEpsilon && point.beat < piece.toBeat - beatEpsilon)
                tempoPoints.push_back ({ point.beat - piece.fromBeat + piece.atBeat, point.bpm, point.ramp });
    }
    const auto tempo = old.isSteady() ? old : TempoMap (std::move (tempoPoints));

    const auto shiftOf = [] (const ArrangePiece& piece) { return piece.atBeat - piece.fromBeat; };
    const auto mapSeconds = [&] (double seconds, const ArrangePiece& piece)
    {
        return juce::jmax (0.0, tempo.toSeconds (old.toBeats (seconds) + shiftOf (piece)));
    };

    //---------------------------------------------------------------- what goes where (read first)
    struct ClipPlan
    {
        juce::ValueTree clip;
        std::vector<size_t> pieces;
        double start, anchorBeat, startBeat, endBeat;
    };
    std::vector<ClipPlan> clipPlans;
    for (const auto& container : containers())
        for (const auto& clip : clipsIn (container))
        {
            const auto start = (double) clip[ids::start], offset = (double) clip[ids::offset], length = (double) clip[ids::length];
            clipPlans.push_back ({ clip, piecesAt (pieces, old.toBeats (start + length * 0.5)), start,
                                   old.toBeats (start - offset), old.toBeats (start), old.toBeats (start + length) });
        }

    struct BlockPlan { juce::ValueTree block; std::vector<size_t> pieces; };
    std::vector<BlockPlan> blockPlans;
    for (const auto& block : getPatternClips())
        blockPlans.push_back ({ block, piecesAt (pieces, (double) block[ids::start] + (double) block[ids::length] * 0.5) });

    struct MarkerPlan { juce::ValueTree marker; std::vector<size_t> pieces; double time, endBeat; };
    std::vector<MarkerPlan> markerPlans;
    for (const auto& marker : getMarkers())
    {
        const auto time = (double) marker[ids::time];
        markerPlans.push_back ({ marker, piecesAt (pieces, old.toBeats (time) + beatEpsilon), time,
                                 old.toBeats (time + (double) marker.getProperty (ids::length, 0.0)) });
    }

    // Chords: each mark with its stretch, and the chord in force where each piece starts.
    const auto chords = getChords();
    std::vector<ChordMark> newChords;
    if (! chords.empty())
    {
        for (const auto& piece : pieces)
        {
            const ChordMark* inForce = nullptr;
            for (const auto& mark : chords)
                if (mark.beat <= piece.fromBeat + beatEpsilon)
                    inForce = &mark;
            if (inForce != nullptr)
                newChords.push_back ({ piece.atBeat, inForce->name });
            for (const auto& mark : chords)
                if (mark.beat > piece.fromBeat + beatEpsilon && mark.beat < piece.toBeat - beatEpsilon)
                    newChords.push_back ({ mark.beat + shiftOf (piece), mark.name });
        }
    }

    // Automation lanes (every channel's): each piece's points, with its values at its edges so its shape holds.
    struct LanePlan { int track; juce::String target; std::vector<AutomationPoint> points; };
    std::vector<LanePlan> lanePlans;
    std::vector<std::pair<int, juce::ValueTree>> channels; // tracks, returns and the master have lanes
    for (const auto& track : getTracks())
        channels.push_back ({ (int) track[ids::id], track });
    for (const auto& bus : getReturns())
        channels.push_back ({ (int) bus[ids::id], bus });
    channels.push_back ({ masterId, getMaster() });
    for (const auto& [channelId, channel] : channels)
        for (const auto& target : getLaneTargets (channel))
        {
            const auto points = getLane (channel, target);
            if (points.empty())
                continue;
            std::vector<AutomationPoint> moved;
            for (const auto& piece : pieces)
            {
                const auto from = old.toSeconds (piece.fromBeat);
                const auto to = isEnd (piece.toBeat) ? std::numeric_limits<double>::max() : old.toSeconds (piece.toBeat);
                moved.push_back ({ mapSeconds (from, piece), getAutomationValueAt (points, from) });
                for (const auto& point : points)
                    if (point.time > from + 1.0e-6 && point.time < to - edgeSeconds)
                        moved.push_back ({ mapSeconds (point.time, piece), point.value });
                if (! isEnd (piece.toBeat))
                    moved.push_back ({ mapSeconds (to, piece) - edgeSeconds, getAutomationValueAt (points, to - edgeSeconds) });
            }
            std::stable_sort (moved.begin(), moved.end(), [] (const auto& a, const auto& b) { return a.time < b.time; });
            lanePlans.push_back ({ channelId, target, thinPoints (moved, 1.0e-5f) });
        }

    const auto loopStart = (double) state.getProperty (ids::loopStart, 0.0), loopEnd = (double) state.getProperty (ids::loopEnd, 0.0);
    const auto loopPieces = piecesAt (pieces, old.toBeats (loopStart) + beatEpsilon);

    //---------------------------------------------------------------- write it
    if (tempo != old)
        setTempoMap (tempo); // moves musical clips by their beats; every clip is placed again below

    for (auto& plan : clipPlans)
    {
        auto parent = plan.clip.getParent();
        if (plan.pieces.empty())
        {
            parent.removeChild (plan.clip, undoTarget);
            continue;
        }
        for (size_t k = 0; k < plan.pieces.size(); ++k)
        {
            const auto& piece = pieces[plan.pieces[k]];
            auto clip = plan.clip;
            if (k > 0)
            {
                clip = plan.clip.createCopy();
                renumber (clip);
            }
            if (isMusical (plan.clip))
            {
                const auto shift = shiftOf (piece);
                placeByBeats (clip, tempo, plan.anchorBeat + shift, plan.startBeat + shift, plan.endBeat + shift);
            }
            else
            {
                clip.setProperty (ids::start, mapSeconds (plan.start, piece), k > 0 ? nullptr : undoTarget);
            }
            if (k > 0)
                parent.appendChild (clip, undoTarget);
        }
    }

    for (auto& plan : blockPlans)
    {
        if (plan.pieces.empty())
        {
            state.removeChild (plan.block, undoTarget);
            continue;
        }
        const auto start = (double) plan.block[ids::start];
        for (size_t k = 0; k < plan.pieces.size(); ++k)
        {
            const auto newStart = juce::jmax (0.0, start + shiftOf (pieces[plan.pieces[k]]));
            if (k == 0)
            {
                plan.block.setProperty (ids::start, newStart, undoTarget);
                continue;
            }
            auto copy = plan.block.createCopy();
            copy.setProperty (ids::id, takeNextId(), nullptr);
            copy.setProperty (ids::start, newStart, nullptr);
            state.addChild (copy, state.indexOf (plan.block) + 1, undoTarget);
        }
    }

    for (auto& plan : markerPlans)
    {
        if (plan.pieces.empty())
        {
            state.removeChild (plan.marker, undoTarget);
            continue;
        }
        for (size_t k = 0; k < plan.pieces.size(); ++k)
        {
            const auto& piece = pieces[plan.pieces[k]];
            auto marker = plan.marker;
            if (k > 0)
            {
                marker = plan.marker.createCopy();
                marker.setProperty (ids::id, takeNextId(), nullptr);
            }
            auto* target = k > 0 ? nullptr : undoTarget;
            const auto time = mapSeconds (plan.time, piece);
            marker.setProperty (ids::time, time, target);
            if (isSection (plan.marker))
            {
                const auto endBeat = juce::jmin (plan.endBeat, piece.toBeat) + shiftOf (piece);
                marker.setProperty (ids::length, juce::jmax (minClipSeconds, tempo.toSeconds (endBeat) - time), target);
            }
            if (k > 0)
                state.appendChild (marker, undoTarget);
        }
    }

    if (! chords.empty())
        setChords (std::move (newChords));

    for (const auto& plan : lanePlans)
        setLane (plan.track, plan.target, plan.points);

    if (! loopPieces.empty() && loopEnd > loopStart)
    {
        const auto& piece = pieces[loopPieces.front()];
        const auto start = mapSeconds (loopStart, piece);
        const auto end = juce::jmax (start + minClipSeconds, tempo.toSeconds (old.toBeats (loopEnd) + shiftOf (piece)));
        state.setProperty (ids::loopStart, start, undoTarget);
        state.setProperty (ids::loopEnd, end, undoTarget);
    }
}

//==============================================================================
namespace
{
    struct SectionBeats { double start = 0.0, end = 0.0; };
}

static std::optional<SectionBeats> getSectionBeats (const juce::ValueTree& marker, const TempoMap& tempo)
{
    if (! marker.isValid() || ! Edit::isSection (marker))
        return std::nullopt;
    const auto time = (double) marker[ids::time], length = (double) marker[ids::length];
    if (length <= 0.0)
        return std::nullopt;
    return SectionBeats { tempo.toBeats (time), tempo.toBeats (time + length) };
}

bool Edit::moveSection (int markerId, double toSeconds)
{
    const auto tempo = getTempoMap();
    const auto beats = getSectionBeats (findById (ids::MARKER, markerId), tempo);
    if (! beats.has_value())
        return false;

    const auto a = beats->start, b = beats->end, c = tempo.toBeats (juce::jmax (0.0, toSeconds));
    const auto tolerance = 1.0e-3;
    if (c > a - tolerance && c < b + tolerance)
        return false;

    if (c > b)
        rearrange ({ { 0.0, a, 0.0 }, { b, c, a }, { a, b, a + (c - b) }, { c, songEndBeat, c } });
    else
        rearrange ({ { 0.0, c, 0.0 }, { a, b, c }, { c, a, c + (b - a) }, { b, songEndBeat, b } });
    return true;
}

int Edit::duplicateSection (int markerId)
{
    const auto tempo = getTempoMap();
    const auto beats = getSectionBeats (findById (ids::MARKER, markerId), tempo);
    if (! beats.has_value())
        return -1;

    std::set<int> before;
    for (const auto& marker : getMarkers())
        before.insert ((int) marker[ids::id]);

    const auto a = beats->start, b = beats->end;
    rearrange ({ { 0.0, b, 0.0 }, { a, b, b }, { b, songEndBeat, b + (b - a) } });

    for (const auto& marker : getSongSections())
        if (before.count ((int) marker[ids::id]) == 0 && std::abs (getTempoMap().toBeats ((double) marker[ids::time]) - b) < 1.0e-3)
            return (int) marker[ids::id];
    return -1;
}

bool Edit::deleteSection (int markerId)
{
    const auto beats = getSectionBeats (findById (ids::MARKER, markerId), getTempoMap());
    if (! beats.has_value())
        return false;
    rearrange ({ { 0.0, beats->start, 0.0 }, { beats->end, songEndBeat, beats->start } });
    return true;
}

void Edit::deleteTime (double startSeconds, double endSeconds)
{
    const auto tempo = getTempoMap();
    const auto a = tempo.toBeats (juce::jmax (0.0, startSeconds)), b = tempo.toBeats (juce::jmax (0.0, endSeconds));
    if (b > a + beatEpsilon)
        rearrange ({ { 0.0, a, 0.0 }, { b, songEndBeat, a } });
}

void Edit::insertTime (double startSeconds, double lengthSeconds)
{
    if (lengthSeconds <= 0.0)
        return;
    const auto tempo = getTempoMap();
    const auto a = tempo.toBeats (juce::jmax (0.0, startSeconds));
    const auto gap = lengthSeconds * tempo.getBpmAtBeat (a) / 60.0;
    rearrange ({ { 0.0, a, 0.0 }, { a, songEndBeat, a + gap } });
}

//==============================================================================
juce::String Edit::getSectionColour (const juce::String& kind, int letter)
{
    // As analysis::structure::Section::getColour, so found and hand-made sections match.
    if (kind == "intro" || kind == "outro") return "#7d8799";
    if (kind == "verse")  return "#4f8cff";
    if (kind == "chorus") return "#ff7a45";
    if (kind == "bridge") return "#b26bff";
    static const char* letters[] = { "#2bb5a0", "#e0b23a", "#e05a8a", "#6fbf4a", "#5aa6e0", "#c07a4a" };
    return letters[juce::jlimit (0, 5, letter % 6)];
}

juce::ValueTree Edit::addSection (double startSeconds, double lengthSeconds, const juce::String& kindIn, const juce::String& name)
{
    const auto start = juce::jmax (0.0, startSeconds);
    const auto end = start + lengthSeconds;
    if (lengthSeconds < minClipSeconds)
        return {};
    const auto kind = sectionKinds().contains (kindIn) ? kindIn : juce::String ("part");

    // Sections don't overlap: the new one wins.
    int sameKind = 0, letter = -1, maxLetter = -1;
    for (auto marker : getSongSections())
    {
        const auto s = (double) marker[ids::time], e = s + (double) marker[ids::length];
        const auto otherKind = marker[ids::section].toString();
        if (s >= start - 1.0e-6 && e <= end + 1.0e-6)
        {
            state.removeChild (marker, undoTarget);
            continue;
        }
        if (s < start && e > start)
            marker.setProperty (ids::length, start - s, undoTarget);
        else if (s < end && e > end)
        {
            marker.setProperty (ids::time, end, undoTarget);
            marker.setProperty (ids::length, e - end, undoTarget);
        }
        if (otherKind == kind)
        {
            ++sameKind;
            if (letter < 0)
                letter = (int) marker[ids::letter];
        }
        maxLetter = juce::jmax (maxLetter, (int) marker[ids::letter]);
    }
    if (letter < 0)
        letter = maxLetter + 1;

    juce::ValueTree marker (ids::MARKER);
    marker.setProperty (ids::id, takeNextId(), nullptr);
    const auto named = name.trim().isNotEmpty() ? name.trim().substring (0, 60)
                                                : capitalised (kind) + (sameKind > 0 ? " " + juce::String (sameKind + 1) : juce::String());
    marker.setProperty (ids::name, named, nullptr);
    marker.setProperty (ids::time, start, nullptr);
    marker.setProperty (ids::length, lengthSeconds, nullptr);
    marker.setProperty (ids::section, kind, nullptr);
    marker.setProperty (ids::letter, letter, nullptr);
    marker.setProperty (ids::colour, getSectionColour (kind, letter), nullptr);
    state.appendChild (marker, undoTarget);
    return marker;
}

void Edit::setSectionRange (int markerId, double startSeconds, double lengthSeconds)
{
    auto marker = findById (ids::MARKER, markerId);
    if (! marker.isValid() || ! isSection (marker))
        return;
    marker.setProperty (ids::time, juce::jmax (0.0, startSeconds), undoTarget);
    marker.setProperty (ids::length, juce::jmax (0.1, lengthSeconds), undoTarget);
}

void Edit::setSectionKind (int markerId, const juce::String& kind)
{
    auto marker = findById (ids::MARKER, markerId);
    if (! marker.isValid() || ! isSection (marker) || ! sectionKinds().contains (kind))
        return;
    marker.setProperty (ids::section, kind, undoTarget);
    marker.setProperty (ids::colour, getSectionColour (kind, (int) marker[ids::letter]), undoTarget);
}
}
