#include "Pitch.h"

#include <juce_dsp/juce_dsp.h>

#include "dsp/Biquad.h"

#include <algorithm>
#include <cmath>
#include <mutex>

namespace analysis::pitch
{
//==============================================================================
int Track::frameAt (double seconds) const noexcept
{
    if (midi.empty())
        return 0;
    return juce::jlimit (0, size() - 1, (int) std::lround (seconds / hop));
}

float Track::midiAt (double seconds) const noexcept
{
    if (midi.empty())
        return 0.0f;
    const auto position = juce::jlimit (0.0, (double) (size() - 1), seconds / hop);
    const auto i = (int) position;
    const auto j = juce::jmin (i + 1, size() - 1);
    const auto f = (float) (position - i);
    if (midi[(size_t) i] > 0.0f && midi[(size_t) j] > 0.0f)
        return midi[(size_t) i] + f * (midi[(size_t) j] - midi[(size_t) i]);
    return f < 0.5f ? midi[(size_t) i] : midi[(size_t) j];
}

float Track::energyAt (double seconds) const noexcept
{
    return energy.empty() ? -100.0f : energy[(size_t) frameAt (seconds)];
}

//==============================================================================
namespace
{
    float median (std::vector<float> values)
    {
        if (values.empty())
            return 0.0f;
        const auto middle = values.begin() + (std::ptrdiff_t) (values.size() / 2);
        std::nth_element (values.begin(), middle, values.end());
        return *middle;
    }
}

Track analyse (const juce::AudioBuffer<float>& source, double sampleRate, const std::function<bool()>& shouldContinue)
{
    Track track;
    const auto numSamples = source.getNumSamples(), numChannels = source.getNumChannels();
    if (numSamples == 0 || numChannels == 0 || sampleRate <= 0.0)
        return track;

    // Mono, filtered and brought down to about 16 kHz: a voice's pitch lives well below.
    const auto factor = juce::jmax (1, (int) std::round (sampleRate / 16000.0));
    const auto rate = sampleRate / factor;
    fx::Biquad first, second;
    first.setup (fx::Biquad::Type::lowPass, sampleRate, 0.42 * rate, 0.541);
    second.setup (fx::Biquad::Type::lowPass, sampleRate, 0.42 * rate, 1.307);

    std::vector<float> x ((size_t) (numSamples / factor + 1), 0.0f);
    for (int i = 0; i < numSamples; ++i)
    {
        auto sum = 0.0f;
        for (int ch = 0; ch < numChannels; ++ch)
            sum += source.getSample (ch, i);
        const auto y = factor > 1 ? second.process (0, first.process (0, sum / (float) numChannels)) : sum / (float) numChannels;
        if (i % factor == 0)
            x[(size_t) (i / factor)] = y;
    }

    // YIN: the cumulative-mean-normalised difference of each 32 ms window with itself later,
    // between 1 kHz and 60 Hz; its correlations come from FFTs.
    const auto window = (int) std::round (0.032 * rate);
    const auto minLag = juce::jmax (2, (int) std::floor (rate / 1000.0));
    const auto maxLag = (int) std::ceil (rate / 60.0);
    const auto order = (int) std::ceil (std::log2 ((double) (window + maxLag + 1)));
    const auto n = 1 << order;
    juce::dsp::FFT fft (order);

    const auto hopSamples = juce::jmax (1, (int) std::round (0.005 * rate));
    track.hop = hopSamples / rate;
    const auto numFrames = (int) (x.size() / (size_t) hopSamples) + 1;
    track.midi.assign ((size_t) numFrames, 0.0f);
    track.energy.assign ((size_t) numFrames, -100.0f);

    std::vector<double> squares (x.size() + 1, 0.0); // running sum of x²
    for (size_t i = 0; i < x.size(); ++i)
        squares[i + 1] = squares[i] + (double) x[i] * x[i];
    const auto energyOf = [&] (int start, int length)
    {
        const auto a = (size_t) juce::jlimit (0, (int) x.size(), start), b = (size_t) juce::jlimit (0, (int) x.size(), start + length);
        return squares[b] - squares[a];
    };

    std::vector<float> a ((size_t) n * 2), b ((size_t) n * 2);
    std::vector<float> difference ((size_t) maxLag + 1), clarity ((size_t) numFrames, 0.0f);
    auto loudest = -100.0f;

    for (int frame = 0; frame < numFrames; ++frame)
    {
        if (shouldContinue && (frame & 255) == 0 && ! shouldContinue())
            return {};

        const auto start = frame * hopSamples - window / 2;
        std::fill (a.begin(), a.end(), 0.0f);
        std::fill (b.begin(), b.end(), 0.0f);
        for (int i = 0; i < window + maxLag; ++i)
        {
            const auto k = start + i;
            const auto v = k >= 0 && k < (int) x.size() ? x[(size_t) k] : 0.0f;
            b[(size_t) i] = v;
            if (i < window)
                a[(size_t) i] = v;
        }

        const auto power = energyOf (start, window);
        const auto db = (float) (10.0 * std::log10 (power / window + 1.0e-12));
        track.energy[(size_t) frame] = db;
        loudest = juce::jmax (loudest, db);
        if (power < 1.0e-9)
            continue;

        fft.performRealOnlyForwardTransform (a.data(), true);
        fft.performRealOnlyForwardTransform (b.data(), true);
        for (int k = 0; k <= n / 2; ++k)
        {
            const std::complex<float> p { a[(size_t) (2 * k)], a[(size_t) (2 * k + 1)] }, q { b[(size_t) (2 * k)], b[(size_t) (2 * k + 1)] };
            const auto r = std::conj (p) * q;
            a[(size_t) (2 * k)] = r.real();
            a[(size_t) (2 * k + 1)] = r.imag();
        }
        fft.performRealOnlyInverseTransform (a.data()); // a[τ] = Σ x[j] x[j + τ]

        // d'(τ) = d(τ) τ / Σ d, with d(τ) = e(0) + e(τ) - 2 r(τ).
        auto sum = 0.0;
        difference[0] = 1.0f;
        for (int lag = 1; lag <= maxLag; ++lag)
        {
            const auto d = juce::jmax (0.0, power + energyOf (start + lag, window) - 2.0 * a[(size_t) lag]);
            sum += d;
            difference[(size_t) lag] = sum > 0.0 ? (float) (d * lag / sum) : 1.0f;
        }

        auto best = -1;
        for (int lag = minLag; lag < maxLag; ++lag)
        {
            if (difference[(size_t) lag] < 0.15f)
            {
                while (lag + 1 < maxLag && difference[(size_t) lag + 1] < difference[(size_t) lag])
                    ++lag;
                best = lag;
                break;
            }
        }
        if (best < 0)
        {
            best = minLag;
            for (int lag = minLag; lag < maxLag; ++lag)
                if (difference[(size_t) lag] < difference[(size_t) best])
                    best = lag;
            if (difference[(size_t) best] > 0.3f)
                continue;
        }

        // Between lags: a parabola through the dip.
        auto lag = (double) best;
        if (best > minLag && best < maxLag - 1)
        {
            const auto l = difference[(size_t) best - 1], c = difference[(size_t) best], r = difference[(size_t) best + 1];
            const auto curve = l + r - 2.0f * c;
            if (curve > 1.0e-9f)
                lag += juce::jlimit (-0.5, 0.5, 0.5 * (l - r) / curve);
        }

        track.midi[(size_t) frame] = (float) (69.0 + 12.0 * std::log2 (rate / lag / 440.0));
        clarity[(size_t) frame] = 1.0f - difference[(size_t) best];
    }

    // Quiet frames aren't singing, and a voiced blip under 20 ms is noise.
    const auto gate = juce::jmax (-60.0f, loudest - 45.0f);
    for (int i = 0; i < numFrames; ++i)
        if (track.energy[(size_t) i] < gate)
            track.midi[(size_t) i] = 0.0f;

    for (int i = 0; i < numFrames;)
    {
        if (track.midi[(size_t) i] <= 0.0f) { ++i; continue; }
        auto j = i;
        while (j < numFrames && track.midi[(size_t) j] > 0.0f)
            ++j;
        if (j - i < 4)
            std::fill (track.midi.begin() + i, track.midi.begin() + j, 0.0f);
        i = j;
    }

    // A median of five takes out single-frame octave slips.
    auto smoothed = track.midi;
    for (int i = 2; i + 2 < numFrames; ++i)
    {
        if (track.midi[(size_t) i] <= 0.0f)
            continue;
        std::vector<float> around;
        for (int k = i - 2; k <= i + 2; ++k)
            if (track.midi[(size_t) k] > 0.0f)
                around.push_back (track.midi[(size_t) k]);
        if (around.size() >= 3)
            smoothed[(size_t) i] = median (around);
    }
    track.midi = std::move (smoothed);
    return track;
}

std::shared_ptr<const Track> analyseCached (const juce::AudioBuffer<float>& source, double sampleRate)
{
    // Identified by its shape and a sparse sample of its audio.
    auto hash = (juce::int64) source.getNumSamples() * 31 + source.getNumChannels() * 7 + (juce::int64) sampleRate;
    const auto stride = juce::jmax (1, source.getNumSamples() / 4096);
    for (int ch = 0; ch < source.getNumChannels(); ++ch)
        for (int i = 0; i < source.getNumSamples(); i += stride)
            hash = hash * 1099511628211LL + (juce::int64) std::lround (source.getSample (ch, i) * 1.0e6f);

    static std::mutex lock;
    static std::vector<std::pair<juce::int64, std::shared_ptr<const Track>>> cache;
    {
        const std::lock_guard<std::mutex> guard (lock);
        for (const auto& [key, track] : cache)
            if (key == hash)
                return track;
    }

    auto track = std::make_shared<const Track> (analyse (source, sampleRate));
    const std::lock_guard<std::mutex> guard (lock);
    cache.emplace_back (hash, track);
    if (cache.size() > 6)
        cache.erase (cache.begin());
    return track;
}

//==============================================================================
bool Note::isEdited() const noexcept
{
    return std::abs (shift) > 1.0e-3f || straighten > 1.0e-3f || std::abs (vibrato - 1.0f) > 1.0e-3f
        || std::abs (formant) > 1.0e-3f || std::abs (gain) > 1.0e-3f;
}

bool anyEdited (const std::vector<Note>& notes)
{
    return std::any_of (notes.begin(), notes.end(), [] (const Note& n) { return n.isEdited(); });
}

float getPitch (const Track& track, double start, double end)
{
    if (track.midi.empty() || end <= start)
        return 0.0f;

    const auto collect = [&] (double a, double b)
    {
        std::vector<float> values;
        for (int i = juce::jmax (0, (int) std::ceil (a / track.hop)); i < track.size() && track.timeOf (i) <= b; ++i)
            if (track.midi[(size_t) i] > 0.0f)
                values.push_back (track.midi[(size_t) i]);
        return values;
    };

    // The steady middle: leave out the scoop in and the fall off.
    const auto length = end - start;
    auto values = collect (start + 0.2 * length, end - 0.2 * length);
    if (values.size() < 3)
        values = collect (start, end);
    return median (values);
}

std::vector<Note> findNotes (const Track& track, double from, double to)
{
    std::vector<Note> notes;
    if (track.midi.empty())
        return notes;

    const auto first = juce::jmax (0, (int) std::ceil (from / track.hop));
    const auto last = (int) juce::jmin ((double) track.size(), std::floor (to / track.hop) + 1.0);
    const auto& midi = track.midi;
    const auto minFrames = (int) std::round (0.06 / track.hop);
    const auto settleFrames = (int) std::round (0.04 / track.hop);
    const auto reach = (int) std::round (0.05 / track.hop);
    std::vector<std::pair<int, int>> segments; // [start, end) frames

    for (int i = first; i < last;)
    {
        if (midi[(size_t) i] <= 0.0f) { ++i; continue; }

        // A run of singing, through gaps of up to 15 ms.
        auto end = i, gap = 0;
        for (auto j = i; j < last; ++j)
        {
            if (midi[(size_t) j] > 0.0f) { end = j + 1; gap = 0; }
            else if (++gap > 3) break;
        }

        // The pitch line with vibrato and wobble taken out: a median over 100 ms.
        std::vector<float> line ((size_t) (end - i), 0.0f);
        for (auto j = i; j < end; ++j)
        {
            if (midi[(size_t) j] <= 0.0f)
                continue;
            std::vector<float> around;
            for (auto k = juce::jmax (i, j - reach); k < juce::jmin (end, j + reach + 1); ++k)
                if (midi[(size_t) k] > 0.0f)
                    around.push_back (midi[(size_t) k]);
            line[(size_t) (j - i)] = median (around);
        }

        // Cut where the line settles more than 0.7 semitones away for 40 ms.
        auto noteStart = i, excursion = -1;
        std::vector<float> values;
        auto reference = line[0];
        for (auto j = i; j < end; ++j)
        {
            const auto v = line[(size_t) (j - i)];
            if (v <= 0.0f)
                continue;
            if (std::abs (v - reference) > 0.7f)
            {
                if (excursion < 0)
                    excursion = j;
                if (j - excursion + 1 >= settleFrames)
                {
                    if (excursion - noteStart >= minFrames)
                        segments.emplace_back (noteStart, excursion);
                    // (a shorter start was a glide into this pitch: it stays with it)
                    if (excursion - noteStart >= minFrames)
                        noteStart = excursion;
                    values.clear();
                    for (auto k = excursion; k <= j; ++k)
                        if (line[(size_t) (k - i)] > 0.0f)
                            values.push_back (line[(size_t) (k - i)]);
                    reference = median (values);
                    excursion = -1;
                }
            }
            else
            {
                excursion = -1;
                values.push_back (v);
                if (values.size() < 16 || values.size() % 4 == 0)
                    reference = median (values);
            }
        }
        segments.emplace_back (noteStart, end);
        i = end;
    }

    // Two syllables on one pitch: cut at a dip of 8 dB or more between them.
    std::vector<std::pair<int, int>> cut;
    for (auto [a, b] : segments)
    {
        auto start = a;
        for (auto m = a + minFrames; m < b - minFrames; ++m)
        {
            const auto e = track.energy[(size_t) m];
            if (e > track.energy[(size_t) m - 1] || e > track.energy[(size_t) m + 1])
                continue;
            auto before = e, after = e;
            for (auto k = start; k < m; ++k) before = juce::jmax (before, track.energy[(size_t) k]);
            for (auto k = m + 1; k < b; ++k) after = juce::jmax (after, track.energy[(size_t) k]);
            if (before - e >= 8.0f && after - e >= 8.0f && m - start >= minFrames)
            {
                cut.emplace_back (start, m);
                start = m;
            }
        }
        cut.emplace_back (start, b);
    }

    for (auto [a, b] : cut)
    {
        // Trim unvoiced frames off the ends.
        while (a < b && midi[(size_t) a] <= 0.0f) ++a;
        while (b > a && midi[(size_t) b - 1] <= 0.0f) --b;
        if (b <= a)
            continue;
        Note note;
        note.start = juce::jmax (0.0, track.timeOf (a) - track.hop / 2);
        note.end = track.timeOf (b - 1) + track.hop / 2;
        note.pitch = getPitch (track, note.start, note.end);
        notes.push_back (note);
    }

    // Shorter than 60 ms: part of the note it touches (the nearer in pitch), or gone.
    for (size_t k = 0; k < notes.size();)
    {
        auto& note = notes[k];
        if (note.end - note.start >= 0.06 || notes.size() == 1)
        {
            ++k;
            continue;
        }
        const auto touchesBefore = k > 0 && note.start - notes[k - 1].end < 0.02;
        const auto touchesAfter = k + 1 < notes.size() && notes[k + 1].start - note.end < 0.02;
        auto into = -1;
        if (touchesBefore && touchesAfter)
            into = std::abs (notes[k - 1].pitch - note.pitch) <= std::abs (notes[k + 1].pitch - note.pitch) ? (int) k - 1 : (int) k + 1;
        else if (touchesBefore)
            into = (int) k - 1;
        else if (touchesAfter)
            into = (int) k + 1;

        if (into >= 0)
        {
            auto& other = notes[(size_t) into];
            other.start = juce::jmin (other.start, note.start);
            other.end = juce::jmax (other.end, note.end);
        }
        if (into < 0 && note.end - note.start >= 0.04)
        {
            ++k;
            continue;
        }
        notes.erase (notes.begin() + (std::ptrdiff_t) k);
        if (into >= 0 && into < (int) k)
            --k; // look at the grown note again
    }

    return notes;
}

//==============================================================================
juce::String toString (const std::vector<Note>& notes)
{
    juce::StringArray parts;
    for (const auto& n : notes)
        parts.add (juce::String (n.start, 4) + ":" + juce::String (n.end, 4) + ":" + juce::String (n.pitch, 3) + ":"
                   + juce::String (n.shift, 3) + ":" + juce::String (n.straighten, 3) + ":" + juce::String (n.vibrato, 3) + ":"
                   + juce::String (n.formant, 3) + ":" + juce::String (n.gain, 2) + ":" + juce::String (n.transition, 3));
    return parts.joinIntoString (";");
}

std::vector<Note> fromString (const juce::String& text)
{
    std::vector<Note> notes;
    for (const auto& part : juce::StringArray::fromTokens (text, ";", {}))
    {
        const auto v = juce::StringArray::fromTokens (part, ":", {});
        if (v.size() < 3)
            continue;
        const auto at = [&] (int i, float fallback) { return i < v.size() ? v[i].getFloatValue() : fallback; };
        Note n;
        n.start = v[0].getDoubleValue();
        n.end = v[1].getDoubleValue();
        n.pitch = at (2, 0.0f);
        n.shift = juce::jlimit (-Note::maxShift, Note::maxShift, at (3, 0.0f));
        n.straighten = juce::jlimit (0.0f, 1.0f, at (4, 0.0f));
        n.vibrato = juce::jlimit (0.0f, 2.0f, at (5, 1.0f));
        n.formant = juce::jlimit (-Note::maxFormant, Note::maxFormant, at (6, 0.0f));
        n.gain = juce::jlimit (-60.0f, Note::maxGain, at (7, 0.0f));
        n.transition = juce::jlimit (0.005f, 1.0f, at (8, Note::defaultTransition));
        if (std::isfinite (n.start) && std::isfinite (n.end) && n.end > n.start)
            notes.push_back (n);
    }
    std::sort (notes.begin(), notes.end(), [] (const Note& a, const Note& b) { return a.start < b.start; });
    return notes;
}

//==============================================================================
std::vector<Shape> getShapes (const Track& track, const std::vector<Note>& input)
{
    std::vector<Shape> shapes ((size_t) track.size());
    if (input.empty() || track.midi.empty())
        return shapes;

    auto notes = input;
    std::sort (notes.begin(), notes.end(), [] (const Note& a, const Note& b) { return a.start < b.start; });

    // The move, formant and level: held over each note, gliding between neighbours (less
    // than half a second apart) over the later one's transition or their gap, and fading in
    // and out around a note on its own.
    struct Key { double time; Shape value; };
    std::vector<Key> keys;
    const auto valueOf = [] (const Note& n) { return Shape { n.shift, n.formant, n.gain }; };
    const auto add = [&] (double time, Shape value)
    {
        if (! keys.empty())
            time = juce::jmax (time, keys.back().time);
        keys.push_back ({ time, value });
    };

    for (size_t k = 0; k < notes.size(); ++k)
    {
        const auto& n = notes[k];
        const auto v = valueOf (n);
        const auto middle = (n.start + n.end) / 2;

        if (k > 0 && n.start - notes[k - 1].end < 0.5)
        {
            const auto& p = notes[k - 1];
            const auto centre = (p.end + n.start) / 2, half = juce::jmax ((double) n.transition / 2, (n.start - p.end) / 2);
            add (juce::jmax (centre - half, (p.start + p.end) / 2), valueOf (p));
            add (juce::jmin (centre + half, middle), v);
        }
        else
        {
            add (n.start - 0.15, {});
            add (n.start - 0.1, v);
        }

        if (k + 1 == notes.size() || notes[k + 1].start - n.end >= 0.5)
        {
            add (n.end + 0.1, v);
            add (n.end + 0.15, {});
        }
    }

    size_t key = 0;
    for (int i = 0; i < track.size(); ++i)
    {
        const auto t = track.timeOf (i);
        while (key < keys.size() && keys[key].time <= t)
            ++key;
        auto& s = shapes[(size_t) i];
        if (key == 0 || key == keys.size())
            continue; // before the first key and after the last: untouched (they're zero)
        const auto& a = keys[key - 1];
        const auto& b = keys[key];
        auto f = b.time > a.time ? (float) ((t - a.time) / (b.time - a.time)) : 1.0f;
        f = f * f * (3.0f - 2.0f * f); // smooth in and out
        s.semitones = a.value.semitones + f * (b.value.semitones - a.value.semitones);
        s.formant = a.value.formant + f * (b.value.formant - a.value.formant);
        s.gainDb = a.value.gainDb + f * (b.value.gainDb - a.value.gainDb);
    }

    // Drift and vibrato, inside each note: the line smoothed over 150 ms is the drift (less
    // the note's centre), the rest is vibrato. Faded in and out over 20 ms at the note's ends.
    const auto& midi = track.midi;
    const auto reach = (int) std::round (0.075 / track.hop);
    for (const auto& n : notes)
    {
        if (n.straighten < 1.0e-3f && std::abs (n.vibrato - 1.0f) < 1.0e-3f)
            continue;
        const auto a = juce::jmax (0, (int) std::ceil (n.start / track.hop));
        const auto b = juce::jmin (track.size(), (int) std::floor (n.end / track.hop) + 1);
        for (auto i = a; i < b; ++i)
        {
            if (midi[(size_t) i] <= 0.0f)
                continue;
            auto sum = 0.0f;
            auto count = 0;
            for (auto k = juce::jmax (a, i - reach); k < juce::jmin (b, i + reach + 1); ++k)
                if (midi[(size_t) k] > 0.0f)
                    sum += midi[(size_t) k], ++count;
            const auto smooth = sum / (float) count;
            const auto drift = smooth - n.pitch, vibrato = midi[(size_t) i] - smooth;
            const auto t = track.timeOf (i);
            const auto fade = (float) juce::jlimit (0.0, 1.0, juce::jmin (t - n.start, n.end - t) / 0.02);
            shapes[(size_t) i].semitones += fade * (-n.straighten * drift + (n.vibrato - 1.0f) * vibrato);
        }
    }

    return shapes;
}

Shape shapeAt (const Track& track, const std::vector<Shape>& shapes, double seconds)
{
    if (shapes.empty())
        return {};
    const auto position = juce::jlimit (0.0, (double) (shapes.size() - 1), seconds / track.hop);
    const auto i = (int) position;
    const auto j = juce::jmin (i + 1, (int) shapes.size() - 1);
    const auto f = (float) (position - i);
    const auto& a = shapes[(size_t) i];
    const auto& b = shapes[(size_t) j];
    return { a.semitones + f * (b.semitones - a.semitones), a.formant + f * (b.formant - a.formant), a.gainDb + f * (b.gainDb - a.gainDb) };
}
}
