#include "Harmony.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace analysis::harmony
{
namespace
{
struct PartInfo
{
    const char* key;
    const char* name;
};

constexpr PartInfo partInfo[] = {
    { "thirdAbove", "3rd above" }, { "thirdBelow", "3rd below" }, { "fifthAbove", "5th above" },
    { "fifthBelow", "5th below" }, { "sixthBelow", "6th below" }, { "octaveAbove", "Octave above" },
    { "octaveBelow", "Octave below" }, { "chordAbove", "Chord above" }, { "chordBelow", "Chord below" },
    { "doubleTrack", "Double" },
};
static_assert (std::size (partInfo) == (size_t) Part::numParts);

int wrap (int pitchClass) noexcept { return ((pitchClass % 12) + 12) % 12; }

// Scale steps each scale part moves (0 for the others).
int getSteps (Part part) noexcept
{
    switch (part)
    {
        case Part::thirdAbove: return 2;
        case Part::thirdBelow: return -2;
        case Part::fifthAbove: return 4;
        case Part::fifthBelow: return -4;
        case Part::sixthBelow: return -5;
        default:               return 0;
    }
}

bool isAbove (Part part) noexcept
{
    return part == Part::thirdAbove || part == Part::fifthAbove || part == Part::octaveAbove || part == Part::chordAbove;
}

// Every MIDI note of the scale, rising.
std::vector<int> getLadder (int mask)
{
    std::vector<int> ladder;
    for (int n = 0; n < 128; ++n)
        if ((mask >> wrap (n)) & 1)
            ladder.push_back (n);
    return ladder;
}

// `steps` up or down the scale from `note`. A note outside the scale moves with the scale
// note below it, keeping its distance (a chromatic passing note stays one).
int stepScale (const std::vector<int>& ladder, int note, int steps)
{
    if (ladder.empty())
        return note;
    auto it = std::upper_bound (ladder.begin(), ladder.end(), note);
    auto index = (int) (it - ladder.begin()) - 1;
    const auto target = index + steps;
    if (index < 0 || target < 0 || target >= (int) ladder.size()) // off the ends of MIDI: a plain third
        return note + (steps > 0 ? 4 : -4);
    const auto chromatic = note - ladder[(size_t) index];
    return ladder[(size_t) target] + chromatic;
}

Chord chordAtTime (const std::vector<TimedChord>& chords, double seconds) noexcept
{
    Chord found;
    for (const auto& c : chords)
    {
        if (c.start > seconds + 1.0e-9)
            break;
        found = c.chord;
    }
    return found;
}
} // namespace

juce::String getPartKey (Part part)
{
    const auto index = juce::jlimit (0, (int) Part::numParts - 1, (int) part);
    return partInfo[index].key;
}

Part partFromKey (const juce::String& key)
{
    for (int i = 0; i < (int) Part::numParts; ++i)
        if (key == partInfo[i].key)
            return (Part) i;
    return Part::thirdAbove;
}

juce::StringArray getPartKeys()
{
    juce::StringArray keys;
    for (const auto& info : partInfo)
        keys.add (info.key);
    return keys;
}

juce::String getPartName (Part part)
{
    const auto index = juce::jlimit (0, (int) Part::numParts - 1, (int) part);
    return partInfo[index].name;
}

KeyEstimate guessKey (const std::vector<pitch::Note>& notes)
{
    Chroma sum {};
    for (const auto& n : notes)
        if (n.pitch > 0.0f && n.end > n.start)
        {
            const auto note = (int) std::lround (n.pitch + n.shift);
            sum[(size_t) (wrap (note) * chromaBinsPerNote)] += (float) juce::jmin (2.0, n.end - n.start);
        }
    return estimateKey (sum);
}

std::vector<int> getIntervals (const std::vector<pitch::Note>& lead, Part part, const Context& context)
{
    std::vector<int> intervals (lead.size(), 0);
    if (lead.empty() || part == Part::doubleTrack)
        return intervals;
    if (part == Part::octaveAbove || part == Part::octaveBelow)
    {
        std::fill (intervals.begin(), intervals.end(), part == Part::octaveAbove ? 12 : -12);
        return intervals;
    }

    // The key: given, or guessed from the lead as it sounds.
    auto tonic = context.tonic;
    auto minor = context.minor;
    if (tonic < 0)
    {
        auto sounding = lead;
        for (auto& n : sounding)
            n.shift += context.transpose;
        const auto guess = guessKey (sounding);
        tonic = guess.isValid() ? guess.key : 0;
        minor = guess.isValid() && guess.minor;
    }
    const auto ladder = getLadder (getKeyMask (tonic, minor));

    const auto above = isAbove (part);
    const auto ideal = above ? 4 : -4; // a third, the most usual harmony distance
    const auto chordPart = part == Part::chordAbove || part == Part::chordBelow;
    const auto steps = chordPart ? (above ? 2 : -2) : getSteps (part);

    int previous = -1;      // the harmony's last note
    double previousEnd = -1.0e9;

    for (size_t i = 0; i < lead.size(); ++i)
    {
        const auto& n = lead[i];
        const auto note = (int) std::lround (n.pitch + n.shift + context.transpose);
        const auto chord = chordAtTime (context.chords, 0.5 * (n.start + n.end));
        const auto connected = previous >= 0 && n.start - previousEnd < 1.5; // a phrase goes on
        auto target = stepScale (ladder, note, steps);

        const auto pick = [&] (int low, int high, int aim)
        {
            // Chord notes between low and high (semitones from the lead), the one closest to
            // `aim`, and to the harmony's last note when the phrase goes on.
            auto best = std::numeric_limits<int>::min();
            auto bestCost = std::numeric_limits<double>::max();
            for (int d = low; d <= high; ++d)
            {
                const auto c = note + d;
                if (! chord.contains (wrap (c)))
                    continue;
                auto cost = 0.6 * std::abs (c - (note + aim));
                if (connected)
                    cost += std::abs (c - previous);
                cost += 0.01 * std::abs (d); // ties: the closer one
                if (cost < bestCost)
                    bestCost = cost, best = c;
            }
            return best;
        };

        if (chord.isValid())
        {
            if (chordPart)
            {
                const auto c = above ? pick (3, 9, ideal) : pick (-9, -3, ideal);
                if (c != std::numeric_limits<int>::min())
                    target = c;
            }
            else if (chord.contains (wrap (note)) && ! chord.contains (wrap (target)))
            {
                // The lead sings a chord note but the scale step doesn't land on one: the
                // nearest chord note to it on the same side (a fourth below a fifth, say).
                const auto interval = target - note;
                const auto low = above ? juce::jmax (3, interval - 2) : interval - 2;
                const auto high = above ? interval + 2 : juce::jmin (-3, interval + 2);
                if (const auto c = pick (low, high, interval); c != std::numeric_limits<int>::min())
                    target = c;
            }
        }

        target = juce::jlimit (24, 108, target);
        if (target == note && part != Part::doubleTrack)
            target = note + ideal; // never a unison
        intervals[i] = target - note;
        previous = target;
        previousEnd = n.end;
    }

    return intervals;
}

void limitOffsets (const std::vector<pitch::Note>& notes, std::vector<double>& offsets, double maxRate)
{
    maxRate = juce::jmax (1.01, maxRate);
    for (size_t i = 1; i < notes.size() && i < offsets.size(); ++i)
    {
        const auto span = notes[i].start - notes[i - 1].start;
        if (span <= 0.0)
        {
            offsets[i] = offsets[i - 1];
            continue;
        }
        const auto lowest = offsets[i - 1] + span * (1.0 / maxRate - 1.0);
        const auto highest = offsets[i - 1] + span * (maxRate - 1.0);
        offsets[i] = juce::jlimit (lowest, highest, offsets[i]);
    }
}

Voice makeVoice (const std::vector<pitch::Note>& lead, Part part, const Context& context, Feel feel, int seed)
{
    Voice voice;
    voice.part = part;
    const auto intervals = getIntervals (lead, part, context);
    const auto loose = feel == Feel::loose;
    const auto isDouble = part == Part::doubleTrack;
    const auto octave = part == Part::octaveAbove || part == Part::octaveBelow;

    juce::Random random ((juce::int64) seed * 7919 + (juce::int64) part * 104729 + (loose ? 31 : 17));
    const auto between = [&] (double low, double high) { return low + (high - low) * random.nextDouble(); };
    const auto sign = [&] { return random.nextBool() ? 1.0 : -1.0; };

    // The voice as a whole: its tuning, its character, its lean in time, its level.
    const auto detune = (float) (sign() * (isDouble ? between (0.05, 0.10) : between (0.02, 0.06)));
    const auto formant = (float) (isDouble ? sign() * between (0.2, 0.5)
                                           : (isAbove (part) ? 1.0 : -1.0) * between (0.4, 0.9));
    const auto steadier = (float) (loose ? between (0.05, 0.2) : between (0.2, 0.45));
    const auto vibratoScale = (float) (loose ? between (0.8, 1.15) : between (0.5, 0.8));
    const auto lean = sign() * (loose ? between (0.0, 0.008) : between (0.0, 0.004));
    const auto lowNudge = loose ? 0.015 : 0.010, highNudge = loose ? 0.025 : 0.015;
    const auto wander = loose ? 1.5 : 0.75;
    voice.levelDb = (float) ((isDouble ? -4.5 : octave ? -9.0 : -6.5) + between (-0.5, 0.5));

    voice.notes.reserve (lead.size());
    voice.offsets.reserve (lead.size());
    for (size_t i = 0; i < lead.size(); ++i)
    {
        auto n = lead[i];
        const auto jitter = (float) between (-0.02, 0.02);
        n.shift = juce::jlimit (-pitch::Note::maxShift, pitch::Note::maxShift, n.shift + (float) intervals[i] + detune + jitter);
        n.formant = juce::jlimit (-pitch::Note::maxFormant, pitch::Note::maxFormant, n.formant + formant);
        n.straighten = juce::jlimit (0.0f, 1.0f, n.straighten + steadier);
        n.vibrato = juce::jlimit (0.0f, 2.0f, n.vibrato * vibratoScale);
        n.gain = juce::jlimit (-pitch::Note::maxGain, pitch::Note::maxGain, n.gain + (float) between (-wander, wander));
        if (loose)
            n.transition = juce::jmin (0.2f, n.transition * 1.25f);
        voice.notes.push_back (n);
        voice.offsets.push_back (lean + sign() * between (lowNudge, highNudge));
    }

    limitOffsets (voice.notes, voice.offsets);
    return voice;
}
}
