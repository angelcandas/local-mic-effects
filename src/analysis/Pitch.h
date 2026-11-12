#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <functional>
#include <limits>
#include <memory>
#include <vector>

// The sung notes of a voice (or any single melodic line), for the note-based pitch editor.
//
// analyse() follows the pitch of a recording every 5 ms (YIN, on a copy brought down to
// about 16 kHz so a long take is quick), and its loudness. findNotes() cuts that pitch line
// into notes: a new note starts where the pitch settles somewhere else for a moment, or
// where the voice stops or dips between two syllables. Each note keeps where it was sung
// (its centre pitch, the median of its steady middle).
//
// A Note also carries its edits: how far it moves, how much of its slow drift and of its
// vibrato stay, its formant shift, its level and how it glides in from the note before.
// getShapes() turns a take's notes into curves over the file's time, which warp::render
// follows (see Warp.h). Everything is in the file's own time (seconds from its start), so
// warping, trimming or splitting a clip leaves its notes where they were sung.
//
// Plain computation; analyse() takes a moment on a long take, so it runs off the message
// thread.
namespace analysis::pitch
{
// MIDI note numbers (fractions are cents), 0 where nothing is sung.
struct Track
{
    double hop = 0.005;          // seconds between frames; frame i is at i * hop
    std::vector<float> midi;     // the pitch, 0 when unvoiced
    std::vector<float> energy;   // loudness in dB (RMS of 32 ms around the frame)

    int size() const noexcept { return (int) midi.size(); }
    double timeOf (int frame) const noexcept { return frame * hop; }
    int frameAt (double seconds) const noexcept;  // nearest frame, clamped
    float midiAt (double seconds) const noexcept; // between frames when both are voiced, else 0
    float energyAt (double seconds) const noexcept;
};

Track analyse (const juce::AudioBuffer<float>&, double sampleRate, const std::function<bool()>& shouldContinue = {});

// The same, remembered for the last few recordings (a render and the editor both ask for
// a take's pitch, often several times). Thread-safe.
std::shared_ptr<const Track> analyseCached (const juce::AudioBuffer<float>&, double sampleRate);

struct Note
{
    double start = 0.0, end = 0.0; // file seconds
    float pitch = 0.0f;            // where it was sung: MIDI note number, fractions are cents

    // Edits.
    float shift = 0.0f;       // semitones up or down
    float straighten = 0.0f;  // 0..1: how much of the pitch's slow drift is taken out (1 = flat)
    float vibrato = 1.0f;     // 0..2: how much vibrato stays (0 = none, 2 = twice as wide)
    float formant = 0.0f;     // semitones the voice's character moves (- deeper, + smaller)
    float gain = 0.0f;        // dB
    float transition = 0.06f; // seconds the pitch takes to glide into this note from the one before

    bool isEdited() const noexcept;
    static constexpr float defaultTransition = 0.06f, maxShift = 24.0f, maxFormant = 12.0f, maxGain = 24.0f;
};

bool anyEdited (const std::vector<Note>&);

// Notes between `from` and `to` seconds of the file, in time order, unedited.
std::vector<Note> findNotes (const Track&, double from = 0.0, double to = std::numeric_limits<double>::max());

// The median pitch sung between two times (the steady middle of it), 0 if nothing is sung.
float getPitch (const Track&, double start, double end);

// "start:end:pitch:shift:straighten:vibrato:formant:gain:transition;..." for the project file.
juce::String toString (const std::vector<Note>&);
std::vector<Note> fromString (const juce::String&);

// What the edits do at one moment of the file.
struct Shape
{
    float semitones = 0.0f; // pitch move
    float formant = 0.0f;   // formant move, semitones
    float gainDb = 0.0f;
};

// The edits as curves over the file, one Shape per frame of the track. A note's move,
// formant and level hold over it and glide to the next note's over its transition (gaps
// between notes take the glide too, so a consonant goes with its syllable). Its drift is
// the pitch line smoothed over 150 ms, less its centre; its vibrato is what's left.
std::vector<Shape> getShapes (const Track&, const std::vector<Note>&);
Shape shapeAt (const Track&, const std::vector<Shape>&, double seconds);
}
