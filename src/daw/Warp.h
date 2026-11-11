#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <functional>
#include <vector>

// Time warp and transpose for audio clips, rendered offline.
//
// A clip's recording is never changed. A warped or transposed clip plays a rendered
// copy of its file instead: the file run through a phase vocoder that follows the warp
// map (where each moment of the file lands) and shifts the pitch. Clip offsets and
// lengths are measured in that rendered copy, so trimming, splitting and moving a warped
// clip work exactly as for a plain one.
//
// Plain computation, no audio-thread constraints: rendering runs on the AudioPool's
// background thread.
namespace daw::warp
{
// Moves one moment of the file: `source` seconds into the file plays at `target` seconds
// into the rendered copy.
struct Marker
{
    double source = 0.0, target = 0.0;
    bool operator== (const Marker& other) const noexcept { return juce::exactlyEqual (source, other.source) && juce::exactlyEqual (target, other.target); }
};

// A piecewise-linear map from file time to rendered time, through the markers. Before
// the first marker and after the last, time runs at normal speed (so one marker just
// slides the audio). No markers: the identity.
class Map
{
public:
    Map() = default;

    // Sorts the markers and drops any that would make time run backwards or stall, or
    // stretch a stretch between two markers by more than 8x either way.
    explicit Map (std::vector<Marker>);

    const std::vector<Marker>& getMarkers() const noexcept { return markers; }
    bool isIdentity() const noexcept;

    double toTarget (double sourceSeconds) const noexcept;
    double toSource (double targetSeconds) const noexcept;

    // How fast the file plays at a moment of the rendered copy (1 = normal, 0.5 = half speed).
    double getRateAt (double targetSeconds) const noexcept;

    // "source:target;source:target..." in seconds, for the project file. Empty for the identity.
    juce::String toString() const;
    static Map fromString (const juce::String&);

    bool operator== (const Map& other) const noexcept { return markers == other.markers; }

    static constexpr double maxStretch = 8.0;

private:
    std::vector<Marker> markers;
};

// Everything that shapes a clip's rendered copy.
struct Settings
{
    Map map;
    double semitones = 0.0; // the clip's and its track's transpose together (fractions are cents)

    bool isNeutral() const noexcept { return map.isIdentity() && std::abs (semitones) < 1.0e-4; }

    // Identifies the rendered copy (with the file's path) in the AudioPool's cache.
    juce::String getKey() const;

    static constexpr double maxSemitones = 24.0;
};

// The file's audio warped and transposed. Its length is the map's target for the file's
// end (silence where the map starts the audio later than 0). `shouldContinue` is polled
// now and then; false stops early and returns an empty buffer.
juce::AudioBuffer<float> render (const juce::AudioBuffer<float>& source, double sampleRate, const Settings&,
                                 const std::function<bool()>& shouldContinue = {});
}
