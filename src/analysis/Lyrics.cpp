#include "Lyrics.h"

#include <cmath>
#include <limits>

namespace analysis::lyrics
{
namespace
{
    juce::String formatLrcTime (double seconds)
    {
        seconds = juce::jmax (0.0, seconds);
        const auto minutes = (int) (seconds / 60.0);
        const auto rest = seconds - minutes * 60.0;
        return juce::String (minutes).paddedLeft ('0', 2) + ":" + juce::String (rest, 2).paddedLeft ('0', 5);
    }

    std::vector<Word> splitWords (const juce::String& text)
    {
        std::vector<Word> words;
        for (const auto& token : juce::StringArray::fromTokens (text, " \t", "\""))
            if (token.trim().isNotEmpty())
                words.push_back ({ token.trim(), 0.0, countSyllables (token) });
        return words;
    }

    int countSyllables (const Line& line)
    {
        auto total = 0;
        for (const auto& w : line.words)
            total += w.syllables;
        return juce::jmax (1, total);
    }

    // A whole line that is only a heading: [Chorus], (Verse 2), {Bridge}.
    bool isHeading (const juce::String& line)
    {
        const auto t = line.trim();
        if (t.length() < 2)
            return false;
        const auto first = t[0], last = t[t.length() - 1];
        return (first == '[' && last == ']') || (first == '(' && last == ')') || (first == '{' && last == '}');
    }

    // The notes that lie inside from..to (by their middle).
    std::pair<int, int> notesIn (const coach::Song& melody, double from, double to)
    {
        int a = (int) melody.notes.size(), b = 0;
        for (int i = 0; i < (int) melody.notes.size(); ++i)
        {
            const auto mid = 0.5 * (melody.notes[(size_t) i].start + melody.notes[(size_t) i].end);
            if (mid >= from && mid < to)
            {
                a = juce::jmin (a, i);
                b = juce::jmax (b, i + 1);
            }
        }
        return a < b ? std::make_pair (a, b) : std::make_pair (0, 0);
    }

    // Lines first..last (not pinned) spread over from..to by their syllables (no notes to go on).
    void spread (Lyrics& lyrics, int first, int last, double from, double to)
    {
        auto total = 0;
        for (int i = first; i <= last; ++i)
            total += countSyllables (lyrics.lines[(size_t) i]);
        auto at = from;
        for (int i = first; i <= last; ++i)
        {
            auto& line = lyrics.lines[(size_t) i];
            const auto length = (to - from) * countSyllables (line) / juce::jmax (1, total);
            line.start = at;
            line.end = at + length;
            line.timed = true;
            at += length;
        }
    }

    // Lines first..last onto notes [a, b) of the melody (the dynamic programme; see the header).
    void placeOnNotes (Lyrics& lyrics, int first, int last, const coach::Song& melody, int a, int b)
    {
        const auto numLines = last - first + 1;
        const auto numNotes = b - a;
        const auto& notes = melody.notes;

        auto syllables = 0;
        for (int i = first; i <= last; ++i)
            syllables += countSyllables (lyrics.lines[(size_t) i]);
        // Notes per syllable, as sung here (several syllables can share a held note, and a
        // syllable can glide over several).
        const auto rate = juce::jlimit (0.25, 4.0, (double) numNotes / juce::jmax (1, syllables));

        constexpr double sizeWeight = 1.0, breathWeight = 0.8, phraseWeight = 0.7, splitWeight = 0.6, skipCost = 0.35;
        constexpr int maxSpan = 80;
        const auto gapBefore = [&] (int j) // seconds of silence before note j (a + j)
        {
            return j == 0 ? 1.5 : notes[(size_t) (a + j)].start - notes[(size_t) (a + j - 1)].end;
        };
        const auto startsPhrase = [&] (int j) { return j == 0 || notes[(size_t) (a + j)].phrase != notes[(size_t) (a + j - 1)].phrase; };

        // Phrase starts inside notes j..k (exclusive of j): a line across a breath costs.
        std::vector<int> phraseStarts ((size_t) numNotes + 1, 0);
        for (int j = 1; j <= numNotes; ++j)
            phraseStarts[(size_t) j] = phraseStarts[(size_t) j - 1] + (j < numNotes && startsPhrase (j) ? 1 : 0);

        const auto lineCost = [&] (int line, int j, int k)
        {
            const auto expected = countSyllables (lyrics.lines[(size_t) (first + line)]) * rate;
            const auto size = std::log ((double) (k - j) / expected);
            auto cost = sizeWeight * size * size;
            cost -= breathWeight * juce::jlimit (0.0, 1.0, gapBefore (j) / 0.8);
            if (startsPhrase (j))
                cost -= phraseWeight;
            cost += splitWeight * (phraseStarts[(size_t) k - 1] - phraseStarts[(size_t) j]);
            return cost;
        };

        constexpr auto inf = std::numeric_limits<double>::infinity();
        // best[i][k]: lines 0..i placed, line i ending at note k (exclusive).
        std::vector<std::vector<double>> best ((size_t) numLines, std::vector<double> ((size_t) numNotes + 1, inf));
        std::vector<std::vector<int>> from ((size_t) numLines, std::vector<int> ((size_t) numNotes + 1, -1));
        std::vector<double> lead ((size_t) numNotes + 1, inf); // best way to be ready to start at note j
        std::vector<int> leadFrom ((size_t) numNotes + 1, -1);   // ... the note the last line ended at

        for (int i = 0; i < numLines; ++i)
        {
            // Ready to start line i at note j: the previous line ended at e <= j, skipping the notes between.
            for (int j = 0; j <= numNotes; ++j)
            {
                if (i == 0)
                {
                    lead[(size_t) j] = 0.5 * skipCost * j; // notes before the first line (an intro, ad libs)
                    leadFrom[(size_t) j] = 0;
                    continue;
                }
                const auto ended = best[(size_t) i - 1][(size_t) j];
                const auto carried = j > 0 ? lead[(size_t) j - 1] + skipCost : inf;
                if (ended <= carried) { lead[(size_t) j] = ended; leadFrom[(size_t) j] = j; }
                else { lead[(size_t) j] = carried; leadFrom[(size_t) j] = j > 0 ? leadFrom[(size_t) j - 1] : -1; }
            }

            for (int k = 1; k <= numNotes; ++k)
                for (int j = juce::jmax (0, k - maxSpan); j < k; ++j)
                {
                    if (lead[(size_t) j] == inf)
                        continue;
                    const auto cost = lead[(size_t) j] + lineCost (i, j, k);
                    if (cost < best[(size_t) i][(size_t) k])
                    {
                        best[(size_t) i][(size_t) k] = cost;
                        from[(size_t) i][(size_t) k] = j;
                    }
                }
        }

        // The last line's end (notes after it are an outro), then back through the choices.
        auto end = -1;
        auto bestCost = inf;
        for (int k = 1; k <= numNotes; ++k)
        {
            const auto cost = best[(size_t) numLines - 1][(size_t) k] + 0.5 * skipCost * (numNotes - k);
            if (cost < bestCost) { bestCost = cost; end = k; }
        }
        if (end < 0)
        {
            spread (lyrics, first, last, notes[(size_t) a].start, notes[(size_t) b - 1].end);
            return;
        }

        // Each line's start note; the lead-in chain gives where the line before ended.
        std::vector<std::pair<int, int>> spans ((size_t) numLines);
        auto k = end;
        for (int i = numLines - 1; i >= 0; --i)
        {
            const auto j = from[(size_t) i][(size_t) k];
            spans[(size_t) i] = { j, k };
            if (i == 0)
                break;
            // Where line i - 1 ended (<= j): the lead-in's choice, worked out again.
            auto bestEnd = inf;
            auto chosen = -1;
            for (int x = j; x >= 1; --x)
            {
                const auto cost = best[(size_t) i - 1][(size_t) x] + skipCost * (j - x);
                if (cost < bestEnd) { bestEnd = cost; chosen = x; }
            }
            k = chosen > 0 ? chosen : j;
        }

        for (int i = 0; i < numLines; ++i)
        {
            auto& line = lyrics.lines[(size_t) (first + i)];
            line.start = notes[(size_t) (a + spans[(size_t) i].first)].start;
            line.end = notes[(size_t) (a + spans[(size_t) i].second - 1)].end;
            line.timed = true;
        }
    }
} // namespace

int countSyllables (const juce::String& word)
{
    const auto lower = word.toLowerCase();
    const juce::String vowelSet (juce::CharPointer_UTF8 ("aeiouy\xc3\xa1\xc3\xa9\xc3\xad\xc3\xb3\xc3\xba\xc3\xbc\xc3\xa0\xc3\xa8\xc3\xac\xc3\xb2\xc3\xb9\xc3\xa2\xc3\xaa\xc3\xae\xc3\xb4\xc3\xbb\xc3\xa4\xc3\xab\xc3\xaf\xc3\xb6"));
    auto count = 0;
    auto inVowel = false;
    for (auto c : lower)
    {
        const auto isVowel = vowelSet.containsChar (c);
        if (isVowel && ! inVowel)
            ++count;
        inVowel = isVowel;
    }
    return juce::jmax (1, count);
}

Lyrics parse (const juce::String& text)
{
    Lyrics lyrics;
    struct Timed { double time; juce::String text; };
    std::vector<Timed> timed;
    juce::StringArray plain;

    for (auto raw : juce::StringArray::fromLines (text))
    {
        auto line = raw.trim();
        if (line.isEmpty())
            continue;

        // LRC: one or more [mm:ss.xx] tags, then the words.
        std::vector<double> times;
        auto tagged = false;
        while (line.startsWithChar ('['))
        {
            const auto close = line.indexOfChar (']');
            if (close < 0)
                break;
            const auto tag = line.substring (1, close);
            const auto colon = tag.indexOfChar (':');
            const auto minutes = tag.substring (0, colon);
            if (colon > 0 && minutes.containsOnly ("0123456789") && tag.substring (colon + 1).containsOnly ("0123456789.:"))
            {
                auto rest = tag.substring (colon + 1).replaceCharacter (':', '.');
                times.push_back (minutes.getIntValue() * 60.0 + rest.getDoubleValue());
                tagged = true;
            }
            else if (colon > 0 && ! tag.containsChar (' ') && tag.substring (0, colon).containsOnly ("abcdefghijklmnopqrstuvwxyz"))
            {
                tagged = true; // [ar:Artist], [offset:+100]: metadata
            }
            else
            {
                break; // a heading such as [Chorus]
            }
            line = line.substring (close + 1).trim();
        }

        if (! times.empty())
        {
            for (const auto t : times)
                timed.push_back ({ t, line });
            continue;
        }
        if (tagged || line.isEmpty() || isHeading (line))
            continue;
        plain.add (line);
    }

    if (! timed.empty())
    {
        lyrics.fromLrc = true;
        std::stable_sort (timed.begin(), timed.end(), [] (const Timed& a, const Timed& b) { return a.time < b.time; });
        for (size_t i = 0; i < timed.size(); ++i)
        {
            if (timed[i].text.isEmpty())
                continue; // an empty timed line marks the end of the one before
            Line line;
            line.text = timed[i].text;
            line.start = timed[i].time;
            line.end = i + 1 < timed.size() ? timed[i + 1].time : timed[i].time + 4.0;
            line.end = juce::jmin (line.end, line.start + 12.0);
            line.timed = line.pinned = true;
            line.words = splitWords (line.text);
            lyrics.lines.push_back (std::move (line));
        }
        return lyrics;
    }

    for (const auto& p : plain)
    {
        Line line;
        line.text = p;
        line.words = splitWords (p);
        lyrics.lines.push_back (std::move (line));
    }
    return lyrics;
}

void align (Lyrics& lyrics, const coach::Song& melody)
{
    const auto numLines = (int) lyrics.lines.size();
    const auto songEnd = melody.notes.empty() ? 0.0 : melody.notes.back().end;

    // Runs of lines that aren't pinned, between the pinned ones (or the song's ends).
    for (int i = 0; i < numLines;)
    {
        if (lyrics.lines[(size_t) i].pinned)
        {
            ++i;
            continue;
        }
        auto last = i;
        while (last + 1 < numLines && ! lyrics.lines[(size_t) last + 1].pinned)
            ++last;

        const auto from = i > 0 ? lyrics.lines[(size_t) i - 1].end : 0.0;
        const auto to = last + 1 < numLines ? lyrics.lines[(size_t) last + 1].start : juce::jmax (songEnd, from + 1.0) + 0.5;
        const auto [a, b] = notesIn (melody, from - 0.05, to + 0.05);
        if (b - a >= last - i + 1)
            placeOnNotes (lyrics, i, last, melody, a, b);
        else
            spread (lyrics, i, last, from, juce::jmax (from + 0.5 * (last - i + 1), to));
        i = last + 1;
    }

    timeWords (lyrics, melody);
}

void timeWords (Lyrics& lyrics, const coach::Song& melody)
{
    for (auto& line : lyrics.lines)
    {
        if (line.words.empty())
            line.words = splitWords (line.text);
        const auto [a, b] = notesIn (melody, line.start - 0.02, line.end + 0.02);
        const auto total = countSyllables (line);
        auto done = 0;
        for (auto& word : line.words)
        {
            if (b > a)
            {
                // Syllables spread over the line's notes: the word starts on its first one's note.
                const auto note = a + juce::jmin (b - a - 1, (int) std::floor ((double) done * (b - a) / total));
                word.start = juce::jmax (line.start, melody.notes[(size_t) note].start);
            }
            else
            {
                word.start = line.start + (line.end - line.start) * done / total;
            }
            done += word.syllables;
        }
    }
}

void nudge (Lyrics& lyrics, int index, double seconds, const coach::Song& melody)
{
    if (! juce::isPositiveAndBelow (index, (int) lyrics.lines.size()))
        return;

    auto& line = lyrics.lines[(size_t) index];
    const auto length = juce::jmax (0.3, line.end - line.start);
    const auto lowest = index > 0 ? lyrics.lines[(size_t) index - 1].start + 0.05 : 0.0;
    const auto highest = index + 1 < (int) lyrics.lines.size() && lyrics.lines[(size_t) index + 1].timed
                       ? lyrics.lines[(size_t) index + 1].start - 0.05 : std::numeric_limits<double>::max();
    line.start = juce::jlimit (lowest, juce::jmax (lowest, highest), line.start + seconds);
    line.end = juce::jmin (line.start + length, highest);
    line.end = juce::jmax (line.end, line.start + 0.1);
    if (index > 0)
    {
        auto& before = lyrics.lines[(size_t) index - 1];
        before.end = juce::jmin (before.end, line.start);
    }
    line.timed = line.pinned = true;
    timeWords (lyrics, melody);
}

juce::String Lyrics::toString() const
{
    juce::String text;
    for (const auto& line : lines)
        text << juce::String (line.start, 3) << "\t" << juce::String (line.end, 3) << "\t" << (line.timed ? (line.pinned ? 2 : 1) : 0) << "\t"
             << line.text.replaceCharacters ("\t\r\n", "   ") << "\n";
    return text;
}

Lyrics Lyrics::fromString (const juce::String& text)
{
    Lyrics lyrics;
    for (const auto& row : juce::StringArray::fromLines (text))
    {
        const auto fields = juce::StringArray::fromTokens (row, "\t", {});
        if (fields.size() < 4)
            continue;
        Line line;
        line.start = fields[0].getDoubleValue();
        line.end = fields[1].getDoubleValue();
        const auto state = fields[2].getIntValue();
        line.timed = state > 0;
        line.pinned = state > 1;
        line.text = fields.joinIntoString ("\t", 3);
        line.words = splitWords (line.text);
        lyrics.lines.push_back (std::move (line));
    }
    return lyrics;
}

juce::String Lyrics::toLrc() const
{
    juce::String text;
    for (const auto& line : lines)
        text << "[" << formatLrcTime (line.start) << "]" << line.text << "\n";
    return text;
}
} // namespace analysis::lyrics
