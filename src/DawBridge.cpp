#include "DawBridge.h"
#include "daw/ClipTools.h"

#include "Presets.h"
#include "dsp/FxModules.h"

#include <numeric>
#include <variant>


namespace
{
    namespace ids = daw::ids;

    constexpr double peakSeconds = 0.02;
    constexpr int autosaveSeconds = 60;
    constexpr double maxImportMinutes = 30.0; // as Multitrack (Live mode's backing tracks)

    juce::var object (std::initializer_list<std::pair<const char*, juce::var>> properties)
    {
        auto* result = new juce::DynamicObject();
        for (const auto& [name, value] : properties)
            result->setProperty (name, value);
        return juce::var (result);
    }

    juce::var error (const juce::String& message)
    {
        return object ({ { "ok", message.isEmpty() }, { "error", message } });
    }

    // A numeric property as a number, whether the tree holds it as one or as text (as
    // a tree read from XML does); 0 if it's missing.
    double number (const juce::ValueTree& tree, const juce::Identifier& name)
    {
        const auto& value = tree[name];
        return value.isString() ? value.toString().getDoubleValue() : (double) value;
    }

    float peakToDecibels (float gain)
    {
        return juce::Decibels::gainToDecibels (gain, -100.0f);
    }

    // args.name, or fallback when it's missing.
    template <typename T>
    T arg (const juce::var& args, const char* name, T fallback)
    {
        return args.hasProperty (name) ? (T) args[name] : fallback;
    }
}

struct DawBridge::ExportJob
{
    std::atomic<double> progress { 0.0 };
    std::atomic<bool> cancel { false }, finished { false };
    daw::Exporter::Result result;
};

struct DawBridge::AutoMixJob
{
    std::atomic<double> progress { 0.0 };
    std::atomic<bool> cancel { false }, finished { false };
    juce::File reference;                     // set for a reference match
    analysis::SessionAnalyser::Result result; // the tracks, or (reference) the mix
    analysis::Profile referenceProfile;
};

struct DawBridge::KeyJob
{
    std::atomic<double> progress { 0.0 };
    std::atomic<bool> cancel { false }, finished { false };
    analysis::SessionAnalyser::Result result;
};

namespace
{
    const char* roleKey (analysis::Role role)
    {
        return role == analysis::Role::voice ? "voice" : role == analysis::Role::backing ? "backing" : "other";
    }

    analysis::Role roleFromKey (const juce::String& key)
    {
        return key == "voice" ? analysis::Role::voice : key == "backing" ? analysis::Role::backing : analysis::Role::other;
    }

    const char* styleKey (analysis::MixStyle style)
    {
        switch (style)
        {
            case analysis::MixStyle::pop:      return "pop";
            case analysis::MixStyle::ballad:   return "ballad";
            case analysis::MixStyle::rock:     return "rock";
            case analysis::MixStyle::coaching: return "coaching";
        }
        return "pop";
    }

    analysis::MixStyle styleFromKey (const juce::String& key)
    {
        for (auto style : { analysis::MixStyle::pop, analysis::MixStyle::ballad, analysis::MixStyle::rock, analysis::MixStyle::coaching })
            if (key == styleKey (style))
                return style;
        return analysis::MixStyle::pop;
    }

    constexpr const char* autoMixUndoName = "Auto Mix";
}

//==============================================================================
DawBridge::DawBridge (PluginProcessor& p, juce::PropertiesFile& s, Emit e, std::function<juce::int64()> latency)
    : processor (p),
      edit (p.getDawEdit()),
      engine (p.getDawEngine()),
      project (p.getDawProject()),
      settings (s),
      emit (std::move (e)),
      roundTripLatency (std::move (latency))
{
    edit.getState().addListener (this);
    project.setAutosaveInterval (autosaveSeconds);

    // Pick up where the app left off: the mode, and the last project if none is open.
    if (isAvailable())
    {
        if (! project.isOpen())
            if (const juce::File last (settings.getValue ("dawProject")); daw::Project::isProjectFolder (last))
                project.open (last);

        if (settings.getBoolValue ("dawMode", false))
            setMode (true);
    }

    engine.setMetronomeVolume ((float) settings.getDoubleValue ("dawClickVolume", 1.0));
}

DawBridge::~DawBridge()
{
    edit.getState().removeListener (this);

    if (exportJob != nullptr)
        exportJob->cancel = true;
    if (autoMixJob != nullptr)
        autoMixJob->cancel = true;
    if (keyJob != nullptr)
        keyJob->cancel = true;
}

bool DawBridge::isAvailable()
{
    if (availableForTesting)
        return true;

   #if JucePlugin_Build_Standalone
    return juce::JUCEApplicationBase::isStandaloneApp();
   #else
    return false;
   #endif
}

void DawBridge::setMode (bool daw)
{
    if (daw && ! isAvailable())
        return;

    if (daw && edit.getTracks().isEmpty())
    {
        // A new session starts with one track, ready to sing on.
        auto track = edit.addTrack ("Vocal");
        track.setProperty (ids::arm, true, nullptr);
        track.setProperty (ids::monitor, true, nullptr);
        edit.getUndoManager().clearUndoHistory();
    }

    if (! daw)
        engine.stop();

    processor.setDawMode (daw);
    settings.setValue ("dawMode", daw);
    sessionChanged = true;
}

void DawBridge::rememberProject()
{
    settings.setValue ("dawProject", project.isOpen() ? project.getFolder().getFullPathName() : juce::String());

    if (! project.isOpen())
        return;

    // Most recent first, at most 8.
    auto recent = juce::StringArray::fromLines (settings.getValue ("dawRecentProjects"));
    recent.removeString (project.getFolder().getFullPathName());
    recent.insert (0, project.getFolder().getFullPathName());
    recent.removeEmptyStrings();
    recent.removeRange (8, recent.size());
    settings.setValue ("dawRecentProjects", recent.joinIntoString ("\n"));
}

juce::var DawBridge::describeRecentProjects() const
{
    juce::Array<juce::var> list;
    for (const auto& path : juce::StringArray::fromLines (settings.getValue ("dawRecentProjects")))
        if (const juce::File folder (path); path.isNotEmpty() && daw::Project::isProjectFolder (folder))
            list.add (object ({ { "name", folder.getFileNameWithoutExtension() }, { "path", path } }));
    return list;
}

juce::File DawBridge::getRecordingFolder() const
{
    return project.isOpen() ? project.getAudioFolder() : Recorder::getDefaultFolder();
}

//==============================================================================
juce::var DawBridge::describeInserts (const juce::ValueTree& owner) const
{
    juce::Array<juce::var> list;

    for (const auto& insert : daw::Edit::getInserts (owner))
    {
        auto* values = new juce::DynamicObject();
        for (int i = 0; i < insert.getNumProperties(); ++i)
            if (const auto name = insert.getPropertyName (i); name != ids::id && name != ids::type)
                values->setProperty (name, number (insert, name));

        list.add (object ({ { "id", (int) insert[ids::id] }, { "type", insert[ids::type].toString() }, { "values", juce::var (values) } }));
    }

    return list;
}

juce::var DawBridge::describeSession()
{
    const auto& state = edit.getState();
    juce::Array<juce::var> tracks;

    for (const auto& track : edit.getTracks())
    {
        juce::Array<juce::var> clips;

        for (const auto& clip : track)
        {
            if (! clip.hasType (ids::CLIP))
                continue;

            const juce::File file (clip[ids::file].toString());
            const auto audio = engine.getAudioPool().get (file);

            // Typed explicitly: the page does arithmetic on these, and text ("0.0") breaks it.
            clips.add (object ({ { "id", (int) clip[ids::id] }, { "name", clip[ids::name].toString() }, { "file", file.getFullPathName() },
                                 { "start", number (clip, ids::start) }, { "offset", number (clip, ids::offset) },
                                 { "length", number (clip, ids::length) }, { "gain", number (clip, ids::gain) },
                                 { "fadeIn", number (clip, ids::fadeIn) }, { "fadeOut", number (clip, ids::fadeOut) },
                                 { "mute", (bool) clip.getProperty (ids::mute, false) },
                                 { "fileLength", audio != nullptr ? (double) audio->getNumSamples() / engine.getSampleRate() : -1.0 },
                                 { "missing", ! file.existsAsFile() } }));
        }

        tracks.add (object ({ { "id", (int) track[ids::id] }, { "name", track[ids::name].toString() }, { "colour", track[ids::colour].toString() },
                              { "volume", number (track, ids::volume) }, { "pan", number (track, ids::pan) },
                              { "inGain", number (track, ids::inGain) }, { "outGain", number (track, ids::outGain) },
                              { "mute", (bool) track[ids::mute] }, { "solo", (bool) track[ids::solo] },
                              { "arm", (bool) track[ids::arm] }, { "monitor", (bool) track[ids::monitor] },
                              { "inserts", describeInserts (track) }, { "clips", clips } }));
    }

    const auto master = edit.getMaster();
    auto& undo = edit.getUndoManager();

    return object ({
        { "available", isAvailable() },
        { "mode", processor.isDawMode() ? "daw" : "live" },
        { "project", object ({ { "open", project.isOpen() }, { "name", project.isOpen() ? project.getName() : juce::String ("Untitled") },
                               { "path", project.getFolder().getFullPathName() }, { "dirty", project.hasUnsavedChanges() },
                               { "recoverable", project.getRecoverableBackup().getFileName() },
                               { "missing", project.getMissingFiles().size() } }) },
        { "tempo", edit.getTempo() },
        { "beatsPerBar", edit.getBeatsPerBar() },
        { "key", object ({ { "key", edit.getKey() }, { "minor", edit.isKeyMinor() },
                           { "name", analysis::KeyEstimate::getName (edit.getKey(), edit.isKeyMinor()) } }) },
        { "markers", [this]
          {
              juce::Array<juce::var> markers;
              for (const auto& m : edit.getMarkers())
                  markers.add (object ({ { "id", (int) m[ids::id] }, { "name", m[ids::name].toString() }, { "time", number (m, ids::time) } }));
              return juce::var (markers);
          }() },
        { "metronomeVolume", engine.getMetronomeVolume() },
        { "recent", describeRecentProjects() },
        { "loop", object ({ { "on", (bool) state.getProperty (ids::loopOn, false) }, { "start", number (state, ids::loopStart) },
                            { "end", number (state, ids::loopEnd) } }) },
        { "metronome", engine.getMetronome() },
        { "countIn", engine.getCountInBars() },
        { "canUndo", undo.canUndo() },
        { "canRedo", undo.canRedo() },
        { "master", object ({ { "volume", number (master, ids::volume) }, { "inGain", number (master, ids::inGain) },
                              { "outGain", number (master, ids::outGain) }, { "inserts", describeInserts (master) } }) },
        { "tracks", tracks },
    });
}

juce::var DawBridge::describeStatus()
{
    const auto rate = engine.getSampleRate();
    auto* peaks = new juce::DynamicObject();

    auto* chains = new juce::DynamicObject();

    const auto addChain = [&] (int id)
    {
        const auto [in, out] = engine.takeChainPeaks (id);
        chains->setProperty (juce::String (id), object ({ { "in", fx::peakToDecibels (in) }, { "out", fx::peakToDecibels (out) } }));
    };

    for (const auto& track : edit.getTracks())
    {
        peaks->setProperty (juce::String ((int) track[ids::id]), peakToDecibels (engine.takePeak ((int) track[ids::id])));
        addChain ((int) track[ids::id]);
    }

    addChain (daw::Edit::masterId);

    // The live readings of the inserts on screen (one track's, or the master's).
    auto* inserts = new juce::DynamicObject();
    if (watchedTrack >= 0)
        engine.forEachInsert (watchedTrack, [&] (int insertId, fx::Module& module)
        {
            inserts->setProperty (juce::String (insertId), fx::describeMeter (module, spectrumAnalyser));
        });

    return object ({
        { "position", (double) engine.getPosition() / rate },
        { "length", (double) engine.getLength() / rate },
        { "playing", engine.isPlaying() },
        { "recording", engine.isRecording() },
        { "recordStart", (double) engine.getRecordStart() / rate },
        { "input", peakToDecibels (processor.inputPeak.exchange (0.0f)) },
        { "master", peakToDecibels (engine.takePeak (daw::Edit::masterId)) },
        { "tracks", juce::var (peaks) },
        { "chains", juce::var (chains) },
        { "inserts", juce::var (inserts) },
        { "loading", engine.getAudioPool().isLoading() },
    });
}

juce::var DawBridge::getPeaks (const juce::File& file)
{
    const auto key = file.getFullPathName();

    if (const auto it = peakCache.find (key); it != peakCache.end())
        return it->second;

    const auto audio = engine.getAudioPool().get (file);
    if (audio == nullptr)
        return {}; // still decoding: the UI asks again

    const auto step = juce::jmax (1, (int) (peakSeconds * engine.getSampleRate()));
    juce::Array<juce::var> peaks;
    peaks.ensureStorageAllocated (audio->getNumSamples() / step + 1);

    for (int i = 0; i < audio->getNumSamples(); i += step)
        peaks.add (std::round (audio->getMagnitude (i, juce::jmin (step, audio->getNumSamples() - i)) * 1000.0f) / 1000.0f);

    auto result = object ({ { "seconds", peakSeconds }, { "peaks", peaks } });
    peakCache[key] = result;
    return result;
}

// Finer peaks for one stretch of a file, for drawing it zoomed in: the signed lowest
// and highest sample (over all channels) of each bin of about `seconds`. Not cached:
// the UI asks only for what is on screen.
juce::var DawBridge::getDetailPeaks (const juce::File& file, double start, double end, double seconds)
{
    const auto audio = engine.getAudioPool().get (file);
    if (audio == nullptr || ! (end > start) || ! (seconds > 0.0))
        return {};

    constexpr int maxBins = 60000;
    const auto rate = engine.getSampleRate();
    const auto total = audio->getNumSamples();
    const auto first = juce::jlimit (0, total, (int) std::floor (start * rate));
    const auto last = juce::jlimit (first, total, (int) std::ceil (end * rate));
    const auto step = juce::jmax (1, (int) std::round (seconds * rate), (last - first) / maxBins + 1);

    juce::Array<juce::var> lows, highs;
    lows.ensureStorageAllocated ((last - first) / step + 1);
    highs.ensureStorageAllocated ((last - first) / step + 1);

    for (int i = first; i < last; i += step)
    {
        const auto count = juce::jmin (step, last - i);
        auto range = juce::Range<float>::emptyRange (0.0f);
        for (int channel = 0; channel < audio->getNumChannels(); ++channel)
        {
            const auto r = audio->findMinMax (channel, i, count);
            range = channel == 0 ? r : range.getUnionWith (r);
        }
        lows.add (std::round (range.getStart() * 10000.0f) / 10000.0f);
        highs.add (std::round (range.getEnd() * 10000.0f) / 10000.0f);
    }

    return object ({ { "start", first / rate }, { "end", last / rate }, { "seconds", step / rate },
                     { "min", lows }, { "max", highs } });
}

//==============================================================================
void DawBridge::tick()
{
    auto& undo = edit.getUndoManager();
    const auto dirty = project.hasUnsavedChanges();

    if (sessionChanged || dirty != lastDirty || undo.canUndo() != lastCanUndo || undo.canRedo() != lastCanRedo)
    {
        sessionChanged = false;
        lastDirty = dirty;
        lastCanUndo = undo.canUndo();
        lastCanRedo = undo.canRedo();
        emit ("dawSession", describeSession());
    }

    if (processor.isDawMode())
    {
        // Stop at the end of the song, unless recording or looping.
        if (engine.isPlaying() && ! engine.isRecording() && engine.getLength() > 0 && engine.getPosition() >= engine.getLength()
            && ! (bool) edit.getState().getProperty (ids::loopOn, false))
            engine.stop();

        emit ("dawStatus", describeStatus());
    }

    if (autoMixJob != nullptr)
    {
        if (autoMixJob->finished)
        {
            auto job = std::move (autoMixJob);

            if (job->result.cancelled)
            {
                emit ("dawAutoMix", object ({ { "state", "cancelled" } }));
            }
            else if (job->reference != juce::File())
            {
                if (! job->result.mix.has_value() || job->referenceProfile.getNumFrames() == 0)
                {
                    emit ("dawAutoMix", object ({ { "state", "error" }, { "error", "Couldn't read " + job->reference.getFileName() + "." } }));
                }
                else
                {
                    autoMixReference = true;
                    autoMixReferenceName = job->reference.getFileNameWithoutExtension();
                    autoMixPlan = analysis::planReferenceMatch (*job->result.mix, job->referenceProfile,
                                                                (float) edit.getMaster().getProperty (ids::volume, 0.0f));
                    emit ("dawAutoMix", describeAutoMix());
                }
            }
            else
            {
                // New analysis: guess the roles of tracks we haven't seen, keep the user's picks.
                autoMixAnalysis = std::move (job->result);
                std::map<int, analysis::Role> roles;
                const auto songSeconds = autoMixAnalysis->tracks.empty() ? 0.0
                                       : autoMixAnalysis->tracks.front().second.getNumFrames() * analysis::Profile::hopSeconds;

                for (const auto& [id, profile] : autoMixAnalysis->tracks)
                {
                    const auto known = autoMixRoles.find (id);
                    roles[id] = known != autoMixRoles.end() ? known->second
                                                            : analysis::guessRole (edit.getTrack (id)[ids::name].toString(), profile, songSeconds);
                }

                autoMixRoles = roles;
                autoMixReference = false;
                replanAutoMix();
                emit ("dawAutoMix", describeAutoMix());
            }
        }
        else
        {
            emit ("dawAutoMix", object ({ { "state", "analysing" }, { "progress", autoMixJob->progress.load() } }));
        }
    }

    if (keyJob != nullptr)
    {
        if (keyJob->finished)
            finishKeyDetection();
        else
            emit ("dawKey", object ({ { "state", "analysing" }, { "progress", keyJob->progress.load() } }));
    }

    if (exportJob != nullptr)
    {
        if (exportJob->finished)
        {
            const auto& result = exportJob->result;
            juce::Array<juce::var> files;
            for (const auto& file : result.files)
                files.add (file.getFullPathName());

            emit ("dawExport", object ({ { "done", true }, { "cancelled", result.cancelled }, { "error", result.error },
                                         { "files", files }, { "peak", result.peakDecibels } }));
            exportJob.reset();
        }
        else
        {
            emit ("dawExport", object ({ { "progress", exportJob->progress.load() } }));
        }
    }
}

//==============================================================================
void DawBridge::withChooser (const juce::String& title, const juce::File& start, const juce::String& patterns, int flags,
                             std::function<juce::var (const juce::File&)> action, Complete complete)
{
    chooser = std::make_unique<juce::FileChooser> (title, start, patterns);
    chooser->launchAsync (flags, [action = std::move (action), complete = std::move (complete)] (const juce::FileChooser& fc)
    {
        const auto file = fc.getResult();

        if (file == juce::File())
            complete (object ({ { "ok", false }, { "cancelled", true } }));
        else
            complete (action (file));
    });
}

//==============================================================================
juce::var DawBridge::openProject (const juce::File& picked)
{
    const auto folder = daw::Project::findProjectFolder (picked);
    if (folder == juce::File())
        return error (picked.getFileName() + " isn't a MicFX project. Pick a .micfx folder, or the project.xml inside it.");

    engine.stop();
    const auto result = project.open (folder);
    peakCache.clear();
    rememberProject();
    sessionChanged = true;
    return error (result);
}

juce::var DawBridge::newProject (const juce::File& folder)
{
    engine.stop();
    auto result = project.createNew (folder);

    if (result.isEmpty())
    {
        setMode (true); // adds the first track
        result = project.save();
    }

    peakCache.clear();
    rememberProject();
    sessionChanged = true;
    return error (result);
}

juce::var DawBridge::saveProjectAs (const juce::File& folder)
{
    auto result = project.saveAs (folder);
    rememberProject();
    sessionChanged = true;
    return error (result);
}

namespace
{
    // The file's length in seconds, from its header (no decoding), or a message saying why
    // it can't be read.
    std::variant<double, juce::String> readAudioLength (const juce::File& file)
    {
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        const std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));

        if (reader == nullptr)
            return juce::String ("Couldn't open " + file.getFileName() + ". Try a WAV, AIFF, FLAC, Ogg or MP3 file.");
        if (reader->sampleRate <= 0.0 || reader->lengthInSamples <= 0)
            return juce::String (file.getFileName() + " is empty.");
        if ((double) reader->lengthInSamples / reader->sampleRate > maxImportMinutes * 60.0)
            return juce::String (file.getFileName() + " is longer than " + juce::String ((int) maxImportMinutes) + " minutes.");

        return (double) reader->lengthInSamples / reader->sampleRate;
    }

    juce::String getAudioFilePatterns()
    {
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        return formats.getWildcardForAllFormats();
    }
}

juce::var DawBridge::importAudio (const juce::File& file, int trackId, double start)
{
    // Check the file before copying it anywhere. Only the header is read: the audio
    // itself loads in the background (AudioPool), so a long song doesn't freeze the app.
    const auto length = readAudioLength (file);
    if (const auto* message = std::get_if<juce::String> (&length))
        return error (*message);

    const auto source = project.isOpen() ? project.importAudio (file) : file;
    if (! source.existsAsFile())
        return error ("Couldn't copy " + file.getFileName() + " into the project's Audio folder.");

    auto& undo = edit.getUndoManager();
    undo.beginNewTransaction ("Import " + file.getFileName());

    if (! edit.getTrack (trackId).isValid())
        trackId = (int) edit.addTrack (file.getFileNameWithoutExtension())[ids::id];

    if (start < 0.0)
        start = (double) engine.getPosition() / engine.getSampleRate();
    start = juce::jmax (0.0, start);
    const auto clip = edit.addClip (trackId, source, start, std::get<double> (length));
    sessionChanged = true;

    if (! clip.isValid())
        return error ("Couldn't add " + file.getFileName() + " to the track.");

    return object ({ { "ok", true }, { "error", juce::String() }, { "track", trackId }, { "clip", clip[ids::id] },
                     { "start", start } });
}

//==============================================================================
namespace
{
    constexpr int maxBrowseEntries = 2000;

    bool isAudioFile (const juce::File& file)
    {
        static const auto extensions = []
        {
            juce::AudioFormatManager formats;
            formats.registerBasicFormats();
            juce::StringArray list;
            for (auto* format : formats)
                for (const auto& extension : format->getFileExtensions())
                    list.addIfNotAlreadyThere (extension.toLowerCase());
            return list;
        }();

        return extensions.contains (file.getFileExtension().toLowerCase());
    }
}

juce::var DawBridge::describePlaces() const
{
    juce::Array<juce::var> list;
    juce::StringArray seen;

    const auto add = [&] (const juce::String& name, const juce::File& folder, const juce::String& kind, bool removable)
    {
        if (! folder.isDirectory() || seen.contains (folder.getFullPathName()))
            return;
        seen.add (folder.getFullPathName());
        list.add (object ({ { "name", name }, { "path", folder.getFullPathName() }, { "kind", kind }, { "removable", removable } }));
    };

    if (project.isOpen())
        add ("This project", project.getAudioFolder(), "project", false);

    add ("Recordings", Recorder::getDefaultFolder(), "recordings", false);
    add ("Music", juce::File::getSpecialLocation (juce::File::userMusicDirectory), "music", false);
    add ("Downloads", juce::File::getSpecialLocation (juce::File::userHomeDirectory).getChildFile ("Downloads"), "downloads", false);
    add ("Desktop", juce::File::getSpecialLocation (juce::File::userDesktopDirectory), "desktop", false);

    for (const auto& path : juce::StringArray::fromLines (settings.getValue ("dawBrowserPlaces")))
        if (juce::File::isAbsolutePath (path))
            add (juce::File (path).getFileName(), juce::File (path), "folder", true);

    return object ({ { "ok", true }, { "error", juce::String() }, { "places", list } });
}

juce::var DawBridge::browseFolder (const juce::File& folder) const
{
    if (! folder.isDirectory())
        return error ("That folder isn't there any more.");

    juce::Array<juce::File> folders, files;
    bool truncated = false;

    for (const auto& entry : juce::RangedDirectoryIterator (folder, false, "*", juce::File::findFilesAndDirectories | juce::File::ignoreHiddenFiles))
    {
        if (folders.size() + files.size() >= maxBrowseEntries) { truncated = true; break; }

        const auto file = entry.getFile();
        if (file.getFileName().startsWithChar ('.'))
            continue;
        if (entry.isDirectory())
            folders.add (file);
        else if (isAudioFile (file))
            files.add (file);
    }

    const auto byName = [] (const juce::File& a, const juce::File& b) { return a.getFileName().compareNatural (b.getFileName()) < 0; };
    std::sort (folders.begin(), folders.end(), byName);
    std::sort (files.begin(), files.end(), byName);

    juce::Array<juce::var> folderList, fileList;
    for (const auto& f : folders)
        folderList.add (object ({ { "name", f.getFileName() }, { "path", f.getFullPathName() } }));
    for (const auto& f : files)
        fileList.add (object ({ { "name", f.getFileName() }, { "path", f.getFullPathName() }, { "size", (double) f.getSize() } }));

    return object ({ { "ok", true }, { "error", juce::String() }, { "path", folder.getFullPathName() }, { "name", folder.getFileName() },
                     { "folders", folderList }, { "files", fileList }, { "truncated", truncated } });
}

void DawBridge::loadPresetInto (int trackId, int presetIndex)
{
    const auto& presets = getPresets();
    if (! juce::isPositiveAndBelow (presetIndex, (int) presets.size()))
        return;

    const auto& preset = presets[(size_t) presetIndex];
    auto inserts = daw::Edit::getInserts (trackId == daw::Edit::masterId ? edit.getMaster() : edit.getTrack (trackId));
    if (! inserts.isValid())
        return;

    // Replace the chain with the preset's switched-on modules, in Live mode order.
    inserts.removeAllChildren (&edit.getUndoManager());

    for (int t = 0; t < fx::numModuleTypes; ++t)
    {
        const auto type = (fx::ModuleType) t;
        auto module = fx::createModule (type);

        // Live mode defaults, then the preset's values.
        for (int v = 0; v < module->getNumValues(); ++v)
            for (const auto& [id, value] : preset.values)
                if (module->getValueId (v) == id)
                    module->getValue (v).store (value);

        // Reverb is on by default but at 0 mix it's inaudible; a preset's reverb counts.
        if (! module->isOn())
            continue;

        const auto insert = edit.addInsert (trackId, type);
        for (int v = 0; v < module->getNumValues(); ++v)
            edit.setInsertValue ((int) insert[ids::id], module->getValueId (v), module->getValue (v).load());
    }
}

void DawBridge::startExport (const juce::var& args, const juce::File& destination)
{
    daw::Exporter::Settings exportSettings;
    exportSettings.sampleRate = engine.getSampleRate();
    exportSettings.bitDepth = arg (args, "bitDepth", 24);
    exportSettings.normalise = arg (args, "normalise", false);
    exportSettings.tail = arg (args, "tail", 2.0);

    if (arg (args, "loopOnly", false) && (bool) edit.getState().getProperty (ids::loopOn, false))
    {
        exportSettings.start = edit.getState()[ids::loopStart];
        exportSettings.end = edit.getState()[ids::loopEnd];
        exportSettings.tail = 0.0;
    }

    auto exporter = std::make_shared<daw::Exporter> (edit, exportSettings); // snapshot on this thread
    auto job = std::make_shared<ExportJob>();
    const auto stems = args["kind"].toString() == "stems";
    exportJob = job;

    juce::Thread::launch ([exporter, job, stems, destination]
    {
        const auto progress = [job] (double p) { job->progress = p; return ! job->cancel.load(); };
        job->result = stems ? exporter->exportStems (destination, progress) : exporter->exportMix (destination, progress);
        job->finished = true;
    });
}

//==============================================================================
void DawBridge::startAutoMix (const juce::File& reference)
{
    auto job = std::make_shared<AutoMixJob>();
    job->reference = reference;

    // Snapshot on this thread. For a reference match only the mix is needed, without
    // a master EQ an earlier match added (the new one replaces it).
    std::shared_ptr<analysis::SessionAnalyser> analyser;
    if (reference == juce::File())
    {
        analyser = std::make_shared<analysis::SessionAnalyser> (edit, analysis::SessionAnalyser::Point::source, false,
                                                                engine.getSampleRate());
    }
    else
    {
        daw::Edit copy;
        copy.replaceState (edit.getState().createCopy());
        auto inserts = daw::Edit::getInserts (copy.getMaster());
        for (int i = inserts.getNumChildren(); --i >= 0;)
            if (inserts.getChild (i)[ids::type].toString() == fx::getModuleKey (fx::ModuleType::paraEq))
            {
                inserts.removeChild (i, nullptr);
                break;
            }

        analyser = std::make_shared<analysis::SessionAnalyser> (copy, analysis::SessionAnalyser::Point::stem, true,
                                                                engine.getSampleRate(), false);
    }

    autoMixJob = job;

    juce::Thread::launch ([analyser, job]
    {
        const auto reading = job->reference != juce::File();
        const auto progress = [job, reading] (double p) { job->progress = reading ? 0.5 * p : p; return ! job->cancel.load(); };
        job->result = analyser->run (progress);

        if (reading && ! job->result.cancelled)
        {
            job->progress = 0.6;
            job->referenceProfile = analysis::analyseFile (job->reference, [job] { return ! job->cancel.load(); });
            job->result.cancelled = job->cancel.load();
        }

        job->finished = true;
    });
}

void DawBridge::replanAutoMix()
{
    if (! autoMixAnalysis.has_value())
        return;

    std::map<int, float> volumes;
    for (const auto& track : edit.getTracks())
        volumes[(int) track[ids::id]] = track.getProperty (ids::volume, 0.0f);

    autoMixPlan = analysis::planMix (*autoMixAnalysis, autoMixRoles, volumes,
                                     (float) edit.getMaster().getProperty (ids::volume, 0.0f), autoMixStyle);
}

juce::var DawBridge::describeAutoMix() const
{
    const auto nameOf = [this] (int trackId)
    {
        return trackId == daw::Edit::masterId ? juce::String ("Master") : edit.getTrack (trackId)[ids::name].toString();
    };

    juce::Array<juce::var> tracks, moves, notes;

    if (autoMixAnalysis.has_value() && ! autoMixReference)
        for (const auto& [id, profile] : autoMixAnalysis->tracks)
        {
            if (! edit.getTrack (id).isValid())
                continue;

            const auto role = autoMixRoles.count (id) > 0 ? autoMixRoles.at (id) : analysis::Role::other;
            tracks.add (object ({ { "id", id }, { "name", nameOf (id) }, { "role", roleKey (role) },
                                  { "lufs", profile.integratedLufs }, { "active", profile.getActiveSeconds() } }));
        }

    for (const auto& move : autoMixPlan.moves)
    {
        juce::String effect = "Volume";
        if (move.insert.has_value())
            effect = *move.insert == fx::ModuleType::paraEq     ? "Parametric EQ"
                   : *move.insert == fx::ModuleType::compressor ? "Compressor"
                   : *move.insert == fx::ModuleType::deEsser    ? "De-esser"
                                                                : juce::String (fx::getModuleKey (*move.insert));

        juce::Array<juce::var> reasons;
        for (const auto& reason : move.reasons)
            reasons.add (reason);

        moves.add (object ({ { "track", move.trackId }, { "name", nameOf (move.trackId) }, { "effect", effect }, { "reasons", reasons } }));
    }

    for (const auto& note : autoMixPlan.notes)
        notes.add (note);

    return object ({ { "state", "ready" }, { "kind", autoMixReference ? "reference" : "mix" }, { "reference", autoMixReferenceName },
                     { "style", styleKey (autoMixStyle) }, { "tracks", tracks }, { "moves", moves }, { "notes", notes },
                     { "voiceOverBackingLu", autoMixPlan.voiceOverBackingLu }, { "mixLufs", autoMixPlan.mixLufs } });
}

//==============================================================================
// The "key" command; args.action is one of:
//   detect      listen to every track (background); "dawKey" brings the result, which
//               also becomes the session's key (one undo step)
//   cancel
//   get         the last result (describeKey)
//   set {key, minor}   pick the key by hand (key -1 forgets it)
//   autotune    set every autotune insert in the session to the key -> { ok, count }
juce::var DawBridge::handleKey (const juce::var& args)
{
    const auto action = args["action"].toString();

    if (action == "detect")
    {
        if (keyJob != nullptr)
            return error ("Already listening for the key.");
        if (edit.getTracks().isEmpty())
            return error ("Add a track with some music first.");

        auto job = std::make_shared<KeyJob>();
        auto analyser = std::make_shared<analysis::SessionAnalyser> (edit, analysis::SessionAnalyser::Point::source, false,
                                                                     engine.getSampleRate());
        keyJob = job;

        juce::Thread::launch ([analyser, job]
        {
            job->result = analyser->run ([job] (double p) { job->progress = p; return ! job->cancel.load(); });
            job->finished = true;
        });

        emit ("dawKey", object ({ { "state", "analysing" }, { "progress", 0.0 } }));
        return error ({});
    }

    if (action == "cancel")
    {
        if (keyJob != nullptr)
            keyJob->cancel = true;
        return error ({});
    }

    if (action == "get")
        return describeKey();

    if (action == "set")
    {
        edit.setKey (arg (args, "key", -1), arg (args, "minor", false));
        return error ({});
    }

    if (action == "autotune")
    {
        if (edit.getKey() < 0)
            return error ("Detect or pick the song's key first.");

        auto result = error ({});
        result.getDynamicObject()->setProperty ("count", setAutotunesToKey());
        return result;
    }

    return error ("Unknown key action: " + action);
}

void DawBridge::finishKeyDetection()
{
    auto job = std::move (keyJob);

    if (job->result.cancelled)
    {
        emit ("dawKey", object ({ { "state", "cancelled" } }));
        return;
    }

    // The harmony comes from the instruments: leave out the voices (whose melody may be
    // the very thing that's off key), unless there's nothing else.
    const auto& tracks = job->result.tracks;
    const auto songSeconds = tracks.empty() ? 0.0 : tracks.front().second.getNumFrames() * analysis::Profile::hopSeconds;

    analysis::Chroma instruments {}, everything {};
    double instrumentWeight = 0.0;
    juce::Array<juce::var> described;
    std::vector<int> used;

    for (const auto& [id, profile] : tracks)
    {
        const auto track = edit.getTrack (id);
        if (! track.isValid())
            continue;

        const auto known = autoMixRoles.find (id);
        const auto role = known != autoMixRoles.end() ? known->second
                                                      : analysis::guessRole (track[ids::name].toString(), profile, songSeconds);
        const auto sum = profile.getChromaSum();
        const auto weight = std::accumulate (sum.begin(), sum.end(), 0.0);

        analysis::addChroma (everything, sum);
        if (role != analysis::Role::voice)
        {
            analysis::addChroma (instruments, sum);
            instrumentWeight += weight;
        }

        described.add (object ({ { "id", id }, { "name", track[ids::name].toString() },
                                 { "key", weight > 0.0 ? analysis::estimateKey (sum).getName() : juce::String() },
                                 { "voice", role == analysis::Role::voice } }));
    }

    const auto useInstruments = instrumentWeight > 0.0;
    lastKey = analysis::estimateKey (useInstruments ? instruments : everything);
    for (auto& track : described)
        track.getDynamicObject()->setProperty ("used", track["key"].toString().isNotEmpty() && (! useInstruments || ! (bool) track["voice"]));
    lastKeyTracks = described;
    keyDetected = true;

    if (lastKey.isValid())
    {
        edit.getUndoManager().beginNewTransaction ("Detect key");
        edit.setKey (lastKey.key, lastKey.minor);
    }

    emit ("dawKey", describeKey());
}

juce::var DawBridge::describeKey() const
{
    if (! keyDetected)
        return object ({ { "state", "none" } });

    juce::Array<juce::var> chroma;
    for (auto value : lastKey.chroma)
        chroma.add (value);

    return object ({ { "state", "ready" }, { "key", lastKey.key }, { "minor", lastKey.minor }, { "name", lastKey.getName() },
                     { "confidence", lastKey.confidence },
                     { "runnerUp", analysis::KeyEstimate::getName (lastKey.runnerUpKey, lastKey.runnerUpMinor) },
                     { "tuningCents", lastKey.tuningCents }, { "chroma", chroma }, { "tracks", lastKeyTracks } });
}

int DawBridge::setAutotunesToKey()
{
    const auto tonic = edit.getKey();
    const auto minor = edit.isKeyMinor();
    if (tonic < 0)
        return 0;

    // Keep a scale that already fits the mode (a minor pentatonic for a minor song);
    // anything else becomes plain Major or Minor.
    const auto& names = fx::PitchEngine::getScaleNames();
    const juce::StringArray minorScales { "Minor", "Harmonic Minor", "Minor Pentatonic", "Blues" };
    const juce::StringArray majorScales { "Major", "Major Pentatonic" };
    const auto& fitting = minor ? minorScales : majorScales;

    edit.getUndoManager().beginNewTransaction ("Autotune to the song's key");
    int count = 0;

    auto owners = edit.getTracks();
    owners.add (edit.getMaster());

    for (const auto& owner : owners)
    {
        const auto inserts = daw::Edit::getInserts (owner);
        for (int i = 0; i < inserts.getNumChildren(); ++i)
        {
            const auto insert = inserts.getChild (i);
            if (insert[ids::type].toString() != fx::getModuleKey (fx::ModuleType::pitch))
                continue;

            const auto id = (int) insert[ids::id];
            const auto scale = names[juce::roundToInt (number (insert, "tuneScale"))];
            edit.setInsertValue (id, "tuneKey", (float) tonic);
            if (! fitting.contains (scale))
                edit.setInsertValue (id, "tuneScale", (float) names.indexOf (minor ? "Minor" : "Major"));
            ++count;
        }
    }

    return count;
}

// The "autoMix" command; args.action is one of:
//   analyse {style}     listen to every track (background); "dawAutoMix" brings the plan
//   reference           pick a song and plan a master match to it (background)
//   replan {style, roles: {trackId: "voice" | "backing" | "other"}}
//   apply {amount, replace}  write the plan as one undo step ("Auto Mix"); with replace,
//                       first undo the Auto Mix just applied (to change the amount)
//   compare {after}     undo or redo the last Auto Mix, to hear before and after
//   cancel
juce::var DawBridge::handleAutoMix (const juce::var& args, Complete& complete)
{
    auto& undo = edit.getUndoManager();
    const auto action = args["action"].toString();

    if (action == "cancel")
    {
        if (autoMixJob != nullptr)
            autoMixJob->cancel = true;
        return error ({});
    }

    if (action == "analyse" || action == "reference")
    {
        if (autoMixJob != nullptr)
            return error ("Auto Mix is already listening.");
        if (edit.getTracks().isEmpty() || engine.getLength() <= 0)
            return error ("Record or import something first.");

        if (args.hasProperty ("style"))
            autoMixStyle = styleFromKey (args["style"].toString());

        if (action == "analyse")
        {
            startAutoMix ({});
            return error ({});
        }

        withChooser ("Pick a reference song", juce::File::getSpecialLocation (juce::File::userMusicDirectory),
                     "*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3;*.m4a",
                     juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                     [this] (const juce::File& file)
                     {
                         startAutoMix (file);
                         return error ({});
                     },
                     complete);
        return {}; // the chooser completes
    }

    if (action == "replan")
    {
        if (args.hasProperty ("style"))
            autoMixStyle = styleFromKey (args["style"].toString());

        if (auto* roles = args["roles"].getDynamicObject())
            for (const auto& property : roles->getProperties())
                autoMixRoles[property.name.toString().getIntValue()] = roleFromKey (property.value.toString());

        autoMixReference = false;
        replanAutoMix();
        return describeAutoMix();
    }

    if (action == "apply")
    {
        if (autoMixPlan.moves.empty())
            return error ("Nothing to apply yet.");

        if (arg (args, "replace", false) && undo.getUndoDescription() == autoMixUndoName)
            undo.undo();

        analysis::applyMix (edit, autoMixPlan, arg (args, "amount", 1.0f));
        return error ({});
    }

    if (action == "compare")
    {
        const auto after = arg (args, "after", true);
        if (after && undo.getRedoDescription() == autoMixUndoName)
            undo.redo();
        else if (! after && undo.getUndoDescription() == autoMixUndoName)
            undo.undo();

        return object ({ { "ok", true }, { "after", undo.getUndoDescription() == autoMixUndoName } });
    }

    return error ("Unknown Auto Mix action: " + action);
}

void DawBridge::handle (const juce::String& command, const juce::var& args, Complete complete)
{
    // Failures show in the page as a toast; log them too, so they're in the terminal.
    complete = [command, inner = std::move (complete)] (juce::var result)
    {
        if (result.isObject() && ! (bool) result.getProperty ("ok", true) && result["error"].toString().isNotEmpty())
            juce::Logger::writeToLog ("[MicFX DAW] " + command + ": " + result["error"].toString());
        inner (std::move (result));
    };

    auto& undo = edit.getUndoManager();

    // The track (or master, 0) whose inserts are on screen gets live readings in
    // "dawStatus" (-1: none). Not an edit.
    if (command == "watchInserts") { watchedTrack = arg (args, "track", -1); complete (object ({ { "ok", true } })); return; }

    // Each command is one undo step, unless it continues a drag (args.continue).
    if (! arg (args, "continue", false))
        undo.beginNewTransaction (command);

    const auto id = arg (args, "id", -1);
    const auto ok = [&] { complete (error ({})); };

    if (command == "session")               { complete (describeSession()); return; }
    if (command == "mode")                  { setMode (arg (args, "daw", false)); ok(); return; }
    if (command == "undo")                  { undo.undo(); ok(); return; }
    if (command == "redo")                  { undo.redo(); ok(); return; }
    if (command == "peaks")
    {
        const juce::File file (args["file"].toString());
        complete (args.hasProperty ("seconds") ? getDetailPeaks (file, arg (args, "start", 0.0), arg (args, "end", 0.0), arg (args, "seconds", 0.0))
                                               : getPeaks (file));
        return;
    }

    //================================================================ tracks
    if (command == "addTrack")
    {
        const auto track = edit.addTrack (arg (args, "name", juce::String ("Track ") + juce::String (edit.getTracks().size() + 1)));
        complete (track[ids::id]);
        return;
    }

    if (command == "duplicateTrack") { complete (edit.duplicateTrack (id)); return; }
    if (command == "pasteInserts")   { edit.copyInserts (arg (args, "from", -1), arg (args, "to", -1)); ok(); return; }
    if (command == "soloExclusive")  { edit.soloExclusive (id); ok(); return; }
    if (command == "clearMuteSolo")  { edit.clearMuteAndSolo(); ok(); return; }
    if (command == "removeTrack") { edit.removeTrack (id); ok(); return; }
    if (command == "moveTrack")   { edit.moveTrack (id, arg (args, "index", 0)); ok(); return; }

    if (command == "setTrack")
    {
        auto track = edit.getTrack (id);

        if (track.isValid())
        {
            if (args.hasProperty ("name"))    track.setProperty (ids::name, args["name"].toString().substring (0, 60), &undo);
            if (args.hasProperty ("colour"))  track.setProperty (ids::colour, args["colour"], &undo);
            if (args.hasProperty ("volume"))  edit.setTrackVolume (id, args["volume"]);
            if (args.hasProperty ("pan"))     edit.setTrackPan (id, args["pan"]);
            if (args.hasProperty ("mute"))    edit.setTrackMute (id, args["mute"]);
            if (args.hasProperty ("solo"))    edit.setTrackSolo (id, args["solo"]);
            if (args.hasProperty ("arm"))     edit.setTrackArm (id, args["arm"]);
            if (args.hasProperty ("monitor")) edit.setTrackMonitor (id, args["monitor"]);
            if (args.hasProperty ("inGain"))  edit.setChainGain (id, false, args["inGain"]);
            if (args.hasProperty ("outGain")) edit.setChainGain (id, true, args["outGain"]);
        }

        ok();
        return;
    }

    if (command == "setMaster")
    {
        if (args.hasProperty ("volume"))  edit.setMasterVolume (args["volume"]);
        if (args.hasProperty ("inGain"))  edit.setChainGain (daw::Edit::masterId, false, args["inGain"]);
        if (args.hasProperty ("outGain")) edit.setChainGain (daw::Edit::masterId, true, args["outGain"]);
        ok();
        return;
    }

    //================================================================ inserts
    if (command == "addInsert")
    {
        const auto type = daw::Edit::moduleTypeFromKey (args["type"].toString());
        if (! type.has_value())
        {
            complete (-1);
            return;
        }

        const auto insert = edit.addInsert (arg (args, "track", -1), *type, arg (args, "index", -1));

        // A new autotune starts in the song's key, when the session knows it.
        if (insert.isValid() && *type == fx::ModuleType::pitch && edit.getKey() >= 0)
        {
            edit.setInsertValue (insert[ids::id], "tuneKey", (float) edit.getKey());
            edit.setInsertValue (insert[ids::id], "tuneScale", edit.isKeyMinor() ? 2.0f : 1.0f);
        }

        complete (insert[ids::id]);
        return;
    }

    if (command == "removeInsert") { edit.removeInsert (id); ok(); return; }
    if (command == "moveInsert")   { edit.moveInsert (id, arg (args, "index", 0)); ok(); return; }
    if (command == "setInsert")    { edit.setInsertValue (id, args["param"].toString(), args["value"]); ok(); return; }
    if (command == "loadPreset")   { loadPresetInto (arg (args, "track", -1), arg (args, "preset", -1)); ok(); return; }

    //================================================================ clips
    if (command == "moveClip")   { edit.moveClip (id, args["start"], arg (args, "track", -1)); ok(); return; }
    if (command == "trimStart")  { edit.trimClipStart (id, args["start"]); ok(); return; }
    if (command == "trimEnd")    { edit.trimClipEnd (id, args["end"]); ok(); return; }
    if (command == "split")      { complete (edit.splitClip (id, args["time"])); return; }
    if (command == "duplicate")  { complete (edit.duplicateClip (id)); return; }
    if (command == "removeClip") { edit.removeClip (id); ok(); return; }

    // A copy of a clip (from copy/cut in the UI) at a new place: { track, start, file, offset,
    // length, gain, fadeIn, fadeOut, name } -> the new clip's id, or -1.
    if (command == "pasteClip")
    {
        const juce::File file (args["file"].toString());
        auto clip = edit.addClip (arg (args, "track", -1), file, juce::jmax (0.0, (double) args["start"]),
                                  args["length"], args["offset"]);
        if (! clip.isValid())
        {
            complete (-1);
            return;
        }

        if (args.hasProperty ("name"))
            clip.setProperty (ids::name, args["name"], &undo);
        edit.setClipGain ((int) clip[ids::id], arg (args, "gain", 0.0f));
        edit.setClipFades ((int) clip[ids::id], arg (args, "fadeIn", 0.0), arg (args, "fadeOut", 0.0));
        complete (clip[ids::id]);
        return;
    }
    if (command == "clipGain")   { edit.setClipGain (id, args["gain"]); ok(); return; }
    if (command == "setClip")
    {
        if (args.hasProperty ("name")) edit.setClipName (id, args["name"].toString());
        if (args.hasProperty ("mute")) edit.setClipMute (id, args["mute"]);
        ok();
        return;
    }

    // Clip commands that read the clip's audio: normalise (gain so the peak is -1 dBFS)
    // and strip silence (keep only the parts with sound, as separate clips).
    if (command == "normaliseClip" || command == "stripSilence")
    {
        const auto clip = edit.getClip (id);
        const auto audio = clip.isValid() ? engine.getAudioPool().get (juce::File (clip[ids::file].toString())) : nullptr;
        if (audio == nullptr)
        {
            complete (error (clip.isValid() ? "The clip's audio is still loading. Try again in a moment." : "Select a clip first."));
            return;
        }

        const auto rate = engine.getAudioPool().getSampleRate();
        const auto offset = (juce::int64) std::llround ((double) clip[ids::offset] * rate);
        const auto length = (juce::int64) std::llround ((double) clip[ids::length] * rate);

        if (command == "normaliseClip")
        {
            if (daw::cliptools::getPeak (*audio, offset, length) < 1.0e-5f)
            {
                complete (error ("The clip is silent."));
                return;
            }

            const auto gain = daw::cliptools::getNormaliseGain (*audio, offset, length, arg (args, "target", -1.0f));
            edit.setClipGain (id, gain);
            complete (object ({ { "ok", true }, { "gain", gain } }));
            return;
        }

        const auto regions = daw::cliptools::findSound (*audio, offset, length, rate, arg (args, "threshold", -45.0f));
        if (regions.empty())
        {
            complete (error ("No sound above the threshold in this clip."));
            return;
        }

        std::vector<std::pair<double, double>> parts;
        for (const auto& r : regions)
            parts.emplace_back (r.start, r.end);

        const auto created = edit.keepClipParts (id, parts);
        complete (object ({ { "ok", true }, { "clips", created.size() } }));
        return;
    }
    if (command == "clipFades")  { edit.setClipFades (id, args["fadeIn"], args["fadeOut"]); ok(); return; }

    //================================================================ timeline and transport
    if (command == "tempo")     { edit.setTempo (args["bpm"]); ok(); return; }
    if (command == "loop")      { edit.setLoop (args["on"], args["start"], args["end"]); ok(); return; }
    if (command == "metronome") { engine.setMetronome (args["on"]); sessionChanged = true; ok(); return; }
    if (command == "countIn")   { engine.setCountInBars (args["bars"]); sessionChanged = true; ok(); return; }
    if (command == "beatsPerBar") { edit.setBeatsPerBar (args["beats"]); ok(); return; }
    if (command == "metronomeVolume")
    {
        engine.setMetronomeVolume (args["volume"]);
        settings.setValue ("dawClickVolume", engine.getMetronomeVolume());
        sessionChanged = true;
        ok();
        return;
    }

    if (command == "addMarker")    { complete (edit.addMarker (args["time"], args["name"].toString())[ids::id]); return; }
    if (command == "removeMarker") { edit.removeMarker (id); ok(); return; }
    if (command == "setMarker")
    {
        if (args.hasProperty ("time")) edit.setMarkerTime (id, args["time"]);
        if (args.hasProperty ("name")) edit.setMarkerName (id, args["name"].toString());
        ok();
        return;
    }

    if (command == "transport")
    {
        const auto action = args["action"].toString();
        const auto rate = engine.getSampleRate();

        if (action == "play")
        {
            if (engine.getLength() > 0 && engine.getPosition() >= engine.getLength())
                engine.seek (0);
            engine.play();
        }
        else if (action == "pause") { engine.stop(); }
        else if (action == "stop")  { engine.stop(); engine.seek (0); }
        else if (action == "seek")  { engine.seek ((juce::int64) ((double) args["time"] * rate)); }

        ok();
        return;
    }

    if (command == "record")
    {
        if (arg (args, "on", false))
        {
            engine.setRecordingLatency (roundTripLatency());
            complete (error (engine.startRecording (getRecordingFolder())));
        }
        else
        {
            const auto take = engine.stopRecording();
            complete (object ({ { "ok", true }, { "file", take.file.getFullPathName() }, { "dropped", take.droppedAudio } }));
        }

        return;
    }

    //================================================================ files
    const auto documents = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory);
    const auto projectsFolder = project.isOpen() ? project.getFolder().getParentDirectory() : documents.getChildFile ("MicFX Projects");
    projectsFolder.createDirectory();

    using Flags = juce::FileBrowserComponent::FileChooserFlags;
    const auto saveFlags = Flags::saveMode | Flags::canSelectFiles | Flags::warnAboutOverwriting;

    if (command == "new" || command == "saveAs" || (command == "save" && ! project.isOpen()))
    {
        const auto isNew = command == "new";
        const auto name = isNew || ! project.isOpen() ? juce::String ("New Song") : project.getName();
        withChooser (isNew ? "New project" : "Save project as", projectsFolder.getChildFile (name), {},
                     Flags::saveMode | Flags::canSelectFiles,
                     [this, isNew] (const juce::File& file) { return isNew ? newProject (file) : saveProjectAs (file); },
                     complete);
        return;
    }

    if (command == "save")
    {
        const auto result = project.save();
        sessionChanged = true;
        complete (error (result));
        return;
    }

    if (command == "open")
    {
        // A project is a folder. Both the folder and the project.xml inside it can be picked
        // ("*.xml": macOS filters by extension only, so "project.xml" would grey it out).
        withChooser ("Open project", projectsFolder, "*.micfx;*.xml", Flags::openMode | Flags::canSelectFiles | Flags::canSelectDirectories,
                     [this] (const juce::File& picked) { return openProject (picked); },
                     complete);
        return;
    }

    if (command == "openRecent")
    {
        const juce::File folder (args["path"].toString());
        if (! daw::Project::isProjectFolder (folder))
        {
            complete (error ("That project isn't there any more."));
            return;
        }

        engine.stop();
        const auto result = project.open (folder);
        peakCache.clear();
        rememberProject();
        sessionChanged = true;
        complete (error (result));
        return;
    }

    if (command == "restore")
    {
        const auto result = project.restoreBackup (project.getRecoverableBackup());
        sessionChanged = true;
        complete (error (result));
        return;
    }

    if (command == "dismissRestore")
    {
        project.save(); // the saved version wins; the backup stays in Backups/
        sessionChanged = true;
        ok();
        return;
    }

    if (command == "import")
    {
        const auto trackId = arg (args, "track", -1);
        withChooser ("Import audio", juce::File::getSpecialLocation (juce::File::userMusicDirectory), getAudioFilePatterns(),
                     Flags::openMode | Flags::canSelectFiles,
                     [this, trackId] (const juce::File& file) { return importAudio (file, trackId); },
                     complete);
        return;
    }

    //================================================================ library sidebar
    if (command == "places") { complete (describePlaces()); return; }
    if (command == "browse") { complete (browseFolder (juce::File (args["path"].toString()))); return; }

    if (command == "importFile")
    {
        const juce::File file (args["file"].toString());
        complete (juce::File::isAbsolutePath (args["file"].toString()) && file.existsAsFile()
                      ? importAudio (file, arg (args, "track", -1), arg (args, "start", -1.0))
                      : error ("That file isn't there any more."));
        return;
    }

    if (command == "addPlace")
    {
        withChooser ("Add a folder to the library", juce::File::getSpecialLocation (juce::File::userMusicDirectory), {},
                     Flags::openMode | Flags::canSelectDirectories,
                     [this] (const juce::File& folder)
                     {
                         if (! folder.isDirectory())
                             return error (folder.getFileName() + " isn't a folder.");
                         auto places = juce::StringArray::fromLines (settings.getValue ("dawBrowserPlaces"));
                         places.removeEmptyStrings();
                         places.addIfNotAlreadyThere (folder.getFullPathName());
                         settings.setValue ("dawBrowserPlaces", places.joinIntoString ("\n"));
                         auto result = describePlaces();
                         result.getDynamicObject()->setProperty ("added", folder.getFullPathName());
                         return result;
                     },
                     complete);
        return;
    }

    if (command == "removePlace")
    {
        auto places = juce::StringArray::fromLines (settings.getValue ("dawBrowserPlaces"));
        places.removeString (args["path"].toString());
        places.removeEmptyStrings();
        settings.setValue ("dawBrowserPlaces", places.joinIntoString ("\n"));
        complete (describePlaces());
        return;
    }

    if (command == "export")
    {
        if (exportJob != nullptr)
        {
            complete (error ("An export is already running."));
            return;
        }

        const auto stems = args["kind"].toString() == "stems";
        const auto exports = project.isOpen() ? project.getExportsFolder() : documents;
        const auto name = project.isOpen() ? project.getName() : juce::String ("Mix");

        withChooser (stems ? "Export stems to a folder" : "Export the mix",
                     stems ? exports.getChildFile (name + " stems") : exports.getChildFile (name + ".wav"),
                     stems ? juce::String() : "*.wav", saveFlags,
                     [this, args, stems] (const juce::File& file)
                     {
                         startExport (args, stems ? file : file.withFileExtension ("wav"));
                         return error ({});
                     },
                     complete);
        return;
    }

    if (command == "key")
    {
        complete (handleKey (args));
        return;
    }

    if (command == "autoMix")
    {
        if (auto result = handleAutoMix (args, complete); ! result.isVoid())
            complete (result);
        return;
    }

    if (command == "cancelExport")
    {
        if (exportJob != nullptr)
            exportJob->cancel = true;
        ok();
        return;
    }

    if (command == "reveal")
    {
        const juce::File file (args["path"].toString());
        if (file.exists())
            file.revealToUser();
        else if (project.isOpen())
            project.getFolder().revealToUser();
        ok();
        return;
    }

    complete (error ("Unknown command: " + command));
}
