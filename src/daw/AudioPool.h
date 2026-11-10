#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_events/juce_events.h>

#include <map>
#include <memory>
#include <set>

namespace daw
{
// Audio files decoded at the session sample rate, shared by every clip that uses them.
//
// Message thread only. get() returns the audio if it's ready, or starts decoding it on a
// background thread and returns null; onLoaded fires (on the message thread) when it's
// ready. The buffers are immutable once published, so the audio thread can read them.
class AudioPool
{
public:
    using Audio = std::shared_ptr<const juce::AudioBuffer<float>>;

    AudioPool();
    ~AudioPool();

    // Drops everything decoded at the old rate; files decode again on their next get().
    void setSampleRate (double);
    double getSampleRate() const noexcept { return sampleRate; }

    Audio get (const juce::File&);
    Audio loadNow (const juce::File&); // synchronous, for tests and small files

    bool isLoading() const noexcept { return ! pending.empty(); }
    juce::String getError (const juce::File&) const;

    std::function<void()> onLoaded;
    std::function<void (const juce::String&)> onError;

private:
    void install (const juce::String& path, double rate, juce::AudioBuffer<float>&&, const juce::String& error);

    double sampleRate = 48000.0;
    std::map<juce::String, Audio> cache;
    std::map<juce::String, juce::String> errors;
    std::set<juce::String> pending;

    juce::ThreadPool loader { juce::ThreadPoolOptions{}.withThreadName ("MicFX audio pool").withNumberOfThreads (1) };
    std::shared_ptr<bool> alive = std::make_shared<bool> (true);

    JUCE_DECLARE_NON_COPYABLE (AudioPool)
};
}
