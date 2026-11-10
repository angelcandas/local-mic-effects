#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "PluginProcessor.h"
#include "daw/Exporter.h"

// Connects the web UI's DAW screen (ui/daw.js) to the processor's DAW session.
//
// The UI calls one native function, daw (command, args), and listens for:
//   "dawSession"  the whole session (tracks, clips, inserts, master, project), after
//                 any change
//   "dawStatus"   transport, recording and meters, 30 times a second (while in DAW mode)
//   "dawExport"   { progress } while an export runs, then { done, error, files }
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

private:
    juce::var describeStatus();
    juce::var describeInserts (const juce::ValueTree& trackOrMaster) const;
    juce::var getPeaks (const juce::File&);

    void setMode (bool daw);
    void rememberProject();
    juce::File getRecordingFolder() const;
    void loadPresetInto (int trackId, int presetIndex);
    void startExport (const juce::var& args, const juce::File& destination);

    juce::String withChooser (const juce::String& title, const juce::File& start, const juce::String& patterns, int flags,
                              std::function<juce::String (const juce::File&)> action, Complete);

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
    bool lastDirty = false, lastCanUndo = false, lastCanRedo = false;

    std::unique_ptr<juce::FileChooser> chooser;
    std::map<juce::String, juce::var> peakCache;

    // A running export (at most one).
    struct ExportJob;
    std::shared_ptr<ExportJob> exportJob;

    JUCE_DECLARE_NON_COPYABLE (DawBridge)
};
