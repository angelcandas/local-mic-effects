#include "MixBench.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include "daw/Edit.h"
#include "daw/Exporter.h"

#include <cmath>
#include <numeric>

namespace analysis::bench
{
namespace ids = daw::ids;

namespace
{
constexpr float silenceDb = -200.0f;

juce::AudioFormatManager& getFormats()
{
    struct Formats : juce::AudioFormatManager
    {
        Formats() { registerBasicFormats(); }
    };
    static Formats formats;
    return formats;
}

bool isAudioFile (const juce::File& file)
{
    return file.existsAsFile() && getFormats().findFormatForFileExtension (file.getFileExtension()) != nullptr;
}

double toPower (float db) { return db <= -199.0f ? 0.0 : std::pow (10.0, db / 10.0); }
float toDb (double power) { return power > 1.0e-20 ? (float) (10.0 * std::log10 (power)) : silenceDb; }

int toneLowBand() { return findBand (40.0); }
int toneHighBand() { return findBand (16000.0); }

// The band range the contributions are fitted on (where every track's energy is measured well).
int fitLowBand() { return findBand (50.0); }
int fitHighBand() { return findBand (12500.0); }

// K-weighted momentary power per hop (0 when silent).
std::vector<double> getPowers (const Profile& profile)
{
    std::vector<double> powers ((size_t) profile.getNumFrames());
    for (size_t i = 0; i < powers.size(); ++i)
        powers[i] = toPower (profile.momentaryLufs[i]);
    return powers;
}

juce::String quoteCsv (const juce::String& s)
{
    return s.containsAnyOf (",\"\n") ? "\"" + s.replace ("\"", "\"\"") + "\"" : s;
}

juce::String num (float value, int decimals = 1)
{
    return std::isfinite (value) && value > -199.0f ? juce::String (value, decimals) : juce::String();
}

juce::String signedNum (float value, int decimals = 1)
{
    return (value > 0.0f ? "+" : "") + juce::String (value, decimals);
}
}

//==============================================================================
std::optional<MixStyle> getStyleFromKey (const juce::String& key)
{
    const auto k = key.trim().toLowerCase().removeCharacters (" -_&/");
    for (const auto style : allStyles)
        if (k == getStyleKey (style) || k == getStyleName (style).toLowerCase().removeCharacters (" -_&/"))
            return style;
    if (k == "hiphop" || k == "rap") return MixStyle::hipHop;
    if (k == "randb" || k == "soul") return MixStyle::rnb;
    if (k == "folk" || k == "country") return MixStyle::acoustic;
    if (k == "edm" || k == "dance") return MixStyle::electronic;
    if (k == "metal" || k == "punk") return MixStyle::rock;
    return std::nullopt;
}

juce::String getStyleKey (MixStyle style)
{
    switch (style)
    {
        case MixStyle::pop:        return "pop";
        case MixStyle::ballad:     return "ballad";
        case MixStyle::rock:       return "rock";
        case MixStyle::coaching:   return "coaching";
        case MixStyle::hipHop:     return "hiphop";
        case MixStyle::rnb:        return "rnb";
        case MixStyle::acoustic:   return "acoustic";
        case MixStyle::electronic: return "electronic";
        case MixStyle::jazz:       return "jazz";
        case MixStyle::lofi:       return "lofi";
        case MixStyle::speech:     return "speech";
    }
    return "pop";
}

std::optional<Song> loadSong (const juce::File& folder, juce::String& error)
{
    Song song;
    song.folder = folder;
    song.name = folder.getFileName();

    const auto tracksFolder = folder.getChildFile ("tracks");
    const auto source = tracksFolder.isDirectory() ? tracksFolder : folder;
    auto files = source.findChildFiles (juce::File::findFiles, false);
    files.sort();

    for (const auto& file : files)
    {
        if (! isAudioFile (file))
            continue;
        if (file.getFileNameWithoutExtension().equalsIgnoreCase ("mix"))
            song.mix = file;
        else if (source == tracksFolder || ! file.getFileNameWithoutExtension().startsWithIgnoreCase ("ours-"))
            song.tracks.add (file);
    }

    if (song.mix == juce::File())
        for (const auto& file : folder.findChildFiles (juce::File::findFiles, false))
            if (isAudioFile (file) && file.getFileNameWithoutExtension().equalsIgnoreCase ("mix"))
                song.mix = file;

    const auto info = folder.getChildFile ("song.txt");
    if (info.existsAsFile())
    {
        juce::StringArray lines;
        lines.addLines (info.loadFileAsString());
        for (const auto& line : lines)
        {
            if (line.trim().startsWith ("#") || ! line.contains ("="))
                continue;
            const auto key = line.upToFirstOccurrenceOf ("=", false, false).trim();
            const auto value = line.fromFirstOccurrenceOf ("=", false, false).trim();
            if (key.equalsIgnoreCase ("style"))
                song.style = getStyleFromKey (value);
            else if (key.equalsIgnoreCase ("tempo"))
                song.tempo = juce::jlimit (40.0, 240.0, value.getDoubleValue());
            else if (key.equalsIgnoreCase ("name"))
                song.name = value;
            else if (key.startsWithIgnoreCase ("role."))
                song.roleOverrides[key.fromFirstOccurrenceOf (".", false, false).trim()] = getRoleFromKey (value);
        }
    }

    if (song.tracks.isEmpty())
    {
        error = folder.getFileName() + ": no audio tracks";
        return std::nullopt;
    }
    return song;
}

std::vector<Song> findSongs (const juce::File& root, juce::StringArray& errors)
{
    std::vector<Song> songs;
    juce::String error;

    if (root.getChildFile ("tracks").isDirectory() || root.getChildFile ("song.txt").existsAsFile())
    {
        if (auto song = loadSong (root, error))
            songs.push_back (std::move (*song));
        else
            errors.add (error);
        return songs;
    }

    auto folders = root.findChildFiles (juce::File::findDirectories, false);
    folders.sort();
    for (const auto& folder : folders)
    {
        if (folder.getFileName().startsWith (".") || folder.getFileName() == "zips")
            continue;
        if (auto song = loadSong (folder, error))
            songs.push_back (std::move (*song));
        else
            errors.add (error);
    }
    return songs;
}

//==============================================================================
Features measureFeatures (const Profile& profile, float balanceDb)
{
    Features f;
    f.balanceDb = balanceDb;
    f.lufs = profile.integratedLufs;

    const auto frames = profile.getNumFrames();
    const auto powers = getPowers (profile);

    // Loudness range: short-term (3 s = 30 hops of momentary power), gated at -70 LUFS
    // and 20 LU under the gated mean, 10th to 95th percentile.
    std::vector<float> shortTerm;
    double sum = 0.0;
    for (int i = 0; i < frames; ++i)
    {
        sum += powers[(size_t) i];
        if (i >= 30)
            sum -= powers[(size_t) (i - 30)];
        if (i >= 29)
            if (const auto db = toDb (std::max (0.0, sum) / 30.0); db > -70.0f)
                shortTerm.push_back (db);
    }
    if (shortTerm.size() > 2)
    {
        double mean = 0.0;
        for (const auto v : shortTerm)
            mean += toPower (v);
        const auto gate = toDb (mean / (double) shortTerm.size()) - 20.0f;
        std::vector<float> gated;
        for (const auto v : shortTerm)
            if (v > gate)
                gated.push_back (v);
        std::sort (gated.begin(), gated.end());
        if (gated.size() > 2)
            f.lra = gated[(size_t) std::lround (0.95 * (double) (gated.size() - 1))]
                  - gated[(size_t) std::lround (0.10 * (double) (gated.size() - 1))];
    }

    // Crest: sample peak over the RMS of the active hops.
    double rms = 0.0;
    int active = 0;
    for (int i = 0; i < frames; ++i)
        if (profile.active[(size_t) i])
            rms += toPower (profile.rmsDb[(size_t) i]), ++active;
    if (active > 0)
        f.crestDb = profile.peakDb - toDb (rms / active);

    // Width and correlation over the active hops.
    if (profile.stereo && ! profile.stereoHops.empty())
    {
        double mid = 0.0, side = 0.0, correlation = 0.0;
        int n = 0;
        for (size_t i = 0; i < profile.stereoHops.size() && i < profile.active.size(); ++i)
        {
            if (! profile.active[i])
                continue;
            const auto& hop = profile.stereoHops[i];
            for (int b = 0; b < Profile::stereoBands; ++b)
                mid += hop.mid[(size_t) b], side += hop.side[(size_t) b];
            correlation += hop.correlation;
            ++n;
        }
        if (n > 0)
        {
            f.widthDb = mid > 0.0 ? toDb (side / mid) : silenceDb;
            f.correlation = (float) (correlation / n);
        }
    }

    // Tonal balance, loudness matched: the mean over 40 Hz-16 kHz is 0 dB.
    auto tone = profile.getAverageSpectrum();
    double mean = 0.0;
    int bands = 0;
    for (int b = toneLowBand(); b <= toneHighBand(); ++b)
        mean += tone[(size_t) b], ++bands;
    mean /= juce::jmax (1, bands);
    for (auto& v : tone)
        v = v <= -199.0f ? silenceDb : v - (float) mean;
    f.tone = tone;
    return f;
}

float measureBalanceDb (const juce::File& file)
{
    std::unique_ptr<juce::AudioFormatReader> reader (getFormats().createReaderFor (file));
    if (reader == nullptr || reader->numChannels < 2)
        return 0.0f;

    constexpr int blockSize = 1 << 15;
    juce::AudioBuffer<float> buffer (2, blockSize);
    double left = 0.0, right = 0.0;
    for (juce::int64 start = 0; start < reader->lengthInSamples; start += blockSize)
    {
        const auto count = (int) juce::jmin ((juce::int64) blockSize, reader->lengthInSamples - start);
        reader->read (&buffer, 0, count, start, true, true);
        for (int i = 0; i < count; ++i)
        {
            left += (double) buffer.getSample (0, i) * buffer.getSample (0, i);
            right += (double) buffer.getSample (1, i) * buffer.getSample (1, i);
        }
    }
    return left > 0.0 && right > 0.0 ? (float) (10.0 * std::log10 (left / right)) : 0.0f;
}

float getToneDistanceDb (const Bands& a, const Bands& b)
{
    double sum = 0.0;
    int n = 0;
    for (int i = toneLowBand(); i <= toneHighBand(); ++i)
        if (a[(size_t) i] > -199.0f && b[(size_t) i] > -199.0f)
            sum += std::abs (a[(size_t) i] - b[(size_t) i]), ++n;
    return n > 0 ? (float) (sum / n) : 0.0f;
}

float getToneTiltDbPerOctave (const Bands& a, const Bands& b)
{
    // Least-squares slope of (a - b) against log2 of the band centre.
    double sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0;
    int n = 0;
    for (int i = toneLowBand(); i <= toneHighBand(); ++i)
    {
        if (a[(size_t) i] <= -199.0f || b[(size_t) i] <= -199.0f)
            continue;
        const auto x = std::log2 (getBandCentre (i) / 1000.0);
        const auto y = (double) (a[(size_t) i] - b[(size_t) i]);
        sx += x, sy += y, sxx += x * x, sxy += x * y, ++n;
    }
    const auto d = n * sxx - sx * sx;
    return n > 2 && d > 0.0 ? (float) ((n * sxy - sx * sy) / d) : 0.0f;
}

//==============================================================================
Contributions estimateContributions (const std::vector<std::pair<int, Profile>>& tracks, const Profile& mix)
{
    Contributions result;
    const auto n = tracks.size();
    for (const auto& [id, profile] : tracks)
        result.gainDb[id] = silenceDb;
    if (n == 0 || mix.getNumFrames() == 0)
        return result;

    // Lag: the mix's loudness envelope against the plain sum of the tracks', ±10 s.
    int frames = 0;
    for (const auto& [id, profile] : tracks)
        frames = juce::jmax (frames, profile.getNumFrames());

    std::vector<double> sumEnvelope ((size_t) frames, 0.0);
    for (const auto& [id, profile] : tracks)
        for (int i = 0; i < profile.getNumFrames(); ++i)
            sumEnvelope[(size_t) i] += toPower (profile.momentaryLufs[(size_t) i]);

    std::vector<float> a ((size_t) frames), m ((size_t) mix.getNumFrames());
    for (size_t i = 0; i < a.size(); ++i)
        a[i] = juce::jmax (-70.0f, toDb (sumEnvelope[i]));
    for (size_t i = 0; i < m.size(); ++i)
        m[i] = juce::jmax (-70.0f, mix.momentaryLufs[i]);

    const auto correlate = [&] (int lag)
    {
        double sa = 0.0, sm = 0.0, saa = 0.0, smm = 0.0, sam = 0.0;
        int count = 0;
        for (int i = 0; i < frames; ++i)
        {
            const auto j = i + lag;
            if (j < 0 || j >= (int) m.size())
                continue;
            const double x = a[(size_t) i], y = m[(size_t) j];
            sa += x, sm += y, saa += x * x, smm += y * y, sam += x * y, ++count;
        }
        if (count < 50)
            return -1.0;
        const auto cov = sam - sa * sm / count;
        const auto va = saa - sa * sa / count, vm = smm - sm * sm / count;
        return va > 0.0 && vm > 0.0 ? cov / std::sqrt (va * vm) : -1.0;
    };

    double best = -2.0;
    for (int lag = -100; lag <= 100; ++lag)
        if (const auto c = correlate (lag); c > best + 1.0e-9)
            best = c, result.lagHops = lag;

    // Normal equations of the relative error: each (hop, band) row is a_i / y with target 1.
    std::vector<double> gram (n * n, 0.0), rhs (n, 0.0), scale (n, 0.0);
    std::vector<double> row (n);
    std::vector<std::vector<double>> sampleRows; // every 7th row, for the fit error
    int used = 0;

    for (int i = 0; i < frames; ++i)
    {
        const auto j = i + result.lagHops;
        if (j < 0 || j >= mix.getNumFrames() || ! mix.active[(size_t) j])
            continue;

        for (int b = fitLowBand(); b <= fitHighBand(); ++b)
        {
            const auto y = toPower (mix.bandsDb[(size_t) j][(size_t) b]);
            if (y <= 1.0e-12)
                continue;

            bool any = false;
            for (size_t t = 0; t < n; ++t)
            {
                const auto& p = tracks[t].second;
                row[t] = i < p.getNumFrames() ? toPower (p.bandsDb[(size_t) i][(size_t) b]) / y : 0.0;
                any = any || row[t] > 0.0;
            }
            if (! any)
                continue;

            for (size_t r = 0; r < n; ++r)
            {
                if (row[r] == 0.0)
                    continue;
                rhs[r] += row[r];
                for (size_t c = 0; c < n; ++c)
                    gram[r * n + c] += row[r] * row[c];
            }
            if ((used++ % 7) == 0)
                sampleRows.push_back (row);
        }
    }

    if (used == 0)
        return result;

    // Column scaling so every track's gain is of order 1, then nonnegative coordinate descent.
    for (size_t t = 0; t < n; ++t)
        scale[t] = gram[t * n + t] > 0.0 ? 1.0 / std::sqrt (gram[t * n + t]) : 0.0;

    std::vector<double> g (n, 0.0);
    for (size_t t = 0; t < n; ++t)
        g[t] = scale[t] > 0.0 ? 1.0 : 0.0;

    for (int iteration = 0; iteration < 500; ++iteration)
    {
        double change = 0.0;
        for (size_t r = 0; r < n; ++r)
        {
            if (scale[r] == 0.0)
                continue;
            const auto diagonal = gram[r * n + r] * scale[r] * scale[r]; // 1
            double s = rhs[r] * scale[r];
            for (size_t c = 0; c < n; ++c)
                if (c != r)
                    s -= gram[r * n + c] * scale[r] * scale[c] * g[c];
            const auto next = juce::jmax (0.0, s / diagonal);
            change = juce::jmax (change, std::abs (next - g[r]));
            g[r] = next;
        }
        if (change < 1.0e-7)
            break;
    }

    std::vector<double> gains (n);
    for (size_t t = 0; t < n; ++t)
    {
        gains[t] = g[t] * scale[t];
        result.gainDb[tracks[t].first] = gains[t] > 1.0e-10 ? (float) (10.0 * std::log10 (gains[t])) : silenceDb;
    }

    double error = 0.0;
    int counted = 0;
    for (const auto& r : sampleRows)
    {
        double model = 0.0;
        for (size_t t = 0; t < n; ++t)
            model += gains[t] * r[t];
        const auto e = 10.0 * std::log10 (juce::jmax (1.0e-6, model));
        error += e * e, ++counted;
    }
    result.fitErrorDb = counted > 0 ? (float) std::sqrt (error / counted) : 0.0f;
    return result;
}

RoleLevels measureRoleLevels (const std::vector<std::pair<int, Profile>>& tracks, const std::map<int, Role>& roles,
                              const Contributions& fit, const Profile& mix)
{
    RoleLevels levels;
    int frames = 0;
    for (const auto& [id, profile] : tracks)
        frames = juce::jmax (frames, profile.getNumFrames());

    // Each role's contribution to the mix per hop (K-weighted power, on the tracks' time grid).
    std::map<Role, std::vector<double>> byRole;
    std::vector<double> total ((size_t) frames, 0.0);
    for (const auto& [id, profile] : tracks)
    {
        const auto gainIt = fit.gainDb.find (id);
        if (gainIt == fit.gainDb.end() || gainIt->second <= -199.0f)
            continue;
        const auto roleIt = roles.find (id);
        const auto role = roleIt != roles.end() ? roleIt->second : Role::other;
        auto& sum = byRole[role];
        sum.resize ((size_t) frames, 0.0);
        const auto gain = toPower (gainIt->second);
        for (int i = 0; i < profile.getNumFrames(); ++i)
        {
            const auto p = gain * toPower (profile.momentaryLufs[(size_t) i]);
            sum[(size_t) i] += p;
            total[(size_t) i] += p;
        }
    }

    const auto mixPower = [&] (int i)
    {
        const auto j = i + fit.lagHops;
        return j >= 0 && j < mix.getNumFrames() ? toPower (mix.momentaryLufs[(size_t) j]) : 0.0;
    };

    for (const auto& [role, sum] : byRole)
    {
        // The hops where the role plays: within 30 dB of its loudest short stretch.
        const auto loudest = *std::max_element (sum.begin(), sum.end());
        if (loudest <= 0.0)
            continue;
        double rolePower = 0.0, mixSum = 0.0;
        for (int i = 0; i < frames; ++i)
            if (sum[(size_t) i] > loudest * 1.0e-3)
                rolePower += sum[(size_t) i], mixSum += mixPower (i);
        if (rolePower > 0.0 && mixSum > 0.0)
            levels.roleLu[role] = (float) (10.0 * std::log10 (rolePower / mixSum));
    }

    if (const auto voice = byRole.find (Role::voice); voice != byRole.end())
    {
        const auto loudest = *std::max_element (voice->second.begin(), voice->second.end());
        double v = 0.0, rest = 0.0;
        for (int i = 0; i < frames; ++i)
            if (voice->second[(size_t) i] > loudest * 1.0e-3)
                v += voice->second[(size_t) i], rest += total[(size_t) i] - voice->second[(size_t) i];
        if (v > 0.0 && rest > 0.0)
        {
            levels.voiceOverBackingLu = (float) (10.0 * std::log10 (v / rest));
            levels.hasVoice = true;
        }
    }
    return levels;
}

//==============================================================================
float Run::getVoiceOverBackingErrorLu() const
{
    return oursLevels.hasVoice && humanLevels.hasVoice
               ? oursLevels.voiceOverBackingLu - humanLevels.voiceOverBackingLu
               : 0.0f;
}

float Run::getRoleErrorLu() const
{
    double sum = 0.0;
    int n = 0;
    for (const auto& [role, level] : oursLevels.roleLu)
        if (const auto it = humanLevels.roleLu.find (role); it != humanLevels.roleLu.end())
            sum += std::abs (level - it->second), ++n;
    return n > 0 ? (float) (sum / n) : 0.0f;
}

std::optional<Role> Run::getWorstRole (float& errorLu) const
{
    std::optional<Role> worst;
    errorLu = 0.0f;
    for (const auto& [role, level] : oursLevels.roleLu)
        if (const auto it = humanLevels.roleLu.find (role); it != humanLevels.roleLu.end())
            if (std::abs (level - it->second) > std::abs (errorLu))
                errorLu = level - it->second, worst = role;
    return worst;
}

namespace
{
// A track's audio has a real stereo image: two channels that differ (checked on up to 20 s
// from the middle of the file).
bool fileHasStereoImage (const juce::File& file)
{
    std::unique_ptr<juce::AudioFormatReader> reader (getFormats().createReaderFor (file));
    if (reader == nullptr || reader->numChannels < 2)
        return false;
    const auto length = (int) juce::jmin ((juce::int64) (20.0 * reader->sampleRate), reader->lengthInSamples);
    juce::AudioBuffer<float> buffer (2, length);
    reader->read (&buffer, 0, length, (reader->lengthInSamples - length) / 2, true, true);
    return hasStereoImage (buffer);
}

double getFileSeconds (const juce::File& file)
{
    std::unique_ptr<juce::AudioFormatReader> reader (getFormats().createReaderFor (file));
    return reader != nullptr && reader->sampleRate > 0.0 ? (double) reader->lengthInSamples / reader->sampleRate : 0.0;
}
}

Run runSong (const Song& song, MixStyle style, const juce::File& outFolder, const Options& options)
{
    Run run;
    run.song = song.name;
    run.style = getStyleKey (style);
    const auto started = juce::Time::getMillisecondCounterHiRes();
    const auto log = [&] (const juce::String& line) { if (options.log) options.log (line); };

    daw::Edit edit;
    edit.setTempo (song.tempo);

    MixOptions mixOptions;
    mixOptions.tempo = song.tempo;
    std::map<int, Role> knownRoles;
    std::map<int, juce::String> fileNames;

    for (const auto& file : song.tracks)
    {
        const auto seconds = getFileSeconds (file);
        if (seconds <= 0.0)
            continue;
        const auto name = file.getFileNameWithoutExtension();
        const auto id = (int) edit.addTrack (name)[ids::id];
        edit.addClip (id, file, 0.0, seconds);
        run.seconds = juce::jmax (run.seconds, seconds);
        run.trackNames[id] = name;
        fileNames[id] = name;
        mixOptions.trackNames[id] = name;
        if (fileHasStereoImage (file))
            mixOptions.stereoTracks.insert (id);
        mixOptions.currentPhase[id] = { false, 0.0f };

        if (const auto it = song.roleOverrides.find (name); it != song.roleOverrides.end())
            knownRoles[id] = it->second;
        else if (const auto byName = guessRoleFromName (name))
            knownRoles[id] = *byName;
    }
    run.numTracks = (int) run.trackNames.size();
    if (run.numTracks == 0)
    {
        run.error = "no readable tracks";
        return run;
    }

    log ("  listening to " + juce::String (run.numTracks) + " tracks (" + juce::String (run.seconds, 0) + " s)");
    const auto analysis = SessionAnalyser (edit, SessionAnalyser::Point::source, false, options.sampleRate).run();
    if (analysis.cancelled)
    {
        run.error = "analysis cancelled";
        return run;
    }

    std::map<int, float> volumes;
    for (const auto& [id, profile] : analysis.tracks)
    {
        if (const auto it = song.roleOverrides.find (fileNames[id]); it != song.roleOverrides.end())
            run.roles[id] = it->second;
        else
            run.roles[id] = guessRole (fileNames[id], profile, run.seconds);
        volumes[id] = 0.0f;
    }

    if (options.alignments)
        mixOptions.alignments = measureAlignments (edit.getState().createCopy(), knownRoles);

    run.plan = planMix (analysis, run.roles, volumes, 0.0f, style, mixOptions);
    run.numMoves = (int) run.plan.moves.size();
    applyMix (edit, run.plan, options.amount);

    outFolder.createDirectory();
    run.rendered = outFolder.getChildFile (juce::File::createLegalFileName (song.name) + "-" + run.style + ".wav");
    log ("  rendering " + run.rendered.getFileName());

    daw::Exporter::Settings settings;
    settings.sampleRate = options.sampleRate;
    settings.bitDepth = 24;
    settings.tail = 0.0;
    const auto exported = daw::Exporter (edit, settings).exportMix (run.rendered);
    if (exported.error.isNotEmpty())
    {
        run.error = "render failed: " + exported.error;
        return run;
    }

    const auto oursProfile = analyseFile (run.rendered);
    run.ours = measureFeatures (oursProfile, measureBalanceDb (run.rendered));
    run.oursFit = estimateContributions (analysis.tracks, oursProfile);
    run.oursLevels = measureRoleLevels (analysis.tracks, run.roles, run.oursFit, oursProfile);

    // The same numbers measured exactly, from each track as it plays in our mix (through its
    // inserts, volume and rides): how far the fit is from the truth on a mix we know.
    {
        const auto stems = SessionAnalyser (edit, SessionAnalyser::Point::stem, true, options.sampleRate).run();
        if (! stems.cancelled && stems.mix.has_value())
        {
            Contributions unity;
            for (const auto& [id, profile] : stems.tracks)
                unity.gainDb[id] = 0.0f;
            run.oursExactLevels = measureRoleLevels (stems.tracks, run.roles, unity, *stems.mix);
        }
    }

    if (song.mix.existsAsFile())
    {
        log ("  scoring against " + song.mix.getFileName());
        const auto humanProfile = analyseFile (song.mix);
        if (humanProfile.getNumFrames() > 0)
        {
            run.hasHuman = true;
            run.human = measureFeatures (humanProfile, measureBalanceDb (song.mix));
            run.humanFit = estimateContributions (analysis.tracks, humanProfile);
            run.humanLevels = measureRoleLevels (analysis.tracks, run.roles, run.humanFit, humanProfile);
        }
    }

    run.elapsed = (juce::Time::getMillisecondCounterHiRes() - started) / 1000.0;
    return run;
}

//==============================================================================
juce::String getCsvHeader()
{
    juce::StringArray columns { "song", "style", "error", "tracks", "seconds", "elapsed", "moves",
                                "lufs_ours", "lufs_human", "lra_ours", "lra_human", "crest_ours", "crest_human",
                                "width_ours", "width_human", "corr_ours", "corr_human", "balance_ours", "balance_human",
                                "tone_distance_db", "tone_tilt_db_oct", "vob_ours", "vob_ours_exact", "vob_human", "vob_error",
                                "role_error_lu", "fit_ours_db", "fit_human_db", "lag_human_s" };
    for (const auto role : allRoles)
    {
        columns.add (juce::String ("lu_") + getRoleKey (role) + "_ours");
        columns.add (juce::String ("lu_") + getRoleKey (role) + "_human");
    }
    return columns.joinIntoString (",");
}

juce::String toCsvRow (const Run& run)
{
    juce::StringArray cells { quoteCsv (run.song), run.style, quoteCsv (run.error), juce::String (run.numTracks),
                              juce::String (run.seconds, 1), juce::String (run.elapsed, 1), juce::String (run.numMoves) };
    const auto pair = [&] (float ours, float human, int decimals = 1)
    {
        cells.add (num (ours, decimals));
        cells.add (run.hasHuman ? num (human, decimals) : juce::String());
    };
    pair (run.ours.lufs, run.human.lufs);
    pair (run.ours.lra, run.human.lra);
    pair (run.ours.crestDb, run.human.crestDb);
    pair (run.ours.widthDb, run.human.widthDb);
    pair (run.ours.correlation, run.human.correlation, 2);
    pair (run.ours.balanceDb, run.human.balanceDb, 2);
    cells.add (run.hasHuman ? num (run.getToneDistanceDb(), 2) : juce::String());
    cells.add (run.hasHuman ? num (run.getToneTiltDbPerOctave(), 2) : juce::String());
    cells.add (run.oursLevels.hasVoice ? num (run.oursLevels.voiceOverBackingLu) : juce::String());
    cells.add (run.oursExactLevels.hasVoice ? num (run.oursExactLevels.voiceOverBackingLu) : juce::String());
    cells.add (run.humanLevels.hasVoice ? num (run.humanLevels.voiceOverBackingLu) : juce::String());
    cells.add (run.oursLevels.hasVoice && run.humanLevels.hasVoice ? num (run.getVoiceOverBackingErrorLu()) : juce::String());
    cells.add (run.hasHuman ? num (run.getRoleErrorLu()) : juce::String());
    cells.add (num (run.oursFit.fitErrorDb, 2));
    cells.add (run.hasHuman ? num (run.humanFit.fitErrorDb, 2) : juce::String());
    cells.add (run.hasHuman ? juce::String (run.humanFit.lagHops * Profile::hopSeconds, 1) : juce::String());
    for (const auto role : allRoles)
    {
        const auto o = run.oursLevels.roleLu.find (role);
        const auto h = run.humanLevels.roleLu.find (role);
        cells.add (o != run.oursLevels.roleLu.end() ? num (o->second) : juce::String());
        cells.add (h != run.humanLevels.roleLu.end() ? num (h->second) : juce::String());
    }
    return cells.joinIntoString (",");
}

juce::String summarise (const std::vector<Run>& runs)
{
    juce::String md;
    md << "| Song | Style | Loudness ours / human (LUFS) | Tone distance (dB) | Tilt (dB/oct) | Voice over backing ours / human (LU) | Role error (LU) | Worst role | Width ours / human (dB) |\n"
       << "|---|---|---|---|---|---|---|---|---|\n";

    struct Totals { double tone = 0, tilt = 0, vob = 0, role = 0; int n = 0, nVob = 0; };
    std::map<juce::String, Totals> byStyle;

    for (const auto& run : runs)
    {
        if (run.error.isNotEmpty())
        {
            md << "| " << run.song << " | " << run.style << " | failed: " << run.error << " | | | | | | |\n";
            continue;
        }
        if (! run.hasHuman)
        {
            md << "| " << run.song << " | " << run.style << " | " << num (run.ours.lufs) << " / no human mix | | | | | | |\n";
            continue;
        }

        float worstError = 0.0f;
        const auto worst = run.getWorstRole (worstError);
        const auto vob = run.oursLevels.hasVoice && run.humanLevels.hasVoice;
        md << "| " << run.song << " | " << run.style
           << " | " << num (run.ours.lufs) << " / " << num (run.human.lufs)
           << " | " << juce::String (run.getToneDistanceDb(), 1)
           << " | " << signedNum (run.getToneTiltDbPerOctave(), 2)
           << " | " << (vob ? num (run.oursLevels.voiceOverBackingLu) + " / " + num (run.humanLevels.voiceOverBackingLu) : juce::String ("no voice"))
           << " | " << juce::String (run.getRoleErrorLu(), 1)
           << " | " << (worst.has_value() ? getRoleName (*worst) + " " + signedNum (worstError) : juce::String ("-"))
           << " | " << num (run.ours.widthDb) << " / " << num (run.human.widthDb) << " |\n";

        auto& t = byStyle[run.style];
        t.tone += run.getToneDistanceDb();
        t.tilt += run.getToneTiltDbPerOctave();
        t.role += run.getRoleErrorLu();
        ++t.n;
        if (vob)
            t.vob += run.getVoiceOverBackingErrorLu(), ++t.nVob;
    }

    if (! byStyle.empty())
    {
        md << "\n| Style | Songs | Tone distance (dB) | Tilt (dB/oct) | Voice over backing, ours minus human (LU) | Role error (LU) |\n"
           << "|---|---|---|---|---|---|\n";
        for (const auto& [style, t] : byStyle)
            md << "| " << style << " | " << t.n << " | " << juce::String (t.tone / t.n, 1) << " | " << signedNum ((float) (t.tilt / t.n), 2)
               << " | " << (t.nVob > 0 ? signedNum ((float) (t.vob / t.nVob)) : juce::String ("-")) << " | " << juce::String (t.role / t.n, 1) << " |\n";
    }

    // Role levels against the human mixes, averaged over every run: where Auto Mix is
    // consistently too loud or too quiet.
    std::map<Role, std::pair<double, int>> roleBias;
    for (const auto& run : runs)
        if (run.hasHuman && run.error.isEmpty())
            for (const auto& [role, level] : run.oursLevels.roleLu)
                if (const auto it = run.humanLevels.roleLu.find (role); it != run.humanLevels.roleLu.end())
                    roleBias[role].first += level - it->second, ++roleBias[role].second;

    if (! roleBias.empty())
    {
        md << "\n| Role | Runs | Ours minus human, average (LU) |\n|---|---|---|\n";
        for (const auto& [role, bias] : roleBias)
            md << "| " << getRoleName (role) << " | " << bias.second << " | " << signedNum ((float) (bias.first / bias.second)) << " |\n";
    }
    return md;
}
}
