#pragma once

#include "Chords.h"
#include "Pitch.h"

#include <vector>

// Backing vocals from a lead vocal: harmony parts (a third above, a fifth below, the
// chord's notes...) and doubles, each sung by the same take moved note by note.
//
// For every sung note of the lead (analysis::pitch::Note, see Pitch.h) a part picks the
// note it sings: a number of steps up or down the song's scale, or the nearest note of
// the chord in force at that moment, chosen so the part moves as little as it can from
// one note to the next (voice leading) instead of jumping with every chord. The answer
// is a copy of the lead's notes with their `shift` set, which the warp render (Warp.h)
// sings; a double keeps the lead's notes and is only detuned a few cents.
//
// To sound like separate takes rather than a copy, each voice is a little different:
// every note is nudged early or late (10-25 ms, see Voice::offsets: the caller turns
// those into warp markers), the drift and vibrato are steadier or wider, the formants move
// a little (a higher part sounds a touch smaller, a lower one a touch bigger), the
// levels wander by a dB or so, and the whole voice sits under the lead.
//
// Plain computation on note lists; nothing here touches audio.
namespace analysis::harmony
{
enum class Part
{
    thirdAbove, thirdBelow, fifthAbove, fifthBelow, sixthBelow, octaveAbove, octaveBelow,
    chordAbove, chordBelow, doubleTrack,
    numParts
};

// "thirdAbove", "fifthBelow"... for the page and the project; unknown text gives thirdAbove.
juce::String getPartKey (Part);
Part partFromKey (const juce::String&);
juce::StringArray getPartKeys();
// "3rd above", "Double"... as the page shows it.
juce::String getPartName (Part);

// A chord from a given time on (file seconds), until the next one starts.
struct TimedChord
{
    double start = 0.0;
    Chord chord;
};

// What the parts are written against.
struct Context
{
    int tonic = -1;      // the song's key (0 = C ... 11 = B), -1 when unknown: then it's guessed from the lead's notes
    bool minor = false;
    std::vector<TimedChord> chords; // in time order; empty when unknown (parts then follow the scale)
    float transpose = 0.0f;         // semitones the lead clip plays moved by (its own and its track's transpose)
};

// The key the lead's notes fit best (weighted by how long each is held), as a KeyEstimate.
KeyEstimate guessKey (const std::vector<pitch::Note>&);

// The note each lead note's harmony sings, as semitones from the lead (whole numbers;
// 0 for a double). Lead pitches are read as sung plus their edits (`shift`) plus the
// context's transpose, rounded to the nearest note.
std::vector<int> getIntervals (const std::vector<pitch::Note>& lead, Part, const Context&);

enum class Feel { tight, loose };

struct Voice
{
    Part part = Part::thirdAbove;
    std::vector<pitch::Note> notes; // the lead's notes with this voice's edits
    std::vector<double> offsets;    // per note: how far it moves in time, seconds (+ = late)
    float levelDb = -6.0f;          // the voice's level against the lead's
};

// One backing voice. `seed` makes each voice of a set different (and the same voice the
// same every time it's built): use the voice's number.
Voice makeVoice (const std::vector<pitch::Note>& lead, Part, const Context&, Feel = Feel::tight, int seed = 0);

// The timing offsets limited so no stretch between two note starts runs faster or slower
// than `maxRate` (e.g. 1.5 = 50% either way): a short note between two nudged ones
// doesn't get squashed.
void limitOffsets (const std::vector<pitch::Note>&, std::vector<double>& offsets, double maxRate = 1.5);
}
