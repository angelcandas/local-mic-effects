#pragma once

#include "Edit.h"

#include <vector>

// Standard MIDI files (.mid) in and out of the DAW's MIDI tracks and clips.
//
// A Song is what a file holds, in beats: notes grouped into parts (one per track and
// channel of the file), the file's tempo changes and its time signature. Importing puts
// each part on its own MIDI track as a clip, placed by beats on the project's tempo map
// (so a file at 90 BPM played in a 120 BPM project keeps its bars; an empty project takes
// the file's tempo). Exporting writes a clip or a MIDI track (its clips and pattern
// blocks) as a format 1 file at 480 ticks per beat, starting on a bar line.
//
// Message thread (it reads and edits the Edit); the read and write functions are plain.
namespace daw::midifile
{
struct Note
{
    int pitch = 60, velocity = 100;
    double beat = 0.0, beats = 1.0; // from the start of the part; length
};

// A controller point: a CC, pitch bend or channel pressure (Edit::controllerCC and the like),
// valued as in the project's CTRL (bend -8192..8191).
struct Controller
{
    juce::String kind;
    int number = 0, value = 0;
    double beat = 0.0;
};

struct Part
{
    juce::String name;
    bool drums = false; // MIDI channel 10
    std::vector<Note> notes;
    std::vector<Controller> controllers;
};

struct Song
{
    std::vector<Part> parts;
    std::vector<TempoMap::Point> tempo; // beats from the start of the song; empty: no tempo in the file
    int beatsPerBar = 4;
};

// Reads a file. False with `error` for a file that isn't MIDI, has no notes, or counts
// time in frames (SMPTE) rather than beats.
bool read (const juce::File&, Song&, juce::String& error);

// Writes a Song (parts as tracks, drum parts on channel 10). False with `error` if the
// file can't be written.
bool write (const juce::File&, const Song&, juce::String& error);

//==============================================================================
struct Placed
{
    juce::Array<int> tracks, clips;
    double tempo = 0.0; // the project's tempo, changed to the file's (0: left alone)
};

struct Instruments
{
    Edit::InstrumentInfo melodic, drums;
};

// Adds the song to the project at `startSeconds`. `firstTrack` (a MIDI track, or -1) takes
// the first melodic part; every other part gets a new track after it (or at the end).
// One undo step if the caller opened one. `baseName` names parts that have no name.
Placed addToEdit (Edit&, const Song&, double startSeconds, int firstTrack, const Instruments&, const juce::String& baseName);

// A clip's notes (inside its window, transposed as they play) or all of a track's (its
// clips and pattern blocks), starting on the bar line before the first note. Muted clips
// are left out of a track. Empty parts if there are no notes.
Song fromClip (const Edit&, int clipId);
Song fromTrack (const Edit&, int trackId);
}
