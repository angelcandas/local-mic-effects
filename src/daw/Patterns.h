#pragma once

#include "Edit.h"

#include <vector>

// How patterns play (the model and its editing are in Edit.h). Plain functions of the
// tree, used by the engine when it builds a graph and by the tests.
//
// Swing delays every second step (odd step indices) by swing x step / 3, so at 1 a pair
// of steps plays as a triplet (two thirds, one third), like TempoMap::makeGrid's swing.
// Steps of a beat or longer don't swing. Only notes that sit on an odd step (within a
// quarter of a step) move; notes between steps play where they are.
namespace daw::patterns
{
struct Event
{
    int pitch = 60, velocity = 100;
    double on = 0.0, off = 0.0; // beats (getPatternEvents) or seconds (the others)
};

// The notes one pass of a pattern plays on a track, swung, in beats from the pattern's
// start, in time order. Notes at or after the pattern's end don't play. Empty when the
// pattern has no part for the track or the part is muted.
std::vector<Event> getPatternEvents (const juce::ValueTree& pattern, int trackId);

// Seconds on the timeline: every unmuted block's notes for the track, the pattern repeated
// over the block and notes cut at the block's end, through the tempo map.
std::vector<Event> getBlockEvents (const juce::ValueTree& project, int trackId, const TempoMap&);

// Pattern mode: the pattern played as a loop from 0 seconds, one pass long. The pass
// before 0 is included too (negative times), for tracks delayed by latency compensation,
// whose window reaches before the loop's start. Notes end at the end of their pass.
std::vector<Event> getLoopEvents (const juce::ValueTree& pattern, int trackId, const TempoMap&);

// One pass of a pattern from 0, in seconds.
double getLoopSeconds (const juce::ValueTree& pattern, const TempoMap&);

// Where the last pattern block ends (muted ones too), in seconds; 0 when there are none.
double getBlocksEnd (const juce::ValueTree& project, const TempoMap&);
}
