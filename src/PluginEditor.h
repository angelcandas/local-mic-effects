#pragma once

#include <juce_gui_extra/juce_gui_extra.h>

#include "PluginProcessor.h"

// Hosts the HTML/JS UI from ui/ in a native WebView. Every processor parameter is
// exposed to JavaScript through a relay named after its parameter ID:
//   bool parameters   -> Juce.getToggleState (id)
//   choice parameters -> Juce.getComboBoxState (id)
//   other parameters  -> Juce.getSliderState (id)
// Native functions: getParameterDefaults, getPresets, loadPreset (index),
//   getRecorderSettings, startRecording, stopRecording, chooseRecordingFolder,
//   setRecordDry (bool), revealRecording.
// Pushed events: "meters" { input, output, note, target, preset, recording } at 30 Hz
// (recording = seconds recorded so far, or -1 when not recording).
class PluginEditor final : public juce::AudioProcessorEditor,
                           private juce::Timer
{
public:
    explicit PluginEditor (PluginProcessor&);

    void resized() override;

private:
    void timerCallback() override;

    juce::WebBrowserComponent::Options createOptions();
    static std::optional<juce::WebBrowserComponent::Resource> getResource (const juce::String& url);

    // Recording folder and "also save the dry mic" are app-wide preferences, not
    // part of the plugin state, so they survive across sessions and projects.
    juce::File getRecordingFolder();
    juce::var describeRecorderSettings();

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

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginEditor)
};
