#include "Warp.h"

#include <juce_dsp/juce_dsp.h>

#include "dsp/Biquad.h"

#include <algorithm>
#include <cmath>
#include <complex>

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

juce::String Settings::getKey() const
{
    return map.toString() + "|" + juce::String (juce::jlimit (-maxSemitones, maxSemitones, semitones), 4);
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
}

juce::AudioBuffer<float> render (const juce::AudioBuffer<float>& source, double sampleRate, const Settings& settings,
                                 const std::function<bool()>& shouldContinue)
{
    const auto numChannels = juce::jmax (1, source.getNumChannels());
    const auto numSource = source.getNumSamples();

    if (settings.isNeutral() || numSource == 0 || source.getNumChannels() == 0)
        return juce::AudioBuffer<float> (source);

    const auto& map = settings.map;
    const auto ratio = std::pow (2.0, juce::jlimit (-Settings::maxSemitones, Settings::maxSemitones, settings.semitones) / 12.0);
    const auto outLength = (int) juce::jmax (0.0, std::round (map.toTarget (numSource / sampleRate) * sampleRate));

    // The phase vocoder writes a stretched copy (`ratio` times longer than the output),
    // which is then read back `ratio` times faster to move the pitch.
    const auto order = juce::jlimit (10, 13, (int) std::round (std::log2 (sampleRate * 0.046)));
    const int size = 1 << order, half = size / 2, hop = size / 4;
    const auto stretchedLength = (int) std::ceil (outLength * ratio) + 4;

    juce::dsp::FFT fft (order);
    std::vector<float> window ((size_t) size);
    for (int i = 0; i < size; ++i)
        window[(size_t) i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) size);

    juce::AudioBuffer<float> stretched (numChannels, stretchedLength + size);
    stretched.clear();

    const auto bins = half + 1;
    std::vector<std::vector<Complex>> now ((size_t) numChannels, std::vector<Complex> ((size_t) bins)),
                                      before ((size_t) numChannels, std::vector<Complex> ((size_t) bins));
    std::vector<Complex> mixNow ((size_t) bins), mixBefore ((size_t) bins);
    std::vector<float> fftData ((size_t) size * 2);
    std::vector<float> magnitude ((size_t) bins), phaseNow ((size_t) bins), theta ((size_t) bins), lastTheta ((size_t) bins);
    std::vector<int> peaks, nearestPeak ((size_t) bins);
    bool fresh = true;
    double lastEnergy = 0.0;

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

    const auto firstFrame = -3, lastFrame = stretchedLength / hop + 3;

    for (int frame = firstFrame; frame <= lastFrame; ++frame)
    {
        if (shouldContinue && ((frame - firstFrame) & 63) == 0 && ! shouldContinue())
            return {};

        const auto centre = (double) frame * hop;                                   // in the stretched copy
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

            mixNow[(size_t) k] = a;
            mixBefore[(size_t) k] = b;
            magnitude[(size_t) k] = std::abs (a);
            phaseNow[(size_t) k] = std::arg (a);
            energyNow += std::norm (a) * k;  // weighted towards the highs, where attacks show
            energyBefore += std::norm (b) * k;
        }

        // An attack: take the analysis phases as they are, so it stays sharp instead of
        // being smeared by the phase propagation.
        const auto attack = energyNow > 4.0 * energyBefore && energyNow > 1.0e-3 * lastEnergy && energyNow > 1.0e-6;
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
                const auto x = spectrum[(size_t) k] * Complex (std::cos (rotate), std::sin (rotate));
                fftData[(size_t) (2 * k)] = x.real();
                fftData[(size_t) (2 * k + 1)] = x.imag();
            }

            fft.performRealOnlyInverseTransform (fftData.data());

            auto* out = stretched.getWritePointer (ch);
            for (int i = 0; i < size; ++i)
                if (const auto n = writeStart + i; n >= 0 && n < stretched.getNumSamples())
                    out[n] += fftData[(size_t) i] * window[(size_t) i] / 1.5f; // Hann² at 75% overlap sums to 1.5
        }
    }

    // Read the stretched copy back at the original speed times the pitch ratio.
    juce::AudioBuffer<float> result (numChannels, outLength);

    for (int ch = 0; ch < numChannels; ++ch)
    {
        auto* y = stretched.getWritePointer (ch);

        if (ratio > 1.0) // reading faster folds the top down: filter it off first
        {
            fx::Biquad first, second;
            const auto cutoff = 0.45 * sampleRate / ratio;
            first.setup (fx::Biquad::Type::lowPass, sampleRate, cutoff, 0.5412);
            second.setup (fx::Biquad::Type::lowPass, sampleRate, cutoff, 1.3066);

            for (int i = 0; i < stretched.getNumSamples(); ++i)
                y[i] = second.process (0, first.process (0, y[i]));
        }

        auto* out = result.getWritePointer (ch);
        if (std::abs (ratio - 1.0) < 1.0e-9)
            for (int j = 0; j < outLength; ++j)
                out[j] = y[j];
        else
            for (int j = 0; j < outLength; ++j)
                out[j] = interpolate (y, stretched.getNumSamples(), j * ratio);
    }

    return result;
}
}
