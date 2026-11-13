#include "PluginInsert.h"

namespace daw
{
PluginInsert::PluginInsert (InstrumentPtr p, double rate, int block)
    : fx::Module (fx::ModuleType::plugin), plugin (std::move (p)), on (addSwitch (onId, true)),
      preparedRate (rate), preparedBlock (block)
{
    jassert (plugin != nullptr);
}

std::unique_ptr<PluginInsert> PluginInsert::create (const juce::ValueTree& insert, double sampleRate, int blockSize,
                                                    bool realtime, juce::String& error)
{
    auto processor = instruments::create (insert, sampleRate, blockSize, realtime, error);
    if (processor == nullptr)
        return {};

    return std::unique_ptr<PluginInsert> (new PluginInsert (std::move (processor), sampleRate, blockSize));
}

std::unique_ptr<PluginInsert> PluginInsert::share (const PluginInsert& running)
{
    return std::unique_ptr<PluginInsert> (new PluginInsert (running.plugin, running.preparedRate, running.preparedBlock));
}

void PluginInsert::prepareModule (const juce::dsp::ProcessSpec& spec)
{
    // Made for this rate and size already (the usual case); otherwise prepared again.
    if (! juce::approximatelyEqual (spec.sampleRate, preparedRate) || (int) spec.maximumBlockSize > preparedBlock)
    {
        plugin->releaseResources();
        plugin->setRateAndBufferSizeDetails (spec.sampleRate, (int) spec.maximumBlockSize);
        plugin->prepareToPlay (spec.sampleRate, (int) spec.maximumBlockSize);
        preparedRate = spec.sampleRate;
        preparedBlock = (int) spec.maximumBlockSize;
    }

    const auto channels = juce::jmax (2, plugin->getTotalNumInputChannels(), plugin->getTotalNumOutputChannels());
    buffer.setSize (channels, (int) spec.maximumBlockSize);
    midi.ensureSize (256);

    // Room for the plugin's delay plus a block (it may report a little more later; the
    // engine then builds the chain again, and this with it).
    delay.setSize (2, juce::jmax (1, plugin->getLatencySamples()) + (int) spec.maximumBlockSize + 1);
    delay.clear();
    delayWrite = 0;
}

void PluginInsert::resetModule()
{
    // The plugin keeps its own state: resetting a third-party plugin can take long (some
    // reload their engine), too long for the audio thread.
    delay.clear();
    delayWrite = 0;
}

void PluginInsert::render (const Context& context) noexcept
{
    auto& block = context.getOutputBlock();
    const auto numSamples = (int) block.getNumSamples();
    const auto numChannels = (int) block.getNumChannels();

    if (numSamples <= 0 || numChannels <= 0 || numSamples > buffer.getNumSamples())
        return;

    auto* left = block.getChannelPointer (0);
    auto* right = block.getChannelPointer ((size_t) juce::jmin (1, numChannels - 1));

    // The signal goes into the delay line every block, so going round the plugin starts
    // with what came before.
    const auto capacity = delay.getNumSamples();
    const auto latency = juce::jlimit (0, capacity - numSamples - 1, plugin->getLatencySamples());
    {
        auto write = delayWrite;
        for (int i = 0; i < numSamples; ++i)
        {
            delay.setSample (0, write, left[i]);
            delay.setSample (1, write, right[i]);
            write = (write + 1) % capacity;
        }
    }

    if (on.load (std::memory_order_relaxed) < 0.5f)
    {
        // Off: the signal from `latency` samples ago, as if it had gone through.
        auto read = (delayWrite - latency + capacity) % capacity;
        for (int i = 0; i < numSamples; ++i)
        {
            left[i] = delay.getSample (0, read);
            if (right != left)
                right[i] = delay.getSample (1, read);
            read = (read + 1) % capacity;
        }

        delayWrite = (delayWrite + numSamples) % capacity;
        return;
    }

    delayWrite = (delayWrite + numSamples) % capacity;

    // The plugin's channels: its main input (stereo, or the two sides summed for a mono
    // one), anything else (a side-chain input) silent.
    const auto inputs = plugin->getMainBusNumInputChannels();
    const auto outputs = plugin->getMainBusNumOutputChannels();
    juce::AudioBuffer<float> io (buffer.getArrayOfWritePointers(), buffer.getNumChannels(), numSamples);
    io.clear();

    if (inputs == 1)
    {
        io.copyFrom (0, 0, left, numSamples, 0.5f);
        io.addFrom (0, 0, right, numSamples, 0.5f);
    }
    else if (inputs >= 2)
    {
        io.copyFrom (0, 0, left, numSamples);
        io.copyFrom (1, 0, right, numSamples);
    }

    midi.clear();
    {
        const juce::ScopedLock lock (plugin->getCallbackLock());
        if (plugin->isSuspended())
            return; // (the signal goes on as it came in)
        plugin->processBlock (io, midi);
    }

    if (outputs == 1)
    {
        juce::FloatVectorOperations::copy (left, io.getReadPointer (0), numSamples);
        if (right != left)
            juce::FloatVectorOperations::copy (right, io.getReadPointer (0), numSamples);
    }
    else if (outputs >= 2)
    {
        juce::FloatVectorOperations::copy (left, io.getReadPointer (0), numSamples);
        if (right != left)
            juce::FloatVectorOperations::copy (right, io.getReadPointer (1), numSamples);
    }
}
}
