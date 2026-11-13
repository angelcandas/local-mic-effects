#include "Warp.h"

#include <juce_dsp/juce_dsp.h>

#include "dsp/Biquad.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstring>
#include <limits>
#include <mutex>

namespace daw::warp
{
//==============================================================================
Map::Map (std::vector<Marker> input)
{
    std::sort (input.begin(), input.end(), [] (const Marker& a, const Marker& b) { return a.source < b.source; });

    for (const auto& marker : input)
    {
        if (! std::isfinite (marker.source) || ! std::isfinite (marker.target))
            continue;

        if (! markers.empty())
        {
            const auto& last = markers.back();
            const auto ds = marker.source - last.source, dt = marker.target - last.target;

            if (ds < 1.0e-4 || dt < 1.0e-4 || dt / ds > maxStretch || ds / dt > maxStretch)
                continue;
        }

        markers.push_back (marker);
    }
}

bool Map::isIdentity() const noexcept
{
    return std::all_of (markers.begin(), markers.end(), [] (const Marker& m) { return std::abs (m.target - m.source) < 1.0e-6; });
}

double Map::toTarget (double s) const noexcept
{
    if (markers.empty())
        return s;

    if (s <= markers.front().source) return s + markers.front().target - markers.front().source;
    if (s >= markers.back().source)  return s + markers.back().target - markers.back().source;

    const auto next = std::upper_bound (markers.begin(), markers.end(), s, [] (double v, const Marker& m) { return v < m.source; });
    const auto& b = *next;
    const auto& a = *(next - 1);
    return a.target + (s - a.source) * (b.target - a.target) / (b.source - a.source);
}

double Map::toSource (double t) const noexcept
{
    if (markers.empty())
        return t;

    if (t <= markers.front().target) return t - markers.front().target + markers.front().source;
    if (t >= markers.back().target)  return t - markers.back().target + markers.back().source;

    const auto next = std::upper_bound (markers.begin(), markers.end(), t, [] (double v, const Marker& m) { return v < m.target; });
    const auto& b = *next;
    const auto& a = *(next - 1);
    return a.source + (t - a.target) * (b.source - a.source) / (b.target - a.target);
}

double Map::getRateAt (double t) const noexcept
{
    if (markers.size() < 2 || t <= markers.front().target || t >= markers.back().target)
        return 1.0;

    const auto next = std::upper_bound (markers.begin(), markers.end(), t, [] (double v, const Marker& m) { return v < m.target; });
    const auto& b = *next;
    const auto& a = *(next - 1);
    return (b.source - a.source) / (b.target - a.target);
}

juce::String Map::toString() const
{
    if (isIdentity())
        return {};

    juce::StringArray parts;
    for (const auto& m : markers)
        parts.add (juce::String (m.source, 6) + ":" + juce::String (m.target, 6));
    return parts.joinIntoString (";");
}

Map Map::fromString (const juce::String& text)
{
    std::vector<Marker> parsed;

    for (const auto& part : juce::StringArray::fromTokens (text, ";", {}))
        if (part.containsChar (':'))
            parsed.push_back ({ part.upToFirstOccurrenceOf (":", false, false).getDoubleValue(),
                                part.fromFirstOccurrenceOf (":", false, false).getDoubleValue() });

    return Map (std::move (parsed));
}

juce::String getAlgorithmKey (Algorithm algorithm)
{
    switch (algorithm)
    {
        case Algorithm::solo:      return "solo";
        case Algorithm::drums:     return "drums";
        case Algorithm::efficient: return "efficient";
        case Algorithm::slice:     return "slice";
        case Algorithm::varispeed: return "varispeed";
        case Algorithm::mix:       break;
    }
    return "mix";
}

juce::StringArray getAlgorithmKeys()
{
    return { "mix", "solo", "drums", "efficient", "slice", "varispeed" };
}

Algorithm algorithmFromKey (const juce::String& key)
{
    for (const auto a : { Algorithm::solo, Algorithm::drums, Algorithm::efficient, Algorithm::slice, Algorithm::varispeed })
        if (key == getAlgorithmKey (a))
            return a;
    return Algorithm::mix;
}

Map compose (const Map& inner, const Map& outer)
{
    if (outer.isIdentity() && outer.getMarkers().empty())
        return inner;

    // Corners of either map, in file time.
    std::vector<double> corners;
    for (const auto& m : inner.getMarkers())
        corners.push_back (m.source);
    for (const auto& m : outer.getMarkers())
        corners.push_back (inner.toSource (m.source));
    std::sort (corners.begin(), corners.end());

    std::vector<Marker> markers;
    for (auto s : corners)
        if (markers.empty() || s - markers.back().source > 1.0e-6)
            markers.push_back ({ s, outer.toTarget (inner.toTarget (s)) });

    return Map (std::move (markers));
}

juce::String Settings::getKey() const
{
    // Mix without formants keeps the key it had before there were algorithms.
    auto key = map.toString() + "|" + juce::String (juce::jlimit (-maxSemitones, maxSemitones, semitones), 4);
    if (algorithm != Algorithm::mix)
        key << "|" << getAlgorithmKey (algorithm);
    if (formant)
        key << "|formant";
    if (linked.size() > 1)
        key << "|linked " << juce::String::toHexString (linked.joinIntoString ("\n").hashCode64());
    if (hasNoteEdits())
        key << "|notes " << juce::String::toHexString (analysis::pitch::toString (notes).hashCode64());
    return key;
}

//==============================================================================
namespace
{
    using Complex = juce::dsp::Complex<float>;

    float princarg (float phase) noexcept
    {
        const auto twoPi = juce::MathConstants<float>::twoPi;
        return phase - twoPi * std::round (phase / twoPi);
    }

    // Catmull-Rom between y[i] and y[i + 1].
    float interpolate (const float* y, int size, double position) noexcept
    {
        const auto i = (int) std::floor (position);
        const auto f = (float) (position - i);
        const auto at = [&] (int n) { return n >= 0 && n < size ? y[n] : 0.0f; };
        const auto a = at (i - 1), b = at (i), c = at (i + 1), d = at (i + 2);
        return b + 0.5f * f * (c - a + f * (2.0f * a - 5.0f * b + 4.0f * c - d + f * (3.0f * (b - c) + d - a)));
    }

    // Two-pole-pair low-pass in place, against aliasing when audio is read faster.
    void lowPass (juce::AudioBuffer<float>& buffer, double sampleRate, double cutoff)
    {
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        {
            fx::Biquad first, second;
            first.setup (fx::Biquad::Type::lowPass, sampleRate, cutoff, 0.5412);
            second.setup (fx::Biquad::Type::lowPass, sampleRate, cutoff, 1.3066);

            auto* y = buffer.getWritePointer (ch);
            for (int i = 0; i < buffer.getNumSamples(); ++i)
                y[i] = second.process (0, first.process (0, y[i]));
        }
    }

    // Reads a copy made `ratio` times longer than wanted back `ratio` times faster, which
    // moves its pitch by `ratio` and gives it the wanted length.
    juce::AudioBuffer<float> readBack (juce::AudioBuffer<float>& stretched, double sampleRate, double ratio, int outLength)
    {
        if (ratio > 1.0) // reading faster folds the top down: filter it off first
            lowPass (stretched, sampleRate, 0.45 * sampleRate / ratio);

        juce::AudioBuffer<float> result (stretched.getNumChannels(), outLength);

        for (int ch = 0; ch < stretched.getNumChannels(); ++ch)
        {
            const auto* y = stretched.getReadPointer (ch);
            auto* out = result.getWritePointer (ch);

            if (std::abs (ratio - 1.0) < 1.0e-9)
                for (int j = 0; j < outLength; ++j)
                    out[j] = j < stretched.getNumSamples() ? y[j] : 0.0f;
            else
                for (int j = 0; j < outLength; ++j)
                    out[j] = interpolate (y, stretched.getNumSamples(), j * ratio);
        }

        return result;
    }

    // How the pitch moves over the output: the read-back's speed at each moment (the pitch
    // ratio) and where it has got to in the stretched copy. Steady for a plain transpose;
    // following the note edits otherwise, on a grid of `step` output samples.
    struct Path
    {
        double ratio = 1.0;          // steady
        int step = 64;
        std::vector<double> position; // stretched-copy sample at output sample j * step (when it varies)
        std::vector<float> ratios, formants;
        double highest = 1.0;

        bool varies() const noexcept { return ! position.empty(); }

        double stretchedAt (double out) const noexcept
        {
            if (! varies())
                return out * ratio;
            const auto g = out / step;
            if (g <= 0.0)
                return out * ratios.front();
            const auto i = (size_t) g;
            if (i + 1 >= position.size())
                return position.back() + (out - (double) (position.size() - 1) * step) * ratios.back();
            return position[i] + (g - (double) i) * (position[i + 1] - position[i]);
        }

        double outAt (double stretched) const noexcept
        {
            if (! varies())
                return stretched / ratio;
            if (stretched <= 0.0)
                return stretched / ratios.front();
            if (stretched >= position.back())
                return (double) (position.size() - 1) * step + (stretched - position.back()) / ratios.back();
            const auto next = (size_t) (std::upper_bound (position.begin(), position.end(), stretched) - position.begin());
            const auto a = position[next - 1], b = position[next];
            return ((double) (next - 1) + (b > a ? (stretched - a) / (b - a) : 0.0)) * step;
        }

        float ratioAt (double out) const noexcept { return varies() ? ratios[(size_t) juce::jlimit (0, (int) ratios.size() - 1, (int) std::lround (out / step))] : (float) ratio; }
        float formantAt (double out) const noexcept { return varies() ? formants[(size_t) juce::jlimit (0, (int) formants.size() - 1, (int) std::lround (out / step))] : 1.0f; }
    };

    juce::AudioBuffer<float> readBack (juce::AudioBuffer<float>& stretched, double sampleRate, const Path& path, int outLength)
    {
        if (! path.varies())
            return readBack (stretched, sampleRate, path.ratio, outLength);

        if (path.highest > 1.0)
            lowPass (stretched, sampleRate, 0.45 * sampleRate / path.highest);

        juce::AudioBuffer<float> result (stretched.getNumChannels(), outLength);
        for (int ch = 0; ch < stretched.getNumChannels(); ++ch)
        {
            const auto* y = stretched.getReadPointer (ch);
            auto* out = result.getWritePointer (ch);
            for (int j = 0; j < outLength; ++j)
                out[j] = interpolate (y, stretched.getNumSamples(), path.stretchedAt (j));
        }
        return result;
    }

    struct VocoderOptions
    {
        double windowSeconds = 0.046;
        double attackRatio = 4.0;  // high-weighted energy jump that counts as an attack
        int overlap = 4;           // frames per window length
        bool formant = false;
    };

    // The phase vocoder: writes a copy `ratio` times longer than the output, following the
    // map, then reads it back to move the pitch. With `formant`, each frame's spectral
    // envelope is put back where it was before the read-back moves it.
    juce::AudioBuffer<float> vocode (const juce::AudioBuffer<float>& source, double sampleRate, const Map& map, const Path& path,
                                     int outLength, const VocoderOptions& options, const std::function<bool()>& shouldContinue)
    {
        const auto numChannels = juce::jmax (1, source.getNumChannels());
        const auto numSource = source.getNumSamples();

        const auto order = juce::jlimit (9, 13, (int) std::round (std::log2 (sampleRate * options.windowSeconds)));
        const int size = 1 << order, half = size / 2, hop = size / juce::jlimit (4, 16, options.overlap);
        const auto stretchedLength = (int) std::ceil (path.stretchedAt (outLength)) + 4;
        const auto overlapGain = 0.375f * (float) (size / hop); // Hann² summed over the frames (1.5 at 75% overlap)

        juce::dsp::FFT fft (order);
        std::vector<float> window ((size_t) size);
        for (int i = 0; i < size; ++i)
            window[(size_t) i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) size);

        juce::AudioBuffer<float> stretched (numChannels, stretchedLength + size);
        stretched.clear();

        const auto bins = half + 1;
        std::vector<std::vector<Complex>> now ((size_t) numChannels, std::vector<Complex> ((size_t) bins)),
                                          before ((size_t) numChannels, std::vector<Complex> ((size_t) bins));
        std::vector<Complex> mixBefore ((size_t) bins);
        std::vector<float> fftData ((size_t) size * 2), cepstrum ((size_t) size * 2);
        std::vector<float> magnitude ((size_t) bins), phaseNow ((size_t) bins), theta ((size_t) bins), lastTheta ((size_t) bins);
        std::vector<float> logEnvelope ((size_t) bins), formantGain ((size_t) bins, 1.0f);
        std::vector<int> peaks, nearestPeak ((size_t) bins);
        bool fresh = true;
        double lastEnergy = 0.0;

        // The envelope's detail: about 1 ms of quefrency (finer than any voice's pitch period)
        // unless the frame shows its pitch.
        const auto baseLifter = juce::jlimit (4, half / 2, (int) std::round (sampleRate * 0.001));
        auto lifter = baseLifter;

        const auto analyse = [&] (int channel, double centre, std::vector<Complex>& out)
        {
            const auto start = (int) std::llround (centre) - half;
            const auto* data = source.getReadPointer (juce::jmin (channel, source.getNumChannels() - 1));

            for (int i = 0; i < size; ++i)
            {
                const auto n = start + i;
                fftData[(size_t) i] = n >= 0 && n < numSource ? data[n] * window[(size_t) i] : 0.0f;
            }

            fft.performRealOnlyForwardTransform (fftData.data(), true);
            for (int k = 0; k < bins; ++k)
                out[(size_t) k] = { fftData[(size_t) (2 * k)], fftData[(size_t) (2 * k + 1)] };
        };

        // The frame's spectral envelope ("true envelope": a cepstrally smoothed log spectrum,
        // raised again to the peaks a few times so the harmonics, not the gaps between them,
        // set it), then the gain per bin that puts the envelope at k * ratio back at k once
        // the read-back has moved it.
        std::vector<float> logMagnitude ((size_t) bins);
        const auto shapeFrame = [&] (double ratio, double formantFactor)
        {
            auto loudest = 0.0f;
            for (auto m : magnitude)
                loudest = juce::jmax (loudest, m);
            const auto floor = loudest * 1.0e-3f + 1.0e-12f;

            for (int k = 0; k < bins; ++k)
                logEnvelope[(size_t) k] = logMagnitude[(size_t) k] = std::log (juce::jmax (floor, magnitude[(size_t) k]));

            for (int pass = 0; pass < 6; ++pass)
            {
                std::fill (cepstrum.begin(), cepstrum.end(), 0.0f);
                for (int k = 0; k < bins; ++k)
                    cepstrum[(size_t) (2 * k)] = logEnvelope[(size_t) k];

                fft.performRealOnlyInverseTransform (cepstrum.data());

                // A clear pitch period in the cepstrum lets the envelope be finer: smooth
                // over a little less than one period, so the formants keep their shape.
                if (pass == 0)
                {
                    lifter = baseLifter;
                    const auto shortest = (int) std::round (sampleRate / 600.0), longest = juce::jmin (half - 1, (int) std::round (sampleRate / 70.0));
                    auto best = -1;
                    auto bestValue = 0.0f, sum = 0.0f;
                    for (int n = shortest; n <= longest; ++n)
                    {
                        sum += std::abs (cepstrum[(size_t) n]);
                        if (cepstrum[(size_t) n] > bestValue)
                            bestValue = cepstrum[(size_t) n], best = n;
                    }
                    const auto mean = sum / (float) juce::jmax (1, longest - shortest + 1);
                    if (best > 0 && bestValue > 4.0f * mean)
                        lifter = juce::jmax (baseLifter, (int) std::round (best * 0.6));
                }

                for (int n = lifter; n <= size - lifter; ++n)
                    cepstrum[(size_t) n] = 0.0f;
                std::fill (cepstrum.begin() + size, cepstrum.end(), 0.0f);
                fft.performRealOnlyForwardTransform (cepstrum.data(), true);

                for (int k = 0; k < bins; ++k)
                    logEnvelope[(size_t) k] = pass < 5 ? juce::jmax (logMagnitude[(size_t) k], cepstrum[(size_t) (2 * k)])
                                                       : cepstrum[(size_t) (2 * k)];
            }

            for (int k = 0; k < bins; ++k)
            {
                const auto at = juce::jmin ((double) (bins - 1), k * ratio / formantFactor);
                const auto i = (int) at;
                const auto f = (float) (at - i);
                const auto wanted = logEnvelope[(size_t) i] + f * (logEnvelope[(size_t) juce::jmin (i + 1, bins - 1)] - logEnvelope[(size_t) i]);
                formantGain[(size_t) k] = std::exp (juce::jlimit (-4.0f, 4.0f, wanted - logEnvelope[(size_t) k]));
            }
        };

        const auto firstFrame = -3, lastFrame = stretchedLength / hop + 3;

        for (int frame = firstFrame; frame <= lastFrame; ++frame)
        {
            if (shouldContinue && ((frame - firstFrame) & 63) == 0 && ! shouldContinue())
                return {};

            const auto centre = (double) frame * hop;                                       // in the stretched copy
            const auto outCentre = path.outAt (centre);                                    // in the output
            const auto sourceCentre = map.toSource (outCentre / sampleRate) * sampleRate;     // in the file

            if (sourceCentre < -size || sourceCentre > numSource + size)
            {
                fresh = true; // nothing to hear: start the phases again when the audio comes in
                continue;
            }

            double energyNow = 0.0, energyBefore = 0.0;

            for (int ch = 0; ch < numChannels; ++ch)
            {
                analyse (ch, sourceCentre, now[(size_t) ch]);
                analyse (ch, sourceCentre - hop, before[(size_t) ch]);
            }

            for (int k = 0; k < bins; ++k)
            {
                Complex a {}, b {};
                for (int ch = 0; ch < numChannels; ++ch)
                {
                    a += now[(size_t) ch][(size_t) k];
                    b += before[(size_t) ch][(size_t) k];
                }

                mixBefore[(size_t) k] = b;
                magnitude[(size_t) k] = std::abs (a);
                phaseNow[(size_t) k] = std::arg (a);
                energyNow += std::norm (a) * k;  // weighted towards the highs, where attacks show
                energyBefore += std::norm (b) * k;
            }

            // An attack: take the analysis phases as they are, so it stays sharp instead of
            // being smeared by the phase propagation.
            const auto attack = energyNow > options.attackRatio * energyBefore && energyNow > 1.0e-3 * lastEnergy && energyNow > 1.0e-6;
            lastEnergy = juce::jmax (lastEnergy * 0.9, energyNow);

            peaks.clear();
            for (int k = 2; k < bins - 2; ++k)
            {
                const auto m = magnitude[(size_t) k];
                if (m > 1.0e-6f && m > magnitude[(size_t) k - 1] && m >= magnitude[(size_t) k + 1]
                    && m > magnitude[(size_t) k - 2] && m >= magnitude[(size_t) k + 2])
                    peaks.push_back (k);
            }

            if (fresh || attack || peaks.empty())
            {
                theta = phaseNow;
            }
            else
            {
                // Phase locking: each peak's phase moves on at its measured frequency, and the
                // bins around it keep their phase relative to the peak.
                const auto twoPi = juce::MathConstants<float>::twoPi;
                for (auto k : peaks)
                {
                    const auto expected = twoPi * (float) k * (float) hop / (float) size;
                    const auto deviation = princarg (phaseNow[(size_t) k] - std::arg (mixBefore[(size_t) k]) - expected);
                    theta[(size_t) k] = princarg (lastTheta[(size_t) k] + expected + deviation);
                }

                size_t p = 0;
                for (int k = 0; k < bins; ++k)
                {
                    while (p + 1 < peaks.size() && std::abs (peaks[p + 1] - k) < std::abs (peaks[p] - k))
                        ++p;
                    nearestPeak[(size_t) k] = peaks[p];
                }

                for (int k = 0; k < bins; ++k)
                {
                    const auto peak = (size_t) nearestPeak[(size_t) k];
                    if ((int) peak != k)
                        theta[(size_t) k] = theta[peak] + phaseNow[(size_t) k] - phaseNow[peak];
                }
            }

            fresh = false;
            lastTheta = theta;

            // Formants: kept where they were (or moved by the note's formant shift) when the
            // read-back moves the pitch.
            if (const auto frameRatio = path.ratioAt (outCentre), formantFactor = path.formantAt (outCentre);
                options.formant && (std::abs (frameRatio - 1.0f) > 1.0e-6f || std::abs (formantFactor - 1.0f) > 1.0e-6f))
                shapeFrame (frameRatio, formantFactor);
            else
                std::fill (formantGain.begin(), formantGain.end(), 1.0f);

            // Synthesis: each channel keeps its own magnitude and its phase relative to the mix,
            // so the stereo picture survives.
            const auto writeStart = (int) centre - half;

            for (int ch = 0; ch < numChannels; ++ch)
            {
                const auto& spectrum = now[(size_t) ch];

                std::fill (fftData.begin(), fftData.end(), 0.0f);
                for (int k = 0; k < bins; ++k)
                {
                    const auto rotate = theta[(size_t) k] - phaseNow[(size_t) k];
                    const auto x = spectrum[(size_t) k] * Complex (std::cos (rotate), std::sin (rotate)) * formantGain[(size_t) k];
                    fftData[(size_t) (2 * k)] = x.real();
                    fftData[(size_t) (2 * k + 1)] = x.imag();
                }

                fft.performRealOnlyInverseTransform (fftData.data());

                auto* out = stretched.getWritePointer (ch);
                for (int i = 0; i < size; ++i)
                    if (const auto n = writeStart + i; n >= 0 && n < stretched.getNumSamples())
                        out[n] += fftData[(size_t) i] * window[(size_t) i] / overlapGain;
            }
        }

        return readBack (stretched, sampleRate, path, outLength);
    }

    // Overlap-add in the time domain (WSOLA): 40 ms grains taken where the map says, each
    // nudged by up to 12 ms to the spot that best continues the grain before it. The nudge
    // is chosen on the channels together, so they stay in phase.
    juce::AudioBuffer<float> overlapAdd (const juce::AudioBuffer<float>& source, double sampleRate, const Map& map, double ratio,
                                         int outLength, const std::function<bool()>& shouldContinue)
    {
        const auto numChannels = juce::jmax (1, source.getNumChannels());
        const auto numSource = source.getNumSamples();
        const auto size = 2 * juce::jmax (64, (int) std::round (sampleRate * 0.02)), hop = size / 2;
        const auto tolerance = juce::jmax (8, (int) std::round (sampleRate * 0.012));
        const auto stretchedLength = (int) std::ceil (outLength * ratio) + 4;

        std::vector<float> window ((size_t) size);
        for (int i = 0; i < size; ++i)
            window[(size_t) i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) size); // sums to 1 at 50%

        std::vector<float> mono ((size_t) numSource, 0.0f);
        for (int ch = 0; ch < source.getNumChannels(); ++ch)
            juce::FloatVectorOperations::add (mono.data(), source.getReadPointer (ch), numSource);

        const auto monoAt = [&] (int n) { return n >= 0 && n < numSource ? mono[(size_t) n] : 0.0f; };

        // How alike two stretches of the mix are (normalised, so loudness doesn't decide).
        const auto similarity = [&] (int a, int b, int step)
        {
            double cross = 0.0, energy = 1.0e-12;
            for (int i = 0; i < size; i += step)
            {
                const auto x = monoAt (a + i), y = monoAt (b + i);
                cross += x * y;
                energy += y * y;
            }
            return cross / std::sqrt (energy);
        };

        juce::AudioBuffer<float> stretched (numChannels, stretchedLength + size);
        stretched.clear();

        auto previous = std::numeric_limits<int>::min();
        const auto firstFrame = -2, lastFrame = stretchedLength / hop + 2;

        for (int frame = firstFrame; frame <= lastFrame; ++frame)
        {
            if (shouldContinue && ((frame - firstFrame) & 255) == 0 && ! shouldContinue())
                return {};

            const auto centre = frame * hop;
            const auto ideal = (int) std::llround (map.toSource (centre / ratio / sampleRate) * sampleRate) - hop;

            if (ideal + size < -tolerance || ideal > numSource + tolerance)
            {
                previous = std::numeric_limits<int>::min();
                continue;
            }

            auto start = ideal;

            if (previous != std::numeric_limits<int>::min())
            {
                const auto natural = previous + hop; // what would follow the last grain seamlessly

                if (natural != ideal && std::abs (natural - ideal) < 4 * tolerance)
                {
                    auto best = ideal;
                    auto bestScore = -std::numeric_limits<double>::max();

                    for (int d = -tolerance; d <= tolerance; d += 4) // coarse
                        if (const auto score = similarity (natural, ideal + d, 8); score > bestScore)
                            bestScore = score, best = ideal + d;

                    const auto coarse = best;
                    bestScore = -std::numeric_limits<double>::max();
                    for (int d = -3; d <= 3; ++d) // fine
                        if (const auto score = similarity (natural, coarse + d, 1); score > bestScore)
                            bestScore = score, best = coarse + d;

                    start = best;
                }
            }

            previous = start;

            const auto writeStart = centre - hop;
            for (int ch = 0; ch < numChannels; ++ch)
            {
                const auto* in = source.getReadPointer (juce::jmin (ch, source.getNumChannels() - 1));
                auto* out = stretched.getWritePointer (ch);

                for (int i = 0; i < size; ++i)
                {
                    const auto n = writeStart + i, s = start + i;
                    if (n >= 0 && n < stretched.getNumSamples() && s >= 0 && s < numSource)
                        out[n] += in[s] * window[(size_t) i];
                }
            }
        }

        return readBack (stretched, sampleRate, ratio, outLength);
    }

    // Like tape: each moment of the output reads the file where the map says, so time and
    // pitch change together.
    juce::AudioBuffer<float> varispeed (const juce::AudioBuffer<float>& source, double sampleRate, const Map& map, int outLength)
    {
        auto fastest = 1.0;
        const auto& markers = map.getMarkers();
        for (size_t i = 1; i < markers.size(); ++i)
            fastest = juce::jmax (fastest, (markers[i].source - markers[i - 1].source) / (markers[i].target - markers[i - 1].target));

        juce::AudioBuffer<float> filtered (source);
        if (fastest > 1.0001)
            lowPass (filtered, sampleRate, 0.45 * sampleRate / fastest);

        juce::AudioBuffer<float> result (filtered.getNumChannels(), outLength);
        std::vector<double> positions ((size_t) outLength);
        for (int j = 0; j < outLength; ++j)
            positions[(size_t) j] = map.toSource (j / sampleRate) * sampleRate;

        for (int ch = 0; ch < filtered.getNumChannels(); ++ch)
        {
            const auto* in = filtered.getReadPointer (ch);
            auto* out = result.getWritePointer (ch);
            for (int j = 0; j < outLength; ++j)
                out[j] = interpolate (in, filtered.getNumSamples(), positions[(size_t) j]);
        }

        return result;
    }

    // No stretching: each piece of the file between markers plays at normal speed from
    // where its first marker landed, until its own end or the next piece's start (with
    // short fades). Moving pieces apart leaves silent gaps; together, they're cut short.
    juce::AudioBuffer<float> slice (const juce::AudioBuffer<float>& source, double sampleRate, const Map& map, int outLength)
    {
        const auto numSource = source.getNumSamples();
        const auto end = numSource / sampleRate;

        // Piece boundaries in the file, and how far each piece moves.
        std::vector<double> bounds { 0.0 }, shifts;
        for (const auto& m : map.getMarkers())
            if (m.source > bounds.back() + 1.0e-6 && m.source < end)
                bounds.push_back (m.source);
        bounds.push_back (end);

        for (size_t i = 0; i + 1 < bounds.size(); ++i)
            shifts.push_back (map.toTarget (bounds[i]) - bounds[i]);

        juce::AudioBuffer<float> result (source.getNumChannels(), outLength);
        result.clear();

        const auto fadeIn = juce::jmax (1, (int) std::round (sampleRate * 0.001));
        const auto fadeOutLength = juce::jmax (1, (int) std::round (sampleRate * 0.004));

        for (size_t i = 0; i < shifts.size(); ++i)
        {
            const auto from = (int) std::llround (bounds[i] * sampleRate);
            const auto shift = shifts[i];
            const auto nextShift = i + 1 < shifts.size() ? shifts[i + 1] : shift;
            const auto to = (int) std::llround ((bounds[i + 1] + juce::jmin (0.0, nextShift - shift)) * sampleRate);
            const auto length = juce::jmin (to, numSource) - from;
            const auto offset = (int) std::llround (shift * sampleRate);

            if (length <= 0)
                continue;

            const auto fadeOut = juce::jmin (fadeOutLength, length / 2);

            for (int ch = 0; ch < source.getNumChannels(); ++ch)
            {
                const auto* in = source.getReadPointer (ch);
                auto* out = result.getWritePointer (ch);

                for (int n = 0; n < length; ++n)
                {
                    const auto at = from + n + offset;
                    if (at < 0 || at >= outLength)
                        continue;

                    auto gain = 1.0f;
                    if (from > 0 && n < fadeIn)  gain *= (float) n / (float) fadeIn;
                    if (to < numSource && n >= length - fadeOut) gain *= (float) (length - n) / (float) fadeOut;
                    out[at] += in[from + n] * gain;
                }
            }
        }

        return result;
    }
}

//==============================================================================
namespace
{
    // The map with every marker's target moved by `by` seconds (so the copy's time t is
    // the new map's t - by). No markers: the identity, moved.
    Map shifted (const Map& map, double by)
    {
        if (map.getMarkers().empty())
            return Map ({ { 0.0, by } });

        auto markers = map.getMarkers();
        for (auto& m : markers)
            m.target += by;
        return Map (std::move (markers));
    }

    juce::int64 hashSamples (const juce::AudioBuffer<float>& audio, int from, int to)
    {
        auto hash = (juce::int64) 1469598103934665603LL;
        for (int ch = 0; ch < audio.getNumChannels(); ++ch)
        {
            const auto* data = audio.getReadPointer (ch);
            for (int i = juce::jmax (0, from); i < juce::jmin (to, audio.getNumSamples()); ++i)
            {
                juce::uint32 bits;
                std::memcpy (&bits, data + i, sizeof (bits));
                hash = (hash ^ (juce::int64) bits) * 1099511628211LL;
            }
            hash = (hash ^ (juce::int64) (from + 7 * ch)) * 1099511628211LL;
        }
        return hash;
    }

    juce::int64 sparseHash (const juce::AudioBuffer<float>& source, double sampleRate)
    {
        auto hash = (juce::int64) source.getNumSamples() * 31 + source.getNumChannels() * 7 + (juce::int64) sampleRate;
        const auto stride = juce::jmax (1, source.getNumSamples() / 4096);
        for (int ch = 0; ch < source.getNumChannels(); ++ch)
            for (int i = 0; i < source.getNumSamples(); i += stride)
                hash = hash * 1099511628211LL + (juce::int64) std::lround (source.getSample (ch, i) * 1.0e6f);
        return hash;
    }
}

std::vector<int> findSeams (const juce::AudioBuffer<float>& source, double sampleRate)
{
    const auto numSamples = source.getNumSamples();
    std::vector<int> seams { 0 };

    const auto spacing = Render::seamSpacing * sampleRate, search = Render::seamSearch * sampleRate;
    if (numSamples > 0 && numSamples >= spacing * 1.5)
    {
        // Energy of the channels' sum, summed up so any 10 ms is quick to read.
        std::vector<double> energy ((size_t) numSamples + 1, 0.0);
        for (int i = 0; i < numSamples; ++i)
        {
            auto x = 0.0f;
            for (int ch = 0; ch < source.getNumChannels(); ++ch)
                x += source.getSample (ch, i);
            energy[(size_t) i + 1] = energy[(size_t) i] + (double) x * x;
        }

        const auto half = juce::jmax (1, (int) std::round (sampleRate * 0.005));
        const auto step = juce::jmax (1, (int) std::round (sampleRate * 0.001));

        for (int k = 1; k * spacing + spacing * 0.5 <= numSamples; ++k)
        {
            const auto centre = (int) std::round (k * spacing);
            auto best = centre;
            auto quietest = std::numeric_limits<double>::max();
            for (int at = centre - (int) search; at <= centre + (int) search; at += step)
            {
                const auto a = juce::jlimit (0, numSamples, at - half), b = juce::jlimit (0, numSamples, at + half);
                // Ties (digital silence) go to the spot nearest the even spacing.
                const auto e = energy[(size_t) b] - energy[(size_t) a] + 1.0e-12 * std::abs (at - centre);
                if (e < quietest)
                    quietest = e, best = at;
            }

            if (best > seams.back() && best < numSamples)
                seams.push_back (best);
        }
    }

    seams.push_back (numSamples);
    return seams;
}

std::shared_ptr<const std::vector<int>> findSeamsCached (const juce::AudioBuffer<float>& source, double sampleRate)
{
    const auto hash = sparseHash (source, sampleRate);

    static std::mutex lock;
    static std::vector<std::pair<juce::int64, std::shared_ptr<const std::vector<int>>>> cache;
    {
        const std::lock_guard<std::mutex> guard (lock);
        for (const auto& [key, seams] : cache)
            if (key == hash)
                return seams;
    }

    auto seams = std::make_shared<const std::vector<int>> (findSeams (source, sampleRate));
    const std::lock_guard<std::mutex> guard (lock);
    cache.emplace_back (hash, seams);
    if (cache.size() > 12)
        cache.erase (cache.begin());
    return seams;
}

//==============================================================================
// Everything one render shares between its pieces.
struct Render::Context
{
    enum class Kind { copy, vocoder, wsola };

    std::shared_ptr<const juce::AudioBuffer<float>> source; // the file
    juce::AudioBuffer<float> placed;                         // slice/varispeed: the file already placed (then the stretcher's input)
    bool usePlaced = false;
    double sampleRate = 48000.0;
    Settings settings;
    Kind kind = Kind::copy;
    VocoderOptions options;
    double ratio = 1.0;
    bool edits = false, gains = false;
    std::shared_ptr<const analysis::pitch::Track> track;
    std::vector<analysis::pitch::Shape> shapes;
    int outLength = 0;
    int preRoll = 0;   // output samples rendered before a piece starts
    double reach = 0.0; // seconds of the map past a piece's ends that its frames look at

    const juce::AudioBuffer<float>& input() const noexcept { return usePlaced ? placed : *source; }
    Map stretchMap() const { return usePlaced ? Map() : settings.map; }

    analysis::pitch::Shape shapeAt (double copySeconds) const
    {
        return edits ? analysis::pitch::shapeAt (*track, shapes, settings.map.toSource (copySeconds)) : analysis::pitch::Shape {};
    }

    // The pitch path for `length` output samples from `start` seconds of the copy.
    Path makePath (double start, int length) const
    {
        Path path;
        path.ratio = ratio;
        if (! edits)
            return path;

        const auto count = (size_t) (length / path.step + 2);
        path.position.assign (count, 0.0);
        path.ratios.assign (count, 1.0f);
        path.formants.assign (count, 1.0f);
        path.highest = 0.0;
        for (size_t j = 0; j < count; ++j)
        {
            const auto shape = shapeAt (start + (double) j * path.step / sampleRate);
            const auto semitones = juce::jlimit (-Settings::maxSemitones, Settings::maxSemitones, settings.semitones + (double) shape.semitones);
            path.ratios[j] = (float) std::pow (2.0, semitones / 12.0);
            path.formants[j] = (float) std::pow (2.0, shape.formant / 12.0);
            path.highest = juce::jmax (path.highest, (double) path.ratios[j]);
            if (j > 0)
                path.position[j] = path.position[j - 1] + path.step * 0.5 * ((double) path.ratios[j - 1] + path.ratios[j]);
        }
        return path;
    }

    // The notes' levels, on finished audio that starts `start` seconds into the copy.
    void applyGains (juce::AudioBuffer<float>& out, double start) const
    {
        if (! gains)
            return;
        constexpr int step = 64;
        for (int j0 = 0; j0 < out.getNumSamples(); j0 += step)
        {
            const auto a = juce::Decibels::decibelsToGain (shapeAt (start + j0 / sampleRate).gainDb);
            const auto b = juce::Decibels::decibelsToGain (shapeAt (start + (j0 + step) / sampleRate).gainDb);
            const auto n = juce::jmin (step, out.getNumSamples() - j0);
            for (int ch = 0; ch < out.getNumChannels(); ++ch)
                out.applyGainRamp (ch, j0, n, a, a + (b - a) * (float) n / (float) step);
        }
    }

    // `length` samples of the copy from `start` seconds (which may be before 0).
    juce::AudioBuffer<float> renderRange (double start, int length, const std::function<bool()>& shouldContinue) const
    {
        juce::AudioBuffer<float> out;
        switch (kind)
        {
            case Kind::copy:
            {
                out.setSize (input().getNumChannels(), length);
                out.clear();
                const auto from = (int) std::llround (start * sampleRate);
                for (int ch = 0; ch < out.getNumChannels(); ++ch)
                    for (int i = juce::jmax (0, -from); i < length && from + i < input().getNumSamples(); ++i)
                        out.setSample (ch, i, input().getSample (ch, from + i));
                break;
            }
            case Kind::vocoder:
                out = vocode (input(), sampleRate, shifted (stretchMap(), -start), makePath (start, length), length, options, shouldContinue);
                break;
            case Kind::wsola:
                out = overlapAdd (input(), sampleRate, shifted (stretchMap(), -start), ratio, length, shouldContinue);
                break;
        }

        if (out.getNumSamples() > 0)
            applyGains (out, start);
        return out;
    }
};

Render::Render (std::shared_ptr<const juce::AudioBuffer<float>> sourceIn, double sampleRate, const Settings& settings)
{
    auto context = std::make_shared<Context>();
    auto& c = *context;
    c.source = std::move (sourceIn);
    c.sampleRate = sampleRate;
    c.settings = settings;
    this->context = context;

    const auto numSource = c.source != nullptr ? c.source->getNumSamples() : 0;
    length = numSource;

    if (c.source == nullptr || settings.isNeutral() || numSource == 0 || c.source->getNumChannels() == 0)
        return; // a plain copy

    const auto& map = settings.map;
    c.ratio = std::pow (2.0, juce::jlimit (-Settings::maxSemitones, Settings::maxSemitones, settings.semitones) / 12.0);
    c.outLength = length = (int) juce::jmax (0.0, std::round (map.toTarget (numSource / sampleRate) * sampleRate));
    c.options.formant = settings.formant;

    // Note edits: the pitch moves along the notes' shapes (looked up in file time), and the
    // voice's formants are kept, as each note says.
    c.edits = settings.hasNoteEdits();
    auto steady = std::abs (c.ratio - 1.0) < 1.0e-9; // no pitch or formant moves anywhere
    if (c.edits)
    {
        c.track = analysis::pitch::analyseCached (*c.source, sampleRate);
        c.shapes = analysis::pitch::getShapes (*c.track, settings.notes);
        c.options.formant = true;
        c.gains = std::any_of (settings.notes.begin(), settings.notes.end(), [] (const auto& n) { return std::abs (n.gain) > 1.0e-3f; });

        const auto path = c.makePath (0.0, length);
        steady = std::all_of (path.ratios.begin(), path.ratios.end(), [] (float r) { return std::abs (r - 1.0f) < 1.0e-6f; })
              && std::all_of (path.formants.begin(), path.formants.end(), [] (float f) { return std::abs (f - 1.0f) < 1.0e-6f; });
    }

    // Only levels changed: the audio as it is.
    if (c.edits && steady && map.isIdentity())
    {
        c.kind = Context::Kind::copy;
        return;
    }

    c.kind = Context::Kind::vocoder;
    switch (settings.algorithm)
    {
        case Algorithm::solo:
            c.options.overlap = 8;
            c.options.attackRatio = 3.0;
            break;
        case Algorithm::drums:
            c.options.windowSeconds = 0.0115;
            c.options.attackRatio = 2.0;
            break;
        case Algorithm::efficient:
            if (c.edits) // the time-domain path can't follow the notes: the voice setting does
            {
                c.options.overlap = 8;
                c.options.attackRatio = 3.0;
                break;
            }
            c.kind = Context::Kind::wsola;
            break;
        case Algorithm::slice:
        case Algorithm::varispeed:
            // These place the audio themselves (quickly, in one go); a transpose or note
            // edits then go through the vocoder on the result.
            c.placed = settings.algorithm == Algorithm::slice ? slice (*c.source, sampleRate, map, length)
                                                              : varispeed (*c.source, sampleRate, map, length);
            c.usePlaced = true;
            c.kind = steady ? Context::Kind::copy : Context::Kind::vocoder;
            break;
        case Algorithm::mix:
            break;
    }

    if (c.kind == Context::Kind::copy || length == 0)
        return;

    // Pieces. The pre-roll lets the overlap-add and the read-back's filter settle before
    // the seam; the reach covers the frames just outside a piece (a few hops, at the
    // slowest read-back).
    const auto order = juce::jlimit (9, 13, (int) std::round (std::log2 (sampleRate * c.options.windowSeconds)));
    const auto window = c.kind == Context::Kind::wsola ? 2 * juce::jmax (64, (int) std::round (sampleRate * 0.02)) : 1 << order;
    const auto hop = c.kind == Context::Kind::wsola ? window / 2 : window / juce::jlimit (4, 16, c.options.overlap);
    c.preRoll = juce::jmax ((int) std::round (sampleRate * 0.05), 4 * window);
    c.reach = (4.0 * hop + 8.0) / std::pow (2.0, -Settings::maxSemitones / 12.0) / sampleRate;

    const auto seams = c.usePlaced ? std::make_shared<const std::vector<int>> (findSeams (c.placed, sampleRate))
                                   : findSeamsCached (*c.source, sampleRate);
    const auto fade = (int) std::round (crossfadeSeconds * sampleRate);
    const auto toCopy = [&] (int seam) { return c.usePlaced ? seam / sampleRate : map.toTarget (seam / sampleRate); };
    const auto copySeconds = length / sampleRate;

    for (size_t k = 0; k + 1 < seams->size(); ++k)
    {
        const auto from = toCopy ((*seams)[k]), to = toCopy ((*seams)[k + 1]);
        const auto first = (int) std::llround (from * sampleRate), last = (int) std::llround (to * sampleRate);
        if (last + fade <= 0 || first >= length || last <= first)
            continue; // lands outside the copy

        // The key: only what shapes this stretch of output, relative to where it lands.
        const auto span = to - from;
        const auto renderStart = from - c.preRoll / sampleRate;
        const auto renderLength = c.preRoll + (int) std::ceil (span * sampleRate) + fade + 2;

        juce::String key;
        key << "p" << (int) k << "|" << getAlgorithmKey (settings.algorithm) << (c.options.formant ? "|f" : "")
            << (c.kind == Context::Kind::wsola ? "|w" : "") << "|" << juce::String (juce::jlimit (-Settings::maxSemitones, Settings::maxSemitones, settings.semitones), 4)
            << "|" << juce::String (span, 7);

        const auto lo = -c.preRoll / sampleRate - c.reach, hi = span + (fade + 2) / sampleRate + c.reach;
        if (c.usePlaced)
        {
            key << "|c" << juce::String::toHexString (hashSamples (c.placed, (int) std::floor ((from + lo) * sampleRate) - window,
                                                                    (int) std::ceil ((from + hi) * sampleRate) + window));
        }
        else
        {
            key << "|m" << juce::String (lo, 7) << ":" << juce::String (map.toSource (from + lo), 7);
            for (const auto& m : map.getMarkers())
                if (m.target > from + lo && m.target < from + hi)
                    key << ";" << juce::String (m.target - from, 7) << ":" << juce::String (m.source, 7);
            key << ";" << juce::String (hi, 7) << ":" << juce::String (map.toSource (from + hi), 7);
        }

        if (c.edits)
        {
            // The very shape values this piece's path and levels read.
            auto hash = (juce::int64) 1469598103934665603LL;
            constexpr int step = 64;
            for (int j = 0; j <= renderLength + 2 * step; j += step)
            {
                const auto shape = c.shapeAt (renderStart + j / sampleRate);
                for (const auto v : { shape.semitones, shape.formant, shape.gainDb })
                    hash = (hash ^ (juce::int64) std::lround (v * 1.0e5f)) * 1099511628211LL;
            }
            key << "|n" << juce::String::toHexString (hash);
        }

        pieces.push_back ({ juce::jmax (0.0, from), juce::jmin (copySeconds, to), key });
        starts.push_back (from);
        ends.push_back (to);
    }
}

Render::~Render() = default;

int Render::getNumChannels() const noexcept
{
    return context->source != nullptr ? juce::jmax (1, context->source->getNumChannels()) : 0;
}

juce::AudioBuffer<float> Render::renderPiece (size_t index, const std::function<bool()>& shouldContinue) const
{
    jassert (index < pieces.size());
    const auto& c = *context;
    const auto fade = (int) std::round (crossfadeSeconds * c.sampleRate);
    const auto lengthNeeded = (int) std::ceil ((ends[index] - starts[index]) * c.sampleRate) + fade + 2;
    const auto start = starts[index] - c.preRoll / c.sampleRate;

    auto out = c.renderRange (start, c.preRoll + lengthNeeded, shouldContinue);
    if (out.getNumSamples() == 0)
        return {};

    juce::AudioBuffer<float> piece (out.getNumChannels(), lengthNeeded);
    for (int ch = 0; ch < out.getNumChannels(); ++ch)
        piece.copyFrom (ch, 0, out, ch, c.preRoll, lengthNeeded);
    return piece;
}

juce::AudioBuffer<float> Render::assemble (const std::vector<std::shared_ptr<const juce::AudioBuffer<float>>>& parts) const
{
    const auto& c = *context;
    juce::AudioBuffer<float> out (getNumChannels(), length);
    out.clear();

    const auto fade = (int) std::round (crossfadeSeconds * c.sampleRate);
    for (size_t k = 0; k < pieces.size() && k < parts.size(); ++k)
    {
        if (parts[k] == nullptr)
            continue;

        const auto& part = *parts[k];
        const auto first = (int) std::llround (starts[k] * c.sampleRate);
        const auto next = k + 1 < pieces.size() ? (int) std::llround (starts[k + 1] * c.sampleRate) : length;
        const auto fadeIn = k > 0, fadeOut = k + 1 < pieces.size();
        const auto count = juce::jmin (part.getNumSamples(), next - first + (fadeOut ? fade : 0));

        // Complementary fades (they sum to one): the pieces mostly agree at the seam, so
        // this keeps the level steady where an equal-power fade would bump it.
        const auto gainAt = [&] (int i)
        {
            auto gain = 1.0f;
            if (fadeIn && i < fade)
                gain *= (float) std::pow (std::sin (juce::MathConstants<double>::halfPi * (i + 0.5) / fade), 2.0);
            if (fadeOut && i >= next - first)
                gain *= (float) std::pow (std::cos (juce::MathConstants<double>::halfPi * (i - (next - first) + 0.5) / fade), 2.0);
            return gain;
        };

        for (int ch = 0; ch < out.getNumChannels(); ++ch)
        {
            const auto* in = part.getReadPointer (juce::jmin (ch, part.getNumChannels() - 1));
            auto* dest = out.getWritePointer (ch);
            for (int i = juce::jmax (0, -first); i < count && first + i < length; ++i)
                dest[first + i] += in[i] * gainAt (i);
        }
    }

    return out;
}

juce::AudioBuffer<float> Render::renderWhole (const std::function<bool()>& shouldContinue) const
{
    const auto& c = *context;
    if (c.source == nullptr)
        return {};
    if (c.settings.isNeutral() || c.source->getNumSamples() == 0 || c.source->getNumChannels() == 0)
        return juce::AudioBuffer<float> (*c.source);
    return c.renderRange (0.0, length, shouldContinue);
}

namespace
{
    std::shared_ptr<const juce::AudioBuffer<float>> borrow (const juce::AudioBuffer<float>& source)
    {
        return { &source, [] (const juce::AudioBuffer<float>*) {} };
    }
}

juce::AudioBuffer<float> renderUnsegmented (const juce::AudioBuffer<float>& source, double sampleRate, const Settings& settings,
                                            const std::function<bool()>& shouldContinue)
{
    return Render (borrow (source), sampleRate, settings).renderWhole (shouldContinue);
}

juce::AudioBuffer<float> render (const juce::AudioBuffer<float>& source, double sampleRate, const Settings& settings,
                                 const std::function<bool()>& shouldContinue)
{
    const Render render (borrow (source), sampleRate, settings);
    if (! render.isSegmented())
        return render.renderWhole (shouldContinue);

    std::vector<std::shared_ptr<const juce::AudioBuffer<float>>> parts;
    for (size_t i = 0; i < render.getPieces().size(); ++i)
    {
        auto part = render.renderPiece (i, shouldContinue);
        if (part.getNumSamples() == 0)
            return {};
        parts.push_back (std::make_shared<const juce::AudioBuffer<float>> (std::move (part)));
    }
    return render.assemble (parts);
}

juce::AudioBuffer<float> combineLinked (const std::vector<const juce::AudioBuffer<float>*>& sources)
{
    int channels = 0, longest = 0;
    for (const auto* source : sources)
    {
        channels += source != nullptr ? source->getNumChannels() : 0;
        longest = juce::jmax (longest, source != nullptr ? source->getNumSamples() : 0);
    }

    juce::AudioBuffer<float> together (juce::jmax (1, channels), longest);
    together.clear();
    int channel = 0;
    for (const auto* source : sources)
        if (source != nullptr)
            for (int ch = 0; ch < source->getNumChannels(); ++ch)
                together.copyFrom (channel++, 0, *source, ch, 0, source->getNumSamples());
    return together;
}

std::vector<juce::AudioBuffer<float>> splitLinked (const juce::AudioBuffer<float>& rendered, const std::vector<const juce::AudioBuffer<float>*>& sources,
                                                   double sampleRate, const Settings& settings)
{
    std::vector<juce::AudioBuffer<float>> parts;
    int channel = 0;
    for (const auto* source : sources)
    {
        if (source == nullptr || rendered.getNumSamples() == 0)
        {
            parts.emplace_back();
            continue;
        }

        const auto length = settings.isNeutral()
                                ? source->getNumSamples()
                                : (int) juce::jmax (0.0, std::round (settings.map.toTarget (source->getNumSamples() / sampleRate) * sampleRate));
        juce::AudioBuffer<float> part (source->getNumChannels(), length);
        part.clear();
        for (int ch = 0; ch < source->getNumChannels(); ++ch)
            part.copyFrom (ch, 0, rendered, channel++, 0, juce::jmin (length, rendered.getNumSamples()));
        parts.push_back (std::move (part));
    }

    return parts;
}

std::vector<juce::AudioBuffer<float>> renderLinked (const std::vector<const juce::AudioBuffer<float>*>& sources, double sampleRate,
                                                    const Settings& settings, const std::function<bool()>& shouldContinue)
{
    const auto together = combineLinked (sources);
    return splitLinked (render (together, sampleRate, settings, shouldContinue), sources, sampleRate, settings);
}
}
