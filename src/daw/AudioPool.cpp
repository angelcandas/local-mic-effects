#include "AudioPool.h"

#include "Multitrack.h"

#include <condition_variable>

namespace daw
{
struct AudioPool::Pieces
{
    using Audio = AudioPool::Audio;

    Audio find (const juce::String& key)
    {
        const std::lock_guard<std::mutex> guard (lock);
        const auto it = entries.find (key);
        if (it == entries.end())
            return {};
        it->second.lastUse = ++clock;
        return it->second.audio;
    }

    void add (const juce::String& key, Audio audio)
    {
        const std::lock_guard<std::mutex> guard (lock);
        if (const auto it = entries.find (key); it != entries.end())
            bytes -= sizeOf (*it->second.audio);
        bytes += sizeOf (*audio);
        entries[key] = { std::move (audio), ++clock };
        trim();
    }

    void clear()
    {
        const std::lock_guard<std::mutex> guard (lock);
        entries.clear();
        bytes = 0;
    }

    void setCapacity (size_t newCapacity)
    {
        const std::lock_guard<std::mutex> guard (lock);
        capacity = newCapacity;
        trim();
    }

    size_t getBytes()
    {
        const std::lock_guard<std::mutex> guard (lock);
        return bytes;
    }

    std::atomic<int> rendered { 0 };

private:
    struct Entry
    {
        Audio audio;
        juce::uint64 lastUse = 0;
    };

    static size_t sizeOf (const juce::AudioBuffer<float>& audio) { return (size_t) audio.getNumChannels() * (size_t) audio.getNumSamples() * sizeof (float); }

    void trim() // the least recently used first
    {
        while (bytes > capacity && ! entries.empty())
        {
            auto oldest = entries.begin();
            for (auto it = entries.begin(); it != entries.end(); ++it)
                if (it->second.lastUse < oldest->second.lastUse)
                    oldest = it;
            bytes -= sizeOf (*oldest->second.audio);
            entries.erase (oldest);
        }
    }

    std::mutex lock;
    std::map<juce::String, Entry> entries;
    size_t bytes = 0, capacity = (size_t) 512 << 20;
    juce::uint64 clock = 0;
};

namespace
{
    // Joins touching [from, to] stretches.
    std::vector<std::pair<double, double>> joined (std::vector<std::pair<double, double>> ranges)
    {
        std::sort (ranges.begin(), ranges.end());
        std::vector<std::pair<double, double>> result;
        for (const auto& r : ranges)
            if (! result.empty() && r.first <= result.back().second + 1.0e-6)
                result.back().second = juce::jmax (result.back().second, r.second);
            else
                result.push_back (r);
        return result;
    }
}

AudioPool::AudioPool() : pieces (std::make_shared<Pieces>()) {}

AudioPool::~AudioPool()
{
    *alive = false;
    warpPool.removeAllJobs (true, 5000);
    loader.removeAllJobs (true, 5000);
}

int AudioPool::getWarpThreads() noexcept
{
    return juce::jlimit (1, 4, juce::SystemStats::getNumCpus() - 1);
}

int AudioPool::getPiecesRendered() const noexcept { return pieces->rendered.load(); }
size_t AudioPool::getPieceBytes() const { return pieces->getBytes(); }
void AudioPool::setPieceCacheBytes (size_t bytes) { pieces->setCapacity (bytes); }

juce::String AudioPool::getPiecePrefix (const juce::String& files) const
{
    return files + "@" + juce::String (sampleRate, 0) + "#";
}

// The pieces the render misses (in parallel: `helpers` more threads join in on the warp
// pool), each cached as soon as it's done, then the whole copy from them. Empty when stopped.
juce::AudioBuffer<float> AudioPool::renderNow (const std::shared_ptr<const juce::AudioBuffer<float>>& source, double rate, const warp::Settings& settings,
                                               const juce::String& prefix, const std::function<bool()>& keepGoing, const CancelFlag& job,
                                               int helpers)
{
    const auto render = std::make_shared<const warp::Render> (source, rate, settings);
    if (! keepGoing())
        return {};
    if (! render->isSegmented())
        return render->renderWhole (keepGoing);

    const auto& list = render->getPieces();
    std::vector<Audio> parts (list.size());
    std::vector<size_t> missing;
    for (size_t i = 0; i < list.size(); ++i)
        if ((parts[i] = pieces->find (prefix + list[i].key)) == nullptr)
            missing.push_back (i);

    const auto publish = [job, render] (const std::vector<size_t>& left)
    {
        if (job == nullptr)
            return;
        std::vector<std::pair<double, double>> ranges;
        for (auto i : left)
            ranges.emplace_back (render->getPieces()[i].from, render->getPieces()[i].to);
        const std::lock_guard<std::mutex> guard (job->lock);
        job->planned = true;
        job->ranges = joined (std::move (ranges));
    };
    publish (missing);

    struct Work
    {
        std::vector<size_t> missing;
        std::vector<Audio> results;
        std::vector<bool> finished;
        std::atomic<size_t> next { 0 };
        size_t done = 0;
        std::mutex lock;
        std::condition_variable allDone;
    };
    auto work = std::make_shared<Work>();
    work->missing = missing;
    work->results.resize (missing.size());
    work->finished.assign (missing.size(), false);

    // Each thread takes the next piece nobody has taken until none are left, so the job
    // only ever waits for pieces being rendered right now (never for queued helpers).
    const auto worker = [work, render, cache = pieces, prefix, keepGoing, publish]
    {
        for (;;)
        {
            const auto n = work->next++;
            if (n >= work->missing.size())
                return;

            const auto index = work->missing[n];
            const auto key = prefix + render->getPieces()[index].key;
            Audio piece;
            if (keepGoing())
            {
                piece = cache->find (key); // another copy's job may have made it meanwhile
                if (piece == nullptr)
                    if (auto audio = render->renderPiece (index, keepGoing); audio.getNumSamples() > 0)
                    {
                        piece = std::make_shared<const juce::AudioBuffer<float>> (std::move (audio));
                        cache->add (key, piece);
                        ++cache->rendered;
                    }
            }

            std::vector<size_t> left;
            {
                const std::lock_guard<std::mutex> guard (work->lock);
                work->results[n] = piece;
                work->finished[n] = true;
                ++work->done;
                for (size_t i = 0; i < work->missing.size(); ++i)
                    if (! work->finished[i])
                        left.push_back (work->missing[i]);
            }
            publish (left);
            work->allDone.notify_all();
        }
    };

    for (int h = 0; h < juce::jmin (helpers, (int) missing.size() - 1); ++h)
        warpPool.addJob (worker);
    worker();

    {
        std::unique_lock<std::mutex> guard (work->lock);
        work->allDone.wait (guard, [&] { return work->done == work->missing.size(); });
    }

    for (size_t n = 0; n < missing.size(); ++n)
    {
        if (work->results[n] == nullptr)
            return {}; // stopped
        parts[missing[n]] = work->results[n];
    }

    return render->assemble (parts);
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
    for (auto& [key, flag] : warpJobs)
        flag->cancel.store (true);
    warpJobs.clear();
    pieces->clear();
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
    const auto cancel = std::make_shared<Job>();
    cancel->length = settings.map.toTarget (source->getNumSamples() / sampleRate);
    warpJobs[key] = cancel;
    const auto rate = sampleRate;
    const auto prefix = getPiecePrefix (file.getFullPathName());

    warpPool.addJob ([this, source, settings, key, rate, prefix, cancel, weak = std::weak_ptr<bool> (alive)]
    {
        const auto keepGoing = [weak, cancel]
        {
            auto flag = weak.lock();
            return flag != nullptr && *flag && ! cancel->cancel.load();
        };
        auto rendered = std::make_shared<juce::AudioBuffer<float>> (keepGoing() ? renderNow (source, rate, settings, prefix, keepGoing, cancel, getWarpThreads() - 1)
                                                                                : juce::AudioBuffer<float>());

        juce::MessageManager::callAsync ([this, key, rate, rendered, cancel, weak]
        {
            if (auto flag = weak.lock(); flag == nullptr || ! *flag)
                return;
            if (cancel->cancel.load())
                return; // dropped by releaseUnusedWarps (and maybe asked for again since)

            warpsPending.erase (key);
            warpJobs.erase (key);
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

        const auto together = std::make_shared<const juce::AudioBuffer<float>> (warp::combineLinked (pointers));
        auto parts = warp::splitLinked (renderNow (together, sampleRate, settings, getPiecePrefix (settings.linked.joinIntoString ("|")), [] { return true; }, {}, getWarpThreads()),
                                        pointers, sampleRate, settings);
        for (int i = 0; i < settings.linked.size(); ++i)
            if (sources[(size_t) i] != nullptr)
                installWarp (getWarpKey (juce::File (settings.linked[i]), settings), std::move (parts[(size_t) i]));

        const auto it = warps.find (key);
        return it != warps.end() ? it->second : Audio();
    }

    const auto source = loadNow (file);
    if (source == nullptr)
        return {};

    auto audio = std::make_shared<const juce::AudioBuffer<float>> (renderNow (source, sampleRate, settings, getPiecePrefix (file.getFullPathName()),
                                                                              [] { return true; }, {}, getWarpThreads()));
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
    const auto cancel = std::make_shared<Job>();
    for (const auto& source : sources)
        if (source != nullptr)
            cancel->length = juce::jmax (cancel->length, settings.map.toTarget (source->getNumSamples() / sampleRate));
    for (const auto& path : settings.linked)
    {
        keys.add (getWarpKey (juce::File (path), settings));
        warpsPending.insert (keys.strings.getLast());
        warpJobs[keys.strings.getLast()] = cancel;
    }

    const auto rate = sampleRate;
    const auto prefix = getPiecePrefix (settings.linked.joinIntoString ("|"));
    warpPool.addJob ([this, sources, settings, keys, rate, prefix, cancel, weak = std::weak_ptr<bool> (alive)]
    {
        const auto keepGoing = [weak, cancel]
        {
            auto flag = weak.lock();
            return flag != nullptr && *flag && ! cancel->cancel.load();
        };
        std::vector<const juce::AudioBuffer<float>*> pointers;
        for (const auto& source : sources)
            pointers.push_back (source.get());

        auto parts = std::make_shared<std::vector<juce::AudioBuffer<float>>>();
        if (keepGoing())
        {
            const auto together = std::make_shared<const juce::AudioBuffer<float>> (warp::combineLinked (pointers));
            const auto rendered = renderNow (together, rate, settings, prefix, keepGoing, cancel, getWarpThreads() - 1);
            if (rendered.getNumSamples() > 0)
                *parts = warp::splitLinked (rendered, pointers, rate, settings);
        }

        juce::MessageManager::callAsync ([this, keys, rate, parts, sources, cancel, weak]
        {
            if (auto flag = weak.lock(); flag == nullptr || ! *flag)
                return;
            if (cancel->cancel.load())
                return; // dropped by releaseUnusedWarps

            for (const auto& k : keys)
            {
                warpsPending.erase (k);
                warpJobs.erase (k);
            }
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
    // Renders nobody waits for any more: the same file is now wanted with other settings
    // (a transpose knob dragged on, a marker moved again). Only for files in use, so a
    // copy asked for elsewhere (a take lane's waveform) keeps rendering.
    std::set<juce::String> filesInUse;
    for (const auto& key : keysInUse)
        filesInUse.insert (key.upToLastOccurrenceOf ("#", false, false));

    std::set<CancelFlag> stale;
    for (const auto& [key, flag] : warpJobs)
        if (keysInUse.count (key) == 0 && filesInUse.count (key.upToLastOccurrenceOf ("#", false, false)) > 0)
            stale.insert (flag);

    // A linked render makes several files' copies: they all go together.
    for (auto it = warpJobs.begin(); it != warpJobs.end();)
    {
        if (stale.count (it->second) > 0)
        {
            it->second->cancel.store (true);
            warpsPending.erase (it->first);
            it = warpJobs.erase (it);
        }
        else
        {
            ++it;
        }
    }

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

std::vector<std::pair<double, double>> AudioPool::getRenderingRanges (const juce::File& file, const warp::Settings& settings) const
{
    if (settings.isNeutral())
        return {};

    const auto it = warpJobs.find (getWarpKey (file, settings));
    if (it == warpJobs.end())
        return {};

    const auto& job = *it->second;
    const std::lock_guard<std::mutex> guard (job.lock);
    if (! job.planned)
        return { { 0.0, juce::jmax (0.0, job.length) } };
    return job.ranges;
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
