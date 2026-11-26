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

struct Practice::ReviewJob
{
    std::atomic<bool> cancel { false }, finished { false };
    std::shared_ptr<const Multitrack::Track> track; // kept alive while it listens
    double sampleRate = 48000.0;
    coach::Song song;       // the notes as they played when the take was recorded
    double start = 0.0, end = 0.0; // where the take sits in the song (seconds)
    std::vector<coach::Frame> frames;
    coach::Report report;
    bool ok = false;
};

struct Practice::ShiftLoad
{
    std::atomic<bool> cancel { false }, finished { false };
    double speed = 1.0, semitones = 0.0;
    juce::File backingFile, guideFile;
    juce::AudioBuffer<float> backing, guide;
    bool ok = false;
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
    visualSyncMs = settings.getDoubleValue ("practice.visualSyncMs", 0.0);

    // The editor closes and opens again (always in a plugin): pick the song up again when
    // its guide is still on the tracks.
    const juce::File folder (settings.getValue ("practice.folder"));
    const auto melody = folder.getChildFile (melodyFileName);
    const juce::File guide (settings.getValue ("practice.guide"));

    // (the tracks may not be back yet: a restart restores them in the background, so the
    // song is picked up from its folder, not from what's on the tracks)
    if (folder.isDirectory() && melody.existsAsFile() && guide.existsAsFile())
    {
        setSong (readMelody (folder, coach::Song::fromString (melody.loadFileAsString())));
        songName = settings.getValue ("practice.name");
        source = juce::File (settings.getValue ("practice.source"));
        guideFile = guide;
        backingFile = juce::File (settings.getValue ("practice.backing"));
        songFolder = folder;
        lyrics = analysis::lyrics::Lyrics::fromString (folder.getChildFile (lyricsFileName).loadFileAsString());
        annotations = labelsFromText (folder.getChildFile (labelsFileName).loadFileAsString());
        analysis::lyrics::timeWords (lyrics, baseSong);
        if (const auto& mt = processor.getMultitrack(); std::abs (mt.getSongSpeed() - 1.0) > 1.0e-3 || std::abs (mt.getSongSemitones()) > 5.0e-3)
        {
            // (the editor was closed and opened again with the song in another key or speed)
            shift.speed = mt.getSongSpeed();
            shift.semitones = mt.getSongSemitones();
            shift.live = true;
            song = baseSong.transformed (shift.semitones, shift.speed);
        }
        guidePending = true; // (its level and mute are set once the track is there)
        takesSeen = folder.getChildFile (takesFileName).loadFileAsString();
        takesReady = false; // (written again once the tracks are back and the song's takes are on them)
        sections = {};
        if (const auto cached = folder.getChildFile (sectionsFileName); cached.existsAsFile())
        {
            sections = sectionsFromText (cached.loadFileAsString());
            sectionsKnown = true;
        }
        // (a session that had no takes row, e.g. from before this was kept, gets them from the file)
        restoreTakes();
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
    if (reviewJob != nullptr)
        reviewJob->cancel = true;
    if (loop.active)
        endLoop();
    dropShift (pending);
    dropShift (shift);
    if (shiftLoad != nullptr)
        shiftLoad->cancel = true;
    saver.removeAllJobs (false, 20000); // a copy being kept finishes (a few seconds at most)
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
    result.source = source;
    result.name = source.getFileNameWithoutExtension();
    result.voiceOnly = ! options.split;
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
            if (LibraryEntry info; readInfo (result.folder, info))
                result.key = info.key;
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

    juce::AudioBuffer<float> voice, keyAudio; // keyAudio: the backing, when there is one
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
        keyAudio = std::move (backing);
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

    // The key (from the backing, or the voice when that's all there is), for the library.
    {
        const auto estimate = analysis::detectKey (keyAudio.getNumSamples() > 0 ? keyAudio : voice, rate,
                                                   [&] { return report (0.5); });
        result.key = estimate.isValid() ? estimate.getName() : juce::String();
    }
    LibraryEntry info;
    info.folder = result.folder;
    info.name = result.name;
    info.key = result.key;
    info.source = source;
    info.seconds = result.seconds;
    info.voiceOnly = result.voiceOnly;
    info.opened = juce::Time::currentTimeMillis();
    writeInfo (result.folder, info);
    return result;
}

void Practice::writeInfo (const juce::File& folder, const LibraryEntry& e)
{
    juce::String text;
    text << "name=" << e.name.replaceCharacters ("\r\n", "  ") << "\n"
         << "key=" << e.key << "\n"
         << "source=" << e.source.getFullPathName() << "\n"
         << "seconds=" << juce::String (e.seconds, 3) << "\n"
         << "voiceOnly=" << (e.voiceOnly ? 1 : 0) << "\n"
         << "opened=" << e.opened << "\n"
         << "best=" << juce::String (e.best, 1) << "\n"
         << "last=" << juce::String (e.last, 1) << "\n"
         << "passes=" << e.passes << "\n";
    folder.getChildFile (infoFileName).replaceWithText (text);
}

bool Practice::readInfo (const juce::File& folder, LibraryEntry& e)
{
    const auto file = folder.getChildFile (infoFileName);
    if (! file.existsAsFile())
        return false;

    e.folder = folder;
    for (const auto& line : juce::StringArray::fromLines (file.loadFileAsString()))
    {
        const auto eq = line.indexOfChar ('=');
        if (eq < 0)
            continue;
        const auto key = line.substring (0, eq), value = line.substring (eq + 1);
        if (key == "name") e.name = value;
        else if (key == "key") e.key = value;
        else if (key == "source") e.source = juce::File (value);
        else if (key == "seconds") e.seconds = value.getDoubleValue();
        else if (key == "voiceOnly") e.voiceOnly = value.getIntValue() != 0;
        else if (key == "opened") e.opened = value.getLargeIntValue();
        else if (key == "best") e.best = value.getFloatValue();
        else if (key == "last") e.last = value.getFloatValue();
        else if (key == "passes") e.passes = value.getIntValue();
    }
    return e.name.isNotEmpty();
}

std::vector<Practice::LibraryEntry> Practice::getLibrary (const juce::File& root)
{
    std::vector<LibraryEntry> entries;
    for (const auto& entry : juce::RangedDirectoryIterator (root, false, "*", juce::File::findDirectories))
    {
        const auto folder = entry.getFile();
        LibraryEntry info;
        if (folder.getChildFile (melodyFileName).existsAsFile() && readInfo (folder, info)
            && (info.voiceOnly || (folder.getChildFile (guideFileName).existsAsFile() && folder.getChildFile (backingFileName).existsAsFile())))
            entries.push_back (info);
    }
    std::sort (entries.begin(), entries.end(), [] (const auto& a, const auto& b) { return a.opened > b.opened; });
    return entries;
}

Practice::Prepared Practice::loadFromCache (const juce::File& folder)
{
    Prepared result;
    LibraryEntry info;
    if (! readInfo (folder, info))
    {
        result.error = "That song isn't in the library any more.";
        return result;
    }

    result.folder = folder;
    result.voiceOnly = info.voiceOnly;
    result.source = info.source;
    result.name = info.name;
    result.key = info.key;
    result.seconds = info.seconds;
    result.fromCache = true;
    result.guide = info.voiceOnly ? info.source : folder.getChildFile (guideFileName);
    result.backing = info.voiceOnly ? juce::File() : folder.getChildFile (backingFileName);
    result.song = coach::Song::fromString (folder.getChildFile (melodyFileName).loadFileAsString());
    if (result.song.isEmpty() || ! result.guide.existsAsFile() || (! info.voiceOnly && ! result.backing.existsAsFile()))
        result.error = info.voiceOnly ? "The voice file " + info.source.getFullPathName() + " has moved. Open it again from its place."
                                      : "This song's saved stems are incomplete. Open the song again.";
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

    if (action == "library")
    {
        juce::Array<juce::var> list;
        for (const auto& e : getLibrary())
            list.add (object ({ { "id", e.folder.getFileName() }, { "name", e.name }, { "key", e.key }, { "seconds", round3 (e.seconds) },
                                { "voiceOnly", e.voiceOnly }, { "opened", (double) e.opened },
                                { "best", round2 (e.best) }, { "last", round2 (e.last) }, { "passes", e.passes },
                                { "current", e.folder.getFullPathName() == settings.getValue ("practice.folder") && ! song.isEmpty() } }));
        return list;
    }

    if (action == "openCached")
    {
        const auto folder = getCacheRoot().getChildFile (juce::File::createLegalFileName (options["id"].toString()));
        auto prepared = loadFromCache (folder);
        if (prepared.error.isNotEmpty())
        {
            emit ("practice", object ({ { "state", "error" }, { "error", prepared.error } }));
            return object ({ { "ok", false }, { "error", prepared.error } });
        }
        if (job != nullptr)
            job->cancel = true;
        job = nullptr;
        install (prepared);
        emit ("practice", object ({ { "state", "ready" }, { "song", describeSong() }, { "fromCache", true } }));
        return object ({ { "ok", true } });
    }

    if (action == "forget")
    {
        const auto folder = getCacheRoot().getChildFile (juce::File::createLegalFileName (options["id"].toString()));
        if (folder.getParentDirectory() == getCacheRoot() && folder.getChildFile (infoFileName).existsAsFile()
            && folder.getFullPathName() != settings.getValue ("practice.folder"))
            folder.deleteRecursively();
        return object ({ { "ok", true } });
    }

    if (action == "speed")
    {
        startSpeed ((double) options.getProperty ("speed", 1.0), (double) options.getProperty ("semitones", 0.0));
        return describe();
    }

    if (action == "quality")
    {
        startQuality();
        return describe();
    }

    if (action == "cancel")
    {
        if (job != nullptr)
            job->cancel = true;
        if (isShiftPending())
        {
            // A key or speed change not heard yet: stay as we are.
            dropShift (pending);
            if (shiftLoad != nullptr)
                shiftLoad->cancel = true;
            shiftLoad = nullptr;
            emit ("practice", object ({ { "state", "shifting" }, { "shift", describeShift() } }));
        }
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
    else if (action == "sync")
    {
        visualSyncMs = juce::jlimit (-200.0, 200.0, (double) options.getProperty ("ms", visualSyncMs));
        settings.setValue ("practice.visualSyncMs", visualSyncMs);
    }
    else if (action == "reset")
    {
        restartFrom (0.0);
    }
    else if (action == "report")
    {
        return describeReport();
    }
    else if (action == "setMelody")
    {
        auto edited = songFromNotes (options["notes"]);
        if (edited.isEmpty())
            return object ({ { "ok", false }, { "error", "The song needs at least one note to sing." } });
        if (songFolder.isDirectory())
            songFolder.getChildFile (editedMelodyFileName).replaceWithText (edited.toString());
        useMelody (std::move (edited));
    }
    else if (action == "resetMelody")
    {
        if (songFolder.isDirectory())
        {
            songFolder.getChildFile (editedMelodyFileName).deleteFile();
            useMelody (coach::Song::fromString (songFolder.getChildFile (melodyFileName).loadFileAsString()));
        }
    }
    else if (action == "labels")
    {
        return object ({ { "library", describeLabelLibrary() }, { "annotations", describeAnnotations() } });
    }
    else if (action == "saveLabels")
    {
        if ((bool) options.getProperty ("reset", false))
        {
            settings.removeValue ("practice.labelLibrary");
            settings.saveIfNeeded();
            return describeLabelLibrary();
        }
        juce::Array<juce::var> clean;
        if (const auto* list = options["library"].getArray())
            for (const auto& item : *list)
            {
                auto label = labelFromVar (item);
                if (label.text.isEmpty())
                    continue;
                if (label.id.isEmpty())
                    label.id = juce::Uuid().toString().substring (0, 8);
                clean.add (describeLabel (label));
            }
        settings.setValue ("practice.labelLibrary", juce::JSON::toString (juce::var (clean), true));
        settings.saveIfNeeded();
        return describeLabelLibrary();
    }
    else if (action == "annotate")
    {
        std::vector<Label> labels;
        if (const auto* list = options["labels"].getArray())
            for (const auto& item : *list)
                if (auto label = labelFromVar (item); label.text.isNotEmpty())
                    labels.push_back (label);
        annotate (options["kind"].toString() == "phrase", toOriginalTime ((double) options["time"]), std::move (labels));
    }
    else if (action == "takes")
    {
        return describeTakes();
    }
    else if (action == "review")
    {
        startReview ((int) options.getProperty ("id", -1));
    }
    else if (action == "stopReview")
    {
        if (reviewJob != nullptr)
            reviewJob->cancel = true;
        reviewJob = nullptr;
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
    else if (action == "lyrics")
    {
        setLyrics (options["text"].toString());
        return describeLyrics();
    }
    else if (action == "importLyrics")
    {
        chooser = std::make_unique<juce::FileChooser> ("Choose the song's lyrics (.lrc or plain text)",
                                                       source.getParentDirectory().isDirectory() ? source.getParentDirectory()
                                                                                                 : juce::File::getSpecialLocation (juce::File::userMusicDirectory),
                                                       "*.lrc;*.txt");
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this] (const juce::FileChooser& c)
        {
            if (! c.getResult().existsAsFile())
                return;
            if (c.getResult().getSize() > 1024 * 1024)
            {
                emit ("practice", object ({ { "state", "error" }, { "error", c.getResult().getFileName() + " is too big for lyrics." } }));
                return;
            }
            setLyrics (c.getResult().loadFileAsString());
        });
        return object ({ { "ok", true } });
    }
    else if (action == "exportLyrics")
    {
        if (lyrics.isEmpty())
            return object ({ { "ok", false } });
        chooser = std::make_unique<juce::FileChooser> ("Save the lyrics with their times",
                                                       (source.getParentDirectory().isDirectory() ? source.getParentDirectory()
                                                                                                  : juce::File::getSpecialLocation (juce::File::userMusicDirectory))
                                                           .getChildFile (songName + ".lrc"),
                                                       "*.lrc");
        const auto text = lyrics.toLrc();
        chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting, [text] (const juce::FileChooser& c)
        {
            if (c.getResult() != juce::File())
                c.getResult().withFileExtension ("lrc").replaceWithText (text);
        });
        return object ({ { "ok", true } });
    }
    else if (action == "alignLyrics")
    {
        if ((bool) options.getProperty ("all", false))
            for (auto& line : lyrics.lines)
                line.pinned = false;
        analysis::lyrics::align (lyrics, baseSong);
        saveLyrics();
        return describeLyrics();
    }
    else if (action == "nudgeLyric")
    {
        const auto index = (int) options.getProperty ("line", -1);
        if (juce::isPositiveAndBelow (index, (int) lyrics.lines.size()))
        {
            const auto& line = lyrics.lines[(size_t) index];
            // (`by` is in the song as it plays: a second at 50% speed is half a second of the song)
            const auto by = options.hasProperty ("at") ? toOriginalTime ((double) options["at"]) - line.start
                                                       : (double) options.getProperty ("by", 0.0) * (toOriginalTime (1.0) - toOriginalTime (0.0));
            analysis::lyrics::nudge (lyrics, index, by, baseSong);
            saveLyrics();
        }
        return describeLyrics();
    }
    else if (action == "unloop")
    {
        if (loopJob != nullptr)
            loopJob->cancel = true;
        loopJob = nullptr;
        if (loop.active)
        {
            const auto from = loop.from / shift.speed;
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
    return object ({ { "song", describeSong() }, { "job", describeJob() }, { "guideLevel", guideLevel }, { "visualSync", visualSyncMs }, { "open", viewOpen },
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
                     { "hasBacking", backingFile != juce::File() }, { "edited", isMelodyEdited() }, { "key", songKey },
                     { "shift", describeShift() }, { "lyrics", describeLyrics() }, { "annotations", describeAnnotations() },
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
    for (auto* s : { &pending, &shift })
    {
        if (s->render != nullptr)
            s->render->cancel();
        *s = {};
    }
    if (shiftLoad != nullptr)
        shiftLoad->cancel = true;
    shiftLoad = nullptr;
    songFolder = prepared.folder;
    processor.getMultitrack().setSongTransform (1.0, 0.0);
    lyrics = prepared.folder != juce::File() ? analysis::lyrics::Lyrics::fromString (prepared.folder.getChildFile (lyricsFileName).loadFileAsString())
                                             : analysis::lyrics::Lyrics();
    annotations = prepared.folder != juce::File() ? labelsFromText (prepared.folder.getChildFile (labelsFileName).loadFileAsString())
                                                  : std::vector<Annotation>();

    auto& multitrack = processor.getMultitrack();
    multitrack.pause();
    multitrack.seek (0);

    // The song replaces whatever was on the Live tracks (takes stay on disk).
    for (const auto& track : multitrack.getTracks())
        multitrack.removeTrack (track->id);

    if (prepared.backing != juce::File())
        multitrack.loadBackingTrack (prepared.backing);
    multitrack.loadBackingTrack (prepared.guide);

    // The takes recorded over this song come back with it.
    takesReady = false;
    takesSeen = prepared.folder.getChildFile (takesFileName).loadFileAsString();
    restoreTakes();

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

    source = prepared.source != juce::File() ? prepared.source : prepared.guide;
    songName = prepared.name.isNotEmpty() ? prepared.name : source.getFileNameWithoutExtension();
    songKey = prepared.key;
    if (LibraryEntry info; readInfo (prepared.folder, info))
    {
        info.opened = juce::Time::currentTimeMillis();
        writeInfo (prepared.folder, info);
    }
    else if (prepared.folder != juce::File())
    {
        info.folder = prepared.folder;
        info.name = songName;
        info.key = prepared.key;
        info.source = source;
        info.seconds = prepared.seconds;
        info.voiceOnly = prepared.voiceOnly;
        info.opened = juce::Time::currentTimeMillis();
        writeInfo (prepared.folder, info);
    }
    guideFile = prepared.guide;
    backingFile = prepared.backing;
    guidePending = true;
    setSong (readMelody (prepared.folder, prepared.song));
    analysis::lyrics::timeWords (lyrics, baseSong); // (the words' times aren't kept)

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
        if (track->file == guideFile || track->id == shift.guideId || track->id == pending.guideId || (loop.active && track->id == loop.guideId))
        {
            multitrack.setTrackGain (track->id, guideLevel);
            if (track->file == guideFile)
                guidePending = false;
        }
    applyMutes();
}

// One line per take: path | start seconds | gain | muted.
juce::String Practice::takesSignature() const
{
    juce::String text;
    const auto& multitrack = processor.getMultitrack();
    const auto rate = juce::jmax (1.0, multitrack.getSampleRate());
    for (const auto& track : multitrack.getTracks())
        if (track->isTake && track->file.existsAsFile())
        {
            // (the key and speed it was sung at, seen when it first shows up)
            const auto found = takeShifts.emplace (track->file.getFullPathName(), std::make_pair (shift.speed, shift.semitones)).first;
            text << track->file.getFullPathName() << "|" << juce::String ((double) track->start / rate, 4) << "|"
                 << juce::String (track->gain.load(), 3) << "|" << (track->muted.load() ? 1 : 0) << "|"
                 << juce::String (found->second.first, 4) << "|" << juce::String (found->second.second, 4) << "\n";
        }
    return text;
}

void Practice::restoreTakes()
{
    if (songFolder == juce::File())
        return;

    auto& multitrack = processor.getMultitrack();
    const auto rate = juce::jmax (1.0, multitrack.getSampleRate());
    const auto present = takesSignature(); // (already on the tracks, e.g. restored with the session)
    for (const auto& line : juce::StringArray::fromLines (takesSeen))
    {
        const auto parts = juce::StringArray::fromTokens (line, "|", "");
        if (parts.size() < 4)
            continue;
        const juce::File file (parts[0]);
        if (parts.size() >= 6)
            takeShifts[file.getFullPathName()] = { juce::jlimit (0.25, 4.0, parts[4].getDoubleValue()), parts[5].getDoubleValue() };
        if (! file.existsAsFile() || present.contains (file.getFullPathName() + "|"))
            continue;
        multitrack.addTake (file, (juce::int64) std::round (parts[1].getDoubleValue() * rate), parts[2].getFloatValue(), parts[3].getIntValue() != 0);
    }
}

void Practice::saveTakesIfChanged()
{
    if (songFolder == juce::File() || ! songFolder.isDirectory())
        return;

    const auto& multitrack = processor.getMultitrack();
    if (! takesReady)
    {
        // Wait for the song's own takes to be on the tracks before writing anything.
        if (multitrack.getNumLoading() == 0)
        {
            takesReady = true;
            takesSeen = takesSignature().isNotEmpty() ? takesSignature() : takesSeen;
        }
        return;
    }
    if (multitrack.getNumLoading() > 0)
        return;

    if (const auto now = takesSignature(); now != takesSeen)
    {
        takesSeen = now;
        songFolder.getChildFile (takesFileName).replaceWithText (now);
    }
}

void Practice::applyMutes()
{
    // Which of the song's versions is heard: a rendered loop's, else the song in another
    // key or speed, else the song as it is. The others rest; the guide is muted at level 0.
    auto& multitrack = processor.getMultitrack();
    const auto loopRendered = loop.active && (loop.backingId >= 0 || loop.guideId >= 0);
    auto changed = false;
    for (const auto& track : multitrack.getTracks())
    {
        const auto own = track->file != juce::File() && (track->file == guideFile || track->file == backingFile);
        const auto isShift = track->id == shift.backingId || track->id == shift.guideId;
        const auto isPending = track->id == pending.backingId || track->id == pending.guideId;
        const auto isLoop = loop.active && (track->id == loop.backingId || track->id == loop.guideId);
        if (! (own || isShift || isPending || isLoop))
            continue;

        if (own)
            multitrack.setTrackStretched (track->id, true); // (it plays through the live key and speed change)
        const auto heard = loopRendered ? isLoop : (shift.backingId >= 0 || shift.guideId >= 0) ? isShift : own;
        const auto isGuide = track->file == guideFile || track->id == shift.guideId || track->id == pending.guideId
                          || (loop.active && track->id == loop.guideId);
        const auto mute = ! heard || (isGuide && guideLevel <= 0.0f);
        changed = changed || track->muted.load() != mute;
        multitrack.setTrackMuted (track->id, mute);
    }

    // (the Live track list shows the mute buttons)
    if (changed && multitrack.onTracksChanged)
        multitrack.onTracksChanged();
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

    if (reviewJob != nullptr && reviewJob->finished)
    {
        const auto finished = reviewJob;
        reviewJob = nullptr;
        if (! finished->cancel)
            emit ("practice", object ({ { "state", "review" }, { "review", finished->ok ? describeReview (*finished) : juce::var() },
                                        { "error", finished->ok ? "" : "This take couldn't be read." } }));
    }

    tickShift();

    if (guidePending)
        applyGuideLevel();
    saveTakesIfChanged();

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
    // What the picture shows: the sound that's being heard now. The singer's own answer (scoring) is later by
    // the input latency too, but the notes should be at the line when you hear them.
    const auto heardLatency = (double) (outputLatency != nullptr ? outputLatency() : deviceLatency != nullptr ? deviceLatency() : 0) / rate;
    const auto displayTime = juce::jmax (0.0, position - heardLatency - visualSyncMs / 1000.0);

    // A jump (a seek, a restart, a loop going round) starts the part after it over.
    if (lastPosition >= 0.0 && (position < lastPosition - 0.05 || position > lastPosition + 0.75))
        restartFrom (songTime);
    lastPosition = position;

    // The song stopped after some singing: the report for the pass.
    if (wasPlaying && ! playing && std::any_of (phraseScores.begin(), phraseScores.end(), [] (const auto& p) { return p.scored; }))
    {
        emit ("practiceReport", describeReport());
        recordPass();
    }
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

    // The backing's chord here (Multitrack listens to it for the harmonies), for the chord chip.
    emit ("practicePitch", object ({ { "time", round3 (displayTime) }, { "playing", playing }, { "midi", round2 (lastMidi) },
                                     { "frames", trail }, { "chord", processor.getMultitrack().getBackingChord (multitrack.getPosition()).getName() } }));
}

//==============================================================================
juce::var Practice::describeTakes() const
{
    juce::Array<juce::var> list;
    const auto& multitrack = processor.getMultitrack();
    const auto rate = juce::jmax (1.0, multitrack.getSampleRate());
    for (const auto& track : multitrack.getTracks())
    {
        if (! track->isTake)
            continue;
        const auto found = track->file != juce::File() ? takeShifts.find (track->file.getFullPathName()) : takeShifts.end();
        list.add (object ({ { "id", track->id }, { "name", track->name }, { "start", round3 ((double) track->start / rate) },
                            { "seconds", round3 (track->audio.getNumSamples() / rate) },
                            { "speed", found != takeShifts.end() ? found->second.first : shift.speed },
                            { "semitones", found != takeShifts.end() ? found->second.second : shift.semitones },
                            { "muted", track->muted.load() } }));
    }
    return list;
}

// Listens to a take again: its pitch laid over the notes it was sung against.
void Practice::startReview (int trackId)
{
    if (reviewJob != nullptr)
        reviewJob->cancel = true;
    reviewJob = nullptr;

    const auto& multitrack = processor.getMultitrack();
    std::shared_ptr<const Multitrack::Track> found;
    for (const auto& track : multitrack.getTracks())
        if (track->id == trackId && track->isTake && track->audio.getNumSamples() > 0)
            found = track;

    if (found == nullptr || baseSong.isEmpty())
    {
        emit ("practice", object ({ { "state", "review" }, { "review", juce::var() },
                                    { "error", baseSong.isEmpty() ? "Open a song first." : "That take isn't ready yet." } }));
        return;
    }

    auto newJob = std::make_shared<ReviewJob>();
    newJob->track = found;
    newJob->sampleRate = multitrack.getSampleRate();
    newJob->start = (double) found->start / juce::jmax (1.0, newJob->sampleRate);
    newJob->end = newJob->start + found->audio.getNumSamples() / juce::jmax (1.0, newJob->sampleRate);

    // The notes as they played when it was recorded (its own key and speed).
    const auto at = found->file != juce::File() ? takeShifts.find (found->file.getFullPathName()) : takeShifts.end();
    const auto speed = at != takeShifts.end() ? at->second.first : shift.speed;
    const auto semitones = at != takeShifts.end() ? at->second.second : shift.semitones;
    newJob->song = baseSong.transformed (semitones, speed);
    reviewJob = newJob;

    juce::Thread::launch ([newJob]
    {
        const auto trackPitch = analysis::pitch::analyse (newJob->track->audio, newJob->sampleRate, [newJob] { return ! newJob->cancel.load(); });
        if (! newJob->cancel)
        {
            newJob->frames = coach::framesFrom (trackPitch, newJob->start);
            newJob->report = coach::score (newJob->song, newJob->frames, newJob->start - 0.5, newJob->end + 0.5);
            newJob->ok = trackPitch.size() > 0;
        }
        newJob->finished = true;
    });
}

// { take, name, start, end, notes: [start, end, pitch, phrase...], phrases: [start, end...], frames: [time, midi...] every
// 20 ms, scores: { score, lean, notes: [...], phrases: [...] } } for the page.
juce::var Practice::describeReview (const ReviewJob& r) const
{
    juce::Array<juce::var> notes, phrases, frames, noteScoresOut, phraseScoresOut;
    for (const auto& n : r.song.notes)
    {
        notes.add (round3 (n.start));
        notes.add (round3 (n.end));
        notes.add (round2 (n.pitch));
        notes.add (n.phrase);
    }
    for (const auto& p : r.song.phrases)
    {
        phrases.add (round3 (p.start));
        phrases.add (round3 (p.end));
    }

    // (a frame every 5 ms is more than the picture can use: one in four)
    for (size_t i = 0; i < r.frames.size(); i += 4)
    {
        frames.add (round2 (r.frames[i].time));
        frames.add (round2 (r.frames[i].midi));
    }

    for (size_t i = 0; i < r.report.notes.size(); ++i)
        if (r.report.phrases[(size_t) r.song.notes[i].phrase].scored)
            noteScoresOut.add (describeNote ((int) i, r.report.notes[i]));
    for (size_t i = 0; i < r.report.phrases.size(); ++i)
        if (r.report.phrases[i].scored)
            phraseScoresOut.add (describePhrase ((int) i, r.report.phrases[i]));

    return object ({ { "take", r.track->id }, { "name", r.track->name }, { "start", round3 (r.start) }, { "end", round3 (r.end) },
                     { "notes", notes }, { "phrases", phrases }, { "frames", frames },
                     { "score", round2 (r.report.score) }, { "lean", round2 (r.report.lean) },
                     { "noteScores", noteScoresOut }, { "phraseScores", phraseScoresOut },
                     { "lowest", round2 (r.song.getLowest()) }, { "highest", round2 (r.song.getHighest()) } });
}

void Practice::recordPass()
{
    if (songFolder == juce::File() || ! songFolder.isDirectory() || song.phrases.empty() || loop.active)
        return;
    int scored = 0;
    for (const auto& p : phraseScores)
        scored += p.scored ? 1 : 0;
    if (scored * 2 < (int) song.phrases.size())
        return;

    LibraryEntry info;
    if (! readInfo (songFolder, info))
        return;
    info.last = getPassScore();
    info.best = juce::jmax (info.best, info.last);
    ++info.passes;
    writeInfo (songFolder, info);
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

juce::File Practice::getShiftFile (const juce::File& folder, double speed, double semitones, bool voice)
{
    juce::String name;
    name << juce::String (std::round (speed * 1000.0) / 1000.0, 3) << "x " << (semitones >= 0.0 ? "+" : "")
         << juce::String (std::round (semitones * 100.0) / 100.0, 2) << " st - " << (voice ? "guide" : "backing") << ".flac";
    return folder.getChildFile (shiftFolderName).getChildFile (name);
}

double Practice::getOriginalTime() const
{
    const auto& multitrack = processor.getMultitrack();
    const auto position = (double) multitrack.getPosition() / juce::jmax (1.0, multitrack.getSampleRate());
    if (loop.active)
        return loop.from + position * (loop.backingId >= 0 || loop.guideId >= 0 ? loop.speed : 1.0); // (roughly: the loop's lead-in)
    return shift.isActive() ? position * shift.speed : position;
}

juce::var Practice::describeShift() const
{
    const auto* render = pending.render != nullptr ? pending.render.get() : shift.render.get();
    const auto busy = render != nullptr || shiftLoad != nullptr;
    return object ({ { "speed", shift.speed }, { "semitones", shift.semitones },
                     { "pending", isShiftPending() },
                     { "target", isShiftPending() ? object ({ { "speed", shiftLoad != nullptr ? shiftLoad->speed : pending.speed },
                                                              { "semitones", shiftLoad != nullptr ? shiftLoad->semitones : pending.semitones } })
                                                  : juce::var() },
                     { "progress", busy ? (render != nullptr ? round3 (render->getProgress()) : 0.0) : 1.0 } });
}

void Practice::dropShift (Shift& s)
{
    if (s.render != nullptr)
        s.render->cancel();
    auto& multitrack = processor.getMultitrack();
    for (auto id : { s.backingId, s.guideId })
        if (id >= 0)
            multitrack.removeTrack (id);
    s = {};
}

void Practice::startSpeed (double speed, double semitones)
{
    if (baseSong.isEmpty() && guideFile == juce::File())
        return;

    speed = std::round (juce::jlimit (0.5, 1.5, speed) * 1000.0) / 1000.0;
    semitones = std::round (juce::jlimit (-12.0, 12.0, semitones) * 100.0) / 100.0;

    // Whatever was on its way is dropped (a newer change wins).
    dropShift (pending);
    if (shiftLoad != nullptr)
        shiftLoad->cancel = true;
    shiftLoad = nullptr;

    const auto neutral = std::abs (speed - 1.0) < 1.0e-3 && std::abs (semitones) < 5.0e-3;
    const auto same = std::abs (speed - shift.speed) < 1.0e-3 && std::abs (semitones - shift.semitones) < 5.0e-3;
    if (same)
    {
        emit ("practice", object ({ { "state", "shifting" }, { "shift", describeShift() } }));
        return;
    }

    if (neutral)
    {
        // Back to the song as it is, where we are.
        processor.getMultitrack().setSongTransform (1.0, 0.0);
        swapShift();
        return;
    }

    // Live: the song's own tracks go through the stretcher (Multitrack::setSongTransform), so there's
    // nothing to render or wait for, and no extra tracks. The notes follow (swapShift).
    pending = {};
    pending.speed = speed;
    pending.semitones = semitones;
    pending.live = true;
    processor.getMultitrack().setSongTransform (speed, semitones);
    swapShift();
}

// A better-sounding copy of the song in the key and speed it plays in now (the live change moves the
// voice's character a little; this keeps it, and takes a while). Hidden tracks: it replaces the live
// change as what's heard, and goes away with the next change.
void Practice::startQuality()
{
    if (! shift.live || (baseSong.isEmpty() && guideFile == juce::File()))
        return;

    const auto speed = shift.speed, semitones = shift.semitones;
    dropShift (pending);
    if (shiftLoad != nullptr)
        shiftLoad->cancel = true;
    shiftLoad = nullptr;
    pending.speed = speed;
    pending.semitones = semitones;

    // Made before: load it (a moment) instead of rendering it again.
    const auto cachedBacking = getShiftFile (songFolder, speed, semitones, false), cachedGuide = getShiftFile (songFolder, speed, semitones, true);
    if (songFolder != juce::File() && cachedGuide.existsAsFile() && (backingFile == juce::File() || cachedBacking.existsAsFile()))
    {
        auto load = std::make_shared<ShiftLoad>();
        load->speed = speed;
        load->semitones = semitones;
        load->guideFile = cachedGuide;
        load->backingFile = backingFile != juce::File() ? cachedBacking : juce::File();
        shiftLoad = load;
        const auto rate = processor.getMultitrack().getSampleRate();
        juce::Thread::launch ([load, rate]
        {
            auto guide = Multitrack::decode (load->guideFile, rate);
            auto backing = load->backingFile != juce::File() ? Multitrack::decode (load->backingFile, rate) : Multitrack::Decoded {};
            load->ok = guide.error.isEmpty() && (load->backingFile == juce::File() || backing.error.isEmpty());
            load->guide = std::move (guide.audio);
            load->backing = std::move (backing.audio);
            load->finished = true;
        });
        emit ("practice", object ({ { "state", "shifting" }, { "shift", describeShift() } }));
        return;
    }

    // Render it: empty tracks (muted until swapped in) filled piece by piece, the playhead's first.
    auto& multitrack = processor.getMultitrack();
    std::vector<KeySpeedRender::Part> parts;
    for (const auto& track : multitrack.getTracks())
    {
        const auto voice = track->file == guideFile;
        if (! voice && ! (backingFile != juce::File() && track->file == backingFile))
            continue;

        KeySpeedRender::Part part;
        part.source = track;
        part.voice = voice;
        const auto length = KeySpeedRender::getLength (track->audio.getNumSamples(), speed);
        const auto id = multitrack.addFilling (voice ? "Guide (new key)" : "Backing (new key)", track->audio.getNumChannels(), length,
                                               0, true, part.fill);
        (voice ? pending.guideId : pending.backingId) = id;
        for (const auto& t : multitrack.getTracks())
            if (t->id == id)
                part.target = t;
        parts.push_back (std::move (part));
    }

    if (parts.empty())
        return;

    pending.render = std::make_shared<KeySpeedRender> (multitrack.getSampleRate(), speed, semitones);
    pending.render->start (std::move (parts), getOriginalTime() / speed);
    applyGuideLevel();
    emit ("practice", object ({ { "state", "shifting" }, { "shift", describeShift() } }));
}

void Practice::swapShift()
{
    auto& multitrack = processor.getMultitrack();
    const auto original = getOriginalTime();

    // The new version becomes the one heard (none: the song as it is); the old one goes.
    auto old = shift;
    shift = pending;
    pending = {};
    if (old.render != nullptr)
        old.render->cancel();
    for (auto id : { old.backingId, old.guideId })
        if (id >= 0)
            multitrack.removeTrack (id);

    if (! shift.isActive())
        shift = {}; // speed 1, key 0
    applyGuideLevel();

    const auto shifted = shift.isActive() && ! baseSong.isEmpty() ? baseSong.transformed (shift.semitones, shift.speed) : baseSong;
    if (loop.active)
    {
        // A loop keeps playing; the whole song comes back changed after it.
    }
    else
    {
        const auto position = original / shift.speed;
        multitrack.seek ((juce::int64) std::round (position * multitrack.getSampleRate()));
        lastPosition = position;
        song = shifted;
        if (noteScores.size() != song.notes.size())
            noteScores.assign (song.notes.size(), {});
        if (phraseScores.size() != song.phrases.size())
            phraseScores.assign (song.phrases.size(), {});
        restartFrom (position);
    }
    emit ("practice", object ({ { "state", "ready" }, { "song", describeSong() }, { "fromCache", true }, { "quiet", true } }));
}

void Practice::tickShift()
{
    auto& multitrack = processor.getMultitrack();

    if (shiftLoad != nullptr && shiftLoad->finished)
    {
        const auto load = shiftLoad;
        shiftLoad = nullptr;
        if (! load->ok || load->cancel)
        {
            // The kept copy is unreadable: forget it and render again.
            load->guideFile.deleteFile();
            load->backingFile.deleteFile();
            startSpeed (load->speed, load->semitones);
            return;
        }
        pending = {};
        pending.speed = load->speed;
        pending.semitones = load->semitones;
        pending.guideId = multitrack.addAudio ("Guide (new key)", std::move (load->guide), 0);
        multitrack.setTrackMuted (pending.guideId, true);
        if (load->backing.getNumSamples() > 0)
        {
            pending.backingId = multitrack.addAudio ("Backing (new key)", std::move (load->backing), 0);
            multitrack.setTrackMuted (pending.backingId, true);
        }
        load->guideFile.setLastModificationTime (juce::Time::getCurrentTime());
        swapShift();
    }

    if (pending.render != nullptr)
    {
        // Swap it in once what's about to play is ready (or the whole of it is).
        const auto at = getOriginalTime() / pending.speed;
        pending.render->setFocus (at);
        const auto ahead = multitrack.isPlaying() ? 0.35 : 0.15;
        if (pending.render->isFinished() || pending.render->isReady (at, at + ahead))
        {
            if (pending.render->isCancelled())
                dropShift (pending);
            else
                swapShift();
        }
        else
        {
            emit ("practice", object ({ { "state", "shifting" }, { "shift", describeShift() } }));
        }
    }

    if (shift.render != nullptr)
    {
        if (! loop.active)
            shift.render->setFocus ((double) multitrack.getPosition() / juce::jmax (1.0, multitrack.getSampleRate()));
        if (shift.render->isFinished())
        {
            const auto done = shift.render;
            shift.render = nullptr;
            if (! done->isCancelled())
            {
                for (auto id : { shift.backingId, shift.guideId })
                    if (id >= 0)
                        multitrack.finishFill (id);
                saveShift();
            }
        }
        emit ("practice", object ({ { "state", "shifting" }, { "shift", describeShift() } }));
    }
}

void Practice::saveShift()
{
    if (songFolder == juce::File() || ! shift.isActive())
        return;

    std::vector<std::pair<std::shared_ptr<const Multitrack::Track>, juce::File>> toWrite;
    for (const auto& track : processor.getMultitrack().getTracks())
        if (track->id == shift.backingId || track->id == shift.guideId)
            toWrite.emplace_back (track, getShiftFile (songFolder, shift.speed, shift.semitones, track->id == shift.guideId));
    const auto rate = processor.getMultitrack().getSampleRate();
    const auto folder = songFolder.getChildFile (shiftFolderName);

    saver.addJob ([toWrite, rate, folder]
    {
        folder.createDirectory();
        for (const auto& [track, file] : toWrite)
        {
            const auto temp = file.getSiblingFile (file.getFileNameWithoutExtension() + ".part.flac");
            temp.deleteFile();
            std::unique_ptr<juce::OutputStream> stream (temp.createOutputStream());
            if (stream == nullptr)
                return;
            auto writer = juce::FlacAudioFormat().createWriterFor (stream, juce::AudioFormatWriterOptions{}.withSampleRate (rate)
                                                                                                       .withNumChannels (track->audio.getNumChannels())
                                                                                                       .withBitsPerSample (24));
            const auto ok = writer != nullptr && writer->writeFromAudioSampleBuffer (track->audio, 0, track->audio.getNumSamples());
            writer.reset();
            if (! ok || ! temp.moveFileTo (file))
            {
                temp.deleteFile();
                return;
            }
        }

        // Only the last few kept (by the guide's date: when each was last used).
        auto guides = folder.findChildFiles (juce::File::findFiles, false, "* - guide.flac");
        std::sort (guides.begin(), guides.end(), [] (const auto& a, const auto& b) { return a.getLastModificationTime() > b.getLastModificationTime(); });
        for (int i = maxShiftsKept; i < guides.size(); ++i)
        {
            const auto stem = guides[i].getFileName().upToLastOccurrenceOf (" - guide.flac", false, false);
            guides[i].deleteFile();
            folder.getChildFile (stem + " - backing.flac").deleteFile();
        }
    });
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

    speed = juce::jlimit (0.5, 1.5, speed);
    semitones = juce::jlimit (-12.0, 12.0, std::round (semitones * 100.0) / 100.0);

    auto& multitrack = processor.getMultitrack();

    // As it is: loop the song's own tracks (the song in another key or speed is rendered).
    if (std::abs (speed - 1.0) < 1.0e-4 && std::abs (semitones) < 1.0e-4 && ! shift.isActive())
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
        song = section != -1 ? cutSong (baseSong, phrase, lastPhrase, 0.0, 1.0, 0.0) : cutSong (baseSong, phrase, 0.0, 1.0, 0.0);
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

    song = done.section != -1 ? cutSong (baseSong, done.phrase, done.lastPhrase, done.from, done.speed, done.semitones)
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

    if (restoreTracks)
        applyMutes();

    if (wasActive)
    {
        song = shift.isActive() && ! baseSong.isEmpty() ? baseSong.transformed (shift.semitones, shift.speed) : baseSong;
        noteScores.assign (song.notes.size(), {});
        phraseScores.assign (song.phrases.size(), {});
        nextNote = nextPhrase = 0;
        scoresReset = true;
    }
}

//==============================================================================
double Practice::toPlayingTime (double t) const
{
    if (loop.active && (loop.backingId >= 0 || loop.guideId >= 0))
        return (t - loop.from) / loop.speed;
    if (loop.active)
        return t;
    return t / shift.speed;
}

double Practice::toOriginalTime (double t) const
{
    if (loop.active && (loop.backingId >= 0 || loop.guideId >= 0))
        return loop.from + t * loop.speed;
    if (loop.active)
        return t;
    return t * shift.speed;
}

void Practice::setLyrics (const juce::String& text)
{
    lyrics = analysis::lyrics::parse (text);
    if (! lyrics.fromLrc)
        analysis::lyrics::align (lyrics, baseSong);
    else
        analysis::lyrics::timeWords (lyrics, baseSong);
    saveLyrics();
    emit ("practice", object ({ { "state", "lyrics" }, { "lyrics", describeLyrics() } }));
}

//==============================================================================
std::vector<Practice::Label> Practice::defaultLabels()
{
    // The cues a vocal coach writes on a lead sheet (icons are UTF-8).
    const auto make = [] (const char* id, const char* text, const char* icon, const char* colour)
    {
        return Label { id, text, juce::String::fromUTF8 (icon), colour };
    };
    return { make ("grit", "Grit", "\xF0\x9F\x94\xA5", "#ff7a5a"), make ("breath", "Breathe", "\xF0\x9F\x92\xA8", "#5aa9ff"),
             make ("prepare", "Prepare position", "\xF0\x9F\x8E\xAF", "#ffc94d"), make ("slower", "Slower", "\xF0\x9F\x90\xA2", "#3fd9c6"),
             make ("piano", "Piano (soft)", "p", "#8fb4ff"), make ("forte", "Forte (loud)", "f", "#ff9a5a"),
             make ("belt", "Belt", "\xF0\x9F\x93\xA2", "#ff6b8a"), make ("head", "Head voice", "\xE2\x98\x81", "#b79bff"),
             make ("vibrato", "Vibrato", "\xE3\x80\xB0", "#7be0a0"), make ("straight", "Straight tone", "\xE2\x80\x94", "#c8cfdd"),
             make ("slide", "Slide in", "\xE2\x86\x97", "#e0b45a"), make ("accent", "Accent", ">", "#ff7a90") };
}

juce::var Practice::describeLabel (const Label& l)
{
    return object ({ { "id", l.id }, { "text", l.text }, { "icon", l.icon }, { "colour", l.colour } });
}

Practice::Label Practice::labelFromVar (const juce::var& v)
{
    Label l;
    l.id = v["id"].toString().trim();
    l.text = v["text"].toString().trim().substring (0, 40);
    l.icon = v["icon"].toString().trim().substring (0, 4);
    l.colour = v["colour"].toString().trim();
    if (! (l.colour.length() == 7 && l.colour.startsWithChar ('#') && l.colour.substring (1).containsOnly ("0123456789abcdefABCDEF")))
        l.colour = "#8c7bff";
    return l;
}

juce::var Practice::describeLabelLibrary() const
{
    juce::Array<juce::var> list;
    const auto saved = juce::JSON::parse (settings.getValue ("practice.labelLibrary"));
    if (const auto* items = saved.getArray())
        for (const auto& item : *items)
            if (auto label = labelFromVar (item); label.text.isNotEmpty())
                list.add (describeLabel (label));
    if (list.isEmpty() && ! saved.isArray())
        for (const auto& label : defaultLabels())
            list.add (describeLabel (label));
    return list;
}

juce::String Practice::labelsToText (const std::vector<Annotation>& all)
{
    juce::Array<juce::var> list;
    for (const auto& a : all)
    {
        juce::Array<juce::var> labels;
        for (const auto& l : a.labels)
            labels.add (describeLabel (l));
        list.add (object ({ { "kind", a.phrase ? "phrase" : "note" }, { "time", round3 (a.time) }, { "labels", labels } }));
    }
    return all.empty() ? juce::String() : juce::JSON::toString (juce::var (list), true);
}

std::vector<Practice::Annotation> Practice::labelsFromText (const juce::String& text)
{
    std::vector<Annotation> all;
    const auto parsed = juce::JSON::parse (text);
    if (const auto* list = parsed.getArray())
        for (const auto& item : *list)
        {
            Annotation a;
            a.phrase = item["kind"].toString() == "phrase";
            a.time = (double) item["time"];
            if (const auto* labels = item["labels"].getArray())
                for (const auto& l : *labels)
                    if (auto label = labelFromVar (l); label.text.isNotEmpty())
                        a.labels.push_back (label);
            if (! a.labels.empty() && std::isfinite (a.time))
                all.push_back (std::move (a));
        }
    return all;
}

juce::var Practice::describeAnnotations() const
{
    // [{ kind, time (as it plays), labels: [label] }]
    juce::Array<juce::var> list;
    for (const auto& a : annotations)
    {
        juce::Array<juce::var> labels;
        for (const auto& l : a.labels)
            labels.add (describeLabel (l));
        list.add (object ({ { "kind", a.phrase ? "phrase" : "note" }, { "time", round3 (toPlayingTime (a.time)) }, { "labels", labels } }));
    }
    return list;
}

void Practice::annotate (bool phrase, double originalTime, std::vector<Label> labels)
{
    // The note or phrase it's on: the one starting there (a hair either side).
    const auto starts = [&]
    {
        std::vector<double> t;
        if (phrase)
            for (const auto& p : baseSong.phrases)
                t.push_back (p.start);
        else
            for (const auto& n : baseSong.notes)
                t.push_back (n.start);
        return t;
    }();
    auto best = -1;
    auto distance = 0.05;
    for (int i = 0; i < (int) starts.size(); ++i)
        if (std::abs (starts[(size_t) i] - originalTime) < distance)
        {
            distance = std::abs (starts[(size_t) i] - originalTime);
            best = i;
        }
    if (best < 0)
        return;

    const auto time = starts[(size_t) best];
    annotations.erase (std::remove_if (annotations.begin(), annotations.end(),
                                       [&] (const Annotation& a) { return a.phrase == phrase && std::abs (a.time - time) < 1.0e-3; }),
                       annotations.end());
    if (! labels.empty())
        annotations.push_back ({ phrase, time, std::move (labels) });
    std::sort (annotations.begin(), annotations.end(), [] (const Annotation& a, const Annotation& b) { return a.time < b.time; });
    saveAnnotations();
    emit ("practice", object ({ { "state", "annotations" }, { "annotations", describeAnnotations() } }));
}

void Practice::saveAnnotations()
{
    if (songFolder == juce::File() || ! songFolder.isDirectory())
        return;
    const auto file = songFolder.getChildFile (labelsFileName);
    const auto text = labelsToText (annotations);
    if (text.isEmpty())
        file.deleteFile();
    else
        file.replaceWithText (text);
}

//==============================================================================
coach::Song Practice::readMelody (const juce::File& folder, const coach::Song& found)
{
    if (folder == juce::File())
        return found;
    const auto edited = folder.getChildFile (editedMelodyFileName);
    if (! edited.existsAsFile())
        return found;
    auto song = coach::Song::fromString (edited.loadFileAsString());
    return song.isEmpty() ? found : song;
}

coach::Song Practice::songFromNotes (const juce::var& flat)
{
    coach::Song result;
    if (const auto* list = flat.getArray())
        for (int i = 0; i + 2 < list->size(); i += 3)
        {
            coach::TargetNote n;
            n.start = (double) (*list)[i];
            n.end = (double) (*list)[i + 1];
            n.pitch = (float) (double) (*list)[i + 2];
            if (std::isfinite (n.start) && std::isfinite (n.end) && std::isfinite (n.pitch) && n.start >= 0.0 && n.end - n.start >= 0.02
                && n.pitch >= 20.0f && n.pitch <= 110.0f)
                result.notes.push_back (n);
        }
    std::stable_sort (result.notes.begin(), result.notes.end(), [] (const coach::TargetNote& a, const coach::TargetNote& b) { return a.start < b.start; });
    // (a note can't run into the next: cut it back)
    for (size_t i = 0; i + 1 < result.notes.size(); ++i)
        result.notes[i].end = juce::jmin (result.notes[i].end, result.notes[i + 1].start);
    result.notes.erase (std::remove_if (result.notes.begin(), result.notes.end(), [] (const coach::TargetNote& n) { return n.end - n.start < 0.02; }),
                        result.notes.end());
    coach::findPhrases (result);
    return result;
}

bool Practice::isMelodyEdited() const
{
    return songFolder.isDirectory() && songFolder.getChildFile (editedMelodyFileName).existsAsFile();
}

// The notes to sing were changed by hand: the song goes on from here (a loop is left first).
void Practice::useMelody (coach::Song edited)
{
    if (loop.active)
        endLoop();
    auto& multitrack = processor.getMultitrack();
    setSong (std::move (edited));
    song = baseSong.transformed (shift.semitones, shift.speed);
    noteScores.assign (song.notes.size(), {});
    phraseScores.assign (song.phrases.size(), {});
    analysis::lyrics::timeWords (lyrics, baseSong);
    restartFrom (multitrack.getPosition() / juce::jmax (1.0, multitrack.getSampleRate()));
    emit ("practice", object ({ { "state", "ready" }, { "song", describeSong() }, { "fromCache", true }, { "quiet", true } }));
}

void Practice::saveLyrics()
{
    if (songFolder == juce::File() || ! songFolder.isDirectory())
        return;
    const auto file = songFolder.getChildFile (lyricsFileName);
    if (lyrics.isEmpty())
        file.deleteFile();
    else
        file.replaceWithText (lyrics.toString());
}

juce::var Practice::describeLyrics() const
{
    // [{ text, start, end, pinned, words: [text, start, ...] }] in the song's time as it plays.
    juce::Array<juce::var> lines;
    for (const auto& line : lyrics.lines)
    {
        juce::Array<juce::var> words;
        for (const auto& w : line.words)
        {
            words.add (w.text);
            words.add (round3 (toPlayingTime (w.start)));
        }
        lines.add (object ({ { "text", line.text }, { "start", round3 (toPlayingTime (line.start)) }, { "end", round3 (toPlayingTime (line.end)) },
                             { "pinned", line.pinned }, { "timed", line.timed }, { "words", words } }));
    }
    return lines;
}
