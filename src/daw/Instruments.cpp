#include "Instruments.h"

#include "instruments/InstrumentProcessor.h"
#include "instruments/Registry.h"

namespace daw
{
namespace
{
    // The first built-in sounds (one simple synth) now play on the MicFX instruments: a
    // project that used "builtin:pad" opens on Nova's Warm Pad.
    struct Alias
    {
        const char* key;
        const char* name;
        const char* instrument;
        const char* preset;
    };

    constexpr Alias aliases[] = {
        { "keys",    "Keys",       "tines",   "Classic" },
        { "organ",   "Organ",      "drawbar", "Jazz Trio" },
        { "piano",   "Soft Piano", "grand", "Felt Piano" },
        { "pad",     "Warm Pad",   "nova",  "Warm Pad" },
        { "strings", "Strings",    "nova",  "Strings" },
        { "bass",    "Synth Bass", "nova",  "Synth Bass" },
        { "lead",    "Lead",       "nova",  "Solo Lead" },
    };

    const Alias* findAlias (const juce::String& key)
    {
        for (const auto& a : aliases)
            if (key == a.key)
                return &a;
        return nullptr;
    }

    instruments::PluginLoader& getLoader()
    {
        static instruments::PluginLoader loader;
        return loader;
    }

    bool onMessageThread()
    {
        auto* mm = juce::MessageManager::getInstanceWithoutCreating();
        return mm == nullptr || mm->isThisTheMessageThread();
    }
}

namespace instruments
{
    juce::Array<Edit::InstrumentInfo> getBuiltins()
    {
        juce::Array<Edit::InstrumentInfo> list;
        for (const auto* spec : micfx::inst::getAllSpecs())
            list.add ({ juce::String ("builtin:") + spec->key, spec->name, builtinFormat, "MicFX", {}, {} });
        for (const auto& a : aliases)
            list.add ({ juce::String ("builtin:") + a.key, a.name, builtinFormat, "MicFX", {}, {} });
        return list;
    }

    std::optional<Edit::InstrumentInfo> findBuiltin (const juce::String& id)
    {
        for (const auto& info : getBuiltins())
            if (info.id == id)
                return info;
        return std::nullopt;
    }

    Edit::InstrumentInfo getDefault() { return getBuiltins().getFirst(); }

    InstrumentPtr create (const juce::ValueTree& instrument, double sampleRate, int blockSize, bool realtime,
                          juce::String& error)
    {
        const auto id = instrument[ids::plugin].toString();
        std::unique_ptr<juce::AudioProcessor> processor;

        if (id.startsWith ("builtin:"))
        {
            const auto key = id.fromFirstOccurrenceOf (":", false, false);
            const auto* alias = findAlias (key);
            if (auto instrumentProcessor = micfx::inst::createInstrument (alias != nullptr ? alias->instrument : key))
            {
                // A new instrument starts on its first preset (or the old sound's).
                if (instrument[ids::state].toString().isEmpty())
                    if (auto* p = dynamic_cast<micfx::inst::InstrumentProcessor*> (instrumentProcessor.get()))
                    {
                        if (alias == nullptr || ! p->loadPreset (alias->preset))
                            p->setCurrentProgram (0);
                    }
                processor = std::move (instrumentProcessor);
            }
            else
                error = "Unknown built-in instrument " + id + ".";
        }
        else if (id.isNotEmpty())
        {
            const auto load = [&]
            {
                if (auto& loader = getLoader())
                    processor = loader (instrument, sampleRate, blockSize, error);
                else
                    error = "Instrument plugins can't be loaded here.";
            };

            if (onMessageThread())
                load();
            else
                juce::MessageManager::getInstance()->callFunctionOnMessageThread ([] (void* f) -> void*
                                                                                 { (*static_cast<decltype (load)*> (f))(); return nullptr; },
                                                                                 (void*) &load);

            if (processor == nullptr && error.isEmpty())
                error = "Couldn't load " + instrument[ids::name].toString() + ".";
        }

        if (processor == nullptr)
            return {};

        if (const auto state = instrument[ids::state].toString(); state.isNotEmpty())
        {
            juce::MemoryBlock block;
            if (block.fromBase64Encoding (state) && block.getSize() > 0)
                processor->setStateInformation (block.getData(), (int) block.getSize());
        }

        processor->setNonRealtime (! realtime);
        processor->setRateAndBufferSizeDetails (sampleRate, blockSize);
        processor->prepareToPlay (sampleRate, blockSize);

        // Instruments are released and deleted on the message thread (their parameters and
        // windows live there).
        return InstrumentPtr (processor.release(), [] (juce::AudioProcessor* p)
        {
            const auto destroy = [p] { p->releaseResources(); delete p; };

            if (onMessageThread())
                destroy();
            else
                juce::MessageManager::getInstance()->callFunctionOnMessageThread ([] (void* f) -> void*
                                                                                 { (*static_cast<decltype (destroy)*> (f))(); return nullptr; },
                                                                                 (void*) &destroy);
        });
    }

    bool hasWindow (const juce::String& id)
    {
        if (! id.startsWith ("builtin:"))
            return true;
        const auto key = id.fromFirstOccurrenceOf (":", false, false);
        return micfx::inst::findSpec (key) != nullptr || findAlias (key) != nullptr;
    }

    juce::String getState (juce::AudioProcessor& processor)
    {
        juce::MemoryBlock block;
        processor.getStateInformation (block);
        return block.getSize() > 0 ? block.toBase64Encoding() : juce::String();
    }

    void setPluginLoader (PluginLoader loader) { getLoader() = std::move (loader); }
}
}
