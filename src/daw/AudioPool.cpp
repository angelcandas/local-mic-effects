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
    pending.clear();
    warps.clear();
    warpOrder.clear();
    warpsPending.clear(); // decodes still running are discarded when they finish (rate mismatch)
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

juce::String AudioPool::getWarpKey (const juce::File& file, const warp::Settings& settings)
{
    return file.getFullPathName() + "#" + settings.getKey();
}

AudioPool::Audio AudioPool::get (const juce::File& file, const warp::Settings& settings)
{
    if (settings.isNeutral())
        return get (file);

    const auto key = getWarpKey (file, settings);
    if (const auto it = warps.find (key); it != warps.end())
        return it->second;

    const auto source = get (file);
    if (source == nullptr || warpsPending.count (key) > 0)
        return {}; // the decode's onLoaded brings us back here

    warpsPending.insert (key);
    const auto rate = sampleRate;

    loader.addJob ([this, source, settings, key, rate, weak = std::weak_ptr<bool> (alive)]
    {
        auto rendered = std::make_shared<juce::AudioBuffer<float>> (warp::render (*source, rate, settings, [weak]
        {
            auto flag = weak.lock();
            return flag != nullptr && *flag;
        }));

        juce::MessageManager::callAsync ([this, key, rate, rendered, weak]
        {
            if (auto flag = weak.lock(); flag == nullptr || ! *flag)
                return;

            warpsPending.erase (key);
            if (! juce::exactlyEqual (rate, sampleRate))
                return;

            warps[key] = std::make_shared<const juce::AudioBuffer<float>> (std::move (*rendered));
            warpOrder.push_back (key);
            if (onLoaded)
                onLoaded();
        });
    });

    return {};
}

AudioPool::Audio AudioPool::loadNow (const juce::File& file, const warp::Settings& settings)
{
    if (settings.isNeutral())
        return loadNow (file);

    const auto key = getWarpKey (file, settings);
    if (const auto it = warps.find (key); it != warps.end())
        return it->second;

    const auto source = loadNow (file);
    if (source == nullptr)
        return {};

    auto audio = std::make_shared<const juce::AudioBuffer<float>> (warp::render (*source, sampleRate, settings));
    warps[key] = audio;
    warpOrder.push_back (key);
    return audio;
}

void AudioPool::releaseUnusedWarps (const std::set<juce::String>& keysInUse)
{
    std::vector<juce::String> unused;
    for (const auto& key : warpOrder)
        if (keysInUse.count (key) == 0)
            unused.push_back (key);

    for (size_t i = 0; i + sparedWarps < unused.size(); ++i)
    {
        warps.erase (unused[i]);
        warpOrder.erase (std::find (warpOrder.begin(), warpOrder.end(), unused[i]));
    }
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
