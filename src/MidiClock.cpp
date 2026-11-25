#include "MidiClock.h"

#include <cmath>

namespace midiclock
{
//==============================================================================
void Follower::handle (const juce::MidiMessage& message, double seconds) noexcept
{
    if (message.isMidiClock())
    {
        tick (seconds);
    }
    else if (message.isMidiStart())
    {
        ticksSinceStart = 0;
        running = true;
        push ({ Event::Type::start, 0 });
    }
    else if (message.isMidiContinue())
    {
        running = true;
        push ({ Event::Type::resume, 0 });
    }
    else if (message.isMidiStop())
    {
        running = false;
        push ({ Event::Type::stop, 0 });
    }
    else if (message.isSongPositionPointer())
    {
        const auto sixteenths = message.getSongPositionPointerMidiBeat();
        ticksSinceStart = (juce::int64) sixteenths * ticksPerSixteenth;
        push ({ Event::Type::position, sixteenths });
    }
}

void Follower::tick (double seconds) noexcept
{
    if (resetRequested.exchange (false) || seconds - lastTick.load (std::memory_order_relaxed) > timeoutSeconds
        || seconds < lastTick.load (std::memory_order_relaxed))
    {
        count = 0; // the clock (re)starts: no tempo from before the gap
        next = 0;
        shown = 0.0;
        rawBpm = 0.0;
        shownBpm = 0.0;
    }

    times[(size_t) next] = seconds;
    next = (next + 1) % windowTicks;
    count = juce::jmin (count + 1, windowTicks);
    lastTick = seconds;
    ticksSinceStart.fetch_add (1, std::memory_order_relaxed);

    const auto at = [this] (int back) { return times[(size_t) ((next - 1 - back + 2 * windowTicks) % windowTicks)]; };

    // A new tempo: the last beat's ticks drift away from the average. Start again from the
    // newest interval (the tempo given out holds until there's enough to say).
    if (count > ticksPerBeat + 1)
    {
        const auto recent = ticksPerBeat / (seconds - at (ticksPerBeat));
        const auto whole = (count - 1) / (seconds - at (count - 1));
        if (std::isfinite (recent) && std::isfinite (whole) && std::abs (recent / whole - 1.0) > jumpRatio)
            count = 2;
    }

    if (count < 2)
        return;

    // The tick interval: a straight line through the window's tick times (least squares), which
    // averages the jitter of every tick rather than just the first and last.
    auto interval = 0.0;
    {
        const auto n = (double) count;
        const auto meanIndex = (n - 1.0) / 2.0;
        auto meanTime = 0.0;
        for (int i = 0; i < count; ++i)
            meanTime += at (i);
        meanTime /= n;
        auto num = 0.0, den = 0.0;
        for (int i = 0; i < count; ++i)
        {
            const auto x = (n - 1.0 - i) - meanIndex; // at (i) is i ticks back
            num += x * (at (i) - meanTime);
            den += x * x;
        }
        interval = den > 0.0 ? num / den : 0.0;
    }
    if (interval <= 0.0)
        return;

    const auto bpm = juce::jlimit (10.0, 999.0, 60.0 / (ticksPerBeat * interval));
    rawBpm = bpm;

    // Half a beat of ticks before a tempo is given out at all.
    if (count >= ticksPerBeat / 2 && (juce::exactlyEqual (shown, 0.0) || std::abs (bpm - shown) > hysteresisBpm))
    {
        shown = std::round (bpm * 10.0) / 10.0;
        shownBpm = shown;
    }
}

double Follower::getBpm (double nowSeconds) const noexcept
{
    if (nowSeconds - lastTick.load (std::memory_order_relaxed) > timeoutSeconds)
        return 0.0;
    return shownBpm.load (std::memory_order_relaxed);
}

void Follower::push (Event event) noexcept
{
    const auto scope = events.write (1);
    if (scope.blockSize1 > 0)
        eventData[(size_t) scope.startIndex1] = event;
}

bool Follower::popEvent (Event& event) noexcept
{
    const auto scope = events.read (1);
    if (scope.blockSize1 == 0)
        return false;
    event = eventData[(size_t) scope.startIndex1];
    return true;
}

void Follower::reset() noexcept
{
    resetRequested = true;
    running = false;
    shownBpm = 0.0;
    rawBpm = 0.0;
    lastTick = -1.0e9;
    Event dropped;
    while (popEvent (dropped)) {}
}

//==============================================================================
void Sender::beginBlock (double measuredMs, double rate, int numSamples) noexcept
{
    sampleRate = rate > 0.0 ? rate : 48000.0;

    // The callbacks come a little early or late; the samples don't. Follow the measured time
    // slowly from where the last block said this one would start, unless it's far off (the
    // first block, a dropout, a new device).
    const auto predicted = lastBlockMs + 1000.0 * lastBlockSamples / sampleRate;
    if (lastBlockSamples <= 0 || std::abs (measuredMs - predicted) > 20.0)
        blockMs = measuredMs;
    else
        blockMs = predicted + 0.05 * (measuredMs - predicted);

    lastBlockMs = blockMs;
    lastBlockSamples = numSamples;
}

void Sender::write (double ms, juce::uint8 b0, int size, juce::uint8 b1, juce::uint8 b2) noexcept
{
    const auto scope = fifo.write (1);
    if (scope.blockSize1 == 0)
        return; // the sender thread is stuck: drop rather than wait

    auto& message = data[(size_t) scope.startIndex1];
    message.ms = ms;
    message.bytes[0] = b0;
    message.bytes[1] = b1;
    message.bytes[2] = b2;
    message.size = size;

    if (b0 == 0xf8)
        ticksSent.fetch_add (1, std::memory_order_relaxed);
}

void Sender::startAt (double beat, double ms) noexcept
{
    if (beat <= 1.0e-6)
    {
        write (ms, 0xfa); // Start: the next tick is the song's first
        nextTick = 0;
        return;
    }

    // Song Position (sixteenths) then Continue: the next tick plays from there, so the ticks
    // start at that sixteenth (the first one at or after the playhead).
    const auto sixteenths = juce::jlimit (0, maxSongPosition, (int) std::ceil (beat * 4.0 - 1.0e-6));
    write (ms, 0xf2, 3, (juce::uint8) (sixteenths & 0x7f), (juce::uint8) ((sixteenths >> 7) & 0x7f));
    write (ms, 0xfb);
    nextTick = (juce::int64) sixteenths * ticksPerSixteenth;
}

void Sender::span (bool playing, double startBeat, double endBeat, double bpm, int offset, int numSamples) noexcept
{
    if (! enabled.load (std::memory_order_relaxed))
    {
        if (wasEnabled && wasPlaying)
            write (timeOf (offset), 0xfc);
        wasEnabled = wasPlaying = false;
        return;
    }

    if (! wasEnabled)
    {
        wasEnabled = true;
        wasPlaying = false;
        stoppedPhase = 0.0;
    }

    if (playing)
    {
        if (! wasPlaying)
        {
            startAt (startBeat, timeOf (offset));
        }
        else if (std::abs (startBeat - lastEndBeat) > 1.0e-4)
        {
            write (timeOf (offset), 0xfc); // a jump (a seek, the loop): stop, move, go on
            startAt (startBeat, timeOf (offset));
        }

        wasPlaying = true;
        lastEndBeat = endBeat;

        const auto length = endBeat - startBeat;
        if (length <= 0.0 || numSamples <= 0)
            return;

        for (;; ++nextTick)
        {
            const auto beat = (double) nextTick / ticksPerBeat;
            if (beat >= endBeat)
                break;
            if (beat >= startBeat - 1.0e-9)
                write (timeOf (offset) + 1000.0 * ((beat - startBeat) / length * numSamples) / sampleRate, 0xf8);
        }
        return;
    }

    if (wasPlaying)
    {
        write (timeOf (offset), 0xfc);
        wasPlaying = false;
        stoppedPhase = 0.0;
    }

    if (! whileStopped.load (std::memory_order_relaxed) || bpm <= 0.0)
    {
        stoppedPhase = 0.0;
        return;
    }

    // Free-running ticks at the tempo; stoppedPhase is how far (in ticks) the next one is.
    const auto ticksPerSample = bpm * ticksPerBeat / (60.0 * sampleRate);
    auto at = stoppedPhase / ticksPerSample;
    for (; at < numSamples; at += 1.0 / ticksPerSample)
        write (timeOf (offset) + 1000.0 * at / sampleRate, 0xf8);
    stoppedPhase = (at - numSamples) * ticksPerSample;
}

bool Sender::peek (Message& message) const noexcept
{
    if (fifo.getNumReady() == 0)
        return false;

    int start1, size1, start2, size2;
    fifo.prepareToRead (1, start1, size1, start2, size2);
    if (size1 == 0)
        return false;
    message = data[(size_t) start1];
    return true;
}

void Sender::pop() noexcept
{
    fifo.finishedRead (1);
}

void Sender::clear() noexcept
{
    fifo.finishedRead (fifo.getNumReady());
}

//==============================================================================
bool Output::canMakeVirtual() noexcept
{
   #if JUCE_WINDOWS || JUCE_ANDROID
    return false;
   #else
    return true;
   #endif
}

Output::Output (Sender& s) : juce::Thread ("MicFX MIDI clock"), sender (s) {}

Output::~Output()
{
    sender.setEnabled (false);
    stopThread (500);
}

juce::String Output::open (const juce::String& identifier)
{
    sender.setEnabled (false);

    {
        const juce::ScopedLock sl (lock);
        device.reset();
        openId = {};
        isOpenNow = false;
    }

    if (identifier.isEmpty())
        return {};

    std::unique_ptr<juce::MidiOutput> opened;
    if (identifier == virtualId)
    {
        if (canMakeVirtual())
            opened = juce::MidiOutput::createNewDevice (virtualName);
    }
    else
    {
        opened = juce::MidiOutput::openDevice (identifier);
    }

    if (opened == nullptr)
        return identifier == virtualId ? "Couldn't make the MicFX Clock port on this computer."
                                       : "Couldn't open that MIDI output. Is it plugged in, or is another app using it?";

    {
        const juce::ScopedLock sl (lock);
        device = std::move (opened);
        openId = identifier;
        isOpenNow = true;
    }

    if (! isThreadRunning())
        startThread (juce::Thread::Priority::highest);

    sender.setEnabled (true);
    return {};
}

void Output::run()
{
    while (! threadShouldExit())
    {
        Sender::Message message;
        if (! sender.peek (message))
        {
            wait (1);
            continue;
        }

        if (! isOpenNow.load())
        {
            sender.clear();
            wait (5);
            continue;
        }

        // Sleep until close to the time, then wait it out exactly.
        const auto early = message.ms - juce::Time::getMillisecondCounterHiRes();
        if (early > 2.0)
        {
            wait (juce::jmin (5, (int) early - 1));
            continue;
        }
        while (message.ms > juce::Time::getMillisecondCounterHiRes() && ! threadShouldExit())
            juce::Thread::yield();

        {
            const juce::ScopedLock sl (lock);
            if (device != nullptr)
                device->sendMessageNow (juce::MidiMessage (message.bytes, message.size));
        }
        sender.pop();
    }
}

//==============================================================================
void Sync::setFollowSource (const juce::String& identifier)
{
    {
        const juce::SpinLock::ScopedLockType sl (sourceLock);
        source = identifier;
    }
    follower.reset();
    following = identifier.isNotEmpty();
}

juce::String Sync::getFollowSource() const
{
    const juce::SpinLock::ScopedLockType sl (sourceLock);
    return source;
}

void Sync::handleIncoming (const juce::String& sourceId, const juce::MidiMessage& message) noexcept
{
    if (! following.load() || ! (message.isMidiClock() || message.isMidiStart() || message.isMidiStop()
                                 || message.isMidiContinue() || message.isSongPositionPointer()))
        return;

    {
        const juce::SpinLock::ScopedLockType sl (sourceLock);
        if (sourceId != source)
            return;
    }

    // Device input is stamped on the same clock as getMillisecondCounterHiRes (in seconds);
    // anything else is stamped now.
    const auto now = juce::Time::getMillisecondCounterHiRes() * 0.001;
    const auto stamp = message.getTimeStamp();
    follower.handle (message, std::abs (stamp - now) < 1.0 ? stamp : now);
}
} // namespace midiclock
