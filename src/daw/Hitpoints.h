#pragma once

#include <functional>
#include <optional>
#include <vector>

#include "AudioPool.h"
#include "Edit.h"
#include "analysis/Beats.h"

// Hitpoints: where the notes and hits of a clip's take start, as the clip editor shows them
// and as every later step uses them (quantize, split, markers, groove, warp tabs, MIDI).
//
// Stored per clip in the file's own time, so they survive warping, splitting and moving:
//   hitFilter  "threshold:minLengthMs:beats"   which detected note starts count
//   hitpoints  "t:flag;t:flag..."              hand edits, flags + added, - disabled, = locked
// Nothing stored: automatic, with the default filter (as the app always did).
//
// The take is analysed once at full sensitivity (analysis::rhythm::findOnsetCandidatesCached);
// the filter only picks from those candidates, so moving a slider answers at once.
namespace daw::hitpoints
{
struct Filter
{
    double threshold = 0.5;  // 0..1: higher keeps only clearer note starts (1 - the old sensitivity)
    double minLength = 0.05; // seconds: of two note starts closer than this, the stronger stays
    double beats = 0.0;      // 0: off; else a note value in beats (0.25 = 1/16): keep only near that grid

    bool operator== (const Filter& o) const noexcept;
};

Filter parseFilter (const juce::String&);
juce::String toString (const Filter&);

enum class State { automatic, added, disabled, locked };

struct HandEdit
{
    double time = 0.0; // file seconds
    State state = State::added;
};

std::vector<HandEdit> parseEdits (const juce::String&);
juce::String toString (const std::vector<HandEdit>&);

struct Hit
{
    double time = 0.0;     // file seconds
    float strength = 0.0f; // 0..1
    float peak = 0.0f;     // linear level of its attack
    State state = State::automatic;

    bool isEnabled() const noexcept { return state != State::disabled; }
};

// A hand edit and a note start this close (seconds) are the same one.
constexpr double matchSeconds = 0.006;

// Candidates -> filter -> hand edits, between `from` and `to` (file seconds). `beatOf` maps a
// file time to a project beat for the Beats filter (unused when the filter has none).
std::vector<Hit> select (const std::vector<analysis::rhythm::OnsetCandidate>&, double from, double to, const Filter&,
                         const std::vector<HandEdit>&, const std::function<double (double)>& beatOf = {});

bool hasStored (const juce::ValueTree& clip);
bool hasEdits (const juce::ValueTree& clip);
Filter getFilter (const juce::ValueTree& clip);
std::vector<HandEdit> getEdits (const juce::ValueTree& clip);

// A clip's hitpoints (file time) over its window, the stored filter unless `filter` is given.
// Split so the analysis can run off the message thread: call prepare on the message thread
// and the function it returns on any thread. Empty function: no such audio clip.
std::function<std::vector<Hit>()> prepare (const juce::ValueTree& clip, AudioPool&, std::optional<Filter> filter = std::nullopt);
std::vector<Hit> get (const juce::ValueTree& clip, AudioPool&, std::optional<Filter> filter = std::nullopt);

// Only the enabled ones' times.
std::vector<double> enabledTimes (const std::vector<Hit>&);

// A groove from hits on the timeline: each hit's place within its bar (in beats, sorted,
// close ones merged), for a grid that repeats every `bars` bars.
struct Groove
{
    juce::String name;
    int bars = 1;
    std::vector<double> offsets; // beats from the bar line, 0 .. bars * beatsPerBar
};

Groove makeGroove (const juce::String& name, const std::vector<double>& timelineHits, const TempoMap&, int beatsPerBar, int bars = 1);

// PROJECT `grooves`: "name=bars:o1,o2,...|name=..." (names without "=", "|", ":").
std::vector<Groove> parseGrooves (const juce::String&);
juce::String toString (const std::vector<Groove>&);

// The groove repeated over [from, to] timeline seconds: a quantize grid.
std::vector<double> makeGrid (const Groove&, const TempoMap&, int beatsPerBar, double from, double to);
}
