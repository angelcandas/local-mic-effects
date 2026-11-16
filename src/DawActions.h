#pragma once

#include <juce_core/juce_core.h>
#include <functional>
#include <map>
#include <vector>

class DawBridge;
class PluginProcessor;

// The catalogue of things the app can do, as named, typed actions (B1 of the AI plan).
//
// It sits on top of the DAW bridge's commands: every action has a stable name ("track.mute"),
// a one-line description, typed arguments with ranges, and answers { ok, summary, ... } (or
// { ok: false, error }). A write action is ONE undo step with a readable name, however many
// bridge commands it takes. Anything that drives the app (the page's command box, the local MCP
// server) goes through the two bridge calls "actions.list" and "actions.run".
//
// Targets resolve flexibly (see resolveTracks / resolveTime). Message thread only.
class DawActions
{
public:
    using Complete = std::function<void (juce::var)>;

    struct Param
    {
        juce::String name, type, description; // type: track, tracks, time, string, number, integer, boolean, object, array
        bool required = false;
        bool hasRange = false;
        double min = 0.0, max = 0.0;
        juce::StringArray choices;            // for strings with a fixed set
    };

    struct Action
    {
        juce::String name, description, undoName; // undoName empty: reads (no undo step)
        std::vector<Param> params;
        bool readOnly = true;
        std::function<void (const juce::var& args, Complete)> run;
    };

    DawActions (PluginProcessor&, DawBridge&);

    // { ok, actions: [{ name, description, readOnly, params: [{ name, type, description, required, min?, max?, choices? }] }] }
    juce::var list() const;

    // Runs one action; always completes (once, maybe later: Auto Mix listens first).
    void run (const juce::String& name, const juce::var& args, Complete);

    // The bridge tells us about its events ("dawAutoMix", "dawMixCheck"...): finishes the
    // actions that were waiting on one.
    void onEvent (const juce::Identifier& event, const juce::var& payload);

    // Track targets: an id, a 1-based number, a name (exact, then contains, any case), a role tag
    // ("voice", "instrumental", "drums"...; also "instrumentals"), "selected", "all", or a list of
    // those. Fills `problem` when something matches nothing.
    std::vector<int> resolveTracks (const juce::var& spec, juce::String& problem) const;

    // Time in seconds: a number, "12.5", "bar 9", "bars 9", a section name ("chorus", "chorus 2"),
    // "start", "end", "playhead", "loop".
    std::optional<double> resolveTime (const juce::var& spec, juce::String& problem) const;
    // A section's or loop's range, for "from": "chorus" -> [start, end]. Fills `problem` on a miss.
    std::optional<std::pair<double, double>> resolveRange (const juce::var& spec, juce::String& problem) const;

    // How a track is called back to the person ("Vocals (track 2)").
    juce::String trackLabel (int trackId) const;

private:
    void build();
    void add (const char* name, const char* description, const char* undoName, std::vector<Param> params,
              std::function<void (const juce::var&, Complete)> run);

    juce::var call (const juce::String& command, const juce::var& args); // a bridge command that answers at once
    int resolveReturn (const juce::var& spec, juce::String& problem) const;
    std::vector<int> allTrackIds() const;
    int selectedTrack() const;

    PluginProcessor& processor;
    DawBridge& bridge;
    std::vector<Action> actions;
    std::map<juce::String, size_t> byName;

    Complete waitingAutoMix, waitingMixCheck;
    bool autoMixApply = true;
};
