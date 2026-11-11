#pragma once

#include <juce_dsp/juce_dsp.h>

#include "FxMeter.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <deque>

namespace fx
{
// The effects a chain can hold, in the order Live mode runs them.
enum class ModuleType
{
    highPass, gate, pitch, eq, paraEq, compressor, deEsser, exciter, saturation, radio, bitcrusher,
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

    // A parameter value. store() and load() are the target, set from any thread. Knob
    // values glide towards a new target over ~15 ms on the audio thread (see process()),
    // so turning a knob or switching preset doesn't click or crackle; switches and
    // choices (key, scale, filter type...) change at once.
    class Value
    {
    public:
        Value (float initial, bool isSmoothed) : target (initial), current (initial), smoothed (isSmoothed) {}

        void store (float v, std::memory_order order = std::memory_order_seq_cst) noexcept { target.store (v, order); }
        float load (std::memory_order order = std::memory_order_seq_cst) const noexcept    { return target.load (order); }

        bool isSmoothed() const noexcept { return smoothed; }

    private:
        friend class Module;

        // What the DSP uses: the target for switches and choices, the gliding value for knobs.
        float getForDsp() const noexcept
        {
            return smoothed ? current.load (std::memory_order_relaxed) : target.load (std::memory_order_relaxed);
        }

        std::atomic<float> target;
        std::atomic<float> current; // audio thread writes; atomic only so other threads may read it
        const bool smoothed;

        JUCE_DECLARE_NON_COPYABLE (Value)
    };

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
    // Metering, for the UI's live pictures. Peak levels (linear) going into and out of
    // the module since the last call; both stay at 0 while it's off. Any thread.
    float takeInputPeak() noexcept   { return inputPeak.exchange (0.0f, std::memory_order_relaxed); }
    float takeOutputPeak() noexcept  { return outputPeak.exchange (0.0f, std::memory_order_relaxed); }

    // The latest samples in and out, captured only while requested (see Scope).
    Scope& getScope() noexcept               { return scope; }
    double getSampleRate() const noexcept    { return sampleRate; }

    //==============================================================================
    void prepare (const juce::dsp::ProcessSpec& spec)
    {
        sampleRate = spec.sampleRate;
        snapToTargets();
        prepareModule (spec);
        update();
        resetModule();
        wasOn = false;
    }

    void reset()
    {
        snapToTargets();
        resetModule();
        wasOn = false;
    }

    // Audio thread.
    void process (const Context& context) noexcept
    {
        const auto on = alwaysProcesses() || isOn();

        if (! alwaysProcesses())
        {
            if (on && ! wasOn)
            {
                // Coming back on: start from the current settings rather than gliding from
                // wherever the values were when it was switched off.
                snapToTargets();
                resetModule();
            }

            wasOn = on;
        }

        if (! on)
        {
            snapToTargets();
            update();
            return;
        }

        // While a knob value is gliding, render in short slices and push the values into
        // the DSP before each one, so gains and filters move smoothly instead of in
        // block-sized steps. Settled, it's one update per block as before.
        auto& block = context.getOutputBlock();
        const auto numSamples = block.getNumSamples();

        raisePeak (inputPeak, blockPeak (block));

        // The spectrum wants the signal before and after; keep the input aside.
        const auto capture = scope.isWanted() && block.getNumChannels() > 0;
        const auto captureLength = juce::jmin (numSamples, scopeInput.size());
        if (capture)
            std::copy_n (block.getChannelPointer (0), captureLength, scopeInput.begin());

        for (size_t start = 0; start < numSamples;)
        {
            const auto length = advanceSmoothing() ? std::min (smoothingSliceSamples, numSamples - start) : numSamples - start;
            update();

            auto slice = block.getSubBlock (start, length);
            render (Context (slice));
            start += length;
        }

        raisePeak (outputPeak, blockPeak (block));

        if (capture)
            scope.push (scopeInput.data(), block.getChannelPointer (0), (int) captureLength);
    }

protected:
    explicit Module (ModuleType t) : type (t) {}

    // Declare values in the order the UI should list them. Call from member
    // initialisers only (before the module is shared with the audio thread).
    // A switch turns the module on; a choice is a value that steps (key, scale, filter
    // type, an on/off option that doesn't switch the module); every other value glides.
    Value& addSwitch (const char* id, bool defaultOn = false) { return add (id, true, defaultOn ? 1.0f : 0.0f, false); }
    Value& addChoice (const char* id, float defaultValue)     { return add (id, false, defaultValue, false); }
    Value& addValue (const char* id, float defaultValue)      { return add (id, false, defaultValue, true); }

    // The value for the DSP. In update() that's the gliding value; switches and choices
    // read their target, so any thread can check them (e.g. getLatencySamples()).
    static float get (const Value& value) noexcept      { return value.getForDsp(); }
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
        Entry (const char* i, bool s, float v, bool smoothed) : id (i), isSwitch (s), value (v, smoothed) {}

        juce::String id;
        bool isSwitch;
        Value value;
    };

    Value& add (const char* id, bool isSwitchValue, float defaultValue, bool smoothed)
    {
        return entries.emplace_back (id, isSwitchValue, defaultValue, smoothed).value;
    }

    void snapToTargets() noexcept
    {
        for (auto& entry : entries)
            entry.value.current.store (entry.value.target.load (std::memory_order_relaxed), std::memory_order_relaxed);
    }

    // Moves every gliding value one slice towards its target. Returns false when they
    // have all arrived (then nothing moves and the block renders in one go).
    bool advanceSmoothing() noexcept
    {
        const auto coefficient = 1.0f - std::exp (-(float) smoothingSliceSamples / (smoothingSeconds * (float) sampleRate));
        bool moving = false;

        for (auto& entry : entries)
        {
            auto& value = entry.value;
            if (! value.smoothed)
                continue;

            const auto target = value.target.load (std::memory_order_relaxed);
            auto current = value.current.load (std::memory_order_relaxed);

            if (current == target)
                continue;

            current += coefficient * (target - current);

            if (std::abs (target - current) <= 1.0e-4f * (1.0f + std::abs (target)))
                current = target;
            else
                moving = true;

            value.current.store (current, std::memory_order_relaxed);
        }

        return moving;
    }

    static constexpr size_t smoothingSliceSamples = 32;
    static constexpr float smoothingSeconds = 0.015f; // time constant: ~90% of the way in 35 ms

    const ModuleType type;
    std::atomic<float> inputPeak { 0.0f }, outputPeak { 0.0f };
    Scope scope;
    std::array<float, 4096> scopeInput {}; // audio thread: this block's input, while capturing
    std::deque<Entry> entries; // a deque keeps references stable as values are added
    bool wasOn = false;
    double sampleRate = 48000.0;

    JUCE_DECLARE_NON_COPYABLE (Module)
};
}
