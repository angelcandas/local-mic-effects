#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "Edit.h"

#include <memory>

// Instruments for MIDI tracks: a few built-in synths, so a MIDI track makes sound
// without any plugin, and VST3/AU instrument plugins through PluginHost.
//
// An instrument is a juce::AudioProcessor: MIDI in, stereo out. The engine makes one per
// MIDI track from the track's INSTRUMENT tree (see Edit.h) and restores its saved state.
namespace daw
{
using InstrumentPtr = std::shared_ptr<juce::AudioProcessor>;

namespace instruments
{
    constexpr const char* builtinFormat = "builtin";

    // The built-in synths ("builtin:keys", "builtin:pad", ...), in display order.
    juce::Array<Edit::InstrumentInfo> getBuiltins();
    std::optional<Edit::InstrumentInfo> findBuiltin (const juce::String& id);
    Edit::InstrumentInfo getDefault(); // what a new MIDI track starts with

    // Makes the instrument an INSTRUMENT tree describes, with its saved state, prepared
    // for the rate and block size. Plugins load on the message thread (a call from
    // another thread, e.g. an export, waits for it there) and are deleted there too.
    // Returns null and sets `error` if it can't (a plugin that isn't installed).
    InstrumentPtr create (const juce::ValueTree& instrument, double sampleRate, int blockSize, bool realtime,
                          juce::String& error);

    // Reads an instrument's settings, for INSTRUMENT `state` (base64). Message thread.
    juce::String getState (juce::AudioProcessor&);

    // How plugins are made (PluginHost installs this). Called on the message thread.
    using PluginLoader = std::function<std::unique_ptr<juce::AudioProcessor> (const juce::ValueTree& instrument,
                                                                             double sampleRate, int blockSize,
                                                                             juce::String& error)>;
    void setPluginLoader (PluginLoader);
}
}
