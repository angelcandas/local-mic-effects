#include "MixCheck.h"

#include "daw/Edit.h"
#include "dsp/ParametricEq.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace analysis
{
namespace
{
double dbToPower (double db) { return db <= -150.0 ? 0.0 : std::pow (10.0, db / 10.0); }
double powerToDb (double power) { return power > 1.0e-15 ? 10.0 * std::log10 (power) : -150.0; }
double roundTo (double value, double step) { return std::round (value / step) * step; }

juce::String formatDb (double db, int decimals = 1)
{
    return (decimals == 0 ? juce::String (juce::roundToInt (std::abs (db))) : juce::String (std::abs (db), decimals)) + " dB";
}

juce::String formatHz (double hz)
{
    return hz >= 1000.0 ? juce::String (hz / 1000.0, hz >= 10000.0 ? 0 : 1) + " kHz" : juce::String (juce::roundToInt (hz)) + " Hz";
}

juce::String formatTime (double seconds)
{
    const auto total = (int) std::floor (seconds);
    return juce::String (total / 60) + ":" + juce::String (total % 60).paddedLeft ('0', 2);
}

juce::String nameOf (const CheckContext& context, int id)
{
    const auto it = context.names.find (id);
    return it != context.names.end() && it->second.isNotEmpty() ? it->second : "Track " + juce::String (id);
}

Role roleOf (const CheckContext& context, int id)
{
    const auto it = context.roles.find (id);
    return it != context.roles.end() ? it->second : Role::other;
}

bool hasInsert (const CheckContext& context, int id, fx::ModuleType type)
{
    const auto it = context.inserts.find (id);
    return it != context.inserts.end() && std::find (it->second.begin(), it->second.end(), type) != it->second.end();
}

float masterValue (const CheckContext& context, const juce::String& id, float fallback)
{
    const auto it = context.masterValues.find (id);
    return it != context.masterValues.end() ? it->second : fallback;
}

// What a finished mix's long-term spectrum looks like (dB, relative): flat to 250 Hz, then
// falling about 4.5 dB an octave (the curve Master mode aims at too).
double typicalBalanceDb (double hz)
{
    static constexpr std::pair<double, double> points[] = { { 20.0, -10.0 }, { 50.0, -1.0 }, { 250.0, 0.0 }, { 1000.0, -9.0 },
                                                            { 4000.0, -18.0 }, { 10000.0, -25.0 }, { 20000.0, -36.0 } };
    if (hz <= points[0].first)
        return points[0].second;
    for (size_t i = 1; i < std::size (points); ++i)
        if (hz <= points[i].first)
        {
            const auto x = std::log (hz / points[i - 1].first) / std::log (points[i].first / points[i - 1].first);
            return points[i - 1].second + x * (points[i].second - points[i - 1].second);
        }
    return points[std::size (points) - 1].second;
}

constexpr int numTonalBands = 4;
constexpr std::pair<double, double> tonalRanges[numTonalBands] = { { 40.0, 200.0 }, { 200.0, 2000.0 }, { 2000.0, 6000.0 }, { 6000.0, 16000.0 } };
const char* const tonalNames[numTonalBands] = { "low end", "mids", "presence", "top end" };
const char* const tonalHints[numTonalBands] = { "40-200 Hz", "200 Hz-2 kHz", "2-6 kHz", "6-16 kHz" };

// Where a style sits against the typical balance, per band (dB): hip-hop and R&B carry more
// low end, lo-fi and jazz are darker, and so on.
std::array<double, numTonalBands> styleTilt (MixStyle style)
{
    switch (style)
    {
        case MixStyle::hipHop:     return { 3.0, 0.0, 0.0, 0.0 };
        case MixStyle::rnb:        return { 2.0, 0.0, 0.0, 0.0 };
        case MixStyle::electronic: return { 2.0, 0.0, 1.0, 1.0 };
        case MixStyle::lofi:       return { 0.0, 0.0, -2.0, -5.0 };
        case MixStyle::jazz:       return { 0.0, 0.0, -1.0, -2.0 };
        case MixStyle::ballad:     return { 0.0, 0.0, -1.0, -1.5 };
        case MixStyle::acoustic:   return { -1.0, 0.0, 0.0, 0.0 };
        case MixStyle::rock:       return { 0.0, 0.5, 0.5, 0.0 };
        case MixStyle::speech:     return { -3.0, 1.0, 1.0, -2.0 };
        case MixStyle::pop:
        case MixStyle::coaching:   break;
    }
    return { 0.0, 0.0, 0.0, 0.0 };
}

// Per band: how far over (+) or under (-) the style's balance the mix is, dB.
std::array<double, numTonalBands> measureTonalBalance (const Profile& mix, MixStyle style)
{
    const auto spectrum = mix.getAverageSpectrum();
    std::vector<double> diff ((size_t) numBands);
    double offset = 0.0;
    int count = 0;
    for (int b = 0; b < numBands; ++b)
    {
        diff[(size_t) b] = juce::jmax (spectrum[(size_t) b], -120.0f) - typicalBalanceDb (getBandCentre (b));
        if (getBandCentre (b) >= 100.0 && getBandCentre (b) <= 10000.0)
            offset += diff[(size_t) b], ++count;
    }
    offset /= juce::jmax (1, count);

    const auto tilt = styleTilt (style);
    std::array<double, numTonalBands> result {};
    for (int g = 0; g < numTonalBands; ++g)
    {
        double sum = 0.0;
        int n = 0;
        for (int b = 0; b < numBands; ++b)
            if (getBandCentre (b) >= tonalRanges[g].first && getBandCentre (b) <= tonalRanges[g].second)
                sum += diff[(size_t) b] - offset, ++n;
        result[(size_t) g] = (n > 0 ? sum / n : 0.0) - tilt[(size_t) g];
    }
    return result;
}

// Loudness range (EBU 3342 flavour): 3 s windows over the momentary values, gated at -70
// LUFS and 20 LU under the rest, the spread between the 10th and 95th percentile.
float measureLoudnessRange (const Profile& mix)
{
    constexpr int window = 30; // hops
    const auto n = (int) mix.momentaryLufs.size();
    if (n < window)
        return 0.0f;

    std::vector<double> shortTerm;
    for (int i = window; i <= n; ++i)
    {
        double power = 0.0;
        for (int k = i - window; k < i; ++k)
            power += dbToPower (mix.momentaryLufs[(size_t) k]);
        shortTerm.push_back (powerToDb (power / window));
    }

    std::vector<double> gated;
    for (auto v : shortTerm)
        if (v > -70.0)
            gated.push_back (v);
    if (gated.empty())
        return 0.0f;

    double mean = 0.0;
    for (auto v : gated)
        mean += dbToPower (v);
    const auto relative = powerToDb (mean / (double) gated.size()) - 20.0;
    gated.erase (std::remove_if (gated.begin(), gated.end(), [relative] (double v) { return v < relative; }), gated.end());
    if (gated.size() < 2)
        return 0.0f;

    std::sort (gated.begin(), gated.end());
    const auto at = [&gated] (double fraction) { return gated[(size_t) juce::jlimit (0, (int) gated.size() - 1, (int) std::floor (fraction * (double) (gated.size() - 1)))]; };
    return (float) (at (0.95) - at (0.10));
}

float getLoudnessRangeFloor (MixStyle style) noexcept
{
    switch (style)
    {
        case MixStyle::jazz:       return 5.0f;
        case MixStyle::acoustic:
        case MixStyle::ballad:     return 4.0f;
        case MixStyle::speech:
        case MixStyle::coaching:   return 3.0f;
        case MixStyle::pop:
        case MixStyle::rock:
        case MixStyle::hipHop:
        case MixStyle::rnb:
        case MixStyle::electronic:
        case MixStyle::lofi:       break;
    }
    return 2.0f;
}

struct StereoTotals
{
    std::array<double, Profile::stereoBands> mid {}, side {};
};

StereoTotals totalStereo (const Profile& mix)
{
    StereoTotals totals;
    for (size_t i = 0; i < mix.stereoHops.size() && i < mix.active.size(); ++i)
        if (mix.active[i])
            for (size_t b = 0; b < Profile::stereoBands; ++b)
            {
                totals.mid[b] += mix.stereoHops[i].mid[b];
                totals.side[b] += mix.stereoHops[i].side[b];
            }
    return totals;
}

// The 5-10 kHz level of a voice against its 1-4 kHz level, in the loudest 5% of its active
// frames (that is where the S's are), dB.
struct Sibilance { float ratioDb = -100.0f; int band = -1; float p75Level = -60.0f; };

Sibilance measureSibilance (const Profile& voice)
{
    std::vector<std::pair<float, float>> frames; // (hf level, ratio)
    std::vector<float> levels;
    std::array<double, numBands> bandSum {};
    for (size_t i = 0; i < voice.bandsDb.size(); ++i)
        if (voice.active[i])
        {
            double hf = 0.0, presence = 0.0;
            for (int b = 0; b < numBands; ++b)
            {
                const auto power = dbToPower (voice.bandsDb[i][(size_t) b]);
                if (getBandCentre (b) >= 5000.0 && getBandCentre (b) <= 10000.0)
                    hf += power, bandSum[(size_t) b] += power;
                else if (getBandCentre (b) >= 1000.0 && getBandCentre (b) <= 4000.0)
                    presence += power;
            }
            frames.push_back ({ (float) powerToDb (hf), (float) (powerToDb (hf) - powerToDb (presence)) });
            levels.push_back ((float) powerToDb (hf));
        }

    Sibilance result;
    if (frames.size() < 10)
        return result;

    std::sort (frames.begin(), frames.end());
    const auto from = (size_t) std::floor (0.95 * (double) frames.size());
    double ratio = 0.0;
    int count = 0;
    for (auto i = from; i < frames.size(); ++i)
        ratio += frames[i].second, ++count;
    result.ratioDb = (float) (ratio / juce::jmax (1, count));

    std::sort (levels.begin(), levels.end());
    result.p75Level = juce::jlimit (-60.0f, 0.0f, levels[(size_t) std::floor (0.75 * (double) (levels.size() - 1))]);
    for (int b = 0; b < numBands; ++b)
        if (getBandCentre (b) >= 5000.0 && getBandCentre (b) <= 10000.0 && (result.band < 0 || bandSum[(size_t) b] > bandSum[(size_t) result.band]))
            result.band = b;
    return result;
}

const Profile* findProfile (const SessionAnalyser::Result& tracks, int id)
{
    for (const auto& [trackId, profile] : tracks.tracks)
        if (trackId == id)
            return &profile;
    return nullptr;
}

bool isVoiceRole (Role role) noexcept { return role == Role::voice || role == Role::backingVocal; }
}

//==============================================================================
const char* getFixKey (FixKind fix) noexcept
{
    switch (fix)
    {
        case FixKind::none:       return "none";
        case FixKind::master:     return "master";
        case FixKind::tonal:      return "tonal";
        case FixKind::monoLows:   return "monoLows";
        case FixKind::narrow:     return "narrow";
        case FixKind::masking:    return "masking";
        case FixKind::clipping:   return "clipping";
        case FixKind::hum:        return "hum";
        case FixKind::deEss:      return "deEss";
        case FixKind::voiceLevel: return "voiceLevel";
    }
    return "none";
}

float getPeakToLoudnessFloor (MixStyle style) noexcept
{
    switch (style)
    {
        case MixStyle::pop:
        case MixStyle::rock:
        case MixStyle::hipHop:     return 7.0f;
        case MixStyle::electronic: return 6.5f;
        case MixStyle::rnb:
        case MixStyle::lofi:       return 8.0f;
        case MixStyle::coaching:   return 9.0f;
        case MixStyle::ballad:
        case MixStyle::speech:     return 10.0f;
        case MixStyle::acoustic:   return 11.0f;
        case MixStyle::jazz:       return 12.0f;
    }
    return 7.0f;
}

//==============================================================================
Report checkMix (const SessionAnalyser::Result& tracks, const Profile& mix, const CheckContext& context)
{
    Report report;
    report.targetLufs = context.targetLufs;
    report.seconds = mix.getNumFrames() * Profile::hopSeconds;
    report.lufs = mix.integratedLufs;
    report.truePeakDb = mix.truePeakDb;
    report.loudnessRange = measureLoudnessRange (mix);
    report.plr = mix.integratedLufs > -150.0f ? mix.truePeakDb - mix.integratedLufs : 0.0f;

    for (const auto& [id, profile] : tracks.tracks)
        if (context.muted.count (id) == 0)
            ++report.tracksChecked;

    if (mix.getActiveSeconds() <= 0.0 || mix.integratedLufs <= -150.0f)
    {
        Finding f;
        f.key = "silent";
        f.severity = 2;
        f.title = "The mix is silent";
        f.detail = "Nothing is playing: no clips are audible, or every track is muted.";
        f.why = "There is nothing to check yet.";
        report.findings.push_back (f);
        report.score = 0;
        return report;
    }
    report.empty = false;

    const auto add = [&report] (Finding f) { report.findings.push_back (std::move (f)); };
    const auto sev = [] (double excess, double warn, double problem) { return excess >= problem ? 2 : excess >= warn ? 1 : 0; };

    //-------------------------------------------------------------------- loudness
    {
        const auto diff = mix.integratedLufs - context.targetLufs;
        if (std::abs (diff) >= 1.5)
        {
            Finding f;
            f.key = "loudness";
            f.severity = juce::jmax (1, sev (std::abs (diff), 1.5, 3.0));
            f.value = mix.integratedLufs;
            f.target = context.targetLufs;
            f.fix = FixKind::master;
            f.title = diff > 0 ? "Louder than the target" : "Quieter than the target";
            f.detail = "The mix measures " + juce::String (mix.integratedLufs, 1) + " LUFS; the target is "
                     + juce::String (context.targetLufs, 0) + " LUFS (" + formatDb (diff) + (diff > 0 ? " over" : " under") + ").";
            f.why = diff > 0 ? "Streaming services turn a louder song down, so the extra squash buys nothing and costs punch."
                             : "A quieter song is turned up by some services and left alone by others: it sounds weaker next to other songs.";
            add (f);
        }
    }

    //-------------------------------------------------------------------- true peak
    if (mix.truePeakDb > -1.0f)
    {
        Finding f;
        f.key = "truePeak";
        f.severity = mix.truePeakDb > 0.0f ? 2 : 1;
        f.value = mix.truePeakDb;
        f.target = -1.0f;
        f.fix = FixKind::master;
        f.title = "Peaks over -1 dBTP";
        f.detail = "The highest peak between the samples is " + juce::String (mix.truePeakDb, 1) + " dBTP; the safe ceiling is -1 dBTP.";
        f.why = "When a service converts the song to MP3 or AAC, peaks this high can clip and crackle.";
        add (f);
    }

    //-------------------------------------------------------------------- over-compressed
    {
        const auto plrFloor = getPeakToLoudnessFloor (context.style);
        const auto lraFloor = getLoudnessRangeFloor (context.style);
        const auto plrShort = plrFloor - report.plr;
        const auto lraShort = report.seconds >= 6.0 ? lraFloor - report.loudnessRange : 0.0f;
        const auto worst = juce::jmax (plrShort, lraShort);
        if (worst > 0.0f)
        {
            Finding f;
            f.key = "overCompressed";
            f.severity = worst >= 2.0f ? 2 : 1;
            f.value = plrShort >= lraShort ? report.plr : report.loudnessRange;
            f.target = plrShort >= lraShort ? plrFloor : lraFloor;
            f.title = "Squashed dynamics";
            f.detail = plrShort >= lraShort
                         ? "Peaks are only " + juce::String (report.plr, 1) + " dB above the average loudness (" + getStyleName (context.style)
                               + " usually keeps at least " + juce::String (plrFloor, 0) + " dB)."
                         : "The loudness moves only " + juce::String (report.loudnessRange, 1) + " LU from quiet to loud parts (at least "
                               + juce::String (lraFloor, 0) + " LU is usual for " + getStyleName (context.style) + ").";
            f.why = "A flat, squashed mix feels tiring and loses the impact of drums and choruses. Back off the limiter or the bus compressor.";
            add (f);
        }
    }

    //-------------------------------------------------------------------- tonal balance
    {
        const auto dev = measureTonalBalance (mix, context.style);
        for (int g = 0; g < numTonalBands; ++g)
        {
            const auto magnitude = std::abs (dev[(size_t) g]);
            if (magnitude < 4.0)
                continue;

            Finding f;
            f.key = "tonalBalance";
            f.severity = magnitude >= 6.0 ? 2 : 1;
            f.band = g;
            f.value = (float) dev[(size_t) g];
            f.target = 0.0f;
            f.fix = FixKind::tonal;
            f.title = juce::String (tonalNames[g]).substring (0, 1).toUpperCase() + juce::String (tonalNames[g]).substring (1)
                    + (dev[(size_t) g] > 0 ? " too strong" : " too weak");
            f.detail = "The " + juce::String (tonalNames[g]) + " (" + tonalHints[g] + ") sit " + formatDb (dev[(size_t) g])
                     + (dev[(size_t) g] > 0 ? " over" : " under") + " the usual balance for " + getStyleName (context.style) + ".";
            f.why = g == 0 ? (dev[(size_t) g] > 0 ? "Too much low end sounds boomy and takes the room the voice and the limiter need."
                                                  : "Too little low end sounds thin on any speaker with a bass.")
                  : g == 1 ? (dev[(size_t) g] > 0 ? "A heavy mid range sounds boxy and crowds the vocal." : "A hollow mid range loses the body of the instruments.")
                  : g == 2 ? (dev[(size_t) g] > 0 ? "Too much presence is harsh and tiring at higher volumes." : "Too little presence makes the words and the attack of the instruments hard to hear.")
                           : (dev[(size_t) g] > 0 ? "A bright top end hisses and gets sharp on earbuds." : "A dull top end sounds closed in, with no air.");
            add (f);
        }
    }

    //-------------------------------------------------------------------- stereo
    if (mix.stereo)
    {
        const auto totals = totalStereo (mix);

        // Low end not mono.
        if (totals.mid[0] + totals.side[0] > 1.0e-12)
        {
            const auto ratio = juce::jlimit (-60.0, 40.0, powerToDb (totals.side[0]) - powerToDb (totals.mid[0]));
            if (ratio > -10.0)
            {
                Finding f;
                f.key = "lowEndStereo";
                f.severity = ratio > -6.0 ? 2 : 1;
                f.value = (float) ratio;
                f.target = -15.0f;
                f.fix = FixKind::monoLows;
                f.title = "Low end is not mono";
                f.detail = "Under 120 Hz the sides carry " + formatDb (ratio) + " under the middle; a solid low end is nearly all in the middle.";
                f.why = "Wide bass cancels on mono speakers and phones, and makes vinyl and club systems unstable.";
                add (f);
            }
        }

        // Mono fold-down loss in the mids and highs.
        for (int b = 1; b < Profile::stereoBands; ++b)
        {
            const auto total = totals.mid[(size_t) b] + totals.side[(size_t) b];
            if (total < 1.0e-12)
                continue;
            const auto loss = juce::jmax (-40.0, powerToDb (totals.mid[(size_t) b]) - powerToDb (total)); // negative: what a mono sum drops
            if (loss < -3.0)
            {
                Finding f;
                f.key = "monoLoss";
                f.severity = loss < -5.0 ? 2 : 1;
                f.band = b;
                f.value = (float) loss;
                f.target = -3.0f;
                f.fix = FixKind::narrow;
                f.title = b == 1 ? "Mids drop in mono" : "Highs drop in mono";
                f.detail = "Folded to mono, the " + juce::String (b == 1 ? "mid range" : "high end") + " loses " + formatDb (loss)
                         + " (3 dB or less is fine).";
                f.why = "Phones, smart speakers and many clubs play mono: wide, out-of-phase parts shrink or vanish there.";
                add (f);
            }
        }

        // Out of phase for more than a second.
        {
            int run = 0, bestRun = 0, bestEnd = 0;
            for (size_t i = 0; i < mix.stereoHops.size(); ++i)
            {
                if (i < mix.active.size() && mix.active[i] && mix.stereoHops[i].correlation < 0.0f)
                {
                    if (++run > bestRun)
                        bestRun = run, bestEnd = (int) i;
                }
                else
                    run = 0;
            }

            if (bestRun >= 10)
            {
                Finding f;
                f.key = "phase";
                f.severity = bestRun >= 30 ? 2 : 1;
                f.from = (bestEnd - bestRun + 1) * Profile::hopSeconds;
                f.to = (bestEnd + 1) * Profile::hopSeconds;
                f.value = (float) (bestRun * Profile::hopSeconds);
                f.target = 0.0f;
                f.title = "Left and right fight each other";
                f.detail = "From " + formatTime (f.from) + " the channels are out of phase for " + juce::String (f.value, 1)
                         + " s (the correlation stays under zero).";
                f.why = "Out-of-phase parts cancel in mono and sound hollow and unfocused in stereo. Check for a flipped polarity or a doubled, delayed track.";
                add (f);
            }
        }
    }

    //-------------------------------------------------------------------- DC offset
    if (mix.dcOffset > 0.01f)
    {
        Finding f;
        f.key = "dcOffset";
        f.severity = 1;
        f.value = mix.dcOffset * 100.0f;
        f.title = "DC offset in the mix";
        f.detail = "The signal sits " + juce::String (mix.dcOffset * 100.0f, 1) + " % away from zero.";
        f.why = "It eats headroom and makes clicks at edits. A low cut on the master (Master mode adds one) removes it.";
        add (f);
    }

    //-------------------------------------------------------------------- the tracks
    const auto lead = [&]
    {
        for (const auto& [id, profile] : tracks.tracks)
            if (context.muted.count (id) == 0 && roleOf (context, id) == Role::voice && profile.getActiveSeconds() > 0.0)
                return id;
        return 0;
    }();

    for (const auto& [id, profile] : tracks.tracks)
    {
        if (context.muted.count (id) > 0)
            continue;
        const auto name = nameOf (context, id);

        if (profile.getActiveSeconds() < 0.3)
        {
            Finding f;
            f.key = "neverHeard";
            f.severity = 1;
            f.trackId = id;
            f.title = name + " is never heard";
            f.detail = "It plays less than a third of a second of sound in the whole song.";
            f.why = "A track that's silent is dead weight: an empty clip, a muted instrument, or a plugin that makes no sound.";
            add (f);
            continue;
        }

        if (profile.clippedRuns > 0)
        {
            Finding f;
            f.key = "clipping";
            f.severity = profile.clippedRuns >= 10 ? 2 : 1;
            f.trackId = id;
            f.value = (float) profile.clippedRuns;
            f.from = profile.clipTimes.empty() ? 0.0 : profile.clipTimes.front();
            f.to = f.from;
            f.fix = FixKind::clipping;
            f.title = name + " is clipped";
            f.detail = juce::String (profile.clippedRuns) + (profile.clippedRuns == 1 ? " stretch" : " stretches") + " hit full scale, the first at "
                     + formatTime (f.from) + ".";
            f.why = "Clipping adds a rough buzz that no effect removes. Trimming only gives the effects after it headroom; to repair the clipped parts, re-record or use a de-clipper.";
            add (f);
        }

        if (profile.humHz > 0.0f && profile.getHumProminenceDb() >= 10.0f && context.humCut.count (id) == 0)
        {
            Finding f;
            f.key = "hum";
            f.severity = profile.getHumProminenceDb() >= 20.0f ? 2 : 1;
            f.trackId = id;
            f.value = profile.humHz;
            f.fix = FixKind::hum;
            f.title = name + " hums";
            f.detail = juce::String (profile.humHz, 0) + " Hz mains hum, standing " + formatDb (profile.getHumProminenceDb(), 0) + " over the sound around it.";
            f.why = "Hum is a constant tone under everything that gets louder with every compressor and limiter after it.";
            add (f);
        }

        if (isVoiceRole (roleOf (context, id)) && ! hasInsert (context, id, fx::ModuleType::deEsser))
        {
            const auto sibilance = measureSibilance (profile);
            if (sibilance.ratioDb > 10.0f)
            {
                Finding f;
                f.key = "sibilance";
                f.severity = sibilance.ratioDb > 15.0f ? 2 : 1;
                f.trackId = id;
                f.value = sibilance.ratioDb;
                f.target = 10.0f;
                f.band = sibilance.band;
                f.fix = FixKind::deEss;
                f.title = name + " has sharp S's";
                f.detail = "On its S's the 5-10 kHz range stands " + formatDb (sibilance.ratioDb, 0) + " over the presence range.";
                f.why = "Piercing S's get harsher with every compressor and bright earbuds. A de-esser tames only those moments.";
                add (f);
            }
        }
    }

    //-------------------------------------------------------------------- masking: what covers the voice
    if (lead != 0)
    {
        const auto* voice = findProfile (tracks, lead);
        struct Pair { int track; double difference; int band; };
        std::vector<Pair> pairs;
        for (const auto& [id, profile] : tracks.tracks)
        {
            if (id == lead || context.muted.count (id) > 0 || isVoiceRole (roleOf (context, id)) || profile.getActiveSeconds() < 1.0)
                continue;
            const auto masking = measureMasking (*voice, profile);
            if (masking.worstBand < 0 || masking.overlapSeconds < 2.0)
                continue;
            const auto difference = (double) masking.backingOverVoiceDb[(size_t) masking.worstBand];
            if (difference >= -4.0) // the same point Auto Mix starts to dip
                pairs.push_back ({ id, difference, masking.worstBand });
        }
        std::sort (pairs.begin(), pairs.end(), [] (const Pair& a, const Pair& b) { return a.difference > b.difference; });
        if (pairs.size() > 3)
            pairs.resize (3);

        for (const auto& pair : pairs)
        {
            Finding f;
            f.key = "masking";
            f.severity = pair.difference >= 4.0 ? 2 : 1;
            f.trackId = pair.track;
            f.otherTrackId = lead;
            f.band = pair.band;
            f.value = (float) pair.difference;
            f.target = -6.0f;
            f.fix = FixKind::masking;
            f.title = nameOf (context, pair.track) + " covers " + nameOf (context, lead);
            f.detail = "Around " + formatHz (getBandCentre (pair.band)) + " it is " + formatDb (pair.difference, 0) + (pair.difference >= 0 ? " louder than" : " under")
                     + " the voice while the voice sings.";
            f.why = "When two sounds fill the same frequencies the quieter one disappears: the words get hard to follow. A small dip in the backing opens a gap for the voice.";
            add (f);
        }

        //---------------------------------------------------------------- lead voice against the rest
        std::vector<bool> mask = voice->active;
        std::vector<double> others (mask.size(), 0.0);
        int numOthers = 0;
        for (const auto& [id, profile] : tracks.tracks)
        {
            if (id == lead || context.muted.count (id) > 0 || profile.getActiveSeconds() <= 0.0)
                continue;
            ++numOthers;
            const auto volume = context.volumes.count (id) > 0 ? (double) context.volumes.at (id) : 0.0;
            for (size_t i = 0; i < others.size() && i < profile.momentaryLufs.size(); ++i)
                others[i] += dbToPower (profile.momentaryLufs[i] + volume);
        }

        if (numOthers > 0)
        {
            double voicePower = 0.0, othersPower = 0.0;
            int count = 0;
            const auto leadVolume = context.volumes.count (lead) > 0 ? (double) context.volumes.at (lead) : 0.0;
            for (size_t i = 0; i < mask.size() && i < voice->momentaryLufs.size(); ++i)
                if (mask[i])
                    voicePower += dbToPower (voice->momentaryLufs[i] + leadVolume), othersPower += others[i], ++count;

            if (count >= 20 && othersPower > 0.0)
            {
                const auto over = powerToDb (voicePower / count) - powerToDb (othersPower / count);
                const auto wanted = (double) getTargets (context.style).voiceOverBackingLu;
                const auto deviation = over - wanted;
                if (std::abs (deviation) >= 3.0)
                {
                    Finding f;
                    f.key = "leadBalance";
                    f.severity = std::abs (deviation) >= 6.0 ? 2 : 1;
                    f.trackId = lead;
                    f.value = (float) over;
                    f.target = (float) wanted;
                    f.fix = FixKind::voiceLevel;
                    f.title = deviation < 0 ? nameOf (context, lead) + " is buried" : nameOf (context, lead) + " is too loud";
                    f.detail = "While it sings, " + nameOf (context, lead) + " is " + juce::String (over, 1) + " LU " + (over >= 0 ? "over" : "under")
                             + " everything else (fader positions counted); " + getStyleName (context.style) + " usually sits "
                             + juce::String (wanted, 0) + " LU over.";
                    f.why = deviation < 0 ? "The words get lost and the listener stops following the song."
                                          : "A voice sitting on top of the music sounds pasted on and tires the ear.";
                    add (f);
                }
            }
        }
    }

    //-------------------------------------------------------------------- silence at the head and tail
    {
        int first = -1, last = -1;
        for (int i = 0; i < (int) mix.active.size(); ++i)
            if (mix.active[(size_t) i])
            {
                if (first < 0)
                    first = i;
                last = i;
            }

        if (first >= 0)
        {
            const auto head = first * Profile::hopSeconds;
            const auto tail = (mix.getNumFrames() - 1 - last) * Profile::hopSeconds;
            if (head > 3.0)
            {
                Finding f;
                f.key = "silenceHead";
                f.severity = head > 8.0 ? 1 : 0;
                f.from = 0.0;
                f.to = head;
                f.value = (float) head;
                f.title = "Silence before the start";
                f.detail = "Nothing plays for the first " + juce::String (head, 1) + " s.";
                f.why = "Listeners skip songs that start with dead air; trim the clips or move them to the start.";
                add (f);
            }
            if (tail > 5.0)
            {
                Finding f;
                f.key = "silenceTail";
                f.severity = tail > 12.0 ? 1 : 0;
                f.from = mix.getNumFrames() * Profile::hopSeconds - tail;
                f.to = mix.getNumFrames() * Profile::hopSeconds;
                f.value = (float) tail;
                f.title = "Silence after the end";
                f.detail = "Nothing plays for the last " + juce::String (tail, 1) + " s.";
                f.why = "A long silent tail makes the song feel unfinished in playlists and adds dead time to exports.";
                add (f);
            }
        }
    }

    std::stable_sort (report.findings.begin(), report.findings.end(), [] (const Finding& a, const Finding& b) { return a.severity > b.severity; });

    int penalty = 0;
    for (const auto& f : report.findings)
        penalty += f.severity == 2 ? 12 : f.severity == 1 ? 5 : 1;
    report.score = juce::jlimit (0, 100, 100 - penalty);
    return report;
}

//==============================================================================
MixPlan planFix (const Finding& finding, const SessionAnalyser::Result& tracks, const Profile& mix, const CheckContext& context)
{
    MixPlan plan;
    const auto master = daw::Edit::masterId;

    switch (finding.fix)
    {
        case FixKind::none:
            break;

        case FixKind::master:
        {
            if (hasInsert (context, master, fx::ModuleType::limiter))
            {
                // A limiter is there already: move its drive (the loudness) and ceiling.
                MixMove move;
                move.trackId = master;
                move.insert = fx::ModuleType::limiter;
                const auto drive = masterValue (context, "limDrive", 0.0f);
                const auto gap = context.targetLufs - mix.integratedLufs;
                const auto newDrive = juce::jlimit (0.0f, 12.0f, (float) roundTo (drive + gap, 0.5));
                move.values = { { "limOn", 1.0f }, { "limDrive", newDrive }, { "limCeiling", -1.0f } };
                move.reasons.add ("Limiter drive " + juce::String (newDrive, 1) + " dB (it was " + juce::String (drive, 1) + ") and ceiling -1 dB: "
                                  + "towards " + juce::String (context.targetLufs, 0) + " LUFS without peaks over -1 dBTP");
                plan.moves.push_back (move);
                plan.mixLufs = (float) (mix.integratedLufs + (newDrive - drive));
            }
            else
            {
                plan = planMaster (mix, context.targetLufs, context.masterVolume);
            }
            break;
        }

        case FixKind::tonal:
        {
            const auto dev = measureTonalBalance (mix, context.style);
            const auto step = [&] (double deviation) { return juce::jlimit (-3.0, 3.0, roundTo (-0.6 * deviation, 0.5)); };

            MixMove move;
            move.trackId = master;
            move.insert = fx::ModuleType::masterEq;
            move.atEnd = true;
            move.values = { { "meqOn", 1.0f } };
            if (! context.masterValues.count ("meqLowFreq"))
                move.values["meqLowFreq"] = 100.0f;

            const auto set = [&] (const char* id, double change, const juce::String& what)
            {
                if (std::abs (change) < 0.5)
                    return;
                move.values[id] = masterValue (context, id, 0.0f) + (float) change;
                move.reasons.add ((change > 0 ? "More " : "Less ") + what + ": " + (change > 0 ? "+" : "-") + formatDb (change));
            };
            set ("meqLowGain", step (dev[0]), "low end (100 Hz shelf)");

            // The mid bell goes where the mix is furthest off: the mids or the presence.
            const auto g = std::abs (dev[1]) >= std::abs (dev[2]) ? 1 : 2;
            if (std::abs (step (dev[(size_t) g])) >= 0.5)
            {
                if (! context.masterValues.count ("meqMidFreq"))
                    move.values["meqMidFreq"] = g == 1 ? 700.0f : 3500.0f;
                set ("meqMidGain", step (dev[(size_t) g]), g == 1 ? "mids (around 700 Hz)" : "presence (around 3.5 kHz)");
            }

            if (! context.masterValues.count ("meqHighFreq"))
                move.values["meqHighFreq"] = 8000.0f;
            set ("meqHighGain", step (dev[3]), "top end (8 kHz shelf)");

            if (! move.reasons.isEmpty())
                plan.moves.push_back (move);
            else
                plan.notes.add ("The balance is within 3 dB of the style's: nothing to move.");
            plan.notes.add ("A gentle first step (at most 3 dB a band): check the result and run Mix Check again.");
            break;
        }

        case FixKind::monoLows:
        {
            MixMove move;
            move.trackId = master;
            move.insert = fx::ModuleType::imager;
            move.atEnd = true;
            move.values = { { "imgOn", 1.0f }, { "imgMonoBelow", 120.0f } };
            move.reasons.add ("Stereo imager: everything under 120 Hz summed to mono, the rest as it is");
            plan.moves.push_back (move);
            break;
        }

        case FixKind::narrow:
        {
            MixMove move;
            move.trackId = master;
            move.insert = fx::ModuleType::imager;
            move.atEnd = true;
            const auto current = masterValue (context, "imgWidth", 100.0f);
            const auto width = juce::jlimit (60.0f, 100.0f, current - 15.0f);
            move.values = { { "imgOn", 1.0f }, { "imgWidth", width } };
            move.reasons.add ("Stereo imager width " + juce::String (width, 0) + " % (was " + juce::String (current, 0) + " %): "
                              "the sides come down a little so the mix folds to mono better");
            plan.moves.push_back (move);
            break;
        }

        case FixKind::masking:
        {
            const auto cut = juce::jlimit (1.0, isInstrument (roleOf (context, finding.trackId)) ? 6.0 : 4.0, 0.5 * ((double) finding.value + 6.0));
            const auto hz = roundTo (getBandCentre (finding.band), 1.0);
            MixMove move;
            move.trackId = finding.trackId;
            move.insert = fx::ModuleType::paraEq;
            move.values = { { "peqOn", 1.0f }, { "peqBell2Freq", (float) hz }, { "peqBell2Gain", (float) -roundTo (cut, 0.5) }, { "peqBell2Q", 1.2f } };
            if (! hasInsert (context, finding.trackId, fx::ModuleType::paraEq))
                for (const char* id : { "peqLowGain", "peqHighGain", "peqBell1Gain", "peqBell3Gain" })
                    move.values[id] = 0.0f;
            move.reasons.add ("Dip " + formatDb (-roundTo (cut, 0.5)) + " at " + formatHz (hz) + " so " + nameOf (context, finding.otherTrackId) + " comes through");
            plan.moves.push_back (move);
            break;
        }

        case FixKind::clipping:
        {
            MixMove move;
            move.trackId = finding.trackId;
            move.values = { { "inGain", -4.0f } };
            move.reasons.add ("Input gain down 4 dB: headroom for the effects after it (the clipped parts stay clipped)");
            plan.moves.push_back (move);
            break;
        }

        case FixKind::hum:
        {
            const auto* profile = findProfile (tracks, finding.trackId);
            if (profile == nullptr || profile->humHz <= 0.0f)
                break;

            std::vector<int> lines;
            for (int h = 0; h < Profile::humHarmonics; ++h)
                if (profile->humHarmonicsDb[(size_t) h] >= 10.0f)
                    lines.push_back (h);
            std::sort (lines.begin(), lines.end(), [&] (int a, int b) { return profile->humHarmonicsDb[(size_t) a] > profile->humHarmonicsDb[(size_t) b]; });
            if (lines.size() > (size_t) fx::ParametricEq::numBells)
                lines.resize ((size_t) fx::ParametricEq::numBells);
            std::sort (lines.begin(), lines.end());

            MixMove move;
            move.trackId = finding.trackId;
            move.insert = fx::ModuleType::paraEq;
            // A second EQ when the track has one already, so the first stays yours.
            move.instance = hasInsert (context, finding.trackId, fx::ModuleType::paraEq) ? 1 : 0;
            move.values = { { "peqOn", 1.0f }, { "peqLowCutOn", 0.0f }, { "peqHighCutOn", 0.0f }, { "peqLowGain", 0.0f }, { "peqHighGain", 0.0f } };
            juce::StringArray at;
            for (int b = 0; b < fx::ParametricEq::numBells; ++b)
            {
                const auto prefix = "peqBell" + juce::String (b + 1);
                if ((size_t) b < lines.size())
                {
                    const auto h = lines[(size_t) b];
                    const auto hz = profile->humHz * (float) (h + 1);
                    move.values[prefix + "Freq"] = hz;
                    move.values[prefix + "Gain"] = -(float) roundTo (juce::jlimit (6.0f, 18.0f, profile->humHarmonicsDb[(size_t) h] + 3.0f), 1.0);
                    move.values[prefix + "Q"] = (float) roundTo (juce::jlimit (4.0f, 18.0f, hz / 6.0f), 0.1);
                    at.add (formatHz (hz));
                }
                else
                    move.values[prefix + "Gain"] = 0.0f;
            }
            move.reasons.add ("Hum: narrow cuts at " + at.joinIntoString (", ") + " (" + juce::String (profile->humHz, 0) + " Hz mains)");
            plan.moves.push_back (move);
            break;
        }

        case FixKind::deEss:
        {
            const auto* profile = findProfile (tracks, finding.trackId);
            if (profile == nullptr)
                break;
            const auto sibilance = measureSibilance (*profile);
            MixMove move;
            move.trackId = finding.trackId;
            move.insert = fx::ModuleType::deEsser;
            const auto hz = sibilance.band >= 0 ? roundTo (juce::jlimit (3000.0, 10000.0, getBandCentre (sibilance.band)), 10.0) : 6500.0;
            move.values = { { "deessOn", 1.0f }, { "deessThreshold", (float) roundTo (sibilance.p75Level, 1.0) }, { "deessFreq", (float) hz } };
            move.reasons.add ("De-esser from " + juce::String (juce::roundToInt (sibilance.p75Level)) + " dB at " + formatHz (hz) + ": only the S's go down");
            plan.moves.push_back (move);
            break;
        }

        case FixKind::voiceLevel:
        {
            const auto current = context.volumes.count (finding.trackId) > 0 ? context.volumes.at (finding.trackId) : 0.0f;
            const auto change = juce::jlimit (-8.0f, 8.0f, (float) roundTo ((double) finding.target - (double) finding.value, 0.5));
            MixMove move;
            move.trackId = finding.trackId;
            move.values = { { "volume", current + change } };
            move.reasons.add ("Fader " + juce::String (change > 0 ? "up " : "down ") + formatDb (change) + ": the voice " + (change > 0 ? "comes forward" : "sits back into the mix"));
            plan.moves.push_back (move);
            break;
        }
    }

    return plan;
}

//==============================================================================
juce::String reportToText (const Report& report, const std::map<int, juce::String>& trackNames)
{
    juce::ignoreUnused (trackNames);
    juce::String text;
    text << "MIX CHECK\n=========\n\n";
    if (report.empty)
    {
        text << "The mix is silent: nothing was checked.\n";
        return text;
    }

    text << "Score: " << report.score << " / 100\n"
         << "Loudness: " << juce::String (report.lufs, 1) << " LUFS (target " << juce::String (report.targetLufs, 0) << ")\n"
         << "True peak: " << juce::String (report.truePeakDb, 1) << " dBTP\n"
         << "Loudness range: " << juce::String (report.loudnessRange, 1) << " LU\n"
         << "Peak to loudness: " << juce::String (report.plr, 1) << " dB\n"
         << "Length: " << formatTime (report.seconds) << ", " << report.tracksChecked << " tracks\n\n";

    if (report.findings.empty())
        text << "Nothing to fix: the mix passes every check.\n";

    static const char* const labels[] = { "NOTE", "WARNING", "PROBLEM" };
    int n = 1;
    for (const auto& f : report.findings)
    {
        text << n++ << ". [" << labels[juce::jlimit (0, 2, f.severity)] << "] " << f.title << "\n   " << f.detail << "\n   Why it matters: " << f.why << "\n";
        if (f.from > 0.0 || f.to > 0.0)
            text << "   Where: " << formatTime (f.from) << " - " << formatTime (f.to) << "\n";
        text << "\n";
    }
    return text;
}
}
