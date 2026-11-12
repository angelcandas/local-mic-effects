#include "Recorder.h"

Recorder::Recorder()
{
    thread.startThread (juce::Thread::Priority::normal);
}

Recorder::~Recorder()
{
    stop();
    thread.stopThread (2000);
}

void Recorder::prepare (double newSampleRate, int numOutputChannels)
{
    stop();
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 48000.0;
    numChannels = juce::jmax (1, numOutputChannels);
}

juce::File Recorder::getDefaultFolder()
{
    return juce::File::getSpecialLocation (juce::File::userMusicDirectory).getChildFile ("MicFX Recordings");
}

std::unique_ptr<Recorder::Writer> Recorder::createWriter (const juce::File& file, int channels, juce::String& error)
{
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);

    if (! static_cast<juce::FileOutputStream&> (*stream).openedOk())
    {
        error = "Couldn't create " + file.getFullPathName();
        return {};
    }

    const auto options = juce::AudioFormatWriterOptions{}
                             .withSampleRate (sampleRate)
                             .withNumChannels (channels)
                             .withBitsPerSample (24);

    auto writer = juce::WavAudioFormat().createWriterFor (stream, options);

    if (writer == nullptr)
    {
        error = "Couldn't write a WAV file at this sample rate";
        file.deleteFile();
        return {};
    }

    // Two seconds of slack in case the disk stalls.
    return std::make_unique<Writer> (writer.release(), thread, (int) (sampleRate * 2.0));
}

juce::String Recorder::start (const juce::File& folder, bool includeDry, const juce::String& label)
{
    stop();

    if (const auto result = folder.createDirectory(); result.failed())
        return "Couldn't create the folder " + folder.getFullPathName() + ": " + result.getErrorMessage();

    const auto name = "MicFX " + juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H-%M-%S") + (label.isNotEmpty() ? " " + label : juce::String());

    Take take;
    take.file = folder.getChildFile (name + ".wav").getNonexistentSibling();

    juce::String error;
    auto output = createWriter (take.file, numChannels, error);

    if (output == nullptr)
        return error;

    std::unique_ptr<Writer> dry;

    if (includeDry)
    {
        take.dryFile = take.file.getSiblingFile (take.file.getFileNameWithoutExtension() + " (dry).wav");
        dry = createWriter (take.dryFile, 1, error);

        if (dry == nullptr)
        {
            output.reset();
            take.file.deleteFile();
            return error;
        }
    }

    current = take;
    samplesWritten = 0;
    dropped = false;
    firstPosition = 0;

    {
        const juce::SpinLock::ScopedLockType lock (writerLock);
        outputWriter = std::move (output);
        dryWriter = std::move (dry);
    }

    recording = true;
    return {};
}

Recorder::Take Recorder::stop()
{
    std::unique_ptr<Writer> output, dry;

    {
        const juce::SpinLock::ScopedLockType lock (writerLock);
        output = std::move (outputWriter);
        dry = std::move (dryWriter);
    }

    if (output == nullptr)
        return last;

    recording = false;

    // Deleting the ThreadedWriters flushes what's left in their FIFOs and closes the files.
    output.reset();
    dry.reset();

    current.seconds = getSeconds();
    current.droppedAudio = dropped.load();
    current.timelineStart = firstPosition.load();
    last = std::exchange (current, {});
    return last;
}

void Recorder::write (const float* const* output, const float* dry, int numSamples,
                      juce::int64 timelinePosition) noexcept
{
    const juce::SpinLock::ScopedTryLockType lock (writerLock);

    if (! lock.isLocked() || outputWriter == nullptr)
        return;

    auto ok = outputWriter->write (output, numSamples);

    if (dryWriter != nullptr)
        ok = (dry != nullptr && dryWriter->write (&dry, numSamples)) && ok;

    if (! ok)
        dropped = true;

    if (samplesWritten.load() == 0)
        firstPosition = timelinePosition;

    samplesWritten += numSamples;
}

double Recorder::getSeconds() const noexcept
{
    return (double) samplesWritten.load() / sampleRate;
}
