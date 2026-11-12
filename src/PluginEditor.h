#pragma once

#include <juce_gui_extra/juce_gui_extra.h>

#include "DawBridge.h"
#include "PluginProcessor.h"

// Hosts the HTML/JS UI from ui/ in a native WebView. Every processor parameter is
// exposed to JavaScript through a relay named after its parameter ID:
//   bool parameters   -> Juce.getToggleState (id)
//   choice parameters -> Juce.getComboBoxState (id)
//   other parameters  -> Juce.getSliderState (id)
// Native functions: getParameterDefaults, getPresets, loadPreset (index),
//   getRecorderSettings, startRecording, stopRecording, chooseRecordingFolder,
//   setRecordDry (bool), revealRecording, getTracks, loadBackingTrack, setTrack (id, { gain, muted }),
//   removeTrack (id), transport (action, seconds), getParameterInfo, looper (action, args), midi (action, args),
//   daw (command, args) for the DAW screen (see DawBridge),
//   window (action): "fullScreen" toggles full screen; returns { available, fullScreen }.
// Pushed events: "meters" { input, output, note, target, preset, recording, transport, looper } at 30 Hz
// (recording = seconds recorded so far, or -1; transport = { position, length, playing } in
// seconds), "tracks" when the track list changes, "trackError" (message), and the DAW
// events "dawSession", "dawStatus" and "dawExport" (see DawBridge), "window" { available, fullScreen }
// when the app window enters or leaves full screen, and "appMenu" (action) when an item of the
// standalone app's menu bar is picked (see StandaloneApp.cpp), "midiLearned" { target, name, midi }
// when MIDI learn maps a control, and "midiAction" (name) for a mapped action the page runs.
class PluginEditor final : public juce::AudioProcessorEditor,
                           private juce::Timer
{
public:
    explicit PluginEditor (PluginProcessor&);
    ~PluginEditor() override;

    void resized() override;

    // For the standalone app's menu bar: hands a menu item to the page, which runs it like
    // the matching button or shortcut.
    void sendMenuCommand (const juce::String& action);
    bool isDawMode() const noexcept { return processorRef.isDawMode(); }
    // What Undo / Redo would do in the DAW ("Move clip"), or empty.
    juce::String getUndoName() const { return daw != nullptr ? daw->getUndoName() : juce::String(); }
    juce::String getRedoName() const { return daw != nullptr ? daw->getRedoName() : juce::String(); }

    // The standalone app's window, or nullptr inside a plugin host.
    juce::ResizableWindow* getAppWindow() const;
    void toggleFullScreen();

private:
    void timerCallback() override;

    juce::WebBrowserComponent::Options createOptions();
    static std::optional<juce::WebBrowserComponent::Resource> getResource (const juce::String& url);

    // Recording folder and "also save the dry mic" are app-wide preferences, not
    // part of the plugin state, so they survive across sessions and projects.
    juce::File getRecordingFolder();
    juce::var describeRecorderSettings();
    juce::var describeTracks();
    juce::var describeLooper (bool withOverview);
    juce::var describeMidi();
    void saveLooperSettings();
    int looperTicks = 0;
    juce::var describeWindow() const;
    juce::int64 getRoundTripLatency();

    juce::File lastTakeOnTimeline;
    bool lastFullScreen = false;
    fx::SpectrumAnalyser spectrumAnalyser; // for the effect cards' live spectra

    std::unique_ptr<juce::FileChooser> folderChooser;

    PluginProcessor& processorRef;
    juce::PropertiesFile settings;

    // Declaration order matters: relays must outlive the WebView, and the
    // attachments must be destroyed before either of them.
    std::vector<std::unique_ptr<juce::WebSliderRelay>> sliderRelays;
    std::vector<std::unique_ptr<juce::WebToggleButtonRelay>> toggleRelays;
    std::vector<std::unique_ptr<juce::WebComboBoxRelay>> comboRelays;
    std::unique_ptr<juce::WebBrowserComponent> webView;
    std::vector<std::unique_ptr<juce::WebSliderParameterAttachment>> sliderAttachments;
    std::vector<std::unique_ptr<juce::WebToggleButtonParameterAttachment>> toggleAttachments;
    std::vector<std::unique_ptr<juce::WebComboBoxParameterAttachment>> comboAttachments;

    std::unique_ptr<DawBridge> daw; // after webView: it emits through it

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginEditor)
};
