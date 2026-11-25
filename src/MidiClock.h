#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_devices/juce_audio_devices.h>

#include <array>
#include <atomic>
#include <memory>

// MIDI clock sync: MicFX as the clock for other gear (drum machines, synths, another app),
// or following another clock.
//
//   Send:   24 clock ticks per beat, Start / Stop / Continue and Song Position from the DAW
//           transport (on its tempo map), or in Live mode from the Live tempo (clock in, the
//           echo's tap, the looper's tempo), with Start when the looper starts and Stop when
//           it stops. Optionally the ticks keep going while stopped, so the other gear shows
//           the tempo. The audio thread works out when each message is due (the moment its
//           sample is heard: the block's time plus the output latency plus an offset) and a
//           sender thread puts it on the wire at that time.
//   Follow: the ticks from one MIDI input become a smoothed tempo (Live mode's tempo: the
//           echo and the looper's bars), and its Start / Stop / Continue / Song Position play
//           and stop the DAW song. The DAW song keeps its own tempo.
namespace midiclock
{
constexpr int ticksPerBeat = 24;
constexpr int ticksPerSixteenth = ticksPerBeat / 4;
constexpr int maxSongPosition = 16383; // sixteenths: the Song Position Pointer's 14 bits

//==============================================================================
// Incoming clock. handle() runs on one thread (the MIDI input's); the getters and
// popEvent() on any one other thread.
class Follower
{
public:
    // No tick for this long: the clock has gone (20 BPM ticks every 125 ms).
    static constexpr double timeoutSeconds = 0.5;
    // The tempo averages up to this many ticks (four beats), and starts again from the
    // last beat's ticks when they move away from it by more than `jumpRatio`.
    static constexpr int windowTicks = 96;
    static constexpr double jumpRatio = 0.03;
    // The tempo given out moves only when it's this far from the last one (no flicker).
    static constexpr double hysteresisBpm = 0.08;

    struct Event
    {
        enum class Type { start, resume, stop, position };
        Type type = Type::start;
        int sixteenths = 0; // position
    };

    // A message from the input, arrived at `seconds` (any steady clock). Only clock,
    // start, continue, stop and song position mean anything; the rest is ignored.
    void handle (const juce::MidiMessage&, double seconds) noexcept;
    void tick (double seconds) noexcept;

    // The tempo, rounded to 0.1 BPM, or 0 when no clock has come for `timeoutSeconds`
    // before `nowSeconds` (or not enough ticks yet).
    double getBpm (double nowSeconds) const noexcept;
    // The averaged tempo, unrounded (0 before there is one). For tests.
    double getRawBpm() const noexcept { return rawBpm.load (std::memory_order_relaxed); }
    // Between a start / continue and a stop.
    bool isRunning() const noexcept { return running.load (std::memory_order_relaxed); }
    // Ticks since the last start (or song position), so a beat can blink.
    juce::int64 getTicksSinceStart() const noexcept { return ticksSinceStart.load (std::memory_order_relaxed); }

    // The transport messages in order; false when there are none.
    bool popEvent (Event&) noexcept;
    void reset() noexcept;

private:
    void push (Event) noexcept;

    std::array<double, windowTicks> times {};
    int count = 0, next = 0;
    double shown = 0.0;

    std::atomic<double> rawBpm { 0.0 }, shownBpm { 0.0 }, lastTick { -1.0e9 };
    std::atomic<bool> running { false }, resetRequested { false };
    std::atomic<juce::int64> ticksSinceStart { 0 };

    static constexpr int eventCapacity = 64;
    juce::AbstractFifo events { eventCapacity };
    std::array<Event, eventCapacity> eventData {};
};

//==============================================================================
// Outgoing clock. The audio thread turns the transport into messages with the time each
// is due; the sender thread (Output) reads them.
class Sender
{
public:
    struct Message
    {
        double ms = 0.0;           // when it's due, on Time::getMillisecondCounterHiRes()
        juce::uint8 bytes[3] {};
        int size = 1;
    };

    // Any thread.
    void setEnabled (bool on) noexcept          { enabled = on; }
    bool isEnabled() const noexcept             { return enabled.load(); }
    void setWhileStopped (bool on) noexcept     { whileStopped = on; }
    bool getWhileStopped() const noexcept       { return whileStopped.load(); }

    // Audio thread. The block's first sample is heard at `blockMs` (milliseconds on the
    // clock the messages use, measured at the callback: it's smoothed over the blocks).
    void beginBlock (double blockMs, double sampleRate, int numSamples) noexcept;
    double getBlockMs() const noexcept { return blockMs; } // smoothed (for tests)

    // Audio thread. A stretch of the block, `numSamples` long from sample `offset`: while
    // playing, the beats at its start and end; while stopped, the tempo (for the ticks
    // that keep going while stopped). A playing stretch that doesn't start where the last
    // one ended is a jump: Stop, the new song position and Continue.
    void span (bool playing, double startBeat, double endBeat, double bpm, int offset, int numSamples) noexcept;

    // The sender thread: the next message without taking it (false when there is none),
    // then pop() once it's been sent.
    bool peek (Message&) const noexcept;
    void pop() noexcept;
    void clear() noexcept; // drops what's waiting (the sender thread, with no output)

    // Messages written since the start (for tests and the status line).
    juce::int64 getTicksSent() const noexcept { return ticksSent.load (std::memory_order_relaxed); }

private:
    void write (double ms, juce::uint8 b0, int size = 1, juce::uint8 b1 = 0, juce::uint8 b2 = 0) noexcept;
    double timeOf (int sample) const noexcept { return blockMs + 1000.0 * sample / sampleRate; }
    void startAt (double beat, double ms) noexcept; // Start, or Song Position + Continue

    std::atomic<bool> enabled { false }, whileStopped { true };
    double blockMs = 0.0, sampleRate = 48000.0, lastBlockMs = 0.0;
    int lastBlockSamples = 0;

    // Audio thread state.
    bool wasPlaying = false, wasEnabled = false;
    double lastEndBeat = 0.0;
    juce::int64 nextTick = 0;       // the next tick while playing: beat nextTick / 24
    double stoppedPhase = 0.0;      // ticks while stopped, 0..1 to the next one

    static constexpr int capacity = 2048;
    juce::AbstractFifo fifo { capacity };
    std::array<Message, capacity> data {};
    std::atomic<juce::int64> ticksSent { 0 };
};

//==============================================================================
// The sender thread: puts the Sender's messages on a MIDI output when they're due.
class Output final : private juce::Thread
{
public:
    // The output for other apps on this computer (not on Windows, which has no virtual ports).
    static constexpr const char* virtualId = "virtual";
    static constexpr const char* virtualName = "MicFX Clock";
    static bool canMakeVirtual() noexcept;

    explicit Output (Sender&);
    ~Output() override;

    // Message thread. Opens the output with this identifier (virtualId, or a device's), or
    // none for empty. Returns an error, or empty.
    juce::String open (const juce::String& identifier);
    juce::String getOpenId() const { return openId; }
    bool isOpen() const noexcept { return isOpenNow.load(); }

private:
    void run() override;

    Sender& sender;
    juce::CriticalSection lock;
    std::unique_ptr<juce::MidiOutput> device;
    juce::String openId;
    std::atomic<bool> isOpenNow { false };
};

//==============================================================================
// Everything together, owned by the processor.
class Sync
{
public:
    Sync() : output (sender) {}

    Follower follower;
    Sender sender;
    Output output;

    // The input to follow (a MidiInput identifier), or empty for none. Any thread.
    void setFollowSource (const juce::String& identifier);
    juce::String getFollowSource() const;
    bool isFollowing() const noexcept { return following.load(); }

    // The MIDI input callback: the source's messages go to the follower. Any one thread.
    void handleIncoming (const juce::String& sourceId, const juce::MidiMessage&) noexcept;

    // The clock's tempo when following and it's coming in, else 0. Any thread.
    double getFollowedBpm() const noexcept
    {
        return following.load() ? follower.getBpm (juce::Time::getMillisecondCounterHiRes() * 0.001) : 0.0;
    }

    // When the output hears a sample: the output latency and an offset (negative sends
    // the clock earlier), in ms. Any thread.
    void setLatencyMs (double ms) noexcept { latencyMs = ms; }
    void setOffsetMs (double ms) noexcept  { offsetMs = juce::jlimit (-100.0, 100.0, ms); }
    double getOffsetMs() const noexcept    { return offsetMs.load(); }
    double getAheadMs() const noexcept     { return latencyMs.load() + offsetMs.load(); }

private:
    juce::SpinLock sourceLock;
    juce::String source;
    std::atomic<bool> following { false };
    std::atomic<double> latencyMs { 0.0 }, offsetMs { 0.0 };
};
} // namespace midiclock
