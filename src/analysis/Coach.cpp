#include "Coach.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace analysis::coach
{
namespace
{
    float median (std::vector<float> values)
    {
        if (values.empty())
            return 0.0f;
        const auto middle = values.begin() + (std::ptrdiff_t) (values.size() / 2);
        std::nth_element (values.begin(), middle, values.end());
        return *middle;
    }

    float ramp (float value, float good, float bad) noexcept
    {
        // 1 at `good` or better, 0 at `bad` or worse, a straight line between.
        return juce::jlimit (0.0f, 1.0f, (bad - value) / (bad - good));
    }

    std::vector<Frame>::const_iterator firstAtOrAfter (const std::vector<Frame>& frames, double time)
    {
        return std::lower_bound (frames.begin(), frames.end(), time,
                                 [] (const Frame& f, double t) { return f.time < t; });
    }

    // The typical spacing of the frames (5 ms from the live tap and from pitch::analyse).
    double getHop (const std::vector<Frame>& frames)
    {
        if (frames.size() < 2)
            return 0.005;
        const auto n = juce::jmin ((size_t) 64, frames.size() - 1);
        std::vector<float> gaps;
        for (size_t i = 0; i < n; ++i)
            gaps.push_back ((float) (frames[i + 1].time - frames[i].time));
        return juce::jlimit (0.001, 0.05, (double) median (gaps));
    }
}

//==============================================================================
float Song::getLowest() const noexcept
{
    auto lowest = 0.0f;
    for (const auto& n : notes)
        lowest = lowest <= 0.0f ? n.pitch : juce::jmin (lowest, n.pitch);
    return lowest;
}

float Song::getHighest() const noexcept
{
    auto highest = 0.0f;
    for (const auto& n : notes)
        highest = juce::jmax (highest, n.pitch);
    return highest;
}

int Song::phraseAt (double seconds) const noexcept
{
    for (int i = 0; i < (int) phrases.size(); ++i)
        if (seconds < phrases[(size_t) i].end)
            return i;
    return -1;
}

Song Song::transformed (double semitones, double speed) const
{
    Song song = *this;
    const auto scale = 1.0 / juce::jmax (0.05, speed);
    for (auto& n : song.notes)
    {
        n.start *= scale;
        n.end *= scale;
        n.pitch += (float) semitones;
    }
    for (auto& p : song.phrases)
    {
        p.start *= scale;
        p.end *= scale;
    }
    return song;
}

juce::String Song::toString() const
{
    juce::String text;
    for (const auto& n : notes)
        text << juce::String (n.start, 3) << " " << juce::String (n.end, 3) << " " << juce::String (n.pitch, 2) << "\n";
    return text;
}

Song Song::fromString (const juce::String& text)
{
    Song song;
    for (const auto& line : juce::StringArray::fromLines (text))
    {
        auto parts = juce::StringArray::fromTokens (line, " ", {});
        parts.removeEmptyStrings();
        if (parts.size() != 3)
            continue;
        TargetNote n;
        n.start = parts[0].getDoubleValue();
        n.end = parts[1].getDoubleValue();
        n.pitch = parts[2].getFloatValue();
        if (n.end > n.start && n.pitch > 0.0f)
            song.notes.push_back (n);
    }
    std::sort (song.notes.begin(), song.notes.end(), [] (const auto& a, const auto& b) { return a.start < b.start; });
    findPhrases (song);
    return song;
}

//==============================================================================
void findPhrases (Song& song, const MelodySettings& settings)
{
    song.phrases.clear();
    auto& notes = song.notes;
    if (notes.empty())
        return;

    // Cut at every silence long enough to breathe.
    std::vector<std::pair<int, int>> ranges; // [first, end) notes
    int first = 0;
    for (int i = 1; i <= (int) notes.size(); ++i)
        if (i == (int) notes.size() || notes[(size_t) i].start - notes[(size_t) i - 1].end >= settings.phraseGap)
        {
            ranges.emplace_back (first, i);
            first = i;
        }

    // A long phrase splits at its longest gap until every part is short enough (a part
    // with no gap to speak of stays whole).
    for (size_t r = 0; r < ranges.size();)
    {
        const auto [a, b] = ranges[r];
        if (notes[(size_t) b - 1].end - notes[(size_t) a].start <= settings.maxPhraseSeconds || b - a < 2)
        {
            ++r;
            continue;
        }
        auto cut = -1;
        auto widest = 0.12; // at least this much silence
        for (int i = a + 1; i < b; ++i)
            if (const auto gap = notes[(size_t) i].start - notes[(size_t) i - 1].end; gap > widest)
                widest = gap, cut = i;
        if (cut < 0)
        {
            ++r;
            continue;
        }
        ranges[r] = { a, cut };
        ranges.insert (ranges.begin() + (std::ptrdiff_t) r + 1, { cut, b });
    }

    for (const auto& [a, b] : ranges)
    {
        Phrase phrase;
        phrase.firstNote = a;
        phrase.endNote = b;
        phrase.start = notes[(size_t) a].start;
        phrase.end = notes[(size_t) b - 1].end;
        for (int i = a; i < b; ++i)
            notes[(size_t) i].phrase = (int) song.phrases.size();
        song.phrases.push_back (phrase);
    }
}

Song findMelody (const pitch::Track& track, const MelodySettings& settings)
{
    Song song;
    const auto found = pitch::findNotes (track);

    // How loud each note is (the median over it), to drop the quiet ones: other
    // instruments bleeding into a vocal stem, breaths, reverb tails.
    std::vector<float> levels;
    auto loudest = -200.0f;
    for (const auto& note : found)
    {
        std::vector<float> values;
        for (auto i = track.frameAt (note.start); i <= track.frameAt (note.end); ++i)
            values.push_back (track.energy[(size_t) i]);
        levels.push_back (median (values));
        loudest = juce::jmax (loudest, levels.back());
    }

    for (size_t i = 0; i < found.size(); ++i)
    {
        const auto& note = found[i];
        if (note.end - note.start < settings.minNoteSeconds || levels[i] < loudest - settings.quietDb)
            continue;
        if (note.pitch < 33.0f || note.pitch > 96.0f) // A1 to C7: outside is a detection error
            continue;
        song.notes.push_back ({ note.start, note.end, note.pitch, 0 });
    }

    findPhrases (song, settings);
    return song;
}

Song findMelody (const juce::AudioBuffer<float>& voice, double sampleRate, const std::function<bool()>& shouldContinue,
                 const MelodySettings& settings)
{
    const auto track = pitch::analyse (voice, sampleRate, shouldContinue);
    if (shouldContinue != nullptr && ! shouldContinue())
        return {};
    return findMelody (track, settings);
}

//==============================================================================
float foldToOctave (float sung, float target) noexcept
{
    if (sung <= 0.0f || target <= 0.0f)
        return sung;
    const auto distance = sung - target;
    return sung - 12.0f * std::round (distance / 12.0f);
}

NoteScore scoreNote (const TargetNote& note, const std::vector<Frame>& sung)
{
    NoteScore result;
    if (sung.empty() || note.end <= note.start)
        return result;

    const auto hop = getHop (sung);
    const auto length = note.end - note.start;

    // Coming in: the first moment within half a semitone of the note's pitch, from a little before it starts
    // until halfway through it (or half a second).
    constexpr double earliest = 0.25;
    const auto latest = note.start + juce::jmin (0.5, length * 0.5);
    for (auto it = firstAtOrAfter (sung, note.start - earliest); it != sung.end() && it->time <= latest; ++it)
        if (it->midi > 0.0f && std::abs (foldToOctave (it->midi, note.pitch) - note.pitch) < 0.5f)
        {
            result.onsetFound = true;
            result.timing = (float) (it->time - note.start);
            break;
        }

    // Holding it: the frames over the note, after the glide into it (the first 20%, at
    // most 60 ms).
    const auto from = note.start + juce::jmin (0.06, length * 0.2);
    std::vector<float> cents;
    for (auto it = firstAtOrAfter (sung, from); it != sung.end() && it->time < note.end; ++it)
    {
        if (it->midi > 0.0f)
            cents.push_back (100.0f * (foldToOctave (it->midi, note.pitch) - note.pitch));
    }

    const auto expected = juce::jmax (1.0, (note.end - from) / hop);
    result.coverage = juce::jlimit (0.0f, 1.0f, (float) (cents.size() / expected));

    if (cents.size() < 3)
        return result; // not sung (a frame or two is noise)

    result.sung = true;
    result.cents = median (cents);
    result.inTune = (float) std::count_if (cents.begin(), cents.end(), [] (float c) { return std::abs (c) <= inTuneCents; })
                    / (float) cents.size();

    // Wander: the pitch smoothed over 150 ms (vibrato averages out), its spread around
    // the note's centre.
    if (cents.size() >= 6)
    {
        const auto reach = juce::jmax (1, (int) std::round (0.075 / hop));
        double sumSquares = 0.0;
        for (int i = 0; i < (int) cents.size(); ++i)
        {
            const auto a = juce::jmax (0, i - reach), b = juce::jmin ((int) cents.size(), i + reach + 1);
            const auto mean = std::accumulate (cents.begin() + a, cents.begin() + b, 0.0) / (b - a);
            sumSquares += (mean - result.cents) * (mean - result.cents);
        }
        result.wander = (float) std::sqrt (sumSquares / (double) cents.size());
    }

    result.pitchScore = ramp (std::abs (result.cents), inTuneCents, 75.0f);
    result.timingScore = result.onsetFound ? ramp ((float) std::abs (result.timing), 0.08f, 0.35f) : 0.0f;
    result.steadyScore = ramp (result.wander, 12.0f, 50.0f);

    // Short notes can't be judged on steadiness; their timing matters more.
    const auto steadyWeight = length < 0.25 ? 0.0f : 0.15f;
    const auto quality = (0.6f * result.pitchScore + 0.25f * result.timingScore + steadyWeight * result.steadyScore)
                         / (0.85f + steadyWeight);
    result.score = 100.0f * juce::jmin (1.0f, result.coverage / 0.6f) * quality;
    return result;
}

PhraseScore scorePhrase (const Song& song, int phraseIndex, const std::vector<NoteScore>& noteScores)
{
    PhraseScore result;
    if (! juce::isPositiveAndBelow (phraseIndex, (int) song.phrases.size()))
        return result;

    const auto& phrase = song.phrases[(size_t) phraseIndex];
    result.scored = true;
    double total = 0.0, sungTotal = 0.0, timedTotal = 0.0;
    double score = 0.0, pitchPart = 0.0, timingPart = 0.0, steadyPart = 0.0, coverage = 0.0, lean = 0.0, timing = 0.0;

    for (int i = phrase.firstNote; i < phrase.endNote && i < (int) noteScores.size(); ++i)
    {
        const auto& s = noteScores[(size_t) i];
        const auto w = song.notes[(size_t) i].length();
        total += w;
        score += w * s.score;
        pitchPart += w * s.pitchScore;
        timingPart += w * s.timingScore;
        steadyPart += w * s.steadyScore;
        coverage += w * s.coverage;

        if (s.sung)
        {
            ++result.notesSung;
            result.notesInTune += std::abs (s.cents) <= inTuneCents ? 1 : 0;
            sungTotal += w;
            lean += w * s.cents;
        }
        if (s.onsetFound)
        {
            timedTotal += w;
            timing += w * s.timing;
        }
    }

    if (total > 0.0)
    {
        result.score = (float) (score / total);
        result.pitchScore = (float) (pitchPart / total);
        result.timingScore = (float) (timingPart / total);
        result.steadyScore = (float) (steadyPart / total);
        result.coverage = (float) (coverage / total);
    }
    result.lean = sungTotal > 0.0 ? (float) (lean / sungTotal) : 0.0f;
    result.timing = timedTotal > 0.0 ? (float) (timing / timedTotal) : 0.0f;
    return result;
}

Report score (const Song& song, const std::vector<Frame>& sung, double from, double to)
{
    Report report;
    report.notes.resize (song.notes.size());
    report.phrases.resize (song.phrases.size());

    double total = 0.0, sum = 0.0, leanWeight = 0.0, lean = 0.0;
    for (int p = 0; p < (int) song.phrases.size(); ++p)
    {
        const auto& phrase = song.phrases[(size_t) p];
        // Only phrases the take covered (a little slack at the start for a late press).
        if (phrase.start < from - 0.25 || phrase.end > to + 0.05)
            continue;

        for (int i = phrase.firstNote; i < phrase.endNote; ++i)
            report.notes[(size_t) i] = scoreNote (song.notes[(size_t) i], sung);

        const auto& s = report.phrases[(size_t) p] = scorePhrase (song, p, report.notes);
        const auto w = phrase.end - phrase.start;
        total += w;
        sum += w * s.score;
        if (s.notesSung > 0)
        {
            leanWeight += w;
            lean += w * s.lean;
        }
    }

    report.score = total > 0.0 ? (float) (sum / total) : 0.0f;
    report.lean = leanWeight > 0.0 ? (float) (lean / leanWeight) : 0.0f;
    return report;
}

std::vector<int> Report::getWeakPhrases (int count, float below) const
{
    std::vector<int> weak;
    for (int i = 0; i < (int) phrases.size(); ++i)
        if (phrases[(size_t) i].scored && phrases[(size_t) i].score < below)
            weak.push_back (i);
    std::sort (weak.begin(), weak.end(), [this] (int a, int b) { return phrases[(size_t) a].score < phrases[(size_t) b].score; });
    if ((int) weak.size() > count)
        weak.resize ((size_t) count);
    return weak;
}

std::vector<Frame> framesFrom (const pitch::Track& track, double offset)
{
    std::vector<Frame> frames;
    frames.reserve (track.midi.size());
    for (int i = 0; i < track.size(); ++i)
        frames.push_back ({ offset + track.timeOf (i), track.midi[(size_t) i] });
    return frames;
}
}
