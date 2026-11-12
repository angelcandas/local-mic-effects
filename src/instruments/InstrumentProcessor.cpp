#include "InstrumentProcessor.h"
#include "InstrumentEditor.h"

namespace micfx::inst
{
juce::String formatValue (const ParamSpec& p, float v)
{
    if (! p.choices.isEmpty())
        return p.choices[juce::jlimit (0, p.choices.size() - 1, (int) std::round (v))];
    if (p.toggle)
        return v >= 0.5f ? "On" : "Off";

    switch (p.unit)
    {
        case Unit::percent:   return juce::String (juce::roundToInt (v * 100.0f)) + "%";
        case Unit::bipolar:   return (v > 0.005f ? "+" : "") + juce::String (juce::roundToInt (v * 100.0f)) + "%";
        case Unit::seconds:   return v < 1.0f ? juce::String (juce::roundToInt (v * 1000.0f)) + " ms" : juce::String (v, 2) + " s";
        case Unit::hertz:     return v < 1000.0f ? juce::String (v, v < 10.0f ? 2 : 0) + " Hz" : juce::String (v / 1000.0f, 1) + " kHz";
        case Unit::decibels:  return juce::String (v, 1) + " dB";
        case Unit::semitones: return (v > 0.0f ? "+" : "") + juce::String (juce::roundToInt (v)) + " st";
        case Unit::cents:     return (v > 0.0f ? "+" : "") + juce::String (juce::roundToInt (v)) + " ct";
        case Unit::octaves:   return (v > 0.0f ? "+" : "") + juce::String (v, 1) + " oct";
        case Unit::multiplier:return juce::String (v, 2) + "x";
        case Unit::none:      break;
    }
    return p.step >= 1.0f ? juce::String (juce::roundToInt (v)) : juce::String (v, 2);
}

namespace
{
    // The level knob every instrument has, in the window's header.
    ParamSpec volumeSpec() { return knob ("volume", "Volume", -36.0f, 6.0f, 0.0f, Unit::decibels, -6.0f); }

    juce::NormalisableRange<float> makeRange (const ParamSpec& p)
    {
        juce::NormalisableRange<float> range (p.min, p.max, p.step);
        if (p.centre > p.min && p.centre < p.max)
            range.setSkewForCentre (p.centre);
        return range;
    }
}

juce::AudioProcessorValueTreeState::ParameterLayout InstrumentProcessor::makeLayout (const Spec& spec)
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    auto all = spec.params;
    all.push_back (volumeSpec());

    for (const auto& p : all)
    {
        const juce::ParameterID id { p.id, 1 };
        if (! p.choices.isEmpty())
            layout.add (std::make_unique<juce::AudioParameterChoice> (id, p.name, p.choices, (int) p.def));
        else if (p.toggle)
            layout.add (std::make_unique<juce::AudioParameterBool> (id, p.name, p.def >= 0.5f));
        else
            layout.add (std::make_unique<juce::AudioParameterFloat> (
                id, p.name, makeRange (p), p.def,
                juce::AudioParameterFloatAttributes().withStringFromValueFunction ([p] (float v, int) { return formatValue (p, v); })));
    }
    return layout;
}

InstrumentProcessor::InstrumentProcessor (const Spec& s)
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      spec (s),
      state (*this, nullptr, juce::Identifier ("Parameters"), makeLayout (s))
{
    engine = spec.makeEngine (Params (state));
    volume = state.getRawParameterValue ("volume");
}

InstrumentProcessor::~InstrumentProcessor() = default;

bool InstrumentProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    return out == juce::AudioChannelSet::stereo() || out == juce::AudioChannelSet::mono();
}

void InstrumentProcessor::prepareToPlay (double sampleRate, int maximumBlockSize)
{
    work.setSize (2, juce::jmax (1, maximumBlockSize));
    engine->prepare (sampleRate, juce::jmax (1, maximumBlockSize));
    gain.reset (sampleRate, 0.05);
    gain.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (volume->load()));
    keyboard.reset();
}

void InstrumentProcessor::reset()
{
    engine->reset();
}

void InstrumentProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    const auto numSamples = buffer.getNumSamples();

    // The window's keyboard plays too.
    keyboard.processNextMidiBuffer (midi, 0, numSamples, true);

    if (work.getNumSamples() < numSamples)
        work.setSize (2, numSamples, false, false, true); // a host that sends more than it said

    work.clear (0, numSamples);
    juce::AudioBuffer<float> stereo (work.getArrayOfWritePointers(), 2, numSamples);
    engine->process (stereo, midi);

    gain.setTargetValue (volume->load() <= -35.9f ? 0.0f : juce::Decibels::decibelsToGain (volume->load()));
    gain.applyGain (stereo, numSamples);

    // Keep a mono copy for the scope.
    auto w = scopeWrite.load (std::memory_order_relaxed);
    const auto* l = stereo.getReadPointer (0);
    const auto* r = stereo.getReadPointer (1);
    for (int i = 0; i < numSamples; ++i)
    {
        scope[(size_t) w] = 0.5f * (l[i] + r[i]);
        w = (w + 1) & (scopeSize - 1);
    }
    scopeWrite.store (w, std::memory_order_release);

    if (buffer.getNumChannels() >= 2)
    {
        buffer.copyFrom (0, 0, stereo, 0, 0, numSamples);
        buffer.copyFrom (1, 0, stereo, 1, 0, numSamples);
        for (int ch = 2; ch < buffer.getNumChannels(); ++ch)
            buffer.clear (ch, 0, numSamples);
    }
    else if (buffer.getNumChannels() == 1)
    {
        buffer.copyFromWithRamp (0, 0, stereo.getReadPointer (0), numSamples, 0.5f, 0.5f);
        buffer.addFrom (0, 0, stereo, 1, 0, numSamples, 0.5f);
    }

    midi.clear();
}

void InstrumentProcessor::readScope (float* destination, int count) const
{
    count = juce::jmin (count, scopeSize);
    auto r = (scopeWrite.load (std::memory_order_acquire) - count) & (scopeSize - 1);
    for (int i = 0; i < count; ++i)
    {
        destination[i] = scope[(size_t) r];
        r = (r + 1) & (scopeSize - 1);
    }
}

double InstrumentProcessor::getTailLengthSeconds() const { return engine->getTailSeconds(); }

juce::AudioProcessorEditor* InstrumentProcessor::createEditor() { return new InstrumentEditor (*this); }

//==============================================================================
int InstrumentProcessor::getNumPrograms() { return juce::jmax (1, (int) spec.presets.size()); }
int InstrumentProcessor::getCurrentProgram() { return program.load(); }

const juce::String InstrumentProcessor::getProgramName (int index)
{
    return juce::isPositiveAndBelow (index, (int) spec.presets.size()) ? spec.presets[(size_t) index].name : juce::String ("Default");
}

void InstrumentProcessor::setCurrentProgram (int index)
{
    if (! juce::isPositiveAndBelow (index, (int) spec.presets.size()))
        return;

    program = index;
    const auto& preset = spec.presets[(size_t) index];

    for (const auto& p : spec.params)
    {
        auto value = p.def;
        for (const auto& [id, v] : preset.values)
            if (juce::String (id) == p.id)
                value = v;

        if (auto* parameter = state.getParameter (p.id))
            parameter->setValueNotifyingHost (parameter->convertTo0to1 (value));
    }
    updateHostDisplay (ChangeDetails().withProgramChanged (true));
}

bool InstrumentProcessor::loadPreset (const juce::String& name)
{
    for (size_t i = 0; i < spec.presets.size(); ++i)
        if (spec.presets[i].name.equalsIgnoreCase (name))
        {
            setCurrentProgram ((int) i);
            return true;
        }
    return false;
}

juce::String InstrumentProcessor::getPresetName() const
{
    const auto index = program.load();
    return juce::isPositiveAndBelow (index, (int) spec.presets.size()) ? spec.presets[(size_t) index].name : juce::String();
}

void InstrumentProcessor::getStateInformation (juce::MemoryBlock& block)
{
    auto copy = state.copyState();
    juce::XmlElement xml ("MicFXInstrument");
    xml.setAttribute ("instrument", spec.key);
    xml.setAttribute ("preset", getPresetName());
    xml.setAttribute ("version", 1);
    if (auto params = copy.createXml())
        xml.addChildElement (params.release());
    copyXmlToBinary (xml, block);
}

void InstrumentProcessor::setStateInformation (const void* data, int size)
{
    const auto xml = getXmlFromBinary (data, size);
    if (xml == nullptr || ! xml->hasTagName ("MicFXInstrument") || xml->getStringAttribute ("instrument") != spec.key)
        return;

    for (size_t i = 0; i < spec.presets.size(); ++i)
        if (spec.presets[i].name == xml->getStringAttribute ("preset"))
            program = (int) i;

    if (auto* params = xml->getChildByName (state.state.getType()))
        state.replaceState (juce::ValueTree::fromXml (*params));
}
}
