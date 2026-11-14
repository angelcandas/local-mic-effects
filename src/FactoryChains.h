#pragma once

#include <juce_data_structures/juce_data_structures.h>

#include <utility>
#include <vector>

// Factory effect chains, kept in code and listed read-only next to the library's own
// (PresetLibrary). The first set is the vocal chains: whole-voice sounds by style, each
// an ordered list of effects with their settings.
//
// Every vocal chain is level-matched for a voice at about -18 LUFS with peaks near
// -8 dBFS (what "Fit to my voice" aims for): it comes out within about 1 LU of what goes
// in, so switching chains compares sounds, not loudness. Chains marked live hold at
// most one of each built-in effect, so the same chain plays in Live, the plug-in and on
// a DAW track; the others are for a DAW track or bus.
namespace factory
{
struct Device
{
    const char* type; // module key ("compressor", "plate"...)
    std::vector<std::pair<const char*, float>> values; // anything left out is the effect's default
};

struct Chain
{
    const char* name;
    const char* folder;      // "Vocals", "Vocals/Returns"
    const char* description; // one line
    std::vector<Device> devices;
    bool dawOnly = false;    // a bus or a return: not offered in Live
};

const std::vector<Chain>& getChains();

// The chain as the library's INSERTS node: one INSERT per device, in order, each with
// every value of its effect (the effect's defaults, then the chain's settings, switched
// on). Invalid if there is no chain with that name.
juce::ValueTree makeInserts (const juce::String& name);

// [{ name, folder, description, effects: [type...], live, factory: true }], in order.
juce::var listChains();
}
