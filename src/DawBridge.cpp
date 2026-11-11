#include "DawBridge.h"
#include "daw/ClipTools.h"

#include "Presets.h"
#include "dsp/FxModules.h"


namespace
{
    namespace ids = daw::ids;

    constexpr double peakSeconds = 0.02;
    constexpr int autosaveSeconds = 60;

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
}

bool DawBridge::isAvailable()
{
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
                values->setProperty (name, insert[name]);

        list.add (object ({ { "id", insert[ids::id] }, { "type", insert[ids::type] }, { "values", juce::var (values) } }));
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

            clips.add (object ({ { "id", clip[ids::id] }, { "name", clip[ids::name] }, { "file", file.getFullPathName() },
                                 { "start", clip[ids::start] }, { "offset", clip[ids::offset] }, { "length", clip[ids::length] },
                                 { "gain", clip[ids::gain] }, { "fadeIn", clip[ids::fadeIn] }, { "fadeOut", clip[ids::fadeOut] },
                                 { "mute", clip.getProperty (ids::mute, false) },
                                 { "fileLength", audio != nullptr ? (double) audio->getNumSamples() / engine.getSampleRate() : -1.0 },
                                 { "missing", ! file.existsAsFile() } }));
        }

        tracks.add (object ({ { "id", track[ids::id] }, { "name", track[ids::name] }, { "colour", track[ids::colour] },
                              { "volume", track[ids::volume] }, { "pan", track[ids::pan] }, { "mute", track[ids::mute] },
                              { "solo", track[ids::solo] }, { "arm", track[ids::arm] }, { "monitor", track[ids::monitor] },
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
        { "markers", [this]
          {
              juce::Array<juce::var> markers;
              for (const auto& m : edit.getMarkers())
                  markers.add (object ({ { "id", m[ids::id] }, { "name", m[ids::name] }, { "time", m[ids::time] } }));
              return juce::var (markers);
          }() },
        { "metronomeVolume", engine.getMetronomeVolume() },
        { "recent", describeRecentProjects() },
        { "loop", object ({ { "on", state.getProperty (ids::loopOn, false) }, { "start", state.getProperty (ids::loopStart, 0.0) },
                            { "end", state.getProperty (ids::loopEnd, 0.0) } }) },
        { "metronome", engine.getMetronome() },
        { "countIn", engine.getCountInBars() },
        { "canUndo", undo.canUndo() },
        { "canRedo", undo.canRedo() },
        { "master", object ({ { "volume", master[ids::volume] }, { "inserts", describeInserts (master) } }) },
        { "tracks", tracks },
    });
}

juce::var DawBridge::describeStatus()
{
    const auto rate = engine.getSampleRate();
    auto* peaks = new juce::DynamicObject();

    for (const auto& track : edit.getTracks())
        peaks->setProperty (juce::String ((int) track[ids::id]), peakToDecibels (engine.takePeak ((int) track[ids::id])));

    return object ({
        { "position", (double) engine.getPosition() / rate },
        { "length", (double) engine.getLength() / rate },
        { "playing", engine.isPlaying() },
        { "recording", engine.isRecording() },
        { "recordStart", (double) engine.getRecordStart() / rate },
        { "input", peakToDecibels (processor.inputPeak.exchange (0.0f)) },
        { "master", peakToDecibels (engine.takePeak (daw::Edit::masterId)) },
        { "tracks", juce::var (peaks) },
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
juce::String DawBridge::withChooser (const juce::String& title, const juce::File& start, const juce::String& patterns, int flags,
                                     std::function<juce::String (const juce::File&)> action, Complete complete)
{
    chooser = std::make_unique<juce::FileChooser> (title, start, patterns);
    chooser->launchAsync (flags, [action = std::move (action), complete = std::move (complete)] (const juce::FileChooser& fc)
    {
        const auto file = fc.getResult();

        if (file == juce::File())
            complete (object ({ { "ok", false }, { "cancelled", true } }));
        else
            complete (error (action (file)));
    });

    return {};
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
                         return juce::String();
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
    auto& undo = edit.getUndoManager();

    // Each command is one undo step, unless it continues a drag (args.continue).
    if (! arg (args, "continue", false))
        undo.beginNewTransaction (command);

    const auto id = arg (args, "id", -1);
    const auto ok = [&] { complete (error ({})); };

    if (command == "session")               { complete (describeSession()); return; }
    if (command == "mode")                  { setMode (arg (args, "daw", false)); ok(); return; }
    if (command == "undo")                  { undo.undo(); ok(); return; }
    if (command == "redo")                  { undo.redo(); ok(); return; }
    if (command == "peaks")                 { complete (getPeaks (juce::File (args["file"].toString()))); return; }

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
        }

        ok();
        return;
    }

    if (command == "setMaster") { edit.setMasterVolume (arg (args, "volume", 0.0f)); ok(); return; }

    //================================================================ inserts
    if (command == "addInsert")
    {
        const auto type = daw::Edit::moduleTypeFromKey (args["type"].toString());
        complete (type.has_value() ? edit.addInsert (arg (args, "track", -1), *type, arg (args, "index", -1))[ids::id] : juce::var (-1));
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
        withChooser (isNew ? "New project" : "Save project as", projectsFolder.getChildFile (isNew ? "New Song" : project.getName()), {},
                     Flags::saveMode | Flags::canSelectFiles,
                     [this, isNew] (const juce::File& file)
                     {
                         engine.stop();
                         const auto result = isNew ? project.createNew (file) : project.saveAs (file);
                         if (result.isEmpty() && isNew)
                             setMode (true); // adds the first track
                         if (result.isEmpty())
                             project.save();
                         rememberProject();
                         sessionChanged = true;
                         return result;
                     },
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
        withChooser ("Open project", projectsFolder, "*.micfx;project.xml", Flags::openMode | Flags::canSelectFiles | Flags::canSelectDirectories,
                     [this] (const juce::File& picked)
                     {
                         const auto folder = picked.existsAsFile() ? picked.getParentDirectory() : picked;
                         engine.stop();
                         const auto result = project.open (folder);
                         peakCache.clear();
                         rememberProject();
                         sessionChanged = true;
                         return result;
                     },
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
        withChooser ("Import audio", juce::File::getSpecialLocation (juce::File::userMusicDirectory), "*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3",
                     Flags::openMode | Flags::canSelectFiles,
                     [this, args] (const juce::File& file)
                     {
                         const auto source = project.isOpen() ? project.importAudio (file) : file;
                         if (! source.existsAsFile())
                             return juce::String ("Couldn't import " + file.getFileName() + ".");

                         auto decoded = Multitrack::decode (source, engine.getSampleRate());
                         if (decoded.error.isNotEmpty())
                             return decoded.error;

                         auto trackId = arg (args, "track", -1);
                         if (! edit.getTrack (trackId).isValid())
                             trackId = (int) edit.addTrack (source.getFileNameWithoutExtension())[ids::id];

                         const auto start = (double) engine.getPosition() / engine.getSampleRate();
                         edit.addClip (trackId, source, juce::jmax (0.0, start),
                                       (double) decoded.audio.getNumSamples() / engine.getSampleRate());
                         return juce::String();
                     },
                     complete);
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
                         return juce::String();
                     },
                     complete);
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
