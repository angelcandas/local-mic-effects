#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include "InstrumentProcessor.h"

namespace micfx::inst
{
// The colours and drawing of the instruments' windows.
class InstrumentLook final : public juce::LookAndFeel_V4
{
public:
    explicit InstrumentLook (juce::Colour accent);

    static constexpr juce::uint32 background = 0xff15171f, panel = 0xff1d2029, panelEdge = 0xff2a2e3a,
                                  text = 0xffe6e8ef, dimText = 0xff8b90a0, track = 0xff2c303d;

    void drawRotarySlider (juce::Graphics&, int x, int y, int width, int height, float position, float startAngle,
                           float endAngle, juce::Slider&) override;
    void drawLinearSlider (juce::Graphics&, int x, int y, int width, int height, float position, float minPosition,
                           float maxPosition, juce::Slider::SliderStyle, juce::Slider&) override;
    void drawComboBox (juce::Graphics&, int width, int height, bool down, int buttonX, int buttonY, int buttonW,
                       int buttonH, juce::ComboBox&) override;
    void positionComboBoxText (juce::ComboBox&, juce::Label&) override;
    juce::Font getComboBoxFont (juce::ComboBox&) override;
    void drawToggleButton (juce::Graphics&, juce::ToggleButton&, bool highlighted, bool down) override;
    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour&, bool highlighted, bool down) override;
    juce::Font getPopupMenuFont() override;
    juce::Font getTextButtonFont (juce::TextButton&, int) override;

    const juce::Colour accent;
};

// An instrument's window: header (name, presets, scope, volume), the sections of
// controls the Spec lays out, and a keyboard that plays it.
class InstrumentEditor final : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit InstrumentEditor (InstrumentProcessor&);
    ~InstrumentEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    class Control;
    class EnvelopeView;
    class Scope;
    class Keyboard;

    void timerCallback() override;
    void updatePresetBox();
    void stepPreset (int delta);

    InstrumentProcessor& processor;
    const Spec& spec;
    InstrumentLook look;

    struct SectionView
    {
        const Section* section = nullptr;
        juce::Rectangle<int> bounds;
        std::vector<Control*> controls;
        std::unique_ptr<EnvelopeView> envelope;
    };

    std::vector<std::unique_ptr<Control>> controls;
    std::vector<SectionView> sections;
    std::vector<int> rowHeights;
    std::unique_ptr<Control> volumeKnob;
    std::unique_ptr<Scope> scope;
    juce::ComboBox presetBox;
    juce::TextButton previousPreset { "<" }, nextPreset { ">" };
    std::unique_ptr<Keyboard> keyboardView;
    juce::MidiKeyboardComponent& keyboard;
    int shownProgram = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (InstrumentEditor)
};
}
