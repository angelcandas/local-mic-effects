#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_events/juce_events.h>

#include "AudioPool.h"
#include "Edit.h"
#include "dsp/FxChain.h"

#include <algorithm>
#include <map>

namespace daw
{
// Plays an Edit: every track's clips through its insert chain, volume and pan, summed
// into the master bus, which has its own inserts and volume.
//
// Threading. The engine listens to the Edit on the message thread and keeps an immutable
// render graph (tracks, clips, the chain each track uses) for the audio thread. A change
// to the track or clip layout builds a new graph and hands it over with an atomic
// pointer swap; the audio thread never locks or waits, and old graphs are freed on the
// message thread. Mixer moves and insert values are atomics and don't need a new graph,
// so dragging a fader or knob is cheap and keeps effect tails ringing.
//
// Latency compensation: every track is delayed to match the track whose inserts add
// the most latency, so tracks stay in time with each other.
class DawEngine : private juce::ValueTree::Listener,
                  private juce::AsyncUpdater,
                  private juce::Timer
{
public:
    explicit DawEngine (Edit&);
    ~DawEngine() override;

    // Message thread, with the audio stopped (e.g. from prepareToPlay).
    void prepare (double sampleRate, int maximumBlockSize);
    double getSampleRate() const noexcept { return sampleRate; }

    // Message thread. Applies any pending change now instead of on the next message
    // loop pass (for tests, and before an offline render).
    void rebuildNow();

    AudioPool& getAudioPool() noexcept { return pool; }

    //==============================================================================
    // Transport, in samples at the session rate. Any thread.
    void play() noexcept                     { playing = true; }
    void stop() noexcept                     { playing = false; }
    void seek (juce::int64 sample) noexcept  { position = juce::jmax ((juce::int64) 0, sample); }
    bool isPlaying() const noexcept          { return playing.load(); }
    juce::int64 getPosition() const noexcept { return position.load(); }
    juce::int64 getLength() const noexcept   { return length.load(); } // end of the last clip

    // Delay the inserts add (the slowest track's chain plus the master chain), in samples.
    int getLatencySamples() const noexcept   { return latency.load(); }

    // Message thread. Peak level (linear) of a track's output, or the master's with
    // Edit::masterId, since the last call.
    float takePeak (int trackId);

    //==============================================================================
    // Audio thread. Renders numSamples into the first numChannels of `output`
    // (overwriting them) and advances the playhead while playing. Inserts keep running
    // while stopped, so reverb and echo tails ring out.
    void process (juce::AudioBuffer<float>& output, int numChannels, int numSamples) noexcept;

private:
    // A track's (or the master's) chain and mixer state. Shared between consecutive
    // graphs, so DSP state survives clip edits. Its audio-thread members are only used
    // by whichever graph is active.
    struct Strip
    {
        int id = 0;
        juce::String insertLayout; // insert ids in order: a different layout needs a new strip
        fx::FxChain chain;
        std::map<int, fx::Module*> modulesByInsertId;

        std::atomic<float> targetLeft { 1.0f }, targetRight { 1.0f };
        std::atomic<float> peak { 0.0f };

        // Audio thread.
        juce::SmoothedValue<float> left, right;
        juce::AudioBuffer<float> buffer;
    };

    struct Clip
    {
        AudioPool::Audio audio;
        juce::int64 start = 0, offset = 0, length = 0, fadeIn = 0, fadeOut = 0;
        float gain = 1.0f;
    };

    struct Track
    {
        std::shared_ptr<Strip> strip;
        std::vector<Clip> clips;

        // Latency compensation, before the inserts (audio thread).
        juce::AudioBuffer<float> delayBuffer;
        int delaySamples = 0, delayWrite = 0;
    };

    struct Graph
    {
        std::vector<Track> tracks;
        std::shared_ptr<Strip> master;
    };

    // ValueTree::Listener
    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override;
    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override        { triggerAsyncUpdate(); }
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override { triggerAsyncUpdate(); }
    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override         { triggerAsyncUpdate(); }
    void valueTreeRedirected (juce::ValueTree&) override                          { triggerAsyncUpdate(); }

    void handleAsyncUpdate() override { rebuild(); }
    void timerCallback() override     { collectGarbage(); }

    void rebuild();
    std::shared_ptr<Strip> getStrip (const juce::ValueTree& trackOrMaster);
    void updateMix();
    void setStripTargets (Strip&, const juce::ValueTree&, bool audible, bool jump);

    void publish (std::unique_ptr<Graph>);
    void collectGarbage();

    static void renderClip (const Clip&, juce::AudioBuffer<float>&, juce::int64 blockStart, int numSamples) noexcept;
    static void applyDelay (Track&, int numSamples) noexcept;
    void renderBlock (Graph&, juce::AudioBuffer<float>& output, int outputOffset, int numChannels, int numSamples) noexcept;

    Edit& edit;
    AudioPool pool;

    double sampleRate = 48000.0;
    int blockSize = 512;

    std::map<int, std::shared_ptr<Strip>> strips; // message thread: the current strip per id

    std::atomic<Graph*> incoming { nullptr }; // built, not yet picked up by the audio thread
    Graph* active = nullptr;                  // audio thread

    // Graphs the audio thread has replaced, freed on the message thread.
    static constexpr int retiredCapacity = 64;
    juce::AbstractFifo retiredFifo { retiredCapacity };
    std::array<Graph*, retiredCapacity> retired {};

    std::atomic<bool> playing { false };
    std::atomic<juce::int64> position { 0 }, length { 0 };
    std::atomic<int> latency { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DawEngine)
};
}
