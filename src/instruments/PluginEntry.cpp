// The VST3/AU build of one instrument: MICFX_INSTRUMENT_KEY says which (see CMakeLists.txt).
#include "Registry.h"

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return micfx::inst::createInstrument (MICFX_INSTRUMENT_KEY).release();
}
