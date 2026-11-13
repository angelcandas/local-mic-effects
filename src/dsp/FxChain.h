#pragma once

#include "FxModules.h"

#include <memory>
#include <vector>

namespace fx
{
// An ordered list of effect modules, run one after another on a block. Live mode uses
// one chain holding every module type; DAW mode gives every track (and the master bus)
// its own chain holding just the inserts the user added.
//
// The list itself is not thread-safe: build it before prepare() and don't change it
// while process() may be running. Module values can be changed from any thread.
//
// Modules are shared: a DAW track's chain is rebuilt when its inserts change, and the new
// chain takes over the modules that are already running (add (module, true)), so a
// reverb tail or a compressor's envelope goes on through the edit. Only one chain at a
// time may process a shared module (the engine's graphs take turns).
class FxChain
{
public:
    FxChain() = default;

    explicit FxChain (const std::vector<ModuleType>& types)
    {
        for (auto type : types)
            add (createModule (type));
    }

    // Every module type in Live mode order.
    static std::vector<ModuleType> getAllTypes()
    {
        std::vector<ModuleType> types;
        for (int i = 0; i < numModuleTypes; ++i)
            types.push_back ((ModuleType) i);
        return types;
    }

    Module& add (std::unique_ptr<Module> module)
    {
        return add (std::shared_ptr<Module> (std::move (module)), false);
    }

    // A module that may already be running in another chain. `alreadyPrepared`: it was
    // prepared for this chain's rate and block size, and prepare() leaves it as it is.
    Module& add (std::shared_ptr<Module> module, bool alreadyPrepared)
    {
        jassert (module != nullptr);
        running.push_back (alreadyPrepared);
        return *modules.emplace_back (std::move (module));
    }

    int size() const noexcept                  { return (int) modules.size(); }
    Module& operator[] (int index) noexcept    { return *modules[(size_t) index]; }

    // The chain's own reference to a module in it (null if it isn't), for handing it on
    // to the chain that replaces this one.
    std::shared_ptr<Module> share (const Module& module) const
    {
        for (const auto& held : modules)
            if (held.get() == &module)
                return held;
        return nullptr;
    }

    // The first module of that type, or null.
    Module* find (ModuleType type) noexcept
    {
        for (auto& module : modules)
            if (module->getType() == type)
                return module.get();

        return nullptr;
    }

    template <typename ModuleClass>
    ModuleClass* find() noexcept
    {
        for (auto& module : modules)
            if (auto* found = dynamic_cast<ModuleClass*> (module.get()))
                return found;

        return nullptr;
    }

    // Sets the value with that ID on the first module that has one. Returns false
    // when no module in the chain uses that ID.
    bool setValue (const juce::String& id, float value) noexcept
    {
        for (auto& module : modules)
            if (auto* found = module->findValue (id))
            {
                found->store (value, std::memory_order_relaxed);
                return true;
            }

        return false;
    }

    //==============================================================================
    // The order the modules play in (Live mode: the singer reorders the one chain while
    // it runs). Indices into the list, a permutation of 0..size()-1; empty is the order
    // they were added in. Message thread. A change while playing dips the sound for a few
    // milliseconds around the swap, so it doesn't click; the audio thread picks it up
    // without waiting.
    void setOrder (std::vector<int> order)
    {
        if (! isPermutation (order))
        {
            order.clear();
            for (int i = 0; i < size(); ++i)
                order.push_back (i);
        }
        if (order == getOrder())
            return; // no change: no dip

        wantedOrder = order;

        const juce::SpinLock::ScopedLockType lock (orderLock);
        pendingOrder = wantedOrder;
        orderPending.store (true, std::memory_order_release);
    }

    // The order set last (the order they were added in, if none). Message thread.
    std::vector<int> getOrder() const
    {
        if (! wantedOrder.empty())
            return wantedOrder;
        std::vector<int> order;
        for (int i = 0; i < size(); ++i)
            order.push_back (i);
        return order;
    }

    bool isPermutation (const std::vector<int>& order) const
    {
        if ((int) order.size() != size())
            return false;
        std::vector<bool> seen ((size_t) size(), false);
        for (const auto index : order)
        {
            if (! juce::isPositiveAndBelow (index, size()) || seen[(size_t) index])
                return false;
            seen[(size_t) index] = true;
        }
        return true;
    }

    // Hands the tempo (bpm, 0 for none) to every module (see Module::setTempo).
    void setTempo (double bpm) noexcept
    {
        for (auto& module : modules)
            module->setTempo (bpm);
    }

    // Gain before and after the modules (the chain's input and output trims), in dB.
    // Any thread; they glide over ~50 ms on the audio thread.
    void setGains (float inputDecibels, float outputDecibels) noexcept
    {
        inputGain.store (juce::Decibels::decibelsToGain (inputDecibels, -100.0f), std::memory_order_relaxed);
        outputGain.store (juce::Decibels::decibelsToGain (outputDecibels, -100.0f), std::memory_order_relaxed);
    }

    // Peak levels (linear) after the input gain and after the output gain, since the
    // last call. Any thread.
    float takeInputPeak() noexcept  { return inputPeak.exchange (0.0f, std::memory_order_relaxed); }
    float takeOutputPeak() noexcept { return outputPeak.exchange (0.0f, std::memory_order_relaxed); }

    void prepare (const juce::dsp::ProcessSpec& spec)
    {
        for (auto* smoothed : { &inputSmoothed, &outputSmoothed })
            smoothed->reset (spec.sampleRate, 0.05);

        inputSmoothed.setCurrentAndTargetValue (inputGain.load (std::memory_order_relaxed));
        outputSmoothed.setCurrentAndTargetValue (outputGain.load (std::memory_order_relaxed));

        // Modules handed over already running keep their state (only this time: a later
        // prepare, for a new rate, prepares them too).
        for (size_t i = 0; i < modules.size(); ++i)
            if (! running[i])
                modules[i]->prepare (spec);

        std::fill (running.begin(), running.end(), false);

        // Not playing now: the order goes straight in. The dip around a later change is
        // ~2.5 ms each way.
        orderFadeStep = 1.0f / (float) juce::jmax (1.0, spec.sampleRate * 0.0025);
        orderFade = 1.0f;
        orderFading = 0;
        {
            const juce::SpinLock::ScopedLockType lock (orderLock);
            playingOrder = wantedOrder;
            playingOrder.reserve ((size_t) size());
            pendingOrder.reserve ((size_t) size());
            orderPending.store (false, std::memory_order_relaxed);
        }
    }

    void reset()
    {
        for (auto& module : modules)
            module->reset();
    }

    // Audio thread.
    void process (const Module::Context& context) noexcept
    {
        auto& block = context.getOutputBlock();

        applyGain (block, inputSmoothed, inputGain);
        raisePeak (inputPeak, blockPeak (block));

        if (playingOrder.empty())
        {
            for (auto& module : modules)
                module->process (context);
        }
        else
        {
            for (const auto index : playingOrder)
                modules[(size_t) index]->process (context);
        }

        if (orderFading != 0 || orderPending.load (std::memory_order_acquire))
            fadeAroundReorder (block);

        applyGain (block, outputSmoothed, outputGain);
        raisePeak (outputPeak, blockPeak (block));
    }

    int getLatencySamples() const noexcept
    {
        int total = 0;
        for (auto& module : modules)
            total += module->getLatencySamples();
        return total;
    }

private:
    // Audio thread: fades out, takes the new order (if the message thread isn't writing
    // it just now; else tries again next block, still silent), fades back in.
    void fadeAroundReorder (juce::dsp::AudioBlock<float>& block) noexcept
    {
        if (orderFading == 0)
            orderFading = -1;

        const auto numSamples = (int) block.getNumSamples();
        const auto numChannels = (int) block.getNumChannels();

        for (int i = 0; i < numSamples; ++i)
        {
            if (orderFading == -1 && orderFade <= 0.0f)
            {
                const juce::SpinLock::ScopedTryLockType lock (orderLock);
                if (lock.isLocked() && orderPending.load (std::memory_order_acquire))
                {
                    playingOrder.assign (pendingOrder.begin(), pendingOrder.end()); // reserved: no allocation
                    orderPending.store (false, std::memory_order_relaxed);
                    orderFading = 1;
                }
                else if (lock.isLocked())
                {
                    orderFading = 1; // nothing new after all
                }
            }

            orderFade = juce::jlimit (0.0f, 1.0f, orderFade + (float) orderFading * orderFadeStep);
            for (int ch = 0; ch < numChannels; ++ch)
                block.getChannelPointer ((size_t) ch)[i] *= orderFade;

            if (orderFading == 1 && orderFade >= 1.0f)
            {
                orderFading = 0;
                // Another change came in while fading back: dip again from here.
                if (orderPending.load (std::memory_order_acquire))
                    orderFading = -1;
                else
                {
                    orderFade = 1.0f;
                    return;
                }
            }
        }
    }

    static void applyGain (juce::dsp::AudioBlock<float>& block, juce::SmoothedValue<float>& smoothed,
                           const std::atomic<float>& target) noexcept
    {
        smoothed.setTargetValue (target.load (std::memory_order_relaxed));

        if (smoothed.isSmoothing())
            block.multiplyBy (smoothed);
        else if (! juce::approximatelyEqual (smoothed.getTargetValue(), 1.0f))
            block.multiplyBy (smoothed.getTargetValue());
    }

    std::vector<std::shared_ptr<Module>> modules;
    std::vector<bool> running; // per module: added already prepared, prepare() skips it

    std::vector<int> wantedOrder;   // message thread
    std::vector<int> pendingOrder;  // under orderLock
    std::vector<int> playingOrder;  // audio thread (empty: the order they were added in)
    juce::SpinLock orderLock;
    std::atomic<bool> orderPending { false };
    float orderFade = 1.0f, orderFadeStep = 0.01f; // audio thread
    int orderFading = 0;                           // audio thread: -1 out, 1 in, 0 none

    std::atomic<float> inputGain { 1.0f }, outputGain { 1.0f };
    std::atomic<float> inputPeak { 0.0f }, outputPeak { 0.0f };
    juce::SmoothedValue<float> inputSmoothed { 1.0f }, outputSmoothed { 1.0f }; // audio thread

    JUCE_DECLARE_NON_COPYABLE (FxChain)
};

//==============================================================================
// Effects whose picture is a frequency response; the UI draws the spectrum behind it.
inline bool hasSpectrumView (ModuleType type) noexcept
{
    return type == ModuleType::highPass || type == ModuleType::eq || type == ModuleType::paraEq
        || type == ModuleType::deEsser || type == ModuleType::radio || type == ModuleType::filter
        || type == ModuleType::masterEq || type == ModuleType::smooth || type == ModuleType::cab || type == ModuleType::amp;
}

// One module's live readings for the UI: { in, out } peak dB since the last call, plus
// { pre, post } spectrum bands for the frequency pictures (which keeps their capture
// running). Null while the module is off. Message thread.
inline juce::var describeMeter (Module& module, SpectrumAnalyser& analyser)
{
    const auto in = module.takeInputPeak(), out = module.takeOutputPeak();

    if (! module.isOn())
        return {};

    auto* result = new juce::DynamicObject();
    result->setProperty ("in", peakToDecibels (in));
    result->setProperty ("out", peakToDecibels (out));

    module.addReadings (*result);

    if (hasSpectrumView (module.getType()))
    {
        module.getScope().request();
        result->setProperty ("spectrum", analyser.analyse (module.getScope(), module.getSampleRate()));
    }

    return juce::var (result);
}
}
