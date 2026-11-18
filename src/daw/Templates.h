#pragma once

#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>

#include <vector>

#include "Edit.h"

// Project templates: a starting point for a new project (tracks, vocal chains, sends,
// tempo, loop), either built in or saved from one of your own projects.
//
// Built-in ones are described here as data and put into an empty Edit by DawBridge (which
// knows the instruments and the vocal chains). Your own are the Edit's tree with the
// recorded audio left out, one .micfxtemplate file each in the MicFX/Templates folder.
namespace daw::templates
{
struct TrackSpec
{
    const char* name;
    const char* instrument = "";   // a MIDI track's instrument id ("builtin:kit"); empty: an audio track
    const char* chain = "";        // the name of a voice preset (Presets.cpp) for the track's effects; empty: no effects
    bool arm = false;              // armed and monitored: ready to sing on
    float reverbSendDb = -100.0f;  // send to the first return (the reverb); -100: none
    const char* colour = "";
};

struct Template
{
    const char* key;
    const char* name;
    const char* description;
    double tempo = 120.0;
    int beatsPerBar = 4;
    int loopBars = 0;              // a loop over this many bars from the start (0: none)
    bool metronome = false;
    int countInBars = 0;
    std::vector<TrackSpec> tracks;
};

const std::vector<Template>& getBuiltIn();
const Template* findBuiltIn (const juce::String& key);

//==============================================================================
struct UserTemplate
{
    juce::String name;
    juce::File file;
};

juce::File getUserFolder();
juce::Array<UserTemplate> listUser();

// A file name made from `name` (no slashes or dots that would change the folder).
juce::String cleanName (const juce::String& name);

// Writes the Edit's session as a template: audio clips, frozen audio and the view's state
// are left out (MIDI clips stay). Returns an error message, or empty. An existing one of
// that name is replaced.
juce::String saveUser (const juce::ValueTree& editState, const juce::String& name);

// Removes one of your templates. Returns an error message, or empty.
juce::String removeUser (const juce::String& name);

// The saved session of one of your templates, or an invalid tree.
juce::ValueTree readUser (const juce::String& name);
}
