#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "PluginProcessor.h"
#include "daw/Exporter.h"
#include "analysis/MixAssistant.h"
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
//   "dawAutoMix"  { state: "analysing", progress } while Auto Mix listens, then
//                 { state: "ready", plan... } (see describeAutoMix), or "cancelled"/"error"
// See handle() for the commands. Message thread only.
class DawBridge final : private juce::ValueTree::Listener
{
public:
    using Emit = std::function<void (const juce::Identifier& event, const juce::var& payload)>;
    using Complete = std::function<void (juce::var)>;

    DawBridge (PluginProcessor&, juce::PropertiesFile& settings, Emit, std::function<juce::int64()> roundTripLatency);
    ~DawBridge() override;

    void handle (const juce::String& command, const juce::var& args, Complete);

    // From the editor's 30 Hz timer.
    void tick();

    juce::var describeSession();

    static bool isAvailable(); // DAW mode is a standalone-app feature

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
    juce::var addStemTracks (const juce::ValueTree& clip, int trackId, const std::array<juce::File, stems::numStems>&);

    // The tests run outside the standalone app; this lets them switch DAW mode on.
    static inline bool availableForTesting = false;

private:
    juce::var describeStatus();
    juce::var describeInserts (const juce::ValueTree& trackOrMaster) const;
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

    // Auto Mix (src/analysis/MixAssistant.h): analysis runs on a background thread;
    // planning and applying on the message thread.
    juce::var handleAutoMix (const juce::var& args, Complete&);
    void startAutoMix (const juce::File& reference);
    void replanAutoMix();
    void readAutoMixEffects (const juce::var& effects);
    juce::var describeAutoMix() const;
    void withChooser (const juce::String& title, const juce::File& start, const juce::String& patterns, int flags,
                      std::function<juce::var (const juce::File&)> action, Complete);

    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override { sessionChanged = true; }
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
    std::map<juce::String, juce::var> peakCache;

    // A running export (at most one).
    struct ExportJob;
    std::shared_ptr<ExportJob> exportJob;

    // A running stem split (at most one).
    struct StemJob;
    std::shared_ptr<StemJob> stemJob;

    // Auto Mix: the running analysis (at most one), the last analysis and its plan.
    struct AutoMixJob;
    std::shared_ptr<AutoMixJob> autoMixJob;
    std::optional<analysis::SessionAnalyser::Result> autoMixAnalysis;
    std::map<int, analysis::Role> autoMixRoles;
    analysis::MixStyle autoMixStyle = analysis::MixStyle::pop;
    analysis::MixPlan autoMixPlan;
    analysis::MixOptions autoMixOptions; // the effects it may add (the inserts are filled per plan)
    bool autoMixReference = false; // the plan matches a reference song
    juce::String autoMixReferenceName;

    JUCE_DECLARE_NON_COPYABLE (DawBridge)
};
