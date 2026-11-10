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
        jassert (module != nullptr);
        return *modules.emplace_back (std::move (module));
    }

    int size() const noexcept                  { return (int) modules.size(); }
    Module& operator[] (int index) noexcept    { return *modules[(size_t) index]; }

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

    void prepare (const juce::dsp::ProcessSpec& spec)
    {
        for (auto& module : modules)
            module->prepare (spec);
    }

    void reset()
    {
        for (auto& module : modules)
            module->reset();
    }

    // Audio thread.
    void process (const Module::Context& context) noexcept
    {
        for (auto& module : modules)
            module->process (context);
    }

    int getLatencySamples() const noexcept
    {
        int total = 0;
        for (auto& module : modules)
            total += module->getLatencySamples();
        return total;
    }

private:
    std::vector<std::unique_ptr<Module>> modules;

    JUCE_DECLARE_NON_COPYABLE (FxChain)
};
}
