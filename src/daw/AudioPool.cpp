#include "AudioPool.h"

#include "Multitrack.h"

namespace daw
{
AudioPool::AudioPool() = default;

AudioPool::~AudioPool()
{
    *alive = false;
    loader.removeAllJobs (true, 5000);
}

void AudioPool::setSampleRate (double newRate)
{
    if (newRate <= 0.0 || juce::exactlyEqual (newRate, sampleRate))
        return;

    sampleRate = newRate;
    cache.clear();
    errors.clear();
    pending.clear(); // decodes still running are discarded when they finish (rate mismatch)
}

AudioPool::Audio AudioPool::get (const juce::File& file)
{
    const auto path = file.getFullPathName();

    if (const auto it = cache.find (path); it != cache.end())
        return it->second;

    if (errors.count (path) > 0 || pending.count (path) > 0)
        return {};

    pending.insert (path);
    const auto rate = sampleRate;

    loader.addJob ([this, file, path, rate, weak = std::weak_ptr<bool> (alive)]
    {
        auto decoded = std::make_shared<Multitrack::Decoded> (Multitrack::decode (file, rate));

        juce::MessageManager::callAsync ([this, path, rate, decoded, weak]
        {
            if (auto flag = weak.lock(); flag != nullptr && *flag)
                install (path, rate, std::move (decoded->audio), decoded->error);
        });
    });

    return {};
}

AudioPool::Audio AudioPool::loadNow (const juce::File& file)
{
    const auto path = file.getFullPathName();

    if (const auto it = cache.find (path); it != cache.end())
        return it->second;

    auto decoded = Multitrack::decode (file, sampleRate);
    install (path, sampleRate, std::move (decoded.audio), decoded.error);
    return cache.count (path) > 0 ? cache[path] : Audio();
}

juce::String AudioPool::getError (const juce::File& file) const
{
    const auto it = errors.find (file.getFullPathName());
    return it != errors.end() ? it->second : juce::String();
}

void AudioPool::install (const juce::String& path, double rate, juce::AudioBuffer<float>&& audio, const juce::String& error)
{
    pending.erase (path);

    if (! juce::exactlyEqual (rate, sampleRate))
        return; // decoded for an old rate; the next get() starts again

    if (error.isNotEmpty())
    {
        errors[path] = error;
        if (onError) onError (error);
        return;
    }

    cache[path] = std::make_shared<const juce::AudioBuffer<float>> (std::move (audio));

    if (onLoaded)
        onLoaded();
}
}
