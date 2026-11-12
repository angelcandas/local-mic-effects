#include "Warp.h"

#include <juce_dsp/juce_dsp.h>

#include "dsp/Biquad.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <limits>

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
    juce::AudioBuffer<float> vocode (const juce::AudioBuffer<float>& source, double sampleRate, const Map& map, double ratio,
                                     int outLength, const VocoderOptions& options, const std::function<bool()>& shouldContinue)
    {
        const auto numChannels = juce::jmax (1, source.getNumChannels());
        const auto numSource = source.getNumSamples();

        const auto order = juce::jlimit (9, 13, (int) std::round (std::log2 (sampleRate * options.windowSeconds)));
        const int size = 1 << order, half = size / 2, hop = size / juce::jlimit (4, 16, options.overlap);
        const auto stretchedLength = (int) std::ceil (outLength * ratio) + 4;
        const auto overlapGain = 0.375f * (float) (size / hop); // Hann² summed over the frames (1.5 at 75% overlap)
        const auto shapeFormants = options.formant && std::abs (ratio - 1.0) > 1.0e-6;

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
        const auto shapeFrame = [&]
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
                const auto at = juce::jmin ((double) (bins - 1), k * ratio);
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
            const auto sourceCentre = map.toSource (centre / ratio / sampleRate) * sampleRate; // in the file

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

            if (shapeFormants)
                shapeFrame();

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

        return readBack (stretched, sampleRate, ratio, outLength);
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

juce::AudioBuffer<float> render (const juce::AudioBuffer<float>& source, double sampleRate, const Settings& settings,
                                 const std::function<bool()>& shouldContinue)
{
    const auto numSource = source.getNumSamples();

    if (settings.isNeutral() || numSource == 0 || source.getNumChannels() == 0)
        return juce::AudioBuffer<float> (source);

    const auto& map = settings.map;
    const auto ratio = std::pow (2.0, juce::jlimit (-Settings::maxSemitones, Settings::maxSemitones, settings.semitones) / 12.0);
    const auto outLength = (int) juce::jmax (0.0, std::round (map.toTarget (numSource / sampleRate) * sampleRate));

    VocoderOptions options;
    options.formant = settings.formant;

    // Slice and varispeed place the audio themselves; a transpose then goes through the
    // vocoder on the result.
    const auto thenTranspose = [&] (juce::AudioBuffer<float> placed)
    {
        if (std::abs (ratio - 1.0) < 1.0e-9 || placed.getNumSamples() == 0)
            return placed;
        return vocode (placed, sampleRate, {}, ratio, placed.getNumSamples(), options, shouldContinue);
    };

    switch (settings.algorithm)
    {
        case Algorithm::solo:
            options.overlap = 8;
            options.attackRatio = 3.0;
            break;
        case Algorithm::drums:
            options.windowSeconds = 0.0115;
            options.attackRatio = 2.0;
            break;
        case Algorithm::efficient:
            return overlapAdd (source, sampleRate, map, ratio, outLength, shouldContinue);
        case Algorithm::slice:
            return thenTranspose (slice (source, sampleRate, map, outLength));
        case Algorithm::varispeed:
            return thenTranspose (varispeed (source, sampleRate, map, outLength));
        case Algorithm::mix:
            break;
    }

    return vocode (source, sampleRate, map, ratio, outLength, options, shouldContinue);
}
}
