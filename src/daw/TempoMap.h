#pragma once

#include <juce_core/juce_core.h>

#include <vector>

// The project's tempo over time: tempo points at beat positions, each either a step (the
// tempo jumps there) or a ramp (the tempo glides there from the point before, changing
// evenly per beat). Before the first point and after the last, the tempo holds.
//
// Beats count from 0 at 0 seconds; a bar is beatsPerBar of them (see Edit). Everything
// that lines up with the music (the click, the ruler, the grid, snapping, quantize, and
// clips in musical mode) goes through here. Plain values and arithmetic, no allocation
// in the lookups, so the audio thread can use a copy.
namespace daw
{
class TempoMap
{
public:
    struct Point
    {
        double beat = 0.0;
        double bpm = 120.0;
        bool ramp = false; // glide into this point from the one before (the first point can't)

        bool operator== (const Point& other) const noexcept
        {
            return juce::exactlyEqual (beat, other.beat) && juce::exactlyEqual (bpm, other.bpm) && ramp == other.ramp;
        }
    };

    static constexpr double minBpm = 20.0, maxBpm = 400.0;

    // A steady tempo.
    explicit TempoMap (double bpm = 120.0);

    // Sorted, tempos limited to 20..400 BPM, points closer than 1/64 beat merged (the later
    // wins). The first point moves to beat 0; none at all gives 120 BPM.
    explicit TempoMap (std::vector<Point>);

    const std::vector<Point>& getPoints() const noexcept { return points; }
    bool isSteady() const noexcept { return points.size() == 1; }

    double toSeconds (double beat) const noexcept;
    double toBeats (double seconds) const noexcept;

    double getBpmAtBeat (double beat) const noexcept;
    double getBpmAt (double seconds) const noexcept { return getBpmAtBeat (toBeats (seconds)); }

    // The index of the point in force at a beat (the last one at or before it).
    int getPointIndexAt (double beat) const noexcept;

    // Lines every `steps` beats from one before a time to one after another (seconds), as seconds. Swing 0..1 delays
    // every second line, up to a triplet feel at 1 (steps shorter than a beat only).
    std::vector<double> makeGrid (double steps, double fromSeconds, double toSeconds, double swing = 0.0) const;

    // The nearest line of a grid `steps` beats apart.
    double snap (double seconds, double steps) const noexcept;

    // A tempo map that follows a performance played without a click: the bar lines land on
    // `downbeats` (seconds, the start of each bar, in order), with one tempo step per bar.
    // A bar line that is clearly misplaced (its two bars swing apart while the pair keeps
    // the pace) moves to the middle. The music before the first downbeat becomes whole
    // lead-in bars at a tempo close to the first bar's (their count goes to `leadInBars`).
    // Fewer than two downbeats give a steady 120 BPM.
    static TempoMap fromDownbeats (std::vector<double> downbeats, int beatsPerBar, int* leadInBars = nullptr);

    // The points after the first, "beat:bpm:ramp;..." (ramp 0 or 1), for the project file.
    // The first point's tempo is the project's `tempo`.
    juce::String changesToString() const;
    static TempoMap fromProject (double tempo, const juce::String& changes);

    bool operator== (const TempoMap& other) const noexcept { return points == other.points; }
    bool operator!= (const TempoMap& other) const noexcept { return ! (*this == other); }

private:
    // Seconds from point i to `beat` (beat within point i's span).
    double secondsWithin (size_t i, double beat) const noexcept;
    double beatsWithin (size_t i, double seconds) const noexcept;
    double slopeAfter (size_t i) const noexcept; // BPM per beat until the next point (0: steady)

    std::vector<Point> points;
    std::vector<double> startSeconds; // when each point is reached
};
}
