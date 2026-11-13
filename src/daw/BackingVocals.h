#pragma once

#include "Edit.h"
#include "analysis/Harmony.h"

// Backing vocals as real clips: each voice (analysis::harmony::Voice) becomes a copy of the
// lead clip on its own track, at the same place, with the voice's notes as its note edits
// (so the note pitch editor can fix any of them) and its timing nudges as warp markers
// laid under the lead's own warp. The warp render then sings it.
namespace daw::backing
{
// The lead's sung notes the clip plays (file seconds): its stored note edits, else the
// notes found in `track` (the take's pitch), cut to the clip's window.
std::vector<analysis::pitch::Note> getLeadNotes (const juce::ValueTree& clip, const analysis::pitch::Track& track);

// The clip's free warp (see Edit::getClipWarp) with each note's start moved by its offset.
// The clip's window edges stay where they are, so the copy lines up with the lead.
warp::Map nudgeWarp (const juce::ValueTree& clip, const std::vector<analysis::pitch::Note>&, const std::vector<double>& offsets);

// Adds the voice on `trackId` as a copy of `leadClipId` (one undoable step each, inside
// whatever transaction the caller opened). Returns the new clip's id, or -1.
int addVoiceClip (Edit&, int leadClipId, int trackId, const analysis::harmony::Voice&);
}
