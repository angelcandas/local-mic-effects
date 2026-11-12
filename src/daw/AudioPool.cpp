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

    if (settings.linked.size() > 1)
        return getLinked (file, settings);

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

    if (settings.linked.size() > 1)
    {
        std::vector<Audio> sources;
        std::vector<const juce::AudioBuffer<float>*> pointers;
        for (const auto& path : settings.linked)
        {
            sources.push_back (loadNow (juce::File (path)));
            pointers.push_back (sources.back().get());
        }

        auto parts = warp::renderLinked (pointers, sampleRate, settings);
        for (int i = 0; i < settings.linked.size(); ++i)
            if (sources[(size_t) i] != nullptr)
                installWarp (getWarpKey (juce::File (settings.linked[i]), settings), std::move (parts[(size_t) i]));

        const auto it = warps.find (key);
        return it != warps.end() ? it->second : Audio();
    }

    const auto source = loadNow (file);
    if (source == nullptr)
        return {};

    auto audio = std::make_shared<const juce::AudioBuffer<float>> (warp::render (*source, sampleRate, settings));
    warps[key] = audio;
    warpOrder.push_back (key);
    return audio;
}

void AudioPool::installWarp (const juce::String& key, juce::AudioBuffer<float>&& audio)
{
    if (warps.count (key) == 0)
        warpOrder.push_back (key);
    warps[key] = std::make_shared<const juce::AudioBuffer<float>> (std::move (audio));
}

AudioPool::Audio AudioPool::getLinked (const juce::File& file, const warp::Settings& settings)
{
    // Every file decoded first; each decode's onLoaded brings us back here.
    std::vector<Audio> sources;
    auto ready = true;
    for (const auto& path : settings.linked)
    {
        sources.push_back (get (juce::File (path)));
        ready = ready && (sources.back() != nullptr || errors.count (path) > 0);
    }

    const auto key = getWarpKey (file, settings);
    if (! ready || warpsPending.count (key) > 0)
        return {};

    // One render makes every file's copy: all their keys are pending until it lands.
    juce::StringArray keys;
    for (const auto& path : settings.linked)
    {
        keys.add (getWarpKey (juce::File (path), settings));
        warpsPending.insert (keys.strings.getLast());
    }

    const auto rate = sampleRate;
    loader.addJob ([this, sources, settings, keys, rate, weak = std::weak_ptr<bool> (alive)]
    {
        std::vector<const juce::AudioBuffer<float>*> pointers;
        for (const auto& source : sources)
            pointers.push_back (source.get());

        auto parts = std::make_shared<std::vector<juce::AudioBuffer<float>>> (warp::renderLinked (pointers, rate, settings, [weak]
        {
            auto flag = weak.lock();
            return flag != nullptr && *flag;
        }));

        juce::MessageManager::callAsync ([this, keys, rate, parts, sources, weak]
        {
            if (auto flag = weak.lock(); flag == nullptr || ! *flag)
                return;

            for (const auto& k : keys)
                warpsPending.erase (k);
            if (! juce::exactlyEqual (rate, sampleRate))
                return;

            for (int i = 0; i < keys.size() && i < (int) parts->size(); ++i)
                if (sources[(size_t) i] != nullptr)
                    installWarp (keys[i], std::move ((*parts)[(size_t) i]));
            if (onLoaded)
                onLoaded();
        });
    });

    return {};
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
