#include "CaptureRing.h"

void CaptureRing::configure (double sampleRate, int numChannels, double seconds)
{
    sampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
    numChannels = juce::jmax (1, numChannels);

    if (seconds <= 0.0)
    {
        on = false;
        const juce::SpinLock::ScopedLockType scoped (lock);
        data.free();
        capacity = 0;
        keepSeconds = 0.0;
        total = 0;
        return;
    }

    if (isOn() && channels.load() == numChannels && juce::approximatelyEqual (rate.load(), sampleRate)
        && juce::approximatelyEqual (keepSeconds.load(), seconds))
        return;

    const auto samples = (juce::int64) std::ceil ((seconds + marginSeconds) * sampleRate);

    // Uninitialised on purpose: the pages are only touched as audio arrives.
    juce::HeapBlock<float> fresh;
    fresh.allocate ((size_t) samples * (size_t) numChannels, false);

    if (fresh == nullptr)
        return;

    on = false;
    const juce::SpinLock::ScopedLockType scoped (lock);
    data = std::move (fresh);
    capacity = samples;
    channels = numChannels;
    rate = sampleRate;
    keepSeconds = seconds;
    total = 0;
    missed = 0;
    runStart = 0;
    runPosition = -1;
    nextPosition = -1;
    on = true;
}

void CaptureRing::clear()
{
    const juce::SpinLock::ScopedLockType scoped (lock);
    total = 0;
    missed = 0;
    runStart = 0;
    runPosition = -1;
    nextPosition = -1;
}

void CaptureRing::write (const float* const* source, int numSamples, juce::int64 timelinePosition) noexcept
{
    if (! on.load (std::memory_order_acquire) || numSamples <= 0)
        return;

    const juce::SpinLock::ScopedTryLockType scoped (lock);

    if (! scoped.isLocked() || data == nullptr)
    {
        missed += numSamples;
        return;
    }

    auto written = total.load (std::memory_order_relaxed);
    const auto numChannels = (juce::int64) channels.load (std::memory_order_relaxed);

    if (missed > 0)
    {
        // The gap holds old audio; it counts as written so the rest stays in time, and
        // it ends the run.
        written += missed;
        missed = 0;
        nextPosition = -2;
    }

    // Where the run starts over: the position jumped (or playing began or stopped).
    if (timelinePosition != nextPosition.load (std::memory_order_relaxed))
    {
        runStart = written;
        runPosition = timelinePosition;
    }
    nextPosition = timelinePosition >= 0 ? timelinePosition + numSamples : -1;

    auto skip = 0;
    if ((juce::int64) numSamples > capacity)
        skip = (int) ((juce::int64) numSamples - capacity); // more than the ring: keep the last of it

    const auto first = written + skip;
    const auto count = (juce::int64) numSamples - skip;

    for (juce::int64 ch = 0; ch < numChannels; ++ch)
    {
        auto* destination = data.get() + ch * capacity;
        auto index = first % capacity;
        const auto now = juce::jmin (count, capacity - index);
        std::memcpy (destination + index, source[ch] + skip, (size_t) now * sizeof (float));
        if (now < count)
            std::memcpy (destination, source[ch] + skip + now, (size_t) (count - now) * sizeof (float));
    }

    total.store (written + numSamples, std::memory_order_release);
}

double CaptureRing::getAvailableSeconds() const noexcept
{
    if (! isOn())
        return 0.0;

    const auto sampleRate = rate.load();
    const auto usable = (juce::int64) (keepSeconds.load() * sampleRate);
    return (double) juce::jmin (total.load (std::memory_order_acquire), usable) / sampleRate;
}

CaptureRing::Result CaptureRing::take (double seconds) const
{
    Result result;

    if (! isOn() || seconds <= 0.0)
        return result;

    const auto sampleRate = rate.load();
    const auto end = total.load (std::memory_order_acquire);
    const auto usable = (juce::int64) (keepSeconds.load() * sampleRate);
    auto count = juce::jmin ((juce::int64) std::llround (seconds * sampleRate), usable, end);

    // Played to the backing: only the part since the position last jumped is placed
    // against it. A capture longer than the run stops at the run's start.
    const auto runLength = end - runStart.load();
    const auto playing = nextPosition.load() >= 0 && runPosition.load() >= 0
                         && (double) runLength >= minRunSeconds * sampleRate;

    if (playing)
    {
        count = juce::jmin (count, runLength);
        result.startPosition = runPosition.load() + (end - count - runStart.load());
    }

    if (count <= 0)
        return result;

    const auto numChannels = channels.load();
    result.audio.setSize (numChannels, (int) count);

    // In slices, so the audio thread only ever waits a moment.
    constexpr juce::int64 slice = 16384;
    const auto begin = end - count;

    for (juce::int64 done = 0; done < count; done += slice)
    {
        const auto length = juce::jmin (slice, count - done);
        const juce::SpinLock::ScopedLockType scoped (lock);

        if (data == nullptr || capacity == 0 || channels.load() != numChannels)
            return {};

        // The audio thread may have written more by now; the margin keeps it off this part.
        for (int ch = 0; ch < numChannels; ++ch)
        {
            const auto* source = data.get() + (juce::int64) ch * capacity;
            auto index = (begin + done) % capacity;
            const auto now = juce::jmin (length, capacity - index);
            std::memcpy (result.audio.getWritePointer (ch, (int) done), source + index, (size_t) now * sizeof (float));
            if (now < length)
                std::memcpy (result.audio.getWritePointer (ch, (int) (done + now)), source, (size_t) (length - now) * sizeof (float));
        }
    }

    return result;
}

juce::String CaptureRing::writeWav (const juce::File& file, const juce::AudioBuffer<float>& audio,
                                    int firstChannel, int numChannels, double sampleRate)
{
    if (audio.getNumSamples() <= 0 || firstChannel + numChannels > audio.getNumChannels())
        return "There is nothing to save.";

    if (const auto made = file.getParentDirectory().createDirectory(); made.failed())
        return "Couldn't create the folder " + file.getParentDirectory().getFullPathName();

    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);

    if (! static_cast<juce::FileOutputStream&> (*stream).openedOk())
        return "Couldn't create " + file.getFullPathName();

    const auto options = juce::AudioFormatWriterOptions{}
                             .withSampleRate (sampleRate)
                             .withNumChannels (numChannels)
                             .withBitsPerSample (24);

    auto writer = juce::WavAudioFormat().createWriterFor (stream, options);

    if (writer == nullptr)
    {
        file.deleteFile();
        return "Couldn't write a WAV file at this sample rate";
    }

    std::vector<const float*> pointers;
    for (int ch = 0; ch < numChannels; ++ch)
        pointers.push_back (audio.getReadPointer (firstChannel + ch));

    if (! writer->writeFromFloatArrays (pointers.data(), numChannels, audio.getNumSamples()))
    {
        writer.reset();
        file.deleteFile();
        return "Couldn't write " + file.getFileName() + ": the disk may be full";
    }

    return {};
}
