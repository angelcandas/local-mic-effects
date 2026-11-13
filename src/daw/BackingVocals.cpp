#include "BackingVocals.h"

namespace daw::backing
{
namespace pitch = analysis::pitch;

std::vector<pitch::Note> getLeadNotes (const juce::ValueTree& clip, const pitch::Track& track)
{
    const auto map = Edit::getEffectiveWarp (clip);
    const auto offset = (double) clip[ids::offset], length = (double) clip[ids::length];
    const auto from = map.toSource (offset), to = map.toSource (offset + length);

    auto notes = Edit::getClipNotes (clip);
    if (notes.empty())
        notes = pitch::findNotes (track, from, to);

    std::vector<pitch::Note> inside;
    for (const auto& n : notes)
        if (n.end > from && n.start < to && n.pitch > 0.0f)
            inside.push_back (n);
    return inside;
}

warp::Map nudgeWarp (const juce::ValueTree& clip, const std::vector<pitch::Note>& notes, const std::vector<double>& offsets)
{
    const auto effective = Edit::getEffectiveWarp (clip);
    const auto offset = (double) clip[ids::offset], length = (double) clip[ids::length];
    const auto from = effective.toSource (offset), to = effective.toSource (offset + length);

    // In the file's own time: the window's edges stay, note starts inside it move. A note
    // close to an edge moves less, so time never runs backwards next to the edge.
    std::vector<warp::Marker> markers { { from, from } };
    for (size_t i = 0; i < notes.size() && i < offsets.size(); ++i)
    {
        const auto s = notes[i].start;
        if (s <= from + 0.005 || s >= to - 0.005)
            continue;
        const auto room = 0.4 * juce::jmin (s - from, to - s);
        markers.push_back ({ s, s + juce::jlimit (-room, room, offsets[i]) });
    }
    markers.push_back ({ to, to });

    return warp::compose (warp::Map (std::move (markers)), Edit::getClipWarp (clip));
}

int addVoiceClip (Edit& edit, int leadClipId, int trackId, const analysis::harmony::Voice& voice)
{
    const auto lead = edit.getClip (leadClipId);
    if (! lead.isValid() || Edit::isMidiClip (lead))
        return -1;

    const auto map = nudgeWarp (lead, voice.notes, voice.offsets);
    const auto id = edit.copyClipTo (leadClipId, trackId);
    if (id < 0)
        return -1;

    auto clip = edit.getClip (id);
    auto* undo = &edit.getUndoManager();
    clip.setProperty (ids::name, (lead[ids::name].toString() + " " + analysis::harmony::getPartName (voice.part)).substring (0, 60), undo);
    // Straight through the free warp: the window is already where the lead's is.
    if (const auto text = map.toString(); text.isNotEmpty())
        clip.setProperty (ids::warp, text, undo);
    clip.setProperty (ids::stretch, warp::getAlgorithmKey (warp::Algorithm::solo), undo);
    clip.setProperty (ids::formant, true, undo);
    clip.removeProperty (ids::warpBypass, undo);
    edit.setClipNotes (id, voice.notes);
    return id;
}
}
