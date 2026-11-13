#include "Practice.h"
#include "PluginProcessor.h"

#include <juce_cryptography/juce_cryptography.h>

namespace coach = analysis::coach;

namespace
{
    juce::var object (std::initializer_list<std::pair<const char*, juce::var>> properties)
    {
        auto* result = new juce::DynamicObject();
        for (const auto& [name, value] : properties)
            result->setProperty (name, value);
        return juce::var (result);
    }

    double round3 (double value) { return std::round (value * 1000.0) / 1000.0; }
    double round2 (double value) { return std::round (value * 100.0) / 100.0; }

    bool writeWav (const juce::File& file, const juce::AudioBuffer<float>& audio, double sampleRate)
    {
        file.deleteFile();
        std::unique_ptr<juce::OutputStream> stream (file.createOutputStream());
        if (stream == nullptr)
            return false;

        juce::WavAudioFormat wav;
        auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions{}.withSampleRate (sampleRate)
                                                                                     .withNumChannels (audio.getNumChannels())
                                                                                     .withBitsPerSample (24));
        const auto ok = writer != nullptr && writer->writeFromAudioSampleBuffer (audio, 0, audio.getNumSamples());
        writer.reset();
        if (! ok)
            file.deleteFile();
        return ok;
    }

    juce::var describeNote (int index, const coach::NoteScore& s)
    {
        return object ({ { "i", index }, { "sung", s.sung }, { "score", round2 (s.score) }, { "cents", round2 (s.cents) },
                         { "inTune", round2 (s.inTune) }, { "timing", round3 (s.timing) }, { "onset", s.onsetFound },
                         { "coverage", round2 (s.coverage) } });
    }

    juce::var describePhrase (int index, const coach::PhraseScore& s)
    {
        return object ({ { "i", index }, { "scored", s.scored }, { "score", round2 (s.score) }, { "pitch", round2 (s.pitchScore) },
                         { "timingScore", round2 (s.timingScore) }, { "steady", round2 (s.steadyScore) },
                         { "coverage", round2 (s.coverage) }, { "lean", round2 (s.lean) }, { "timing", round3 (s.timing) },
                         { "notesSung", s.notesSung }, { "notesInTune", s.notesInTune } });
    }
}

//==============================================================================
struct Practice::Job
{
    std::atomic<int> phase { (int) Phase::reading };
    std::atomic<double> progress { 0.0 };
    std::atomic<bool> cancel { false }, finished { false };
    juce::File source;
    bool split = true;
    Prepared result; // written by the job before `finished`
};

Practice::Practice (PluginProcessor& p, juce::PropertiesFile& s, Emit e, std::function<juce::int64()> latency)
    : processor (p), settings (s), emit (std::move (e)), deviceLatency (std::move (latency))
{
    guideLevel = (float) settings.getDoubleValue ("practice.guideLevel", 0.35);

    // The editor closes and opens again (always in a plugin): pick the song up again when
    // its guide is still on the tracks.
    const juce::File folder (settings.getValue ("practice.folder"));
    const auto melody = folder.getChildFile (melodyFileName);
    const juce::File guide (settings.getValue ("practice.guide"));

    if (folder.isDirectory() && melody.existsAsFile())
        for (const auto& track : processor.getMultitrack().getTracks())
            if (track->file == guide)
            {
                setSong (coach::Song::fromString (melody.loadFileAsString()));
                songName = settings.getValue ("practice.name");
                source = juce::File (settings.getValue ("practice.source"));
                guideFile = guide;
                backingFile = juce::File (settings.getValue ("practice.backing"));
                break;
            }
}

Practice::~Practice()
{
    if (job != nullptr)
        job->cancel = true;
    processor.getPitchTap().setEnabled (false);
}

//==============================================================================
juce::File Practice::getCacheRoot()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
       #if JUCE_MAC
        .getChildFile ("Application Support")
       #endif
        .getChildFile ("MicFX").getChildFile ("Practice");
}

juce::File Practice::getCacheFolder (const juce::File& source, bool split, const juce::File& root)
{
    const auto hash = juce::SHA256 (source).toHexString().substring (0, 20);
    return root.getChildFile (split ? hash : hash + "-voice");
}

Practice::Prepared Practice::prepare (const juce::File& source, const Options& options, const Progress& progress)
{
    Prepared result;
    auto phase = Phase::reading;
    const auto report = [&] (double p) { return progress == nullptr || progress (phase, p); };
    const auto stop = [&]
    {
        result.cancelled = true;
        return result;
    };

    if (! source.existsAsFile())
    {
        result.error = "Couldn't find " + source.getFullPathName() + ".";
        return result;
    }

    report (0.0);
    result.folder = getCacheFolder (source, options.split, options.cacheRoot);
    const auto melodyFile = result.folder.getChildFile (melodyFileName);
    result.guide = options.split ? result.folder.getChildFile (guideFileName) : source;
    result.backing = options.split ? result.folder.getChildFile (backingFileName) : juce::File();

    // Prepared before: read it back.
    if (melodyFile.existsAsFile() && result.guide.existsAsFile() && (! options.split || result.backing.existsAsFile()))
    {
        result.song = coach::Song::fromString (melodyFile.loadFileAsString());
        if (! result.song.isEmpty())
        {
            result.fromCache = true;
            juce::AudioFormatManager formats;
            formats.registerBasicFormats();
            if (std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (result.guide)); reader != nullptr && reader->sampleRate > 0.0)
                result.seconds = (double) reader->lengthInSamples / reader->sampleRate;
            return result;
        }
    }

    if (! report (0.0))
        return stop();

    if (! result.folder.createDirectory())
    {
        result.error = "Couldn't create " + result.folder.getFullPathName() + ".";
        return result;
    }

    juce::AudioBuffer<float> voice;
    double rate = stems::modelSampleRate;

    if (options.split)
    {
        auto loaded = options.loadSeparator ([&] (stems::LoadPhase p)
                                             {
                                                 phase = p == stems::LoadPhase::downloading ? Phase::downloading : Phase::loading;
                                                 report (0.0);
                                             },
                                             report);
        result.note = loaded.note;
        if (loaded.separator == nullptr)
        {
            if (loaded.cancelled)
                return stop();
            result.error = loaded.error.isNotEmpty() ? loaded.error : juce::String ("The stem splitter didn't load.");
            return result;
        }

        phase = Phase::reading;
        report (0.0);
        auto decoded = Multitrack::decode (source, stems::modelSampleRate);
        if (decoded.error.isNotEmpty())
        {
            result.error = decoded.error;
            return result;
        }

        auto& audio = decoded.audio;
        juce::AudioBuffer<float> stereo (2, audio.getNumSamples());
        for (int ch = 0; ch < 2; ++ch)
            stereo.copyFrom (ch, 0, audio, juce::jmin (ch, audio.getNumChannels() - 1), 0, audio.getNumSamples());

        phase = Phase::splitting;
        const auto parts = stems::separate (stereo, loaded.separator, report, stems::Settings { loaded.gpu ? 2 : 0 });
        if (parts.size() != (size_t) stems::numStems)
            return stop();

        auto backing = parts[(size_t) stems::Stem::drums];
        for (const auto stem : { stems::Stem::bass, stems::Stem::other })
            for (int ch = 0; ch < backing.getNumChannels(); ++ch)
                backing.addFrom (ch, 0, parts[(size_t) stem], ch, 0, backing.getNumSamples());

        voice = parts[(size_t) stems::Stem::vocals];
        if (! writeWav (result.backing, backing, rate) || ! writeWav (result.guide, voice, rate))
        {
            result.error = "Couldn't write the stems into " + result.folder.getFullPathName() + ".";
            return result;
        }
    }
    else
    {
        auto decoded = Multitrack::decode (source, rate);
        if (decoded.error.isNotEmpty())
        {
            result.error = decoded.error;
            return result;
        }
        voice = std::move (decoded.audio);
    }

    result.seconds = voice.getNumSamples() / rate;
    phase = Phase::listening;
    if (! report (0.0))
        return stop();

    result.song = coach::findMelody (voice, rate, [&] { return report (0.5); });
    if (! report (1.0))
        return stop();

    if (result.song.isEmpty())
    {
        result.error = options.split ? "Couldn't find a sung melody in this song." : "Couldn't find a sung melody in this file.";
        return result;
    }

    melodyFile.replaceWithText (result.song.toString());
    return result;
}

//==============================================================================
juce::var Practice::handle (const juce::String& action, const juce::var& options)
{
    if (action == "open")
    {
        const auto split = (bool) options.getProperty ("split", true);
        chooser = std::make_unique<juce::FileChooser> (split ? "Choose a song to practise" : "Choose a voice on its own (a guide vocal)",
                                                       juce::File::getSpecialLocation (juce::File::userMusicDirectory),
                                                       "*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3;*.m4a");
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [this, split] (const juce::FileChooser& c)
                              {
                                  if (c.getResult().existsAsFile())
                                      start (c.getResult(), split);
                              });
        return object ({ { "ok", true } });
    }

    if (action == "openFile")
    {
        const juce::File file (options["path"].toString());
        if (! juce::File::isAbsolutePath (options["path"].toString()) || ! file.existsAsFile())
            return object ({ { "ok", false }, { "error", "Couldn't find " + options["path"].toString() + "." } });
        start (file, (bool) options.getProperty ("split", true));
        return object ({ { "ok", true } });
    }

    if (action == "cancel")
    {
        if (job != nullptr)
            job->cancel = true;
    }
    else if (action == "view")
    {
        viewOpen = (bool) options["open"];
        processor.getPitchTap().setEnabled (viewOpen);
    }
    else if (action == "guide")
    {
        guideLevel = juce::jlimit (0.0f, 1.5f, (float) options.getProperty ("level", guideLevel));
        settings.setValue ("practice.guideLevel", guideLevel);
        applyGuideLevel();
    }
    else if (action == "reset")
    {
        restartFrom (0.0);
    }

    return describe();
}

juce::var Practice::describe() const
{
    return object ({ { "song", describeSong() }, { "job", describeJob() }, { "guideLevel", guideLevel }, { "open", viewOpen },
                     { "scores", describeScores (true) } });
}

juce::var Practice::describeSong() const
{
    if (song.isEmpty())
        return {};

    // Flat arrays keep the payload small: [start, end, pitch, phrase, ...] and [start, end, ...].
    juce::Array<juce::var> notes, phrases;
    for (const auto& n : song.notes)
    {
        notes.add (round3 (n.start));
        notes.add (round3 (n.end));
        notes.add (round2 (n.pitch));
        notes.add (n.phrase);
    }
    for (const auto& p : song.phrases)
    {
        phrases.add (round3 (p.start));
        phrases.add (round3 (p.end));
    }

    return object ({ { "name", songName }, { "file", source.getFullPathName() }, { "notes", notes }, { "phrases", phrases },
                     { "lowest", round2 (song.getLowest()) }, { "highest", round2 (song.getHighest()) },
                     { "hasBacking", backingFile != juce::File() } });
}

juce::var Practice::describeJob() const
{
    if (job == nullptr)
        return {};
    static constexpr const char* phases[] { "reading", "downloading", "loading", "splitting", "listening" };
    return object ({ { "phase", phases[juce::jlimit (0, 4, job->phase.load())] }, { "progress", round3 (job->progress.load()) },
                     { "name", job->source.getFileNameWithoutExtension() } });
}

juce::var Practice::describeScores (bool all) const
{
    juce::Array<juce::var> notes, phrases;
    if (all)
    {
        for (int i = 0; i < nextNote && i < (int) noteScores.size(); ++i)
            notes.add (describeNote (i, noteScores[(size_t) i]));
        for (int i = 0; i < nextPhrase && i < (int) phraseScores.size(); ++i)
            phrases.add (describePhrase (i, phraseScores[(size_t) i]));
    }
    else
    {
        for (auto i : changedNotes)
            notes.add (describeNote (i, noteScores[(size_t) i]));
        for (auto i : changedPhrases)
            phrases.add (describePhrase (i, phraseScores[(size_t) i]));
    }

    return object ({ { "reset", all || scoresReset }, { "from", round3 (lastSongTime) }, { "notes", notes }, { "phrases", phrases },
                     { "pass", round2 (getPassScore()) } });
}

//==============================================================================
void Practice::start (const juce::File& file, bool split)
{
    if (job != nullptr)
        job->cancel = true;

    auto newJob = std::make_shared<Job>();
    newJob->source = file;
    newJob->split = split;
    job = newJob;

    juce::Thread::launch ([newJob]
    {
        Options options;
        options.split = newJob->split;
        newJob->result = prepare (newJob->source, options, [newJob] (Phase phase, double p)
        {
            newJob->phase = (int) phase;
            newJob->progress = p;
            return ! newJob->cancel.load();
        });
        newJob->finished = true;
    });

    emit ("practice", object ({ { "state", "preparing" }, { "job", describeJob() } }));
}

void Practice::install (const Prepared& prepared)
{
    auto& multitrack = processor.getMultitrack();
    multitrack.pause();
    multitrack.seek (0);

    // The song replaces whatever was on the Live tracks (takes stay on disk).
    for (const auto& track : multitrack.getTracks())
        multitrack.removeTrack (track->id);

    if (prepared.backing != juce::File())
        multitrack.loadBackingTrack (prepared.backing);
    multitrack.loadBackingTrack (prepared.guide);

    source = job != nullptr ? job->source : prepared.guide;
    songName = source.getFileNameWithoutExtension();
    guideFile = prepared.guide;
    backingFile = prepared.backing;
    guidePending = true;
    setSong (prepared.song);

    settings.setValue ("practice.folder", prepared.folder.getFullPathName());
    settings.setValue ("practice.guide", guideFile.getFullPathName());
    settings.setValue ("practice.backing", backingFile.getFullPathName());
    settings.setValue ("practice.source", source.getFullPathName());
    settings.setValue ("practice.name", songName);
}

void Practice::applyGuideLevel()
{
    auto& multitrack = processor.getMultitrack();
    for (const auto& track : multitrack.getTracks())
        if (track->file == guideFile)
        {
            multitrack.setTrackGain (track->id, guideLevel);
            multitrack.setTrackMuted (track->id, guideLevel <= 0.0f);
            guidePending = false;
        }
}

void Practice::setSong (coach::Song newSong)
{
    song = std::move (newSong);
    sung.clear();
    noteScores.assign (song.notes.size(), {});
    phraseScores.assign (song.phrases.size(), {});
    nextNote = nextPhrase = 0;
    changedNotes.clear();
    changedPhrases.clear();
    scoresReset = true;
    lastSongTime = 0.0;
}

void Practice::restartFrom (double songTime)
{
    sung.erase (std::lower_bound (sung.begin(), sung.end(), songTime,
                                  [] (const coach::Frame& f, double t) { return f.time < t; }),
                sung.end());

    // Everything that ends after the jump is sung again.
    nextNote = 0;
    while (nextNote < (int) song.notes.size() && song.notes[(size_t) nextNote].end <= songTime)
        ++nextNote;
    nextPhrase = 0;
    while (nextPhrase < (int) song.phrases.size() && song.phrases[(size_t) nextPhrase].endNote <= nextNote
           && song.phrases[(size_t) nextPhrase].end <= songTime)
        ++nextPhrase;

    for (auto i = (size_t) nextNote; i < noteScores.size(); ++i)
        noteScores[i] = {};
    for (auto i = (size_t) nextPhrase; i < phraseScores.size(); ++i)
        phraseScores[i] = {};

    changedNotes.clear();
    changedPhrases.clear();
    scoresReset = true;
    lastSongTime = songTime;
}

void Practice::addFrames (const std::vector<coach::Frame>& frames, double songTime)
{
    for (const auto& frame : frames)
        if (sung.empty() || frame.time > sung.back().time)
            sung.push_back (frame);

    lastSongTime = songTime;

    // A note is scored once it has ended (with a moment for its last frames to arrive).
    constexpr double settle = 0.05;
    while (nextNote < (int) song.notes.size() && song.notes[(size_t) nextNote].end + settle <= songTime)
    {
        noteScores[(size_t) nextNote] = coach::scoreNote (song.notes[(size_t) nextNote], sung);
        changedNotes.push_back (nextNote++);
    }

    while (nextPhrase < (int) song.phrases.size() && song.phrases[(size_t) nextPhrase].endNote <= nextNote)
    {
        phraseScores[(size_t) nextPhrase] = coach::scorePhrase (song, nextPhrase, noteScores);
        changedPhrases.push_back (nextPhrase++);
    }
}

float Practice::getPassScore() const
{
    double total = 0.0, sum = 0.0;
    for (size_t i = 0; i < phraseScores.size(); ++i)
        if (phraseScores[i].scored)
        {
            const auto w = song.phrases[i].end - song.phrases[i].start;
            total += w;
            sum += w * phraseScores[i].score;
        }
    return total > 0.0 ? (float) (sum / total) : 0.0f;
}

//==============================================================================
void Practice::tick()
{
    if (job != nullptr && job->finished)
    {
        const auto finished = job;
        const auto& result = finished->result;

        if (result.cancelled)
            emit ("practice", object ({ { "state", "cancelled" } }));
        else if (result.error.isNotEmpty())
            emit ("practice", object ({ { "state", "error" }, { "error", result.error } }));
        else
        {
            install (result);
            emit ("practice", object ({ { "state", "ready" }, { "song", describeSong() }, { "fromCache", result.fromCache },
                                        { "note", result.note } }));
        }
        job = nullptr;
    }
    else if (job != nullptr)
    {
        emit ("practice", object ({ { "state", "preparing" }, { "job", describeJob() } }));
    }

    if (guidePending)
        applyGuideLevel();

    auto& tap = processor.getPitchTap();
    if (! tap.isEnabled())
        return;

    std::vector<PitchTap::Reading> readings;
    tap.read (readings);

    const auto& multitrack = processor.getMultitrack();
    const auto rate = juce::jmax (1.0, multitrack.getSampleRate());
    const auto latency = (double) (deviceLatency != nullptr ? deviceLatency() : 0) / rate;
    const auto position = (double) multitrack.getPosition() / rate;
    const auto playing = multitrack.isPlaying();
    const auto songTime = juce::jmax (0.0, position - latency);

    // A jump (a seek, a restart, a loop going round) starts the part after it over.
    if (lastPosition >= 0.0 && (position < lastPosition - 0.05 || position > lastPosition + 0.75))
        restartFrom (songTime);
    lastPosition = position;

    std::vector<coach::Frame> frames;
    juce::Array<juce::var> trail;
    auto lastMidi = 0.0f;
    for (const auto& r : readings)
    {
        const auto time = (double) r.position / rate - latency;
        lastMidi = r.levelDb > -60.0f ? r.midi : 0.0f;
        // (readings from before a jump, still on their way, are left out)
        if (playing && time >= lastSongTime - 0.5 && time <= songTime + 0.1)
        {
            frames.push_back ({ time, lastMidi });
            trail.add (round3 (time));
            trail.add (round2 (lastMidi));
        }
    }

    if (playing && ! song.isEmpty())
        addFrames (frames, songTime);
    else
        lastSongTime = songTime;

    if (scoresReset || ! changedNotes.empty() || ! changedPhrases.empty())
    {
        emit ("practiceScore", describeScores (false));
        changedNotes.clear();
        changedPhrases.clear();
        scoresReset = false;
    }

    emit ("practicePitch", object ({ { "time", round3 (songTime) }, { "playing", playing }, { "midi", round2 (lastMidi) },
                                     { "frames", trail } }));
}
