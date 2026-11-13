#include "Alignment.h"

#include <juce_dsp/juce_dsp.h>

namespace analysis
{
namespace
{
    std::vector<float> toMono (const juce::AudioBuffer<float>& audio, int length)
    {
        std::vector<float> mono ((size_t) length, 0.0f);
        const auto channels = audio.getNumChannels();
        for (int ch = 0; ch < channels; ++ch)
        {
            const auto* data = audio.getReadPointer (ch);
            for (int i = 0; i < length; ++i)
                mono[(size_t) i] += data[i] / (float) channels;
        }
        return mono;
    }

    double energy (const std::vector<float>& x, int from, int to)
    {
        double sum = 0.0;
        for (int i = from; i < to; ++i)
            sum += (double) x[(size_t) i] * x[(size_t) i];
        return sum;
    }
}

Alignment measureAlignment (const juce::AudioBuffer<float>& bufferA, const juce::AudioBuffer<float>& bufferB, double sampleRate,
                            double maxSeconds, double maxAnalysedSeconds)
{
    Alignment result;
    const auto length = juce::jmin (bufferA.getNumSamples(), bufferB.getNumSamples(), (int) (maxAnalysedSeconds * sampleRate));
    if (length <= 0 || sampleRate <= 0.0 || bufferA.getNumChannels() == 0 || bufferB.getNumChannels() == 0)
        return result;

    const auto a = toMono (bufferA, length);
    const auto b = toMono (bufferB, length);

    // Frames of `frame` samples, zero-padded to twice that, so the correlation is linear
    // (no wrap) for every lag up to the frame's length.
    const auto maxLag = juce::jlimit (1, 1 << 14, (int) std::ceil (maxSeconds * sampleRate));
    int order = 12;
    while ((1 << order) < 4 * maxLag && order < 16)
        ++order;
    const auto frame = 1 << order, size = frame * 2;
    juce::dsp::FFT fft (order + 1);

    // Only frames where both play (over -60 dBFS), so silence and noise don't vote.
    const auto threshold = (double) frame * std::pow (10.0, -60.0 / 10.0);

    std::vector<std::complex<double>> cross ((size_t) size / 2 + 1);
    std::vector<float> fa ((size_t) size * 2), fb ((size_t) size * 2);
    int frames = 0;
    for (int start = 0; start + frame <= length || (start == 0 && length > 0); start += frame / 2)
    {
        const auto n = juce::jmin (frame, length - start);
        if (energy (a, start, start + n) < threshold * n / frame || energy (b, start, start + n) < threshold * n / frame)
        {
            if (start + frame > length)
                break;
            continue;
        }

        std::fill (fa.begin(), fa.end(), 0.0f);
        std::fill (fb.begin(), fb.end(), 0.0f);
        for (int i = 0; i < n; ++i)
        {
            const auto w = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) n); // Hann
            fa[(size_t) i] = a[(size_t) (start + i)] * w;
            fb[(size_t) i] = b[(size_t) (start + i)] * w;
        }
        fft.performRealOnlyForwardTransform (fa.data(), true);
        fft.performRealOnlyForwardTransform (fb.data(), true);

        for (size_t k = 0; k < cross.size(); ++k)
        {
            const std::complex<double> A (fa[2 * k], fa[2 * k + 1]), B (fb[2 * k], fb[2 * k + 1]);
            cross[k] += A * std::conj (B);
        }
        ++frames;
        if (start + frame > length)
            break;
    }

    if (frames == 0)
        return result;

    // Whitened (PHAT) over 60 Hz - 12 kHz, where mics share most of their sound.
    const auto binHz = sampleRate / size;
    const auto lowBin = juce::jmax (1, (int) std::ceil (60.0 / binHz));
    const auto highBin = juce::jmin ((int) cross.size() - 1, (int) std::floor (12000.0 / binHz));
    std::vector<float> spectrum ((size_t) size * 2, 0.0f);
    int used = 0;
    for (int k = lowBin; k <= highBin; ++k)
    {
        const auto magnitude = std::abs (cross[(size_t) k]);
        if (magnitude <= 1.0e-20)
            continue;
        const auto unit = cross[(size_t) k] / magnitude;
        spectrum[(size_t) (2 * k)] = (float) unit.real();
        spectrum[(size_t) (2 * k + 1)] = (float) unit.imag();
        ++used;
    }
    if (used == 0)
        return result;

    fft.performRealOnlyInverseTransform (spectrum.data());
    // r[lag] = sum a[n + lag] b[n]: b arriving d samples late peaks at lag -d. JUCE's
    // inverse scales by 1/size; a perfect match over `used` bins peaks at 2 * used / size.
    const auto scale = (float) size / (2.0f * (float) used);
    int bestLag = 0;
    float best = 0.0f;
    for (int lag = -maxLag; lag <= maxLag; ++lag)
    {
        const auto value = spectrum[(size_t) (lag >= 0 ? lag : size + lag)] * scale;
        if (std::abs (value) > std::abs (best))
        {
            best = value;
            bestLag = lag;
        }
    }

    const auto delay = -bestLag; // samples b is late
    result.delaySeconds = delay / sampleRate;
    result.inverted = best < 0.0f;
    result.confidence = juce::jlimit (0.0f, 1.0f, std::abs (best));
    result.related = result.confidence >= minAlignmentConfidence;

    // How much fuller the two get together once b is lined up.
    double before = 0.0, after = 0.0;
    const auto sign = result.inverted ? -1.0 : 1.0;
    for (int i = 0; i < length; ++i)
    {
        const auto x = (double) a[(size_t) i];
        before += (x + b[(size_t) i]) * (x + b[(size_t) i]);
        const auto j = i + delay;
        const auto y = j >= 0 && j < length ? sign * b[(size_t) j] : 0.0;
        after += (x + y) * (x + y);
    }
    result.sumGainDb = before > 0.0 && after > 0.0 ? (float) (10.0 * std::log10 (after / before)) : 0.0f;
    return result;
}
}
