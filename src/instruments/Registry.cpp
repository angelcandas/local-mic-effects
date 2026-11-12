#include "Registry.h"
#include "InstrumentProcessor.h"

namespace micfx::inst
{
std::vector<const Spec*> getAllSpecs()
{
    return { &getGrandSpec(), &getNovaSpec() };
}

const Spec* findSpec (const juce::String& key)
{
    for (const auto* spec : getAllSpecs())
        if (spec->key == key)
            return spec;
    return nullptr;
}

std::unique_ptr<juce::AudioProcessor> createInstrument (const juce::String& key)
{
    if (const auto* spec = findSpec (key))
        return std::make_unique<InstrumentProcessor> (*spec);
    return {};
}
}
