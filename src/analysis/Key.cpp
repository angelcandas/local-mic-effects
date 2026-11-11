#include "Key.h"

#include <juce_dsp/juce_dsp.h>

#include <cmath>
#include <numeric>

namespace analysis
{
namespace
{
    constexpr double lowestHz = 60.0, highestHz = 5000.0;
    constexpr float peakFloor = 0.01f; // peaks below -40 dB of the frame's strongest are ignored

    // How much each scale degree tends to sound in major and minor music, from the
    // Temperley (2007) Kostka-Payne corpus profiles, which suit chord-based (pop, rock,
    // folk) material better than the older probe-tone profiles.
    constexpr std::array<float, 12> majorProfile { 0.748f, 0.060f, 0.488f, 0.082f, 0.670f, 0.460f,
                                                   0.096f, 0.715f, 0.104f, 0.366f, 0.057f, 0.400f };
    constexpr std::array<float, 12> minorProfile { 0.712f, 0.084f, 0.474f, 0.618f, 0.049f, 0.460f,
                                                   0.105f, 0.747f, 0.404f, 0.067f, 0.133f, 0.330f };

    float correlate (const std::array<float, 12>& chroma, const std::array<float, 12>& profile, int tonic)
    {
        const auto meanOf = [] (const std::array<float, 12>& values)
        {
            return std::accumulate (values.begin(), values.end(), 0.0f) / 12.0f;
        };
        const auto chromaMean = meanOf (chroma), profileMean = meanOf (profile);

        double sum = 0.0, chromaSquares = 0.0, profileSquares = 0.0;
        for (int note = 0; note < 12; ++note)
        {
            const auto c = chroma[(size_t) note] - chromaMean;
            const auto p = profile[(size_t) ((note - tonic + 12) % 12)] - profileMean;
            sum += c * p;
            chromaSquares += c * c;
            profileSquares += p * p;
        }

        return chromaSquares > 0.0 && profileSquares > 0.0 ? (float) (sum / std::sqrt (chromaSquares * profileSquares)) : 0.0f;
    }
}

//==============================================================================
bool computeChroma (const float* magnitudes, int fftSize, double sampleRate, Chroma& chroma)
{
    const auto binWidth = sampleRate / fftSize;
    const auto first = juce::jmax (2, (int) std::ceil (lowestHz / binWidth));
    const auto last = juce::jmin (fftSize / 2 - 2, (int) std::floor (highestHz / binWidth));

    float strongest = 0.0f;
    for (int k = first; k <= last; ++k)
        strongest = juce::jmax (strongest, magnitudes[k]);

    if (! (strongest > 0.0f) || ! std::isfinite (strongest))
        return false;

    Chroma frame {};
    bool any = false;

    for (int k = first; k <= last; ++k)
    {
        const auto m = magnitudes[k];
        if (m < strongest * peakFloor || m <= magnitudes[k - 1] || m < magnitudes[k + 1])
            continue;

        // The peak's true frequency, from a parabola through the log magnitudes.
        const auto a = std::log (magnitudes[k - 1] + 1.0e-12f), b = std::log (m), c = std::log (magnitudes[k + 1] + 1.0e-12f);
        const auto denominator = a - 2.0f * b + c;
        const auto offset = denominator < 0.0f ? juce::jlimit (-0.5f, 0.5f, 0.5f * (a - c) / denominator) : 0.0f;
        const auto frequency = (k + offset) * binWidth;

        // Position in thirds of a semitone from C, shared between the two nearest bins.
        const auto position = 3.0 * (69.0 + 12.0 * std::log2 (frequency / 440.0));
        const auto wrapped = std::fmod (std::fmod (position, (double) numChromaBins) + numChromaBins, (double) numChromaBins);
        const auto lower = (int) std::floor (wrapped);
        const auto upperShare = (float) (wrapped - lower);

        frame[(size_t) (lower % numChromaBins)] += m * (1.0f - upperShare);
        frame[(size_t) ((lower + 1) % numChromaBins)] += m * upperShare;
        any = true;
    }

    if (! any)
        return false;

    const auto peak = *std::max_element (frame.begin(), frame.end());
    for (size_t i = 0; i < frame.size(); ++i)
        chroma[i] += frame[i] / peak;

    return true;
}

//==============================================================================
juce::String KeyEstimate::getName (int key, bool minor)
{
    static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    return key >= 0 && key < 12 ? juce::String (names[key]) + (minor ? " minor" : " major") : juce::String();
}

juce::String KeyEstimate::getName() const
{
    return getName (key, minor);
}

void addChroma (Chroma& into, const Chroma& from, float weight)
{
    for (size_t i = 0; i < into.size(); ++i)
        into[i] += from[i] * weight;
}

Chroma sumChroma (const std::vector<Chroma>& frames, const std::vector<bool>* use)
{
    Chroma sum {};
    for (size_t i = 0; i < frames.size(); ++i)
        if (use == nullptr || (i < use->size() && (*use)[i]))
            addChroma (sum, frames[i]);
    return sum;
}

KeyEstimate estimateKey (const std::vector<Chroma>& frames, const std::vector<bool>* use)
{
    return estimateKey (sumChroma (frames, use));
}

KeyEstimate estimateKey (const Chroma& sum)
{
    KeyEstimate result;

    const auto total = std::accumulate (sum.begin(), sum.end(), 0.0);
    if (! (total > 0.0) || ! std::isfinite (total))
        return result;

    // Tuning: where the energy sits within each semitone, as a circular mean of the
    // three sub-bins (0, +1/3, -1/3 of a semitone).
    double x = 0.0, y = 0.0;
    for (int bin = 0; bin < numChromaBins; ++bin)
    {
        const auto angle = juce::MathConstants<double>::twoPi * (bin % chromaBinsPerNote) / chromaBinsPerNote;
        x += sum[(size_t) bin] * std::cos (angle);
        y += sum[(size_t) bin] * std::sin (angle);
    }
    const auto tuning = std::atan2 (y, x) / juce::MathConstants<double>::twoPi; // semitones, -0.5 .. 0.5
    result.tuningCents = (float) (tuning * 100.0);

    // Fold to 12 notes around the tuned centres.
    std::array<float, 12> chroma {};
    for (int bin = 0; bin < numChromaBins; ++bin)
    {
        const auto semitones = (double) bin / chromaBinsPerNote - tuning;
        const auto note = ((int) std::lround (semitones) % 12 + 12) % 12;
        chroma[(size_t) note] += sum[(size_t) bin];
    }

    const auto peak = *std::max_element (chroma.begin(), chroma.end());
    for (auto& value : chroma)
        value /= peak;
    result.chroma = chroma;

    for (int tonic = 0; tonic < 12; ++tonic)
    {
        result.scores[(size_t) tonic] = correlate (chroma, majorProfile, tonic);
        result.scores[(size_t) tonic + 12] = correlate (chroma, minorProfile, tonic);
    }

    int best = 0, second = -1;
    for (int i = 1; i < 24; ++i)
        if (result.scores[(size_t) i] > result.scores[(size_t) best])
            best = i;
    for (int i = 0; i < 24; ++i)
        if (i != best && (second < 0 || result.scores[(size_t) i] > result.scores[(size_t) second]))
            second = i;

    result.correlation = result.scores[(size_t) best];

    // Flat or random harmony (noise, drums alone, a single sustained note) doesn't fit
    // any key well.
    if (result.correlation < 0.3f)
        return result;

    result.key = best % 12;
    result.minor = best >= 12;
    result.runnerUpKey = second % 12;
    result.runnerUpMinor = second >= 12;

    // A gap of 0.15 in correlation between the two best keys is a clear win.
    result.confidence = juce::jlimit (0.0f, 1.0f, (result.correlation - result.scores[(size_t) second]) / 0.15f);
    return result;
}

//==============================================================================
KeyEstimate detectKey (const juce::AudioBuffer<float>& audio, double sampleRate, const std::function<bool()>& shouldContinue)
{
    constexpr int fftOrder = 13, fftSize = 1 << fftOrder;
    constexpr double silenceDb = -55.0;

    const auto hop = juce::jmax (1, (int) std::lround (sampleRate * 0.1));
    const auto numSamples = audio.getNumSamples();
    const auto numChannels = juce::jmin (2, audio.getNumChannels());
    if (numSamples == 0 || numChannels == 0)
        return {};

    juce::dsp::FFT fft (fftOrder);
    std::vector<float> window ((size_t) fftSize), data ((size_t) fftSize * 2);
    for (int i = 0; i < fftSize; ++i)
        window[(size_t) i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) fftSize);

    const auto mono = [&] (int i)
    {
        auto value = audio.getSample (0, i);
        if (numChannels > 1)
            value = 0.5f * (value + audio.getSample (1, i));
        return value;
    };

    Chroma sum {};

    // Each hop analyses the fftSize samples that end where it ends (as Analyser does).
    for (int end = hop, count = 0; end <= numSamples; end += hop, ++count)
    {
        if (shouldContinue && count % 50 == 0 && ! shouldContinue())
            return {};

        double squares = 0.0;
        for (int i = end - hop; i < end; ++i)
            squares += juce::square ((double) mono (i));
        if (10.0 * std::log10 (squares / hop + 1.0e-20) < silenceDb)
            continue;

        for (int i = 0; i < fftSize; ++i)
        {
            const auto source = end - fftSize + i;
            data[(size_t) i] = source >= 0 ? mono (source) * window[(size_t) i] : 0.0f;
        }
        std::fill (data.begin() + fftSize, data.end(), 0.0f);
        fft.performFrequencyOnlyForwardTransform (data.data(), true);

        computeChroma (data.data(), fftSize, sampleRate, sum);
    }

    return estimateKey (sum);
}
}
