#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_events/juce_events.h>

#include "Warp.h"

#include <algorithm>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
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
    // kept, so undo doesn't render again). Linked settings render every linked file in
    // one go and keep each one's copy. A render still running for a file whose clips have
    // moved on to other settings (a knob dragged past it) is cancelled there too, so the
    // copy that's wanted now isn't stuck behind renders nobody will hear.
    Audio get (const juce::File&, const warp::Settings&);
    Audio loadNow (const juce::File&, const warp::Settings&);
    static juce::String getWarpKey (const juce::File&, const warp::Settings&);
    void releaseUnusedWarps (const std::set<juce::String>& keysInUse);

    // Renders are cut into pieces (see warp::Render) kept in a cache of their own (up to
    // segmentBytes, the least recently used go first), so an edit renders only the pieces
    // it changed and a copy is put together from the rest. A copy's missing pieces render
    // in parallel on the warp threads; pieces that finish stay cached even when their copy
    // is cancelled. Undo usually finds every piece of the old copy still cached.
    //
    // While a copy renders: the stretches of it (seconds into the copy, [from, to] pairs,
    // joined where they touch) still to come. Empty when it's ready or not asked for.
    std::vector<std::pair<double, double>> getRenderingRanges (const juce::File&, const warp::Settings&) const;
    int getPiecesRendered() const noexcept;  // since the pool was made (tests and timings)
    size_t getPieceBytes() const;            // cached pieces' audio
    void setPieceCacheBytes (size_t);        // the cache's limit (tests)
    static int getWarpThreads() noexcept;

    bool isLoading() const noexcept { return ! pending.empty() || ! warpsPending.empty(); }
    juce::String getError (const juce::File&) const;

    std::function<void()> onLoaded;
    std::function<void (const juce::String&)> onError;

private:
    Audio getLinked (const juce::File&, const warp::Settings&);
    void installWarp (const juce::String& key, juce::AudioBuffer<float>&&);
    void install (const juce::String& path, double rate, juce::AudioBuffer<float>&&, const juce::String& error);

    double sampleRate = 48000.0;
    std::map<juce::String, Audio> cache;
    std::map<juce::String, juce::String> errors;
    std::set<juce::String> pending;

    std::map<juce::String, Audio> warps;       // by getWarpKey()
    std::vector<juce::String> warpOrder;       // oldest first, for releaseUnusedWarps()
    std::set<juce::String> warpsPending;
    // A render in flight: its cancel flag and which stretches of the copy are still to come.
    struct Job
    {
        std::atomic<bool> cancel { false };
        mutable std::mutex lock;
        bool planned = false;                        // false: the whole copy is to come
        std::vector<std::pair<double, double>> ranges;
        double length = 0.0;                         // the copy's, seconds
    };
    using CancelFlag = std::shared_ptr<Job>;
    std::map<juce::String, CancelFlag> warpJobs; // pending key -> its render
    static constexpr size_t sparedWarps = 2;     // whole copies kept past use (undo re-assembles from pieces anyway)

public:
    struct Pieces; // the piece cache (thread-safe, shared with the render jobs)

private:
    std::shared_ptr<Pieces> pieces;
    juce::String getPiecePrefix (const juce::String& files) const;
    juce::AudioBuffer<float> renderNow (const std::shared_ptr<const juce::AudioBuffer<float>>& source, double rate, const warp::Settings&,
                                        const juce::String& prefix, const std::function<bool()>& keepGoing, const CancelFlag&,
                                        int helpers);

    juce::ThreadPool loader { juce::ThreadPoolOptions{}.withThreadName ("MicFX audio pool").withNumberOfThreads (1) };
    juce::ThreadPool warpPool { juce::ThreadPoolOptions{}.withThreadName ("MicFX warp").withNumberOfThreads (getWarpThreads()) };
    std::shared_ptr<bool> alive = std::make_shared<bool> (true);

    JUCE_DECLARE_NON_COPYABLE (AudioPool)
};
}
