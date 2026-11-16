#include "MixCorrect.h"

#include "daw/Edit.h"

#include <cmath>

namespace analysis
{
namespace
{
constexpr float loudnessTolerance = 0.5f; // LU: closer than this is left as it is
constexpr float maxCorrection = 6.0f;     // dB: a bigger gap is a planning problem, not a trim

double toPower (float db) { return db <= -199.0f ? 0.0 : std::pow (10.0, db / 10.0); }
float toDb (double power) { return power > 1.0e-20 ? (float) (10.0 * std::log10 (power)) : -200.0f; }

// Power mean of the momentary loudness over the hops where `mask` is true (all active
// hops when null), with `gainDb` added.
double meanPower (const Profile& p, const std::vector<bool>* mask, float gainDb)
{
    double sum = 0.0;
    int n = 0;
    const auto& m = mask != nullptr ? *mask : p.active;
    for (size_t i = 0; i < p.momentaryLufs.size() && i < m.size(); ++i)
        if (m[i])
            sum += toPower (p.momentaryLufs[i]), ++n;
    return n > 0 ? sum / n * toPower (gainDb) : 0.0;
}

// BS.1770-style integration of momentary powers: absolute gate -70 LUFS, relative -10 LU.
float integrate (const std::vector<double>& powers)
{
    double sum = 0.0;
    int n = 0;
    for (const auto p : powers)
        if (p > toPower (-70.0f))
            sum += p, ++n;
    if (n == 0)
        return -200.0f;
    const auto gate = sum / n * toPower (-10.0f);
    sum = 0.0, n = 0;
    for (const auto p : powers)
        if (p > toPower (-70.0f) && p > gate)
            sum += p, ++n;
    return n > 0 ? toDb (sum / n) : -200.0f;
}

// Several renders summed as power (they're different parts, so mostly uncorrelated).
Profile sumProfiles (const std::vector<const Profile*>& parts)
{
    Profile sum;
    size_t frames = 0;
    for (const auto* p : parts)
        frames = juce::jmax (frames, (size_t) p->getNumFrames());

    sum.momentaryLufs.assign (frames, -200.0f);
    sum.rmsDb.assign (frames, -200.0f);
    sum.bandsDb.assign (frames, {});
    sum.active.assign (frames, false);
    sum.pitchHz.assign (frames, 0.0f);

    for (size_t i = 0; i < frames; ++i)
    {
        double momentary = 0.0, rms = 0.0;
        std::array<double, numBands> bands {};
        for (const auto* p : parts)
        {
            if (i >= (size_t) p->getNumFrames())
                continue;
            momentary += toPower (p->momentaryLufs[i]);
            rms += toPower (p->rmsDb[i]);
            for (size_t b = 0; b < (size_t) numBands; ++b)
                bands[b] += toPower (p->bandsDb[i][b]);
            sum.active[i] = sum.active[i] || p->active[i];
        }
        sum.momentaryLufs[i] = toDb (momentary);
        sum.rmsDb[i] = toDb (rms);
        for (size_t b = 0; b < (size_t) numBands; ++b)
            sum.bandsDb[i][b] = toDb (bands[b]);
    }
    return sum;
}

juce::String formatLu (float v, int decimals = 1)
{
    return (v > 0.0f ? "+" : "") + juce::String (v, decimals);
}

bool isVoice (Role r) { return r == Role::voice; }
bool isBackground (Role r) { return r == Role::backingVocal; }

const MixMove* findVolumeMove (const MixPlan& plan, int trackId)
{
    for (const auto& move : plan.moves)
        if (move.trackId == trackId && ! move.insert.has_value() && move.sendTo == 0 && move.ride.empty()
            && move.values.count ("volume") > 0)
            return &move;
    return nullptr;
}
}

//==============================================================================
MixCorrection listenAndCorrect (const juce::ValueTree& session, const MixPlan& plan, const std::map<int, Role>& roles,
                                MixStyle style, const MixOptions& options, const ApplyOptions& apply, double sampleRate,
                                ProfileCache* cache, const CorrectionProgress& progress)
{
    MixCorrection result;
    result.style = style;
    result.mode = options.mode;
    if (options.mode != MixMode::mix && options.mode != MixMode::fit)
        return result;

    const auto targets = getTargets (style);
    const auto fit = options.mode == MixMode::fit;

    // The plan as it would be written, on a copy.
    daw::Edit copy;
    copy.replaceState (session.createCopy());
    applyMix (copy, plan, apply, "Auto Mix check");

    const auto rendered = SessionAnalyser (copy, SessionAnalyser::Point::stem, true, sampleRate).run (progress, cache);
    if (rendered.cancelled || ! rendered.mix.has_value())
    {
        result.cancelled = true;
        return result;
    }

    const auto& mix = *rendered.mix;
    const auto masterVolume = (float) copy.getMaster().getProperty (daw::ids::volume, 0.0f);

    std::map<int, const Profile*> stems;
    for (const auto& [id, profile] : rendered.tracks)
        if (! (bool) copy.getTrack (id).getProperty (daw::ids::mute, false))
            stems[id] = &profile;

    const auto roleOf = [&] (int id) { const auto it = roles.find (id); return it != roles.end() ? it->second : Role::other; };

    // The lead: the voice in focus, or the voice that sings the longest.
    int lead = 0;
    if (options.focusTrackId != 0 && stems.count (options.focusTrackId) > 0 && isVoice (roleOf (options.focusTrackId)))
        lead = options.focusTrackId;
    else
    {
        double longest = 0.0;
        for (const auto& [id, profile] : stems)
            if (isVoice (roleOf (id)) && profile->getActiveSeconds() > longest)
                longest = profile->getActiveSeconds(), lead = id;
    }

    std::vector<int> backing, background;
    for (const auto& [id, profile] : stems)
    {
        const auto role = roleOf (id);
        if (isBackground (role))
            background.push_back (id);
        else if (! isVoice (role))
            backing.push_back (id);
    }

    // Moves the corrections can't go through: unticked, or on a slider at zero.
    const auto canChange = [&] (int id)
    {
        const auto* move = findVolumeMove (plan, id);
        if (move == nullptr)
            return true; // a new volume move (group fixed or vocal)
        const auto scale = apply.amount * apply.groups[(size_t) move->group];
        return apply.off.count (move->key) == 0 && scale > 0.05f;
    };

    std::map<int, float> gain; // dB change per track
    float masterGain = 0.0f;

    const auto voiceOverBacking = [&]() -> std::optional<float>
    {
        if (lead == 0 || backing.empty())
            return std::nullopt;
        const auto& voice = *stems.at (lead);
        double rest = 0.0;
        for (const auto id : backing)
            rest += meanPower (*stems.at (id), &voice.active, gain[id]);
        const auto v = meanPower (voice, nullptr, gain[lead]);
        if (v <= 0.0 || rest <= 0.0)
            return std::nullopt;
        return toDb (v) - toDb (rest);
    };

    const auto underLead = [&] (int id) -> std::optional<float>
    {
        if (lead == 0)
            return std::nullopt;
        const auto v = meanPower (*stems.at (lead), nullptr, gain[lead]);
        const auto b = meanPower (*stems.at (id), nullptr, gain[id]);
        if (v <= 0.0 || b <= 0.0)
            return std::nullopt;
        return toDb (v) - toDb (b);
    };

    // The mix with the corrections: each track's render scaled by its change, through the
    // master's volume (the stems are rendered through a bare master).
    const auto mixPowers = [&]
    {
        std::vector<double> powers ((size_t) mix.getNumFrames());
        for (size_t i = 0; i < powers.size(); ++i)
        {
            auto p = toPower (mix.momentaryLufs[i]);
            for (const auto& [id, stem] : stems)
                if (const auto g = gain[id]; g != 0.0f && i < (size_t) stem->getNumFrames())
                    p += (toPower (g) - 1.0) * toPower (stem->momentaryLufs[i]) * toPower (masterVolume);
            powers[i] = juce::jmax (0.0, p) * toPower (masterGain);
        }
        return integrate (powers);
    };

    const auto truePeak = [&]
    {
        // Peaks add up at worst for the tracks that got louder; quieter ones are left as measured.
        auto peak = std::pow (10.0, mix.truePeakDb / 20.0);
        for (const auto& [id, stem] : stems)
            if (const auto g = gain[id]; g > 0.0f && stem->peakDb > -150.0f)
                peak += (std::pow (10.0, g / 20.0) - 1.0) * std::pow (10.0, (stem->peakDb + masterVolume) / 20.0);
        return (float) (20.0 * std::log10 (juce::jmax (1.0e-10, peak))) + masterGain;
    };

    // What came out, before any correction.
    const auto vobGot = voiceOverBacking();
    std::map<int, float> underGot;
    for (const auto id : background)
        if (const auto u = underLead (id))
            underGot[id] = *u;
    const auto mixGot = mixPowers();
    const auto peakGot = mix.truePeakDb;

    std::map<juce::String, float> lastStep; // the size of each correction in the pass before
    const auto step = [&] (const juce::String& what, float wanted)
    {
        wanted = juce::jlimit (-maxCorrection, maxCorrection, wanted);
        if (const auto it = lastStep.find (what); it != lastStep.end())
            wanted = juce::jlimit (-std::abs (it->second) * 0.99f, std::abs (it->second) * 0.99f, wanted); // smaller each time
        lastStep[what] = wanted;
        return wanted;
    };

    for (int pass = 0; pass < 2; ++pass)
    {
        bool changed = false;

        if (const auto vob = voiceOverBacking(); vob.has_value() && canChange (lead))
            if (const auto error = *vob - targets.voiceOverBackingLu; std::abs (error) > loudnessTolerance)
            {
                const auto change = step ("voice", -error);
                if (std::abs (change) > 0.05f)
                    gain[lead] += change, changed = true;
            }

        for (const auto id : background)
            if (const auto under = underLead (id); under.has_value() && canChange (id))
                if (const auto error = *under - targets.backingVocalUnderLeadLu; std::abs (error) > loudnessTolerance)
                {
                    const auto change = step ("background " + juce::String (id), error);
                    if (std::abs (change) > 0.05f)
                        gain[id] += change, changed = true;
                }

        // The master: towards the loudness target, the true peak under -1 dBTP. Fit mode
        // leaves it (the backing's mix set it).
        if (! fit && mixGot > -150.0f && canChange (daw::Edit::masterId))
        {
            const auto loud = mixPowers();
            const auto peak = truePeak();
            const auto forLoudness = targets.masterLufs - loud;
            const auto forPeak = -1.0f - peak;
            const auto wanted = juce::jmin (forLoudness, forPeak);
            if (std::abs (forLoudness) > loudnessTolerance || forPeak < 0.0f)
            {
                const auto change = step ("master", wanted);
                if (std::abs (change) > 0.05f)
                    masterGain += change, changed = true;
            }
        }

        if (! changed)
            break;
        ++result.passes;
    }

    for (const auto& [id, g] : gain)
        if (std::abs (g) > 0.05f)
            result.volumeDb[id] = std::round (g * 10.0f) / 10.0f;
    if (std::abs (masterGain) > 0.05f)
        result.volumeDb[daw::Edit::masterId] = std::round (masterGain * 10.0f) / 10.0f;

    // Readings, with the rounded corrections.
    for (auto& [id, g] : gain)
        g = result.volumeDb.count (id) > 0 ? result.volumeDb[id] : 0.0f;
    masterGain = result.volumeDb.count (daw::Edit::masterId) > 0 ? result.volumeDb[daw::Edit::masterId] : 0.0f;

    if (vobGot.has_value())
        result.readings.push_back ({ "Voice over backing", "LU", targets.voiceOverBackingLu, *vobGot, voiceOverBacking().value_or (*vobGot),
                                     result.volumeDb.count (lead) > 0 });
    for (const auto& [id, got] : underGot)
        result.readings.push_back ({ "Background vocals under the lead", "LU", targets.backingVocalUnderLeadLu, got, underLead (id).value_or (got),
                                     result.volumeDb.count (id) > 0 });
    if (! fit && mixGot > -150.0f)
    {
        const auto masterChanged = result.volumeDb.count (daw::Edit::masterId) > 0;
        result.readings.push_back ({ "Master loudness", "LUFS", targets.masterLufs, mixGot, mixPowers(), masterChanged });
        result.readings.push_back ({ "True peak", "dBTP", -1.0f, peakGot, truePeak(), masterChanged });
    }

    // How much the rest still covers the lead's words, as rendered.
    if (lead != 0 && ! backing.empty())
    {
        std::vector<const Profile*> parts;
        for (const auto id : backing)
            parts.push_back (stems.at (id));
        const auto masking = measureMasking (*stems.at (lead), sumProfiles (parts));
        if (masking.overlapSeconds > 0.0)
        {
            result.maskingDb = masking.importanceWeightedDb;
            result.hasMasking = true;
        }
    }
    return result;
}

void applyCorrection (MixPlan& plan, const MixCorrection& correction, const std::map<int, Role>& roles,
                      const std::map<int, float>& currentVolumes, float currentMasterVolume, const ApplyOptions& apply)
{
    bool added = false;
    for (const auto& [id, change] : correction.volumeDb)
    {
        if (findVolumeMove (plan, id) == nullptr)
        {
            MixMove move;
            move.trackId = id;
            const auto it = currentVolumes.find (id);
            move.values = { { "volume", id == daw::Edit::masterId ? currentMasterVolume : it != currentVolumes.end() ? it->second : 0.0f } };
            plan.moves.push_back (move);
            added = true;
        }
    }
    if (added)
        labelMoves (plan, roles);

    for (auto& move : plan.moves)
    {
        if (move.insert.has_value() || move.sendTo != 0 || ! move.ride.empty() || move.values.count ("volume") == 0)
            continue;
        const auto it = correction.volumeDb.find (move.trackId);
        if (it == correction.volumeDb.end() || &move != findVolumeMove (plan, move.trackId))
            continue;

        const auto scale = juce::jlimit (0.0f, 1.0f, apply.amount) * juce::jlimit (0.0f, 1.0f, apply.groups[(size_t) move.group]);
        if (scale <= 0.05f || apply.off.count (move.key) > 0)
            continue;

        move.values["volume"] = juce::jlimit (-60.0f, 12.0f, move.values["volume"] + it->second / scale);
        move.reasons.add ("Listened to the result: " + formatLu (it->second) + " dB more"
                          + (move.trackId == daw::Edit::masterId ? juce::String (" on the master") : juce::String())
                          + ", so it lands where it was aimed");
    }

    for (const auto& r : correction.readings)
    {
        const auto format = [&] (float v) { return (r.unit == "LU" ? formatLu (v) : juce::String (v, 1)) + " " + r.unit; };
        juce::String line;
        line << "Listened to the result: " << r.what.toLowerCase() << (r.unit == "dBTP" ? " kept under " : " aimed for ")
             << format (r.aimed) << ", came out at " << format (r.got);
        if (r.corrected)
            line << ", corrected to " << format (r.after);
        plan.notes.add (line + ".");
    }
    if (correction.hasMasking)
        plan.notes.add (correction.maskingDb > 0.0f
                            ? "Listened to the result: the band still covers the words by " + juce::String (correction.maskingDb, 1)
                                  + " dB on average where they matter most."
                            : "Listened to the result: the words sit " + juce::String (-correction.maskingDb, 1)
                                  + " dB over the band on average where they matter most.");

    // The plan's predictions follow what was measured.
    for (const auto& r : correction.readings)
    {
        if (r.what == "Voice over backing")
            plan.voiceOverBackingLu = r.after;
        else if (r.what == "Master loudness")
            plan.mixLufs = r.after;
    }
}
}
