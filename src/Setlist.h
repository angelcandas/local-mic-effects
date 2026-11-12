#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "Looper.h"
#include "Multitrack.h"
#include "Scenes.h"

// A setlist for the stage (standalone app): the songs of a show, in order. Each song
// keeps its sound (every setting but the input gain), its scenes, its tempo (for the
// looper's bars), its key (for the autotune), its backing track, its lyrics (plain text,
// or .lrc with times) and notes. Loading a song puts all of that in place at once.
//
// Saved as a .micfxset file (XML). Message thread only.
class Setlist
{
public:
    Setlist (juce::AudioProcessor&, Scenes&, Looper&, Multitrack&);

    int size() const                     { return tree.getNumChildren(); }
    int getCurrent() const;              // the loaded song, or -1
    juce::String getName() const;        // the setlist's (its file name once saved)
    juce::File getFile() const           { return file; }
    bool isDirty() const                 { return dirty; }

    // Songs. add() captures the current sound, scenes, looper tempo and backing track.
    int addSong (const juce::String& name = {});
    void updateSong (int index);         // the current sound and scenes replace the song's
    void removeSong (int index);
    void moveSong (int index, int newIndex);
    // Any of { name, key (-1..11), minor, tempo (0 = none), backing (path), lyrics, notes }.
    void setSongInfo (int index, const juce::var& fields);

    // Puts the song's sound, scenes, tempo, key and backing track in place; returns false
    // if there's no such song. A missing backing file is reported in `warning`.
    bool loadSong (int index, juce::String& warning);
    bool step (int delta, juce::String& warning);

    // Files. Each returns an error message, or an empty string.
    void clear();
    juce::String save (const juce::File&);
    juce::String open (const juce::File&);

    // { name, file, dirty, current, songs: [{ name, key, minor, tempo, backing, lyrics, notes, scenes }] }.
    juce::var describe() const;

private:
    juce::ValueTree captureSong (const juce::String& name) const;
    juce::String captureSound() const;
    juce::File getBackingFile() const;

    juce::AudioProcessor& processor;
    Scenes& scenes;
    Looper& looper;
    Multitrack& multitrack;

    juce::ValueTree tree { "SETLIST" };
    juce::File file;
    bool dirty = false;
};
