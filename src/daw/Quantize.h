#pragma once

#include <functional>

#include "AudioPool.h"
#include "Edit.h"
#include "analysis/Beats.h"

// Rhythm tools for audio in the DAW: find the tempo and beats of a leading track, line
// the song up with the grid, and quantize a voice or an instrument by warping its clips
// so its onsets land on the grid (or on the leading track's groove). Recordings stay
// dry: quantizing only writes warp maps (see Warp.h).
//
// Message thread: these read clips' audio through the AudioPool synchronously.
namespace daw::rhythm
{
// A track as it plays (clips, warps and transposes; no effects), mixed to mono from 0 s.
// Sampled at the pool's rate divided by `decimation`; the rate is returned in `rate`.
juce::AudioBuffer<float> renderTrack (const Edit&, AudioPool&, int trackId, double& rate, int decimation = 2);

// Tempo and beats of a track (beat times on the timeline).
analysis::rhythm::Tempo analyseTrack (const Edit&, AudioPool&, int trackId);

// The slide (seconds, at most half a bar either way) that puts the analysed downbeats on
// the bar lines of a grid at `tempo.bpm`, keeping every clip at or after 0 s.
double getLineUpShift (const Edit&, const analysis::rhythm::Tempo&);

// Moves every clip and marker by `seconds`. The caller opens the undo transaction.
void shiftSong (Edit&, double seconds);

struct Options
{
    double strength = 1.0;     // 0..1: how far onsets move towards the grid
    double catchRange = 0.5;   // only onsets within this share of a grid step move (0..0.5)
    float sensitivity = 0.5f;  // 0..1: higher finds softer onsets

    // Iterative: start from where the notes play now (after earlier quantizing), so each
    // pass closes `strength` of the distance that's left. Off, every pass starts from the
    // audio as played before any free warp.
    bool iterative = false;

    double safeRange = 0.0;    // 0..0.5 of a grid step: note starts already this close stay put
    double maxMove = 0.0;      // seconds: the farthest a note start moves (0: no limit)
};

// Note starts (hitpoints) in what a clip shows, as times in its rendered copy, in time
// order: the clip's stored hitpoints (filter and hand edits, see Hitpoints.h), or with none
// stored, the automatic ones at this sensitivity (0..1). Disabled ones are left out.
std::vector<analysis::rhythm::Onset> findHitpoints (const Edit&, AudioPool&, int clipId, float sensitivity = 0.5f);

// The same, split so the analysis can run off the message thread: call this on the message
// thread, and the function it returns on any thread (it holds what it needs). Returns an
// empty function for a missing clip or one that isn't audio.
std::function<std::vector<analysis::rhythm::Onset>()> prepareHitpoints (const Edit&, AudioPool&, int clipId, float sensitivity = 0.5f);

struct Result
{
    int onsets = 0; // found in the clips
    int moved = 0;  // pulled towards the grid
};

// Warps the clips so their onsets land on `grid` (timeline seconds, sorted). The onsets are
// the clips' hitpoints as the editor shows them (Hitpoints.h; options.sensitivity sets the
// filter of a clip with none stored; linked tracks with no hand edits use all mics together). Clips that
// play one stretch of the same take together (split or strip-silenced pieces) share one
// warp, so they stay in time with each other and render once. Quantizing again starts
// from the unwarped audio, so a new strength or grid replaces the old result (unless
// options.iterative). The caller
// opens the undo transaction.
Result quantizeClips (Edit&, AudioPool&, const juce::Array<int>& clipIds, const std::vector<double>& grid, const Options&);

// Every clip on a track.
juce::Array<int> getClipIds (const Edit&, int trackId);
}
