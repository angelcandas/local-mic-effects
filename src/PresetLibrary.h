#pragma once

#include <juce_data_structures/juce_data_structures.h>

// The preset library on disk, shared by Live, the DAW and the plug-in:
//
//   <app data>/MicFX/Library/
//     Effects/<kind>/<name>.micfxfx      one effect's settings  (<PRESET kind name><VALUE id value/>...)
//     Chains/<name>.micfxchain           a chain of effects     (<INSERTS name><INSERT type .../>...</INSERTS>)
//     Defaults/Effects/<kind>.micfxfx    the settings a new effect of that kind starts with
//
// <kind> is the effect's preset key (a module id like "comp", or "tune+pitch+harmony" for the
// DAW's pitch insert). A chain is the DAW's INSERTS node without ids: one INSERT per effect,
// in order, with its values as properties. Live loads a chain that has at most one of each
// built-in effect and no plug-ins ("live" in the listing).
//
// Factory presets stay in code; this holds yours. Names are trimmed to 60 characters and
// made safe for file names. Message thread only.
class PresetLibrary
{
public:
    PresetLibrary();                            // the app's own folder
    explicit PresetLibrary (juce::File root);   // somewhere else (tests)

    const juce::File& getRoot() const noexcept { return root; }
    void setRoot (const juce::File& folder)        { root = folder; } // tests

    //==============================================================================
    // Effect presets: { kind: [{ name, values: { id: value } }] }, names sorted.
    juce::var listEffects() const;
    juce::String saveEffect (const juce::String& kind, const juce::String& name, const juce::var& values);
    bool deleteEffect (const juce::String& kind, const juce::String& name);
    juce::String renameEffect (const juce::String& kind, const juce::String& name, const juce::String& newName);

    // The default for new effects of a kind (values, or void when there is none).
    juce::var getDefault (const juce::String& kind) const;
    juce::String setDefault (const juce::String& kind, const juce::var& values);
    bool clearDefault (const juce::String& kind);

    //==============================================================================
    // Chains: [{ name, effects: [type...], live }], names sorted.
    juce::var listChains() const;
    juce::String saveChain (const juce::String& name, const juce::ValueTree& inserts);
    juce::ValueTree loadChain (const juce::String& name) const; // invalid if there's no such chain
    bool deleteChain (const juce::String& name);
    juce::String renameChain (const juce::String& name, const juce::String& newName);

    // The chain a new audio track starts with (an INSERTS node; invalid when there is none).
    juce::ValueTree getDefaultChain() const;
    juce::String setDefaultChain (const juce::ValueTree& inserts);
    bool clearDefaultChain();

    // A chain Live can play: built-in effects only, at most one of each.
    static bool isLiveChain (const juce::ValueTree& inserts);

    //==============================================================================
    // The presets kept in the app's settings before the library ({ kind: [{ name, values }] }
    // as JSON). Imported once: later calls do nothing. Returns how many were imported.
    int importOnce (const juce::String& json);

    static juce::String safeName (const juce::String& name);

private:
    juce::File effectFile (const juce::String& kind, const juce::String& name) const;
    juce::File chainFile (const juce::String& name) const;
    juce::File defaultFile (const juce::String& kind) const;

    static juce::String writeValues (const juce::File& file, const juce::String& kind, const juce::String& name, const juce::var& values);
    static juce::var readValues (const juce::File& file);

    juce::File root;
};
