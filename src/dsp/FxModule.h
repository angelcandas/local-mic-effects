#pragma once

#include <juce_dsp/juce_dsp.h>

#include <atomic>
#include <deque>

namespace fx
{
// The effects a chain can hold, in the order Live mode runs them.
enum class ModuleType
{
    highPass, gate, pitch, eq, compressor, deEsser, exciter, saturation, radio, bitcrusher,
    robot, filter, autoWah, tremolo, vibrato, chorus, flanger, phaser, doubler, echo, reverb, width
};

inline constexpr int numModuleTypes = (int) ModuleType::width + 1;

//==============================================================================
// One effect in an FxChain: a DSP processor plus its own parameter values.
//
// Values are in real units, keyed by the same IDs as the plugin parameters (see
// Parameters.cpp), so presets and the UI work with any chain. They are atomics: any
// thread may set them, and the audio thread picks them up on the next block.
//
// A module switches itself on and off with its switch values (most have one, the pitch
// module has three). While off it doesn't process, and it starts from a clean state
// when it comes back on, so stale samples from before the bypass don't click.
class Module
{
public:
    using Context = juce::dsp::ProcessContextReplacing<float>;
    using Value = std::atomic<float>;

    virtual ~Module() = default;

    ModuleType getType() const noexcept { return type; }

    int getNumValues() const noexcept                     { return (int) entries.size(); }
    const juce::String& getValueId (int index) const      { return entries[(size_t) index].id; }
    bool isSwitch (int index) const                       { return entries[(size_t) index].isSwitch; }
    Value& getValue (int index) noexcept                  { return entries[(size_t) index].value; }

    // Null when this module has no value with that ID.
    Value* findValue (const juce::String& id) noexcept
    {
        for (auto& entry : entries)
            if (entry.id == id)
                return &entry.value;

        return nullptr;
    }

    bool isOn() const noexcept
    {
        for (const auto& entry : entries)
            if (entry.isSwitch && entry.value.load (std::memory_order_relaxed) >= 0.5f)
                return true;

        return false;
    }

    // Delay this module adds to the signal, in samples.
    virtual int getLatencySamples() const noexcept { return 0; }

    //==============================================================================
    void prepare (const juce::dsp::ProcessSpec& spec)
    {
        prepareModule (spec);
        update();
        resetModule();
        wasOn = false;
    }

    void reset()
    {
        resetModule();
        wasOn = false;
    }

    // Audio thread.
    void process (const Context& context) noexcept
    {
        update();

        if (alwaysProcesses())
        {
            render (context);
            return;
        }

        const auto on = isOn();

        if (on && ! wasOn)
            resetModule();

        wasOn = on;

        if (on)
            render (context);
    }

protected:
    explicit Module (ModuleType t) : type (t) {}

    // Declare values in the order the UI should list them. Call from member
    // initialisers only (before the module is shared with the audio thread).
    Value& addSwitch (const char* id, bool defaultOn = false) { return add (id, true, defaultOn ? 1.0f : 0.0f); }
    Value& addValue (const char* id, float defaultValue)      { return add (id, false, defaultValue); }

    static float get (const Value& value) noexcept      { return value.load (std::memory_order_relaxed); }
    static float fraction (const Value& value) noexcept { return get (value) / 100.0f; }
    static bool  flag (const Value& value) noexcept     { return get (value) >= 0.5f; }
    static int   index (const Value& value) noexcept    { return juce::roundToInt (get (value)); }

    virtual void prepareModule (const juce::dsp::ProcessSpec&) = 0;
    virtual void resetModule() = 0;
    virtual void update() noexcept = 0;               // pushes the values into the DSP
    virtual void render (const Context&) noexcept = 0;

    // A module that handles its own switch (e.g. to let a tail fade out) runs every block.
    virtual bool alwaysProcesses() const noexcept { return false; }

private:
    struct Entry
    {
        Entry (const char* i, bool s, float v) : id (i), isSwitch (s), value (v) {}

        juce::String id;
        bool isSwitch;
        Value value;
    };

    Value& add (const char* id, bool isSwitchValue, float defaultValue)
    {
        return entries.emplace_back (id, isSwitchValue, defaultValue).value;
    }

    const ModuleType type;
    std::deque<Entry> entries; // a deque keeps references stable as values are added
    bool wasOn = false;

    JUCE_DECLARE_NON_COPYABLE (Module)
};
}
