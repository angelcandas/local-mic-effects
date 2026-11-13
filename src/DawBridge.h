#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "PluginProcessor.h"
#include "daw/Exporter.h"
#include "analysis/MixAssistant.h"
#include "analysis/ToMidi.h"
#include "stems/StemSplitter.h"

// Connects the web UI's DAW screen (ui/daw.js) to the processor's DAW session.
//
// The UI calls one native function, daw (command, args), and listens for:
//   "dawSession"  the whole session (tracks, clips, inserts, master, project), after
//                 any change
//   "dawStatus"   transport, recording and meters, 30 times a second (while in DAW mode)
//   "dawExport"   { progress } while an export runs, then { done, error, files }
//   "dawStems"    { state: "downloading" | "loading" | "splitting", progress } while a
//                 song splits into stems, then { state: "done", tracks } / "cancelled" /
//                 { state: "error", error }
//   "dawPlugins"  { scanning, progress, count, failed } while an instrument plugin scan
//                 runs and when it ends (refresh the "instruments" list then)
//   "dawAutoMix"  { state: "analysing", progress } while Auto Mix listens, then
//                 { state: "ready", plan... } (see describeAutoMix), or "cancelled"/"error"
//   "dawTakeComp" { state: "analysing", progress } while Auto Comp listens to the takes,
//                 then { state: "done", tracks: [{ track, picks, summary }] }, "cancelled"
//                 or { state: "error", error }
//   "dawKey"      { state: "analysing", progress } while the key is detected, then
//                 { state: "ready", key... } (see describeKey), or "cancelled"
// See handle() for the commands. Message thread only.
class DawBridge final : private juce::ValueTree::Listener
{
public:
    // What Undo and Redo would do ("Move clip"), empty when there is nothing; for the app's Edit menu.
    juce::String getUndoName() const;
    juce::String getRedoName() const;

    using Emit = std::function<void (const juce::Identifier& event, const juce::var& payload)>;
    using Complete = std::function<void (juce::var)>;

    DawBridge (PluginProcessor&, juce::PropertiesFile& settings, Emit, std::function<juce::int64()> roundTripLatency);
    ~DawBridge() override;

    void handle (const juce::String& command, const juce::var& args, Complete);

    // From the editor's 30 Hz timer.
    void tick();

    juce::var describeSession();
    static juce::var describeRanges (const std::vector<std::pair<double, double>>&);

    static bool isAvailable(); // DAW mode is a standalone-app feature

    // The audio interface's open inputs by name, in the order the engine gets them (set
    // by the app's editor). Without it, "Input 1", "Input 2"... from the processor.
    std::function<juce::StringArray()> inputNames;

    // What the file commands do once a file is picked (public for the tests, which can't
    // click through a file chooser). Each returns { ok, error, ... } like handle().
    juce::var openProject (const juce::File& picked);       // a project folder, or any file in one
    juce::var newProject (const juce::File& folder);
    juce::var saveProjectAs (const juce::File& folder);
    // start < 0: at the playhead. trackId not a track: on a new track.
    juce::var importAudio (const juce::File& file, int trackId, double start = -1.0); // -> { ok, error, track, clip, start }

    // The library sidebar (ui/browser.js): the places it shows, and one folder's
    // subfolders and audio files.
    juce::var describePlaces() const;                  // -> { ok, places: [{ name, path, kind, removable }] }
    juce::var browseFolder (const juce::File&) const;  // -> { ok, error, path, name, folders, files }

    // Puts finished stems (in stems::Stem order) on new tracks under the clip's track,
    // lined up with the clip, and mutes the clip. One undo step. -> { ok, error, tracks }
    juce::var addStemTracks (const juce::ValueTree& clip, int trackId, const std::vector<juce::File>&);

    // Puts loop layers (files in layer order, with their level in dB and mute) on new tracks
    // "Loop 1"..., each repeated `repeats` times from the start of the song. With bpm > 0,
    // sets the project's tempo and beats per bar first. One undo step. -> { ok, error, tracks }
    struct LoopLayer { juce::File file; float gainDecibels = 0.0f; bool muted = false; };
    juce::var addLoopTracks (const std::vector<LoopLayer>&, double loopSeconds, int repeats, double bpm, int beatsPerBar);

    // The tests run outside the standalone app; this lets them switch DAW mode on.
    static inline bool availableForTesting = false;

private:
    // False once the bridge is gone: work finishing on other threads checks it.
    std::shared_ptr<bool> alive = std::make_shared<bool> (true);

    juce::var describeStatus();
    juce::var describeInserts (const juce::ValueTree& trackOrMaster) const;
    juce::var describeInstrument (const juce::ValueTree& track) const;
    juce::var describeNotes (const juce::ValueTree& clip) const;

    // MIDI tracks, notes and instruments (see the commands in DawBridge.cpp). Returns
    // false if `command` isn't one of them.
    bool handleMidi (const juce::String& command, const juce::var& args, Complete&);
    // The step sequencer's commands (patterns, parts, steps, pattern blocks, pattern mode).
    bool handlePatterns (const juce::String& command, const juce::var& args, Complete&);
    juce::var describePatterns() const;
    juce::var describePatternClips() const;
    juce::var describeKeyNames (int trackId) const;
    juce::var describeInstruments() const;
    juce::var addInstrument (int trackId, const juce::String& instrumentId);
    void showInstrumentWindow (int trackId);
    void updateInstrumentWindows();
    juce::var getPeaks (const juce::File&, const daw::warp::Settings& = {});
    juce::var getDetailPeaks (const juce::File&, double start, double end, double seconds, const daw::warp::Settings& = {});

    void setMode (bool daw);
    void rememberProject();
    juce::var describeRecentProjects() const;
    juce::File getRecordingFolder() const;
    void loadPresetInto (int trackId, int presetIndex);
    void startExport (const juce::var& args, const juce::File& destination);

    // Split into stems (src/stems/StemSplitter.h): downloads the model the first time,
    // then separates on background threads.
    juce::var handleStems (const juce::var& args);
    juce::var handleRebalance (const juce::var& args);

    // Auto Mix (src/analysis/MixAssistant.h): analysis runs on a background thread;
    // planning and applying on the message thread.
    juce::var handleAutoMix (const juce::var& args, Complete&);
    void startAutoMix (const juce::File& reference);
    bool startAutoMixSplit();
    void replanAutoMix();
    void readAutoMixEffects (const juce::var& effects);
    void readAutoMixMode (const juce::var& args); // mode, focus
    static std::optional<analysis::Role> guessRoleFromTrack (const juce::ValueTree& track);
    juce::var describeAutoMix() const;
    juce::var describeAutoMixTags() const;

    // Take lanes and comping (daw::Edit "Takes and comping"), and Auto Comp
    // (src/analysis/TakeComp.h), which listens on a background thread.
    juce::var handleTakes (const juce::var& args);
    void finishTakeComp();

    // Convert to MIDI (src/analysis/ToMidi.h): the analysis runs on a background thread, the
    // notes go in on the message thread as one undo step.
    juce::var handleToMidi (const juce::var& args);
    void finishToMidi();
    struct ToMidiJob;
    std::shared_ptr<ToMidiJob> toMidiJob;

    // Song key (src/analysis/Key.h): detection runs on a background thread.
    juce::var handleKey (const juce::var& args);
    void finishKeyDetection();
    juce::var describeKey() const;
    int setAutotunesToKey();
    void withChooser (const juce::String& title, const juce::File& start, const juce::String& patterns, int flags,
                      std::function<juce::var (const juce::File&)> action, Complete);

    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier& property) override { if (property != daw::ids::viewState) sessionChanged = true; }
    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override            { sessionChanged = true; }
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override     { sessionChanged = true; }
    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override             { sessionChanged = true; }

    PluginProcessor& processor;
    daw::Edit& edit;
    daw::DawEngine& engine;
    daw::Project& project;
    juce::PropertiesFile& settings;
    Emit emit;
    std::function<juce::int64()> roundTripLatency;

    bool sessionChanged = true;
    int watchedTrack = -1;                    // whose inserts get live readings
    fx::SpectrumAnalyser spectrumAnalyser;
    bool lastDirty = false, lastCanUndo = false, lastCanRedo = false;

    std::unique_ptr<juce::FileChooser> chooser;

    // Instrument plugin editors, one window per MIDI track, and what the tracks' instruments
    // were last tick (a change redraws the session and closes stale windows).
    std::map<int, std::unique_ptr<juce::Component>> instrumentWindows;
    juce::String instrumentsSeen;
    int scanTicks = 0, inputTicks = 0;
    juce::StringArray lastInputNames;
    juce::StringArray getInputNames() const;
    std::map<juce::String, juce::var> peakCache;

    // A running export (at most one).
    struct ExportJob;
    std::shared_ptr<ExportJob> exportJob;

    // A running stem split (at most one).
    struct StemJob;
    void finishAutoMixSplit (StemJob&);
    void finishRebalance (StemJob&);
    std::shared_ptr<StemJob> stemJob;

    // Auto Mix: the running analysis (at most one), the last analysis and its plan.
    struct AutoMixJob;
    std::shared_ptr<AutoMixJob> autoMixJob;
    std::optional<analysis::SessionAnalyser::Result> autoMixAnalysis;
    // Track profiles already measured (Auto Mix and key detection share them), so
    // listening again only renders the tracks whose audio changed.
    std::shared_ptr<analysis::ProfileCache> profileCache = std::make_shared<analysis::ProfileCache>();
    std::map<int, analysis::Role> autoMixRoles;
    analysis::MixStyle autoMixStyle = analysis::MixStyle::pop;
    analysis::MixPlan autoMixPlan;
    analysis::MixOptions autoMixOptions; // the effects it may add (the inserts are filled per plan)
    bool autoMixReference = false; // the plan matches a reference song
    std::map<int, float> autoMixPans;  // pans the last applied plan set (not yours: planned again)
    std::map<juce::String, bool> stereoFiles; // audio files by path: a real stereo image (hasStereoImage)
    bool hasStereoImage (const juce::ValueTree& track);
    bool autoMixSplit = false;
    juce::String autoMixSplitNote;     // split full songs (instrumentals) into stems before listening
    juce::String autoMixReferenceName;

    // Key detection: the running analysis (at most one) and the last result.
    struct KeyJob;
    std::shared_ptr<KeyJob> keyJob;
    struct TakeCompJob;
    std::shared_ptr<TakeCompJob> takeCompJob;
    analysis::KeyEstimate lastKey;
    juce::Array<juce::var> lastKeyTracks; // per track: { id, name, key, used }
    bool keyDetected = false;

    JUCE_DECLARE_NON_COPYABLE (DawBridge)
};
