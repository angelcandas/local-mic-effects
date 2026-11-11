#include "Quantize.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace daw::rhythm
{
juce::Array<int> getClipIds (const Edit& edit, int trackId)
{
    juce::Array<int> ids;
    for (const auto& child : edit.getTrack (trackId))
        if (child.hasType (ids::CLIP))
            ids.add ((int) child[ids::id]);
    return ids;
}

juce::AudioBuffer<float> renderTrack (const Edit& edit, AudioPool& pool, int trackId, double& rate, int decimation)
{
    decimation = juce::jmax (1, decimation);
    rate = pool.getSampleRate() / decimation;

    struct Piece { AudioPool::Audio audio; juce::int64 start, offset, length; float gain; };
    std::vector<Piece> pieces;
    juce::int64 end = 0;
    const auto toSamples = [&] (const juce::var& seconds) { return (juce::int64) std::llround ((double) seconds * pool.getSampleRate()); };

    for (const auto& clip : edit.getTrack (trackId))
    {
        if (! clip.hasType (ids::CLIP) || (bool) clip.getProperty (ids::mute, false))
            continue;

        auto audio = pool.loadNow (juce::File (clip[ids::file].toString()), Edit::getWarpSettings (clip));
        if (audio == nullptr)
            continue;

        Piece piece { audio, toSamples (clip[ids::start]), juce::jmax ((juce::int64) 0, toSamples (clip[ids::offset])), 0,
                      juce::Decibels::decibelsToGain ((float) clip.getProperty (ids::gain, 0.0f)) };
        piece.length = juce::jmin (toSamples (clip[ids::length]), (juce::int64) audio->getNumSamples() - piece.offset);

        if (piece.length > 0)
        {
            end = juce::jmax (end, piece.start + piece.length);
            pieces.push_back (piece);
        }
    }

    juce::AudioBuffer<float> mix (1, (int) (end / decimation) + 1);
    mix.clear();
    auto* out = mix.getWritePointer (0);

    for (const auto& piece : pieces)
    {
        const auto channels = piece.audio->getNumChannels();
        const auto scale = piece.gain / (float) (channels * decimation);

        for (int ch = 0; ch < channels; ++ch)
        {
            const auto* in = piece.audio->getReadPointer (ch);
            for (juce::int64 i = 0; i < piece.length; ++i)
                out[(piece.start + i) / decimation] += scale * in[piece.offset + i];
        }
    }

    return mix;
}

analysis::rhythm::Tempo analyseTrack (const Edit& edit, AudioPool& pool, int trackId)
{
    double rate = 0.0;
    const auto audio = renderTrack (edit, pool, trackId, rate);
    return analysis::rhythm::detectTempo (audio, rate, edit.getBeatsPerBar());
}

double getLineUpShift (const Edit& edit, const analysis::rhythm::Tempo& tempo)
{
    if (tempo.bpm <= 0.0 || tempo.beats.empty())
        return 0.0;

    // The fitted straight grid's downbeat, so one early or late beat doesn't decide it.
    const auto beat = 60.0 / tempo.bpm;
    const auto bar = beat * edit.getBeatsPerBar();
    const auto downbeat = tempo.gridStart + tempo.firstDownbeat * beat;
    auto shift = std::round (downbeat / bar) * bar - downbeat;

    double earliest = std::numeric_limits<double>::max();
    for (const auto& track : edit.getTracks())
        for (const auto& clip : track)
            if (clip.hasType (ids::CLIP))
                earliest = juce::jmin (earliest, (double) clip[ids::start]);

    if (earliest < std::numeric_limits<double>::max())
        while (earliest + shift < -1.0e-9)
            shift += bar;

    return shift;
}

void shiftSong (Edit& edit, double seconds)
{
    if (std::abs (seconds) < 1.0e-9)
        return;

    for (const auto& track : edit.getTracks())
        for (const auto& clip : track)
            if (clip.hasType (ids::CLIP))
                edit.moveClip ((int) clip[ids::id], (double) clip[ids::start] + seconds);

    for (const auto& marker : edit.getMarkers())
        edit.setMarkerTime ((int) marker[ids::id], juce::jmax (0.0, (double) marker[ids::time] + seconds));

    const auto& project = edit.getState();
    if (project.hasProperty (ids::loopStart))
        edit.setLoop ((bool) project[ids::loopOn], juce::jmax (0.0, (double) project[ids::loopStart] + seconds),
                      juce::jmax (0.0, (double) project[ids::loopEnd] + seconds));
}

//==============================================================================
Result quantizeClips (Edit& edit, AudioPool& pool, const juce::Array<int>& clipIds, const std::vector<double>& grid,
                      const Options& options)
{
    Result result;
    if (grid.size() < 2)
        return result;

    // Pieces of one take, still in time with each other, share one warp.
    struct Group
    {
        juce::File file;
        warp::Map map;
        double timelineOffset = 0.0; // clip start minus clip offset: rendered time -> timeline
        juce::Array<int> clips;
        std::vector<std::pair<double, double>> windows; // in file time
    };

    std::map<juce::String, Group> groups;

    for (auto id : clipIds)
    {
        const auto clip = edit.getClip (id);
        if (! clip.isValid())
            continue;

        const auto map = Edit::getClipWarp (clip);
        const auto start = (double) clip[ids::start], offset = (double) clip[ids::offset], length = (double) clip[ids::length];
        const auto key = clip[ids::file].toString() + "|" + map.toString() + "|" + juce::String (start - offset, 4);

        auto& group = groups[key];
        group.file = juce::File (clip[ids::file].toString());
        group.map = map;
        group.timelineOffset = start - offset;
        group.clips.add (id);
        group.windows.emplace_back (map.toSource (offset), map.toSource (offset + length));
    }

    const auto nearestLine = [&] (double t, double& step)
    {
        auto it = std::lower_bound (grid.begin(), grid.end(), t);
        if (it == grid.end()) --it;
        if (it != grid.begin() && t - *(it - 1) < *it - t) --it;

        const auto i = (size_t) (it - grid.begin());
        step = i + 1 < grid.size() ? grid[i + 1] - grid[i] : grid[i] - grid[i - 1];
        if (i > 0) step = juce::jmin (step, grid[i] - grid[i - 1]);
        return *it;
    };

    for (auto& [key, group] : groups)
    {
        const auto source = pool.loadNow (group.file);
        if (source == nullptr)
            continue;

        const auto rate = pool.getSampleRate();
        std::sort (group.windows.begin(), group.windows.end());
        const auto from = group.windows.front().first, to = std::max_element (group.windows.begin(), group.windows.end(),
                                                                                [] (auto& a, auto& b) { return a.second < b.second; })->second;

        // The unwarped audio, slid so the first window starts where it plays now.
        const auto slide = group.map.toTarget (from) - from;
        const auto natural = [&] (double s) { return s + slide; };
        const auto inWindow = [&] (double s)
        {
            return std::any_of (group.windows.begin(), group.windows.end(), [s] (auto& w) { return s >= w.first && s < w.second; });
        };

        std::vector<warp::Marker> markers;
        const auto add = [&] (double s, double target)
        {
            if (! markers.empty())
            {
                const auto ds = s - markers.back().source, dt = target - markers.back().target;
                if (ds <= 0.0 || dt <= 0.0 || dt / ds > 2.0 || ds / dt > 2.0)
                    return false;
            }
            markers.push_back ({ s, target });
            return true;
        };

        const auto onsets = analysis::rhythm::findOnsets (*source, rate, from, to, options.sensitivity);
        std::vector<double> found;
        for (const auto& onset : onsets)
            if (inWindow (onset.time))
                found.push_back (onset.time);

        result.onsets += (int) found.size();

        // Hold the clip edges in place when no onset is near them.
        std::vector<double> anchors;
        for (const auto& [a, b] : group.windows)
            for (const auto edge : { a, b })
                if (std::none_of (found.begin(), found.end(), [edge] (double s) { return std::abs (s - edge) < 0.15; }))
                    anchors.push_back (edge);

        struct Point { double source; bool onset; };
        std::vector<Point> points;
        for (auto s : found)   points.push_back ({ s, true });
        for (auto s : anchors) points.push_back ({ s, false });
        std::sort (points.begin(), points.end(), [] (auto& a, auto& b) { return a.source < b.source; });

        for (const auto& point : points)
        {
            const auto naturalTarget = natural (point.source);

            if (point.onset)
            {
                const auto timeline = naturalTarget + group.timelineOffset;
                double step = 0.0;
                const auto line = nearestLine (timeline, step);

                if (std::abs (line - timeline) <= juce::jlimit (0.0, 0.5, options.catchRange) * step + 1.0e-9)
                {
                    const auto moved = naturalTarget + juce::jlimit (0.0, 1.0, options.strength) * (line - timeline);
                    if (add (point.source, moved))
                    {
                        result.moved += std::abs (moved - naturalTarget) > 1.0e-4 ? 1 : 0;
                        continue;
                    }
                }
            }

            add (point.source, naturalTarget);
        }

        for (auto id : group.clips)
            edit.setClipWarp (id, warp::Map (markers));
    }

    return result;
}
}
