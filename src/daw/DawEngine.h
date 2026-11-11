#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_events/juce_events.h>

#include "AudioPool.h"
#include "Recorder.h"
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
//
// Recording is always dry: the raw mic goes to a WAV file and becomes a clip on every
// armed track, and the track's inserts are applied on playback. Tracks with monitor on
// hear the mic live through their inserts.
class DawEngine : private juce::ValueTree::Listener,
                  private juce::AsyncUpdater,
                  private juce::Timer
{
public:
    // An offline engine (for exports) runs entirely on the thread that drives it: no
    // timer, and replaced graphs are freed straight away.
    explicit DawEngine (Edit&, bool realtime = true);

    // Clip edges that cut into a file's audio fade over this long, so edits don't click.
    static constexpr double antiClickSeconds = 0.004;
    ~DawEngine() override;

    // With the audio stopped (e.g. from prepareToPlay). Called off the message thread,
    // the work is posted to it; until then the old graph keeps playing safely.
    void prepare (double sampleRate, int maximumBlockSize);
    double getSampleRate() const noexcept { return sampleRate; }

    // Message thread. Applies any pending change now instead of on the next message
    // loop pass (for tests, and before an offline render).
    void rebuildNow();

    // Message thread (or the offline engine's thread). Decodes every clip's audio now
    // and rebuilds, so playback is complete from the first block.
    void loadAllNow();

    AudioPool& getAudioPool() noexcept { return pool; }

    //==============================================================================
    // Transport, in samples at the session rate. Any thread.
    void play() noexcept                     { playing = true; }
    void stop() noexcept                     { playing = false; }
    void seek (juce::int64 sample) noexcept  { position = juce::jmax ((juce::int64) 0, sample); }
    bool isPlaying() const noexcept          { return playing.load(); }
    juce::int64 getPosition() const noexcept { return position.load(); }
    juce::int64 getLength() const noexcept   { return length.load(); } // end of the last clip
    // While the Edit's loop is on, playback wraps from its end to its start (not while
    // recording, which always runs straight on).

    // Delay the inserts add (the slowest track's chain plus the master chain), in samples.
    int getLatencySamples() const noexcept   { return latency.load(); }

    //==============================================================================
    // Recording. Message thread.
    //
    // startRecording() needs at least one armed track. It starts the transport from
    // the playhead (after the count-in, if any) and writes the dry mic to a new WAV in
    // `folder`. stopRecording() stops the transport and adds the take as a clip on
    // every armed track, in one undo step. Returns an error message, or empty.
    juce::String startRecording (const juce::File& folder);
    Recorder::Take stopRecording();
    bool isRecording() const noexcept { return recorder.isRecording() || countingIn.load(); }
    juce::int64 getRecordStart() const noexcept { return recordStart.load(); } // where the take begins

    // Interface input + output latency, so takes line up with what was heard.
    void setRecordingLatency (juce::int64 samples) noexcept { recordingLatency = samples; }

    // Click on every beat at the project tempo while playing (accent on the first beat
    // of each bar, from the Edit's time signature).
    void setMetronome (bool on) noexcept    { metronome = on; }
    bool getMetronome() const noexcept      { return metronome.load(); }
    // Click loudness, 0 (silent) .. 1 (the default) .. 2.
    void setMetronomeVolume (float v) noexcept { metronomeVolume = juce::jlimit (0.0f, 2.0f, v); }
    float getMetronomeVolume() const noexcept  { return metronomeVolume.load(); }
    // Bars of click before recording starts (0 = none). The click plays during the
    // count-in even when the metronome is off.
    void setCountInBars (int bars) noexcept { countInBars = juce::jlimit (0, 4, bars); }
    int getCountInBars() const noexcept     { return countInBars.load(); }

    // Message thread. Peak level (linear) of a track's output, or the master's with
    // Edit::masterId, since the last call.
    float takePeak (int trackId);

    //==============================================================================
    // Audio thread. Renders numSamples into the first numChannels of `output`
    // (overwriting them) and advances the playhead while playing. Inserts keep running
    // while stopped, so reverb and echo tails ring out.
    // `input` is the mono mic (numSamples long), or null when there isn't one. It may
    // alias the first output channel: each chunk is read before it's overwritten.
    void process (juce::AudioBuffer<float>& output, int numChannels, int numSamples,
                  const float* input = nullptr) noexcept;

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
        std::atomic<bool> monitoring { false }; // mic in, through the inserts

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
        int blockSize = 512; // the strips' buffer size: the most one chunk can render
    };

    // ValueTree::Listener
    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override;
    // Markers don't change what plays, so they don't need a new graph.
    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree& child) override        { if (! child.hasType (ids::MARKER)) triggerAsyncUpdate(); }
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree& child, int) override { if (! child.hasType (ids::MARKER)) triggerAsyncUpdate(); }
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
    void renderBlock (Graph&, juce::AudioBuffer<float>& output, int outputOffset, int numChannels, int numSamples,
                      const float* input) noexcept;
    void renderClick (juce::AudioBuffer<float>&, int offset, int numChannels, int numSamples, juce::int64 blockStart) noexcept;
    void updateTempo();
    void updateLoop();

    Edit& edit;
    const bool realtime;
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
    std::shared_ptr<bool> alive = std::make_shared<bool> (true);

    Recorder recorder;
    std::atomic<bool> countingIn { false };
    std::atomic<juce::int64> recordStart { 0 };
    std::atomic<juce::int64> recordingLatency { 0 };

    std::atomic<bool> loopOn { false };
    std::atomic<juce::int64> loopStart { 0 }, loopEnd { 0 };

    std::atomic<bool> metronome { false };
    std::atomic<int> countInBars { 0 };
    std::atomic<double> samplesPerBeat { 24000.0 };
    std::atomic<int> beatsPerBar { 4 };
    std::atomic<float> metronomeVolume { 1.0f };
    float clickPhase = 0.0f, clickIncrement = 0.0f, clickLevel = 0.0f, clickDecay = 0.0f; // audio thread

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DawEngine)
};
}
