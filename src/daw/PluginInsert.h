#pragma once

#include "Instruments.h"
#include "dsp/FxModule.h"

#include <limits>

// A third-party effect plugin (VST3 everywhere, AU on macOS) as a DAW insert.
//
// In the project it's an INSERT of type "plugin" that carries the plugin like an
// INSTRUMENT does: `plugin` (its id), `name`, `format`, `vendor`, `description` (its
// PluginDescription as XML, to find it again on another computer) and `state` (its own
// settings, base64). Like every insert it also has an on switch (`pluginOn`) and its
// in and out gains (`pluginInGain`, `pluginOutGain`).
//
// In the engine it's an fx::Module, so it sits in a track's chain with the built-in
// effects: stereo in and out (a mono plugin gets the two sides summed and its output on
// both), its delay reported for latency compensation. Switched off, it isn't run and the
// signal goes round it, delayed by the plugin's latency so the timing doesn't jump.
//
// Its panel: plug-in parameters picked to show as knobs on its card (`panel`, their indices
// "3,17,42"). Each is a value of the insert, `p<index>` (the parameter's own 0..1), so it
// goes through the same paths as any insert value: the card, undo, macros, automation. The
// audio thread hands a changed value to the plug-in; a parameter moved in the plug-in's own
// window comes back into the insert (DawBridge watches them).
//
// The plugin is made and deleted on the message thread (instruments::create does both,
// waiting there when an offline render builds its chain on another thread).
namespace daw
{
class PluginInsert final : public fx::Module
{
public:
    static constexpr const char* onId = "pluginOn";

    // Loads the plugin an INSERT describes, with its saved state, prepared for the rate and
    // block size. Null (and `error` says why) when it can't, e.g. it isn't installed.
    static std::unique_ptr<PluginInsert> create (const juce::ValueTree& insert, double sampleRate, int blockSize,
                                                 bool realtime, juce::String& error);

    // Another insert module on the same running plugin (a track's chain is rebuilt when its
    // inserts change; the plugin keeps going, with everything it has in memory).
    static std::unique_ptr<PluginInsert> share (const PluginInsert& running, const juce::Array<int>& panel);

    // The panel an INSERT asks for (`panel`: parameter indices), and the value ID of one.
    static juce::Array<int> getPanel (const juce::ValueTree& insert);
    static juce::String panelValueId (int parameterIndex) { return "p" + juce::String (parameterIndex); }
    const juce::Array<int>& getPanel() const noexcept { return panelIndices; }
    // The value last handed to the plug-in for a panel parameter (at first: where it was).
    float getPushed (int parameterIndex) const noexcept;
    // A panel value was moved in the plug-in's window: taken as handed over, so it isn't
    // sent back (message thread, with the insert value set to it).
    void setPushed (int parameterIndex, float value) noexcept;

    InstrumentPtr getPlugin() const noexcept { return plugin; }

    int getLatencySamples() const noexcept override { return plugin->getLatencySamples(); }

protected:
    void prepareModule (const juce::dsp::ProcessSpec&) override;
    void resetModule() override;
    void update() noexcept override {}
    void render (const Context&) noexcept override;
    bool alwaysProcesses() const noexcept override { return true; } // its own bypass, see render()

private:
    PluginInsert (InstrumentPtr, double preparedRate, int preparedBlock, const juce::Array<int>& panel);

    InstrumentPtr plugin;
    fx::Module::Value& on;
    double preparedRate = 0.0;
    int preparedBlock = 0;

    juce::AudioBuffer<float> buffer; // the plugin's channels (audio thread)
    juce::MidiBuffer midi;
    juce::AudioBuffer<float> delay;  // the signal, for going round the plugin while it's off
    int delayWrite = 0;

    struct PanelValue
    {
        int index = 0;
        Value* value = nullptr;
        juce::AudioProcessorParameter* parameter = nullptr;
        std::atomic<float> pushed { std::numeric_limits<float>::quiet_NaN() };
    };
    juce::Array<int> panelIndices;
    std::vector<std::unique_ptr<PanelValue>> panel;
};
}
