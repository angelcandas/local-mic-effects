#include "ComputerAudio.h"

namespace
{
// A loopback (or any other) input device, opened on its own next to the interface.
class DeviceBackend final : public ComputerAudio::Backend,
                            private juce::AudioIODeviceCallback
{
public:
    DeviceBackend (ComputerAudio& o, std::unique_ptr<juce::AudioIODevice> d)
        : owner (o), device (std::move (d)) {}

    ~DeviceBackend() override
    {
        device->stop();
        device->close();
    }

    double getSampleRate() const override       { return device->getCurrentSampleRate(); }
    juce::String getName() const override       { return device->getName(); }

    juce::String startCapture() override
    {
        device->start (this);
        return device->isPlaying() ? juce::String() : device->getLastError();
    }

private:
    void audioDeviceIOCallbackWithContext (const float* const* inputs, int numInputs,
                                           float* const* outputs, int numOutputs, int numSamples,
                                           const juce::AudioIODeviceCallbackContext&) override
    {
        for (int ch = 0; ch < numOutputs; ++ch)
            if (outputs[ch] != nullptr)
                juce::FloatVectorOperations::clear (outputs[ch], numSamples);

        if (numInputs >= 2 && inputs[0] != nullptr && inputs[1] != nullptr)
            owner.push (inputs[0], inputs[1], numSamples);
        else if (numInputs >= 1 && inputs[0] != nullptr)
            owner.push (inputs[0], nullptr, numSamples);
    }

    void audioDeviceAboutToStart (juce::AudioIODevice*) override {}
    void audioDeviceStopped() override {}

    ComputerAudio& owner;
    std::unique_ptr<juce::AudioIODevice> device;
};
} // namespace

ComputerAudio::ComputerAudio()
{
    history.setSize (2, 48000);
    history.clear();
}

ComputerAudio::~ComputerAudio()
{
    stop();
}

void ComputerAudio::prepare (double newSampleRate, int maxBlockSize)
{
    const juce::SpinLock::ScopedLockType producer (producerLock);
    const juce::SpinLock::ScopedLockType consumer (consumerLock);

    sampleRate = newSampleRate;
    maxBlock = juce::jmax (maxBlockSize, 4096);
    history.setSize (2, juce::jmax (maxBlock + 1, (int) (maxRecordDelaySeconds * sampleRate) + maxBlock));
    history.clear();
    historyWrite = 0;
    gain.reset (sampleRate, 0.05);
    gain.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (gainDb.load(), -60.0f));

    if (backend != nullptr)
        ring.prepare (backend->getSampleRate(), sampleRate);
}

juce::Array<ComputerAudio::SourceInfo> ComputerAudio::listSources (juce::AudioIODeviceType* type, const juce::String& exclude)
{
    juce::Array<SourceInfo> sources;

    if (isSystemCaptureAvailable())
        sources.add ({ systemSource, "Computer audio (all apps)" });

    if (type != nullptr)
        for (const auto& name : type->getDeviceNames (true))
            if (name != exclude)
                sources.add ({ "device:" + name, name });

    return sources;
}

juce::String ComputerAudio::start (const juce::String& source, juce::AudioIODeviceType* type, const juce::String& outputDevice)
{
    stop();

    if (source.isEmpty())
        return {};

    juce::String error;
    std::unique_ptr<Backend> created;

    if (source == systemSource)
    {
        created = createSystemBackend (muteOnSpeakers.load(), error);
    }
    else if (source.startsWith ("device:"))
    {
        const auto name = source.fromFirstOccurrenceOf ("device:", false, false);

        // MicFX playing into the device it listens to would feed its own output back in.
        if (name == outputDevice)
            return "MicFX plays to " + name + ", so it can't also listen to it (it would feed back). "
                   "Pick another output for MicFX, or route the computer's sound to a different device.";

        created = createDeviceBackend (name, type, error);
    }
    else
    {
        error = "Unknown source.";
    }

    if (created == nullptr)
        return error.isNotEmpty() ? error : juce::String ("Couldn't open the computer audio.");

    return start (std::move (created), source);
}

juce::String ComputerAudio::start (std::unique_ptr<Backend> created, const juce::String& source)
{
    stop();

    {
        const juce::SpinLock::ScopedLockType producer (producerLock);
        const juce::SpinLock::ScopedLockType consumer (consumerLock);
        ring.prepare (created->getSampleRate(), sampleRate);
        backend = std::move (created);
        currentSource = source;
        active = true;
    }

    if (auto startError = backend->startCapture(); startError.isNotEmpty())
    {
        stop();
        return startError;
    }

    return {};
}

void ComputerAudio::stop()
{
    {
        const juce::SpinLock::ScopedLockType producer (producerLock);
        const juce::SpinLock::ScopedLockType consumer (consumerLock);
        active = false;
    }

    // Stops the capture thread (pushes after `active` went off are ignored).
    backend.reset();
    currentSource = {};
}

std::unique_ptr<ComputerAudio::Backend> ComputerAudio::createDeviceBackend (const juce::String& name, juce::AudioIODeviceType* type,
                                                                           juce::String& error)
{
    if (type == nullptr)
    {
        error = "No audio driver to open " + name + " with.";
        return nullptr;
    }

    std::unique_ptr<juce::AudioIODevice> device (type->createDevice ({}, name));
    if (device == nullptr)
    {
        error = "Couldn't find " + name + ".";
        return nullptr;
    }

    const auto inputs = device->getInputChannelNames().size();
    if (inputs <= 0)
    {
        error = name + " has no inputs.";
        return nullptr;
    }

    juce::BigInteger channels;
    channels.setRange (0, juce::jmin (2, inputs), true);

    // The interface's rate when the device has it (no conversion), else the closest.
    const auto rates = device->getAvailableSampleRates();
    auto rate = rates.isEmpty() ? sampleRate : rates[0];
    for (const auto candidate : rates)
        if (std::abs (candidate - sampleRate) < std::abs (rate - sampleRate))
            rate = candidate;

    if (auto openError = device->open (channels, {}, rate, device->getDefaultBufferSize()); openError.isNotEmpty())
    {
        error = "Couldn't open " + name + ": " + openError;
        return nullptr;
    }

    return std::make_unique<DeviceBackend> (*this, std::move (device));
}

void ComputerAudio::push (const float* left, const float* right, int numFrames) noexcept
{
    const juce::SpinLock::ScopedTryLockType producer (producerLock);
    if (producer.isLocked() && active.load (std::memory_order_relaxed))
        ring.push (left, right, numFrames);
}

bool ComputerAudio::read (float* const* monitor, float* const* record, int numSamples) noexcept
{
    if (! active.load (std::memory_order_relaxed) || numSamples > maxBlock)
        return false;

    const juce::SpinLock::ScopedTryLockType consumer (consumerLock);
    if (! consumer.isLocked() || ! active.load (std::memory_order_relaxed))
        return false;

    ring.pull (monitor[0], monitor[1], numSamples);

    gain.setTargetValue (juce::Decibels::decibelsToGain (gainDb.load (std::memory_order_relaxed), -60.0f));
    auto blockPeak = 0.0f;
    const auto size = history.getNumSamples();
    const auto delay = juce::jlimit (0, size - 1, recordDelay.load (std::memory_order_relaxed));
    auto* historyLeft = history.getWritePointer (0);
    auto* historyRight = history.getWritePointer (1);

    for (int i = 0; i < numSamples; ++i)
    {
        const auto g = gain.getNextValue();
        const auto l = monitor[0][i] * g;
        const auto r = monitor[1][i] * g;
        monitor[0][i] = l;
        monitor[1][i] = r;
        blockPeak = juce::jmax (blockPeak, std::abs (l), std::abs (r));

        historyLeft[historyWrite] = l;
        historyRight[historyWrite] = r;
        auto readIndex = historyWrite - delay;
        if (readIndex < 0)
            readIndex += size;
        record[0][i] = historyLeft[readIndex];
        record[1][i] = historyRight[readIndex];

        if (++historyWrite == size)
            historyWrite = 0;
    }

    if (blockPeak > peak.load (std::memory_order_relaxed))
        peak.store (blockPeak, std::memory_order_relaxed);

    return true;
}

#if ! JUCE_MAC
bool ComputerAudio::isSystemCaptureAvailable()
{
    return false;
}

std::unique_ptr<ComputerAudio::Backend> ComputerAudio::createSystemBackend (bool, juce::String& error)
{
    error = "Capturing every app's sound needs macOS 14.2 or later. Route the computer's sound to a loopback "
            "device (Stereo Mix on Windows, a monitor source on Linux) and pick that instead.";
    return nullptr;
}
#endif
