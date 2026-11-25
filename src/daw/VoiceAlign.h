#pragma once

#include "AudioPool.h"
#include "Edit.h"
#include "analysis/VocalAlign.h"

// "Align to lead": line clips (a double, a harmony, a re-sung line) up in time with a
// lead track, syllable by syllable (see analysis/VocalAlign.h), and optionally tune their
// sung notes to the lead's. Recordings stay as they are: aligning only adds to the clips'
// warp maps (Warp.h) and, for pitch, writes their note edits (the note pitch editor's).
//
// Message thread: reads the tracks' audio through the AudioPool synchronously.
namespace daw::voicealign
{
struct Options
{
    analysis::align::Options timing;
    bool timingOn = true;     // off: only the pitch is matched (the timing stays)
    bool matchPitch = false;  // also tune each sung note to what the lead sings there
    float pitchRange = 2.0f;  // semitones: notes further than this from the lead (in any octave) are another part, left alone
};

struct Result
{
    int aligned = 0;          // clips lined up
    int unrelated = 0;        // clips that don't sing what the lead sings, left alone
    int notes = 0;            // notes tuned to the lead
    float match = 0.0f;       // the lowest match of the aligned clips (0..1)
    double meanShift = 0.0;   // seconds, the average move over the aligned clips' sung frames
    double largestShift = 0.0;
    juce::String error;       // set when nothing could be done
};

// Aligns the audio clips `clipIds` (clips on the lead track itself are left out) to the
// track `guideTrackId` as it plays. Linked partners follow their clip. The caller opens
// the undo transaction.
Result alignToGuide (Edit&, AudioPool&, const juce::Array<int>& clipIds, int guideTrackId, const Options&);
}
