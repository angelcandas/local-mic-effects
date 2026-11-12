#include "DawBridge.h"

#include "analysis/TakeComp.h"

#include "daw/Quantize.h"
#include "daw/ClipTools.h"

#include "Presets.h"
#include "dsp/FxModules.h"

#include <map>
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

namespace
{
    // An instrument plugin's own editor in a window of its own (or a generic one with its
    // parameters when it has none). Keeps the instrument alive while it's open.
    class InstrumentWindow final : public juce::DocumentWindow
    {
    public:
        InstrumentWindow (const juce::String& title, daw::InstrumentPtr instrumentToShow, std::function<void()> onClose)
            : DocumentWindow (title, juce::Colour (0xff1b1d27), DocumentWindow::closeButton | DocumentWindow::minimiseButton),
              instrument (std::move (instrumentToShow)),
              closed (std::move (onClose))
        {
            auto* editor = instrument->hasEditor() ? instrument->createEditorAndMakeActive() : nullptr;
            if (editor == nullptr)
                editor = new juce::GenericAudioProcessorEditor (*instrument);

            setUsingNativeTitleBar (true);
            setContentOwned (editor, true);
            setResizable (editor->isResizable(), false);
            centreWithSize (juce::jmax (200, getWidth()), juce::jmax (100, getHeight()));
            setVisible (true);
            toFront (true);
        }

        ~InstrumentWindow() override { clearContentComponent(); } // the editor goes before the instrument

        void closeButtonPressed() override
        {
            juce::MessageManager::callAsync ([callback = closed] { if (callback) callback(); });
        }

        const daw::InstrumentPtr instrument;

    private:
        std::function<void()> closed;
    };

    juce::String instrumentKind (const juce::String& format)
    {
        return format.toLowerCase().removeCharacters (" ");
    }
}

struct DawBridge::ExportJob
{
    std::atomic<double> progress { 0.0 };
    std::atomic<bool> cancel { false }, finished { false };
    daw::Exporter::Result result;
};

struct DawBridge::StemJob
{
    enum Phase { downloading, loading, splitting };
    std::atomic<int> phase { loading };
    std::atomic<double> progress { 0.0 };
    std::atomic<bool> cancel { false }, finished { false };
    juce::ValueTree clip; // a copy, taken when the split started
    int trackId = -1;
    stems::Result result;
    std::atomic<juce::int64> splitStartedMs { 0 }; // when separating began (after download and load)
    std::atomic<bool> gpu { false };               // separating on the GPU (Core ML), not the CPU
    juce::String note;                             // why the GPU wasn't used, when it was meant to be; set before finished
    bool autoMix = false; // started by Auto Mix, which listens once it's done
};

struct DawBridge::AutoMixJob
{
    std::atomic<double> progress { 0.0 };
    std::atomic<bool> cancel { false }, finished { false };
    juce::File reference;                     // set for a reference match
    analysis::SessionAnalyser::Result result; // the tracks, or (reference) the mix
    analysis::Profile referenceProfile;
};

struct DawBridge::TakeCompJob
{
    std::atomic<double> progress { 0.0 };
    std::atomic<bool> cancel { false }, finished { false };
    std::vector<juce::ValueTree> tracks; // copies, taken when it started
    std::vector<std::pair<int, analysis::CompPlan>> plans; // by track id
    int keyTonic = -1;
    bool keyMinor = false;
};

struct DawBridge::KeyJob
{
    std::atomic<double> progress { 0.0 };
    std::atomic<bool> cancel { false }, finished { false };
    analysis::SessionAnalyser::Result result;
};

namespace
{
    const char* styleKey (analysis::MixStyle style)
    {
        switch (style)
        {
            case analysis::MixStyle::pop:      return "pop";
            case analysis::MixStyle::ballad:   return "ballad";
            case analysis::MixStyle::rock:     return "rock";
            case analysis::MixStyle::coaching: return "coaching";
            case analysis::MixStyle::hipHop:     return "hiphop";
            case analysis::MixStyle::rnb:        return "rnb";
            case analysis::MixStyle::acoustic:   return "acoustic";
            case analysis::MixStyle::electronic: return "electronic";
            case analysis::MixStyle::jazz:       return "jazz";
            case analysis::MixStyle::lofi:       return "lofi";
            case analysis::MixStyle::speech:     return "speech";
        }
        return "pop";
    }

    analysis::MixStyle styleFromKey (const juce::String& key)
    {
        for (auto style : analysis::allStyles)
            if (key == styleKey (style))
                return style;
        return analysis::MixStyle::pop;
    }

    constexpr const char* autoMixUndoName = "Auto Mix";

    // A track's volume ride for the page: [[seconds, dB], ...].
    juce::var describeRide (const juce::ValueTree& track)
    {
        juce::Array<juce::var> points;
        for (const auto& point : daw::Edit::getVolumeRide (track))
            points.add (juce::Array<juce::var> { point.time, point.db });
        return points;
    }

    // The tempo map for the page: [[beat, bpm, ramp], ...], the first at beat 0.
    juce::var describeTempo (const daw::TempoMap& tempo)
    {
        juce::Array<juce::var> points;
        for (const auto& point : tempo.getPoints())
            points.add (juce::Array<juce::var> { point.beat, point.bpm, point.ramp });
        return points;
    }

    // A track's effect automation for the page: [{ insert, type, value, points: [[seconds, value]...] }].
    juce::var describeAutomation (const juce::ValueTree& track)
    {
        juce::Array<juce::var> lanes;
        for (const auto& insert : daw::Edit::getInserts (track))
            for (const auto& [valueId, points] : daw::Edit::getInsertAutomation (insert))
            {
                juce::Array<juce::var> list;
                for (const auto& point : points)
                    list.add (juce::Array<juce::var> { point.time, point.value });
                auto* lane = new juce::DynamicObject();
                lane->setProperty ("insert", (int) insert[ids::id]);
                lane->setProperty ("type", insert[ids::type].toString());
                lane->setProperty ("value", valueId);
                lane->setProperty ("base", insert.getProperty (juce::Identifier (valueId), 0.0f));
                lane->setProperty ("points", list);
                lanes.add (juce::var (lane));
            }
        return lanes;
    }

    // What an edit command is called in "Undo ..." (the page shows it on the Undo button).
    juce::String describeUndoStep (const juce::String& command, const juce::var& args)
    {
        if (command == "setTrack" || command == "setMaster")
        {
            static const std::pair<const char*, const char*> keys[] {
                { "volume", "Volume" }, { "pan", "Pan" }, { "mute", "Mute" }, { "solo", "Solo" }, { "arm", "Arm" },
                { "monitor", "Monitor" }, { "monitorMode", "Monitor" }, { "name", "Rename track" }, { "colour", "Track colour" }, { "input", "Track input" },
                { "inGain", "Input gain" }, { "outGain", "Output gain" }, { "transpose", "Track key" },
                { "clearRide", "Clear volume ride" }, { "clearAutomation", "Clear effect automation" } };
            for (const auto& [key, name] : keys)
                if (args.hasProperty (key))
                    return name;
            return command == "setMaster" ? "Master change" : "Track change";
        }

        static const std::map<juce::String, const char*> names {
            { "addTrack", "Add track" }, { "addMidiTrack", "Add MIDI track" }, { "removeTrack", "Delete track" },
            { "duplicateTrack", "Duplicate track" }, { "moveTrack", "Move track" }, { "linkTracks", "Link tracks" },
            { "unlinkTrack", "Unlink track" }, { "moveClip", "Move clip" }, { "trimStart", "Trim clip" }, { "trimEnd", "Trim clip" },
            { "clipFades", "Fade" }, { "clipGain", "Clip gain" }, { "split", "Split clip" }, { "removeClip", "Delete clip" },
            { "duplicate", "Duplicate clip" }, { "pasteClip", "Paste clip" }, { "setClip", "Clip change" },
            { "normaliseClip", "Normalise clip" }, { "stripSilence", "Strip silence" }, { "addMidiClip", "Add MIDI clip" },
            { "editNotes", "Edit notes" }, { "addInsert", "Add effect" }, { "removeInsert", "Remove effect" },
            { "moveInsert", "Move effect" }, { "setInsert", "Effect change" }, { "pasteInserts", "Paste effects" },
            { "loadPreset", "Load preset" }, { "setSend", "Send level" }, { "setOutput", "Track output" },
            { "groupTracks", "Group tracks" }, { "ungroup", "Ungroup tracks" }, { "setGroup", "Move to group" },
            { "addReturn", "Add return" }, { "removeReturn", "Delete return" }, { "moveReturn", "Move return" },
            { "tempo", "Tempo" }, { "tempoMap", "Tempo change" }, { "beatsPerBar", "Time signature" }, { "loop", "Loop range" },
            { "addMarker", "Add marker" }, { "setMarker", "Marker change" }, { "removeMarker", "Delete marker" },
            { "quantize", "Quantize" }, { "setWarp", "Warp" }, { "clearWarp", "Clear warp" }, { "detectTempo", "Detect tempo" },
            { "addInstrument", "Change instrument" }, { "key", "Song key" }, { "clearMuteSolo", "Clear mute and solo" },
            { "soloExclusive", "Solo" }, { "import", "Import" }, { "importFile", "Import" }, { "autoMix", "Auto Mix" },
        };
        if (const auto it = names.find (command); it != names.end())
            return it->second;
        return command;
    }

    // A track's sends for the page: [{ target (return id), level (dB, -inf when off), pre }].
    juce::var describeSends (const juce::ValueTree& track)
    {
        juce::Array<juce::var> sends;
        for (const auto& send : track)
        {
            if (! send.hasType (daw::ids::SEND))
                continue;

            const auto level = (float) (double) send.getProperty (daw::ids::level, -100.0);
            sends.add (object ({ { "target", (int) send[daw::ids::target] },
                                 { "level", level >= daw::Edit::minSendDb ? (double) level : -100.0 },
                                 { "pre", (bool) send.getProperty (daw::ids::pre, false) } }));
        }
        return sends;
    }
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

    processor.getPluginHost().onScanFinished = [this]
    {
        auto& host = processor.getPluginHost();
        emit ("dawPlugins", object ({ { "scanning", false }, { "progress", 1.0 }, { "count", host.getInstruments().size() },
                                      { "failed", host.getFailedFiles().size() } }));
    };
}

DawBridge::~DawBridge()
{
    *alive = false;
    edit.getState().removeListener (this);
    processor.getPluginHost().onScanFinished = nullptr;
    instrumentWindows.clear();

    if (exportJob != nullptr)
        exportJob->cancel = true;
    if (autoMixJob != nullptr)
        autoMixJob->cancel = true;
    if (stemJob != nullptr)
        stemJob->cancel = true;
    if (keyJob != nullptr)
        keyJob->cancel = true;
    if (takeCompJob != nullptr)
        takeCompJob->cancel = true;
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

juce::var DawBridge::describeNotes (const juce::ValueTree& clip) const
{
    juce::Array<juce::var> notes;

    for (const auto& note : clip)
        if (note.hasType (ids::NOTE))
            notes.add (object ({ { "id", (int) note[ids::id] }, { "pitch", (int) number (note, ids::pitch) },
                                 { "time", number (note, ids::time) }, { "length", number (note, ids::length) },
                                 { "velocity", (int) number (note, ids::velocity) } }));

    return notes;
}

juce::var DawBridge::describeInstrument (const juce::ValueTree& track) const
{
    if (! daw::Edit::isMidiTrack (track))
        return {};

    const auto instrument = track.getChildWithName (ids::INSTRUMENT);
    const auto trackId = (int) track[ids::id];
    const auto format = instrument[ids::format].toString();

    return object ({ { "id", instrument[ids::plugin].toString() }, { "name", instrument[ids::name].toString() },
                     { "kind", instrumentKind (format) }, { "vendor", instrument[ids::vendor].toString() },
                     { "loaded", engine.getInstrument (trackId) != nullptr }, { "error", engine.getInstrumentError (trackId) },
                     { "hasWindow", daw::instruments::hasWindow (instrument[ids::plugin].toString()) },
                     { "windowOpen", instrumentWindows.count (trackId) > 0 } });
}

juce::var DawBridge::describeSession()
{
    const auto& state = edit.getState();
    juce::Array<juce::var> tracks;

    // An audio clip as the page draws it (in a track's comp or in a take lane).
    const auto describeAudioClip = [this] (const juce::ValueTree& clip)
    {
            const juce::File file (clip[ids::file].toString());
            const auto warpSettings = daw::Edit::getWarpSettings (clip);
            const auto audio = engine.getAudioPool().get (file, warpSettings);

            juce::Array<juce::var> markers;
            for (const auto& m : daw::Edit::getShownWarp (clip))
                markers.add (juce::Array<juce::var> { m.source, m.target });

            // Typed explicitly: the page does arithmetic on these, and text ("0.0") breaks it.
            return object ({ { "id", (int) clip[ids::id] }, { "name", clip[ids::name].toString() }, { "file", file.getFullPathName() },
                           { "start", number (clip, ids::start) }, { "offset", number (clip, ids::offset) },
                           { "length", number (clip, ids::length) }, { "gain", number (clip, ids::gain) },
                           { "fadeIn", number (clip, ids::fadeIn) }, { "fadeOut", number (clip, ids::fadeOut) },
                           { "mute", (bool) clip.getProperty (ids::mute, false) },
                           { "transpose", number (clip, ids::transpose) }, { "warp", markers },
                           { "stretch", daw::warp::getAlgorithmKey (warpSettings.algorithm) }, { "formant", warpSettings.formant },
                           { "musical", daw::Edit::isMusical (clip) }, { "clipTempo", daw::Edit::getClipTempo (clip) },
                           { "pitchEdited", ! warpSettings.notes.empty() },
                           // The audio the clip plays (its file, or a rendered copy): the page keys waveforms on it.
                           { "audioKey", warpSettings.isNeutral() ? file.getFullPathName() : daw::AudioPool::getWarpKey (file, warpSettings) },
                           { "fileLength", audio != nullptr ? (double) audio->getNumSamples() / engine.getSampleRate() : -1.0 },
                           { "missing", ! file.existsAsFile() },
                           { "take", (int) clip.getProperty (ids::take, -1) } });
    };

    for (const auto& track : edit.getTracks())
    {
        juce::Array<juce::var> clips;

        for (const auto& clip : track)
        {
            if (! clip.hasType (ids::CLIP))
                continue;

            if (daw::Edit::isMidiClip (clip))
            {
                clips.add (object ({ { "id", (int) clip[ids::id] }, { "name", clip[ids::name].toString() }, { "midi", true },
                                     { "start", number (clip, ids::start) }, { "offset", number (clip, ids::offset) },
                                     { "length", number (clip, ids::length) }, { "gain", 0.0 }, { "fadeIn", 0.0 }, { "fadeOut", 0.0 },
                                     { "mute", (bool) clip.getProperty (ids::mute, false) }, { "file", juce::String() },
                                     { "fileLength", -1.0 }, { "missing", false }, { "notes", describeNotes (clip) } }));
                continue;
            }

            clips.add (describeAudioClip (clip));
        }

        tracks.add (object ({ { "id", (int) track[ids::id] }, { "name", track[ids::name].toString() }, { "colour", track[ids::colour].toString() },
                              { "volume", number (track, ids::volume) }, { "pan", number (track, ids::pan) },
                              { "inGain", number (track, ids::inGain) }, { "outGain", number (track, ids::outGain) },
                              { "mute", (bool) track[ids::mute] }, { "solo", (bool) track[ids::solo] },
                              { "arm", (bool) track[ids::arm] }, { "monitor", (bool) track[ids::monitor] },
                              { "monitorMode", daw::Edit::getTrackMonitorMode (track) },
                              { "transpose", number (track, ids::transpose) }, { "ride", describeRide (track) }, { "automation", describeAutomation (track) },
                              { "link", daw::Edit::getLink (track) },
                              { "inserts", describeInserts (track) }, { "clips", clips },
                              { "output", daw::Edit::getTrackOutput (track) }, { "sends", describeSends (track) },
                              { "group", daw::Edit::getTrackGroup (track) },
                              { "input", [&track]
                                {
                                    const auto input = daw::Edit::getTrackInput (track);
                                    juce::Array<juce::var> list;
                                    if (input.left != daw::Edit::noInput) list.add (input.left);
                                    if (input.right != daw::Edit::noInput) list.add (input.right);
                                    return list;
                                }() },
                              { "kind", daw::Edit::isMidiTrack (track) ? "midi" : "audio" },
                              { "takes", [&]
                                {
                                    juce::Array<juce::var> takes;
                                    for (const auto& take : track)
                                    {
                                        if (! take.hasType (ids::TAKE))
                                            continue;
                                        juce::Array<juce::var> takeClips;
                                        for (const auto& clip : take)
                                            if (clip.hasType (ids::CLIP) && ! daw::Edit::isMidiClip (clip))
                                                takeClips.add (describeAudioClip (clip));
                                        takes.add (object ({ { "id", (int) take[ids::id] }, { "name", take[ids::name].toString() }, { "clips", takeClips } }));
                                    }
                                    return takes;
                                }() },
                              { "takesShown", (bool) track.getProperty (ids::takesShown, false) },
                              { "listenTake", daw::Edit::getListenTake (track) },
                              { "instrument", describeInstrument (track) } }));
    }

    const auto master = edit.getMaster();
    auto& undo = edit.getUndoManager();

    juce::Array<juce::var> returns;
    for (const auto& bus : edit.getReturns())
        returns.add (object ({ { "id", (int) bus[ids::id] }, { "name", bus[ids::name].toString() }, { "colour", bus[ids::colour].toString() },
                               { "volume", number (bus, ids::volume) }, { "pan", number (bus, ids::pan) },
                               { "inGain", number (bus, ids::inGain) }, { "outGain", number (bus, ids::outGain) },
                               { "mute", (bool) bus[ids::mute] }, { "solo", (bool) bus[ids::solo] },
                               { "group", daw::Edit::isGroup (bus) }, { "folded", (bool) bus.getProperty (ids::folded, false) },
                               { "sends", describeSends (bus) },
                               { "inserts", describeInserts (bus) } }));

    if (lastInputNames.isEmpty())
        lastInputNames = getInputNames();

    juce::Array<juce::var> inputs;
    for (const auto& name : lastInputNames)
        inputs.add (name);

    return object ({
        { "available", isAvailable() },
        { "inputs", inputs },
        { "mode", processor.isDawMode() ? "daw" : "live" },
        { "project", object ({ { "open", project.isOpen() }, { "name", project.isOpen() ? project.getName() : juce::String ("Untitled") },
                               { "path", project.getFolder().getFullPathName() }, { "dirty", project.hasUnsavedChanges() },
                               { "recoverable", project.getRecoverableBackup().getFileName() },
                               { "missing", project.getMissingFiles().size() } }) },
        { "tempo", edit.getTempo() },
        { "tempoMap", describeTempo (edit.getTempoMap()) },
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
        { "undoName", undo.getUndoDescription() },
        { "redoName", undo.getRedoDescription() },
        { "view", state[ids::viewState].toString() },
        { "returns", returns },
        { "master", object ({ { "volume", number (master, ids::volume) }, { "pan", number (master, ids::pan) }, { "inGain", number (master, ids::inGain) },
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

    // Each strip's level: the louder side, and left and right for the mixer's stereo meters.
    auto* stereo = new juce::DynamicObject();
    const auto addPeaks = [&] (int id)
    {
        const auto [left, right] = engine.takePeaks (id);
        stereo->setProperty (juce::String (id), juce::Array<juce::var> { peakToDecibels (left), peakToDecibels (right) });
        return peakToDecibels (juce::jmax (left, right));
    };

    for (const auto& track : edit.getTracks())
    {
        peaks->setProperty (juce::String ((int) track[ids::id]), addPeaks ((int) track[ids::id]));
        addChain ((int) track[ids::id]);
    }

    for (const auto& bus : edit.getReturns())
    {
        peaks->setProperty (juce::String ((int) bus[ids::id]), addPeaks ((int) bus[ids::id]));
        addChain ((int) bus[ids::id]);
    }

    const auto masterPeak = addPeaks (daw::Edit::masterId);

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
        { "master", masterPeak },
        { "tracks", juce::var (peaks) },
        { "stereo", juce::var (stereo) },
        { "chains", juce::var (chains) },
        { "inserts", juce::var (inserts) },
        { "loading", engine.getAudioPool().isLoading() },
        { "midiNotes", [this]
          {
              juce::Array<juce::var> held;
              const auto notes = engine.getHeldNotes();
              for (int n = 0; n < 128; ++n)
                  if (notes[(size_t) n])
                      held.add (n);
              return juce::var (held);
          }() },
    });
}

juce::var DawBridge::getPeaks (const juce::File& file, const daw::warp::Settings& settings)
{
    // A warped or transposed clip shows its rendered copy.
    const auto key = settings.isNeutral() ? file.getFullPathName() : daw::AudioPool::getWarpKey (file, settings);

    if (const auto it = peakCache.find (key); it != peakCache.end())
        return it->second;

    const auto audio = engine.getAudioPool().get (file, settings);
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
juce::var DawBridge::getDetailPeaks (const juce::File& file, double start, double end, double seconds, const daw::warp::Settings& settings)
{
    const auto audio = engine.getAudioPool().get (file, settings);
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
void DawBridge::updateInstrumentWindows()
{
    // What each MIDI track's instrument is now (a rebuild loads them after the edit).
    juce::String seen;
    for (const auto& track : edit.getTracks())
        if (daw::Edit::isMidiTrack (track))
        {
            const auto id = (int) track[ids::id];
            seen << id << ":" << juce::String::toHexString ((juce::pointer_sized_int) engine.getInstrument (id).get())
                 << ":" << engine.getInstrumentError (id) << ";";
        }

    if (seen == instrumentsSeen)
        return;

    instrumentsSeen = seen;
    sessionChanged = true;

    // Windows of instruments that were replaced or removed close.
    for (auto it = instrumentWindows.begin(); it != instrumentWindows.end();)
    {
        const auto* window = static_cast<InstrumentWindow*> (it->second.get());
        if (window->instrument != engine.getInstrument (it->first))
            it = instrumentWindows.erase (it);
        else
            ++it;
    }
}

juce::StringArray DawBridge::getInputNames() const
{
    if (inputNames != nullptr)
        if (auto names = inputNames(); ! names.isEmpty())
            return names;

    // Without the app's device: the processor's input channels.
    juce::StringArray names;
    for (int i = 0; i < juce::jmax (1, processor.getTotalNumInputChannels()); ++i)
        names.add ("Input " + juce::String (i + 1));
    return names;
}

void DawBridge::tick()
{
    updateInstrumentWindows();

    if (auto& host = processor.getPluginHost(); host.isScanning() && ++scanTicks % 10 == 0)
        emit ("dawPlugins", object ({ { "scanning", true }, { "progress", host.getScanProgress() },
                                      { "name", juce::File (host.getScanningName()).getFileNameWithoutExtension() } }));

    // The interface's inputs (tracks pick from them): the session again when they change.
    if (++inputTicks % 10 == 1)
    {
        auto names = getInputNames();
        if (names != lastInputNames)
        {
            lastInputNames = std::move (names);
            sessionChanged = true;
        }
    }

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
                // New analysis: the tracks' tags, and a guess from the sound for the rest.
                autoMixAnalysis = std::move (job->result);
                std::map<int, analysis::Role> roles;
                const auto songSeconds = autoMixAnalysis->tracks.empty() ? 0.0
                                       : autoMixAnalysis->tracks.front().second.getNumFrames() * analysis::Profile::hopSeconds;

                for (const auto& [id, profile] : autoMixAnalysis->tracks)
                {
                    const auto track = edit.getTrack (id);
                    const auto byInstrument = guessRoleFromTrack (track);
                    roles[id] = track.hasProperty (ids::mixRole) ? analysis::getRoleFromKey (track[ids::mixRole].toString())
                              : byInstrument.has_value() ? *byInstrument
                                                         : analysis::guessRole (track[ids::name].toString(), profile, songSeconds);
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

    if (stemJob != nullptr)
    {
        if (stemJob->finished)
        {
            auto job = std::move (stemJob);

            if (job->autoMix)
                finishAutoMixSplit (*job);
            else if (job->result.cancelled)
                emit ("dawStems", object ({ { "state", "cancelled" } }));
            else if (job->result.error.isNotEmpty())
                emit ("dawStems", object ({ { "state", "error" }, { "error", job->result.error } }));
            else
            {
                const auto added = addStemTracks (job->clip, job->trackId, job->result.files);
                const auto seconds = (juce::Time::getMillisecondCounter() - job->splitStartedMs.load()) / 1000.0;
                emit ("dawStems", (bool) added["ok"] ? object ({ { "state", "done" }, { "tracks", added["tracks"] }, { "seconds", seconds },
                                                                                                    { "gpu", job->gpu.load() }, { "note", job->note } })
                                                     : object ({ { "state", "error" }, { "error", added["error"] } }));
            }
        }
        else if (stemJob->autoMix)
        {
            // Auto Mix splitting a song first: its own panel shows the progress.
            emit ("dawAutoMix", object ({ { "state", "splitting" }, { "phase", stemJob->phase.load() },
                                          { "progress", stemJob->progress.load() } }));
        }
        else
        {
            static constexpr const char* phases[] { "downloading", "loading", "splitting" };
            emit ("dawStems", object ({ { "state", phases[juce::jlimit (0, 2, stemJob->phase.load())] },
                                        { "progress", stemJob->progress.load() }, { "gpu", stemJob->gpu.load() } }));
        }
    }

    if (takeCompJob != nullptr)
    {
        if (takeCompJob->finished)
            finishTakeComp();
        else
            emit ("dawTakeComp", object ({ { "state", "analysing" }, { "progress", takeCompJob->progress.load() } }));
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

juce::var DawBridge::addStemTracks (const juce::ValueTree& clip, int trackId,
                                    const std::array<juce::File, stems::numStems>& files)
{
    for (const auto& file : files)
        if (! file.existsAsFile())
            return error ("A stem file is missing: " + file.getFullPathName());

    auto& undo = edit.getUndoManager();
    undo.beginNewTransaction ("Split into stems");

    // Right under the song's track (or at the end if it has gone), vocals first.
    const auto tracks = edit.getTracks();
    auto index = tracks.size();
    for (int i = 0; i < tracks.size(); ++i)
        if ((int) tracks[i][ids::id] == trackId)
            index = i + 1;

    juce::Array<juce::var> added;
    for (const auto stem : { stems::Stem::vocals, stems::Stem::drums, stems::Stem::bass, stems::Stem::other })
    {
        const auto track = edit.addTrack (stems::stemNames[(size_t) stem], index++);
        const auto id = (int) track[ids::id];
        auto copy = edit.addClip (id, files[(size_t) stem], number (clip, ids::start), number (clip, ids::length),
                                  number (clip, ids::offset));
        if (! copy.isValid())
            continue;

        edit.setClipGain ((int) copy[ids::id], (float) number (clip, ids::gain));
        edit.setClipFades ((int) copy[ids::id], number (clip, ids::fadeIn), number (clip, ids::fadeOut));
        added.add (id);
    }

    // The stems replace the song, so stop it playing on top of them.
    if (const auto original = edit.getClip ((int) clip[ids::id]); original.isValid())
        edit.setClipMute ((int) clip[ids::id], true);

    sessionChanged = true;
    return object ({ { "ok", true }, { "error", juce::String() }, { "tracks", added } });
}

juce::var DawBridge::addLoopTracks (const std::vector<LoopLayer>& layers, double loopSeconds, int repeats, double bpm, int beatsPerBar)
{
    if (layers.empty() || loopSeconds <= 0.0)
        return error ("There's no loop to send.");

    for (const auto& layer : layers)
        if (! layer.file.existsAsFile())
            return error ("A loop file is missing: " + layer.file.getFullPathName());

    auto& undo = edit.getUndoManager();
    undo.beginNewTransaction ("Add loop tracks");

    // Bars line up with the loop when it was recorded to a tempo.
    if (bpm > 0.0)
    {
        edit.setTempo (bpm);
        edit.setBeatsPerBar (juce::jlimit (2, 12, beatsPerBar));
    }

    repeats = juce::jlimit (1, 64, repeats);
    juce::Array<juce::var> added;
    int number = 1;

    for (const auto& layer : layers)
    {
        const auto source = project.isOpen() ? project.importAudio (layer.file) : layer.file;
        if (! source.existsAsFile())
            return error ("Couldn't copy " + layer.file.getFileName() + " into the project's Audio folder.");

        const auto id = (int) edit.addTrack ("Loop " + juce::String (number++))[ids::id];

        for (int pass = 0; pass < repeats; ++pass)
        {
            const auto clip = edit.addClip (id, source, pass * loopSeconds, loopSeconds);
            if (clip.isValid() && std::abs (layer.gainDecibels) > 0.01f)
                edit.setClipGain ((int) clip[ids::id], layer.gainDecibels);
        }

        if (layer.muted)
            edit.setTrackMute (id, true);
        added.add (id);
    }

    sessionChanged = true;
    return object ({ { "ok", true }, { "error", juce::String() }, { "tracks", added } });
}

juce::var DawBridge::handleStems (const juce::var& args)
{
    const auto action = args["action"].toString();

    if (action == "cancel")
    {
        if (stemJob != nullptr)
            stemJob->cancel = true;
        return error ({});
    }

    if (stemJob != nullptr)
        return error ("A song is already splitting into stems.");
    if (! stems::Model::isBuiltIn())
        return error ("This build of MicFX doesn't include the stem splitter.");

    const auto clip = edit.getClip (arg (args, "id", -1));
    if (! clip.isValid())
        return error ("Select the song's clip first.");

    const juce::File source (clip[ids::file].toString());
    if (! source.existsAsFile())
        return error ("Couldn't find " + source.getFullPathName() + ".");

    auto job = std::make_shared<StemJob>();
    job->clip = clip.createCopy();
    job->trackId = (int) clip.getParent()[ids::id];
    stemJob = job;

    const auto folder = getRecordingFolder();
    const auto name = source.getFileNameWithoutExtension();
    const auto model = stems::Model::getDefaultFile();

    juce::Thread::launch ([job, source, folder, name, model]
    {
        const auto progress = [job] (double p) { job->progress = p; return ! job->cancel.load(); };
        auto& result = job->result;
        stems::Separator separator;

        // The GPU first, where Core ML is built in: its converted model downloads once, like the CPU one.
        if (const auto gpuModel = stems::GpuModel::getDefaultFile(); stems::GpuModel::isBuiltIn())
        {
            juce::String problem;

            if (! gpuModel.isDirectory())
            {
                job->phase = StemJob::downloading;
                problem = stems::GpuModel::download (gpuModel, progress);

                if (job->cancel.load())
                {
                    result.cancelled = true;
                    job->finished = true;
                    return;
                }

                if (problem.isNotEmpty())
                    problem = "couldn't get it: " + problem;
            }

            if (problem.isEmpty())
            {
                job->phase = StemJob::loading;
                job->progress = 0.0;
                separator = stems::GpuModel::load (gpuModel, problem);
            }

            if (separator != nullptr)
                job->gpu = true;
            else
                job->note = "The GPU model didn't work (" + problem + "), so this split ran on the CPU.";
        }

        if (separator == nullptr && ! job->cancel.load())
        {
            if (! model.existsAsFile() || stems::Model::verify (model).isNotEmpty())
            {
                job->phase = StemJob::downloading;
                if (const auto problem = stems::Model::download (model, progress); problem.isNotEmpty())
                {
                    result.cancelled = job->cancel.load();
                    result.error = result.cancelled ? juce::String() : problem;
                    job->finished = true;
                    return;
                }
            }

            job->phase = StemJob::loading;
            job->progress = 0.0;
            separator = stems::Model::load (model, result.error);
        }

        if (separator != nullptr && ! job->cancel.load())
        {
            // The GPU takes one segment at a time; two workers keep it fed while the other
            // prepares or finishes a segment. On the CPU every core works on its own chunk.
            stems::Settings settings;
            settings.threads = job->gpu ? 2 : 0;

            job->phase = StemJob::splitting;
            job->progress = 0.0;
            job->splitStartedMs = (juce::int64) juce::Time::getMillisecondCounter();
            result = stems::split (source, folder, name, separator, progress, settings);
        }

        result.cancelled = result.cancelled || job->cancel.load();
        job->finished = true;
    });

    return error ({});
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
    auto inserts = daw::Edit::getInserts (edit.getChannel (trackId));
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
    engine.captureInstrumentStates(); // the export loads its own instruments from these
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
    engine.captureInstrumentStates();
    auto job = std::make_shared<AutoMixJob>();
    job->reference = reference;

    // Snapshot on this thread. For a reference match only the mix is needed, without
    // a master EQ an earlier match added (the new one replaces it).
    std::shared_ptr<analysis::SessionAnalyser> analyser;
    if (reference == juce::File() && autoMixOptions.mode == analysis::MixMode::master)
    {
        // Master mode measures the mix as it plays, without the mastering chain an
        // earlier pass added (the new one replaces it).
        daw::Edit copy;
        copy.replaceState (edit.getState().createCopy());
        auto inserts = daw::Edit::getInserts (copy.getMaster());
        for (int i = inserts.getNumChildren(); --i >= 0;)
            for (auto type : { fx::ModuleType::masterEq, fx::ModuleType::multiband, fx::ModuleType::imager, fx::ModuleType::limiter,
                               fx::ModuleType::loudness })
                if (inserts.getChild (i)[ids::type].toString() == fx::getModuleKey (type))
                {
                    inserts.removeChild (i, nullptr);
                    break;
                }

        analyser = std::make_shared<analysis::SessionAnalyser> (copy, analysis::SessionAnalyser::Point::stem, true,
                                                                engine.getSampleRate(), false);
    }
    else if (reference == juce::File())
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

    juce::Thread::launch ([analyser, job, cache = profileCache]
    {
        const auto reading = job->reference != juce::File();
        const auto progress = [job, reading] (double p) { job->progress = reading ? 0.5 * p : p; return ! job->cancel.load(); };
        job->result = analyser->run (progress, cache.get());

        if (reading && ! job->result.cancelled)
        {
            job->progress = 0.6;
            job->referenceProfile = analysis::analyseFile (job->reference, [job] { return ! job->cancel.load(); });
            job->result.cancelled = job->cancel.load();
        }

        job->finished = true;
    });
}

// Split a full song into stems before Auto Mix listens: the first track tagged (or
// named) instrumental with one audio clip playing. Returns false when there's nothing
// to split (Auto Mix then listens at once).
bool DawBridge::startAutoMixSplit()
{
    if (stemJob != nullptr)
    {
        autoMixSplitNote = "A song is already splitting into stems: Auto Mix listened without splitting.";
        return false;
    }

    juce::ValueTree song;
    int candidates = 0;
    for (const auto& track : edit.getTracks())
    {
        const auto role = track.hasProperty (ids::mixRole) ? analysis::getRoleFromKey (track[ids::mixRole].toString())
                                                           : analysis::guessRoleFromName (track[ids::name].toString()).value_or (analysis::Role::other);
        if (role != analysis::Role::backing || daw::Edit::isMidiTrack (track))
            continue;

        juce::ValueTree playing;
        int count = 0;
        for (const auto& child : track)
            if (child.hasType (ids::CLIP) && ! (bool) child.getProperty (ids::mute, false))
                playing = child, ++count;

        if (count == 1)
        {
            ++candidates;
            if (! song.isValid())
                song = playing;
        }
    }

    if (! song.isValid())
    {
        autoMixSplitNote = "Nothing to split: tag a track holding the whole song (one clip) as Instrumental.";
        return false;
    }
    if (! stems::Model::isBuiltIn())
    {
        autoMixSplitNote = "This build of MicFX doesn't include the stem splitter: Auto Mix listened without splitting.";
        return false;
    }

    auto args = object ({ { "action", "split" }, { "id", (int) song[ids::id] } });
    if (const auto started = handleStems (args); ! (bool) started["ok"])
    {
        autoMixSplitNote = "Couldn't split: " + started["error"].toString();
        return false;
    }

    stemJob->autoMix = true;
    if (candidates > 1)
        autoMixSplitNote = "Split the first instrumental only. Split the others from their clips, then listen again.";
    emit ("dawAutoMix", object ({ { "state", "splitting" }, { "phase", (int) StemJob::loading }, { "progress", 0.0 } }));
    return true;
}

void DawBridge::finishAutoMixSplit (StemJob& job)
{
    if (job.result.cancelled)
    {
        emit ("dawAutoMix", object ({ { "state", "cancelled" } }));
        return;
    }
    if (job.result.error.isNotEmpty())
    {
        emit ("dawAutoMix", object ({ { "state", "error" }, { "error", "Couldn't split the song: " + job.result.error } }));
        return;
    }

    const auto added = addStemTracks (job.clip, job.trackId, job.result.files);
    if (! (bool) added["ok"])
    {
        emit ("dawAutoMix", object ({ { "state", "error" }, { "error", added["error"] } }));
        return;
    }

    // Tag the stems: the song's own vocals become background vocals under yours (a guide
    // to sing along to); drums, bass and the rest by name. The song itself, now muted, is
    // left as it is.
    for (const auto& id : *added["tracks"].getArray())
    {
        auto track = edit.getTrack ((int) id);
        const auto role = analysis::guessRoleFromName (track[ids::name].toString()).value_or (analysis::Role::instrument);
        track.setProperty (ids::mixRole, analysis::getRoleKey (role == analysis::Role::voice ? analysis::Role::backingVocal : role), nullptr);
    }
    if (auto song = edit.getTrack (job.trackId); song.isValid())
        song.setProperty (ids::mixRole, analysis::getRoleKey (analysis::Role::other), nullptr);

    autoMixSplitNote = "Split " + job.clip[ids::name].toString().quoted() + " into vocals, drums, bass and other, and muted the song.";
    startAutoMix ({});
}

void DawBridge::replanAutoMix()
{
    if (! autoMixAnalysis.has_value())
        return;

    std::map<int, float> volumes;
    for (const auto& track : edit.getTracks())
        volumes[(int) track[ids::id]] = track.getProperty (ids::volume, 0.0f);

    auto options = autoMixOptions;
    options.tempo = edit.getTempo();
    for (const auto& track : edit.getTracks())
    {
        const auto id = (int) track[ids::id];
        options.trackNames[id] = track[ids::name].toString();
        if (daw::Edit::isMidiTrack (track))
            options.midiTracks.insert (id);
        for (const auto& insert : daw::Edit::getInserts (track))
            for (int type = 0; type < fx::numModuleTypes; ++type)
                if (insert[ids::type].toString() == fx::getModuleKey ((fx::ModuleType) type))
                    options.existingInserts[id].push_back ((fx::ModuleType) type);
    }

    // The shared reverb: the first return with a reverb on it.
    for (const auto& ret : edit.getReturns())
    {
        for (const auto& insert : daw::Edit::getInserts (ret))
            if (insert[ids::type].toString() == fx::getModuleKey (fx::ModuleType::reverb))
                options.reverbReturn = (int) ret[ids::id];
        if (options.reverbReturn != 0)
            break;
    }

    autoMixPlan = analysis::planMix (*autoMixAnalysis, autoMixRoles, volumes,
                                     (float) edit.getMaster().getProperty (ids::volume, 0.0f), autoMixStyle, options);
}

void DawBridge::readAutoMixEffects (const juce::var& effects)
{
    if (! effects.isObject())
        return;

    const auto read = [&effects] (const char* key, bool& flag)
    {
        if (effects.hasProperty (key))
            flag = (bool) effects[key];
    };
    read ("eq", autoMixOptions.eq);
    read ("compressor", autoMixOptions.compressor);
    read ("deEsser", autoMixOptions.deEsser);
    read ("reverb", autoMixOptions.reverb);
    read ("saturation", autoMixOptions.saturation);
    read ("echo", autoMixOptions.echo);
    read ("exciter", autoMixOptions.exciter);
    read ("width", autoMixOptions.width);
    read ("normalize", autoMixOptions.normalize);
    read ("rides", autoMixOptions.rides);
    read ("colour", autoMixOptions.colour);
    read ("sends", autoMixOptions.sends);
    read ("throws", autoMixOptions.throws);
    read ("dynamicEq", autoMixOptions.dynamicEq);
    read ("transients", autoMixOptions.transients);
    read ("split", autoMixSplit);
}

void DawBridge::readAutoMixMode (const juce::var& args)
{
    if (args.hasProperty ("mode"))
        autoMixOptions.mode = analysis::getModeFromKey (args["mode"].toString());
    if (args.hasProperty ("focus"))
        autoMixOptions.focusTrackId = (int) args["focus"];
    if (args.hasProperty ("masterLufs"))
        autoMixOptions.masterLufs = juce::jlimit (-24.0f, -6.0f, (float) args["masterLufs"]);
}

// Every track's tag before Auto Mix listens: the one you picked (tagged), or the guess
// from its name ("" when the name says nothing, and the sound decides).
// A track's role from its name, or a MIDI track's from its instrument.
std::optional<analysis::Role> DawBridge::guessRoleFromTrack (const juce::ValueTree& track)
{
    if (const auto byName = analysis::guessRoleFromName (track[ids::name].toString()))
        return byName;
    if (! daw::Edit::isMidiTrack (track))
        return std::nullopt;
    const auto instrument = track.getChildWithName (ids::INSTRUMENT);
    return analysis::guessRoleFromInstrument (instrument[ids::plugin].toString(), instrument[ids::name].toString());
}

juce::var DawBridge::describeAutoMixTags() const
{
    juce::Array<juce::var> tracks, roles;
    for (const auto& track : edit.getTracks())
    {
        const auto tagged = track.hasProperty (ids::mixRole);
        const auto guess = guessRoleFromTrack (track);
        const auto role = tagged ? juce::String (analysis::getRoleKey (analysis::getRoleFromKey (track[ids::mixRole].toString())))
                        : guess.has_value() ? juce::String (analysis::getRoleKey (*guess)) : juce::String();
        tracks.add (object ({ { "id", (int) track[ids::id] }, { "name", track[ids::name].toString() }, { "role", role },
                              { "tagged", tagged }, { "midi", daw::Edit::isMidiTrack (track) } }));
    }

    for (auto role : analysis::allRoles)
        roles.add (object ({ { "key", analysis::getRoleKey (role) }, { "name", analysis::getRoleName (role) } }));

    return object ({ { "ok", true }, { "tracks", tracks }, { "roles", roles } });
}

juce::var DawBridge::describeAutoMix() const
{
    const auto nameOf = [this] (int trackId)
    {
        return trackId == daw::Edit::masterId ? juce::String ("Master") : edit.getChannel (trackId)[ids::name].toString();
    };

    juce::Array<juce::var> tracks, moves, notes;
    if (autoMixSplitNote.isNotEmpty() && ! autoMixReference)
        notes.add (autoMixSplitNote);

    if (autoMixAnalysis.has_value() && ! autoMixReference)
        for (const auto& [id, profile] : autoMixAnalysis->tracks)
        {
            if (! edit.getTrack (id).isValid())
                continue;

            const auto role = autoMixRoles.count (id) > 0 ? autoMixRoles.at (id) : analysis::Role::other;
            tracks.add (object ({ { "id", id }, { "name", nameOf (id) }, { "role", analysis::getRoleKey (role) },
                                  { "lufs", profile.integratedLufs }, { "active", profile.getActiveSeconds() } }));
        }

    for (const auto& move : autoMixPlan.moves)
    {
        juce::String effect = ! move.ride.empty() ? "Volume ride" : move.values.count ("inGain") > 0 ? "Input gain" : "Volume";
        if (move.sendTo != 0)
            effect = "Send to " + edit.getReturn (move.sendTo)[ids::name].toString();
        if (move.insert.has_value())
            effect = *move.insert == fx::ModuleType::paraEq     ? (move.instance > 0 ? "Parametric EQ (hum)" : "Parametric EQ")
                   : *move.insert == fx::ModuleType::gate       ? "Noise gate"
                   : *move.insert == fx::ModuleType::compressor ? "Compressor"
                   : *move.insert == fx::ModuleType::deEsser    ? "De-esser"
                   : *move.insert == fx::ModuleType::reverb     ? "Reverb"
                   : *move.insert == fx::ModuleType::saturation ? "Saturation"
                   : *move.insert == fx::ModuleType::exciter    ? "Exciter"
                   : *move.insert == fx::ModuleType::echo       ? "Echo"
                   : *move.insert == fx::ModuleType::width      ? "Stereo width"
                   : *move.insert == fx::ModuleType::transient  ? "Transient shaper"
                   : *move.insert == fx::ModuleType::dynamicEq  ? "Dynamic EQ"
                   : *move.insert == fx::ModuleType::masterEq   ? "Mastering EQ"
                   : *move.insert == fx::ModuleType::multiband  ? "Multiband compressor"
                   : *move.insert == fx::ModuleType::imager     ? "Stereo imager"
                   : *move.insert == fx::ModuleType::limiter    ? "Limiter"
                   : *move.insert == fx::ModuleType::loudness   ? "Loudness meter"
                                                                : juce::String (fx::getModuleKey (*move.insert));

        juce::Array<juce::var> reasons;
        for (const auto& reason : move.reasons)
            reasons.add (reason);

        moves.add (object ({ { "track", move.trackId }, { "name", nameOf (move.trackId) }, { "effect", effect }, { "reasons", reasons } }));
    }

    for (const auto& note : autoMixPlan.notes)
        notes.add (note);

    const auto effects = object ({ { "eq", autoMixOptions.eq }, { "compressor", autoMixOptions.compressor },
                                   { "deEsser", autoMixOptions.deEsser }, { "reverb", autoMixOptions.reverb },
                                   { "saturation", autoMixOptions.saturation }, { "echo", autoMixOptions.echo },
                                   { "exciter", autoMixOptions.exciter }, { "width", autoMixOptions.width },
                                   { "rides", autoMixOptions.rides }, { "colour", autoMixOptions.colour }, { "sends", autoMixOptions.sends }, { "throws", autoMixOptions.throws },
                                   { "dynamicEq", autoMixOptions.dynamicEq }, { "transients", autoMixOptions.transients },
                                   { "normalize", autoMixOptions.normalize }, { "split", autoMixSplit } });

    return object ({ { "state", "ready" }, { "kind", autoMixReference ? "reference" : "mix" }, { "reference", autoMixReferenceName },
                     { "style", styleKey (autoMixStyle) }, { "mode", analysis::getModeKey (autoMixOptions.mode) },
                     { "focus", autoMixOptions.focusTrackId }, { "masterLufs", autoMixOptions.masterLufs }, { "effects", effects }, { "tracks", tracks }, { "moves", moves }, { "notes", notes },
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

        engine.captureInstrumentStates(); // the analysis loads its own instruments from these
        auto job = std::make_shared<KeyJob>();
        auto analyser = std::make_shared<analysis::SessionAnalyser> (edit, analysis::SessionAnalyser::Point::source, false,
                                                                     engine.getSampleRate());
        keyJob = job;

        juce::Thread::launch ([analyser, job, cache = profileCache]
        {
            job->result = analyser->run ([job] (double p) { job->progress = p; return ! job->cancel.load(); }, cache.get());
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

//==============================================================================
// "takes" { action, ... }: take lanes and comping.
//   show      { track, on }        unfold (or fold) the track's take lanes
//   listen    { track, take }      hear one take instead of the comp (take -1: the comp)
//   comp      { take, start, end } comp a range from a take
//   compAt    { take, time }       comp the stretch of the comp at a time -> { ok, start, end }
//   rename    { take, name }
//   remove    { take }
//   fromClip  { clip }             a copy of a clip as a new take lane -> { ok, take }
//   auto      { track }            Auto Comp: pick the best takes (track -1: every track
//                                  with two or more takes); answers come as "dawTakeComp"
//   cancel
juce::var DawBridge::handleTakes (const juce::var& args)
{
    const auto action = args["action"].toString();
    const auto trackId = arg (args, "track", -1), takeId = arg (args, "take", -1);

    if (action == "show")   { edit.setTakesShown (trackId, arg (args, "on", true)); return error ({}); }
    if (action == "listen") { edit.setListenTake (trackId, takeId); return error ({}); }
    if (action == "rename") { edit.setTakeName (takeId, args["name"].toString()); return error ({}); }
    if (action == "remove") { edit.removeTake (takeId); return error ({}); }

    if (action == "comp")
    {
        if (! edit.getTake (takeId).isValid())
            return error ("That take is gone.");
        edit.compTake (takeId, arg (args, "start", 0.0), arg (args, "end", 0.0));
        return error ({});
    }

    if (action == "compAt")
    {
        const auto [start, end] = edit.compTakeAt (takeId, arg (args, "time", 0.0));
        if (end <= start)
            return error ("This take has no audio there.");
        return object ({ { "ok", true }, { "start", start }, { "end", end } });
    }

    if (action == "fromClip")
    {
        const auto take = edit.addTakeFromClip (arg (args, "clip", -1));
        if (! take.isValid())
            return error ("Only an audio clip on a track can become a take.");
        edit.setTakesShown ((int) daw::Edit::getOwnerTrack (take)[ids::id], true);
        return object ({ { "ok", true }, { "take", (int) take[ids::id] } });
    }

    if (action == "cancel")
    {
        if (takeCompJob != nullptr)
            takeCompJob->cancel = true;
        return error ({});
    }

    if (action == "auto")
    {
        if (takeCompJob != nullptr)
            return error ("Already listening to the takes.");

        auto job = std::make_shared<TakeCompJob>();
        for (const auto& track : edit.getTracks())
            if ((trackId < 0 || (int) track[ids::id] == trackId) && edit.getTakes ((int) track[ids::id]).size() >= 2)
                job->tracks.push_back (track.createCopy());

        if (job->tracks.empty())
            return error (trackId < 0 ? "No track has two or more takes to choose from."
                                      : "This track needs two or more takes to choose from.");

        job->keyTonic = edit.getKey();
        job->keyMinor = edit.isKeyMinor();
        takeCompJob = job;

        juce::Thread::launch ([job]
        {
            const auto keepGoing = [job] { return ! job->cancel.load(); };
            for (size_t i = 0; i < job->tracks.size() && keepGoing(); ++i)
            {
                auto input = analysis::measureTakes (job->tracks[i], keepGoing);
                input.keyTonic = job->keyTonic;
                input.keyMinor = job->keyMinor;
                if (keepGoing())
                    job->plans.emplace_back ((int) job->tracks[i][ids::id], analysis::planComp (input));
                job->progress = (double) (i + 1) / (double) job->tracks.size();
            }
            job->finished = true;
        });

        emit ("dawTakeComp", object ({ { "state", "analysing" }, { "progress", 0.0 } }));
        return error ({});
    }

    return error ("Unknown takes action: " + action);
}

void DawBridge::finishTakeComp()
{
    auto job = std::move (takeCompJob);

    if (job->cancel)
    {
        emit ("dawTakeComp", object ({ { "state", "cancelled" } }));
        return;
    }

    // One undo step for the whole comp, so Ctrl+Z puts the old one back.
    edit.getUndoManager().beginNewTransaction ("Auto Comp");
    juce::Array<juce::var> tracks;

    for (const auto& [trackId, plan] : job->plans)
    {
        juce::Array<juce::var> picks;
        for (const auto& pick : plan.picks)
        {
            if (! edit.getTake (pick.takeId).isValid())
                continue; // deleted while it listened
            edit.compTake (pick.takeId, pick.start, pick.end);
            picks.add (object ({ { "start", pick.start }, { "end", pick.end }, { "take", pick.takeId }, { "name", pick.takeName },
                                 { "score", pick.score }, { "reason", pick.reason } }));
        }
        if (! plan.picks.empty())
            edit.setTakesShown (trackId, true);

        tracks.add (object ({ { "track", trackId }, { "picks", picks }, { "summary", plan.summary } }));
    }

    edit.getUndoManager().beginNewTransaction();
    emit ("dawTakeComp", object ({ { "state", "done" }, { "tracks", tracks } }));
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
        const auto role = track.hasProperty (ids::mixRole) ? analysis::getRoleFromKey (track[ids::mixRole].toString())
                        : known != autoMixRoles.end() ? known->second
                                                      : analysis::guessRole (track[ids::name].toString(), profile, songSeconds);
        const auto isVoice = role == analysis::Role::voice || role == analysis::Role::backingVocal;
        const auto sum = profile.getChromaSum();
        const auto weight = std::accumulate (sum.begin(), sum.end(), 0.0);

        analysis::addChroma (everything, sum);
        if (! isVoice)
        {
            analysis::addChroma (instruments, sum);
            instrumentWeight += weight;
        }

        described.add (object ({ { "id", id }, { "name", track[ids::name].toString() },
                                 { "key", weight > 0.0 ? analysis::estimateKey (sum).getName() : juce::String() },
                                 { "voice", isVoice } }));
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
//   analyse {style, effects}  listen to every track (background); "dawAutoMix" brings the plan.
//                       With effects.split, first split the instrumental into stems
//                       ("dawAutoMix" { state: "splitting", phase, progress })
//   reference           pick a song and plan a master match to it (background)
//   tags                which role each track has before listening (describeAutoMixTags)
//   tag {track, role}   tag a track: a role key (analysis::getRoleKey), or "auto" to guess
//                       from its name and sound again. Saved with the project.
//   replan {style, mode, focus, effects, roles: {trackId: role key}}  (roles also saved as the tracks' tags)
//                       mode: "mix" (the whole song), "fit" (focus into the backing), "clean"
//                       (clean & polish focus only) or "master" (a mastering chain on the master,
//                       towards masterLufs); focus: a track id (0: the lead voice).
//                       analyse takes mode and focus too.
//                       effects: {eq, compressor, deEsser, reverb, saturation, echo, exciter,
//                       width}, the effects it may add; normalize, split (see MixOptions)
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
        if (stemJob != nullptr && stemJob->autoMix)
            stemJob->cancel = true;
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
        readAutoMixEffects (args["effects"]);
        readAutoMixMode (args);

        if (action == "analyse")
        {
            autoMixSplitNote = {};
            if (autoMixSplit && startAutoMixSplit())
                return error ({}); // listens once the stems are in
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
        readAutoMixEffects (args["effects"]);
        readAutoMixMode (args);

        // A role picked here is also the track's tag, for next time.
        if (auto* roles = args["roles"].getDynamicObject())
            for (const auto& property : roles->getProperties())
            {
                const auto trackId = property.name.toString().getIntValue();
                autoMixRoles[trackId] = analysis::getRoleFromKey (property.value.toString());
                if (auto track = edit.getTrack (trackId); track.isValid())
                    track.setProperty (ids::mixRole, analysis::getRoleKey (autoMixRoles[trackId]), nullptr);
            }

        autoMixReference = false;
        replanAutoMix();
        return describeAutoMix();
    }

    if (action == "tags")
        return describeAutoMixTags();

    if (action == "tag")
    {
        auto track = edit.getTrack (arg (args, "track", -1));
        if (! track.isValid())
            return error ("That track is gone.");

        // "auto" forgets the tag: Auto Mix guesses again from the name and the sound.
        const auto key = args["role"].toString();
        if (key == "auto" || key.isEmpty())
        {
            track.removeProperty (ids::mixRole, nullptr);
            autoMixRoles.erase ((int) track[ids::id]);
        }
        else
        {
            const auto role = analysis::getRoleFromKey (key);
            track.setProperty (ids::mixRole, analysis::getRoleKey (role), nullptr);
            autoMixRoles[(int) track[ids::id]] = role;
        }
        return describeAutoMixTags();
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

    // The page's view state (zoom, scroll, selection...), kept in the project. Not an edit.
    if (command == "viewState")
    {
        edit.getState().setProperty (ids::viewState, args["view"].toString(), nullptr);
        complete (object ({ { "ok", true } }));
        return;
    }

    // Each command is one undo step, unless it continues a drag (args.continue). The step
    // is named for the Undo button and the toasts ("Undo move clip").
    if (! arg (args, "continue", false))
        undo.beginNewTransaction (describeUndoStep (command, args));

    const auto id = arg (args, "id", -1);
    const auto ok = [&] { complete (error ({})); };

    if (handleMidi (command, args, complete))
        return;

    if (command == "session")               { complete (describeSession()); return; }
    if (command == "mode")                  { setMode (arg (args, "daw", false)); ok(); return; }
    if (command == "undo")                  { undo.undo(); ok(); return; }
    if (command == "redo")                  { undo.redo(); ok(); return; }
    if (command == "peaks")
    {
        // With a clip id, the audio that clip plays (warped and transposed); else the plain file.
        const auto clip = edit.getClip (arg (args, "clip", -1));
        const auto file = clip.isValid() ? juce::File (clip[ids::file].toString()) : juce::File (args["file"].toString());
        const auto settings = clip.isValid() ? daw::Edit::getWarpSettings (clip) : daw::warp::Settings();
        complete (args.hasProperty ("seconds") ? getDetailPeaks (file, arg (args, "start", 0.0), arg (args, "end", 0.0), arg (args, "seconds", 0.0), settings)
                                               : getPeaks (file, settings));
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

    if (command == "setTrack") // a track or a return
    {
        auto track = edit.getChannel (id);
        if (id == daw::Edit::masterId)
            track = {};

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
            if (args.hasProperty ("monitorMode")) edit.setTrackMonitorMode (id, args["monitorMode"].toString());
            if (args.hasProperty ("input"))   // [left] mono, [left, right] stereo, [] none
            {
                const auto* list = args["input"].getArray();
                daw::Edit::Input input { daw::Edit::noInput, daw::Edit::noInput };
                if (list != nullptr && list->size() > 0) input.left = (int) list->getReference (0);
                if (list != nullptr && list->size() > 1) input.right = (int) list->getReference (1);
                edit.setTrackInput (id, input);
            }
            if (args.hasProperty ("transpose")) edit.setTrackTranspose (id, args["transpose"]);
            if (arg (args, "clearRide", false)) edit.setVolumeRide (id, {});
            if (arg (args, "clearAutomation", false)) edit.clearTrackAutomation (id);
            if (args.hasProperty ("inGain"))  edit.setChainGain (id, false, args["inGain"]);
            if (args.hasProperty ("outGain")) edit.setChainGain (id, true, args["outGain"]);
        }

        ok();
        return;
    }

    if (command == "setMaster")
    {
        if (args.hasProperty ("volume"))  edit.setMasterVolume (args["volume"]);
        if (args.hasProperty ("pan"))     edit.setMasterPan (args["pan"]);
        if (args.hasProperty ("inGain"))  edit.setChainGain (daw::Edit::masterId, false, args["inGain"]);
        if (args.hasProperty ("outGain")) edit.setChainGain (daw::Edit::masterId, true, args["outGain"]);
        ok();
        return;
    }

    //================================================================ returns and sends
    if (command == "addReturn")
    {
        const auto effect = daw::Edit::moduleTypeFromKey (arg (args, "effect", juce::String()));
        const auto letter = juce::String::charToString ((juce::juce_wchar) ('A' + edit.getReturns().size() % 26));
        auto name = arg (args, "name", juce::String());
        if (name.trim().isEmpty())
            name = "Return " + letter;
        complete (edit.addReturn (name.substring (0, 60), effect)[ids::id]);
        return;
    }

    if (command == "groupTracks")
    {
        juce::Array<int> trackIds;
        if (const auto* list = args["tracks"].getArray())
            for (const auto& item : *list)
                trackIds.add ((int) item);

        auto name = arg (args, "name", juce::String());
        if (name.trim().isEmpty())
            name = "Group";
        const auto group = edit.groupTracks (trackIds, name.substring (0, 60));
        if (group < 0) { complete (error ("Pick at least one track to group.")); return; }
        complete (group);
        return;
    }

    if (command == "ungroup")   { edit.ungroup (id); ok(); return; }
    if (command == "setGroup")  { edit.setTrackGroup (id, arg (args, "group", -1)); ok(); return; }
    if (command == "foldGroup") { edit.setGroupFolded (id, arg (args, "folded", true)); ok(); return; }

    if (command == "removeReturn") { edit.removeReturn (id); ok(); return; }
    if (command == "moveReturn")   { edit.moveReturn (id, arg (args, "index", 0)); ok(); return; }

    if (command == "setSend")
    {
        const auto target = arg (args, "target", -1);
        if (args.hasProperty ("level")) edit.setSend (id, target, args["level"]);
        if (args.hasProperty ("pre"))   edit.setSendPre (id, target, args["pre"]);
        ok();
        return;
    }

    if (command == "setOutput") { edit.setTrackOutput (id, arg (args, "output", (int) daw::Edit::masterId)); ok(); return; }

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
    if (command == "pasteClip" && arg (args, "midi", false))
    {
        auto clip = edit.addMidiClip (arg (args, "track", -1), juce::jmax (0.0, (double) args["start"]), args["length"], args["name"].toString());
        if (! clip.isValid())
        {
            complete (-1);
            return;
        }

        const auto clipId = (int) clip[ids::id];
        clip.setProperty (ids::offset, juce::jmax (0.0, (double) args["offset"]), &undo);
        if (auto* notes = args["notes"].getArray())
            for (const auto& n : *notes)
                edit.addNote (clipId, n["pitch"], n["time"], n["length"], arg (n, "velocity", 100));
        complete (clipId);
        return;
    }

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
        if (args.hasProperty ("transpose")) edit.setClipTranspose (id, args["transpose"]);
        if (args.hasProperty ("stretch")) edit.setClipStretch (id, daw::warp::algorithmFromKey (args["stretch"].toString()));
        if (args.hasProperty ("formant")) edit.setClipFormant (id, args["formant"]);
        if (args.hasProperty ("musical") || args.hasProperty ("clipTempo"))
        {
            const auto clip = edit.getClip (id);
            auto on = args.hasProperty ("musical") ? (bool) args["musical"] : daw::Edit::isMusical (clip);
            auto bpm = arg (args, "clipTempo", 0.0);

            // Switching on with no tempo given: the clip's own tempo, found from its audio
            // the first time (or the project tempo there, when no steady beat shows).
            if (on && bpm <= 0.0 && ! clip.hasProperty (ids::clipTempo))
                if (const auto audio = engine.getAudioPool().loadNow (juce::File (clip[ids::file].toString())); audio != nullptr)
                    if (const auto found = analysis::rhythm::detectTempo (*audio, engine.getAudioPool().getSampleRate(), edit.getBeatsPerBar());
                        found.bpm > 0.0 && found.confidence >= 0.2)
                        bpm = std::round (found.bpm * 100.0) / 100.0;

            edit.setClipMusical (id, on, bpm);
        }
        ok();
        return;
    }

    // Clip commands that read the clip's audio: normalise (gain so the peak is -1 dBFS)
    // and strip silence (keep only the parts with sound, as separate clips).
    if (command == "normaliseClip" || command == "stripSilence")
    {
        const auto clip = edit.getClip (id);
        if (daw::Edit::isMidiClip (clip))
        {
            complete (error ("That works on audio clips, not MIDI clips."));
            return;
        }

        // The audio the clip plays: offset and length are measured in the rendered copy when it's warped.
        const auto audio = clip.isValid() ? engine.getAudioPool().get (juce::File (clip[ids::file].toString()), daw::Edit::getWarpSettings (clip))
                                          : nullptr;
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
    //================================================================ rhythm: tempo, quantize, warp
    // Selected clips (args.clips) or every clip on a track (args.track).
    const auto clipsFrom = [&] (const juce::var& a)
    {
        juce::Array<int> clips;
        if (const auto* list = a["clips"].getArray())
            for (const auto& c : *list)
                clips.add ((int) c);
        if (clips.isEmpty())
            clips = daw::rhythm::getClipIds (edit, arg (a, "track", -1));
        return clips;
    };

    // Tempo and beats of a leading track. With apply, sets the project tempo; with lineUp
    // too, slides the whole song so the track's downbeats sit on the bar lines.
    if (command == "detectTempo")
    {
        const auto track = edit.getTrack (arg (args, "track", -1));
        if (! track.isValid())
        {
            complete (error ("Pick the track that leads the tempo first."));
            return;
        }

        const auto tempo = daw::rhythm::analyseTrack (edit, engine.getAudioPool(), (int) track[ids::id]);
        if (tempo.bpm <= 0.0)
        {
            complete (error ("No steady beat found in " + track[ids::name].toString() + ". It needs a few bars of rhythm."));
            return;
        }

        const auto bpm = std::round (tempo.bpm * 100.0) / 100.0;
        auto shift = 0.0;

        if (arg (args, "apply", false))
        {
            edit.setTempoMap (daw::TempoMap (bpm)); // a steady tempo: any tempo changes go
            if (arg (args, "lineUp", false))
            {
                auto fitted = tempo;
                fitted.bpm = bpm;
                shift = daw::rhythm::getLineUpShift (edit, fitted);
                daw::rhythm::shiftSong (edit, shift);
            }
        }

        complete (object ({ { "ok", true }, { "bpm", bpm }, { "confidence", tempo.confidence },
                            { "beats", (int) tempo.beats.size() }, { "shift", shift } }));
        return;
    }

    // Quantize audio: warps the clips so their onsets land on the project grid (steps in
    // beats: 1 = quarters, 0.5 = eighths...) or on a leading track's groove (args.groove =
    // its track id). Strength, swing, catch (share of a step) and sensitivity are 0..1.
    if (command == "quantize")
    {
        const auto clips = clipsFrom (args);
        if (clips.isEmpty())
        {
            complete (error ("Select clips or a track to quantize."));
            return;
        }

        const auto steps = juce::jlimit (1.0 / 16.0, 16.0, arg (args, "steps", 0.5));
        const auto swing = juce::jlimit (0.0, 1.0, arg (args, "swing", 0.0));
        const auto songEnd = (double) engine.getLength() / engine.getSampleRate() + 10.0;
        std::vector<double> grid;

        if (const auto leader = edit.getTrack (arg (args, "groove", -1)); leader.isValid())
        {
            const auto tempo = daw::rhythm::analyseTrack (edit, engine.getAudioPool(), (int) leader[ids::id]);
            if (tempo.beats.size() < 4)
            {
                complete (error ("No steady beat found in " + leader[ids::name].toString() + " to quantize to."));
                return;
            }
            grid = analysis::rhythm::makeGroove (tempo.beats, steps, 0.0, songEnd, swing);
        }
        else
        {
            grid = edit.getTempoMap().makeGrid (steps, 0.0, songEnd, swing);
        }

        daw::rhythm::Options options;
        options.strength = juce::jlimit (0.0, 1.0, arg (args, "strength", 1.0));
        options.catchRange = juce::jlimit (0.0, 0.5, arg (args, "catch", 0.5));
        options.sensitivity = juce::jlimit (0.0f, 1.0f, arg (args, "sensitivity", 0.5f));
        options.iterative = arg (args, "iterative", false);

        const auto result = daw::rhythm::quantizeClips (edit, engine.getAudioPool(), clips, grid, options);

        // Slicing: cut at the notes and move the pieces, with no stretching.
        if (arg (args, "slice", false))
            for (auto clip : clips)
                edit.setClipStretch (clip, daw::warp::Algorithm::slice);

        complete (object ({ { "ok", true }, { "onsets", result.onsets }, { "moved", result.moved } }));
        return;
    }

    // The note pitch editor's view of a clip (args.id): the take's sung notes with their
    // edits (the stored ones, or as found when nothing is edited yet), and the pitch line
    // as sung and as edited, with the loudness, every 10 ms of the clip. Times: `start`/`end`
    // and the notes' are file seconds; `at`/`until` and `times` are in the clip's rendered
    // time, as the clip editor shows it. Pitches are MIDI note numbers before transposes
    // (`transpose`: the clip's and its track's), 0 where nothing is sung.
    if (command == "pitchNotes")
    {
        const auto clip = edit.getClip (id);
        if (! clip.isValid() || daw::Edit::isMidiClip (clip))
        {
            complete (error ("Pick an audio clip to edit its notes."));
            return;
        }
        auto source = engine.getAudioPool().loadNow (juce::File (clip[ids::file].toString()));
        if (source == nullptr)
        {
            complete (error ("The clip's audio couldn't be read."));
            return;
        }

        const auto settings = daw::Edit::getWarpSettings (clip);
        const auto map = settings.map;
        const auto offset = (double) clip[ids::offset], length = (double) clip[ids::length];
        const auto linked = ! daw::Edit::getLinkedClips (clip).isEmpty();
        auto stored = daw::Edit::getClipNotes (clip);
        const auto rate = engine.getAudioPool().getSampleRate();
        const auto transpose = settings.semitones;

        juce::Thread::launch ([source, map, offset, length, linked, stored, rate, transpose, complete, weak = std::weak_ptr<bool> (alive)]
        {
            namespace pitch = analysis::pitch;
            const auto track = pitch::analyseCached (*source, rate);
            const auto notes = stored.empty() ? pitch::findNotes (*track) : stored;
            const auto shapes = pitch::getShapes (*track, notes);
            const auto from = juce::jmax (0.0, map.toSource (offset) - 0.5), to = map.toSource (offset + length) + 0.5;

            juce::DynamicObject::Ptr result (new juce::DynamicObject());
            juce::Array<juce::var> list, times, sung, edited, energy;
            for (const auto& n : notes)
                list.add (object ({ { "start", n.start }, { "end", n.end }, { "pitch", n.pitch }, { "shift", n.shift },
                                    { "straighten", n.straighten }, { "vibrato", n.vibrato }, { "formant", n.formant },
                                    { "gain", n.gain }, { "transition", n.transition },
                                    { "at", map.toTarget (n.start) }, { "until", map.toTarget (n.end) } }));
            for (int i = track->frameAt (from); i < track->size() && track->timeOf (i) <= to; i += 2)
            {
                const auto t = track->timeOf (i);
                const auto m = track->midi[(size_t) i];
                times.add (std::round (map.toTarget (t) * 1000.0) / 1000.0);
                sung.add (std::round (m * 100.0f) / 100.0f);
                edited.add (m > 0.0f ? std::round ((m + shapes[(size_t) i].semitones) * 100.0f) / 100.0f : 0.0f);
                energy.add (std::round (track->energy[(size_t) i] * 10.0f) / 10.0f);
            }

            result->setProperty ("ok", true);
            result->setProperty ("edited", ! stored.empty());
            result->setProperty ("linked", linked);
            result->setProperty ("transpose", transpose);
            result->setProperty ("notes", list);
            result->setProperty ("times", times);
            result->setProperty ("sung", sung);
            result->setProperty ("pitch", edited);
            result->setProperty ("energy", energy);
            juce::var reply (result.get());

            juce::MessageManager::callAsync ([reply, complete, weak]
            {
                if (auto flag = weak.lock(); flag != nullptr && *flag)
                    complete (reply);
            });
        });
        return;
    }

    // Stores a clip's notes with their edits (args.notes: [{ start, end, pitch, shift,
    // straighten, vibrato, formant, gain, transition }], file seconds). Nothing edited (or no
    // notes) takes the edits off.
    if (command == "setPitchNotes")
    {
        const auto clip = edit.getClip (id);
        if (! daw::Edit::getLinkedClips (clip).isEmpty())
        {
            complete (error ("Notes on linked tracks can't be edited one by one: unlink the track first."));
            return;
        }
        std::vector<analysis::pitch::Note> notes;
        if (const auto* list = args["notes"].getArray())
            for (const auto& n : *list)
            {
                analysis::pitch::Note note;
                note.start = arg (n, "start", 0.0);
                note.end = arg (n, "end", 0.0);
                note.pitch = arg (n, "pitch", 0.0f);
                note.shift = arg (n, "shift", 0.0f);
                note.straighten = arg (n, "straighten", 0.0f);
                note.vibrato = arg (n, "vibrato", 1.0f);
                note.formant = arg (n, "formant", 0.0f);
                note.gain = arg (n, "gain", 0.0f);
                note.transition = arg (n, "transition", analysis::pitch::Note::defaultTransition);
                notes.push_back (note);
            }
        // Through the text form, which keeps every value in range.
        edit.setClipNotes (id, analysis::pitch::fromString (analysis::pitch::toString (notes)));
        ok();
        return;
    }

    // Note starts in a clip (args.id), in its rendered copy's time: [[time, strength], ...].
    if (command == "hitpoints")
    {
        // Analysed off the message thread: a long take takes a moment, and the page waits.
        auto find = daw::rhythm::prepareHitpoints (edit, engine.getAudioPool(), id, juce::jlimit (0.0f, 1.0f, arg (args, "sensitivity", 0.5f)));
        if (! find)
        {
            complete (object ({ { "ok", true }, { "hits", juce::Array<juce::var>() } }));
            return;
        }
        juce::Thread::launch ([find = std::move (find), complete, weak = std::weak_ptr<bool> (alive)]
        {
            auto hits = std::make_shared<std::vector<analysis::rhythm::Onset>> (find());
            juce::MessageManager::callAsync ([hits, complete, weak]
            {
                if (auto flag = weak.lock(); flag == nullptr || ! *flag)
                    return;
                juce::Array<juce::var> list;
                for (const auto& onset : *hits)
                    list.add (juce::Array<juce::var> { onset.time, onset.strength });
                complete (object ({ { "ok", true }, { "hits", list } }));
            });
        });
        return;
    }

    // Linked tracks warp as one (args.tracks: ids to link together); unlinkTrack takes one off.
    if (command == "linkTracks")
    {
        juce::Array<int> tracks;
        if (const auto* list = args["tracks"].getArray())
            for (const auto& t : *list)
                tracks.add ((int) t);
        if (tracks.size() < 2)
        {
            complete (error ("Pick at least two audio tracks to link."));
            return;
        }
        edit.linkTracks (tracks);
        ok();
        return;
    }
    if (command == "unlinkTrack") { edit.unlinkTrack (arg (args, "track", -1)); ok(); return; }

    // Back to the audio as recorded (time only; transposes stay).
    if (command == "clearWarp")
    {
        for (auto clip : clipsFrom (args))
            edit.setClipWarp (clip, {});
        ok();
        return;
    }

    // Warp markers: [[source, target], ...] in seconds (file time, rendered time). An
    // empty list clears the warp.
    if (command == "setWarp")
    {
        std::vector<daw::warp::Marker> markers;
        if (const auto* list = args["markers"].getArray())
            for (const auto& pair : *list)
                if (pair.isArray() && pair.size() == 2)
                    markers.push_back ({ (double) pair[0], (double) pair[1] });

        edit.setEffectiveWarp (id, daw::warp::Map (std::move (markers)));
        ok();
        return;
    }
    if (command == "clipFades")  { edit.setClipFades (id, args["fadeIn"], args["fadeOut"]); ok(); return; }

    //================================================================ timeline and transport
    // The tempo; with `at` (seconds), the tempo of the point in force there.
    if (command == "tempo")
    {
        if (args.hasProperty ("at"))
            edit.setTempoAt (args["at"], args["bpm"]);
        else
            edit.setTempo (args["bpm"]);
        ok();
        return;
    }

    // The whole tempo map: points [[beat, bpm, ramp], ...]; the first is the start tempo
    // (it moves to beat 0). One point (or none) makes the tempo steady.
    if (command == "tempoMap")
    {
        std::vector<daw::TempoMap::Point> points;
        if (const auto* list = args["points"].getArray())
            for (const auto& p : *list)
                if (p.isArray() && p.size() >= 2)
                    points.push_back ({ (double) p[0], (double) p[1], p.size() > 2 && (bool) p[2] });

        if (points.empty())
            points.push_back ({ 0.0, edit.getTempo(), false });

        edit.setTempoMap (daw::TempoMap (std::move (points)));
        ok();
        return;
    }
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
            // MIDI: only the output side (keys are played to what's heard), about half.
            engine.setMidiRecordingLatency (roundTripLatency() / 2);
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

    if (command == "takes")
    {
        complete (handleTakes (args));
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

    if (command == "stems")
    {
        complete (handleStems (args));
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

//==============================================================================
// MIDI tracks, notes and instruments.
juce::var DawBridge::describeInstruments() const
{
    auto& host = processor.getPluginHost();
    juce::Array<juce::var> list;

    for (const auto& info : host.getInstruments())
        list.add (object ({ { "id", info.id }, { "name", info.name }, { "kind", instrumentKind (info.format) },
                            { "vendor", info.vendor }, { "format", info.format } }));

    return object ({ { "ok", true }, { "instruments", list }, { "scanning", host.isScanning() },
                     { "folders", host.getExtraFolders() } });
}

juce::var DawBridge::addInstrument (int trackId, const juce::String& instrumentId)
{
    const auto info = processor.getPluginHost().findInstrument (instrumentId);
    if (! info.has_value())
        return error ("That instrument isn't available. Rescan plug-ins and try again.");

    auto track = edit.getTrack (trackId);
    const auto hasAudioClips = [&]
    {
        for (const auto& child : track)
            if (child.hasType (ids::CLIP) && ! daw::Edit::isMidiClip (child))
                return true;
        return false;
    };

    // On a MIDI track (or an empty audio track): swap its instrument. Otherwise a new MIDI
    // track, right below the one it was dropped on.
    if (track.isValid() && (daw::Edit::isMidiTrack (track) || ! hasAudioClips()))
    {
        edit.setInstrument (trackId, *info);
        if (track[ids::arm])
            edit.setTrackArm (trackId, true); // re-applies monitoring for the new kind
    }
    else
    {
        const auto index = track.isValid() ? edit.getTracks().indexOf (track) + 1 : -1;
        trackId = (int) edit.addMidiTrack (info->name.substring (0, 60), *info, index)[ids::id];
    }

    return object ({ { "ok", true }, { "error", juce::String() }, { "track", trackId } });
}

void DawBridge::showInstrumentWindow (int trackId)
{
    if (const auto it = instrumentWindows.find (trackId); it != instrumentWindows.end())
    {
        it->second->toFront (true);
        return;
    }

    const auto instrument = engine.getInstrument (trackId);
    if (instrument == nullptr)
        return;

    const auto title = edit.getTrack (trackId)[ids::name].toString() + " - " + instrument->getName();
    instrumentWindows[trackId] = std::make_unique<InstrumentWindow> (title, instrument, [this, trackId]
    {
        instrumentWindows.erase (trackId);
        sessionChanged = true;
    });
    sessionChanged = true;
}

// The MIDI commands:
//   instruments                      -> { ok, instruments: [{ id, name, kind, vendor, format }], scanning, folders }
//   addInstrument {track, instrument} on a MIDI track (or empty audio track) swaps its
//                                    instrument; track -1 (or one with audio) adds a MIDI track
//                                    -> { ok, track }
//   addMidiTrack {name, instrument}  -> the new track's id
//   scanPlugins {full}               look for instrument plugins ("dawPlugins" reports)
//   addPluginFolder / removePluginFolder {path}   extra folders to scan
//   showInstrument {track}           opens the plugin's own window
//   addMidiClip {track, start, length} -> the new clip's id
//   editNotes {clip, add: [{pitch, time, length, velocity}], change: [{id, ...}], remove: [ids]}
//                                    one undo step -> { ok, added: [ids] }
//   liveNote {note, velocity}        plays a note on the armed/monitored MIDI tracks (0 = off)
bool DawBridge::handleMidi (const juce::String& command, const juce::var& args, Complete& complete)
{
    auto& host = processor.getPluginHost();

    if (command == "instruments")
    {
        complete (describeInstruments());
        return true;
    }

    if (command == "addInstrument")
    {
        complete (addInstrument (arg (args, "track", -1), args["instrument"].toString()));
        return true;
    }

    if (command == "addMidiTrack")
    {
        auto info = host.findInstrument (args["instrument"].toString()).value_or (daw::instruments::getDefault());
        const auto name = args["name"].toString().isNotEmpty() ? args["name"].toString()
                                                               : "MIDI " + juce::String (edit.getTracks().size() + 1);
        complete (edit.addMidiTrack (name.substring (0, 60), info, arg (args, "index", -1))[ids::id]);
        return true;
    }

    if (command == "scanPlugins")
    {
        host.startScan (arg (args, "full", false));
        emit ("dawPlugins", object ({ { "scanning", true }, { "progress", 0.0 } }));
        complete (error ({}));
        return true;
    }

    if (command == "addPluginFolder")
    {
        using Flags = juce::FileBrowserComponent::FileChooserFlags;
        withChooser ("Add a plug-in folder", juce::File::getSpecialLocation (juce::File::userHomeDirectory), {},
                     Flags::openMode | Flags::canSelectDirectories,
                     [this] (const juce::File& folder)
                     {
                         auto& h = processor.getPluginHost();
                         auto folders = h.getExtraFolders();
                         folders.addIfNotAlreadyThere (folder.getFullPathName());
                         h.setExtraFolders (folders);
                         h.startScan();
                         emit ("dawPlugins", object ({ { "scanning", true }, { "progress", 0.0 } }));
                         return error ({});
                     },
                     complete);
        return true;
    }

    if (command == "removePluginFolder")
    {
        auto folders = host.getExtraFolders();
        folders.removeString (args["path"].toString());
        host.setExtraFolders (folders);
        complete (error ({}));
        return true;
    }

    if (command == "showInstrument")
    {
        const auto trackId = arg (args, "track", -1);
        if (engine.getInstrument (trackId) == nullptr)
        {
            const auto reason = engine.getInstrumentError (trackId);
            complete (error (reason.isNotEmpty() ? reason : juce::String ("The instrument is still loading.")));
            return true;
        }

        showInstrumentWindow (trackId);
        complete (error ({}));
        return true;
    }

    if (command == "addMidiClip")
    {
        const auto trackId = arg (args, "track", -1);
        const auto start = arg (args, "start", 0.0);
        const auto tempo = edit.getTempoMap();
        const auto bar = tempo.toSeconds (tempo.toBeats (start) + edit.getBeatsPerBar()) - start; // one bar from there
        const auto clip = edit.addMidiClip (trackId, start, arg (args, "length", bar));
        complete (clip.isValid() ? clip[ids::id] : juce::var (-1));
        return true;
    }

    if (command == "editNotes")
    {
        const auto clipId = arg (args, "clip", -1);
        if (! daw::Edit::isMidiClip (edit.getClip (clipId)))
        {
            complete (error ("Pick a MIDI clip first."));
            return true;
        }

        if (auto* removed = args["remove"].getArray())
            for (const auto& id : *removed)
                edit.removeNote (id);

        if (auto* changed = args["change"].getArray())
        {
            for (const auto& n : *changed)
            {
                const auto note = edit.getNote (n["id"]);
                if (! note.isValid())
                    continue;

                edit.setNote (n["id"], arg (n, "pitch", (int) number (note, ids::pitch)), arg (n, "time", number (note, ids::time)),
                              arg (n, "length", number (note, ids::length)), arg (n, "velocity", (int) number (note, ids::velocity)));
            }
        }

        juce::Array<juce::var> added;
        if (auto* notes = args["add"].getArray())
            for (const auto& n : *notes)
                added.add (edit.addNote (clipId, n["pitch"], n["time"], n["length"], arg (n, "velocity", 100))[ids::id]);

        complete (object ({ { "ok", true }, { "error", juce::String() }, { "added", added } }));
        return true;
    }

    if (command == "liveNote")
    {
        const auto note = juce::jlimit (0, 127, arg (args, "note", 60));
        const auto velocity = juce::jlimit (0, 127, arg (args, "velocity", 100));
        engine.addLiveMidi (velocity > 0 ? juce::MidiMessage::noteOn (1, note, (juce::uint8) velocity)
                                         : juce::MidiMessage::noteOff (1, note));
        complete (error ({}));
        return true;
    }

    return false;
}

juce::String DawBridge::getUndoName() const
{
    auto& undo = edit.getUndoManager();
    return undo.canUndo() ? undo.getUndoDescription() : juce::String();
}

juce::String DawBridge::getRedoName() const
{
    auto& undo = edit.getUndoManager();
    return undo.canRedo() ? undo.getRedoDescription() : juce::String();
}
