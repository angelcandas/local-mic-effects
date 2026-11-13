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
    highPass, denoise, gate, pitch, eq, paraEq, smooth, compressor, opto, autoLevel, deEsser, exciter, saturation, radio, bitcrusher,
    robot, filter, autoWah, amp, cab, tremolo, vibrato, chorus, flanger, phaser, doubler, echo, reverb, plate, irReverb, width,
    multiband, limiter, masterEq, imager, loudness, // mastering (see Mastering.h)
    transient, dynamicEq,

    // Not a built-in effect: a third-party VST3/AU effect hosted as a DAW insert (see
    // daw/PluginInsert.h). It comes after numModuleTypes, so Live mode, the parameters
    // and every list of effects leave it out.
    plugin
};

inline constexpr int numModuleTypes = (int) ModuleType::dynamicEq + 1;

// A stable name for each type, for project files, parameter IDs and the UI.
inline const char* getModuleKey (ModuleType type)
{
    if (type == ModuleType::plugin)
        return "plugin";

    static constexpr const char* keys[] = {
        "highPass", "denoise", "gate", "pitch", "eq", "paraEq", "smooth", "compressor", "opto", "autoLevel", "deEsser", "exciter", "saturation", "radio", "bitcrusher",
        "robot", "filter", "autoWah", "amp", "cab", "tremolo", "vibrato", "chorus", "flanger", "phaser", "doubler", "echo", "reverb", "plate", "irReverb", "width",
        "multiband", "limiter", "masterEq", "imager", "loudness", "transient", "dynamicEq"
    };
    static_assert (std::size (keys) == numModuleTypes);
    return keys[(size_t) type];
}

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

    // The song's tempo, for effects that sync to it (the echo). Called with the current
    // tempo (bpm, 0 when there is none) before each block or whenever it changes; any
    // thread. Most effects ignore it.
    virtual void setTempo (double /*bpm*/) noexcept {}

    // Extra live readings for the UI (gain reduction, loudness...), added to the module's
    // meter object. Message thread.
    virtual void addReadings (juce::DynamicObject&) {}

    // Called on a new module, before prepare(), with the module of the same type it
    // replaces (a DAW track's chain is rebuilt when its inserts change), to carry over
    // what it has learnt that isn't a value, such as the denoiser's noise profile. The
    // previous module may still be running. Message thread.
    virtual void carryStateFrom (const Module&) {}

    // Effects that play through a file the user picks (a cabinet or room impulse
    // response). Message thread. loadFile says why it can't use a file (empty when it
    // can); an empty File clears it. The file is the effect's own state, not a value:
    // the DAW keeps it on the insert, Live in the plugin state.
    struct FileChoice
    {
        juce::String valueId; // the choice that selects "your file", set when one loads
        float index = 0.0f;
    };

    virtual bool usesFile() const noexcept                   { return false; }
    virtual juce::String getFileWildcard() const             { return {}; }
    virtual juce::String loadFile (const juce::File&)        { return "This effect doesn't use files."; }
    virtual juce::File getFile() const                       { return {}; }
    virtual FileChoice getFileChoice() const                 { return {}; }

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

        // The input and output gain only act while the effect is switched on (a module
        // that runs while off, to let a tail ring out, passes its input untouched).
        const auto gainsOn = ! alwaysProcesses() || isOn();

        // The spectrum wants the signal before and after; the input is kept aside after
        // the input gain, as the effect hears it.
        const auto capture = scope.isWanted() && block.getNumChannels() > 0;
        const auto captureLength = juce::jmin (numSamples, scopeInput.size());

        for (size_t start = 0; start < numSamples;)
        {
            const auto length = advanceSmoothing() ? std::min (smoothingSliceSamples, numSamples - start) : numSamples - start;
            update();

            auto slice = block.getSubBlock (start, length);

            if (gainsOn)
                applyGainDb (slice, get (*inputGainDb));

            raisePeak (inputPeak, blockPeak (slice));

            if (capture && start < captureLength)
                std::copy_n (slice.getChannelPointer (0), std::min (length, captureLength - start), scopeInput.begin() + (std::ptrdiff_t) start);

            render (Context (slice));

            if (gainsOn)
                applyGainDb (slice, get (*outputGainDb));

            start += length;
        }

        raisePeak (outputPeak, blockPeak (block));

        if (capture)
            scope.push (scopeInput.data(), block.getChannelPointer (0), (int) captureLength);
    }

    // Every module's own input and output gain (dB), before and after its effect.
    // Their IDs are the module key + "InGain" / "OutGain" ("compressorInGain").
    Value& getInputGain() noexcept  { return *inputGainDb; }
    Value& getOutputGain() noexcept { return *outputGainDb; }

    static constexpr float maxGainDb = 24.0f;

protected:
    // The gains are declared first, so every module has them.
    explicit Module (ModuleType t) : type (t)
    {
        const juce::String key (getModuleKey (t));
        inputGainDb = &add (key + "InGain", false, 0.0f, true);
        outputGainDb = &add (key + "OutGain", false, 0.0f, true);
    }

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
        Entry (const juce::String& i, bool s, float v, bool smoothed) : id (i), isSwitch (s), value (v, smoothed) {}

        juce::String id;
        bool isSwitch;
        Value value;
    };

    static void applyGainDb (juce::dsp::AudioBlock<float>& block, float decibels) noexcept
    {
        if (std::abs (decibels) > 0.001f)
            block.multiplyBy (juce::Decibels::decibelsToGain (decibels, -100.0f));
    }

    Value& add (const juce::String& id, bool isSwitchValue, float defaultValue, bool smoothed)
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
    Value* inputGainDb = nullptr;
    Value* outputGainDb = nullptr;
    std::atomic<float> inputPeak { 0.0f }, outputPeak { 0.0f };
    Scope scope;
    std::array<float, 4096> scopeInput {}; // audio thread: this block's input, while capturing
    std::deque<Entry> entries; // a deque keeps references stable as values are added
    bool wasOn = false;
    double sampleRate = 48000.0;

    JUCE_DECLARE_NON_COPYABLE (Module)
};
}
