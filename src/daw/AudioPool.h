#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_events/juce_events.h>

#include "Warp.h"

#include <algorithm>
#include <map>
#include <memory>
#include <set>

namespace daw
{
// Audio files decoded at the session sample rate, shared by every clip that uses them.
//
// Message thread only. get() returns the audio if it's ready, or starts decoding it on a
// background thread and returns null; onLoaded fires (on the message thread) when it's
// ready. The buffers are immutable once published, so the audio thread can read them.
class AudioPool
{
public:
    using Audio = std::shared_ptr<const juce::AudioBuffer<float>>;

    AudioPool();
    ~AudioPool();

    // Drops everything decoded at the old rate; files decode again on their next get().
    void setSampleRate (double);
    double getSampleRate() const noexcept { return sampleRate; }

    Audio get (const juce::File&);
    Audio loadNow (const juce::File&); // synchronous, for tests and small files

    // A warped and/or transposed copy of the file (see warp::render), rendered on the
    // background thread like a decode; onLoaded fires when it's ready. Neutral settings
    // give the plain file. Rendered copies stay cached while something uses them: call
    // releaseUnusedWarps() with the keys in use, and the rest go (the newest few are
    // kept, so undo doesn't render again).
    Audio get (const juce::File&, const warp::Settings&);
    Audio loadNow (const juce::File&, const warp::Settings&);
    static juce::String getWarpKey (const juce::File&, const warp::Settings&);
    void releaseUnusedWarps (const std::set<juce::String>& keysInUse);

    bool isLoading() const noexcept { return ! pending.empty() || ! warpsPending.empty(); }
    juce::String getError (const juce::File&) const;

    std::function<void()> onLoaded;
    std::function<void (const juce::String&)> onError;

private:
    void install (const juce::String& path, double rate, juce::AudioBuffer<float>&&, const juce::String& error);

    double sampleRate = 48000.0;
    std::map<juce::String, Audio> cache;
    std::map<juce::String, juce::String> errors;
    std::set<juce::String> pending;

    std::map<juce::String, Audio> warps;       // by getWarpKey()
    std::vector<juce::String> warpOrder;       // oldest first, for releaseUnusedWarps()
    std::set<juce::String> warpsPending;
    static constexpr size_t sparedWarps = 6;

    juce::ThreadPool loader { juce::ThreadPoolOptions{}.withThreadName ("MicFX audio pool").withNumberOfThreads (1) };
    std::shared_ptr<bool> alive = std::make_shared<bool> (true);

    JUCE_DECLARE_NON_COPYABLE (AudioPool)
};
}
