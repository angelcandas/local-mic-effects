#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include "Pitch.h"

#include <functional>
#include <vector>

// Singing practice on any song: the melody to sing (found in the song's vocal stem) and
// how close a singer gets to it.
//
// findMelody() turns a voice into target notes (analysis/Pitch.h finds them) and groups
// them into phrases at the breaths between them. score() compares what was sung (pitch
// frames, e.g. from the live mic) with a target: per note, how close the pitch was in
// any octave (so a low voice can sing a high song), whether it came in on time, how it
// leaned sharp or flat and how steady it held; per phrase, the same over its notes.
//
// Everything is in the song's seconds. Plain computation, any thread.
namespace analysis::coach
{
struct TargetNote
{
    double start = 0.0, end = 0.0;
    float pitch = 0.0f; // MIDI note number, fractions are cents
    int phrase = 0;     // index into Song::phrases

    double length() const noexcept { return end - start; }
};

struct Phrase
{
    double start = 0.0, end = 0.0;
    int firstNote = 0, endNote = 0; // notes [firstNote, endNote)
};

struct Song
{
    std::vector<TargetNote> notes; // in time order
    std::vector<Phrase> phrases;

    bool isEmpty() const noexcept { return notes.empty(); }
    float getLowest() const noexcept;
    float getHighest() const noexcept;

    // The phrase sung at `seconds` (or the next one, between phrases); -1 after the last.
    int phraseAt (double seconds) const noexcept;

    // The same melody moved: `semitones` up or down, and played `speed` times as fast
    // from the song's start (0.5: half speed, so every time doubles).
    Song transformed (double semitones, double speed) const;

    // "start end pitch" per line, for the cache. Phrases are found again when read.
    juce::String toString() const;
    static Song fromString (const juce::String&);
};

struct MelodySettings
{
    double minNoteSeconds = 0.08; // shorter notes are slides and consonants, not targets
    double phraseGap = 0.45;      // a silence at least this long starts a new phrase
    double maxPhraseSeconds = 12.0; // longer phrases split at their longest gap
    float quietDb = 30.0f;        // notes this far under the loudest are bleed, dropped
};

// Groups notes (in time order) into phrases and sets each note's phrase.
void findPhrases (Song&, const MelodySettings& = {});

// The melody of a voice already analysed.
Song findMelody (const pitch::Track&, const MelodySettings& = {});

// The melody of a recorded voice (a vocal stem, or a voice on its own).
Song findMelody (const juce::AudioBuffer<float>&, double sampleRate, const std::function<bool()>& shouldContinue = {},
                 const MelodySettings& = {});

//==============================================================================
// What was sung: a pitch every few milliseconds, in song time.
struct Frame
{
    double time = 0.0;
    float midi = 0.0f; // 0 when nothing pitched was sung
};

// `sung` moved by whole octaves to the nearest one to `target` (both MIDI notes).
float foldToOctave (float sung, float target) noexcept;

// Within this many cents a note counts as in tune (and shows green).
inline constexpr float inTuneCents = 25.0f;

struct NoteScore
{
    bool sung = false;     // some of the note was sung
    float coverage = 0.0f; // share of the note's time with a pitch sung, 0..1
    float cents = 0.0f;    // median distance from the target (any octave), + sharp, - flat
    float inTune = 0.0f;   // share of the sung frames within inTuneCents
    float wander = 0.0f;   // how far the held pitch drifted around its centre, cents (vibrato left out)
    float timing = 0.0f;   // seconds: + came in late, - early (0 when it wasn't found)
    bool onsetFound = false;

    // 0..1 parts, and the note's score 0..100.
    float pitchScore = 0.0f, timingScore = 0.0f, steadyScore = 0.0f;
    float score = 0.0f;
};

struct PhraseScore
{
    bool scored = false;      // the phrase was in the take
    float score = 0.0f;       // 0..100, the notes' scores weighted by length
    float pitchScore = 0.0f, timingScore = 0.0f, steadyScore = 0.0f; // 0..1, the same way
    float coverage = 0.0f;    // share of the phrase's notes' time sung
    float lean = 0.0f;        // cents: + sharp, - flat (sung notes, weighted by length)
    float timing = 0.0f;      // seconds: + late, - early (notes that came in)
    int notesSung = 0, notesInTune = 0;
};

struct Report
{
    std::vector<NoteScore> notes;     // one per Song::notes
    std::vector<PhraseScore> phrases; // one per Song::phrases
    float score = 0.0f;               // 0..100 over the phrases scored, weighted by length
    float lean = 0.0f;                // cents, over everything sung

    // Phrases worth looping: the lowest scored under `below`, worst first, at most `count`.
    std::vector<int> getWeakPhrases (int count = 3, float below = 75.0f) const;
};

// One note against the sung frames (sorted by time).
NoteScore scoreNote (const TargetNote&, const std::vector<Frame>& sung);

// One phrase from its notes' scores.
PhraseScore scorePhrase (const Song&, int phrase, const std::vector<NoteScore>& noteScores);

// The phrases that lie between `from` and `to` seconds (default: all of them), and their
// notes. Phrases outside that keep empty scores and don't count towards the total.
Report score (const Song&, const std::vector<Frame>& sung, double from = 0.0, double to = 1.0e9);

// Sung frames from a recording (a take): its pitch every 5 ms, moved `offset` seconds
// (where the recording starts in the song).
std::vector<Frame> framesFrom (const pitch::Track&, double offset = 0.0);
}
