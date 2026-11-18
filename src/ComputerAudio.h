#pragma once

#include <juce_audio_devices/juce_audio_devices.h>

#include "dsp/DriftRing.h"

// Computer audio for Live mode: the sound of the other apps (a video, a song playing) mixed
// in with the voice, to hear and to record together.
//
// Where it comes from (a "source"):
//  - "system": every app's sound, straight from the computer (macOS 14.2 or later, through a
//    Core Audio process tap that leaves MicFX itself out, so its own output never loops back).
//    It can also mute those apps on the speakers so you hear them once, through MicFX, in
//    time with your voice.
//  - an input device: a loopback device the computer's sound is routed to (BlackHole or
//    Loopback on a Mac, Stereo Mix on Windows, a PipeWire/Pulse monitor on Linux).
//
// The capture runs on its own thread and clock; a DriftRing brings it to the interface's.
// start() and stop() run on the message thread; read() on the audio thread.
class ComputerAudio
{
public:
    static constexpr const char* systemSource = "system";
    static constexpr double maxRecordDelaySeconds = 1.0;

    // One way of getting the sound in. Pushes into the ring from its own thread.
    struct Backend
    {
        virtual ~Backend() = default;
        virtual double getSampleRate() const = 0;
        virtual juce::String getName() const = 0;
        // Begins calling push(). Returns an error message, or "".
        virtual juce::String startCapture() = 0;
    };

    struct SourceInfo
    {
        juce::String id, name;
    };

    ComputerAudio();
    ~ComputerAudio();

    // Audio prepare (message thread, audio stopped).
    void prepare (double sampleRate, int maxBlockSize);

    // Message thread. The sources there are: the system tap where the OS has it, then the
    // input devices of `type` (the app's audio driver), less `exclude` (the interface MicFX
    // already uses).
    static juce::Array<SourceInfo> listSources (juce::AudioIODeviceType* type, const juce::String& exclude);
    static bool isSystemCaptureAvailable();

    // Message thread. Starts capturing from `source` (an id from listSources). `type` opens
    // device sources; `outputDevice` is where MicFX plays, which a device source mustn't be
    // (it would feed back). Returns an error message, or "" on success.
    juce::String start (const juce::String& source, juce::AudioIODeviceType* type, const juce::String& outputDevice);
    void stop();
    // Message thread. Starts from a backend made elsewhere (the tests' fake one).
    juce::String start (std::unique_ptr<Backend> created, const juce::String& source);

    bool isOn() const noexcept                   { return backend != nullptr; }
    juce::String getSource() const               { return currentSource; }
    juce::String getSourceName() const           { return backend != nullptr ? backend->getName() : juce::String(); }

    // Any thread.
    void setGainDecibels (float db) noexcept     { gainDb.store (db); }
    float getGainDecibels() const noexcept       { return gainDb.load(); }
    void setMuteOnSpeakers (bool mute) noexcept  { muteOnSpeakers.store (mute); }
    bool getMuteOnSpeakers() const noexcept      { return muteOnSpeakers.load(); }
    // The takes get the computer audio delayed by the audio round trip (the voice comes back
    // that much later than the sound it was sung to), so the two line up in the file.
    void setRecordDelay (int samples) noexcept   { recordDelay.store (samples); }
    float getPeak() noexcept                     { return peak.exchange (0.0f); }
    bool isReceiving() const noexcept            { return ring.isRunning(); }

    // Audio thread. When it's on, writes `numSamples` of the computer audio (gain applied)
    // to `monitor` (2 channels: what you hear now) and `record` (2 channels: the same,
    // delayed for the takes), and returns true. False (nothing written) when it's off,
    // between sources or the block is larger than prepared.
    bool read (float* const* monitor, float* const* record, int numSamples) noexcept;

    // Called by the backends from their capture thread.
    void push (const float* left, const float* right, int numFrames) noexcept;

private:
    std::unique_ptr<Backend> createDeviceBackend (const juce::String& name, juce::AudioIODeviceType* type, juce::String& error);
    // Platform code (ComputerAudioMac.mm); null and an error where there's no system capture.
    std::unique_ptr<Backend> createSystemBackend (bool mute, juce::String& error);

    fx::DriftRing ring;
    std::unique_ptr<Backend> backend;
    juce::String currentSource;
    // The capture thread and the audio thread only try-lock these (and skip a block when they
    // can't); the message thread holds both while it (re)prepares the ring.
    juce::SpinLock producerLock, consumerLock;
    std::atomic<bool> active { false };

    double sampleRate = 48000.0;
    int maxBlock = 0;
    juce::AudioBuffer<float> history; // the last second, for the takes' delay
    int historyWrite = 0;
    juce::SmoothedValue<float> gain;

    std::atomic<float> gainDb { -6.0f };
    std::atomic<bool> muteOnSpeakers { true };
    std::atomic<int> recordDelay { 0 };
    std::atomic<float> peak { 0.0f };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ComputerAudio)
};
