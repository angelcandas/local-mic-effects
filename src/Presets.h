#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <utility>
#include <vector>

// Factory voice presets. A preset starts from every parameter's default (except input
// gain, which belongs to your mic) and then applies its own values, in real units:
// dB, Hz, %, semitones, 1/0 for switches and the index for choices.
struct Preset
{
    const char* name;
    const char* category;
    std::vector<std::pair<const char*, float>> values;
    // The effects' order (module keys, "eq", "echo"...), first to last. Empty: the
    // standard order (fx::ModuleType's). Keys left out keep their standard places after
    // the listed ones.
    std::vector<const char*> order {};
};

const std::vector<Preset>& getPresets();

// Must be called on the message thread.
void applyPreset (juce::AudioProcessor&, const Preset&);
