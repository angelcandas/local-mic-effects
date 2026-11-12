#include "DemucsPipeline.h"

#include "dsp.hpp" // libs/demucs.cpp/src: stft, istft

#include <algorithm>
#include <cmath>

namespace stems
{
namespace
{
    constexpr int segmentSamples = CoreNetwork::segmentSamples;
    constexpr int bins = CoreNetwork::freqBins;
    constexpr int frames = CoreNetwork::frames;
    constexpr int fftHop = demucscpp::FFT_HOP_SIZE;

    // The padding Demucs puts round a segment so its spectrogram lines up (_spec).
    constexpr int le = (segmentSamples + fftHop - 1) / fftHop;
    constexpr int padStart = fftHop / 2 * 3;
    constexpr int padEnd = padStart + le * fftHop - segmentSamples;
    constexpr int paddedSamples = segmentSamples + padStart + padEnd;
    static_assert (le == frames, "the network expects 336 frames");

    constexpr float epsilon = 1.0e-5f;

    struct MeanStd { double mean, std; };

    // Mean and unbiased standard deviation, as torch's x.mean() and x.std().
    MeanStd measure (const float* data, size_t n)
    {
        double sum = 0.0;
        for (size_t i = 0; i < n; ++i)
            sum += data[i];

        const auto mean = sum / (double) n;
        double squares = 0.0;
        for (size_t i = 0; i < n; ++i)
            squares += (data[i] - mean) * (data[i] - mean);

        return { mean, std::sqrt (squares / (double) (n - 1)) };
    }

    // One worker's memory for segments.
    struct Workspace
    {
        Workspace() : stft (paddedSamples),
                      x ((size_t) CoreNetwork::freqChannels * bins * frames),
                      xt ((size_t) CoreNetwork::timeChannels * segmentSamples),
                      xOut ((size_t) numStems * CoreNetwork::freqChannels * bins * frames),
                      xtOut ((size_t) numStems * CoreNetwork::timeChannels * segmentSamples) {}

        demucscpp::stft_buffers stft;
        std::vector<float> x, xt, xOut, xtOut;
    };

    // Runs the network on one segment (2 x segmentSamples, already normalised overall) and
    // writes numStems stereo segments to out[s] (2 x segmentSamples each).
    juce::String processSegment (const CoreNetwork& core, Workspace& w, const juce::AudioBuffer<float>& segment,
                                 std::vector<juce::AudioBuffer<float>>& out)
    {
        // Reflect-pad (like torch's pad1d), then the spectrogram.
        auto& padded = w.stft.waveform;
        for (int ch = 0; ch < 2; ++ch)
        {
            const auto* in = segment.getReadPointer (ch);
            for (int i = 0; i < segmentSamples; ++i)
                padded (ch, padStart + i) = in[i];
            for (int i = 0; i < padStart; ++i)
                padded (ch, padStart - 1 - i) = in[juce::jmin (i + 1, segmentSamples - 1)];
            for (int i = 0; i < padEnd; ++i)
                padded (ch, padStart + segmentSamples + i) = in[juce::jmax (0, segmentSamples - 2 - i)];
        }

        demucscpp::stft (w.stft);

        // Complex as channels: drop the last bin and the two frames either side, normalise.
        for (int ch = 0; ch < 2; ++ch)
            for (int f = 0; f < bins; ++f)
                for (int t = 0; t < frames; ++t)
                {
                    const auto z = w.stft.spec (ch, f, t + 2);
                    w.x[((size_t) (2 * ch) * bins + f) * frames + t] = z.real();
                    w.x[((size_t) (2 * ch + 1) * bins + f) * frames + t] = z.imag();
                }

        const auto freq = measure (w.x.data(), w.x.size());
        for (auto& v : w.x)
            v = (float) ((v - freq.mean) / (freq.std + epsilon));

        for (int ch = 0; ch < 2; ++ch)
            std::copy_n (segment.getReadPointer (ch), segmentSamples, w.xt.data() + (size_t) ch * segmentSamples);

        const auto time = measure (w.xt.data(), w.xt.size());
        for (auto& v : w.xt)
            v = (float) ((v - time.mean) / (time.std + epsilon));

        if (const auto problem = core.run (w.x.data(), w.xt.data(), w.xOut.data(), w.xtOut.data()); problem.isNotEmpty())
            return problem;

        // Undo the normalising, turn each source's spectrogram back into a waveform, add the
        // time branch.
        for (int s = 0; s < numStems; ++s)
        {
            // The bin and the frames the network doesn't see (the last bin, two frames either
            // side) are zero.
            w.stft.spec.setZero();

            for (int ch = 0; ch < 2; ++ch)
                for (int f = 0; f < bins; ++f)
                {
                    const auto* re = w.xOut.data() + (((size_t) s * 4 + 2 * ch) * bins + f) * frames;
                    const auto* im = w.xOut.data() + (((size_t) s * 4 + 2 * ch + 1) * bins + f) * frames;
                    for (int t = 0; t < frames; ++t)
                        w.stft.spec (ch, f, t + 2) = std::complex<float> ((float) (re[t] * freq.std + freq.mean),
                                                                          (float) (im[t] * freq.std + freq.mean));
                }

            demucscpp::istft (w.stft);

            for (int ch = 0; ch < 2; ++ch)
            {
                auto* dest = out[(size_t) s].getWritePointer (ch);
                const auto* tail = w.xtOut.data() + ((size_t) s * 2 + ch) * segmentSamples;
                for (int i = 0; i < segmentSamples; ++i)
                    dest[i] = (float) (w.stft.waveform (ch, padStart + i) + tail[i] * time.std + time.mean);
            }
        }

        return {};
    }
}

Separator makeSeparator (std::shared_ptr<const CoreNetwork> core)
{
    return [core] (const juce::AudioBuffer<float>& input, const Progress& progress) -> std::vector<juce::AudioBuffer<float>>
    {
        const auto length = input.getNumSamples();

        // Demucs normalises the whole input by its mono mean and std, and undoes it after.
        std::vector<float> mono ((size_t) length);
        for (int i = 0; i < length; ++i)
            mono[(size_t) i] = 0.5f * (input.getSample (0, i) + input.getSample (1, i));

        const auto ref = length > 1 ? measure (mono.data(), mono.size()) : MeanStd { 0.0, 1.0 };
        const auto scale = juce::jmax (ref.std, 1.0e-8);

        juce::AudioBuffer<float> normalised (2, length);
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < length; ++i)
                normalised.setSample (ch, i, (float) ((input.getSample (ch, i) - ref.mean) / scale));

        std::vector<juce::AudioBuffer<float>> result;
        for (int s = 0; s < numStems; ++s)
        {
            result.emplace_back (2, length);
            result.back().clear();
        }

        // Segments overlap by a quarter, weighted by a triangle so they crossfade.
        const auto stride = (int) ((1.0 - 0.25) * segmentSamples);
        std::vector<float> weight (segmentSamples);
        for (int i = 0; i < segmentSamples / 2; ++i)
            weight[(size_t) i] = (float) (i + 1);
        for (int i = 0; i < segmentSamples - segmentSamples / 2; ++i)
            weight[(size_t) (segmentSamples - 1 - i)] = (float) (i + 1);
        const auto top = *std::max_element (weight.begin(), weight.end());
        for (auto& w : weight)
            w /= top;

        std::vector<float> weightSum ((size_t) length, 0.0f);
        const auto numSegments = juce::jmax (1, (length + stride - 1) / stride);

        Workspace workspace;
        std::vector<juce::AudioBuffer<float>> segmentOut;
        for (int s = 0; s < numStems; ++s)
            segmentOut.emplace_back (2, segmentSamples);

        for (int k = 0; k < numSegments; ++k)
        {
            if (progress != nullptr && ! progress ((double) k / numSegments))
                return {};

            const auto offset = k * stride;
            const auto chunk = juce::jmin (segmentSamples, length - offset);
            const auto before = (segmentSamples - chunk) / 2; // zeros round a short last chunk

            juce::AudioBuffer<float> segment (2, segmentSamples);
            segment.clear();
            for (int ch = 0; ch < 2; ++ch)
                segment.copyFrom (ch, before, normalised, ch, offset, chunk);

            if (const auto problem = processSegment (*core, workspace, segment, segmentOut); problem.isNotEmpty())
            {
                DBG ("Stem splitter network: " << problem);
                return {};
            }

            for (int i = 0; i < chunk; ++i)
            {
                const auto w = weight[(size_t) i];
                weightSum[(size_t) (offset + i)] += w;

                for (int s = 0; s < numStems; ++s)
                    for (int ch = 0; ch < 2; ++ch)
                        result[(size_t) s].getWritePointer (ch)[offset + i] += w * segmentOut[(size_t) s].getSample (ch, before + i);
            }
        }

        for (int i = 0; i < length; ++i)
            if (weightSum[(size_t) i] > 0.0f)
                for (auto& stem : result)
                    for (int ch = 0; ch < 2; ++ch)
                        stem.getWritePointer (ch)[i] = (float) (stem.getSample (ch, i) / weightSum[(size_t) i] * scale + ref.mean);

        if (progress != nullptr)
            progress (1.0);

        return result;
    };
}
}
