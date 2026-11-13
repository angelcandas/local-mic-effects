#pragma once

#include "Pitch.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <functional>
#include <limits>
#include <vector>

// Audio to MIDI: turns a recorded part into notes.
//
// Melody: a sung, hummed or played single line (a voice, a bass line, a whistle) becomes
// one note per sung note. The pitch comes from analysis::pitch (the note pitch editor's
// analysis), snapped to the nearest semitone, or to the nearest note of a key, after
// taking out how far the whole take sits from A = 440 Hz. Velocity follows how loud each
// note was. Notes can be quantised to a grid and moved by octaves (a bass line hummed an
// octave or two up comes down to where a bass plays).
//
// Drums: a drum loop or a drums stem becomes General MIDI drum notes (36 kick, 38 snare,
// 42 closed hat, 46 open hat, 45 / 48 toms, 49 crash). Hits are found where the whole kit
// or one band of it (lows, toms, wires, cymbals) jumps in level, so a soft hat after a
// loud snare still counts. Each hit is described by the new sound it adds in five regions
// of the spectrum, the pitch of its low end and how long its cymbal part rings; a kick
// has lows of its own, a snare wires as well as a body, a tom a tonal body, a cymbal
// brightness the others don't explain, so a kick and a hat played together give two
// notes. Where the clip's hits fall into two clear groups on one of these, the threshold
// between them is taken from the clip, so a dark kit and a bright kit both come out right.
// A hat hidden under a snare is put back when a steady hat pulse runs through it.
//
// Times are seconds of the analysed audio (the file's own time), like analysis::pitch.
// Plain computation, a moment on a long take: run it off the message thread.
namespace analysis::tomidi
{
struct Note
{
    double time = 0.0;   // seconds from the start of the audio
    double length = 0.0; // seconds
    int pitch = 60;      // MIDI note number
    int velocity = 100;  // 1..127
};

//==============================================================================
struct MelodyOptions
{
    // Snap to this key's notes when key >= 0 (0 = C ... 11 = B, as KeyEstimate::key);
    // otherwise to the nearest semitone.
    int key = -1;
    bool minor = false;

    // Take out how far the whole take sits from A = 440 Hz before snapping, so a singer
    // who is a little flat throughout still lands on the notes they meant.
    bool followTuning = true;

    // Whole octaves up (+) or down (-).
    int octaves = 0;

    // When fitHigh > fitLow: also moves the line by whole octaves so the middle of its
    // notes lands between these MIDI notes (bass() sets 28 .. 52, E1 .. E3).
    int fitLow = 0, fitHigh = 0;

    // Notes shorter than this join the note they touch (or go).
    double minLength = 0.06;

    // 0..1: higher keeps quieter notes. Notes more than 18 + 30 * sensitivity dB below
    // the loudest are left out (breaths, bleed, a quiet ghost of the backing).
    float sensitivity = 0.5f;

    // Quantise: grid lines in the audio's seconds (rhythm::makeGrid, or a tempo map's
    // grid moved into the file's time) and how far starts and ends move towards them.
    std::vector<double> grid;
    float quantise = 0.0f;

    static MelodyOptions bass();
};

// The notes of a single line, in time order, never overlapping.
std::vector<Note> melodyToMidi (const pitch::Track&, const MelodyOptions& = {}, double from = 0.0,
                                double to = std::numeric_limits<double>::max());
std::vector<Note> melodyToMidi (const juce::AudioBuffer<float>&, double sampleRate, const MelodyOptions& = {},
                                const std::function<bool()>& shouldContinue = {});

// How far the notes of a take sit from A = 440 Hz on average, in semitones (-0.5 .. 0.5;
// -0.2 = 20 cents flat). Longer notes count more.
float getTuningOffset (const std::vector<pitch::Note>&);

// The nearest note to `midi` (fractions are cents) that belongs to the key's scale
// (natural minor for minor keys), or the nearest semitone when key < 0.
int snapToKey (float midi, int key, bool minor);

//==============================================================================
namespace gm
{
    enum : int { kick = 36, snare = 38, closedHat = 42, openHat = 46, lowTom = 45, highTom = 48, crash = 49 };
}

struct DrumOptions
{
    float sensitivity = 0.5f;  // 0..1: higher finds softer hits
    double noteLength = 0.1;   // seconds, the length given to every hit
    std::vector<double> grid;  // as MelodyOptions
    float quantise = 0.0f;
};

// What each hit was found from, for tuning the rules (and the tests).
struct DrumHit
{
    double time = 0.0;

    // dB of new sound (over what was already ringing) in five regions of the spectrum:
    // 30-90 Hz (kick), 90-400 Hz (toms, snare body), 400 Hz-1.5 kHz, 1.5-5 kHz (snare
    // wires), 5-16 kHz (cymbals).
    std::array<float, 5> region { -120.0f, -120.0f, -120.0f, -120.0f, -120.0f };

    float bodyHz = 0.0f; // the strongest low pitch (35 - 400 Hz) just after the attack
    float ring = 0.0f;   // seconds the cymbal band (above 6 kHz) takes to fall 20 dB

    std::vector<int> notes; // what it was named: GM drum notes
};

// Every hit, in time order, with what it was named.
std::vector<DrumHit> findDrumHits (const juce::AudioBuffer<float>&, double sampleRate, const DrumOptions& = {},
                                   const std::function<bool()>& shouldContinue = {});

// The hits as notes, in time order (two notes can start together).
std::vector<Note> drumsToMidi (const juce::AudioBuffer<float>&, double sampleRate, const DrumOptions& = {},
                               const std::function<bool()>& shouldContinue = {});
std::vector<Note> toNotes (const std::vector<DrumHit>&, const DrumOptions& = {});

//==============================================================================
// Moves times `strength` (0..1) of the way to the nearest line of `grid` (sorted).
double quantiseTime (double time, const std::vector<double>& grid, float strength);
}
