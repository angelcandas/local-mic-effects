// Starter content: sounds and patterns that come with the app so a new project isn't silent.
// Everything is made by our own code (a few oscillators and noise, no recordings), so it has no
// licence strings. The drum kits are written once as WAV files into the app's data folder
// ("MicFX/Starter"), where the library lists them and a new Kit instrument loads its pads from.
// The patterns (drums, bass, chords, a lead hook) are plain note lists; DawBridge turns one into
// a pattern in the project (addStarterPattern).
#pragma once

#include <juce_core/juce_core.h>
#include <vector>

namespace micfx::starter
{
    struct Note { int pitch; double time, length; int velocity; };   // time and length in beats

    struct Pattern
    {
        juce::String name, category;   // category: Drums, Bass, Chords, Melody
        double beats;                  // length of the pattern
        std::vector<Note> notes;
        bool drum;                     // plays on a drum kit (rows per sound)
    };

    // The folder everything lives in (created on demand).
    juce::File getFolder();

    // Writes the WAV files that aren't there yet (or are from an older version). Cheap when they
    // are; safe to call often. False if the folder can't be written.
    bool ensure();

    // The kits, in the order they are offered.
    juce::StringArray getKitNames();

    // The sample files for a kit, in the Kit instrument's pad order (16 entries; a file that
    // doesn't exist is an empty File).
    std::vector<juce::File> getKitFiles (const juce::String& kit);

    // The patterns, in the order they are listed.
    const std::vector<Pattern>& getPatterns();
}
