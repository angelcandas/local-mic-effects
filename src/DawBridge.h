#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "PluginProcessor.h"
#include "daw/Exporter.h"
#include "analysis/MixAssistant.h"

// Connects the web UI's DAW screen (ui/daw.js) to the processor's DAW session.
//
// The UI calls one native function, daw (command, args), and listens for:
//   "dawSession"  the whole session (tracks, clips, inserts, master, project), after
//                 any change
//   "dawStatus"   transport, recording and meters, 30 times a second (while in DAW mode)
//   "dawExport"   { progress } while an export runs, then { done, error, files }
//   "dawPlugins"  { scanning, progress, count, failed } while an instrument plugin scan
//                 runs and when it ends (refresh the "instruments" list then)
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

    // The tests run outside the standalone app; this lets them switch DAW mode on.
    static inline bool availableForTesting = false;

private:
    juce::var describeStatus();
    juce::var describeInserts (const juce::ValueTree& trackOrMaster) const;
    juce::var describeInstrument (const juce::ValueTree& track) const;
    juce::var describeNotes (const juce::ValueTree& clip) const;

    // MIDI tracks, notes and instruments (see the commands in DawBridge.cpp). Returns
    // false if `command` isn't one of them.
    bool handleMidi (const juce::String& command, const juce::var& args, Complete&);
    juce::var describeInstruments() const;
    juce::var addInstrument (int trackId, const juce::String& instrumentId);
    void showInstrumentWindow (int trackId);
    void updateInstrumentWindows();
    juce::var getPeaks (const juce::File&);
    juce::var getDetailPeaks (const juce::File&, double start, double end, double seconds);

    void setMode (bool daw);
    void rememberProject();
    juce::var describeRecentProjects() const;
    juce::File getRecordingFolder() const;
    void loadPresetInto (int trackId, int presetIndex);
    void startExport (const juce::var& args, const juce::File& destination);

    // Auto Mix (src/analysis/MixAssistant.h): analysis runs on a background thread;
    // planning and applying on the message thread.
    juce::var handleAutoMix (const juce::var& args, Complete&);
    void startAutoMix (const juce::File& reference);
    void replanAutoMix();
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

    // Instrument plugin editors, one window per MIDI track, and what the tracks' instruments
    // were last tick (a change redraws the session and closes stale windows).
    std::map<int, std::unique_ptr<juce::Component>> instrumentWindows;
    juce::String instrumentsSeen;
    int scanTicks = 0;
    std::map<juce::String, juce::var> peakCache;

    // A running export (at most one).
    struct ExportJob;
    std::shared_ptr<ExportJob> exportJob;

    // Auto Mix: the running analysis (at most one), the last analysis and its plan.
    struct AutoMixJob;
    std::shared_ptr<AutoMixJob> autoMixJob;
    std::optional<analysis::SessionAnalyser::Result> autoMixAnalysis;
    std::map<int, analysis::Role> autoMixRoles;
    analysis::MixStyle autoMixStyle = analysis::MixStyle::pop;
    analysis::MixPlan autoMixPlan;
    bool autoMixReference = false; // the plan matches a reference song
    juce::String autoMixReferenceName;

    JUCE_DECLARE_NON_COPYABLE (DawBridge)
};
