#include "Practice.h"
#include "PluginProcessor.h"

#include <juce_cryptography/juce_cryptography.h>

#include "daw/Warp.h"

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

struct Practice::SectionsJob
{
    std::atomic<bool> cancel { false }, finished { false };
    std::vector<std::shared_ptr<const Multitrack::Track>> tracks; // the song's, kept alive while it listens
    double sampleRate = 48000.0;
    analysis::structure::SongMap map;
    juce::File folder; // the song's cache folder, for the result
};

struct Practice::LoopJob
{
    std::atomic<bool> cancel { false }, finished { false };
    int phrase = 0, lastPhrase = 0, section = -1;
    double speed = 1.0, semitones = 0.0, from = 0.0, to = 0.0;
    std::shared_ptr<const Multitrack::Track> backing, guide; // kept alive while it renders
    LoopAudio result;
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
    if (loopJob != nullptr)
        loopJob->cancel = true;
    if (sectionsJob != nullptr)
        sectionsJob->cancel = true;
    if (loop.active)
        endLoop();
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
        if (parts.size() < (size_t) stems::numStems)
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
    else if (action == "report")
    {
        return describeReport();
    }
    else if (action == "loop" && options.hasProperty ("section"))
    {
        startSectionLoop ((int) options.getProperty ("section", -1), (double) options.getProperty ("speed", 1.0),
                          (double) options.getProperty ("semitones", 0.0));
    }
    else if (action == "loop")
    {
        startLoop ((int) options.getProperty ("phrase", -1), (double) options.getProperty ("speed", 1.0),
                   (double) options.getProperty ("semitones", 0.0));
    }
    else if (action == "sections")
    {
        findSections();
    }
    else if (action == "unloop")
    {
        if (loopJob != nullptr)
            loopJob->cancel = true;
        loopJob = nullptr;
        if (loop.active)
        {
            const auto from = loop.from;
            endLoop();
            auto& multitrack = processor.getMultitrack();
            multitrack.seek ((juce::int64) (from * multitrack.getSampleRate()));
            restartFrom (from);
        }
        emit ("practice", object ({ { "state", "ready" }, { "song", describeSong() }, { "fromCache", true }, { "quiet", true } }));
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
                     { "hasBacking", backingFile != juce::File() },
                     { "sections", describeSections() },
                     { "loop", loop.active ? object ({ { "phrase", loop.phrase }, { "lastPhrase", loop.lastPhrase }, { "section", loop.section },
                                                       { "speed", loop.speed }, { "semitones", loop.semitones },
                                                       { "from", round3 (loop.from) }, { "to", round3 (loop.to) } })
                                           : juce::var() } });
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
    if (loopJob != nullptr)
        loopJob->cancel = true;
    loopJob = nullptr;
    endLoop (false);

    auto& multitrack = processor.getMultitrack();
    multitrack.pause();
    multitrack.seek (0);

    // The song replaces whatever was on the Live tracks (takes stay on disk).
    for (const auto& track : multitrack.getTracks())
        multitrack.removeTrack (track->id);

    if (prepared.backing != juce::File())
        multitrack.loadBackingTrack (prepared.backing);
    multitrack.loadBackingTrack (prepared.guide);

    if (sectionsJob != nullptr)
        sectionsJob->cancel = true;
    sectionsJob = nullptr;
    sections.clear();
    sectionsKnown = false;
    if (const auto cached = prepared.folder.getChildFile (sectionsFileName); prepared.folder != juce::File() && cached.existsAsFile())
    {
        sections = sectionsFromText (cached.loadFileAsString());
        sectionsKnown = true;
    }

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
            // (the song's own guide stays muted while a rendered loop plays instead)
            if (! (loop.active && loop.guideId >= 0))
                multitrack.setTrackMuted (track->id, guideLevel <= 0.0f);
            guidePending = false;
        }
        else if (loop.active && track->id == loop.guideId)
        {
            multitrack.setTrackGain (track->id, guideLevel);
            multitrack.setTrackMuted (track->id, guideLevel <= 0.0f);
        }
}

void Practice::setSong (coach::Song newSong)
{
    baseSong = newSong;
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

    if (sectionsJob != nullptr && sectionsJob->finished)
    {
        const auto finished = sectionsJob;
        sectionsJob = nullptr;
        if (! finished->cancel && ! finished->map.cancelled)
        {
            sections = finished->map.sections;
            sectionsKnown = true;
            if (finished->folder.isDirectory())
                finished->folder.getChildFile (sectionsFileName).replaceWithText (sectionsToText (sections));
            emit ("practice", object ({ { "state", "sections" }, { "sections", describeSections() } }));
        }
    }

    if (loopJob != nullptr && loopJob->finished)
    {
        const auto finished = loopJob;
        loopJob = nullptr;
        if (! finished->result.cancelled)
            finishLoop (*finished);
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

    // The song stopped after some singing: the report for the pass.
    if (wasPlaying && ! playing && std::any_of (phraseScores.begin(), phraseScores.end(), [] (const auto& p) { return p.scored; }))
        emit ("practiceReport", describeReport());
    wasPlaying = playing;

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

//==============================================================================
juce::var Practice::describeReport() const
{
    double total = 0.0, score = 0.0, pitch = 0.0, timing = 0.0, steady = 0.0, lean = 0.0, leanWeight = 0.0, seconds = 0.0;
    int scored = 0;
    juce::Array<juce::var> phrases;

    for (size_t i = 0; i < phraseScores.size(); ++i)
    {
        const auto& p = phraseScores[i];
        if (! p.scored)
            continue;
        ++scored;
        phrases.add (describePhrase ((int) i, p));
        const auto w = song.phrases[i].end - song.phrases[i].start;
        total += w;
        score += w * p.score;
        pitch += w * p.pitchScore;
        timing += w * p.timingScore;
        steady += w * p.steadyScore;
        seconds += w * p.timing;
        if (p.notesSung > 0)
        {
            leanWeight += w;
            lean += w * p.lean;
        }
    }

    // The weak phrases: the lowest under 75, worst first.
    std::vector<int> weak;
    for (int i = 0; i < (int) phraseScores.size(); ++i)
        if (phraseScores[(size_t) i].scored && phraseScores[(size_t) i].score < 75.0f)
            weak.push_back (i);
    std::sort (weak.begin(), weak.end(), [this] (int a, int b) { return phraseScores[(size_t) a].score < phraseScores[(size_t) b].score; });
    if (weak.size() > 3)
        weak.resize (3);
    juce::Array<juce::var> weakList;
    for (auto i : weak)
        weakList.add (i);

    const auto mean = [total] (double v) { return total > 0.0 ? round2 (v / total) : 0.0; };
    return object ({ { "score", mean (score) }, { "pitch", mean (pitch) }, { "timing", mean (timing) }, { "steady", mean (steady) },
                     { "lean", leanWeight > 0.0 ? round2 (lean / leanWeight) : 0.0 }, { "timingSeconds", total > 0.0 ? round3 (seconds / total) : 0.0 },
                     { "scored", scored }, { "total", (int) song.phrases.size() }, { "weak", weakList }, { "phrases", phrases },
                     { "looping", loop.active } });
}

coach::Song Practice::cutSong (const coach::Song& whole, int phrase, double offset, double speed, double semitones)
{
    coach::Song part;
    if (! juce::isPositiveAndBelow (phrase, (int) whole.phrases.size()))
        return part;

    const auto& p = whole.phrases[(size_t) phrase];
    const auto scale = 1.0 / juce::jlimit (0.25, 2.0, speed);
    for (int i = p.firstNote; i < p.endNote; ++i)
    {
        const auto& n = whole.notes[(size_t) i];
        part.notes.push_back ({ (n.start - offset) * scale, (n.end - offset) * scale, n.pitch + (float) semitones, 0 });
    }

    // One phrase, however long.
    coach::MelodySettings settings;
    settings.phraseGap = settings.maxPhraseSeconds = 1.0e9;
    coach::findPhrases (part, settings);
    return part;
}

coach::Song Practice::cutSong (const coach::Song& whole, int firstPhrase, int lastPhrase, double offset, double speed, double semitones)
{
    coach::Song part;
    if (! juce::isPositiveAndBelow (firstPhrase, (int) whole.phrases.size()) || lastPhrase < firstPhrase)
        return part;
    lastPhrase = juce::jmin (lastPhrase, (int) whole.phrases.size() - 1);

    const auto scale = 1.0 / juce::jlimit (0.25, 2.0, speed);
    for (int p = firstPhrase; p <= lastPhrase; ++p)
    {
        const auto& phrase = whole.phrases[(size_t) p];
        const auto index = (int) part.phrases.size();
        coach::Phrase cut = phrase;
        cut.start = (phrase.start - offset) * scale;
        cut.end = (phrase.end - offset) * scale;
        cut.firstNote = (int) part.notes.size();
        for (int i = phrase.firstNote; i < phrase.endNote; ++i)
        {
            const auto& n = whole.notes[(size_t) i];
            part.notes.push_back ({ (n.start - offset) * scale, (n.end - offset) * scale, n.pitch + (float) semitones, index });
        }
        cut.endNote = (int) part.notes.size();
        part.phrases.push_back (cut);
    }
    return part;
}

std::pair<int, int> Practice::getPhrasesIn (const coach::Song& whole, double from, double to)
{
    // A phrase belongs to the section it starts in (a pickup sung just before the bar line
    // still counts: a quarter of a second of slack).
    int first = -1, last = -1;
    for (int i = 0; i < (int) whole.phrases.size(); ++i)
        if (const auto start = whole.phrases[(size_t) i].start; start >= from - 0.25 && start < to - 0.25)
        {
            if (first < 0)
                first = i;
            last = i;
        }
    return { first, last };
}

juce::String Practice::sectionsToText (const std::vector<analysis::structure::Section>& list)
{
    juce::StringArray lines;
    for (const auto& section : list)
        lines.add (juce::String (section.start, 3) + "\t" + juce::String (section.end, 3) + "\t" + section.kind + "\t"
                   + juce::String (section.letter) + "\t" + juce::String (section.firstBar) + "\t" + juce::String (section.bars)
                   + "\t" + section.name);
    return lines.joinIntoString ("\n");
}

std::vector<analysis::structure::Section> Practice::sectionsFromText (const juce::String& text)
{
    std::vector<analysis::structure::Section> list;
    for (const auto& line : juce::StringArray::fromLines (text))
    {
        const auto fields = juce::StringArray::fromTokens (line, "\t", {});
        if (fields.size() < 7)
            continue;
        analysis::structure::Section section;
        section.start = fields[0].getDoubleValue();
        section.end = fields[1].getDoubleValue();
        section.kind = fields[2];
        section.letter = fields[3].getIntValue();
        section.firstBar = fields[4].getIntValue();
        section.bars = fields[5].getIntValue();
        section.name = fields[6];
        if (section.end > section.start)
            list.push_back (section);
    }
    return list;
}

juce::var Practice::describeSections() const
{
    if (! sectionsKnown)
        return sectionsJob != nullptr ? juce::var ("finding") : juce::var();

    juce::Array<juce::var> list;
    for (const auto& s : sections)
        list.add (object ({ { "name", s.name }, { "kind", s.kind }, { "letter", s.getLetterName() }, { "colour", s.getColour() },
                            { "start", round3 (s.start) }, { "end", round3 (s.end) }, { "bars", s.bars } }));
    return list;
}

void Practice::findSections()
{
    if (sectionsKnown)
    {
        emit ("practice", object ({ { "state", "sections" }, { "sections", describeSections() } }));
        return;
    }
    if (sectionsJob != nullptr)
        return;

    // The song's own tracks (the backing and the guide voice), summed.
    auto newJob = std::make_shared<SectionsJob>();
    const auto& multitrack = processor.getMultitrack();
    newJob->sampleRate = multitrack.getSampleRate();
    for (const auto& track : multitrack.getTracks())
        if ((track->file == guideFile || (backingFile != juce::File() && track->file == backingFile)) && track->audio.getNumSamples() > 0)
            newJob->tracks.push_back (track);

    if (newJob->tracks.empty())
    {
        emit ("practice", object ({ { "state", "sections" }, { "sections", juce::var() },
                                    { "error", baseSong.notes.empty() ? "Open a song first." : "The song is still loading. Try again in a moment." } }));
        return;
    }

    newJob->folder = juce::File (settings.getValue ("practice.folder"));
    sectionsJob = newJob;

    juce::Thread::launch ([newJob]
    {
        auto length = 0;
        for (const auto& track : newJob->tracks)
            length = juce::jmax (length, track->audio.getNumSamples());

        juce::AudioBuffer<float> mix (1, length);
        mix.clear();
        for (const auto& track : newJob->tracks)
            for (int ch = 0; ch < track->audio.getNumChannels(); ++ch)
                mix.addFrom (0, 0, track->audio, ch, 0, track->audio.getNumSamples(), 1.0f / (float) track->audio.getNumChannels());

        newJob->map = analysis::structure::analyse (mix, newJob->sampleRate, {}, [newJob] { return ! newJob->cancel.load(); });
        newJob->finished = true;
    });

    emit ("practice", object ({ { "state", "sections" }, { "sections", "finding" } }));
}

Practice::LoopAudio Practice::renderLoop (const juce::AudioBuffer<float>* backing, const juce::AudioBuffer<float>* guide, double rate,
                                          double from, double to, double speed, double semitones,
                                          const std::function<bool()>& shouldContinue)
{
    LoopAudio result;
    const auto render = [&] (const juce::AudioBuffer<float>* source, bool voice)
    {
        if (source == nullptr || source->getNumSamples() == 0)
            return juce::AudioBuffer<float>();

        const auto a = juce::jlimit (0, source->getNumSamples(), (int) (from * rate));
        const auto b = juce::jlimit (a, source->getNumSamples(), (int) (to * rate));
        juce::AudioBuffer<float> region (source->getNumChannels(), juce::jmax (1, b - a));
        region.clear();
        for (int ch = 0; ch < source->getNumChannels(); ++ch)
            region.copyFrom (ch, 0, *source, ch, a, b - a);

        if (std::abs (speed - 1.0) < 1.0e-4 && std::abs (semitones) < 1.0e-4)
            return region;

        const auto length = region.getNumSamples() / rate;
        daw::warp::Settings settings;
        if (std::abs (speed - 1.0) >= 1.0e-4)
            settings.map = daw::warp::Map (std::vector<daw::warp::Marker> { { 0.0, 0.0 }, { length, length / speed } });
        settings.semitones = semitones;
        settings.algorithm = voice ? daw::warp::Algorithm::solo : daw::warp::Algorithm::mix;
        settings.formant = voice;
        return daw::warp::render (region, rate, settings, shouldContinue);
    };

    result.backing = render (backing, false);
    if (shouldContinue != nullptr && ! shouldContinue())
        result.cancelled = true;
    result.guide = render (guide, true);
    if (shouldContinue != nullptr && ! shouldContinue())
        result.cancelled = true;
    return result;
}

void Practice::startLoop (int phrase, double speed, double semitones)
{
    if (! juce::isPositiveAndBelow (phrase, (int) baseSong.phrases.size()))
        return;

    const auto& p = baseSong.phrases[(size_t) phrase];
    const auto& multitrack = processor.getMultitrack();
    const auto length = (double) multitrack.getLength() / juce::jmax (1.0, multitrack.getSampleRate());
    const auto from = juce::jmax (0.0, p.start - loopLeadIn);
    const auto to = juce::jmin (length > 0.0 ? length : p.end + loopTail, p.end + loopTail);
    startRangeLoop (phrase, phrase, -1, from, to, speed, semitones);
}

void Practice::startSectionLoop (int index, double speed, double semitones)
{
    if (! juce::isPositiveAndBelow (index, (int) sections.size()))
        return;

    // The whole section, from a short lead-in to just past its end, with every phrase that
    // starts in it to sing (an instrumental section just loops).
    const auto& section = sections[(size_t) index];
    const auto& multitrack = processor.getMultitrack();
    const auto length = (double) multitrack.getLength() / juce::jmax (1.0, multitrack.getSampleRate());
    const auto from = juce::jmax (0.0, section.start - loopLeadIn);
    const auto to = juce::jmin (length > 0.0 ? length : section.end + loopTail, section.end + loopTail);
    const auto [first, last] = getPhrasesIn (baseSong, section.start, section.end);
    startRangeLoop (first, last, index, from, to, speed, semitones);
}

void Practice::startRangeLoop (int phrase, int lastPhrase, int section, double from, double to, double speed, double semitones)
{
    if (to <= from)
        return;

    if (loopJob != nullptr)
        loopJob->cancel = true;
    loopJob = nullptr;
    if (loop.active)
        endLoop();

    speed = juce::jlimit (0.5, 1.0, speed);
    semitones = juce::jlimit (-6.0, 6.0, std::round (semitones));

    auto& multitrack = processor.getMultitrack();

    // As it is: loop the song's own tracks.
    if (std::abs (speed - 1.0) < 1.0e-4 && std::abs (semitones) < 1.0e-4)
    {
        loop = {};
        loop.active = true;
        loop.phrase = phrase;
        loop.lastPhrase = lastPhrase;
        loop.section = section;
        loop.from = from;
        loop.to = to;
        const auto rate = multitrack.getSampleRate();
        multitrack.setLoop ((juce::int64) (from * rate), (juce::int64) (to * rate));
        multitrack.seek ((juce::int64) (from * rate));
        song = section >= 0 ? cutSong (baseSong, phrase, lastPhrase, 0.0, 1.0, 0.0) : cutSong (baseSong, phrase, 0.0, 1.0, 0.0);
        noteScores.assign (song.notes.size(), {});
        phraseScores.assign (song.phrases.size(), {});
        restartFrom (from);
        lastPosition = from;
        multitrack.play();
        emit ("practice", object ({ { "state", "loop" }, { "song", describeSong() } }));
        return;
    }

    auto newJob = std::make_shared<LoopJob>();
    newJob->phrase = phrase;
    newJob->lastPhrase = lastPhrase;
    newJob->section = section;
    newJob->speed = speed;
    newJob->semitones = semitones;
    newJob->from = from;
    newJob->to = to;
    for (const auto& track : multitrack.getTracks())
    {
        if (track->file == guideFile)
            newJob->guide = track;
        else if (backingFile != juce::File() && track->file == backingFile)
            newJob->backing = track;
    }
    loopJob = newJob;
    const auto rate = multitrack.getSampleRate();

    juce::Thread::launch ([newJob, rate]
    {
        newJob->result = renderLoop (newJob->backing != nullptr ? &newJob->backing->audio : nullptr,
                                     newJob->guide != nullptr ? &newJob->guide->audio : nullptr,
                                     rate, newJob->from, newJob->to, newJob->speed, newJob->semitones,
                                     [newJob] { return ! newJob->cancel.load(); });
        newJob->result.cancelled = newJob->result.cancelled || newJob->cancel.load();
        newJob->finished = true;
    });

    emit ("practice", object ({ { "state", "rendering" } }));
}

void Practice::finishLoop (LoopJob& done)
{
    auto& multitrack = processor.getMultitrack();
    multitrack.pause();

    loop = {};
    loop.active = true;
    loop.phrase = done.phrase;
    loop.lastPhrase = done.lastPhrase;
    loop.section = done.section;
    loop.speed = done.speed;
    loop.semitones = done.semitones;
    loop.from = done.from;
    loop.to = done.to;

    // The song's tracks rest while the rendered loop plays (each remembers how it was).
    for (const auto& track : multitrack.getTracks())
    {
        loop.muted.emplace_back (track->id, track->muted.load());
        multitrack.setTrackMuted (track->id, true);
    }

    auto length = (juce::int64) 0;
    if (done.result.backing.getNumSamples() > 0)
    {
        length = juce::jmax (length, (juce::int64) done.result.backing.getNumSamples());
        loop.backingId = multitrack.addAudio ("Loop backing", std::move (done.result.backing), 0);
    }
    if (done.result.guide.getNumSamples() > 0)
    {
        length = juce::jmax (length, (juce::int64) done.result.guide.getNumSamples());
        loop.guideId = multitrack.addAudio ("Loop guide", std::move (done.result.guide), 0);
    }
    applyGuideLevel();

    song = done.section >= 0 ? cutSong (baseSong, done.phrase, done.lastPhrase, done.from, done.speed, done.semitones)
                             : cutSong (baseSong, done.phrase, done.from, done.speed, done.semitones);
    noteScores.assign (song.notes.size(), {});
    phraseScores.assign (song.phrases.size(), {});
    restartFrom (0.0);

    multitrack.setLoop (0, length);
    multitrack.seek (0);
    lastPosition = 0.0;
    multitrack.play();
    emit ("practice", object ({ { "state", "loop" }, { "song", describeSong() } }));
}

void Practice::endLoop (bool restoreTracks)
{
    auto& multitrack = processor.getMultitrack();
    multitrack.clearLoop();

    if (restoreTracks && loop.active)
    {
        for (auto id : { loop.backingId, loop.guideId })
            if (id >= 0)
                multitrack.removeTrack (id);
        for (const auto& [id, muted] : loop.muted)
            multitrack.setTrackMuted (id, muted);
    }

    const auto wasActive = loop.active;
    loop = {};

    if (wasActive)
    {
        song = baseSong;
        noteScores.assign (song.notes.size(), {});
        phraseScores.assign (song.phrases.size(), {});
        nextNote = nextPhrase = 0;
        scoresReset = true;
    }
}
