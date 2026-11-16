#include "MixSections.h"

#include "daw/Edit.h"

#include <cmath>

namespace analysis
{
namespace
{
constexpr double minVoiceSeconds = 2.0; // a section where the voice sings less is left at the song's level
constexpr float voiceFollow = 0.75f;    // share of a section's measured difference the voice makes up
constexpr float maxVoiceDb = 4.0f;
constexpr float liftedSendDb = 2.0f, verseSendDb = -1.5f; // reverb send per section
constexpr float liftedReverbScale = 1.25f, verseReverbScale = 0.85f; // a reverb insert's mix per section
constexpr float liftedWidth = 10.0f;    // % more width in a lifted section

double toPower (float db) { return db <= -150.0f ? 0.0 : std::pow (10.0, db / 10.0); }
double toDb (double power) { return power > 1.0e-20 ? 10.0 * std::log10 (power) : -200.0; }

int toHop (double seconds) { return (int) std::lround (seconds / Profile::hopSeconds); }

// Piecewise linear value of (time, value) points: the first value before the first point,
// the last after the last.
float valueAt (const std::vector<std::pair<double, float>>& points, double time)
{
    if (points.empty())
        return 0.0f;
    if (time <= points.front().first)
        return points.front().second;
    for (size_t i = 1; i < points.size(); ++i)
        if (time <= points[i].first)
        {
            const auto& [t0, v0] = points[i - 1];
            const auto& [t1, v1] = points[i];
            return t1 - t0 > 1.0e-9 ? v0 + (float) ((time - t0) / (t1 - t0)) * (v1 - v0) : v1;
        }
    return points.back().second;
}

// A step per section (0 outside them), ramping over `ramp` seconds into each change.
std::vector<std::pair<double, float>> sectionCurve (const std::vector<MixPlan::Section>& sections, const std::vector<float>& values,
                                                    double ramp, float outside = 0.0f)
{
    std::vector<std::pair<double, float>> points;
    auto previous = outside;
    double previousEnd = 0.0;
    points.push_back ({ 0.0, outside });

    for (size_t i = 0; i < sections.size(); ++i)
    {
        const auto& s = sections[i];
        // A gap between sections goes back to the outside value.
        if (s.start - previousEnd > ramp && std::abs (previous - outside) > 1.0e-4f)
        {
            points.push_back ({ juce::jmax (0.0, previousEnd - ramp), previous });
            points.push_back ({ previousEnd, outside });
            previous = outside;
        }
        if (std::abs (values[i] - previous) > 1.0e-4f)
        {
            points.push_back ({ juce::jmax (points.back().first, s.start - ramp), previous });
            points.push_back ({ juce::jmax (points.back().first, s.start), values[i] });
        }
        previous = values[i];
        previousEnd = s.end;
    }
    if (std::abs (previous - outside) > 1.0e-4f)
    {
        points.push_back ({ juce::jmax (points.back().first, previousEnd - ramp), previous });
        points.push_back ({ previousEnd, outside });
    }

    // Drop points that change nothing.
    std::vector<std::pair<double, float>> thinned;
    for (size_t i = 0; i < points.size(); ++i)
    {
        const auto same = [] (float a, float b) { return std::abs (a - b) < 1.0e-4f; };
        if (i > 0 && i + 1 < points.size() && same (points[i - 1].second, points[i].second) && same (points[i].second, points[i + 1].second))
            continue;
        thinned.push_back (points[i]);
    }
    return thinned;
}

// a(t) + b(t) on the union of their points.
std::vector<std::pair<double, float>> addCurves (const std::vector<std::pair<double, float>>& a, const std::vector<std::pair<double, float>>& b)
{
    std::vector<double> times;
    for (const auto& p : a) times.push_back (p.first);
    for (const auto& p : b) times.push_back (p.first);
    std::sort (times.begin(), times.end());
    times.erase (std::unique (times.begin(), times.end(), [] (double x, double y) { return std::abs (x - y) < 1.0e-6; }), times.end());

    std::vector<std::pair<double, float>> sum;
    for (const auto t : times)
        sum.push_back ({ t, juce::jlimit (-daw::Edit::maxRideDb, daw::Edit::maxRideDb, valueAt (a, t) + valueAt (b, t)) });
    return sum;
}

juce::String formatDb (float db)
{
    return (db > 0.0f ? "+" : "") + juce::String (db, 1) + " dB";
}
}

float getSectionLiftDb (MixStyle style)
{
    switch (style)
    {
        case MixStyle::ballad:     return 1.5f;
        case MixStyle::electronic: return 1.5f;
        case MixStyle::pop:        return 1.0f;
        case MixStyle::rock:       return 1.0f;
        case MixStyle::rnb:        return 1.0f;
        case MixStyle::hipHop:     return 0.5f;
        case MixStyle::acoustic:   return 0.5f;
        case MixStyle::jazz:       return 0.5f;
        case MixStyle::lofi:       return 0.5f;
        case MixStyle::coaching:   return 0.0f;
        case MixStyle::speech:     return 0.0f;
    }
    return 0.0f;
}

void planBySection (MixPlan& plan, const SessionAnalyser::Result& analysis, const std::map<int, Role>& roles,
                    const std::map<int, float>& currentVolumes, MixStyle style, const MixOptions& options)
{
    // The song's sections, in order, that hold some of the song.
    std::vector<MixOptions::Section> sections;
    for (const auto& s : options.sections)
        if (s.length > 0.5)
            sections.push_back (s);
    std::sort (sections.begin(), sections.end(), [] (const auto& a, const auto& b) { return a.start < b.start; });

    if (sections.size() < 2)
    {
        plan.notes.add (sections.empty()
                            ? "No song sections yet: the whole song is mixed the same way. Find them with Song map to mix by section."
                            : "One song section only: the whole song is mixed the same way.");
        return;
    }

    const auto fit = options.mode == MixMode::fit;
    const auto roleOf = [&] (int id) { const auto it = roles.find (id); return it != roles.end() ? it->second : Role::other; };
    const auto findProfile = [&] (int id) -> const Profile*
    {
        for (const auto& [trackId, profile] : analysis.tracks)
            if (trackId == id)
                return &profile;
        return nullptr;
    };

    // Each track's volume as planned (or as it is).
    const auto volumeOf = [&] (int id)
    {
        for (const auto& move : plan.moves)
            if (move.trackId == id && ! move.insert.has_value() && move.sendTo == 0 && move.ride.empty() && move.values.count ("volume") > 0)
                return move.values.at ("volume");
        const auto it = currentVolumes.find (id);
        return it != currentVolumes.end() ? it->second : 0.0f;
    };

    // The lead: the voice in focus, or the voice that sings the longest.
    int lead = 0;
    if (roleOf (options.focusTrackId) == Role::voice && findProfile (options.focusTrackId) != nullptr)
        lead = options.focusTrackId;
    else
    {
        double longest = 0.0;
        for (const auto& [id, profile] : analysis.tracks)
            if (roleOf (id) == Role::voice && profile.getActiveSeconds() > longest)
                longest = profile.getActiveSeconds(), lead = id;
    }

    std::vector<int> band, background;
    for (const auto& [id, profile] : analysis.tracks)
    {
        const auto role = roleOf (id);
        if (role == Role::backingVocal)
            background.push_back (id);
        else if (role != Role::voice && role != Role::other)
            band.push_back (id);
    }

    // The mix as planned (levels only), for the louder sections.
    Profile estimate;
    {
        int frames = 0;
        for (const auto& [id, profile] : analysis.tracks)
            frames = juce::jmax (frames, profile.getNumFrames());
        std::vector<double> power ((size_t) frames, 0.0);
        estimate.active.assign ((size_t) frames, false);
        for (const auto& [id, profile] : analysis.tracks)
        {
            const auto gain = std::pow (10.0, volumeOf (id) / 10.0);
            for (int i = 0; i < profile.getNumFrames(); ++i)
            {
                power[(size_t) i] += gain * toPower (profile.momentaryLufs[(size_t) i]);
                estimate.active[(size_t) i] = estimate.active[(size_t) i] || profile.active[(size_t) i];
            }
        }
        estimate.momentaryLufs.resize ((size_t) frames);
        for (size_t i = 0; i < power.size(); ++i)
            estimate.momentaryLufs[i] = (float) toDb (power[i]);
        estimate.rmsDb = estimate.momentaryLufs;
    }
    const auto lifted = findLiftedParts (estimate, sections);
    const auto isLifted = [&] (const MixOptions::Section& s)
    {
        for (const auto& [from, to] : lifted)
            if (std::abs (from - s.start) < 0.5 && std::abs (to - (s.start + s.length)) < 0.5)
                return true;
        return false;
    };

    const auto lift = fit ? 0.0f : getSectionLiftDb (style);
    const auto ramp = 2.0 * 60.0 / juce::jlimit (40.0, 240.0, options.tempo); // half a bar of 4/4

    // Voice over the band (levels as planned) over the hops of [from, to) where the lead sings.
    const auto voiceOverBand = [&] (double from, double to, double& seconds) -> std::optional<double>
    {
        const auto* voice = findProfile (lead);
        seconds = 0.0;
        if (voice == nullptr || band.empty())
            return std::nullopt;
        double v = 0.0, b = 0.0;
        int count = 0;
        for (auto i = juce::jmax (0, toHop (from)); i < juce::jmin (voice->getNumFrames(), toHop (to)); ++i)
        {
            if (! voice->active[(size_t) i])
                continue;
            v += toPower (voice->momentaryLufs[(size_t) i]);
            for (const auto id : band)
                if (const auto* p = findProfile (id); p != nullptr && i < p->getNumFrames())
                    b += std::pow (10.0, volumeOf (id) / 10.0) * toPower (p->momentaryLufs[(size_t) i]);
            ++count;
        }
        seconds = count * Profile::hopSeconds;
        if (count == 0 || v <= 0.0 || b <= 0.0)
            return std::nullopt;
        return toDb (v / count) + volumeOf (lead) - toDb (b / count);
    };

    double songSeconds = 0.0;
    const auto songVoice = voiceOverBand (0.0, 1.0e9, songSeconds);

    std::vector<MixPlan::Section> summary;
    std::vector<float> voiceDb, bandDb, sendDb, reverbScale, widthAdd;
    for (const auto& s : sections)
    {
        MixPlan::Section out;
        out.name = s.name.isNotEmpty() ? s.name : s.kind;
        out.start = s.start;
        out.end = s.start + s.length;
        out.lifted = lift > 0.0f && isLifted (s);
        out.liftDb = out.lifted ? lift : 0.0f;

        double seconds = 0.0;
        const auto here = voiceOverBand (out.start, out.end, seconds);
        float voice = out.liftDb; // keep its place over a band that lifts
        if (songVoice.has_value() && here.has_value() && seconds >= minVoiceSeconds)
            voice += -voiceFollow * (float) (*here - *songVoice);
        out.voiceDb = std::round (juce::jlimit (-maxVoiceDb, maxVoiceDb, voice) * 10.0f) / 10.0f;

        const auto verse = ! out.lifted && (s.kind == "verse" || s.kind.isEmpty()) && seconds >= minVoiceSeconds;
        voiceDb.push_back (out.voiceDb);
        bandDb.push_back (out.liftDb);
        sendDb.push_back (out.lifted ? liftedSendDb : verse ? verseSendDb : 0.0f);
        reverbScale.push_back (out.lifted ? liftedReverbScale : verse ? verseReverbScale : 1.0f);
        widthAdd.push_back (out.lifted ? liftedWidth : 0.0f);
        summary.push_back (out);
    }

    const auto any = [] (const std::vector<float>& v) { return std::any_of (v.begin(), v.end(), [] (float x) { return std::abs (x) > 0.05f; }); };

    // Volume: the section curve on top of the track's ride (or a new ride).
    const auto addToRide = [&] (int id, const std::vector<float>& values, const juce::String& reason)
    {
        if (! any (values) || options.drawnRides.count (id) > 0)
            return;
        const auto curve = sectionCurve (summary, values, ramp);
        for (auto& move : plan.moves)
            if (move.trackId == id && ! move.ride.empty())
            {
                move.ride = addCurves (move.ride, curve);
                move.reasons.add (reason);
                return;
            }
        MixMove move;
        move.trackId = id;
        move.ride = curve;
        move.reasons.add (reason);
        plan.moves.push_back (move);
    };

    const auto range = [&] (const std::vector<float>& values)
    {
        const auto [lo, hi] = std::minmax_element (values.begin(), values.end());
        return formatDb (*lo) + " to " + formatDb (*hi);
    };

    if (lead != 0)
    {
        addToRide (lead, voiceDb, "By section: the voice " + range (voiceDb) + " from section to section, so it sits as clearly in each");
        if (! fit)
            for (const auto id : background)
                addToRide (id, voiceDb, "By section: follows the lead (" + range (voiceDb) + ")");
    }

    if (! fit && lift > 0.0f && any (bandDb))
    {
        for (const auto id : band)
            addToRide (id, bandDb, "By section: up " + formatDb (lift) + " in the choruses, back for the verses");

        // Space and width: a reverb insert's mix, a send to the shared reverb, a width insert.
        for (auto& move : plan.moves)
        {
            const auto role = roleOf (move.trackId);
            if (move.sendTo != 0 && move.values.count ("send") > 0 && any (sendDb))
            {
                std::vector<float> values;
                for (const auto v : sendDb)
                    values.push_back (move.values.at ("send") + v);
                auto curve = sectionCurve (summary, values, ramp, move.values.at ("send"));
                move.automation["send"] = curve;
                move.reasons.add ("By section: more reverb in the choruses, less in the verses");
            }
            else if (move.insert.has_value() && (*move.insert == fx::ModuleType::plate || *move.insert == fx::ModuleType::reverb))
            {
                const juce::String id = *move.insert == fx::ModuleType::plate ? "plateMix" : "reverbMix";
                if (move.values.count (id) == 0 || move.trackId == options.reverbReturn)
                    continue;
                const auto base = move.values.at (id);
                std::vector<float> values;
                for (const auto s : reverbScale)
                    values.push_back (juce::jlimit (0.0f, 100.0f, base * s));
                move.automation[id] = sectionCurve (summary, values, ramp, base);
                move.reasons.add ("By section: more reverb in the choruses, drier verses");
            }
            else if (move.insert == fx::ModuleType::width && move.values.count ("widthAmount") > 0 && role != Role::voice)
            {
                const auto base = move.values.at ("widthAmount");
                std::vector<float> values;
                for (const auto w : widthAdd)
                    values.push_back (juce::jlimit (0.0f, 200.0f, base + w));
                move.automation["widthAmount"] = sectionCurve (summary, values, ramp, base);
                move.reasons.add ("By section: wider in the choruses");
            }
        }
    }

    plan.sections = summary;

    int liftedCount = 0;
    for (const auto& s : summary)
        liftedCount += s.lifted ? 1 : 0;
    juce::String note = "By section: " + juce::String ((int) summary.size()) + " sections";
    if (liftedCount > 0)
        note << ", " << liftedCount << (liftedCount == 1 ? " chorus lifts " : " choruses lift ") << formatDb (lift) << " (wider, more reverb)";
    if (lead != 0 && any (voiceDb))
        note << "; the voice moves " << range (voiceDb) << " so it stays as clear in each";
    plan.notes.add (note + ".");
}
}
