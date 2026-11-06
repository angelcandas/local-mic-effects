#pragma once

#include <juce_audio_formats/juce_audio_formats.h>

// Records the processed output (and optionally the dry mic input) to 24-bit WAV files.
//
// start() and stop() run on the message thread. write() runs on the audio thread and is
// real-time safe: it only pushes into the lock-free FIFOs of juce::AudioFormatWriter::
// ThreadedWriter, whose background thread does the disk I/O. The audio thread never
// waits: while start()/stop() are swapping writers it skips the block instead.
class Recorder
{
public:
    struct Take
    {
        juce::File file;    // processed output
        juce::File dryFile; // dry mic input; empty unless requested
        double seconds = 0.0;
        bool droppedAudio = false; // the disk couldn't keep up and some audio was lost
    };

    Recorder();
    ~Recorder();

    // Message thread. Stops (and saves) any recording in progress first.
    void prepare (double sampleRate, int numOutputChannels);

    // Message thread. Creates the folder if needed. Returns an error message, or an
    // empty string on success.
    juce::String start (const juce::File& folder, bool includeDry);

    // Message thread. Flushes and closes the files and returns them. When nothing is
    // recording, returns the last take (e.g. one that prepare() stopped).
    Take stop();

    // Audio thread. `output` has the channel count passed to prepare(); `dry` is mono
    // and may be null when no dry file is being written.
    void write (const float* const* output, const float* dry, int numSamples) noexcept;

    // Message thread. The most recent finished take (empty file if there's none yet).
    const Take& getLastTake() const noexcept { return last; }

    bool isRecording() const noexcept     { return recording.load(); }
    double getSeconds() const noexcept;

    static juce::File getDefaultFolder();

private:
    using Writer = juce::AudioFormatWriter::ThreadedWriter;

    std::unique_ptr<Writer> createWriter (const juce::File&, int numChannels, juce::String& error);

    juce::TimeSliceThread thread { "MicFX recorder" };
    juce::SpinLock writerLock; // guards the two writer pointers below
    std::unique_ptr<Writer> outputWriter, dryWriter;

    double sampleRate = 48000.0;
    int numChannels = 2;

    std::atomic<bool> recording { false };
    std::atomic<juce::int64> samplesWritten { 0 };
    std::atomic<bool> dropped { false };

    Take current, last;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Recorder)
};
