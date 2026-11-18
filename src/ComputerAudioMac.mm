// The Mac's own computer audio capture: a Core Audio process tap (macOS 14.2+) on every app
// but MicFX, read through a private aggregate device. Built with ARC.

#include "ComputerAudio.h"

#import <Foundation/Foundation.h>
#import <CoreAudio/CoreAudio.h>
#include <unistd.h>

// The tap API is in the macOS 14.2 SDK; older SDKs build without it.
#if defined (__MAC_14_2) && __MAC_OS_X_VERSION_MAX_ALLOWED >= __MAC_14_2
 #import <CoreAudio/AudioHardwareTapping.h>
 #import <CoreAudio/CATapDescription.h>
 #define MICFX_HAS_PROCESS_TAP 1
#else
 #define MICFX_HAS_PROCESS_TAP 0
#endif

#if MICFX_HAS_PROCESS_TAP
namespace
{
template <typename Value>
OSStatus getProperty (AudioObjectID object, AudioObjectPropertySelector selector, Value& value,
                      UInt32 qualifierSize = 0, const void* qualifier = nullptr)
{
    const AudioObjectPropertyAddress address { selector, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    UInt32 size = sizeof (Value);
    return AudioObjectGetPropertyData (object, &address, qualifierSize, qualifier, &size, &value);
}

juce::String describe (const char* what, OSStatus status)
{
    return juce::String (what) + " (error " + juce::String ((int) status) + ").";
}

class API_AVAILABLE (macos (14.2)) SystemTapBackend final : public ComputerAudio::Backend
{
public:
    explicit SystemTapBackend (ComputerAudio& o) : owner (o)
    {
        left.resize (chunk);
        right.resize (chunk);
    }

    ~SystemTapBackend() override
    {
        if (aggregate != kAudioObjectUnknown)
        {
            if (procID != nullptr)
            {
                AudioDeviceStop (aggregate, procID);
                AudioDeviceDestroyIOProcID (aggregate, procID);
            }

            AudioHardwareDestroyAggregateDevice (aggregate);
        }

        if (tap != kAudioObjectUnknown)
            AudioHardwareDestroyProcessTap (tap);
    }

    juce::String open (bool mute)
    {
        // MicFX itself is left out of the tap so its own output never comes back in.
        pid_t pid = getpid();
        AudioObjectID self = kAudioObjectUnknown;
        getProperty (kAudioObjectSystemObject, kAudioHardwarePropertyTranslatePIDToProcessObject, self, sizeof (pid), &pid);

        NSArray<NSNumber*>* excluded = self != kAudioObjectUnknown ? @[ @(self) ] : @[];
        CATapDescription* description = [[CATapDescription alloc] initStereoGlobalTapButExcludeProcesses: excluded];
        description.name = @"MicFX computer audio";
        // Muted: the apps go quiet on the speakers and you hear them once, through MicFX,
        // in time with your voice.
        description.muteBehavior = mute ? CATapMutedWhenTapped : CATapUnmuted;

        if (auto status = AudioHardwareCreateProcessTap (description, &tap); status != noErr)
        {
            tap = kAudioObjectUnknown;
            return describe ("Couldn't listen to the computer's sound. Allow MicFX in System Settings, Privacy & Security, "
                             "Screen & System Audio Recording", status);
        }

        CFStringRef tapUID = nullptr;
        if (auto status = getProperty (tap, kAudioTapPropertyUID, tapUID); status != noErr || tapUID == nullptr)
            return describe ("Couldn't read the tap", status);

        NSString* tapId = (__bridge_transfer NSString*) tapUID;

        AudioStreamBasicDescription format {};
        if (auto status = getProperty (tap, kAudioTapPropertyFormat, format); status != noErr)
            return describe ("Couldn't read the tap's format", status);

        if ((format.mFormatFlags & kAudioFormatFlagIsFloat) == 0 || format.mBitsPerChannel != 32)
            return "The computer's sound came in a format MicFX can't read.";

        rate = format.mSampleRate;

        // The tap runs on the clock of the computer's output device.
        AudioObjectID output = kAudioObjectUnknown;
        CFStringRef outputUID = nullptr;
        if (getProperty (kAudioObjectSystemObject, kAudioHardwarePropertyDefaultOutputDevice, output) != noErr
            || output == kAudioObjectUnknown
            || getProperty (output, kAudioDevicePropertyDeviceUID, outputUID) != noErr || outputUID == nullptr)
        {
            return "Couldn't find the computer's output device.";
        }

        NSString* outputId = (__bridge_transfer NSString*) outputUID;

        NSDictionary* settings = @{
            @kAudioAggregateDeviceNameKey: @"MicFX Computer Audio",
            @kAudioAggregateDeviceUIDKey: [[NSUUID UUID] UUIDString],
            @kAudioAggregateDeviceMainSubDeviceKey: outputId,
            @kAudioAggregateDeviceIsPrivateKey: @YES,
            @kAudioAggregateDeviceIsStackedKey: @NO,
            @kAudioAggregateDeviceTapAutoStartKey: @YES,
            @kAudioAggregateDeviceSubDeviceListKey: @[ @{ @kAudioSubDeviceUIDKey: outputId } ],
            @kAudioAggregateDeviceTapListKey: @[ @{ @kAudioSubTapDriftCompensationKey: @YES, @kAudioSubTapUIDKey: tapId } ],
        };

        if (auto status = AudioHardwareCreateAggregateDevice ((__bridge CFDictionaryRef) settings, &aggregate); status != noErr)
        {
            aggregate = kAudioObjectUnknown;
            return describe ("Couldn't set up the computer audio device", status);
        }

        Float64 aggregateRate = 0.0;
        if (getProperty (aggregate, kAudioDevicePropertyNominalSampleRate, aggregateRate) == noErr && aggregateRate > 0.0)
            rate = aggregateRate;

        return rate > 0.0 ? juce::String() : juce::String ("The computer's sound has no sample rate.");
    }

    double getSampleRate() const override   { return rate; }
    juce::String getName() const override   { return "Computer audio (all apps)"; }

    juce::String startCapture() override
    {
        auto* backend = this;
        auto status = AudioDeviceCreateIOProcIDWithBlock (&procID, aggregate, nullptr,
            ^(const AudioTimeStamp*, const AudioBufferList* input, const AudioTimeStamp*, AudioBufferList*, const AudioTimeStamp*)
            {
                backend->receive (input);
            });

        if (status != noErr)
        {
            procID = nullptr;
            return describe ("Couldn't start the computer audio", status);
        }

        if (status = AudioDeviceStart (aggregate, procID); status != noErr)
            return describe ("Couldn't start the computer audio", status);

        return {};
    }

private:
    // The capture thread. The tap's two channels come after any inputs of the output
    // device itself, so they are the last two; each buffer may be interleaved.
    void receive (const AudioBufferList* list) noexcept
    {
        if (list == nullptr || list->mNumberBuffers == 0)
            return;

        struct Channel { const float* data = nullptr; UInt32 stride = 1; UInt32 frames = 0; };
        Channel channels[2];
        int found = 0;

        for (int b = (int) list->mNumberBuffers - 1; b >= 0 && found < 2; --b)
        {
            const auto& buffer = list->mBuffers[b];
            const auto count = juce::jmax ((UInt32) 1, buffer.mNumberChannels);
            if (buffer.mData == nullptr)
                continue;

            const auto frames = buffer.mDataByteSize / (UInt32) (sizeof (float) * count);
            for (int c = (int) count - 1; c >= 0 && found < 2; --c)
                channels[found++] = { static_cast<const float*> (buffer.mData) + c, count, frames };
        }

        if (found == 0)
            return;

        // Found from the end: [0] is the right channel when there are two.
        const auto& l = found == 2 ? channels[1] : channels[0];
        const auto& r = channels[0];
        const auto frames = (int) juce::jmin (l.frames, r.frames);

        for (int start = 0; start < frames; start += chunk)
        {
            const auto length = juce::jmin (chunk, frames - start);
            for (int i = 0; i < length; ++i)
            {
                left[(size_t) i] = l.data[(size_t) (start + i) * l.stride];
                right[(size_t) i] = r.data[(size_t) (start + i) * r.stride];
            }
            owner.push (left.data(), right.data(), length);
        }
    }

    static constexpr int chunk = 4096;

    ComputerAudio& owner;
    AudioObjectID tap = kAudioObjectUnknown, aggregate = kAudioObjectUnknown;
    AudioDeviceIOProcID procID = nullptr;
    double rate = 0.0;
    std::vector<float> left, right;
};
} // namespace
#endif

bool ComputerAudio::isSystemCaptureAvailable()
{
   #if MICFX_HAS_PROCESS_TAP
    if (@available (macOS 14.2, *))
        return true;
   #endif
    return false;
}

std::unique_ptr<ComputerAudio::Backend> ComputerAudio::createSystemBackend (bool mute, juce::String& error)
{
   #if MICFX_HAS_PROCESS_TAP
    if (@available (macOS 14.2, *))
    {
        auto backend = std::make_unique<SystemTapBackend> (*this);
        error = backend->open (mute);
        if (error.isNotEmpty())
            return nullptr;

        return backend;
    }
   #endif

    juce::ignoreUnused (mute);
    error = "Capturing every app's sound needs macOS 14.2 or later. Route the computer's sound to a loopback device "
            "(such as BlackHole) and pick that instead.";
    return nullptr;
}
