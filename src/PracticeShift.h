#pragma once

#include "Multitrack.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

namespace daw::warp { class Render; }

// The whole song in another key and/or at another speed (Practice), rendered with the warp
// engine into tracks that are already on the Multitrack.
//
// The song is cut into the warp engine's pieces (about 1.5 s each) and they're rendered on
// several background threads, the ones at the playhead first: within a moment of a change
// the part about to play is ready, so it can be swapped in where the song is, while the
// rest fills in behind it. A seek moves the focus, so the pieces there come next.
//
// Message thread for everything but the work itself.
class KeySpeedRender : public std::enable_shared_from_this<KeySpeedRender>
{
public:
    struct Part
    {
        std::shared_ptr<const Multitrack::Track> source; // the song's stem (kept alive while it renders)
        bool voice = false;                               // the voice keeps its formants
        std::shared_ptr<const Multitrack::Track> target;  // the track it's written into
        std::shared_ptr<Multitrack::Fill> fill;           // ... and its ready blocks
    };

    KeySpeedRender (double sampleRate, double speed, double semitones);
    ~KeySpeedRender();

    // Samples of the rendered copy of a stem `numSamples` long.
    static int getLength (int numSamples, double speed);

    // Starts working (each part's target must be getLength() samples, its fill empty), the
    // pieces at `focusSeconds` of the copy first. `threads` <= 0: one per core but one.
    // The object must be owned by a shared_ptr (the threads keep it alive).
    void start (std::vector<Part>, double focusSeconds, int threads = 0);

    void setFocus (double copySeconds) noexcept;
    void cancel() noexcept { cancelled = true; }

    // 0..1: the share of the pieces done.
    double getProgress() const noexcept;
    bool isFinished() const noexcept { return finished.load(); }
    bool isCancelled() const noexcept { return cancelled.load(); }

    // Every part has samples from..to (copy seconds) ready to play.
    bool isReady (double fromSeconds, double toSeconds) const;

    double getSpeed() const noexcept { return speed; }
    double getSemitones() const noexcept { return semitones; }
    const std::vector<Part>& getParts() const noexcept { return parts; }

private:
    struct State;
    void prepareAndRun (int threads);
    void work();
    bool takeNext (size_t& item);
    void finishItem (size_t item, const juce::AudioBuffer<float>&);

    const double sampleRate, speed, semitones;
    std::vector<Part> parts;
    std::shared_ptr<State> state;
    std::atomic<bool> cancelled { false }, finished { false };
    std::atomic<juce::int64> focus { 0 }; // copy samples

    JUCE_DECLARE_NON_COPYABLE (KeySpeedRender)
};
