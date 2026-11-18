#pragma once

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_processors/juce_audio_processors.h>

#include <array>

// Live mode's MIDI instruments: a keyboard and a drum kit (or two of anything) playing the
// MicFX instruments (Grand, Nova, Kit, ...) next to the voice, heard and recorded with it.
//
// Each slot has an instrument, a MIDI input (one device, or "any": every device no other
// slot has picked) and a channel (0 = any). The standalone app hands each MIDI message
// over with the device it came from (handleMidi, any thread); in a plug-in, the block's
// MIDI goes to the slots with input "any".
//
// The message thread swaps instruments (prepared before they go in); the audio thread
// renders them and only try-locks, skipping a slot for a block rather than wait.
class LiveInstruments
{
public:
    static constexpr int numSlots = 2;
    static constexpr const char* anyInput = "any";

    struct Settings
    {
        bool on = false;
        juce::String key;            // instrument key ("grand", "kit")
        juce::String input = anyInput; // a MIDI device identifier, or anyInput
        int channel = 0;             // 1-16, 0 = any
        float levelDb = 0.0f;
    };

    LiveInstruments();
    ~LiveInstruments();

    // Audio prepare (message thread, audio stopped). Prepares the instruments again too.
    void prepare (double sampleRate, int maxBlockSize);

    // Message thread. Sets a slot; a new key makes a new instrument (on its first preset,
    // or `state` when given: a saved sound).
    void setSettings (int slot, const Settings&, const juce::MemoryBlock& state = {});
    Settings getSettings (int slot) const;
    // Message thread. The slot's instrument (null when it has none). Editors keep it alive
    // with the shared pointer; it stays valid while shown even if the slot changes.
    std::shared_ptr<juce::AudioProcessor> getInstrument (int slot) const;
    juce::MemoryBlock getState (int slot) const;
    // Message thread. Loads a factory preset by name.
    bool loadPreset (int slot, const juce::String& name);
    juce::String getPresetName (int slot) const;

    // Any thread (the MIDI input's). `source`: the device's identifier.
    void handleMidi (const juce::String& source, const juce::MidiMessage&);
    // Whether the app hands MIDI over through handleMidi (then the block's MIDI is left alone).
    void setUsesDeviceMidi (bool uses) noexcept { usesDeviceMidi = uses; }

    // Audio thread. Renders every slot that's on into `out` (stereo, cleared first; numSamples
    // at most the prepared block size), with `blockMidi` (the plug-in's MIDI; may be null).
    // `filter` takes out what mustn't play (MIDI learn's mapped notes). False when nothing played.
    bool render (juce::AudioBuffer<float>& out, int numSamples, const juce::MidiBuffer* blockMidi,
                 const std::function<void (juce::MidiBuffer&)>& filter) noexcept;

    // The peak of each slot since last asked, and whether notes came in. Any thread.
    float getPeak (int slot) noexcept   { return slots[(size_t) slot].peak.exchange (0.0f); }
    bool takeActivity (int slot) noexcept { return slots[(size_t) slot].activity.exchange (false); }

    // Whether a slot's channel setting lets a message on `channel` (1-16) through.
    static bool matches (const Settings&, int channel) noexcept;

private:
    struct Slot
    {
        Settings settings;
        juce::SpinLock settingsLock; // input and channel are read by the MIDI thread
        juce::SpinLock audioLock;    // instrument swaps
        std::shared_ptr<juce::AudioProcessor> instrument;
        juce::MidiMessageCollector collector;
        juce::MidiBuffer midi;
        juce::AudioBuffer<float> buffer;
        juce::SmoothedValue<float> gain;
        std::atomic<bool> on { false };
        std::atomic<float> gainDb { 0.0f };
        std::atomic<float> peak { 0.0f };
        std::atomic<bool> activity { false };
        std::atomic<int> channel { 0 };
        bool wasOn = false; // audio thread: a slot coming on starts with every note off
    };

    std::shared_ptr<juce::AudioProcessor> createInstrument (const juce::String& key, const juce::MemoryBlock& state) const;

    std::array<Slot, numSlots> slots;
    double sampleRate = 48000.0;
    int maxBlock = 4096;
    std::atomic<bool> usesDeviceMidi { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LiveInstruments)
};
