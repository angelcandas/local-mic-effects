#include "InstrumentEditor.h"

#include <complex>

namespace micfx::inst
{
namespace
{
    constexpr int margin = 16, gap = 10, headerHeight = 62, keyboardHeight = 84;
    constexpr int sectionPadding = 10, titleHeight = 24, envelopeHeight = 56;

    juce::Font uiFont (float size, bool bold = false)
    {
        return juce::Font (juce::FontOptions (size, bold ? juce::Font::bold : juce::Font::plain));
    }

    const ParamSpec* findParam (const Spec& spec, const juce::String& id)
    {
        for (const auto& p : spec.params)
            if (id == p.id)
                return &p;
        return nullptr;
    }

}

//==============================================================================
InstrumentLook::InstrumentLook (juce::Colour a) : accent (a)
{
    setColour (juce::PopupMenu::backgroundColourId, juce::Colour (panel));
    setColour (juce::PopupMenu::textColourId, juce::Colour (text));
    setColour (juce::PopupMenu::highlightedBackgroundColourId, accent.withAlpha (0.25f));
    setColour (juce::PopupMenu::highlightedTextColourId, juce::Colour (text));
    setColour (juce::ComboBox::textColourId, juce::Colour (text));
    setColour (juce::ComboBox::arrowColourId, accent);
    setColour (juce::TextButton::textColourOffId, juce::Colour (text));
    setColour (juce::TextButton::textColourOnId, juce::Colour (text));
    setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
}

void InstrumentLook::drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height, float position,
                                       float startAngle, float endAngle, juce::Slider& slider)
{
    const auto bounds = juce::Rectangle<int> (x, y, width, height).toFloat().reduced (3.0f);
    const auto radius = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;
    const auto centre = bounds.getCentre();
    const auto angle = startAngle + position * (endAngle - startAngle);
    const auto arcRadius = radius - 2.5f;
    const auto hover = slider.isMouseOverOrDragging();

    // The track, then the value from the start (or from the middle for +/- values).
    juce::Path trackArc;
    trackArc.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f, startAngle, endAngle, true);
    g.setColour (juce::Colour (track));
    g.strokePath (trackArc, juce::PathStrokeType (3.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    const auto bipolar = slider.getMinimum() < 0.0 && slider.getMaximum() > 0.0;
    const auto from = bipolar ? startAngle + (float) slider.valueToProportionOfLength (0.0) * (endAngle - startAngle) : startAngle;
    juce::Path valueArc;
    valueArc.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f, juce::jmin (from, angle), juce::jmax (from, angle), true);
    g.setColour (accent.withAlpha (hover ? 1.0f : 0.9f));
    g.strokePath (valueArc, juce::PathStrokeType (3.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    // The cap.
    const auto capRadius = arcRadius - 6.0f;
    juce::ColourGradient cap (juce::Colour (0xff3a3f4e), centre.x, centre.y - capRadius,
                              juce::Colour (0xff22252f), centre.x, centre.y + capRadius, false);
    g.setGradientFill (cap);
    g.fillEllipse (centre.x - capRadius, centre.y - capRadius, capRadius * 2.0f, capRadius * 2.0f);
    g.setColour (juce::Colour (0xff0d0f14).withAlpha (0.8f));
    g.drawEllipse (centre.x - capRadius, centre.y - capRadius, capRadius * 2.0f, capRadius * 2.0f, 1.0f);

    // The pointer.
    juce::Path pointer;
    pointer.addRoundedRectangle (-1.25f, -capRadius + 2.5f, 2.5f, capRadius * 0.55f, 1.2f);
    g.setColour (hover ? accent.brighter (0.3f) : juce::Colour (text));
    g.fillPath (pointer, juce::AffineTransform::rotation (angle).translated (centre));
}

void InstrumentLook::drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height, float position, float,
                                       float, juce::Slider::SliderStyle style, juce::Slider& slider)
{
    if (style != juce::Slider::LinearVertical)
    {
        LookAndFeel_V4::drawLinearSlider (g, x, y, width, height, position, 0.0f, 0.0f, style, slider);
        return;
    }

    // A drawbar: a slot, a bar pulled down to the value and a numbered handle.
    const auto area = juce::Rectangle<int> (x, y, width, height).toFloat();
    const auto slot = area.withSizeKeepingCentre (8.0f, area.getHeight());
    g.setColour (juce::Colour (0xff0e1016));
    g.fillRoundedRectangle (slot, 4.0f);

    const auto top = area.getY();
    const auto bar = juce::Rectangle<float> (area.getCentreX() - 6.0f, top, 12.0f, juce::jmax (0.0f, position - top));
    g.setColour (slider.findColour (juce::Slider::trackColourId));
    g.fillRoundedRectangle (bar, 3.0f);

    const auto handle = juce::Rectangle<float> (area.getCentreX() - 14.0f, position - 9.0f, 28.0f, 18.0f);
    g.setColour (slider.findColour (juce::Slider::thumbColourId));
    g.fillRoundedRectangle (handle, 4.0f);
    g.setColour (juce::Colour (0xff111318));
    g.setFont (uiFont (11.0f, true));
    g.drawText (juce::String (juce::roundToInt (slider.getValue())), handle, juce::Justification::centred);
}

void InstrumentLook::drawComboBox (juce::Graphics& g, int width, int height, bool, int, int, int, int, juce::ComboBox& box)
{
    const auto area = juce::Rectangle<int> (width, height).toFloat().reduced (0.5f);
    g.setColour (juce::Colour (0xff111319));
    g.fillRoundedRectangle (area, 5.0f);
    g.setColour (box.isMouseOver (true) ? accent.withAlpha (0.7f) : juce::Colour (panelEdge));
    g.drawRoundedRectangle (area, 5.0f, 1.0f);

    juce::Path arrow;
    const auto cx = (float) width - 10.0f, cy = (float) height * 0.5f;
    arrow.startNewSubPath (cx - 3.5f, cy - 1.5f);
    arrow.lineTo (cx, cy + 2.0f);
    arrow.lineTo (cx + 3.5f, cy - 1.5f);
    g.setColour (accent);
    g.strokePath (arrow, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

void InstrumentLook::positionComboBoxText (juce::ComboBox& box, juce::Label& label)
{
    label.setBounds (4, 0, box.getWidth() - 18, box.getHeight());
    label.setFont (getComboBoxFont (box));
    label.setJustificationType (juce::Justification::centredLeft);
}

juce::Font InstrumentLook::getComboBoxFont (juce::ComboBox& box) { return uiFont (box.getHeight() > 26 ? 14.0f : 12.5f); }
juce::Font InstrumentLook::getPopupMenuFont() { return uiFont (14.0f); }
juce::Font InstrumentLook::getTextButtonFont (juce::TextButton&, int) { return uiFont (14.0f, true); }

void InstrumentLook::drawToggleButton (juce::Graphics& g, juce::ToggleButton& button, bool highlighted, bool)
{
    const auto on = button.getToggleState();
    const auto pill = button.getLocalBounds().toFloat().withSizeKeepingCentre (38.0f, 20.0f);
    g.setColour (on ? accent : juce::Colour (track));
    g.fillRoundedRectangle (pill, 10.0f);
    if (highlighted)
    {
        g.setColour (juce::Colours::white.withAlpha (0.08f));
        g.fillRoundedRectangle (pill, 10.0f);
    }
    const auto knobX = on ? pill.getRight() - 18.0f : pill.getX() + 2.0f;
    g.setColour (on ? juce::Colour (0xff111318) : juce::Colour (dimText));
    g.fillEllipse (knobX, pill.getY() + 2.0f, 16.0f, 16.0f);
}

void InstrumentLook::drawButtonBackground (juce::Graphics& g, juce::Button& button, const juce::Colour&, bool highlighted, bool down)
{
    const auto area = button.getLocalBounds().toFloat().reduced (0.5f);
    g.setColour (juce::Colour (down ? 0xff2a2e3a : 0xff111319));
    g.fillRoundedRectangle (area, 5.0f);
    g.setColour (highlighted ? accent.withAlpha (0.7f) : juce::Colour (panelEdge));
    g.drawRoundedRectangle (area, 5.0f, 1.0f);
}

//==============================================================================
// One parameter's control: a knob, a menu, a switch or a drawbar, with its name under
// it (its value while the mouse is over it or it's being moved).
class InstrumentEditor::Control final : public juce::Component, private juce::Slider::Listener
{
public:
    Control (InstrumentProcessor& processor, const ParamSpec& p) : param (p)
    {
        auto& state = processor.getState();
        if (! p.choices.isEmpty())
        {
            box = std::make_unique<juce::ComboBox>();
            box->addItemList (p.choices, 1);
            addAndMakeVisible (*box);
            boxAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (state, p.id, *box);
            box->addMouseListener (this, true);
        }
        else if (p.toggle)
        {
            button = std::make_unique<juce::ToggleButton>();
            addAndMakeVisible (*button);
            buttonAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (state, p.id, *button);
            button->addMouseListener (this, true);
        }
        else
        {
            slider = std::make_unique<juce::Slider> (p.vertical ? juce::Slider::LinearVertical : juce::Slider::RotaryHorizontalVerticalDrag,
                                                     juce::Slider::NoTextBox);
            slider->setRotaryParameters (juce::degreesToRadians (225.0f), juce::degreesToRadians (495.0f), true);
            slider->setPopupDisplayEnabled (false, false, nullptr);
            slider->setMouseDragSensitivity (180);
            slider->setVelocityModeParameters (0.6, 1, 0.05, true, juce::ModifierKeys::shiftModifier);
            addAndMakeVisible (*slider);
            sliderAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (state, p.id, *slider);
            slider->setDoubleClickReturnValue (true, p.def);
            slider->addListener (this);
            slider->addMouseListener (this, true);
            slider->setTooltip (p.name);
        }
    }

    ~Control() override
    {
        if (slider != nullptr)
            slider->removeListener (this);
    }

    static int cellWidth (const ParamSpec& p) { return p.vertical ? 34 : ! p.choices.isEmpty() ? 86 : p.toggle ? 62 : 64; }
    static int cellHeight (const ParamSpec& p) { return p.vertical ? 150 : 72; }

    void setDrawbarColour (juce::Colour c)
    {
        if (slider != nullptr)
        {
            slider->setColour (juce::Slider::trackColourId, c.withAlpha (0.85f));
            slider->setColour (juce::Slider::thumbColourId, c);
        }
    }

    void resized() override
    {
        auto area = getLocalBounds();
        const auto label = area.removeFromBottom (16);
        juce::ignoreUnused (label);
        if (slider != nullptr)
            slider->setBounds (param.vertical ? area.reduced (0, 4) : area.withSizeKeepingCentre (52, 52));
        if (box != nullptr)
            box->setBounds (area.withSizeKeepingCentre (getWidth() - 8, 26));
        if (button != nullptr)
            button->setBounds (area.withSizeKeepingCentre (48, 28));
    }

    void paint (juce::Graphics& g) override
    {
        const auto active = isMouseOverOrDragging (true) || (slider != nullptr && slider->isMouseButtonDown());
        const auto showValue = active && slider != nullptr;
        const auto value = slider != nullptr ? (float) slider->getValue() : 0.0f;
        g.setFont (uiFont (11.5f, showValue));
        g.setColour (showValue ? juce::Colour (0xffffffff) : juce::Colour (InstrumentLook::dimText));
        g.drawFittedText (showValue ? formatValue (param, value) : juce::String (param.name),
                          getLocalBounds().removeFromBottom (16), juce::Justification::centred, 1, 0.8f);
    }

    const ParamSpec param;

private:
    void sliderValueChanged (juce::Slider*) override { repaint(); }
    void mouseEnter (const juce::MouseEvent&) override { repaint(); }
    void mouseExit (const juce::MouseEvent&) override { repaint(); }
    void mouseUp (const juce::MouseEvent&) override { repaint(); }

    std::unique_ptr<juce::Slider> slider;
    std::unique_ptr<juce::ComboBox> box;
    std::unique_ptr<juce::ToggleButton> button;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> sliderAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> boxAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> buttonAttachment;
};

//==============================================================================
// The shape of an attack/decay/sustain/release envelope.
class InstrumentEditor::EnvelopeView final : public juce::Component
{
public:
    EnvelopeView (InstrumentProcessor& processor, const juce::StringArray& ids, juce::Colour c, bool isFilter)
        : accent (c), filter (isFilter)
    {
        for (int i = 0; i < ids.size() && i < 4; ++i)
            values[i] = processor.getState().getRawParameterValue (ids[i]);
    }

    bool update()
    {
        std::array<float, 4> now {};
        for (int i = 0; i < 4; ++i)
            now[(size_t) i] = values[i] != nullptr ? values[i]->load() : 0.0f;
        if (now == shown)
            return false;
        shown = now;
        repaint();
        return true;
    }

    void paint (juce::Graphics& g) override
    {
        const auto area = getLocalBounds().toFloat();
        g.setColour (juce::Colour (0xff111319));
        g.fillRoundedRectangle (area, 5.0f);

        const auto inner = area.reduced (6.0f, 6.0f);
        if (filter)
        {
            paintFilter (g, inner);
            return;
        }
        const auto stretch = [] (float seconds) { return std::sqrt (juce::jmax (0.0f, seconds)) + 0.02f; };
        const auto a = stretch (shown[0]), d = stretch (shown[1]), r = stretch (shown[3]);
        const auto hold = (a + d + r) * 0.35f;
        const auto total = a + d + hold + r;
        const auto x = [&] (float t) { return inner.getX() + inner.getWidth() * t / total; };
        const auto y = [&] (float level) { return inner.getBottom() - inner.getHeight() * level; };
        const auto sustain = juce::jlimit (0.0f, 1.0f, shown[2]);

        juce::Path shape;
        shape.startNewSubPath (x (0.0f), y (0.0f));
        shape.lineTo (x (a), y (1.0f));
        shape.quadraticTo (x (a + d * 0.25f), y (sustain), x (a + d), y (sustain));
        shape.lineTo (x (a + d + hold), y (sustain));
        shape.quadraticTo (x (a + d + hold + r * 0.2f), y (0.0f), x (total), y (0.0f));

        auto fill = shape;
        fill.lineTo (x (0.0f), y (0.0f));
        fill.closeSubPath();
        g.setGradientFill (juce::ColourGradient (accent.withAlpha (0.35f), 0.0f, inner.getY(),
                                                 accent.withAlpha (0.02f), 0.0f, inner.getBottom(), false));
        g.fillPath (fill);
        g.setColour (accent);
        g.strokePath (shape, juce::PathStrokeType (1.8f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

private:
    // The filter's response from 20 Hz to 20 kHz (the analog prototype of Nova's filter).
    void paintFilter (juce::Graphics& g, juce::Rectangle<float> inner)
    {
        const auto type = juce::roundToInt (shown[0]);
        const auto cutoff = juce::jmax (10.0f, shown[1]);
        const auto k = 2.0f - 1.96f * juce::jlimit (0.0f, 1.0f, shown[2]);
        const auto k1 = 2.0f - 1.96f * juce::jlimit (0.0f, 1.0f, shown[2] * 0.4f);
        const auto response = [&] (float hz)
        {
            using C = std::complex<float>;
            const C s (0.0f, hz / cutoff);
            const auto pole = [&] (float kk) { return s * s + kk * s + 1.0f; };
            C h;
            switch (type)
            {
                case 0:  h = 1.0f / (pole (k1) * pole (k)); break;
                case 1:  h = 1.0f / pole (k); break;
                case 2:  h = 1.5f * k * s / pole (k); break;
                default: h = s * s / pole (k); break;
            }
            return juce::Decibels::gainToDecibels (std::abs (h), -60.0f);
        };

        juce::Path curve;
        constexpr int points = 120;
        for (int i = 0; i <= points; ++i)
        {
            const auto hz = 20.0f * std::pow (1000.0f, (float) i / points);
            const auto db = juce::jlimit (-36.0f, 24.0f, response (hz));
            const auto px = inner.getX() + inner.getWidth() * (float) i / points;
            const auto py = inner.getY() + inner.getHeight() * (24.0f - db) / 60.0f;
            if (i == 0) curve.startNewSubPath (px, py); else curve.lineTo (px, py);
        }
        auto fill = curve;
        fill.lineTo (inner.getRight(), inner.getBottom());
        fill.lineTo (inner.getX(), inner.getBottom());
        fill.closeSubPath();
        g.setGradientFill (juce::ColourGradient (accent.withAlpha (0.35f), 0.0f, inner.getY(),
                                                 accent.withAlpha (0.02f), 0.0f, inner.getBottom(), false));
        g.fillPath (fill);
        g.setColour (accent);
        g.strokePath (curve, juce::PathStrokeType (1.8f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    std::atomic<float>* values[4] {};
    std::array<float, 4> shown { -1.0f, -1.0f, -1.0f, -1.0f };
    juce::Colour accent;
    bool filter = false;
};

//==============================================================================
// The output waveform, steadied on a rising zero crossing.
class InstrumentEditor::Scope final : public juce::Component
{
public:
    Scope (InstrumentProcessor& p, juce::Colour c) : processor (p), accent (c) {}

    void refresh()
    {
        processor.readScope (samples.data(), (int) samples.size());
        auto peak = 0.0f;
        for (auto s : samples)
            peak = juce::jmax (peak, std::abs (s));
        if (peak > 1.0e-4f || wasSounding)
            repaint();
        wasSounding = peak > 1.0e-4f;
    }

    void paint (juce::Graphics& g) override
    {
        const auto area = getLocalBounds().toFloat();
        g.setColour (juce::Colour (0xff0e1015));
        g.fillRoundedRectangle (area, 6.0f);
        g.setColour (juce::Colour (InstrumentLook::panelEdge));
        g.drawRoundedRectangle (area.reduced (0.5f), 6.0f, 1.0f);
        g.setColour (juce::Colour (InstrumentLook::track).withAlpha (0.6f));
        g.drawHorizontalLine (getHeight() / 2, 6.0f, area.getRight() - 6.0f);

        constexpr int shown = 700;
        const auto available = (int) samples.size() - shown;
        auto start = available;
        for (int i = available - 1; i > 1; --i)
            if (samples[(size_t) i - 1] <= 0.0f && samples[(size_t) i] > 0.0f)
            {
                start = i;
                break;
            }

        auto peak = 0.05f;
        for (int i = 0; i < shown; ++i)
            peak = juce::jmax (peak, std::abs (samples[(size_t) (start + i)]));
        const auto scale = 0.9f / juce::jmin (1.0f, peak * 1.1f) * 0.5f * area.getHeight();

        juce::Path wave;
        for (int i = 0; i < shown; ++i)
        {
            const auto px = 6.0f + (area.getWidth() - 12.0f) * (float) i / (float) (shown - 1);
            const auto py = area.getCentreY() - juce::jlimit (-1.0f, 1.0f, samples[(size_t) (start + i)] * scale / (0.5f * area.getHeight())) * 0.5f * area.getHeight() * 0.85f;
            if (i == 0) wave.startNewSubPath (px, py); else wave.lineTo (px, py);
        }
        g.setColour (accent.withAlpha (0.25f));
        g.strokePath (wave, juce::PathStrokeType (4.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setColour (accent);
        g.strokePath (wave, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

private:
    InstrumentProcessor& processor;
    juce::Colour accent;
    std::array<float, 2048> samples {};
    bool wasSounding = true;
};

//==============================================================================
InstrumentEditor::InstrumentEditor (InstrumentProcessor& p)
    : AudioProcessorEditor (p),
      processor (p),
      spec (p.getSpec()),
      look (p.getSpec().accent),
      keyboard (p.getKeyboardState(), juce::MidiKeyboardComponent::horizontalKeyboard)
{
    setLookAndFeel (&look);

    // The sections, row by row, and how big that makes the window.
    int width = 820;
    for (const auto& row : spec.rows)
    {
        int rowWidth = 0, controlHeight = 0;
        bool hasEnvelope = false;
        for (const auto& section : row)
        {
            SectionView view;
            view.section = &section;
            int sectionWidth = 2 * sectionPadding;
            for (const auto& id : section.params)
                if (const auto* param = findParam (spec, id))
                {
                    controls.push_back (std::make_unique<Control> (processor, *param));
                    view.controls.push_back (controls.back().get());
                    addAndMakeVisible (*controls.back());
                    sectionWidth += Control::cellWidth (*param);
                    controlHeight = juce::jmax (controlHeight, Control::cellHeight (*param));
                }
            if (section.envelope.size() == 4 || section.filter.size() == 3)
            {
                const auto isFilter = section.envelope.size() != 4;
                view.envelope = std::make_unique<EnvelopeView> (processor, isFilter ? section.filter : section.envelope, spec.accent, isFilter);
                addAndMakeVisible (*view.envelope);
                hasEnvelope = true;
            }
            view.bounds.setWidth (sectionWidth);
            rowWidth += sectionWidth;
            sections.push_back (std::move (view));
        }
        rowWidth += gap * juce::jmax (0, (int) row.size() - 1);
        width = juce::jmax (width, rowWidth + 2 * margin);
        rowHeights.push_back (titleHeight + (hasEnvelope ? envelopeHeight + 6 : 0) + controlHeight + sectionPadding);
    }

    // Drawbars wear the colours of an organ's: brown, white and black.
    for (auto& control : controls)
        if (control->param.vertical)
        {
            const juce::String id (control->param.id);
            control->setDrawbarColour (id.endsWith ("16") || id.endsWith ("513") ? juce::Colour (0xff9a5a36)
                                       : id.endsWith ("135") || id.endsWith ("113") || id.endsWith ("171") ? juce::Colour (0xff2b2b30).brighter (0.6f)
                                       : juce::Colour (0xffeae6dc));
        }

    volumeKnob = std::make_unique<Control> (processor, knob ("volume", "Volume", -36.0f, 6.0f, 0.0f, Unit::decibels, -6.0f));
    addAndMakeVisible (*volumeKnob);

    scope = std::make_unique<Scope> (processor, spec.accent);
    addAndMakeVisible (*scope);

    for (size_t i = 0; i < spec.presets.size(); ++i)
        presetBox.addItem (spec.presets[i].name, (int) i + 1);
    presetBox.setTextWhenNothingSelected ("Presets");
    presetBox.onChange = [this]
    {
        const auto index = presetBox.getSelectedId() - 1;
        if (index >= 0 && index != processor.getCurrentProgram())
            processor.setCurrentProgram (index);
        shownProgram = processor.getCurrentProgram();
    };
    addAndMakeVisible (presetBox);
    previousPreset.onClick = [this] { stepPreset (-1); };
    nextPreset.onClick = [this] { stepPreset (1); };
    addAndMakeVisible (previousPreset);
    addAndMakeVisible (nextPreset);
    updatePresetBox();

    keyboard.setAvailableRange (spec.lowestKey, spec.highestKey);
    keyboard.setScrollButtonsVisible (false);
    keyboard.setOctaveForMiddleC (4);
    keyboard.setMidiChannel (1);
    keyboard.setVelocity (0.8f, true);
    keyboard.setColour (juce::MidiKeyboardComponent::whiteNoteColourId, juce::Colour (0xffe8eaf0));
    keyboard.setColour (juce::MidiKeyboardComponent::blackNoteColourId, juce::Colour (0xff0d0f14));
    keyboard.setColour (juce::MidiKeyboardComponent::keySeparatorLineColourId, juce::Colour (0xff9da2b0));
    keyboard.setColour (juce::MidiKeyboardComponent::mouseOverKeyOverlayColourId, spec.accent.withAlpha (0.25f));
    keyboard.setColour (juce::MidiKeyboardComponent::keyDownOverlayColourId, spec.accent.withAlpha (0.85f));
    keyboard.setColour (juce::MidiKeyboardComponent::shadowColourId, juce::Colours::black.withAlpha (0.3f));
    keyboard.setColour (juce::MidiKeyboardComponent::textLabelColourId, juce::Colour (0xff6b7080));
    keyboard.setWantsKeyboardFocus (false);
    addAndMakeVisible (keyboard);

    auto height = margin + headerHeight + gap;
    for (auto h : rowHeights)
        height += h + gap;
    height += keyboardHeight + margin;
    setSize (width, height);

    startTimerHz (30);
}

InstrumentEditor::~InstrumentEditor()
{
    stopTimer();
    setLookAndFeel (nullptr);
}

void InstrumentEditor::resized()
{
    auto area = getLocalBounds().reduced (margin);

    // Header: name on the left; presets in the middle; scope and volume on the right.
    auto header = area.removeFromTop (headerHeight);
    volumeKnob->setBounds (header.removeFromRight (64).withSizeKeepingCentre (64, 72).withY (header.getY() - 6));
    header.removeFromRight (8);
    scope->setBounds (header.removeFromRight (200).withSizeKeepingCentre (200, 50));
    const auto presetWidth = juce::jlimit (240, 340, header.getWidth() - 300);
    auto presets = header.withSizeKeepingCentre (presetWidth, 32)
                         .withX (header.getX() + juce::jmax (270, (header.getWidth() - presetWidth) / 2));
    previousPreset.setBounds (presets.removeFromLeft (30));
    nextPreset.setBounds (presets.removeFromRight (30));
    presetBox.setBounds (presets.reduced (4, 0));
    area.removeFromTop (gap);

    keyboard.setBounds (area.removeFromBottom (keyboardHeight));
    int whiteKeys = 0;
    for (int note = spec.lowestKey; note <= spec.highestKey; ++note)
        whiteKeys += juce::MidiMessage::isMidiNoteBlack (note) ? 0 : 1;
    keyboard.setKeyWidth ((float) keyboard.getWidth() / (float) juce::jmax (1, whiteKeys));
    keyboard.setBlackNoteLengthProportion (0.62f);

    // Rows of sections; each row's sections share out its spare width.
    size_t index = 0;
    for (size_t row = 0; row < spec.rows.size(); ++row)
    {
        auto rowArea = area.removeFromTop (rowHeights[row]);
        area.removeFromTop (gap);
        const auto count = spec.rows[row].size();
        int natural = gap * (int) juce::jmax ((size_t) 0, count - 1);
        for (size_t s = 0; s < count; ++s)
            natural += sections[index + s].bounds.getWidth();
        const auto spare = juce::jmax (0, rowArea.getWidth() - natural);

        for (size_t s = 0; s < count; ++s)
        {
            auto& view = sections[index + s];
            const auto extra = spare * view.bounds.getWidth() / juce::jmax (1, natural - gap * (int) (count - 1));
            const auto sectionWidth = s + 1 == count ? rowArea.getWidth() : view.bounds.getWidth() + extra;
            view.bounds = rowArea.removeFromLeft (sectionWidth);
            rowArea.removeFromLeft (gap);

            auto inside = view.bounds.reduced (sectionPadding, 0);
            inside.removeFromTop (titleHeight);
            inside.removeFromBottom (sectionPadding);
            if (view.envelope != nullptr)
            {
                view.envelope->setBounds (inside.removeFromTop (envelopeHeight));
                inside.removeFromTop (6);
            }

            int controlsWidth = 0;
            for (auto* c : view.controls)
                controlsWidth += Control::cellWidth (c->param);
            const auto spread = view.controls.empty() ? 0 : (inside.getWidth() - controlsWidth) / (int) view.controls.size();
            auto x = inside.getX() + spread / 2;
            for (auto* c : view.controls)
            {
                const auto w = Control::cellWidth (c->param);
                const auto h = Control::cellHeight (c->param);
                c->setBounds (x, inside.getBottom() - h, w, h);
                x += w + spread;
            }
        }
        index += count;
    }
}

void InstrumentEditor::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xff1b1e28), 0.0f, 0.0f, juce::Colour (0xff111319), 0.0f, bounds.getBottom(), false));
    g.fillAll();

    // A soft glow of the instrument's colour behind its name.
    g.setGradientFill (juce::ColourGradient (spec.accent.withAlpha (0.18f), 40.0f, 10.0f,
                                             spec.accent.withAlpha (0.0f), 360.0f, 120.0f, true));
    g.fillRect (bounds.withHeight (160.0f));

    // Name and tagline.
    const auto header = getLocalBounds().reduced (margin).removeFromTop (headerHeight);
    auto mark = juce::Rectangle<float> ((float) header.getX(), (float) header.getCentreY() - 17.0f, 34.0f, 34.0f);
    g.setGradientFill (juce::ColourGradient (spec.accent.brighter (0.3f), mark.getX(), mark.getY(),
                                             spec.accent.darker (0.5f), mark.getRight(), mark.getBottom(), false));
    g.fillRoundedRectangle (mark, 9.0f);
    g.setColour (juce::Colour (0xff111318));
    g.setFont (uiFont (20.0f, true));
    g.drawText (spec.name.substring (0, 1), mark, juce::Justification::centred);

    const auto textX = (int) mark.getRight() + 12;
    g.setColour (juce::Colour (InstrumentLook::text));
    g.setFont (uiFont (25.0f, true));
    g.drawText (spec.name, textX, header.getCentreY() - 21, 220, 28, juce::Justification::centredLeft);
    g.setColour (juce::Colour (InstrumentLook::dimText));
    g.setFont (uiFont (12.5f));
    g.drawText (spec.tagline + "  \xc2\xb7  MicFX", textX, header.getCentreY() + 5, 240, 18, juce::Justification::centredLeft);

    // Section cards and their titles.
    for (const auto& view : sections)
    {
        const auto card = view.bounds.toFloat();
        g.setColour (juce::Colour (InstrumentLook::panel));
        g.fillRoundedRectangle (card, 9.0f);
        g.setColour (juce::Colour (InstrumentLook::panelEdge));
        g.drawRoundedRectangle (card.reduced (0.5f), 9.0f, 1.0f);

        g.setColour (spec.accent.withAlpha (0.9f));
        g.fillRoundedRectangle (card.getX() + sectionPadding, card.getY() + 10.0f, 3.0f, 10.0f, 1.5f);
        g.setFont (uiFont (11.0f, true).withExtraKerningFactor (0.12f));
        g.setColour (juce::Colour (InstrumentLook::text).withAlpha (0.85f));
        g.drawText (view.section->title.toUpperCase(), view.bounds.getX() + sectionPadding + 9, view.bounds.getY() + 6,
                    view.bounds.getWidth() - 2 * sectionPadding, 18, juce::Justification::centredLeft);
    }

    // A strip above the keyboard.
    const auto keys = keyboard.getBounds().toFloat().expanded (0.0f, 3.0f).withHeight (3.0f);
    g.setColour (spec.accent.withAlpha (0.8f));
    g.fillRoundedRectangle (keys, 1.5f);
}

void InstrumentEditor::timerCallback()
{
    scope->refresh();
    for (auto& view : sections)
        if (view.envelope != nullptr)
            view.envelope->update();
    if (processor.getCurrentProgram() != shownProgram)
        updatePresetBox();
}

void InstrumentEditor::updatePresetBox()
{
    shownProgram = processor.getCurrentProgram();
    presetBox.setSelectedId (shownProgram + 1, juce::dontSendNotification);
}

void InstrumentEditor::stepPreset (int delta)
{
    const auto count = (int) spec.presets.size();
    if (count == 0)
        return;
    processor.setCurrentProgram ((processor.getCurrentProgram() + delta + count) % count);
    updatePresetBox();
}
}
