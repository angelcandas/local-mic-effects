#include "MixAssistant.h"

#include "daw/Edit.h"
#include "dsp/FxModules.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace analysis
{
namespace
{
    constexpr double analysisRate = 48000.0; // the curves are compared at this rate

    double dbToPower (double db) { return std::pow (10.0, db / 10.0); }
    double powerToDb (double p)  { return p > 1.0e-20 ? 10.0 * std::log10 (p) : -200.0; }

    juce::String formatHz (double hz)
    {
        return hz >= 1000.0 ? juce::String (hz / 1000.0, hz >= 10000.0 ? 0 : 1) + " kHz" : juce::String (juce::roundToInt (hz)) + " Hz";
    }

    juce::String formatDb (double db) { return juce::String (std::abs (db), 1) + " dB"; }

    // A long-term vocal spectrum to aim for (dB per 1/3 octave, relative), by log-
    // frequency interpolation between these points: full between 200 and 500 Hz, then
    // falling about 5 dB per octave, as sung and spoken voices do on average.
    double vocalTargetDb (double hz, const MixTargets& targets)
    {
        static constexpr std::pair<double, double> points[] = {
            { 20, -40 }, { 80, -14 }, { 125, -8 }, { 160, -4 }, { 200, -1.5 }, { 250, 0 }, { 400, 0 }, { 500, -0.5 },
            { 800, -2 }, { 1000, -3.5 }, { 1600, -6.5 }, { 2000, -8 }, { 3150, -10 }, { 4000, -11 }, { 5000, -13 },
            { 6300, -15 }, { 8000, -17 }, { 10000, -20 }, { 12500, -24 }, { 16000, -30 }, { 20000, -40 }
        };

        double base = points[std::size (points) - 1].second;
        for (size_t i = 0; i + 1 < std::size (points); ++i)
        {
            const auto [f0, d0] = points[i];
            const auto [f1, d1] = points[i + 1];
            if (hz <= f1)
            {
                const auto t = juce::jlimit (0.0, 1.0, std::log (hz / f0) / std::log (f1 / f0));
                base = d0 + (d1 - d0) * t;
                break;
            }
        }

        // Style: presence rises from 2 kHz over an octave and a bit; warmth is a hump
        // around 250 Hz.
        const auto presence = juce::jlimit (0.0, 1.0, std::log2 (hz / 2000.0) / 1.3);
        const auto warmth = juce::jmax (0.0, 1.0 - std::abs (std::log2 (hz / 250.0)) / 1.3);
        return base + targets.presenceDb * presence + targets.warmthDb * warmth;
    }

    std::vector<double> bandCentres()
    {
        std::vector<double> f;
        for (int b = 0; b < numBands; ++b)
            f.push_back (getBandCentre (b));
        return f;
    }

    // How much an EQ changes the loudness of a signal with this spectrum (dB), using
    // the K-weighting so it matches what a loudness meter would show.
    double loudnessChangeDb (const Bands& spectrumDb, const std::vector<double>& eqDb)
    {
        double before = 0.0, after = 0.0;
        for (int b = 0; b < numBands; ++b)
        {
            const auto p = dbToPower (spectrumDb[(size_t) b]) * dbToPower (Analyser::getKWeightingDb (getBandCentre (b), analysisRate));
            before += p;
            after += p * dbToPower (eqDb[(size_t) b]);
        }
        return before > 0.0 ? powerToDb (after) - powerToDb (before) : 0.0;
    }

    float percentile (std::vector<float> values, double fraction)
    {
        if (values.empty())
            return -200.0f;
        const auto nth = values.begin() + (long) std::lround (juce::jlimit (0.0, 1.0, fraction) * (double) (values.size() - 1));
        std::nth_element (values.begin(), nth, values.end());
        return *nth;
    }

    // The band with the largest (or smallest) value between two frequencies.
    int findExtreme (const std::vector<double>& values, double fromHz, double toHz, bool largest)
    {
        int best = -1;
        for (int b = 0; b < numBands; ++b)
        {
            const auto f = getBandCentre (b);
            if (f < fromHz || f > toHz)
                continue;
            if (best < 0 || (largest ? values[(size_t) b] > values[(size_t) best] : values[(size_t) b] < values[(size_t) best]))
                best = b;
        }
        return best;
    }

    // Q for a bell covering the bands around `centre` that are at least half as far off.
    float bellQ (const std::vector<double>& deviation, int centre)
    {
        const auto half = deviation[(size_t) centre] / 2.0;
        const auto sameSide = [&] (int b) { return b >= 0 && b < numBands && (half > 0 ? deviation[(size_t) b] > half : deviation[(size_t) b] < half); };
        int width = 1;
        for (int b = centre - 1; sameSide (b); --b) ++width;
        for (int b = centre + 1; sameSide (b); ++b) ++width;
        const auto octaves = width / 3.0;
        return (float) juce::jlimit (0.7, 4.0, 1.4 / octaves); // about the right width for one band at Q 4
    }

    float roundTo (double v, double step) { return (float) (std::round (v / step) * step); }

    const Profile* findProfile (const SessionAnalyser::Result& analysis, int trackId)
    {
        for (const auto& [id, profile] : analysis.tracks)
            if (id == trackId)
                return &profile;
        return nullptr;
    }

    // Power-mean loudness of several sources (LUFS), as if they played together.
    double sumLoudness (const std::vector<double>& lufs)
    {
        double p = 0.0;
        for (auto l : lufs)
            if (l > -150.0)
                p += dbToPower (l);
        return p > 0.0 ? powerToDb (p) : -200.0;
    }

    // A compressor from how much a track's level swings: the momentary loudness for an
    // instrument (so single plucks and hits don't count as swings), the level of each
    // hop for a voice (phrase to phrase). Leaves `move` empty when it's steady enough.
    // Returns the change to its loudness; adds the change to its peak to `peakChange`.
    // `minSpread` is the swing it takes (dB, 10th to 90th percentile); `always` plans a
    // gentle one even under it (a bass, so the low end stays even).
    double planCompressor (MixMove& move, const Profile& profile, double eqChange, bool instrument, double& peakChange,
                           float minSpread = -1.0f, bool always = false)
    {
        std::vector<float> levels;
        const auto& source = instrument ? profile.momentaryLufs : profile.rmsDb;
        for (size_t i = 0; i < source.size() && i < profile.active.size(); ++i)
            if (profile.active[i] && source[i] > -150.0f)
                levels.push_back (source[i] + (float) eqChange);

        if (minSpread < 0.0f)
            minSpread = instrument ? 8.0f : 6.0f;

        const auto spread = percentile (levels, 0.9) - percentile (levels, 0.1);
        if (levels.empty() || (spread < minSpread && ! always))
            return 0.0;

        const auto ratio = instrument ? (spread > 12.0f ? 3.0f : 2.0f) : (spread > 14.0f ? 4.0f : spread > 10.0f ? 3.0f : 2.0f);

        // The compressor follows peaks, which sit about 3 dB over the RMS of a voice's
        // steady parts: the threshold is set there, so it acts on the louder half. An
        // instrument is only caught on its loudest third.
        std::vector<float> rms;
        for (size_t i = 0; i < profile.rmsDb.size(); ++i)
            if (profile.active[i])
                rms.push_back (profile.rmsDb[i] + (float) eqChange);

        constexpr float crest = 3.0f;
        const auto threshold = juce::jlimit (-60.0f, 0.0f, roundTo (percentile (rms, instrument ? 0.67 : 0.5) + crest, 1.0));
        move.insert = fx::ModuleType::compressor;
        move.values = { { "compOn", 1.0f }, { "compThreshold", threshold }, { "compRatio", ratio } };
        move.reasons.add ("Compressor " + juce::String (ratio, 0) + ":1 from " + juce::String (threshold, 0) + " dB: "
                          + (spread < minSpread ? "keeps it even (it swings " + formatDb (spread) + ")"
                                                : (instrument ? "its level swings " : "your level swings ") + formatDb (spread)
                                                      + (instrument ? "" : " between phrases")));

        // Its effect on loudness: each frame above the threshold comes down.
        double before = 0.0, after = 0.0;
        for (size_t i = 0; i < profile.momentaryLufs.size(); ++i)
            if (profile.active[i])
            {
                const auto level = profile.rmsDb[i] + eqChange + crest;
                const auto reduction = juce::jmax (0.0, level - threshold) * (1.0 - 1.0 / ratio);
                before += dbToPower (profile.momentaryLufs[i]);
                after += dbToPower (profile.momentaryLufs[i] - reduction);
            }

        peakChange -= juce::jmax (0.0, (double) profile.peakDb + eqChange - threshold) * (1.0 - 1.0 / ratio);
        return powerToDb (after) - powerToDb (before);
    }

    // How long a voice takes to die away by 20 dB after its notes stop (median, seconds),
    // or nothing when there are too few clear note ends to tell. A close, dry recording
    // stops within a hop; one made in a room, or with reverb already on it, rings on.
    std::optional<double> measureRingSeconds (const Profile& profile)
    {
        const auto& rms = profile.rmsDb;
        const auto n = (int) rms.size();

        std::vector<float> active;
        for (int i = 0; i < n; ++i)
            if (profile.active[(size_t) i])
                active.push_back (rms[(size_t) i]);
        const auto loud = percentile (active, 0.95);

        const auto level = [&] (int i) { return juce::jmax (-100.0f, rms[(size_t) i]); };
        std::vector<float> rings;

        for (int i = 0; i + 2 < n; ++i)
        {
            // A note end: a loud hop followed by a clear drop over the next two.
            if (level (i) < loud - 12.0f || level (i + 1) > level (i) || level (i + 2) > level (i) - 4.0f)
                continue;

            // Follow the fall until it's 20 dB down, the next note starts, or 1.5 s.
            const auto start = level (i);
            auto last = i + 1;
            auto reached = level (last) <= start - 20.0f;
            for (int k = i + 2; ! reached && k < juce::jmin (n, i + 16); ++k)
            {
                if (level (k) > level (k - 1) + 2.0f)
                    break;
                last = k;
                reached = level (k) <= start - 20.0f;
            }

            if (reached)
                rings.push_back ((float) ((last - i) * Profile::hopSeconds));
            else if (last >= i + 3) // fell slowly and got interrupted: from its rate
                rings.push_back ((float) juce::jmin (2.0, 20.0 / juce::jmax (1.0, (double) (start - level (last)) / ((last - i) * Profile::hopSeconds))));

            i = last;
        }

        if (rings.size() < 2)
            return {};
        return (double) percentile (rings, 0.5);
    }

    // The same signal `db` louder (or quieter).
    void shiftProfile (Profile& profile, float db)
    {
        const auto shift = [db] (float& v) { if (v > -150.0f) v += db; };
        shift (profile.integratedLufs);
        shift (profile.shortTermMaxLufs);
        shift (profile.peakDb);
        for (auto& v : profile.momentaryLufs) shift (v);
        for (auto& v : profile.rmsDb) shift (v);
        for (auto& bands : profile.bandsDb)
            for (auto& v : bands) shift (v);
    }

    //==========================================================================
    // Volume automation.
    using Curve = std::vector<std::pair<double, float>>; // seconds, dB

    double frameTime (size_t frame) { return ((double) frame + 0.5) * Profile::hopSeconds; }

    // A per-hop curve (dB) as few points: one wherever it has moved half a dB from the
    // last (no closer than 0.3 s), with the frame before it so holds stay flat, and both ends.
    Curve toPoints (const std::vector<float>& curve)
    {
        Curve points;
        if (curve.empty())
            return points;

        const auto add = [&points] (double t, float db)
        {
            db = roundTo (db, 0.1);
            if (points.empty() || points.back().first < t - 1.0e-6)
                points.emplace_back (t, db);
        };

        add (0.0, curve.front());
        size_t last = 0;
        for (size_t i = 1; i < curve.size(); ++i)
        {
            if (std::abs (curve[i] - curve[last]) < 0.5f || frameTime (i) - points.back().first < 0.3)
                continue;
            if (i - 1 > last)
                add (frameTime (i - 1), curve[i - 1]);
            add (frameTime (i), curve[i]);
            last = i;
        }
        add (frameTime (curve.size() - 1), curve.back());
        return points;
    }

    // The change a gain curve makes to a signal's loudness (dB), over its active frames.
    double rideLoudnessChange (const Profile& profile, const std::vector<float>& curve, const std::vector<bool>* mask = nullptr)
    {
        double before = 0.0, after = 0.0;
        for (size_t i = 0; i < profile.momentaryLufs.size() && i < curve.size(); ++i)
            if ((mask != nullptr ? (i < mask->size() && (*mask)[i]) : (bool) profile.active[i]) && profile.momentaryLufs[i] > -150.0f)
            {
                before += dbToPower (profile.momentaryLufs[i]);
                after += dbToPower (profile.momentaryLufs[i] + curve[i]);
            }
        return before > 0.0 ? powerToDb (after) - powerToDb (before) : 0.0;
    }

    // A vocal rider: evens out the level from phrase to phrase (over ~1.5 s, after the EQ
    // and compressor), up to the style's range. Leaves `move` without a ride when the
    // phrases are already even. Returns the change to the voice's loudness.
    double planVoiceRide (MixMove& move, const Profile& voice, double eqChange, const MixMove& compressor, const MixTargets& targets)
    {
        const auto n = voice.rmsDb.size();
        const auto hasComp = compressor.values.count ("compThreshold") > 0;
        const auto threshold = hasComp ? compressor.values.at ("compThreshold") : 0.0f;
        const auto ratio = hasComp ? compressor.values.at ("compRatio") : 1.0f;

        std::vector<float> level (n, -200.0f);
        for (size_t i = 0; i < n; ++i)
            if (voice.active[i] && voice.rmsDb[i] > -150.0f)
            {
                const auto l = voice.rmsDb[i] + (float) eqChange;
                level[i] = l - (hasComp ? juce::jmax (0.0f, l + 3.0f - threshold) * (1.0f - 1.0f / ratio) : 0.0f);
            }

        std::vector<float> smoothed (n, -200.0f), active;
        for (size_t i = 0; i < n; ++i)
        {
            if (level[i] <= -150.0f)
                continue;
            double p = 0.0;
            int count = 0;
            for (size_t k = i >= 7 ? i - 7 : 0; k < juce::jmin (n, i + 8); ++k)
                if (level[k] > -150.0f)
                    p += dbToPower (level[k]), ++count;
            smoothed[i] = (float) powerToDb (p / juce::jmax (1, count));
            active.push_back (smoothed[i]);
        }

        if (active.size() < 10)
            return 0.0;

        const auto target = percentile (active, 0.5);
        std::vector<float> curve (n, 0.0f);
        for (size_t i = 0; i < n; ++i)
            if (smoothed[i] > -150.0f)
                curve[i] = juce::jlimit (-targets.rideRangeDb, targets.rideRangeDb, 0.8f * (target - smoothed[i]));

        // Between phrases it holds, then sets itself for the next phrase before it starts.
        std::optional<float> next;
        for (size_t i = n; i-- > 0;)
        {
            if (smoothed[i] > -150.0f) next = curve[i];
            else if (next.has_value()) curve[i] = *next;
        }
        // After the last phrase it stays where it was.
        for (size_t i = n; i-- > 0;)
            if (smoothed[i] > -150.0f)
            {
                for (auto k = i + 1; k < n; ++k)
                    curve[k] = curve[i];
                break;
            }

        const auto most = std::accumulate (curve.begin(), curve.end(), 0.0f, [] (float m, float v) { return juce::jmax (m, std::abs (v)); });
        if (most < 1.0f)
            return 0.0;

        std::vector<float> after;
        for (size_t i = 0; i < n; ++i)
            if (smoothed[i] > -150.0f)
                after.push_back (smoothed[i] + curve[i]);

        move.ride = toPoints (curve);
        move.reasons.add ("Volume ride, up to " + formatDb (most) + " up or down: evens out your phrases ("
                          + formatDb (percentile (active, 0.9) - percentile (active, 0.1)) + " apart before, "
                          + formatDb (percentile (after, 0.9) - percentile (after, 0.1)) + " after), "
                          + juce::String ((int) move.ride.size()) + " points you can see on the track");
        return rideLoudnessChange (voice, curve);
    }

    // A dip on an instrument while the lead sings (with 0.3 s to get there), back up in
    // the gaps of a second or more between lines, so the band fills them. Nothing when
    // the instrument doesn't play in any gap (the dip would be a fader move).
    // `whileSinging` gets the change while the lead sings; returns the overall change.
    double planDuck (MixMove& move, const Profile& track, const Profile& lead, const MixTargets& targets, double& whileSinging)
    {
        whileSinging = 0.0;
        const auto n = juce::jmin (track.active.size(), lead.active.size());
        if (targets.duckDb <= 0.0f || n == 0)
            return 0.0;

        // Singing, with a short hangover; gaps under a second count as singing.
        std::vector<bool> singing (n, false);
        for (size_t i = 0; i < n; ++i)
            for (size_t k = i >= 2 ? i - 2 : 0; k < juce::jmin (n, i + 3) && ! singing[i]; ++k)
                singing[i] = lead.active[k];

        int gaps = 0;
        for (size_t i = 0; i < n;)
        {
            if (singing[i]) { ++i; continue; }
            auto end = i;
            while (end < n && ! singing[end]) ++end;
            const auto between = i > 0 && end < n;
            if (between && end - i < 10)
                for (auto k = i; k < end; ++k) singing[k] = true;
            else
            {
                bool plays = false;
                for (auto k = i; k < end && ! plays; ++k) plays = track.active[k];
                gaps += plays ? 1 : 0;
            }
            i = end;
        }

        if (gaps == 0)
            return 0.0;

        std::vector<float> curve (n, 0.0f);
        Curve points;
        const auto dip = -targets.duckDb;
        points.emplace_back (0.0, singing[0] ? dip : 0.0f);
        for (size_t i = 1; i < n; ++i)
        {
            curve[i] = singing[i] ? dip : 0.0f;
            if (singing[i] == singing[i - 1])
                continue;
            const auto t = i * Profile::hopSeconds;
            if (singing[i]) // into a line: down by the time it starts
            {
                points.emplace_back (juce::jmax (points.back().first + 0.01, t - 0.3), 0.0f);
                points.emplace_back (juce::jmax (points.back().first + 0.01, t), dip);
            }
            else // out of it: back up over 0.4 s
            {
                points.emplace_back (juce::jmax (points.back().first + 0.01, t), dip);
                points.emplace_back (t + 0.4, 0.0f);
            }
        }
        curve[0] = singing[0] ? dip : 0.0f;

        move.ride = points;
        move.reasons.add ("Volume ride: down " + formatDb (targets.duckDb) + " while you sing, back up in "
                          + juce::String (gaps) + (gaps == 1 ? " gap" : " gaps") + " between lines: room for the voice, the full band in between");
        whileSinging = dip;
        return rideLoudnessChange (track, curve);
    }

    bool hasAny (const MixOptions& options, int trackId, std::initializer_list<fx::ModuleType> types)
    {
        const auto it = options.existingInserts.find (trackId);
        if (it == options.existingInserts.end())
            return false;
        for (auto type : types)
            if (std::find (it->second.begin(), it->second.end(), type) != it->second.end())
                return true;
        return false;
    }

    //==========================================================================
    struct VoicePlan
    {
        MixMove eq, compressor, deEsser, saturation, exciter, echo, reverb, width, ride;
        double eqChange = 0.0;       // what the EQ does to its loudness
        double loudnessChange = 0.0; // what the EQ and compressor do to its loudness
        double peakChange = 0.0;     // roughly, to the peak
    };

    VoicePlan planVoice (int trackId, const Profile& voice, const MixTargets& targets, const MixOptions& options, juce::StringArray& notes,
                         const juce::String& name, bool background = false)
    {
        VoicePlan plan;
        for (auto* move : { &plan.eq, &plan.compressor, &plan.deEsser, &plan.saturation, &plan.exciter, &plan.echo, &plan.reverb, &plan.width })
            move->trackId = trackId;
        plan.eq.insert = fx::ModuleType::paraEq;
        plan.compressor.insert = fx::ModuleType::compressor;
        plan.deEsser.insert = fx::ModuleType::deEsser;

        const auto spectrum = voice.getAverageSpectrum();
        const auto loudest = *std::max_element (spectrum.begin(), spectrum.end());

        // How far each band is from the target, after matching the overall level over
        // 200 Hz - 5 kHz. Bands with nothing in them don't count.
        std::vector<double> deviation ((size_t) numBands, 0.0);
        {
            double offset = 0.0;
            int count = 0;
            for (int b = 0; b < numBands; ++b)
            {
                deviation[(size_t) b] = spectrum[(size_t) b] - vocalTargetDb (getBandCentre (b), targets);
                if (getBandCentre (b) >= 200.0 && getBandCentre (b) <= 5000.0 && spectrum[(size_t) b] > loudest - 50.0f)
                {
                    offset += deviation[(size_t) b];
                    ++count;
                }
            }
            offset = count > 0 ? offset / count : 0.0;

            auto raw = deviation;
            for (int b = 0; b < numBands; ++b)
            {
                raw[(size_t) b] = spectrum[(size_t) b] > loudest - 50.0f ? deviation[(size_t) b] - offset : 0.0;
            }
            // Light smoothing, so one band (one strong harmonic) doesn't decide alone.
            for (int b = 0; b < numBands; ++b)
            {
                const auto left = raw[(size_t) juce::jmax (0, b - 1)], right = raw[(size_t) juce::jmin (numBands - 1, b + 1)];
                deviation[(size_t) b] = 0.25 * left + 0.5 * raw[(size_t) b] + 0.25 * right;
            }
        }

        fx::ParametricEq::Settings eq;
        auto& reasons = plan.eq.reasons;

        // Low cut under the lowest notes.
        const auto lowestNote = voice.getPitchedFraction() > 0.3 ? voice.getPitchPercentile (0.05) : 0.0f;
        eq.lowCutOn = true;
        // Background vocals are cut higher: their low end only muddies the lead's.
        eq.lowCutHz = background ? roundTo (lowestNote > 0.0f ? juce::jlimit (100.0f, 220.0f, lowestNote * 0.9f) : 150.0f, 1.0)
                                 : roundTo (lowestNote > 0.0f ? juce::jlimit (50.0f, 160.0f, lowestNote * 0.75f) : 80.0f, 1.0);
        reasons.add ("Low cut at " + formatHz (eq.lowCutHz)
                     + (background ? ": background vocals leave the low end to the lead"
                        : lowestNote > 0.0f ? ", under your lowest note (" + formatHz (lowestNote) + ")" : ": rumble and handling noise"));

        // Boom: a low shelf when 80-160 Hz is well above the target.
        {
            double sum = 0.0;
            int count = 0;
            for (int b = 0; b < numBands; ++b)
                if (getBandCentre (b) >= juce::jmax (80.0, (double) eq.lowCutHz) && getBandCentre (b) <= 160.0)
                    sum += deviation[(size_t) b], ++count;

            const auto excess = count > 0 ? sum / count : 0.0;
            if (excess > 3.0)
            {
                eq.lowShelfHz = 150.0f;
                eq.lowShelfDb = -roundTo (juce::jmin (6.0, 0.7 * excess), 0.5);
                reasons.add ("Low shelf " + formatDb (eq.lowShelfDb) + " down at 150 Hz: less boom");
            }
        }

        // Bell 1: mud (up to 400 Hz) or boxiness (to 1 kHz).
        if (const auto b = findExtreme (deviation, juce::jmax (160.0, eq.lowCutHz * 1.5), 1000.0, true); b >= 0 && deviation[(size_t) b] > 2.0)
        {
            const auto hz = getBandCentre (b);
            eq.bells[0] = { (float) hz, -roundTo (juce::jmin (6.0, 0.8 * deviation[(size_t) b]), 0.5), bellQ (deviation, b) };
            reasons.add ("Cut " + formatDb (eq.bells[0].db) + " at " + formatHz (hz) + (hz <= 400.0 ? ": muddy" : ": boxy"));
        }

        // Bell 2: harshness at 2-6.3 kHz, or bell 3: presence when that area is weak.
        if (const auto b = findExtreme (deviation, 2000.0, 6300.0, true); b >= 0 && deviation[(size_t) b] > 2.5)
        {
            const auto hz = getBandCentre (b);
            eq.bells[1] = { (float) hz, -roundTo (juce::jmin (6.0, 0.8 * deviation[(size_t) b]), 0.5), bellQ (deviation, b) };
            reasons.add ("Cut " + formatDb (eq.bells[1].db) + " at " + formatHz (hz) + ": harsh");
        }
        else if (const auto w = findExtreme (deviation, 2000.0, 5000.0, false); w >= 0 && deviation[(size_t) w] < -2.5)
        {
            const auto hz = getBandCentre (w);
            eq.bells[2] = { (float) hz, roundTo (juce::jmin (4.0, -0.7 * deviation[(size_t) w]), 0.5), 0.9f };
            reasons.add ("Lift " + formatDb (eq.bells[2].db) + " at " + formatHz (hz) + ": more presence");
        }

        // Air: a high shelf when 10-16 kHz is well under the target.
        {
            double sum = 0.0;
            int count = 0;
            for (int b = 0; b < numBands; ++b)
                if (getBandCentre (b) >= 10000.0 && getBandCentre (b) <= 16000.0)
                    sum += deviation[(size_t) b], ++count;

            const auto lack = count > 0 ? -sum / count : 0.0;
            if (lack > 4.0)
            {
                eq.highShelfHz = 10000.0f;
                eq.highShelfDb = roundTo (juce::jmin (3.0, 0.5 * lack), 0.5);
                reasons.add ("High shelf " + formatDb (eq.highShelfDb) + " up at 10 kHz: air");
            }
        }

        plan.eq.values = {
            { "peqOn", 1.0f }, { "peqLowCutOn", eq.lowCutOn ? 1.0f : 0.0f }, { "peqLowCutFreq", eq.lowCutHz },
            { "peqLowFreq", eq.lowShelfHz }, { "peqLowGain", eq.lowShelfDb },
            { "peqHighFreq", eq.highShelfHz }, { "peqHighGain", eq.highShelfDb },
            { "peqHighCutOn", eq.highCutOn ? 1.0f : 0.0f }, { "peqHighCutFreq", eq.highCutHz },
        };
        for (int b = 0; b < fx::ParametricEq::numBells; ++b)
        {
            const auto prefix = "peqBell" + juce::String (b + 1);
            plan.eq.values[prefix + "Freq"] = roundTo (eq.bells[(size_t) b].hz, 1.0);
            plan.eq.values[prefix + "Gain"] = eq.bells[(size_t) b].db;
            plan.eq.values[prefix + "Q"] = roundTo (eq.bells[(size_t) b].q, 0.01);
        }

        if (! options.eq)
        {
            eq = {};
            plan.eq = {};
        }

        const auto eqDb = fx::ParametricEq::getMagnitudeDb (eq, bandCentres(), analysisRate);
        const auto eqChange = loudnessChangeDb (spectrum, eqDb);

        // De-esser when 5-10 kHz stands out.
        if (options.deEsser)
        {
            double sum = 0.0;
            int count = 0;
            for (int b = 0; b < numBands; ++b)
                if (getBandCentre (b) >= 5000.0 && getBandCentre (b) <= 10000.0)
                    sum += deviation[(size_t) b] + eqDb[(size_t) b], ++count;

            // Stacked S's add up, so background vocals get one sooner.
            const auto excess = count > 0 ? sum / count : 0.0;
            if (excess > (background ? 1.5 : 3.0))
            {
                const auto band = findExtreme (deviation, 5000.0, 10000.0, true);
                std::vector<float> levels;
                for (size_t i = 0; i < voice.bandsDb.size(); ++i)
                    if (voice.active[i])
                    {
                        double p = 0.0;
                        for (int b = 0; b < numBands; ++b)
                            if (getBandCentre (b) >= 5000.0 && getBandCentre (b) <= 10000.0)
                                p += dbToPower (voice.bandsDb[i][(size_t) b]);
                        levels.push_back ((float) powerToDb (p));
                    }

                const auto threshold = juce::jlimit (-60.0f, 0.0f, roundTo (percentile (levels, background ? 0.6 : 0.75), 1.0));
                plan.deEsser.values = { { "deessOn", 1.0f }, { "deessThreshold", threshold },
                                        { "deessFreq", roundTo (juce::jlimit (3000.0, 10000.0, getBandCentre (band)), 10.0) } };
                plan.deEsser.reasons.add ("De-esser from " + juce::String (threshold, 0) + " dB at " + formatHz (getBandCentre (band))
                                          + ": the S's stand out by " + formatDb (excess));
            }
        }

        // Compressor from how much the level swings between phrases.
        double compChange = 0.0;
        if (options.compressor)
        {
            plan.compressor.trackId = trackId;
            compChange = planCompressor (plan.compressor, voice, eqChange, false, plan.peakChange);
        }

        // Character the style asks for: saturation (warmth or grit, mixed in), and air from
        // an exciter, unless the S's already stick out.
        if (options.saturation && targets.voiceSaturationDb > 0.0f)
        {
            plan.saturation.insert = fx::ModuleType::saturation;
            plan.saturation.values = { { "satOn", 1.0f }, { "satDrive", targets.voiceSaturationDb }, { "satMix", 35.0f } };
            plan.saturation.reasons.add ("Saturation " + juce::String (targets.voiceSaturationDb, 0) + " dB drive, mixed in at 35%: "
                                         + (targets.voiceSaturationDb >= 6.0f ? "grit" : "warmth and body") + " for " + getStyleName (targets.style));
        }

        if (options.exciter && targets.exciterAmount > 0.0f && ! background)
        {
            if (! plan.deEsser.values.empty())
                notes.add (name + " is already sibilant: no exciter added.");
            else
            {
                plan.exciter.insert = fx::ModuleType::exciter;
                plan.exciter.values = { { "exciterOn", 1.0f }, { "exciterFreq", 4000.0f }, { "exciterAmount", targets.exciterAmount } };
                plan.exciter.reasons.add ("Exciter " + juce::String (targets.exciterAmount, 0) + "% from 4 kHz: air and detail ("
                                          + getStyleName (targets.style) + ")");
            }
        }

        // A tempo-synced echo on the lead, at the end of the chain.
        const auto ownSpace = hasAny (options, trackId, { fx::ModuleType::reverb, fx::ModuleType::echo });
        if (options.echo && targets.echoMix > 0.0f && ! background && ! ownSpace)
        {
            const auto beat = 60000.0 / juce::jlimit (40.0, 240.0, options.tempo);
            const auto ms = roundTo (juce::jlimit (20.0, 1500.0, beat * targets.echoBeats), 1.0);
            const auto note = juce::approximatelyEqual (targets.echoBeats, 0.5f) ? "an eighth note"
                            : juce::approximatelyEqual (targets.echoBeats, 0.75f) ? "a dotted eighth"
                            : juce::approximatelyEqual (targets.echoBeats, 1.0f) ? "a quarter note" : "a beat fraction";
            plan.echo.insert = fx::ModuleType::echo;
            plan.echo.atEnd = true;
            plan.echo.values = { { "delayOn", 1.0f }, { "delayTime", ms }, { "delayFeedback", targets.echoFeedback },
                                 { "delayMix", targets.echoMix }, { "delayPingPong", 1.0f } };
            plan.echo.reasons.add ("Echo " + juce::String (targets.echoMix, 0) + "%, " + juce::String (ms, 0) + " ms (" + note + " at "
                                   + juce::String (options.tempo, 0) + " BPM), ping-pong: depth that moves with the song");
        }

        // Background vocals spread wider than the lead, which stays in the middle.
        if (background && options.width && targets.widthAmount > 100.0f)
        {
            plan.width.insert = fx::ModuleType::width;
            plan.width.atEnd = true;
            plan.width.values = { { "widthOn", 1.0f }, { "widthAmount", targets.widthAmount } };
            plan.width.reasons.add ("Stereo width " + juce::String (targets.widthAmount, 0) + "%: spreads them around the lead");
        }

        // Reverb at the end of the chain: some space, as much as the style asks for, but
        // only on a dry recording that doesn't have a reverb or echo of its own.
        if (options.reverb && targets.reverbMix > 0.0f)
        {
            const auto ring = measureRingSeconds (voice);

            if (hasAny (options, trackId, { fx::ModuleType::reverb, fx::ModuleType::echo }))
                notes.add (name + " already has its own reverb or echo: no reverb added.");
            else if (ring.has_value() && *ring > 0.4)
                notes.add (name + " already sounds roomy (it rings on " + juce::String (*ring, 1) + " s after each note): no reverb added.");
            else
            {
                // Background vocals sit further back: more reverb, a bigger room.
                const auto mix = background ? roundTo (juce::jmin (40.0f, targets.reverbMix * 1.5f), 1.0) : targets.reverbMix;
                const auto size = background ? juce::jmin (90.0f, targets.reverbSize + 10.0f) : targets.reverbSize;
                plan.reverb.insert = fx::ModuleType::reverb;
                plan.reverb.atEnd = true;
                plan.reverb.values = { { "reverbOn", 1.0f }, { "reverbMix", mix }, { "reverbSize", size },
                                       { "reverbDamping", 55.0f }, { "reverbWidth", 100.0f } };
                plan.reverb.reasons.add ("Reverb " + juce::String (mix, 0) + "%, "
                                         + (size >= 60.0f ? "large" : size >= 45.0f ? "medium" : "small")
                                         + (background ? " room: puts the background vocals behind the lead"
                                                       : " room: the recording is dry and gives the voice some space"));
            }
        }

        plan.eqChange = eqChange;
        plan.loudnessChange = eqChange + compChange;
        plan.peakChange += eqChange + juce::jmax (0.0, *std::max_element (eqDb.begin(), eqDb.end()) - eqChange);
        return plan;
    }
}

//==============================================================================
MixTargets getTargets (MixStyle style)
{
    MixTargets t { 1.5f, 0.0f, 0.0f };

    switch (style)
    {
        case MixStyle::pop:
            t = { 1.5f, 2.0f, 0.0f, -14.0f, 12.0f, 45.0f, 6.0f };
            t.voiceSaturationDb = 3.0f; t.exciterAmount = 20.0f; t.echoMix = 10.0f; t.echoBeats = 0.5f; t.widthAmount = 115.0f;
            break;
        case MixStyle::ballad:
            t = { 2.5f, 0.0f, 1.5f, -14.0f, 20.0f, 65.0f, 7.0f };
            t.exciterAmount = 10.0f; t.echoMix = 12.0f; t.echoBeats = 1.0f; t.echoFeedback = 30.0f; t.widthAmount = 110.0f;
            t.rideRangeDb = 5.0f; t.duckDb = 2.0f;
            break;
        case MixStyle::rock:
            t = { 0.0f, 1.0f, -1.0f, -14.0f, 9.0f, 35.0f, 5.0f };
            t.voiceSaturationDb = 6.0f; t.bassSaturationDb = 6.0f; t.widthAmount = 120.0f; t.duckDb = 1.0f;
            t.drumsLufs = -19.0f; t.guitarLufs = -21.0f;
            break;
        case MixStyle::coaching: // the voice up front and dry, so every detail of it can be heard
            t = { 4.0f, 1.5f, 0.0f, -14.0f, 0.0f, 50.0f, 9.0f };
            t.rideRangeDb = 3.0f; t.duckDb = 3.0f;
            break;
        case MixStyle::hipHop:
            t = { 2.5f, 2.5f, 0.5f, -13.0f, 6.0f, 30.0f, 6.0f };
            t.voiceSaturationDb = 4.0f; t.bassSaturationDb = 4.0f; t.exciterAmount = 25.0f;
            t.echoMix = 8.0f; t.echoBeats = 0.5f; t.echoFeedback = 20.0f; t.widthAmount = 110.0f;
            t.drumsLufs = -19.0f; t.bassLufs = -19.5f; t.duckDb = 2.0f;
            break;
        case MixStyle::rnb:
            t = { 2.0f, 1.5f, 1.5f, -14.0f, 16.0f, 55.0f, 6.0f };
            t.voiceSaturationDb = 2.0f; t.exciterAmount = 20.0f; t.echoMix = 12.0f; t.echoBeats = 0.75f; t.echoFeedback = 30.0f;
            t.widthAmount = 120.0f; t.bassLufs = -20.0f;
            break;
        case MixStyle::acoustic:
            t = { 2.0f, 0.5f, 1.0f, -16.0f, 14.0f, 50.0f, 7.0f };
            t.widthAmount = 110.0f; t.guitarLufs = -20.0f; t.pianoLufs = -21.0f; t.drumsLufs = -23.0f;
            break;
        case MixStyle::electronic:
            t = { 1.0f, 2.5f, -0.5f, -12.0f, 14.0f, 60.0f, 5.0f };
            t.voiceSaturationDb = 3.0f; t.bassSaturationDb = 3.0f; t.exciterAmount = 30.0f;
            t.echoMix = 15.0f; t.echoBeats = 0.75f; t.echoFeedback = 35.0f; t.widthAmount = 140.0f;
            t.drumsLufs = -19.0f; t.bassLufs = -20.0f; t.duckDb = 2.0f;
            break;
        case MixStyle::jazz:
            t = { 1.0f, 0.0f, 1.0f, -18.0f, 18.0f, 55.0f, 8.0f };
            t.voiceSaturationDb = 2.0f; t.pianoLufs = -21.0f; t.drumsLufs = -23.0f; t.rideRangeDb = 3.0f; t.duckDb = 1.0f;
            break;
        case MixStyle::lofi:
            t = { 0.5f, -2.0f, 2.0f, -14.0f, 15.0f, 45.0f, 6.0f };
            t.voiceSaturationDb = 8.0f; t.bassSaturationDb = 5.0f; t.echoMix = 10.0f; t.echoBeats = 0.5f; t.echoFeedback = 30.0f;
            t.widthAmount = 90.0f; t.rideRangeDb = 3.0f; t.duckDb = 1.0f;
            break;
    }
    t.style = style;
    return t;
}

juce::String getStyleName (MixStyle style)
{
    switch (style)
    {
        case MixStyle::pop:        return "Pop";
        case MixStyle::ballad:     return "Ballad";
        case MixStyle::rock:       return "Rock";
        case MixStyle::coaching:   return "Coaching";
        case MixStyle::hipHop:     return "Hip-hop";
        case MixStyle::rnb:        return "R&B";
        case MixStyle::acoustic:   return "Acoustic";
        case MixStyle::electronic: return "Electronic";
        case MixStyle::jazz:       return "Jazz";
        case MixStyle::lofi:       return "Lo-fi";
    }
    return {};
}

const char* getRoleKey (Role role)
{
    switch (role)
    {
        case Role::voice:        return "voice";
        case Role::backingVocal: return "background";
        case Role::backing:      return "instrumental";
        case Role::guitar:       return "guitar";
        case Role::piano:        return "piano";
        case Role::bass:         return "bass";
        case Role::drums:        return "drums";
        case Role::instrument:   return "instrument";
        case Role::other:        return "other";
    }
    return "other";
}

Role getRoleFromKey (const juce::String& key)
{
    if (key == "backing")
        return Role::backing; // the key before background vocals had one of their own
    for (auto role : allRoles)
        if (key == getRoleKey (role))
            return role;
    return Role::other;
}

juce::String getRoleName (Role role)
{
    switch (role)
    {
        case Role::voice:        return "Voice";
        case Role::backingVocal: return "Background vocals";
        case Role::backing:      return "Instrumental";
        case Role::guitar:       return "Guitar";
        case Role::piano:        return "Piano / keys";
        case Role::bass:         return "Bass";
        case Role::drums:        return "Drums";
        case Role::instrument:   return "Other instrument";
        case Role::other:        return "Leave as is";
    }
    return {};
}

bool isInstrument (Role role) noexcept
{
    return role == Role::guitar || role == Role::piano || role == Role::bass || role == Role::drums || role == Role::instrument;
}

std::optional<Role> guessRoleFromName (const juce::String& trackName)
{
    // Whole words where a short one would match inside others ("bgv", "keys"), parts of
    // words elsewhere. Background vocals before voice, so "Backing vocals" isn't the lead.
    const auto name = trackName.toLowerCase().replaceCharacters ("_-.()[]", "       ");
    juce::StringArray words;
    words.addTokens (name, " ", "");
    words.removeEmptyStrings();

    const auto hasWord = [&] (std::initializer_list<const char*> list)
    {
        for (auto word : list)
            if (words.contains (word))
                return true;
        return false;
    };
    const auto hasPart = [&] (std::initializer_list<const char*> list)
    {
        for (auto part : list)
            if (name.contains (part))
                return true;
        return false;
    };

    const auto vocal = hasPart ({ "vocal", "voice", "vox", "voz", "sing", "cant" }) || hasWord ({ "vo", "vx" });
    if (hasPart ({ "harmon", "choir", "background", "bgv", "bvs", "bckvoc", "segunda", "coro", "adlib", "ad lib" })
        || hasWord ({ "bv", "bvox", "bg", "backs" })
        || (vocal && hasPart ({ "back", "bkg", "fondo", "dbl", "double", "chorus" })))
        return Role::backingVocal;
    if (vocal || ((hasPart ({ "lead", "take", "mic" }) || hasWord ({ "rap", "mc" })) && ! hasPart ({ "synth", "guitar", "gtr" })))
        return Role::voice;

    // A stem from the splitter is called "<song> - Drums", "<song> - Other"...
    if (hasPart ({ "drum", "bateria", "batería", "kick", "snare", "hihat", "perc", "overhead", "cymbal", "808", "bombo", "caja" })
        || hasWord ({ "kd", "sn", "oh", "hh", "hat", "hats", "tom", "toms", "loop" }))
        return Role::drums;
    if (hasPart ({ "bass", "bajo" }) || hasWord ({ "sub" }))
        return Role::bass;
    if (hasPart ({ "guitar", "gtr", "guitarra", "acoustic", "strat", "uke", "ukulele", "banjo", "mandolin", "requinto" }) || hasWord ({ "gt" }))
        return Role::guitar;
    if (hasPart ({ "piano", "keys", "keyboard", "rhodes", "wurli", "organ", "organo", "órgano", "teclado", "epiano", "clav" }) || hasWord ({ "ep", "key", "pno" }))
        return Role::piano;
    if (hasWord ({ "other", "otros", "others" }))
        return Role::instrument; // the splitter's "everything else" stem
    if (hasPart ({ "backing", "music", "beat", "instrumental", "karaoke", "pista", "song", "mix", "band", "playback" }))
        return Role::backing;
    if (hasPart ({ "synth", "string", "pad", "violin", "cello", "viola", "sax", "trumpet", "brass", "horn", "flute", "flauta", "lead synth",
                   "arp", "pluck", "fx", "accordion", "acordeon", "harp", "marimba", "bells", "trombon", "orchestra", "other", "otros" }))
        return Role::instrument;
    return {};
}

Role guessRole (const juce::String& trackName, const Profile& profile, double songSeconds)
{
    const auto byName = guessRoleFromName (trackName);
    if (byName == Role::voice || byName == Role::backingVocal || byName == Role::backing)
        return *byName;

    const auto active = profile.getActiveSeconds();
    if (active <= 0.0)
        return Role::other;
    if (byName.has_value())
        return *byName;

    // Drums are short unpitched hits (the level jumps from hop to hop); a bass has nearly
    // all of its energy under 250 Hz and next to nothing up top.
    const auto spectrum = profile.getAverageSpectrum();
    double total = 0.0, low = 0.0, high = 0.0;
    for (int b = 0; b < numBands; ++b)
    {
        const auto p = dbToPower (spectrum[(size_t) b]);
        total += p;
        if (getBandCentre (b) <= 250.0) low += p;
        if (getBandCentre (b) >= 5000.0) high += p;
    }

    double jumps = 0.0;
    int count = 0;
    for (size_t i = 1; i < profile.rmsDb.size(); ++i)
        if (profile.active[i] && profile.active[i - 1])
            jumps += std::abs (juce::jmax (-80.0f, profile.rmsDb[i]) - juce::jmax (-80.0f, profile.rmsDb[i - 1])), ++count;
    const auto meanJump = count > 0 ? jumps / count : 0.0;

    const auto pitched = profile.getPitchedFraction();
    if (pitched < 0.25 && meanJump > 4.0)
        return Role::drums;
    if (total > 0.0 && low / total > 0.85 && high / total < 0.01)
        return Role::bass;
    if (pitched > 0.6)
        return Role::voice;
    return active > 0.5 * songSeconds ? Role::backing : Role::instrument;
}

//==============================================================================
MixPlan planMix (const SessionAnalyser::Result& measured, const std::map<int, Role>& roles,
                 const std::map<int, float>& currentVolumes, float currentMasterVolume, MixStyle style,
                 const MixOptions& options)
{
    MixPlan plan;
    const auto targets = getTargets (style);

    const auto volumeOf = [&] (int id) { auto it = currentVolumes.find (id); return it != currentVolumes.end() ? it->second : 0.0f; };
    const auto roleOf = [&] (int id) { auto it = roles.find (id); return it != roles.end() ? it->second : Role::other; };

    // Normalize: each track to its role's level, with its chain's input gain. The rest
    // is planned on the measurements as they will be after it.
    SessionAnalyser::Result normalized;
    if (options.normalize)
    {
        normalized = measured;
        for (auto& [id, profile] : normalized.tracks)
        {
            const auto role = roleOf (id);
            if (role == Role::other || profile.getActiveSeconds() <= 0.0 || profile.integratedLufs <= -150.0f)
                continue;

            const auto level = role == Role::voice || role == Role::backingVocal ? targets.voiceLufs
                             : role == Role::backing ? targets.instrumentalLufs
                             : role == Role::drums   ? targets.drumsLufs
                             : role == Role::bass    ? targets.bassLufs
                             : role == Role::guitar  ? targets.guitarLufs
                             : role == Role::piano   ? targets.pianoLufs
                                                     : targets.otherInstrumentLufs;
            const auto forLevel = level - profile.integratedLufs;
            const auto forPeak = -1.0f - profile.peakDb;
            const auto trim = roundTo (juce::jlimit (-24.0f, 24.0f, juce::jmin (forLevel, forPeak)), 0.5);
            if (std::abs (trim) < 0.5f)
                continue;

            shiftProfile (profile, trim);

            MixMove move;
            move.trackId = id;
            move.values = { { "inGain", trim } };
            move.reasons.add ("Input gain " + juce::String (trim > 0 ? "up " : "down ") + formatDb (trim) + ": normalized to "
                              + juce::String (level, 0) + " LUFS, the level for " + getRoleName (role).toLowerCase() + " in "
                              + getStyleName (style) + (forPeak < forLevel ? " (held back so its peaks stay under -1 dB)" : ""));
            plan.moves.push_back (move);
        }
    }
    const auto& analysis = options.normalize ? normalized : measured;

    std::vector<int> voices, backgroundVocals, backings, instruments;
    for (const auto& [id, profile] : analysis.tracks)
    {
        if (profile.getActiveSeconds() <= 0.0)
            continue;
        const auto role = roleOf (id);
        if (role == Role::voice)        voices.push_back (id);
        if (role == Role::backingVocal) backgroundVocals.push_back (id);
        if (role == Role::backing)      backings.push_back (id);
        if (isInstrument (role))        instruments.push_back (id);
    }

    if (voices.empty())
    {
        plan.notes.add ("No voice track with sound in it: mark one as Voice.");
        return plan;
    }

    // Final level (LUFS at unity master) and peak (dBFS) of every track after the plan.
    std::map<int, double> finalLoudness, finalPeak;
    for (const auto& [id, profile] : analysis.tracks)
    {
        finalLoudness[id] = profile.integratedLufs + volumeOf (id);
        finalPeak[id] = profile.peakDb + volumeOf (id);
    }

    // Voices: EQ, compressor, de-esser.
    std::map<int, VoicePlan> voicePlans;
    for (auto id : voices)
        voicePlans[id] = planVoice (id, *findProfile (analysis, id), targets, options, plan.notes,
                                    options.trackNames.count (id) > 0 ? options.trackNames.at (id) : "The voice");
    for (auto id : backgroundVocals)
        voicePlans[id] = planVoice (id, *findProfile (analysis, id), targets, options, plan.notes,
                                    options.trackNames.count (id) > 0 ? options.trackNames.at (id) : "The background vocals", true);

    // Rides on the voices: phrase to phrase, after the EQ and compressor.
    if (options.rides)
        for (auto id : voices)
        {
            auto& vp = voicePlans[id];
            vp.ride.trackId = id;
            vp.loudnessChange += planVoiceRide (vp.ride, *findProfile (analysis, id), vp.eqChange, vp.compressor, targets);
        }

    // The lead is the voice that sings the most; the others are balanced like it.
    const auto lead = *std::max_element (voices.begin(), voices.end(), [&] (int a, int b)
                                         { return findProfile (analysis, a)->getActiveSeconds() < findProfile (analysis, b)->getActiveSeconds(); });
    const auto& leadProfile = *findProfile (analysis, lead);

    // Backing and instruments: a dip where each one covers the lead's words most.
    // Instruments also get a low cut under their lowest notes and, when their level
    // swings a lot, a compressor. All of it in one EQ per track.
    std::vector<MixMove> pendingWidth;   // after each track's other inserts
    std::map<int, double> backingChange; // loudness change of each one while the lead sings
    for (auto id : backings)
        backingChange[id] = 0.0;
    for (auto id : instruments)
        backingChange[id] = 0.0;

    for (auto& [id, change] : backingChange)
    {
        const auto& profile = *findProfile (analysis, id);
        const auto role = roleOf (id);
        const auto instrument = isInstrument (role);

        // Per instrument: where its low cut may go, how deep a dip for the words, and
        // when it gets a compressor.
        struct { double minCut, maxCut, maskCap; float compSpread; bool alwaysComp; } shape { 40.0, 120.0, 3.0, 8.0f, false };
        if (role == Role::drums)       shape = { 30.0, 30.0, 2.0, 7.0f, false };  // the kick reaches low: rumble only
        else if (role == Role::bass)   shape = { 30.0, 30.0, 0.0, 6.0f, true };   // hardly covers the words: no dip
        else if (role == Role::guitar) shape = { 80.0, 150.0, 3.5, 8.0f, false }; // leaves the low end to the bass
        else if (role == Role::piano)  shape = { 40.0, 100.0, 3.0, 9.0f, false };

        MixMove eqMove;
        eqMove.trackId = id;
        eqMove.insert = fx::ModuleType::paraEq;
        fx::ParametricEq::Settings eq;
        eq.lowShelfDb = 0.0f;

        if (options.eq && instrument)
        {
            // The lowest band with much in it (within 24 dB of its loudest), and a low
            // cut an octave under that, so it doesn't add rumble under the song. A bass
            // or a kick reaches so low it gets none.
            const auto spectrum = profile.getAverageSpectrum();
            const auto loudest = *std::max_element (spectrum.begin(), spectrum.end());
            int lowest = -1;
            for (int b = 0; b < numBands && lowest < 0; ++b)
                if (spectrum[(size_t) b] > loudest - 24.0f)
                    lowest = b;

            auto cutHz = lowest >= 0 ? juce::jmin (shape.maxCut, 0.5 * getBandCentre (lowest)) : 0.0;
            if (role == Role::guitar || role == Role::drums || role == Role::bass)
                cutHz = juce::jlimit (shape.minCut, shape.maxCut, cutHz);

            if (cutHz >= shape.minCut)
            {
                eq.lowCutOn = true;
                eq.lowCutHz = roundTo (cutHz, 1.0);
                eqMove.reasons.add ("Low cut at " + formatHz (eq.lowCutHz)
                                    + (role == Role::drums || role == Role::bass ? ": rumble under the low end only"
                                       : role == Role::guitar ? ": leaves the low end to the bass and kick"
                                                              : ", under its lowest notes: less rumble under the song"));
            }
        }

        if (options.eq)
        {
            const auto masking = measureMasking (leadProfile, profile);
            if (masking.worstBand >= 0)
            {
                const auto difference = masking.backingOverVoiceDb[(size_t) masking.worstBand];
                const auto cut = juce::jlimit (0.0, instrument ? shape.maskCap : 4.0, 0.5 * (difference + 6.0));

                if (cut >= 1.0)
                {
                    const auto hz = getBandCentre (masking.worstBand);
                    eq.bells[1] = { (float) roundTo (hz, 1.0), -roundTo (cut, 0.5), 1.2f };
                    eqMove.reasons.add ("Dip " + formatDb (eq.bells[1].db) + " at " + formatHz (hz) + " so the words come through");
                }
            }
        }

        double eqChange = 0.0;
        if (! eqMove.reasons.isEmpty())
        {
            eqMove.values = { { "peqOn", 1.0f }, { "peqLowCutOn", eq.lowCutOn ? 1.0f : 0.0f }, { "peqLowGain", 0.0f }, { "peqHighGain", 0.0f },
                              { "peqHighCutOn", 0.0f }, { "peqBell1Gain", 0.0f }, { "peqBell3Gain", 0.0f },
                              { "peqBell2Freq", eq.bells[1].hz }, { "peqBell2Gain", eq.bells[1].db }, { "peqBell2Q", eq.bells[1].q } };
            if (eq.lowCutOn)
                eqMove.values["peqLowCutFreq"] = eq.lowCutHz;
            plan.moves.push_back (eqMove);

            const auto eqDb = fx::ParametricEq::getMagnitudeDb (eq, bandCentres(), analysisRate);
            change = loudnessChangeDb (profile.getAverageSpectrum (&leadProfile.active), eqDb);
            eqChange = loudnessChangeDb (profile.getAverageSpectrum(), eqDb);
        }

        // Bass: some saturation where the style asks, so it's heard on small speakers.
        // Wider guitars, keys and pads; bass and drums stay centred.
        if (role == Role::bass && options.saturation && targets.bassSaturationDb > 0.0f)
        {
            MixMove saturation;
            saturation.trackId = id;
            saturation.insert = fx::ModuleType::saturation;
            saturation.values = { { "satOn", 1.0f }, { "satDrive", targets.bassSaturationDb }, { "satMix", 40.0f } };
            saturation.reasons.add ("Saturation " + juce::String (targets.bassSaturationDb, 0)
                                    + " dB drive, mixed in at 40%: harmonics so the bass is heard on small speakers");
            plan.moves.push_back (saturation);
        }

        if (options.width && ! juce::approximatelyEqual (targets.widthAmount, 100.0f)
            && (role == Role::guitar || role == Role::piano || role == Role::instrument || role == Role::backing))
        {
            MixMove width;
            width.trackId = id;
            width.insert = fx::ModuleType::width;
            width.atEnd = true;
            width.values = { { "widthOn", 1.0f }, { "widthAmount", targets.widthAmount } };
            width.reasons.add ("Stereo width " + juce::String (targets.widthAmount, 0) + "%: "
                               + (targets.widthAmount > 100.0f ? "wider, leaving the middle to the voice" : "narrower, for a tighter " + getStyleName (style) + " sound"));
            pendingWidth.push_back (width);
        }

        if (options.compressor && instrument)
        {
            MixMove compressor;
            compressor.trackId = id;
            double peakChange = 0.0;
            const auto compChange = planCompressor (compressor, profile, eqChange, true, peakChange, shape.compSpread, shape.alwaysComp);
            if (! compressor.values.empty())
            {
                plan.moves.push_back (compressor);
                change += compChange;
                finalPeak[id] += peakChange;
                finalLoudness[id] += compChange;
            }
        }

        finalLoudness[id] += eqChange;

        // A dip while the lead sings, up between lines (not bass and drums: the groove
        // stays steady).
        if (options.rides && role != Role::bass && role != Role::drums)
        {
            MixMove duck;
            duck.trackId = id;
            double whileSinging = 0.0;
            const auto overall = planDuck (duck, profile, leadProfile, targets, whileSinging);
            if (! duck.ride.empty())
            {
                plan.moves.push_back (duck);
                change += whileSinging;
                finalLoudness[id] += overall;
            }
        }
    }

    for (const auto& move : pendingWidth)
        plan.moves.push_back (move);

    for (const auto& [id, vp] : voicePlans)
    {
        for (auto* move : { &vp.eq, &vp.compressor, &vp.deEsser, &vp.saturation, &vp.exciter, &vp.echo, &vp.reverb, &vp.width, &vp.ride })
            if (! move->values.empty() || ! move->ride.empty())
                plan.moves.push_back (*move);
    }

    // Balance: each voice's volume so it sits the style's LU over the backing and the
    // instruments together.
    double backingLoudness = -200.0;
    {
        std::vector<double> parts;
        for (const auto& [id, change] : backingChange)
            parts.push_back (findProfile (analysis, id)->getLoudnessOver (&leadProfile.active) + volumeOf (id) + change);
        backingLoudness = sumLoudness (parts);
    }

    if (backingLoudness <= -150.0)
        plan.notes.add ("No backing or instrument playing while you sing: the voice's level is left as it is.");

    for (auto id : voices)
    {
        const auto& profile = *findProfile (analysis, id);
        const auto& vp = voicePlans[id];
        const auto current = volumeOf (id);
        const auto voiceLoudness = profile.getLoudnessOver() + vp.loudnessChange; // at unity

        auto volume = current;
        if (backingLoudness > -150.0)
        {
            volume = (float) juce::jlimit (-30.0, 12.0, backingLoudness + targets.voiceOverBackingLu - voiceLoudness);
            volume = roundTo (volume, 0.1);

            MixMove move;
            move.trackId = id;
            move.values = { { "volume", volume } };
            move.reasons.add ("Volume " + juce::String (volume, 1) + " dB, so the voice sits "
                              + juce::String (targets.voiceOverBackingLu, 1) + " LU over the backing (" + getStyleName (style) + ")");
            plan.moves.push_back (move);
        }

        finalLoudness[id] = profile.integratedLufs + vp.loudnessChange + volume;
        finalPeak[id] = profile.peakDb + vp.peakChange + volume;

        if (id == lead)
            plan.voiceOverBackingLu = backingLoudness > -150.0 ? (float) (voiceLoudness + volume - backingLoudness) : 0.0f;
    }

    // Background vocals: the style's LU under the lead, each while it sings.
    {
        const auto& leadPlan = voicePlans[lead];
        const auto leadVolume = [&]
        {
            for (const auto& move : plan.moves)
                if (move.trackId == lead && ! move.insert.has_value() && move.values.count ("volume") > 0)
                    return (double) move.values.at ("volume");
            return (double) volumeOf (lead);
        }();
        const auto leadLoudness = leadProfile.getLoudnessOver() + leadPlan.loudnessChange + leadVolume;

        for (auto id : backgroundVocals)
        {
            const auto& profile = *findProfile (analysis, id);
            const auto& vp = voicePlans[id];
            const auto loudness = profile.getLoudnessOver() + vp.loudnessChange; // at unity
            const auto volume = roundTo (juce::jlimit (-30.0, 12.0, leadLoudness - targets.backingVocalUnderLeadLu - loudness), 0.1);

            MixMove move;
            move.trackId = id;
            move.values = { { "volume", volume } };
            move.reasons.add ("Volume " + juce::String (volume, 1) + " dB, so the background vocals sit "
                              + juce::String (targets.backingVocalUnderLeadLu, 1) + " LU under the lead (" + getStyleName (style) + ")");
            plan.moves.push_back (move);

            finalLoudness[id] = profile.integratedLufs + vp.loudnessChange + volume;
            finalPeak[id] = profile.peakDb + vp.peakChange + volume;
        }
    }

    // Master: towards the loudness target, without clipping (peaks add up at worst).
    {
        std::vector<double> parts;
        double peakSum = 0.0;
        for (const auto& [id, profile] : analysis.tracks)
        {
            parts.push_back (finalLoudness[id]);
            if (profile.peakDb > -150.0f)
                peakSum += std::pow (10.0, finalPeak[id] / 20.0);
        }

        const auto mix = sumLoudness (parts);
        if (mix > -150.0)
        {
            const auto forLoudness = targets.masterLufs - mix;
            const auto forPeak = peakSum > 0.0 ? -1.0 - 20.0 * std::log10 (peakSum) : 24.0;
            const auto master = roundTo (juce::jlimit (-24.0, 12.0, juce::jmin (forLoudness, forPeak)), 0.1);

            MixMove move;
            move.trackId = daw::Edit::masterId;
            move.values = { { "volume", master } };
            move.reasons.add ("Master " + juce::String (master, 1) + " dB, towards " + juce::String (targets.masterLufs, 0) + " LUFS");
            if (forPeak < forLoudness)
                move.reasons.add ("Held back " + formatDb (forLoudness - forPeak) + " so the peaks stay under -1 dB. "
                                  "Export with Normalise for more loudness.");
            plan.moves.push_back (move);
            plan.mixLufs = (float) (mix + master);
        }

        juce::ignoreUnused (currentMasterVolume);
    }

    return plan;
}

//==============================================================================
MixPlan planReferenceMatch (const Profile& mix, const Profile& reference, float currentMasterVolume)
{
    MixPlan plan;

    if (mix.getActiveSeconds() <= 0.0 || reference.getActiveSeconds() <= 0.0)
    {
        plan.notes.add (mix.getActiveSeconds() <= 0.0 ? "The mix is silent." : "Couldn't read the reference.");
        return plan;
    }

    // Tonal balance: the difference of the long-term spectra, both levelled over
    // 100 Hz - 10 kHz, smoothed over about an octave so only the broad shape counts.
    const auto mixSpectrum = mix.getAverageSpectrum(), referenceSpectrum = reference.getAverageSpectrum();
    std::vector<double> difference ((size_t) numBands, 0.0);
    {
        double offset = 0.0;
        int count = 0;
        for (int b = 0; b < numBands; ++b)
        {
            difference[(size_t) b] = juce::jmax (referenceSpectrum[(size_t) b], -120.0f) - juce::jmax (mixSpectrum[(size_t) b], -120.0f);
            if (getBandCentre (b) >= 100.0 && getBandCentre (b) <= 10000.0)
                offset += difference[(size_t) b], ++count;
        }
        offset /= juce::jmax (1, count);

        auto raw = difference;
        for (int b = 0; b < numBands; ++b)
        {
            double sum = 0.0;
            int n = 0;
            for (int k = juce::jmax (0, b - 1); k <= juce::jmin (numBands - 1, b + 1); ++k)
                sum += raw[(size_t) k] - offset, ++n;
            difference[(size_t) b] = sum / n;
        }
    }

    const auto meanOver = [&] (double fromHz, double toHz)
    {
        double sum = 0.0;
        int n = 0;
        for (int b = 0; b < numBands; ++b)
            if (getBandCentre (b) >= fromHz && getBandCentre (b) <= toHz)
                sum += difference[(size_t) b], ++n;
        return n > 0 ? sum / n : 0.0;
    };

    const auto gain = [] (double db) { return roundTo (juce::jlimit (-4.0, 4.0, 0.8 * db), 0.5); };

    fx::ParametricEq::Settings eq;
    eq.lowShelfHz = 120.0f;
    eq.lowShelfDb = gain (meanOver (40.0, 125.0));
    eq.highShelfHz = 8000.0f;
    eq.highShelfDb = gain (meanOver (8000.0, 16000.0));
    eq.bells[0] = { 300.0f, gain (meanOver (200.0, 450.0)), 0.7f };
    eq.bells[1] = { 1000.0f, gain (meanOver (700.0, 1400.0)), 0.7f };
    eq.bells[2] = { 3500.0f, gain (meanOver (2500.0, 5000.0)), 0.7f };

    MixMove eqMove;
    eqMove.trackId = daw::Edit::masterId;
    eqMove.insert = fx::ModuleType::paraEq;
    eqMove.values = { { "peqOn", 1.0f }, { "peqLowCutOn", 0.0f }, { "peqHighCutOn", 0.0f },
                      { "peqLowFreq", eq.lowShelfHz }, { "peqLowGain", eq.lowShelfDb },
                      { "peqHighFreq", eq.highShelfHz }, { "peqHighGain", eq.highShelfDb } };
    for (int b = 0; b < fx::ParametricEq::numBells; ++b)
    {
        const auto prefix = "peqBell" + juce::String (b + 1);
        eqMove.values[prefix + "Freq"] = eq.bells[(size_t) b].hz;
        eqMove.values[prefix + "Gain"] = eq.bells[(size_t) b].db;
        eqMove.values[prefix + "Q"] = eq.bells[(size_t) b].q;
    }

    const auto describe = [&eqMove] (float db, const juce::String& what)
    {
        if (std::abs (db) >= 0.5f)
            eqMove.reasons.add ((db > 0 ? "More " : "Less ") + what + ": " + (db > 0 ? "+" : "-") + formatDb (db));
    };
    describe (eq.lowShelfDb, "low end (under 120 Hz)");
    describe (eq.bells[0].db, "body (300 Hz)");
    describe (eq.bells[1].db, "middle (1 kHz)");
    describe (eq.bells[2].db, "presence (3.5 kHz)");
    describe (eq.highShelfDb, "top end (over 8 kHz)");
    if (eqMove.reasons.isEmpty())
        eqMove.reasons.add ("The tonal balance already matches the reference");
    plan.moves.push_back (eqMove);

    // Loudness: like the reference, without clipping.
    const auto eqDb = fx::ParametricEq::getMagnitudeDb (eq, bandCentres(), analysisRate);
    const auto eqChange = loudnessChangeDb (mixSpectrum, eqDb);
    const auto boost = juce::jmax (0.0, *std::max_element (eqDb.begin(), eqDb.end()));
    const auto forLoudness = reference.integratedLufs - (mix.integratedLufs + eqChange);
    const auto forPeak = -1.0 - (mix.peakDb + boost);
    const auto change = juce::jmin (forLoudness, forPeak);
    const auto master = roundTo (juce::jlimit (-24.0, 12.0, currentMasterVolume + change), 0.1);

    MixMove volume;
    volume.trackId = daw::Edit::masterId;
    volume.values = { { "volume", master } };
    volume.reasons.add ("Master " + juce::String (master, 1) + " dB, towards the reference's "
                        + juce::String (reference.integratedLufs, 1) + " LUFS");
    if (forPeak < forLoudness)
        volume.reasons.add ("Held back " + formatDb (forLoudness - forPeak) + " so the peaks stay under -1 dB. "
                            "Export with Normalise for more loudness.");
    plan.moves.push_back (volume);

    plan.mixLufs = (float) (mix.integratedLufs + eqChange + (master - currentMasterVolume));
    return plan;
}

//==============================================================================
void applyMix (daw::Edit& edit, const MixPlan& plan, float amount)
{
    // Volume rides are written whole (a new ride replaces one the track had).
    amount = juce::jlimit (0.0f, 1.0f, amount);
    edit.getUndoManager().beginNewTransaction ("Auto Mix"); // DawBridge looks for this name

    std::map<int, int> added; // inserts added per track, so they keep the plan's order

    for (const auto& move : plan.moves)
    {
        auto owner = move.trackId == daw::Edit::masterId ? edit.getMaster() : edit.getTrack (move.trackId);
        if (! owner.isValid())
            continue;

        if (! move.ride.empty())
        {
            std::vector<daw::Edit::RidePoint> points;
            for (const auto& [time, db] : move.ride)
                points.push_back ({ time, db * amount });
            edit.setVolumeRide (move.trackId, points);
            continue;
        }

        if (! move.insert.has_value() && move.values.count ("inGain") > 0)
        {
            // A change from the input gain it was measured with.
            const auto current = (float) owner.getProperty (daw::ids::inGain, 0.0f);
            edit.setChainGain (move.trackId, false, current + move.values.at ("inGain") * amount);
            continue;
        }

        if (! move.insert.has_value())
        {
            const auto current = (float) owner.getProperty (daw::ids::volume, 0.0f);
            const auto target = move.values.at ("volume");
            const auto volume = current + (target - current) * amount;

            if (move.trackId == daw::Edit::masterId)
                edit.setMasterVolume (volume);
            else
                edit.setTrackVolume (move.trackId, volume);
            continue;
        }

        // Reuse the track's first insert of this type, or add one.
        juce::ValueTree insert;
        for (const auto& child : daw::Edit::getInserts (owner))
            if (child[daw::ids::type].toString() == fx::getModuleKey (*move.insert))
            {
                insert = child;
                break;
            }

        if (! insert.isValid())
            insert = move.atEnd ? edit.addInsert (move.trackId, *move.insert)
                                : edit.addInsert (move.trackId, *move.insert, added[move.trackId]++);

        const auto id = (int) insert[daw::ids::id];
        for (const auto& [valueId, value] : move.values)
        {
            auto scaled = value;
            if (valueId.endsWith ("Gain") || valueId.endsWith ("Threshold") || valueId.endsWith ("Mix") || valueId == "exciterAmount")
                scaled = value * amount; // from 0 dB: no boost or cut, no compression; from a dry mix
            else if (valueId == "compRatio")
                scaled = 1.0f + (value - 1.0f) * amount;
            else if (valueId == "widthAmount")
                scaled = 100.0f + (value - 100.0f) * amount; // from as it is

            edit.setInsertValue (id, valueId, scaled);
        }
    }
}
}
