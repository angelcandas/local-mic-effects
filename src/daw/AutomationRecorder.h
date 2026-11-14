#pragma once

#include "Edit.h"

#include <map>

// Recording automation: while the song plays with a write mode on, moving a track's
// volume, pan, a send, an effect value or a macro writes a lane (Edit::setLane).
//   touch: writes while the knob moves; let go (no move for `releaseSeconds`) and the
//          lane carries on as it was
//   latch: once touched, keeps writing the last value until playback stops
// While a knob writes, the engine holds that lane so you hear the knob (the volume ride
// isn't held: a fader move is added to the ride already there). Each finished pass is
// thinned and written as one undo step, and the knob goes back to where it was, so the
// song plays back what you did. Message thread.
namespace daw
{
class DawEngine;

class AutomationRecorder
{
public:
    enum class Mode { off, touch, latch };
    static constexpr double releaseSeconds = 0.35;

    AutomationRecorder (Edit&, DawEngine*);

    void setMode (Mode);
    Mode getMode() const noexcept { return mode; }
    static juce::String modeKey (Mode);
    static Mode modeFromKey (const juce::String&);

    // A knob of a track moved to `knob` (in the knob's own units: the fader's dB, pan,
    // send dB, the effect's value, the macro 0..1) at `time` seconds on the timeline.
    // Returns false (and records nothing) when not writing or the target isn't a track's.
    // Call before the knob's new value is stored, so its setting before the move is known.
    bool moved (int trackId, const juce::String& target, float knob, double time);

    // From a timer: touch passes that went quiet end. `time` is the playhead, `playing`
    // whether the song plays; when it stops, every pass ends (latch ones at `time`).
    void update (double time, bool playing);

    // Ends every pass now (a mode change, the project closing).
    void finishAll (double time);

    bool isWriting() const noexcept { return ! passes.empty(); }
    juce::StringArray getWritingTargets (int trackId) const;

private:
    struct Pass
    {
        double start = 0.0, last = 0.0;  // timeline seconds
        double lastMove = 0.0;           // wall clock (ms) of the last move
        float knobBefore = 0.0f;         // the knob's setting before the pass
        float rideAtStart = 0.0f;        // volume: the ride where the pass began
        float lastValue = 0.0f;          // in lane units
        std::vector<Edit::AutomationPoint> points;
    };

    void finish (int trackId, const juce::String& target, Pass&, double end);
    void restoreKnob (int trackId, const juce::String& target, float value);

    Edit& edit;
    DawEngine* engine;
    Mode mode = Mode::off;
    std::map<std::pair<int, juce::String>, Pass> passes;
    double lastTime = 0.0;
};
}
