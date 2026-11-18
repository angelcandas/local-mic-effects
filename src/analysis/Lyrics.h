#pragma once

#include <juce_core/juce_core.h>

#include "analysis/Coach.h"

#include <vector>

// Song lyrics, timed to the singing (the Practice view's karaoke lines).
//
// Lyrics come pasted as plain text (one line per sung line) or as an .lrc file. LRC
// timestamps are used as they are. Plain lines are placed on the song by matching them to
// the notes found in the voice stem (analysis::coach::findMelody): each line takes a run
// of consecutive notes about as long as its syllables, and lines prefer to start after a
// breath (a gap between notes). It's syllable-to-note matching on the sung melody, not
// phoneme-level forced alignment, so a line can land a little early or late: every line
// can be nudged by hand, and nudged lines stay put when the rest is aligned again.
//
// Times are in the song as it is (seconds); the page gets them moved for a speed change.
namespace analysis::lyrics
{
struct Word
{
    juce::String text;
    double start = 0.0; // when it's sung
    int syllables = 1;
};

struct Line
{
    juce::String text;
    double start = 0.0, end = 0.0;
    bool timed = false;   // has a time (from the .lrc, the alignment or a nudge)
    bool pinned = false;  // set by hand (or by the .lrc): aligning again keeps it
    std::vector<Word> words;
};

struct Lyrics
{
    std::vector<Line> lines;
    bool fromLrc = false;

    bool isEmpty() const noexcept { return lines.empty(); }

    // One line per lyric line: "start<TAB>end<TAB>pinned<TAB>text" (the words' times follow
    // from the line's), for the song's cache folder.
    juce::String toString() const;
    static Lyrics fromString (const juce::String&);

    // As an .lrc file ([mm:ss.xx] text).
    juce::String toLrc() const;
};

// Pasted text or an .lrc file's contents. Blank lines, [Verse]-style headings and LRC tags
// that aren't times ([ar:...]) are left out. A line with several times ([00:12.00][01:30.00])
// is repeated at each.
Lyrics parse (const juce::String& text);

// Rough syllable count of a word (vowel groups; English and Spanish), at least 1.
int countSyllables (const juce::String& word);

// Places the lines that aren't pinned on the melody's notes (see above). Pinned lines stay,
// and the lines between two pinned ones go between them. Then every line's words get
// times along its notes.
void align (Lyrics&, const analysis::coach::Song& melody);

// The words' times inside each line from its start and end (syllables spread over the
// notes sung in it, or evenly when there are none).
void timeWords (Lyrics&, const analysis::coach::Song& melody);

// Moves line `index` by `seconds` (pinning it), keeping it after the line before and before the next.
void nudge (Lyrics&, int index, double seconds, const analysis::coach::Song& melody);
} // namespace analysis::lyrics
