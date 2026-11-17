#include "LiveInstruments.h"

#include "instruments/InstrumentProcessor.h"
#include "instruments/Registry.h"

LiveInstruments::LiveInstruments()
{
    for (auto& slot : slots)
    {
        slot.collector.reset (sampleRate);
        slot.buffer.setSize (2, maxBlock);
        slot.midi.ensureSize (2048);
    }

    // A keyboard on the first, a drum kit on the second (drum kits send on channel 10).
    slots[0].settings.key = "grand";
    slots[1].settings.key = "beat"; // the Kit is a sampler that starts empty
    slots[1].settings.channel = 10;
    slots[1].channel = 10;
}

LiveInstruments::~LiveInstruments() = default;

void LiveInstruments::prepare (double newSampleRate, int maxBlockSize)
{
    sampleRate = newSampleRate;
    maxBlock = juce::jmax (maxBlockSize, 4096);

    for (auto& slot : slots)
    {
        const juce::SpinLock::ScopedLockType lock (slot.audioLock);
        slot.collector.reset (sampleRate);
        slot.buffer.setSize (2, maxBlock);
        slot.gain.reset (sampleRate, 0.05);
        slot.gain.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (slot.gainDb.load(), -60.0f));

        if (slot.instrument != nullptr)
        {
            slot.instrument->setRateAndBufferSizeDetails (sampleRate, maxBlock);
            slot.instrument->prepareToPlay (sampleRate, maxBlock);
        }
    }
}

std::shared_ptr<juce::AudioProcessor> LiveInstruments::createInstrument (const juce::String& key, const juce::MemoryBlock& state) const
{
    std::shared_ptr<juce::AudioProcessor> instrument (micfx::inst::createInstrument (key).release());
    if (instrument == nullptr)
        return nullptr;

    if (state.getSize() > 0)
        instrument->setStateInformation (state.getData(), (int) state.getSize());
    else
        instrument->setCurrentProgram (0);

    instrument->setRateAndBufferSizeDetails (sampleRate, maxBlock);
    instrument->prepareToPlay (sampleRate, maxBlock);
    return instrument;
}

void LiveInstruments::setSettings (int index, const Settings& settings, const juce::MemoryBlock& state)
{
    if (! juce::isPositiveAndBelow (index, numSlots))
        return;

    auto& slot = slots[(size_t) index];
    Settings previous;
    {
        const juce::SpinLock::ScopedLockType lock (slot.settingsLock);
        previous = slot.settings;
        slot.settings = settings;
        slot.settings.channel = juce::jlimit (0, 16, settings.channel);
    }

    slot.channel = juce::jlimit (0, 16, settings.channel);
    slot.gainDb = juce::jlimit (-60.0f, 12.0f, settings.levelDb);

    // A new instrument (or a saved sound) is made and prepared here, then swapped in.
    if (settings.key != previous.key || slot.instrument == nullptr || state.getSize() > 0)
    {
        auto created = settings.on || slot.instrument != nullptr || state.getSize() > 0
                         ? createInstrument (settings.key, state) : nullptr;
        {
            const juce::SpinLock::ScopedLockType lock (slot.audioLock);
            std::swap (slot.instrument, created);
        }
        // The old one goes here, on the message thread.
    }

    slot.on = settings.on && slot.instrument != nullptr;
}

LiveInstruments::Settings LiveInstruments::getSettings (int index) const
{
    if (! juce::isPositiveAndBelow (index, numSlots))
        return {};

    auto& slot = slots[(size_t) index];
    const juce::SpinLock::ScopedLockType lock (slot.settingsLock);
    auto settings = slot.settings;
    settings.on = slot.on.load();
    return settings;
}

std::shared_ptr<juce::AudioProcessor> LiveInstruments::getInstrument (int index) const
{
    return juce::isPositiveAndBelow (index, numSlots) ? slots[(size_t) index].instrument : nullptr;
}

juce::MemoryBlock LiveInstruments::getState (int index) const
{
    juce::MemoryBlock state;
    if (auto instrument = getInstrument (index))
        instrument->getStateInformation (state);
    return state;
}

bool LiveInstruments::loadPreset (int index, const juce::String& name)
{
    // Parameter changes are safe while it plays (they're atomics the engine reads).
    if (auto* instrument = dynamic_cast<micfx::inst::InstrumentProcessor*> (getInstrument (index).get()))
        return instrument->loadPreset (name);
    return false;
}

juce::String LiveInstruments::getPresetName (int index) const
{
    if (auto* instrument = dynamic_cast<micfx::inst::InstrumentProcessor*> (getInstrument (index).get()))
        return instrument->getPresetName();
    return {};
}

bool LiveInstruments::matches (const Settings& settings, int channel) noexcept
{
    return settings.channel == 0 || settings.channel == channel;
}

void LiveInstruments::handleMidi (const juce::String& source, const juce::MidiMessage& message)
{
    const auto channel = message.getChannel();
    if (channel == 0)
        return; // clock, sysex: not for the instruments

    // A device a slot picked plays that slot only; the others play the slots on "any".
    std::array<bool, numSlots> owns {}, anyInput {};
    auto owned = false;
    for (size_t i = 0; i < slots.size(); ++i)
    {
        const juce::SpinLock::ScopedLockType lock (slots[i].settingsLock);
        owns[i] = slots[i].settings.input == source;
        anyInput[i] = slots[i].settings.input == LiveInstruments::anyInput;
        owned = owned || owns[i];
    }

    for (size_t i = 0; i < slots.size(); ++i)
    {
        auto& slot = slots[i];
        const auto channelOk = slot.channel.load() == 0 || slot.channel.load() == channel;
        if ((owned ? owns[i] : anyInput[i]) && channelOk && slot.on.load())
        {
            slot.collector.addMessageToQueue (message);
            if (message.isNoteOn())
                slot.activity = true;
        }
    }
}

bool LiveInstruments::render (juce::AudioBuffer<float>& out, int numSamples, const juce::MidiBuffer* blockMidi,
                              const std::function<void (juce::MidiBuffer&)>& filter) noexcept
{
    auto played = false;
    out.clear (0, numSamples);

    for (auto& slot : slots)
    {
        // Drained every block, so nothing old bursts out when a slot comes on.
        slot.midi.clear();
        slot.collector.removeNextBlockOfMessages (slot.midi, numSamples);

        if (! slot.on.load() || numSamples > slot.buffer.getNumSamples())
        {
            slot.wasOn = false;
            continue;
        }

        const juce::SpinLock::ScopedTryLockType lock (slot.audioLock);
        if (! lock.isLocked() || slot.instrument == nullptr)
            continue;

        if (! slot.wasOn)
        {
            // Notes held when it went off (or in a sound it just got) would hang on.
            for (int channel = 1; channel <= 16; ++channel)
                slot.midi.addEvent (juce::MidiMessage::allNotesOff (channel), 0);
            slot.wasOn = true;
        }

        if (blockMidi != nullptr && ! usesDeviceMidi.load())
        {
            const auto channel = slot.channel.load();
            for (const auto metadata : *blockMidi)
            {
                const auto message = metadata.getMessage();
                if (message.getChannel() != 0 && (channel == 0 || message.getChannel() == channel))
                {
                    slot.midi.addEvent (message, metadata.samplePosition);
                    if (message.isNoteOn())
                        slot.activity = true;
                }
            }
        }

        if (filter)
            filter (slot.midi);

        juce::AudioBuffer<float> view (slot.buffer.getArrayOfWritePointers(), 2, numSamples);
        view.clear();
        slot.instrument->processBlock (view, slot.midi);

        slot.gain.setTargetValue (juce::Decibels::decibelsToGain (slot.gainDb.load(), -60.0f));
        auto peak = 0.0f;
        auto* left = view.getWritePointer (0);
        auto* right = view.getWritePointer (1);
        auto* outLeft = out.getWritePointer (0);
        auto* outRight = out.getWritePointer (1);
        for (int i = 0; i < numSamples; ++i)
        {
            const auto g = slot.gain.getNextValue();
            outLeft[i] += left[i] * g;
            outRight[i] += right[i] * g;
            peak = juce::jmax (peak, std::abs (left[i] * g), std::abs (right[i] * g));
        }

        if (peak > slot.peak.load())
            slot.peak = peak;
        played = true;
    }

    return played;
}
