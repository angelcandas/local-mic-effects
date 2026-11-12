#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <atomic>

// The Live mode looper: up to four stacked layers of the processed voice, played back
// under what you sing.
//
//   empty --main--> recording --main--> playing --main--> overdubbing --main--> playing
//
// The first layer sets the loop's length. With the tempo locked, that length is rounded
// to whole bars: pressing a little early keeps recording to the end of the bar
// ("closing"), pressing a little late trims the extra and plays on in time. Free loops
// keep exactly what you recorded. Each later layer starts where the playhead is, records
// for as long as overdub is on (several passes add up in that layer), and fades in and
// out so it never clicks. Undo takes off the top layer (or drops the one being recorded);
// redo puts it back until a new layer is recorded.
//
// Threading: the commands and the setters run on the message thread; commands queue up
// for the audio thread, which owns the loop. The layer memory (four stereo minutes' worth
// at most) is allocated on the message thread the first time a loop is recorded, while
// the audio thread isn't using it. The getters are safe anywhere; getOverview and
// writeLayer read the layers from the message thread, which is fine for drawing and for
// saving a loop that isn't being recorded.
class Looper
{
public:
    static constexpr int maxLayers = 4;
    static constexpr double maxSeconds = 60.0;
    static constexpr double minSeconds = 0.25; // a shorter first layer is thrown away
    static constexpr double fadeSeconds = 0.005;

    enum class State { empty, recording, closing, playing, overdubbing, stopped };
    enum class Command { main, stop, play, undo, redo, clear };

    static juce::String getStateName (State);

    // Audio stopped. Same rate: the loop stays. New rate: the loop is cleared.
    void prepare (double sampleRate);

    // Message thread. Queues a command for the audio thread (allocating the layers
    // first if this is the first recording). Returns false if the queue was full.
    bool send (Command);

    // Message thread. Allocates the layer memory now (if the looper is empty and hasn't
    // yet), so a command from the audio thread can start the first recording.
    void prepareMemory();

    // Audio thread, before process(): runs a command at once (a MIDI pedal). Starting the
    // first recording needs prepareMemory() to have run.
    void handleOnAudioThread (Command command) noexcept { handle (command); publish(); }

    // Message thread (any thread is fine: they're atomics). Gains are linear.
    void setLayerGain (int layer, float gain) noexcept;
    void setLayerMuted (int layer, bool muted) noexcept;
    void setVolume (float gain) noexcept { volume = gain; }
    // Bars at `bpm` with `beatsPerBar` beats; used when the first layer closes.
    void setTempo (double bpm, int beatsPerBar) noexcept;
    void setTempoLocked (bool locked) noexcept { tempoLocked = locked; }

    float getLayerGain (int layer) const noexcept;
    bool isLayerMuted (int layer) const noexcept;
    float getVolume() const noexcept   { return volume.load(); }
    double getBpm() const noexcept     { return bpm.load(); }
    int getBeatsPerBar() const noexcept { return beatsPerBar.load(); }
    bool isTempoLocked() const noexcept { return tempoLocked.load(); }

    // Audio thread. Records `input` (the processed voice) when recording, and adds the
    // loop's playback into `output`. They may be the same buffer. With one channel, the
    // left is recorded and played.
    void process (const juce::AudioBuffer<float>& input, juce::AudioBuffer<float>& output,
                  int numChannels, int numSamples) noexcept;

    // True while commands wait for the next block.
    bool hasPendingCommands() const noexcept  { return fifo.getNumReady() > 0; }

    // Status, as of the last processed block.
    State getState() const noexcept           { return state.load(); }
    int getNumLayers() const noexcept         { return numLayers.load(); }  // including one being recorded
    int getNumRedoLayers() const noexcept     { return numRedo.load(); }
    juce::int64 getLength() const noexcept    { return length.load(); }     // 0 until the first layer closes
    juce::int64 getPosition() const noexcept  { return position.load(); }   // playhead, or samples recorded so far
    juce::int64 getClosingTarget() const noexcept { return closingTarget.load(); }
    double getSampleRate() const noexcept     { return sampleRate; }
    // The bar length at the current tempo, in samples.
    juce::int64 getBarLength() const noexcept;

    // Message thread. Peak level of `layer` in `bins` equal slices of the loop (0..1).
    juce::Array<float> getOverview (int layer, int bins) const;

    // Message thread, while the layer isn't being recorded. Copies one pass of `layer`
    // (the parts an overdub didn't reach are silent). Returns false if there's no such layer.
    bool copyLayer (int layer, juce::AudioBuffer<float>& destination) const;

    // Message thread. Writes every layer (one pass each) as a 24-bit stereo WAV named
    // "<baseName> - layer N.wav" in `folder`, and returns them in layer order. Empty, with
    // `error` set, if there's no closed loop or a file can't be written.
    juce::Array<juce::File> saveLayers (const juce::File& folder, const juce::String& baseName, juce::String& error) const;

private:
    struct Layer
    {
        juce::AudioBuffer<float> audio;     // capacity: maxSeconds, stereo
        std::atomic<juce::int64> start { 0 };    // where the first pass started
        std::atomic<juce::int64> coverage { 0 }; // how much of one pass has been written
        float smoothedGain = 1.0f;
    };

    void handle (Command) noexcept;
    void startFirstLayer() noexcept;
    void requestClose (bool padInsteadOfWaiting) noexcept;
    void finishFirstLayer (juce::int64 newLength, juce::int64 playFrom) noexcept;
    void startOverdub() noexcept;
    void endOverdubNow() noexcept;
    void clearLoop() noexcept;
    bool isCovered (const Layer&, juce::int64 pos) const noexcept;
    void publish() noexcept;

    double sampleRate = 48000.0;
    juce::int64 capacity = 0;
    std::atomic<bool> allocated { false }, memoryWanted { false };
    std::array<Layer, maxLayers> layers;

    // Commands, message thread -> audio thread.
    juce::AbstractFifo fifo { 32 };
    std::array<Command, 32> commands {};
    juce::SpinLock sendLock; // several message-thread callers stay one producer

    // Audio thread state.
    State current = State::empty;
    int layerCount = 0, redoCount = 0;
    juce::int64 loopLength = 0, playhead = 0, recorded = 0, target = 0;
    float recordGain = 0.0f, recordGainTarget = 0.0f, rampStep = 1.0f;
    int fadeLength = 192;

    // Settings (any thread).
    std::array<std::atomic<float>, maxLayers> gains { 1.0f, 1.0f, 1.0f, 1.0f };
    std::array<std::atomic<bool>, maxLayers> mutes { false, false, false, false };
    std::atomic<float> volume { 1.0f };
    std::atomic<double> bpm { 120.0 };
    std::atomic<int> beatsPerBar { 4 };
    std::atomic<bool> tempoLocked { false };

    // Published status.
    std::atomic<State> state { State::empty };
    std::atomic<int> numLayers { 0 }, numRedo { 0 };
    std::atomic<juce::int64> length { 0 }, position { 0 }, closingTarget { 0 };
};
