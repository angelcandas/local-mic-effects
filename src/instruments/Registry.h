#pragma once

#include "InstrumentCore.h"

// Every MicFX instrument, by key.
namespace micfx::inst
{
const Spec& getGrandSpec();
const Spec& getNovaSpec();
const Spec& getTinesSpec();
const Spec& getDrawbarSpec();
const Spec& getPluckSpec();
const Spec& getMalletsSpec();
const Spec& getBeatSpec();
const Spec& getKitSpec();

// In the order the library lists them.
std::vector<const Spec*> getAllSpecs();
const Spec* findSpec (const juce::String& key);

// A new instrument (InstrumentProcessor), or null for an unknown key.
std::unique_ptr<juce::AudioProcessor> createInstrument (const juce::String& key);
}
