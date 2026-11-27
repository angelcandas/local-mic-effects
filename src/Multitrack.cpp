#include "Multitrack.h"
#include "AudioImport.h"

namespace
{
    constexpr double maxTrackMinutes = 30.0;
}

Multitrack::Multitrack() = default;

Multitrack::~Multitrack()
{
    *alive = false;
    loader.removeAllJobs (true, 5000);
}

//==============================================================================
void Multitrack::prepare (double newSampleRate)
{
    if (newSampleRate <= 0.0 || juce::exactlyEqual (newSampleRate, sampleRate))
        return;

    const auto reload = [this, newSampleRate]
    {
        const auto ratio = newSampleRate / sampleRate;
        sampleRate = newSampleRate;
        position = (juce::int64) ((double) position.load() * ratio);

        auto old = std::move (tracks);
        tracks.clear();
        publish();

        for (const auto& track : old)
        {
            pendingMix[track->id] = { track->gain.load(), track->muted.load() };
            startLoad (track->file, track->isTake, (juce::int64) ((double) track->start * ratio), track->id);
        }
    };

    if (auto* mm = juce::MessageManager::getInstanceWithoutCreating(); mm == nullptr || mm->isThisTheMessageThread())
        reload();
    else
        juce::MessageManager::callAsync ([reload, weak = std::weak_ptr<bool> (alive)]
        {
            if (auto flag = weak.lock(); flag && *flag)
                reload();
        });
}

//==============================================================================
Multitrack::Decoded Multitrack::decode (const juce::File& file, double targetRate)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();

    // An MP4, M4A or other file JUCE can't open here: decoded to a WAV first (see AudioImport.h).
    juce::String unreadable;
    const auto readable = micfx::audioimport::makeReadable (file, unreadable);
    if (readable == juce::File())
        return { {}, unreadable };

    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (readable));

    if (reader == nullptr)
        return { {}, "Couldn't open " + file.getFileName() + ". Try a WAV, AIFF, FLAC, Ogg or MP3 file." };

    if (reader->sampleRate <= 0.0 || reader->lengthInSamples <= 0)
        return { {}, file.getFileName() + " is empty." };

    if ((double) reader->lengthInSamples / reader->sampleRate > maxTrackMinutes * 60.0)
        return { {}, file.getFileName() + " is longer than " + juce::String ((int) maxTrackMinutes) + " minutes." };

    const auto channels = (int) juce::jlimit (1u, 2u, reader->numChannels);
    juce::AudioBuffer<float> source (channels, (int) reader->lengthInSamples);
    reader->read (&source, 0, source.getNumSamples(), 0, true, channels > 1);

    if (juce::approximatelyEqual (reader->sampleRate, targetRate))
        return { std::move (source), {} };

    // Resample to the device rate so playback is a straight copy on the audio thread.
    const auto speed = reader->sampleRate / targetRate;
    const auto outLength = (int) std::floor ((double) source.getNumSamples() / speed);
    juce::AudioBuffer<float> resampled (channels, outLength);

    for (int ch = 0; ch < channels; ++ch)
    {
        juce::LagrangeInterpolator interpolator;
        interpolator.process (speed, source.getReadPointer (ch), resampled.getWritePointer (ch), outLength,
                              source.getNumSamples(), 0);
    }

    return { std::move (resampled), {} };
}

std::shared_ptr<Multitrack::Track> Multitrack::makeTrack (const juce::File& file, bool isTake, juce::int64 startSample,
                                                          int id, juce::AudioBuffer<float>&& audio)
{
    auto track = std::make_shared<Track>();
    track->id = id;
    track->name = file.getFileNameWithoutExtension();
    track->file = file;
    track->isTake = isTake;
    track->start = startSample;
    track->audio = std::move (audio);

    const auto step = juce::jmax (1, (int) (peakSeconds * sampleRate));

    for (int i = 0; i < track->audio.getNumSamples(); i += step)
    {
        const auto count = juce::jmin (step, track->audio.getNumSamples() - i);
        track->peaks.add (track->audio.getMagnitude (i, count));
    }

    return track;
}

void Multitrack::startLoad (const juce::File& file, bool isTake, juce::int64 startSample, int id)
{
    ++numLoading;
    const auto rate = sampleRate;

    loader.addJob ([this, file, isTake, startSample, id, rate, weak = std::weak_ptr<bool> (alive)]
    {
        auto decoded = std::make_shared<Decoded> (decode (file, rate));
        auto chords = std::make_shared<std::vector<analysis::ChordSegment>>();

        // The song's chords, for the harmonizer (takes are only the voice).
        if (! isTake && decoded->error.isEmpty())
            *chords = analysis::detectChords (decoded->audio, rate);

        juce::MessageManager::callAsync ([this, file, isTake, startSample, id, rate, decoded, chords, weak]
        {
            if (auto flag = weak.lock(); flag == nullptr || ! *flag)
                return;

            --numLoading;

            if (decoded->error.isNotEmpty())
            {
                if (onError) onError (decoded->error);
                if (onTracksChanged) onTracksChanged();
                return;
            }

            if (! juce::exactlyEqual (rate, sampleRate))
            {
                // The device rate changed while decoding: load it again at the new rate.
                startLoad (file, isTake, (juce::int64) ((double) startSample * sampleRate / rate), id);
                return;
            }

            auto track = makeTrack (file, isTake, startSample, id, std::move (decoded->audio));
            track->chords = std::move (*chords);
            install (std::move (track));
        });
    });
}

void Multitrack::loadBackingTrack (const juce::File& file)
{
    startLoad (file, false, 0, nextId++);
}

void Multitrack::addTake (const juce::File& file, juce::int64 startSample)
{
    startLoad (file, true, startSample, nextId++);
}

void Multitrack::addTake (const juce::File& file, juce::int64 startSample, float gain, bool muted)
{
    const auto id = nextId++;
    pendingMix[id] = { gain, muted };
    startLoad (file, true, startSample, id);
}

int Multitrack::addAudio (const juce::String& name, juce::AudioBuffer<float>&& audio, juce::int64 startSample)
{
    const auto id = nextId++;
    auto track = makeTrack ({}, false, startSample, id, std::move (audio));
    track->name = name;
    install (std::move (track));
    return id;
}

int Multitrack::loadNow (const juce::File& file, bool isTake, juce::int64 startSample)
{
    auto decoded = decode (file, sampleRate);

    if (decoded.error.isNotEmpty())
        return -1;

    const auto id = nextId++;
    auto chords = isTake ? std::vector<analysis::ChordSegment>() : analysis::detectChords (decoded.audio, sampleRate);
    auto track = makeTrack (file, isTake, startSample, id, std::move (decoded.audio));
    track->chords = std::move (chords);
    install (std::move (track));
    return id;
}

int Multitrack::addFilling (const juce::String& name, int numChannels, int numSamples, juce::int64 startSample, bool muted,
                            std::shared_ptr<Fill>& fill)
{
    numChannels = juce::jlimit (1, 2, numChannels);
    numSamples = juce::jmax (1, numSamples);
    const auto id = nextId++;

    auto track = std::make_shared<Track>();
    track->id = id;
    track->name = name;
    track->start = startSample;
    track->audio.setSize (numChannels, numSamples);
    track->muted = muted;
    fill = std::make_shared<Fill> (numSamples);
    for (int ch = 0; ch < numChannels; ++ch)
    {
        // getWritePointer also marks the buffer as not clear, so mixing it adds what's written.
        auto* data = track->audio.getWritePointer (ch);
        juce::FloatVectorOperations::clear (data, numSamples);
        fill->channels.push_back (data);
    }
    track->fill = fill;
    install (std::move (track));
    return id;
}

void Multitrack::finishFill (int id)
{
    for (auto& track : tracks)
    {
        if (track->id != id || track->fill == nullptr)
            continue;

        track->fill->markReady (0, track->audio.getNumSamples());
        track->fill->complete = true;
        const auto step = juce::jmax (1, (int) (peakSeconds * sampleRate));
        juce::Array<float> peaks;
        for (int i = 0; i < track->audio.getNumSamples(); i += step)
            peaks.add (track->audio.getMagnitude (i, juce::jmin (step, track->audio.getNumSamples() - i)));
        track->peaks = std::move (peaks);
        if (onTracksChanged)
            onTracksChanged();
        return;
    }
}

Multitrack::Fill::Fill (int samples)
    : numSamples (juce::jmax (1, samples)),
      numBlocks ((numSamples + blockSize - 1) / blockSize),
      ready (new std::atomic<bool>[(size_t) numBlocks])
{
    for (int i = 0; i < numBlocks; ++i)
        ready[(size_t) i].store (false);
}

bool Multitrack::Fill::isReady (juce::int64 from, juce::int64 to) const noexcept
{
    if (complete.load (std::memory_order_acquire))
        return true;
    from = juce::jlimit ((juce::int64) 0, (juce::int64) numSamples, from);
    to = juce::jlimit (from, (juce::int64) numSamples, to);
    if (to <= from)
        return true;
    for (auto b = (int) (from / blockSize); b <= (int) ((to - 1) / blockSize); ++b)
        if (! isBlockReady (b))
            return false;
    return true;
}

void Multitrack::Fill::markReady (juce::int64 from, juce::int64 to) noexcept
{
    from = juce::jmax ((juce::int64) 0, from);
    to = juce::jmin ((juce::int64) numSamples, to);
    // Whole blocks only, except the last block of the track, which may be short.
    for (auto b = (int) ((from + blockSize - 1) / blockSize); b < numBlocks; ++b)
    {
        const auto end = juce::jmin ((juce::int64) numSamples, (juce::int64) (b + 1) * blockSize);
        if (end > to)
            break;
        markBlock (b);
    }
}

//==============================================================================
void Multitrack::install (std::shared_ptr<Track> track)
{
    if (const auto mix = pendingMix.find (track->id); mix != pendingMix.end())
    {
        track->gain = mix->second.first;
        track->muted = mix->second.second;
        pendingMix.erase (mix);
    }

    // The same take twice (the session and Practice both bringing it back) is kept once.
    if (track->isTake && track->file != juce::File())
        for (const auto& t : tracks)
            if (t->isTake && t->file == track->file && t->start == track->start)
                return;

    const auto it = std::lower_bound (tracks.begin(), tracks.end(), track->id,
                                      [] (const auto& t, int id) { return t->id < id; });
    tracks.insert (it, std::move (track));
    publish();
}

juce::ValueTree Multitrack::saveSession() const
{
    juce::ValueTree session ("SINGALONG");
    for (const auto& track : tracks)
    {
        if (! track->file.existsAsFile())
            continue; // rendered copies (Practice) have no file of their own

        juce::ValueTree row ("ROW");
        row.setProperty ("file", track->file.getFullPathName(), nullptr);
        row.setProperty ("take", track->isTake, nullptr);
        row.setProperty ("start", (double) track->start / sampleRate, nullptr);
        row.setProperty ("gain", track->gain.load(), nullptr);
        row.setProperty ("muted", track->muted.load(), nullptr);
        session.appendChild (row, nullptr);
    }
    return session;
}

void Multitrack::restoreSession (const juce::ValueTree& session)
{
    for (const auto& id : [this] { std::vector<int> ids; for (const auto& t : tracks) ids.push_back (t->id); return ids; }())
        removeTrack (id);

    for (const auto& row : session)
    {
        const auto path = row.getProperty ("file").toString();
        const juce::File file (juce::File::isAbsolutePath (path) ? path : juce::String());
        if (! file.existsAsFile())
            continue;

        const auto id = nextId++;
        pendingMix[id] = { (float) row.getProperty ("gain", 1.0f), (bool) row.getProperty ("muted", false) };
        startLoad (file, (bool) row.getProperty ("take", false),
                   (juce::int64) std::round ((double) row.getProperty ("start", 0.0) * sampleRate), id);
    }
}

void Multitrack::setSongTransform (double speed, double semitones)
{
    speed = juce::jlimit (0.5, 1.5, speed);
    semitones = juce::jlimit (-12.0, 12.0, semitones);
    songSpeed = speed;
    songSemitones = semitones;
    songPitch = std::pow (2.0, semitones / 12.0);
    publish();
    if (onTracksChanged)
        onTracksChanged();
}

void Multitrack::setTrackStretched (int id, bool stretched)
{
    for (auto& track : tracks)
        if (track->id == id)
        {
            if (stretched && track->stretcher == nullptr)
                track->stretcher = std::make_unique<SongStretcher>();
            if (track->stretched.load() != stretched)
            {
                track->stretched = stretched;
                publish();
            }
        }
}

void Multitrack::removeTrack (int id)
{
    const auto it = std::find_if (tracks.begin(), tracks.end(), [id] (const auto& t) { return t->id == id; });

    if (it == tracks.end())
        return;

    auto removed = *it; // freed after the audio thread stops seeing it
    tracks.erase (it);
    publish();
}

void Multitrack::setTrackGain (int id, float gain)
{
    for (auto& track : tracks)
        if (track->id == id)
            track->gain = juce::jlimit (0.0f, 4.0f, gain);
}

void Multitrack::setTrackMuted (int id, bool muted)
{
    for (auto& track : tracks)
        if (track->id == id)
            track->muted = muted;
}

void Multitrack::publish()
{
    std::vector<Track*> next;
    next.reserve (tracks.size());
    juce::int64 end = 0;

    for (auto& track : tracks)
    {
        next.push_back (track.get());
        end = juce::jmax (end, track->stretched.load() ? track->start + (juce::int64) ((double) track->audio.getNumSamples() / songSpeed.load()) : track->end());
    }

    {
        const juce::SpinLock::ScopedLockType lock (liveLock);
        std::swap (live, next);
    }

    length = end;

    if (onTracksChanged)
        onTracksChanged();
}

//==============================================================================
analysis::Chord Multitrack::getBackingChord (juce::int64 timelinePosition) noexcept
{
    const juce::SpinLock::ScopedTryLockType lock (liveLock);

    if (! lock.isLocked())
        return {};

    // The first backing track that covers the position.
    for (const auto* track : live)
        if (! track->isTake && ! track->chords.empty() && timelinePosition >= track->start && timelinePosition < track->end())
            return analysis::chordAt (track->chords, timelinePosition - track->start);

    return {};
}

juce::int64 Multitrack::process (juce::AudioBuffer<float>& buffer, int numChannels, int numSamples) noexcept
{
    const auto blockStart = position.load();

    if (! playing.load())
        return blockStart;

    auto blockEnd = blockStart + numSamples;
    const auto start = loopStart.load(), end = loopEnd.load();

    if (end > start && blockStart < end && blockEnd > end)
    {
        // The loop closes inside this block: play to its end, then on from its start.
        const auto first = (int) (end - blockStart);
        mix (buffer, numChannels, 0, blockStart, first);
        for (int done = first; done < numSamples;)
        {
            const auto count = (int) juce::jmin ((juce::int64) (numSamples - done), end - start);
            mix (buffer, numChannels, done, start, count);
            done += count;
            blockEnd = start + count;
        }
    }
    else
    {
        mix (buffer, numChannels, 0, blockStart, numSamples);
    }

    // A seek from the UI during this block wins over advancing.
    auto expected = blockStart;
    position.compare_exchange_strong (expected, blockEnd);
    return blockStart;
}

void Multitrack::mix (juce::AudioBuffer<float>& buffer, int numChannels, int offset, juce::int64 blockStart, int numSamples) noexcept
{
    const auto blockEnd = blockStart + numSamples;
    const juce::SpinLock::ScopedTryLockType lock (liveLock);

    if (! lock.isLocked())
        return;

    for (auto* track : live)
    {
        if (track->muted.load())
            continue;

        const auto speed = songSpeed.load(), pitch = songPitch.load();
        const auto liveShift = track->stretched.load() && track->stretcher != nullptr && track->start == 0
                       && (std::abs (speed - 1.0) > 1.0e-4 || std::abs (pitch - 1.0) > 1.0e-4);
        const auto from = juce::jmax (blockStart, track->start);
        const auto to   = juce::jmin (blockEnd, liveShift ? (juce::int64) ((double) track->audio.getNumSamples() / speed) : track->end());

        if (from >= to)
            continue;

        const auto gain = track->gain.load();

        if (liveShift)
        {
            // The song in another key or speed, made as it plays.
            float* out[2] = { stretchBuffer[0].data(), stretchBuffer[1].data() };
            for (auto a = from; a < to;)
            {
                const auto count = (int) juce::jmin ((juce::int64) stretchBuffer[0].size(), to - a);
                track->stretcher->pull (track->audio, speed, pitch, a, out, 2, count);
                for (int ch = 0; ch < numChannels; ++ch)
                    juce::FloatVectorOperations::addWithMultiply (buffer.getWritePointer (ch) + offset + (int) (a - blockStart), out[juce::jmin (ch, 1)], gain, count);
                a += count;
            }
            continue;
        }

        const auto sourceChannels = track->audio.getNumChannels();
        const auto add = [&] (juce::int64 a, juce::int64 b)
        {
            for (int ch = 0; ch < numChannels; ++ch)
                buffer.addFrom (ch, offset + (int) (a - blockStart), track->audio,
                                juce::jmin (ch, sourceChannels - 1), (int) (a - track->start),
                                (int) (b - a), gain);
        };

        if (const auto* fill = track->fill.get(); fill != nullptr && ! fill->complete.load (std::memory_order_acquire))
        {
            // Still being written: only the blocks that are ready.
            for (auto a = from; a < to;)
            {
                const auto block = (int) ((a - track->start) / Fill::blockSize);
                const auto b = juce::jmin (to, track->start + (juce::int64) (block + 1) * Fill::blockSize);
                if (fill->isBlockReady (block))
                    add (a, b);
                a = b;
            }
        }
        else
        {
            add (from, to);
        }
    }
}
