#include "Multitrack.h"

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
            startLoad (track->file, track->isTake, (juce::int64) ((double) track->start * ratio), track->id);
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

    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));

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

//==============================================================================
void Multitrack::install (std::shared_ptr<Track> track)
{
    const auto it = std::lower_bound (tracks.begin(), tracks.end(), track->id,
                                      [] (const auto& t, int id) { return t->id < id; });
    tracks.insert (it, std::move (track));
    publish();
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
        end = juce::jmax (end, track->end());
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

        const auto from = juce::jmax (blockStart, track->start);
        const auto to   = juce::jmin (blockEnd, track->end());

        if (from >= to)
            continue;

        const auto gain = track->gain.load();
        const auto sourceChannels = track->audio.getNumChannels();

        for (int ch = 0; ch < numChannels; ++ch)
            buffer.addFrom (ch, offset + (int) (from - blockStart), track->audio,
                            juce::jmin (ch, sourceChannels - 1), (int) (from - track->start),
                            (int) (to - from), gain);
    }
}
