#include "ToMidi.h"

#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>

namespace analysis::tomidi
{
namespace
{
    constexpr double twoPi = juce::MathConstants<double>::twoPi;

    float levelDb (const std::vector<float>& x, double sampleRate, double from, double to)
    {
        const auto a = juce::jlimit (0, (int) x.size(), (int) std::floor (from * sampleRate));
        const auto b = juce::jlimit (0, (int) x.size(), (int) std::ceil (to * sampleRate));
        if (b <= a)
            return -120.0f;
        double sum = 0.0;
        for (auto i = a; i < b; ++i)
            sum += (double) x[(size_t) i] * x[(size_t) i];
        return (float) (10.0 * std::log10 (sum / (b - a) + 1.0e-12));
    }

    // Two groups of values: the split between them, when they are clearly apart.
    // Returns false (and leaves `split` alone) when the values look like one group.
    bool findSplit (std::vector<float> values, float minGap, float& split)
    {
        if (values.size() < 2)
            return false;
        std::sort (values.begin(), values.end());
        // 1-D two-means: the cut that leaves the least spread on either side.
        auto best = -1;
        double bestCost = std::numeric_limits<double>::max();
        const auto n = (int) values.size();
        std::vector<double> prefix ((size_t) n + 1, 0.0), squares ((size_t) n + 1, 0.0);
        for (int i = 0; i < n; ++i)
        {
            prefix[(size_t) i + 1] = prefix[(size_t) i] + values[(size_t) i];
            squares[(size_t) i + 1] = squares[(size_t) i] + (double) values[(size_t) i] * values[(size_t) i];
        }
        const auto cost = [&] (int a, int b) // spread of values[a, b)
        {
            const auto count = b - a;
            const auto sum = prefix[(size_t) b] - prefix[(size_t) a];
            return squares[(size_t) b] - squares[(size_t) a] - sum * sum / count;
        };
        for (int cut = 1; cut < n; ++cut)
            if (const auto c = cost (0, cut) + cost (cut, n); c < bestCost)
                bestCost = c, best = cut;
        if (best < 0)
            return false;
        const auto lowMean = prefix[(size_t) best] / best;
        const auto highMean = (prefix[(size_t) n] - prefix[(size_t) best]) / (n - best);
        if (highMean - lowMean < minGap)
            return false;
        split = (float) (0.5 * (values[(size_t) best - 1] + values[(size_t) best]));
        return true;
    }

    int velocityFrom (float db, float loudest)
    {
        // The loudest hit or note of its kind plays at 116, 30 dB under it at 30.
        const auto below = juce::jlimit (-30.0f, 0.0f, db - loudest);
        return juce::jlimit (1, 127, (int) std::round (juce::jmap (below, -30.0f, 0.0f, 30.0f, 116.0f)));
    }

    std::vector<float> mixToMono (const juce::AudioBuffer<float>& audio)
    {
        std::vector<float> mono ((size_t) audio.getNumSamples(), 0.0f);
        const auto channels = audio.getNumChannels();
        for (int ch = 0; ch < channels; ++ch)
        {
            const auto* data = audio.getReadPointer (ch);
            for (size_t i = 0; i < mono.size(); ++i)
                mono[i] += data[i] / (float) channels;
        }
        return mono;
    }

    // Steep (24 dB / octave) band filters.
    std::vector<float> filter (const std::vector<float>& in, double sampleRate, double highPass, double lowPass)
    {
        std::vector<float> out (in);
        const auto run = [&] (const juce::IIRCoefficients& c)
        {
            juce::IIRFilter f;
            f.setCoefficients (c);
            f.processSamples (out.data(), (int) out.size());
        };
        for (int stage = 0; stage < 2; ++stage)
        {
            if (highPass > 0.0)
                run (juce::IIRCoefficients::makeHighPass (sampleRate, highPass, 0.7071));
            if (lowPass > 0.0 && lowPass < sampleRate * 0.45)
                run (juce::IIRCoefficients::makeLowPass (sampleRate, lowPass, 0.7071));
        }
        return out;
    }
}

//==============================================================================
double quantiseTime (double time, const std::vector<double>& grid, float strength)
{
    if (grid.empty() || strength <= 0.0f)
        return time;
    const auto it = std::lower_bound (grid.begin(), grid.end(), time);
    auto nearest = it == grid.end() ? grid.back() : *it;
    if (it != grid.begin() && (it == grid.end() || time - *(it - 1) < *it - time))
        nearest = *(it - 1);
    return time + juce::jlimit (0.0f, 1.0f, strength) * (nearest - time);
}

MelodyOptions MelodyOptions::bass()
{
    MelodyOptions o;
    o.fitLow = 28;
    o.fitHigh = 52;
    return o;
}

float getTuningOffset (const std::vector<pitch::Note>& notes)
{
    // The average of each note's distance from its nearest semitone, on a circle (so
    // 0.45 and -0.45 average to 0.5, not to 0).
    double c = 0.0, s = 0.0;
    for (const auto& n : notes)
    {
        if (n.pitch <= 0.0f)
            continue;
        const auto weight = juce::jmax (0.0, n.end - n.start);
        const auto angle = twoPi * (n.pitch - std::round (n.pitch));
        c += weight * std::cos (angle);
        s += weight * std::sin (angle);
    }
    if (std::hypot (c, s) <= 1.0e-9)
        return 0.0f;
    return (float) (std::atan2 (s, c) / twoPi);
}

int snapToKey (float midi, int key, bool minor)
{
    const auto nearest = (int) std::round (midi);
    if (key < 0)
        return nearest;
    static constexpr int majorScale[] = { 0, 2, 4, 5, 7, 9, 11 }, minorScale[] = { 0, 2, 3, 5, 7, 8, 10 };
    const auto inKey = [&] (int note)
    {
        const auto degree = ((note - key) % 12 + 12) % 12;
        const auto& scale = minor ? minorScale : majorScale;
        return std::find (std::begin (scale), std::end (scale), degree) != std::end (scale);
    };
    auto best = nearest;
    auto bestDistance = 100.0f;
    for (auto note = nearest - 2; note <= nearest + 2; ++note)
        if (inKey (note) && std::abs ((float) note - midi) < bestDistance)
            best = note, bestDistance = std::abs ((float) note - midi);
    return best;
}

//==============================================================================
std::vector<Note> melodyToMidi (const pitch::Track& track, const MelodyOptions& options, double from, double to)
{
    std::vector<Note> result;
    const auto sung = pitch::findNotes (track, from, to);
    if (sung.empty())
        return result;

    struct Work
    {
        double start, end;
        float sung;  // MIDI, fractions are cents
        int pitch;
        float level; // dB, the note's loudest moment
    };

    const auto loudestIn = [&] (double a, double b)
    {
        auto level = -120.0f;
        for (auto i = track.frameAt (a); i <= track.frameAt (b); ++i)
            if (track.midi[(size_t) i] > 0.0f)
                level = juce::jmax (level, track.energy[(size_t) i]);
        return level;
    };
    const auto quietestIn = [&] (double a, double b)
    {
        auto level = 0.0f;
        for (auto i = track.frameAt (a); i <= track.frameAt (b); ++i)
            level = juce::jmin (level, track.energy[(size_t) i]);
        return level;
    };

    // Leave out what is much quieter than the line.
    std::vector<Work> notes;
    std::vector<pitch::Note> kept;
    auto loudest = -120.0f;
    for (const auto& n : sung)
        loudest = juce::jmax (loudest, loudestIn (n.start, n.end));
    const auto floor = loudest - (18.0f + 30.0f * juce::jlimit (0.0f, 1.0f, options.sensitivity));
    for (const auto& n : sung)
        if (const auto level = loudestIn (n.start, n.end); level >= floor && n.pitch > 0.0f)
        {
            notes.push_back ({ n.start, n.end, n.pitch, 0, level });
            kept.push_back (n);
        }

    // Onto the notes of the scale (or the semitones), from where the take is tuned.
    const auto offset = options.followTuning ? getTuningOffset (kept) : 0.0f;
    for (auto& n : notes)
        n.pitch = snapToKey (n.sung - offset, options.key, options.minor);

    // One note again where a drift was cut but both halves land on the same note, unless
    // the voice dipped between them (two syllables on one pitch).
    for (size_t k = 1; k < notes.size();)
    {
        auto& a = notes[k - 1];
        const auto& b = notes[k];
        const auto touching = b.start - a.end < 0.025;
        const auto dip = juce::jmin (a.level, b.level) - quietestIn (a.end - 0.02, b.start + 0.02);
        if (touching && a.pitch == b.pitch && dip < 6.0f)
        {
            a.end = b.end;
            a.level = juce::jmax (a.level, b.level);
            notes.erase (notes.begin() + (long) k);
        }
        else
            ++k;
    }

    // Too short: part of the note it touches (the nearer in pitch), or gone.
    for (size_t k = 0; k < notes.size();)
    {
        const auto& n = notes[k];
        if (n.end - n.start >= options.minLength || notes.size() == 1)
        {
            ++k;
            continue;
        }
        const auto touchesBefore = k > 0 && n.start - notes[k - 1].end < 0.025;
        const auto touchesAfter = k + 1 < notes.size() && notes[k + 1].start - n.end < 0.025;
        auto into = -1;
        if (touchesBefore && touchesAfter)
            into = std::abs (notes[k - 1].sung - n.sung) <= std::abs (notes[k + 1].sung - n.sung) ? (int) k - 1 : (int) k + 1;
        else if (touchesBefore)
            into = (int) k - 1;
        else if (touchesAfter)
            into = (int) k + 1;
        if (into >= 0)
        {
            auto& other = notes[(size_t) into];
            other.start = juce::jmin (other.start, n.start);
            other.end = juce::jmax (other.end, n.end);
        }
        notes.erase (notes.begin() + (long) k);
        if (into >= 0 && into < (int) k)
            k = (size_t) into; // look at the grown note again
    }

    // Octaves: the asked ones, then into the range the line should sit in.
    auto shift = 12 * options.octaves;
    if (options.fitHigh > options.fitLow && ! notes.empty())
    {
        std::vector<std::pair<int, double>> weighted;
        double total = 0.0;
        for (const auto& n : notes)
            weighted.emplace_back (n.pitch + shift, n.end - n.start), total += n.end - n.start;
        std::sort (weighted.begin(), weighted.end());
        auto middle = weighted.back().first;
        for (double sum = 0.0; const auto& [p, w] : weighted)
            if ((sum += w) >= total / 2.0) { middle = p; break; }
        const auto centre = 0.5 * (options.fitLow + options.fitHigh);
        auto best = 0;
        for (auto o = -6; o <= 6; ++o)
            if (std::abs (middle + 12 * o - centre) < std::abs (middle + 12 * best - centre))
                best = o;
        shift += 12 * best;
    }

    for (const auto& n : notes)
    {
        Note note;
        note.time = n.start;
        note.length = n.end - n.start;
        note.pitch = juce::jlimit (0, 127, n.pitch + shift);
        note.velocity = velocityFrom (n.level, loudest);
        result.push_back (note);
    }

    // On the grid: starts and ends move, a note keeps at least half a step.
    if (! options.grid.empty() && options.quantise > 0.0f)
    {
        for (auto& n : result)
        {
            const auto start = quantiseTime (n.time, options.grid, options.quantise);
            auto end = quantiseTime (n.time + n.length, options.grid, options.quantise);
            if (end <= start + 1.0e-6)
            {
                // Both went to the same line: to the line after it (scaled by strength).
                const auto next = std::upper_bound (options.grid.begin(), options.grid.end(), start + 1.0e-6);
                const auto step = next != options.grid.end() ? *next - start : n.length;
                end = start + juce::jmax ((double) options.quantise * step, n.length * (1.0 - options.quantise), 0.01);
            }
            n.time = start;
            n.length = end - start;
        }
        // Two notes on one line: the louder stays.
        for (size_t k = 1; k < result.size();)
            if (std::abs (result[k].time - result[k - 1].time) < 1.0e-6)
            {
                if (result[k].velocity > result[k - 1].velocity)
                    result[k - 1] = result[k];
                result.erase (result.begin() + (long) k);
            }
            else
                ++k;
    }

    // One line: a note ends where the next starts.
    for (size_t k = 0; k + 1 < result.size(); ++k)
        result[k].length = juce::jmax (0.01, juce::jmin (result[k].length, result[k + 1].time - result[k].time));

    return result;
}

std::vector<Note> melodyToMidi (const juce::AudioBuffer<float>& audio, double sampleRate, const MelodyOptions& options,
                                const std::function<bool()>& shouldContinue)
{
    const auto track = pitch::analyse (audio, sampleRate, shouldContinue);
    if (shouldContinue && ! shouldContinue())
        return {};
    return melodyToMidi (track, options);
}

//==============================================================================
namespace
{
    // The regions of a kit's spectrum the rules look at.
    enum Region { sub, body, mids, wires, air, numRegions };
    constexpr double regionEdges[numRegions + 1] = { 30.0, 90.0, 400.0, 1500.0, 5000.0, 16000.0 };

    // Attacks in one band: where its level (over the last `window` seconds, every 2 ms)
    // jumps `jump` dB or more over the quietest moment of the 30 ms before, within 45 dB
    // of the band's loudest moment. Unlike rhythm::findOnsets, a soft hit counts as much
    // as a loud one, so a quiet hat after a loud snare is still found. The window holds a
    // few cycles of the band's lowest frequency, so a low drum's level is smooth.
    std::vector<double> findAttacks (const std::vector<float>& signal, double sampleRate, float jump, double window)
    {
        std::vector<double> attacks;
        const auto block = juce::jmax (1, (int) std::round (0.002 * sampleRate));
        const auto length = juce::jmax (block, (int) std::round (window * sampleRate));
        const auto numBlocks = (int) signal.size() / block;
        if (numBlocks < 20)
            return attacks;
        std::vector<double> squares (signal.size() + 1, 0.0);
        for (size_t n = 0; n < signal.size(); ++n)
            squares[n + 1] = squares[n] + (double) signal[n] * signal[n];
        std::vector<float> level ((size_t) numBlocks);
        auto loudest = -200.0f;
        for (int i = 0; i < numBlocks; ++i)
        {
            const auto end = (i + 1) * block, start = juce::jmax (0, end - length);
            level[(size_t) i] = (float) (10.0 * std::log10 ((squares[(size_t) end] - squares[(size_t) start]) / length + 1.0e-14));
            loudest = juce::jmax (loudest, level[(size_t) i]);
        }
        const auto floor = juce::jmax (loudest - 45.0f, -100.0f);
        constexpr int lookBack = 15, gap = 20; // 30 ms; attacks at least 40 ms apart
        auto last = -gap;
        for (int i = 2; i < numBlocks; ++i)
        {
            if (level[(size_t) i] < floor || i - last < gap)
                continue; // (from where the last attack started)
            auto base = level[(size_t) i - 2];
            for (int j = juce::jmax (0, i - lookBack); j <= i - 2; ++j)
                base = juce::jmin (base, level[(size_t) j]);
            if (level[(size_t) i] - base < jump)
                continue;
            // Back to where the rise first gets half way (in dB): the level is over the last
            // `window`, so that is where the attack came into it, in the block's end.
            const auto halfway = base + 0.5f * (level[(size_t) i] - base);
            auto from = i;
            while (from > juce::jmax (0, i - lookBack) && level[(size_t) from - 1] >= halfway
                   && level[(size_t) from - 1] <= level[(size_t) from] + 1.0f)
                --from;
            if (from - last < gap)
                continue;
            attacks.push_back ((from + 0.5) * block / sampleRate);
            last = from;
        }
        return attacks;
    }
}

std::vector<DrumHit> findDrumHits (const juce::AudioBuffer<float>& audio, double sampleRate, const DrumOptions& options,
                                   const std::function<bool()>& shouldContinue)
{
    std::vector<DrumHit> hits;
    if (audio.getNumSamples() < (int) (0.1 * sampleRate) || sampleRate <= 0.0)
        return hits;

    const auto stopped = [&] { return shouldContinue && ! shouldContinue(); };
    auto mono = mixToMono (audio);
    const auto duration = mono.size() / sampleRate;

    // Where hits start: attacks in the whole kit and in four bands on their own (a kick
    // under a loud snare, a soft hat after one). Sensitivity sets the jump: 12 dB .. 4 dB.
    const auto jump = 12.0f - 8.0f * juce::jlimit (0.0f, 1.0f, options.sensitivity);
    struct Found { double time; int band; };
    std::vector<Found> found;
    struct Band { double highPass, lowPass, window; };
    const Band bands[] = { { 0.0, 0.0, 0.025 }, { 0.0, 100.0, 0.025 }, { 100.0, 400.0, 0.012 }, { 1500.0, 5000.0, 0.005 }, { 6000.0, 0.0, 0.005 } };
    for (int b = 0; b < (int) std::size (bands); ++b)
    {
        const auto signal = b == 0 ? mono : filter (mono, sampleRate, bands[b].highPass, bands[b].lowPass);
        for (auto t : findAttacks (signal, sampleRate, jump, bands[b].window))
            found.push_back ({ t, b });
        if (stopped())
            return {};
    }
    std::sort (found.begin(), found.end(), [] (const Found& a, const Found& b) { return a.time < b.time; });

    // The whole kit's attacks are the hits; an attack in one band only makes a hit of its
    // own when no hit started within 30 ms after or before it (70 ms before, for the low
    // bands: their level rises later, and a tom's glide shows up again a little later).
    // Hits closer than 40 ms are one.
    std::vector<double> times;
    for (const auto& f : found)
        if (f.band == 0)
            times.push_back (f.time);
    const auto wholeKit = times;
    for (const auto& f : found) // in time order
    {
        const auto late = f.band <= 2 ? 0.07 : 0.03;
        if (f.band != 0 && std::none_of (times.begin(), times.end(), [&] (double t) { return f.time > t - 0.03 && f.time < t + late; }))
            times.push_back (f.time);
    }
    std::sort (times.begin(), times.end());
    times.erase (std::unique (times.begin(), times.end()), times.end());
    for (size_t i = 1; i < times.size();)
        if (times[i] - times[i - 1] < 0.04)
        {
            // Keep the whole kit's attack.
            const auto keepLater = std::find (wholeKit.begin(), wholeKit.end(), times[i]) != wholeKit.end();
            times.erase (times.begin() + (long) (keepLater ? i - 1 : i));
        }
        else
            ++i;
    if (times.empty())
        return hits;

    // The new sound of each hit: power per region from the attack on, less what was
    // already sounding. The lows are taken over 60 ms either side; higher up, where a
    // ringing snare or cymbal fades, over 30 ms after against the last 20 ms before (per
    // second), so a hat right after a snare still shows.
    constexpr int order = 12, size = 1 << order;
    juce::dsp::FFT fft (order);
    std::vector<float> data ((size_t) size * 2);
    const auto fade = juce::jmax (1, (int) std::round (0.002 * sampleRate));
    const auto binHz = sampleRate / size;
    const auto spectrumAt = [&] (double from, double seconds = 0.06) // magnitudes in data[0 .. size / 2]
    {
        const auto windowLength = juce::jmin (size, (int) std::round (seconds * sampleRate));
        const auto start = (int) std::round (from * sampleRate);
        std::fill (data.begin(), data.end(), 0.0f);
        for (int i = 0; i < windowLength; ++i)
        {
            const auto n = start + i;
            if (n < 0 || n >= (int) mono.size())
                continue;
            const auto edge = juce::jmin (i, windowLength - 1 - i);
            const auto gain = edge < fade ? 0.5f - 0.5f * (float) std::cos (juce::MathConstants<double>::pi * edge / fade) : 1.0f;
            data[(size_t) i] = mono[(size_t) n] * gain;
        }
        fft.performFrequencyOnlyForwardTransform (data.data(), true);
    };
    const auto regionPowers = [&] (double from, double seconds) // per second of the window
    {
        spectrumAt (from, seconds);
        std::array<double, numRegions> powers {};
        for (int r = 0; r < numRegions; ++r)
            for (auto k = (int) std::ceil (regionEdges[r] / binHz); k < (int) std::ceil (regionEdges[r + 1] / binHz) && k < size / 2; ++k)
                powers[(size_t) r] += (double) data[(size_t) k] * data[(size_t) k] / seconds;
        return powers;
    };
    auto high = filter (mono, sampleRate, 6000.0, 0.0);

    for (size_t h = 0; h < times.size(); ++h)
    {
        DrumHit hit;
        hit.time = times[h];
        const auto t = hit.time;
        // (Up to the next hit.)
        const auto room = h + 1 < times.size() ? juce::jmax (0.01, times[h + 1] - t) : 0.06;
        const auto lowNow = regionPowers (t - 0.002, juce::jmin (0.06, room)), lowBefore = regionPowers (t - 0.064, 0.06);
        const auto highNow = regionPowers (t - 0.002, juce::jmin (0.03, room)), highBefore = regionPowers (t - 0.022, 0.02);
        for (int r = 0; r < numRegions; ++r)
        {
            // In dB, as power over 60 ms (so the regions compare as before).
            const auto added = r <= body ? lowNow[(size_t) r] - lowBefore[(size_t) r] : highNow[(size_t) r] - highBefore[(size_t) r];
            hit.region[(size_t) r] = (float) (10.0 * std::log10 (juce::jmax (0.0, added * 0.06) + 1.0e-12));
        }

        // The pitch of the low end once the attack has settled (a kick's sweep is over by
        // then; toms are higher, and have mostly glided down to their note).
        spectrumAt (t + juce::jmin (0.03, 0.5 * room), juce::jmin (0.06, juce::jmax (0.02, room - 0.03)));
        auto peakBin = -1;
        auto peakValue = 0.0f;
        for (auto k = (int) std::ceil (35.0 / binHz); k <= (int) (400.0 / binHz); ++k)
            if (data[(size_t) k] > peakValue)
                peakValue = data[(size_t) k], peakBin = k;
        if (peakBin > 0)
        {
            const auto a = data[(size_t) peakBin - 1], b = data[(size_t) peakBin], c = data[(size_t) peakBin + 1];
            const auto denominator = a - 2.0f * b + c;
            const auto delta = std::abs (denominator) > 1.0e-12f ? juce::jlimit (-0.5f, 0.5f, 0.5f * (a - c) / denominator) : 0.0f;
            hit.bodyHz = (float) ((peakBin + delta) * binHz);
        }

        // How long the cymbal band rings: 5 ms steps from its peak until it falls 20 dB or
        // the next hit; cut short, the fall so far is carried on.
        {
            const auto until = juce::jmin (h + 1 < times.size() ? times[h + 1] : duration, t + 3.0);
            const auto step = 0.005;
            auto peak = -120.0f;
            auto peakTime = t;
            for (auto s = t; s < juce::jmin (until, t + 0.04); s += step)
                if (const auto l = levelDb (high, sampleRate, s, s + step); l > peak)
                    peak = l, peakTime = s;
            auto ring = -1.0;
            auto lastTime = peakTime;
            auto lastLevel = peak;
            for (auto s = peakTime + step; s + step <= until; s += step)
            {
                lastLevel = levelDb (high, sampleRate, s, s + step);
                lastTime = s;
                if (lastLevel <= peak - 20.0f) { ring = s - peakTime; break; }
            }
            if (ring < 0.0)
            {
                const auto fallen = peak - lastLevel;
                const auto heard = lastTime - peakTime;
                ring = fallen > 1.0f && heard > 0.0 ? heard * 20.0 / fallen : juce::jmax (heard, 3.0);
            }
            hit.ring = (float) juce::jmin (ring, 5.0);
        }

        hits.push_back (hit);
        if (stopped())
            return {};
    }

    // A low drum's lows can rise well after its attack (a kick's sweep, a tom's glide): new
    // sound that is only lows, within 80 ms of the hit before, belongs to that hit.
    auto loudest = hits.front().region;
    for (const auto& hit : hits)
        for (int r = 0; r < numRegions; ++r)
            loudest[(size_t) r] = juce::jmax (loudest[(size_t) r], hit.region[(size_t) r]);
    for (size_t h = 1; h < hits.size();)
    {
        auto& hit = hits[h];
        auto& previous = hits[h - 1];
        const auto quiet = [&] (Region r) { return hit.region[r] <= -100.0f || hit.region[r] < loudest[r] - 40.0f; };
        const auto lowsOnly = quiet (wires) && quiet (air)
                              && (quiet (mids) || hit.region[mids] < juce::jmax (hit.region[sub], hit.region[body]) - 20.0f);
        if (lowsOnly && hit.time - previous.time < 0.08)
        {
            previous.region[sub] = juce::jmax (previous.region[sub], hit.region[sub]);
            previous.region[body] = juce::jmax (previous.region[body], hit.region[body]);
            hits.erase (hits.begin() + (long) h);
        }
        else
            ++h;
    }

    // Name the hits. Each part is looked for in the region it lives in, against the
    // clip's loudest hit there; how the regions compare tells parts apart, with the
    // thresholds taken from the clip where its hits fall into two clear groups.
    const auto strong = [&] (const DrumHit& hit, Region r, float below) { return hit.region[r] > -100.0f && hit.region[r] >= loudest[r] - below; };

    // A kick has lows of its own (a tom or a snare's body has little under 90 Hz); a snare
    // has wires as well as a body (a tom is tonal).
    const auto learnt = [&] (auto&& value, auto&& include, float fallback)
    {
        std::vector<float> values;
        for (const auto& hit : hits)
            if (include (hit))
                values.push_back (value (hit));
        auto split = fallback;
        if (float found = 0.0f; findSplit (values, 8.0f, found))
            split = found;
        return split;
    };
    const auto kickness = [] (const DrumHit& h) { return h.region[sub] - h.region[body]; };
    const auto wiriness = [] (const DrumHit& h) { return h.region[wires] - h.region[body]; };
    const auto kickSplit = learnt (kickness, [&] (const DrumHit& h) { return strong (h, sub, 30.0f) && strong (h, body, 40.0f); }, -18.0f);
    const auto snareSplit = learnt (wiriness, [&] (const DrumHit& h) { return strong (h, body, 30.0f) && strong (h, wires, 40.0f); }, -18.0f);

    std::vector<bool> isKick, isSnare;
    for (const auto& hit : hits)
    {
        isKick.push_back (strong (hit, sub, 30.0f) && (hit.region[body] <= -100.0f || kickness (hit) >= kickSplit));
        isSnare.push_back (strong (hit, body, 30.0f) && strong (hit, wires, 30.0f) && wiriness (hit) >= snareSplit);
    }

    // How bright a snare is on its own here (its wires reach into the cymbals' region).
    std::vector<float> snareAir;
    for (size_t e = 0; e < hits.size(); ++e)
        if (isSnare[e])
            snareAir.push_back (hits[e].region[air] - hits[e].region[wires]);
    std::sort (snareAir.begin(), snareAir.end());
    const auto snareBrightness = snareAir.empty() ? 0.0f : snareAir[snareAir.size() / 2];

    std::vector<float> tomOctaves;
    for (size_t e = 0; e < hits.size(); ++e)
    {
        auto& hit = hits[e];
        if (isKick[e])
            hit.notes.push_back (gm::kick);

        // Toms: a strong tonal body that isn't a snare; with a kick, only when the body's
        // pitch is well above a kick's.
        if (! isSnare[e] && strong (hit, body, 25.0f) && (isKick[e] ? hit.bodyHz > 140.0f : true))
        {
            hit.notes.push_back (gm::lowTom); // low or high: below, once all toms are known
            tomOctaves.push_back (std::log2 (juce::jmax (30.0f, hit.bodyHz)));
        }
        if (isSnare[e])
            hit.notes.push_back (gm::snare);

        // Cymbals: brightness that the other parts don't explain. Over a snare: clearly
        // brighter than this clip's snares, or ringing on; over a kick: ringing longer than
        // its click; alone: the brightest part of the hit.
        auto cymbal = false;
        if (strong (hit, air, 30.0f))
        {
            if (isSnare[e])
                cymbal = hit.region[air] - hit.region[wires] > snareBrightness + 6.0f || hit.ring >= 0.4f;
            else if (isKick[e])
                cymbal = hit.ring >= 0.035f;
            else
                cymbal = hit.region[air] - juce::jmax (hit.region[sub], hit.region[body]) >= -20.0f && hit.region[air] >= hit.region[mids] + 6.0f;
        }
        if (cymbal)
            hit.notes.push_back (hit.ring < 0.2f ? gm::closedHat : hit.ring < 0.8f ? gm::openHat : gm::crash);
    }

    // A tom hit again while it still rings adds little new body, but its attack is there:
    // a hit with nothing named, whose low pitch is a tom's (the clip has toms around it),
    // is that tom.
    if (! tomOctaves.empty())
    {
        auto lowest = *std::min_element (tomOctaves.begin(), tomOctaves.end());
        auto highest = *std::max_element (tomOctaves.begin(), tomOctaves.end());
        for (auto& hit : hits)
            if (hit.notes.empty() && hit.bodyHz > 0.0f && strong (hit, mids, 30.0f))
                if (const auto octave = std::log2 (hit.bodyHz); octave >= lowest - 0.2f && octave <= highest + 0.2f)
                {
                    hit.notes.push_back (gm::lowTom);
                    tomOctaves.push_back (octave);
                }
    }

    // Two groups of toms when their pitches are clearly apart (a quarter of an octave).
    auto tomSplit = std::log2 (135.0f);
    findSplit (tomOctaves, 0.25f, tomSplit);
    for (auto& hit : hits)
        for (auto& n : hit.notes)
            if (n == gm::lowTom && std::log2 (juce::jmax (30.0f, hit.bodyHz)) >= tomSplit)
                n = gm::highTom;

    // A steady hat pulse carries on through the snares that hide it: a snare halfway
    // between two closed hats, on the pulse the hats before it keep, gets one too.
    std::vector<double> hatTimes;
    for (size_t e = 0; e < hits.size(); ++e)
    {
        auto& hit = hits[e];
        const auto hasHat = [] (const DrumHit& h) { return std::find (h.notes.begin(), h.notes.end(), (int) gm::closedHat) != h.notes.end(); };
        if (isSnare[e] && ! hasHat (hit) && hatTimes.size() >= 2)
        {
            const auto previous = hatTimes.back(), beforeThat = hatTimes[hatTimes.size() - 2];
            const auto step = hit.time - previous;
            auto next = -1.0;
            for (auto j = e + 1; j < hits.size() && next < 0.0; ++j)
                if (hasHat (hits[j]))
                    next = hits[j].time;
            if (step > 0.05 && step < 0.6 && std::abs ((previous - beforeThat) - step) < 0.015 && next > 0.0
                && std::abs ((next - hit.time) - step) < 0.015)
                hit.notes.push_back (gm::closedHat);
        }
        if (hasHat (hit))
            hatTimes.push_back (hit.time);
    }

    return hits;
}

std::vector<Note> toNotes (const std::vector<DrumHit>& hits, const DrumOptions& options)
{
    std::vector<Note> notes;
    // Velocity against the loudest hit of the same drum.
    const auto levelFor = [] (const DrumHit& hit, int note)
    {
        switch (note)
        {
            case gm::kick: return hit.region[sub];
            case gm::lowTom: case gm::highTom: return hit.region[body];
            case gm::snare: return hit.region[wires];
            default: return hit.region[air];
        }
    };
    std::map<int, float> loudest;
    for (const auto& hit : hits)
        for (auto n : hit.notes)
            loudest[n] = loudest.count (n) > 0 ? juce::jmax (loudest[n], levelFor (hit, n)) : levelFor (hit, n);

    for (const auto& hit : hits)
        for (auto n : hit.notes)
        {
            Note note;
            note.time = juce::jmax (0.0, quantiseTime (hit.time, options.grid, options.quantise));
            note.length = options.noteLength;
            note.pitch = n;
            note.velocity = velocityFrom (levelFor (hit, n), loudest[n]);
            notes.push_back (note);
        }
    std::stable_sort (notes.begin(), notes.end(), [] (const Note& a, const Note& b) { return a.time < b.time; });
    return notes;
}

std::vector<Note> drumsToMidi (const juce::AudioBuffer<float>& audio, double sampleRate, const DrumOptions& options,
                               const std::function<bool()>& shouldContinue)
{
    return toNotes (findDrumHits (audio, sampleRate, options, shouldContinue), options);
}
}
