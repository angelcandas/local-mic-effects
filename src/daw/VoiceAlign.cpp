#include "VoiceAlign.h"

#include "Quantize.h"
#include "analysis/Pitch.h"

#include <map>
#include <set>

namespace daw::voicealign
{
Result alignToGuide (Edit& edit, AudioPool& pool, const juce::Array<int>& clipIds, int guideTrackId, const Options& options)
{
    Result result;
    const auto guideTrack = edit.getTrack (guideTrackId);
    if (! guideTrack.isValid() || Edit::isMidiTrack (guideTrack))
    {
        result.error = "Pick the audio track that sings the lead.";
        return result;
    }

    // The clips to line up, by track (each track is heard as it plays now, once).
    std::map<int, juce::Array<int>> byTrack;
    for (auto id : clipIds)
    {
        const auto clip = edit.getClip (id);
        if (! clip.isValid() || Edit::isMidiClip (clip))
            continue;
        const auto track = Edit::getOwnerTrack (clip);
        if (! track.isValid() || (int) track[ids::id] == guideTrackId)
            continue;
        byTrack[(int) track[ids::id]].addIfNotAlreadyThere (id);
    }
    if (byTrack.empty())
    {
        result.error = "Pick clips on another audio track than the lead.";
        return result;
    }

    double rate = 0.0;
    const auto guide = rhythm::renderTrack (edit, pool, guideTrackId, rate);
    const auto guideFeatures = analysis::align::analyse (guide, rate);
    if (guideFeatures.size() < 10)
    {
        result.error = guideTrack[ids::name].toString() + " has no audio to follow.";
        return result;
    }
    std::shared_ptr<const analysis::pitch::Track> guidePitch;

    std::set<int> done; // clips (and their linked partners) already moved
    double shiftSum = 0.0;
    result.match = 1.0f;

    for (const auto& [trackId, clips] : byTrack)
    {
        double dubRate = 0.0;
        const auto dub = rhythm::renderTrack (edit, pool, trackId, dubRate);
        const auto dubFeatures = analysis::align::analyse (dub, dubRate);

        for (auto clipId : clips)
        {
            if (done.count (clipId) != 0)
                continue;
            const auto clip = edit.getClip (clipId);
            const auto start = (double) clip[ids::start], offset = (double) clip[ids::offset], length = (double) clip[ids::length];
            done.insert (clipId);
            for (const auto& partner : Edit::getLinkedClips (clip))
                done.insert ((int) partner[ids::id]);

            const auto found = analysis::align::align (guideFeatures, dubFeatures, options.timing, start, start + length);
            if (! found.related)
            {
                ++result.unrelated;
                continue;
            }

            ++result.aligned;
            result.match = juce::jmin (result.match, found.match);
            shiftSum += found.meanShift;
            result.largestShift = juce::jmax (result.largestShift, found.largestShift);

            // The moves, from timeline time to the clip's rendered time, after its warp now.
            auto map = Edit::getEffectiveWarp (clip);
            if (options.timingOn && ! found.markers.empty())
            {
                std::vector<warp::Marker> moves;
                for (const auto& m : found.markers)
                    moves.push_back ({ m.source - start + offset, m.target - start + offset });
                map = warp::compose (map, warp::Map (std::move (moves)));
                edit.setEffectiveWarp (clipId, map);
            }

            // Pitch: each sung note in the window to the lead's note there, in its octave.
            if (! options.matchPitch || ! Edit::getLinkedClips (clip).isEmpty())
                continue;
            const auto source = pool.loadNow (juce::File (clip[ids::file].toString()));
            if (source == nullptr)
                continue;
            if (guidePitch == nullptr)
                guidePitch = analysis::pitch::analyseCached (guide, rate);
            const auto track = analysis::pitch::analyseCached (*source, pool.getSampleRate());
            auto notes = Edit::getClipNotes (clip);
            if (notes.empty())
                notes = analysis::pitch::findNotes (*track);

            const auto semitones = Edit::getWarpSettings (clip).semitones;
            const auto from = map.toSource (offset), to = map.toSource (offset + length);
            auto changed = 0;
            for (auto& note : notes)
            {
                if (note.end <= from || note.start >= to || note.pitch <= 0.0f)
                    continue;
                const auto a = start + map.toTarget (note.start) - offset, b = start + map.toTarget (note.end) - offset;
                const auto lead = analysis::pitch::getPitch (*guidePitch, a, b);
                if (lead <= 0.0f)
                    continue;
                auto move = lead - (note.pitch + (float) semitones);
                move -= 12.0f * std::round (move / 12.0f);
                if (std::abs (move) > options.pitchRange)
                    continue;
                if (std::abs (move - note.shift) > 0.01f)
                {
                    note.shift = std::round (move * 100.0f) / 100.0f;
                    ++changed;
                }
            }
            if (changed > 0)
            {
                edit.setClipNotes (clipId, notes);
                result.notes += changed;
            }
        }
    }

    if (result.aligned == 0)
    {
        result.match = 0.0f;
        if (result.error.isEmpty())
            result.error = "These clips don't sing what " + guideTrack[ids::name].toString() + " sings, so nothing was moved.";
        return result;
    }
    result.meanShift = shiftSum / result.aligned;
    return result;
}
}
