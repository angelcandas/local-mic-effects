#pragma once

#include "Edit.h"

#include <atomic>
#include <functional>

// Offline rendering of a session to audio files, faster than real time.
//
// The export renders a copy of the session with its own engine, so the live engine
// (and what you hear) is untouched, effects start from a clean state, and the result
// matches real-time playback. The metronome and live input are never in an export.
//
// Construct and call from any one thread (e.g. a background job); `edit` is only read
// while the constructor copies it, so that part must happen on the message thread.
namespace daw
{
class Exporter
{
public:
    struct Settings
    {
        double sampleRate = 48000.0;
        juce::String format = "wav"; // "wav", "aiff", "flac", "ogg" or "mp3" (see formatExtension)
        int bitDepth = 24;          // 16 (dithered), 24, or 32 (float, WAV only); ignored for ogg and mp3
        int oggQuality = 8;         // ogg and mp3: index into qualityNames (mp3: 2 = 192 kbps)
        double start = 0.0;         // seconds
        double end = -1.0;          // seconds; < 0 = the end of the last clip
        double tail = 2.0;          // seconds after `end` so reverb and echo ring out
        bool normalise = false;     // scale so the peak sits at normaliseTo
        float normaliseTo = -1.0f;  // dBFS
    };

    struct Result
    {
        juce::String error;         // empty on success
        bool cancelled = false;
        double seconds = 0.0;
        float peakDecibels = -100.0f; // of the written file
        juce::Array<juce::File> files;
    };

    // The file extension for a format name (unknown names give "wav"), the bit depths it can
    // hold, and the names of the quality steps (ogg, mp3) (index = Settings::oggQuality).
    static juce::String formatExtension (const juce::String& format);
    static juce::Array<int> formatBitDepths (const juce::String& format);
    static juce::StringArray qualityNames (const juce::String& format); // ogg and mp3 only, else empty

    // Called with 0..1. Return false to cancel.
    using Progress = std::function<bool (double)>;

    // Message thread: takes a snapshot of the session.
    Exporter (Edit&, Settings);

    // The full mix (master bus and its inserts) to one stereo WAV.
    Result exportMix (const juce::File& file, Progress = {});

    // One stereo WAV per track, named after it, into `folder`: each track through its
    // inserts, volume and pan, without the master bus (its inserts and volume), so the
    // stems sum to the mix before master processing. Each stem covers the whole song so
    // they line up. With normalise on, each file is normalised on its own.
    Result exportStems (const juce::File& folder, Progress = {});

    // One track to one stereo WAV for "Bounce to a new track": through its inserts (and
    // instrument), at 0 dB and centred, without its sends, the returns or the master bus.
    // `forFreeze`: the track's own volume ride and pan automation are left out (they keep
    // playing live over the frozen audio).
    Result exportTrack (int trackId, const juce::File& file, Progress = {}, bool forFreeze = false);

private:
    Result render (const juce::ValueTree& session, const juce::File&, const Progress&, double progressFrom, double progressTo);

    juce::ValueTree snapshot;
    Settings settings;
};
}
