#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>

#include <optional>

// Audio loops in tempo: is a file a loop (a whole number of bars at a tempo we can tell),
// and at what tempo exactly. A dropped or previewed loop is then played at the project's
// tempo (musical mode for a clip, an offline stretch for the library preview).
namespace daw::loops
{
struct Loop
{
    double bpm = 0.0;        // exact: bars x beatsPerBar beats fill the file's length
    double nominalBpm = 0.0; // what was found (name, chunk or detection), before fitting
    int bars = 0;
    juce::String from;       // "name", "acid" or "audio"
};

// A tempo written in a file name: "Drums 120bpm.wav", "loop_98_Am.wav", "Beat - 85 BPM".
// 0 when none (numbers outside 50..250, or that look like a take number, don't count).
double tempoFromName (const juce::String& fileNameWithoutExtension);

// A file `seconds` long at `bpm` is a loop when it holds a whole number of bars (1 to 32)
// within 3 %. Returns the bars (0: not a loop).
int wholeBars (double seconds, double bpm, int beatsPerBar);

// Is the file a loop? The tempo comes from, in order, the file name, the WAV acid chunk
// (`metadata`: a reader's metadataValues), then the audio (detectTempo, also trying half
// and double; a detected loop must be 1, 2, 4, 8, 16 or 32 bars). One-shots (acid flag, or under a bar at any tempo) and files longer than
// 32 bars are not loops. `audio` may be null (then no detection).
std::optional<Loop> findLoop (const juce::String& fileNameWithoutExtension, const juce::StringPairArray& metadata,
                              double seconds, const juce::AudioBuffer<float>* audio, double sampleRate, int beatsPerBar);

// The same for a file on disk (reads it; detection only for files up to 130 s).
std::optional<Loop> findLoop (const juce::File&, int beatsPerBar);
}
