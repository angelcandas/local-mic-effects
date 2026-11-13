#pragma once

#include <juce_audio_formats/juce_audio_formats.h>

#include <atomic>

// Capture: the last few minutes of what came in, always kept, so a take you didn't
// press Record for isn't lost. A ring of audio that the audio thread writes all the
// time and the message thread copies the newest part of.
//
// Memory is allocated by configure() (message thread, audio may be running). The audio
// thread only copies samples in: it never allocates, and when the message thread is in
// the middle of a copy it skips that block rather than wait (the ring gets a few
// milliseconds of old audio there, and the "run" below restarts).
//
// Where it was played: write() takes the position of the block on a timeline (the
// backing track's, or the DAW's) or -1 when nothing was playing. A "run" is the stretch
// of blocks since the position last jumped (stop, seek, loop) so that a capture made
// while playing can be placed against the music it was played to.
class CaptureRing
{
public:
    // Samples kept apart from the newest ones so a copy never reads what the audio
    // thread is overwriting.
    static constexpr double marginSeconds = 2.0;
    static constexpr double minRunSeconds = 1.0;

    struct Result
    {
        juce::AudioBuffer<float> audio;     // empty when there's nothing yet
        juce::int64 startPosition = -1;     // timeline sample of audio's first sample, or -1: not played to anything
    };

    CaptureRing() = default;

    // Message thread. Keeps `seconds` of `numChannels` channels; 0 or less frees the
    // memory and turns the ring off. Reconfiguring to the same values keeps the audio.
    void configure (double sampleRate, int numChannels, double seconds);

    // Audio thread. `channels` has numChannels pointers (as configured).
    void write (const float* const* channels, int numSamples, juce::int64 timelinePosition = -1) noexcept;

    // Message thread. Forgets what's been recorded (keeps the memory).
    void clear();

    bool isOn() const noexcept                { return on.load (std::memory_order_acquire); }
    int getNumChannels() const noexcept       { return channels.load(); }
    double getSampleRate() const noexcept     { return rate.load(); }

    // Seconds of audio a capture could take now (up to the length configured).
    double getAvailableSeconds() const noexcept;

    // Message thread. The newest `seconds` (or what there is). While the position has
    // been running without a jump for at least a second, the capture stops at where the
    // run began, and says where on the timeline it starts.
    Result take (double seconds) const;

    // Any thread. Writes `numChannels` channels of `audio` from `firstChannel` as a 24-bit
    // WAV. Returns an error message, empty on success.
    static juce::String writeWav (const juce::File&, const juce::AudioBuffer<float>& audio,
                                  int firstChannel, int numChannels, double sampleRate);

private:
    mutable juce::SpinLock lock;
    juce::HeapBlock<float> data;
    juce::int64 capacity = 0; // samples per channel
    std::atomic<int> channels { 0 };
    std::atomic<double> rate { 48000.0 };
    std::atomic<double> keepSeconds { 0.0 };
    std::atomic<bool> on { false };

    std::atomic<juce::int64> total { 0 }; // samples written since clear()

    // The current run (audio thread writes, message thread reads).
    std::atomic<juce::int64> runStart { 0 };      // `total` where it began
    std::atomic<juce::int64> runPosition { -1 };  // timeline position there; -1 = nothing playing
    std::atomic<juce::int64> nextPosition { -1 }; // where the next block should start to continue the run
    juce::int64 missed = 0;                       // audio thread: samples skipped while a copy held the lock

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CaptureRing)
};
