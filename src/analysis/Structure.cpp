#include "Structure.h"

#include "Beats.h"
#include "Chords.h"
#include "Key.h"

#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>

namespace analysis::structure
{
namespace
{
    constexpr float silenceDb = -60.0f;

    float cosine (const float* a, const float* b, size_t n) noexcept
    {
        double ab = 0.0, aa = 0.0, bb = 0.0;
        for (size_t i = 0; i < n; ++i)
        {
            ab += (double) a[i] * b[i];
            aa += (double) a[i] * a[i];
            bb += (double) b[i] * b[i];
        }
        return aa > 0.0 && bb > 0.0 ? (float) (ab / std::sqrt (aa * bb)) : (aa <= 0.0 && bb <= 0.0 ? 1.0f : 0.0f);
    }

    float rmsDifference (const std::array<float, 12>& a, const std::array<float, 12>& b) noexcept
    {
        double sum = 0.0;
        for (size_t i = 0; i < 12; ++i)
            sum += (double) (a[i] - b[i]) * (a[i] - b[i]);
        return (float) std::sqrt (sum / 12.0);
    }

    float percentile (std::vector<float> values, double fraction)
    {
        if (values.empty())
            return 0.0f;
        const auto index = (size_t) juce::jlimit (0, (int) values.size() - 1, (int) std::lround (fraction * (double) (values.size() - 1)));
        std::nth_element (values.begin(), values.begin() + (long) index, values.end());
        return values[index];
    }

    // Mean features over bars [from, to).
    struct Summary
    {
        std::array<float, 12> chroma {}, timbre {};
        float levelDb = 0.0f;
    };

    Summary summarise (const BarFeatures& bars, int from, int to)
    {
        Summary s;
        const auto count = juce::jmax (1, to - from);
        double power = 0.0;
        for (int b = from; b < to; ++b)
        {
            for (size_t i = 0; i < 12; ++i)
            {
                s.chroma[i] += bars.chroma[(size_t) b][i];
                s.timbre[i] += bars.timbre[(size_t) b][i] / (float) count;
            }
            power += std::pow (10.0, bars.levelDb[(size_t) b] / 10.0);
        }
        s.levelDb = (float) (10.0 * std::log10 (power / count + 1.0e-20));
        return s;
    }

    float summaryDistance (const Summary& a, const Summary& b) noexcept
    {
        return 2.0f * (1.0f - cosine (a.chroma.data(), b.chroma.data(), 12))
             + rmsDifference (a.timbre, b.timbre) / 6.0f
             + std::abs (a.levelDb - b.levelDb) / 6.0f;
    }

    // The mean bar distance between two stretches laid side by side, bar by bar (the
    // shorter one's length).
    float alignedDistance (const BarFeatures& bars, int a, int b, int length)
    {
        if (length <= 0)
            return 0.0f;
        float sum = 0.0f;
        for (int k = 0; k < length; ++k)
            sum += getBarDistance (bars, a + k, b + k);
        return sum / (float) length;
    }

    // How well a section length fits common song forms (lower is better).
    float lengthPenalty (int bars) noexcept
    {
        switch (bars)
        {
            case 8:  return 0.0f;
            case 4: case 16: return 0.05f;
            case 12: return 0.15f;
            case 2: case 24: case 32: return 0.25f;
            case 6: case 10: return 0.3f;
            default: return 0.45f;
        }
    }
}

//==============================================================================
int SongMap::getSectionAt (double seconds) const noexcept
{
    for (size_t i = 0; i < sections.size(); ++i)
        if (seconds >= sections[i].start && seconds < sections[i].end)
            return (int) i;
    return -1;
}

std::vector<double> SongMap::getBarTempos() const
{
    std::vector<double> tempos;
    for (size_t i = 0; i + 1 < downbeats.size(); ++i)
        if (const auto length = downbeats[i + 1] - downbeats[i]; length > 0.0)
            tempos.push_back (60.0 * beatsPerBar / length);
    return tempos;
}

double SongMap::getTempoDrift() const
{
    const auto tempos = getBarTempos();
    if (tempos.size() < 2)
        return 0.0;
    const auto mean = std::accumulate (tempos.begin(), tempos.end(), 0.0) / (double) tempos.size();
    double variance = 0.0;
    for (auto t : tempos)
        variance += (t - mean) * (t - mean);
    return mean > 0.0 ? std::sqrt (variance / (double) tempos.size()) / mean : 0.0;
}

//==============================================================================
BeatFeatures measureBeats (const juce::AudioBuffer<float>& audio, double sampleRate, const std::vector<double>& beats,
                           const std::function<bool()>& shouldContinue)
{
    BeatFeatures features;
    const auto numBeats = beats.size();
    if (numBeats == 0 || audio.getNumSamples() == 0)
        return features;

    const auto numSamples = audio.getNumSamples();
    const auto channels = audio.getNumChannels();
    std::vector<float> mono ((size_t) numSamples, 0.0f);
    for (int ch = 0; ch < channels; ++ch)
    {
        const auto* data = audio.getReadPointer (ch);
        for (int i = 0; i < numSamples; ++i)
            mono[(size_t) i] += data[i] / (float) channels;
    }

    // Frames of 8192 samples (enough for the chroma of bass notes) every ~21 ms.
    constexpr int fftOrder = 13, fftSize = 1 << fftOrder, half = fftSize / 2;
    const auto hop = juce::jmax (1, (int) std::lround (sampleRate * 0.0213));
    juce::dsp::FFT fft (fftOrder);
    std::vector<float> window ((size_t) fftSize), data ((size_t) fftSize * 2), previousLow;
    for (int i = 0; i < fftSize; ++i)
        window[(size_t) i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) fftSize);

    // The timbre bands: 1/3 octaves from about 40 Hz to 10 kHz, in pairs (2/3 octave each).
    const auto binWidth = sampleRate / fftSize;
    std::array<int, 13> bandEdges {};
    for (int b = 0; b <= 12; ++b)
        bandEdges[(size_t) b] = juce::jlimit (1, half, (int) std::lround (40.0 * std::pow (2.0, b * 2.0 / 3.0) / binWidth));
    const auto lowTop = juce::jmax (2, (int) std::lround (150.0 / binWidth));
    previousLow.assign ((size_t) lowTop, 0.0f);

    const auto numFrames = numSamples / hop + 1;
    std::vector<Chroma> frameChroma ((size_t) numFrames);
    std::vector<std::array<double, 12>> framePower ((size_t) numFrames);
    std::vector<double> frameSquares ((size_t) numFrames, 0.0);
    std::vector<float> frameLowFlux ((size_t) numFrames, 0.0f);
    Chroma total {};

    for (int f = 0; f < numFrames; ++f)
    {
        if ((f & 255) == 0 && shouldContinue && ! shouldContinue())
            return {};

        const auto centre = f * hop;
        double squares = 0.0;
        for (int i = centre - hop / 2; i < centre + hop / 2; ++i)
            if (i >= 0 && i < numSamples)
                squares += (double) mono[(size_t) i] * mono[(size_t) i];
        frameSquares[(size_t) f] = squares / hop;

        for (int i = 0; i < fftSize; ++i)
        {
            const auto n = centre - half + i;
            data[(size_t) i] = n >= 0 && n < numSamples ? mono[(size_t) n] * window[(size_t) i] : 0.0f;
        }
        std::fill (data.begin() + fftSize, data.end(), 0.0f);
        fft.performFrequencyOnlyForwardTransform (data.data(), true);

        if (10.0 * std::log10 (frameSquares[(size_t) f] + 1.0e-20) > silenceDb)
            if (computeChroma (data.data(), fftSize, sampleRate, frameChroma[(size_t) f]))
                addChroma (total, frameChroma[(size_t) f]);

        for (size_t b = 0; b < 12; ++b)
        {
            double sum = 0.0;
            for (int k = bandEdges[b]; k < bandEdges[b + 1]; ++k)
                sum += (double) data[(size_t) k] * data[(size_t) k];
            framePower[(size_t) f][b] = sum;
        }

        float flux = 0.0f;
        for (int k = 1; k < lowTop; ++k)
        {
            const auto level = std::log1p (100.0f * data[(size_t) k] / (float) (fftSize / 4));
            flux += juce::jmax (0.0f, level - previousLow[(size_t) k]);
            previousLow[(size_t) k] = level;
        }
        frameLowFlux[(size_t) f] = flux;
    }

    const auto tuning = estimateKey (total).tuningCents / 100.0f;
    const auto frameOf = [&] (double seconds) { return juce::jlimit (0, numFrames, (int) std::lround (seconds * sampleRate / hop)); };

    features.chroma.resize (numBeats);
    features.timbre.resize (numBeats);
    features.levelDb.resize (numBeats);
    features.lowOnset.resize (numBeats);
    features.chordChange.assign (numBeats, 0.0f);

    for (size_t i = 0; i < numBeats; ++i)
    {
        const auto period = i + 1 < numBeats ? beats[i + 1] - beats[i] : (i > 0 ? beats[i] - beats[i - 1] : 0.5);
        const auto first = frameOf (beats[i]), last = juce::jmax (first + 1, frameOf (beats[i] + period));

        Chroma chroma {};
        std::array<double, 12> power {};
        double squares = 0.0;
        int count = 0;
        for (int f = first; f < last && f < numFrames; ++f, ++count)
        {
            addChroma (chroma, frameChroma[(size_t) f]);
            for (size_t b = 0; b < 12; ++b)
                power[b] += framePower[(size_t) f][b];
            squares += frameSquares[(size_t) f];
        }
        count = juce::jmax (1, count);

        const auto anyTone = std::any_of (chroma.begin(), chroma.end(), [] (float v) { return v > 0.0f; });
        features.chroma[i] = anyTone ? foldChroma (chroma, tuning) : std::array<float, 12> {};

        const auto levelDb = (float) (10.0 * std::log10 (squares / count + 1.0e-20));
        features.levelDb[i] = juce::jmax (-200.0f, levelDb);

        // Tone colour as the shape of the spectrum (the level is measured apart).
        std::array<float, 12> timbre {};
        float mean = 0.0f;
        for (size_t b = 0; b < 12; ++b)
        {
            timbre[b] = (float) (10.0 * std::log10 (power[b] / count + 1.0e-12));
            mean += timbre[b] / 12.0f;
        }
        for (auto& t : timbre)
            t = juce::jmax (-40.0f, t - mean);
        features.timbre[i] = timbre;

        // Low end arriving right at the beat (within 40 ms).
        float strongest = 0.0f;
        const auto around = juce::jmax (1, (int) std::lround (0.04 * sampleRate / hop));
        const auto at = frameOf (beats[i]);
        for (int f = juce::jmax (0, at - around); f <= juce::jmin (numFrames - 1, at + around); ++f)
            strongest = juce::jmax (strongest, frameLowFlux[(size_t) f]);
        features.lowOnset[i] = strongest;
    }

    // How much the harmony changes at each beat: the chord (Chords.h) and the notes.
    std::vector<Chord> chords (numBeats);
    for (size_t i = 0; i < numBeats; ++i)
        chords[i] = matchChord (features.chroma[i], -1, 0.55f);

    for (size_t i = 1; i < numBeats; ++i)
    {
        const auto& a = features.chroma[i - 1];
        const auto& b = features.chroma[i];
        if (std::all_of (b.begin(), b.end(), [] (float v) { return v <= 0.0f; }))
            continue;
        const auto noteChange = 1.0f - cosine (a.data(), b.data(), 12);
        const auto chordChanged = chords[i].isValid() && chords[i] != chords[i - 1] ? 1.0f : 0.0f;
        features.chordChange[i] = juce::jlimit (0.0f, 1.0f, 0.5f * noteChange * 2.0f + 0.5f * chordChanged);
    }

    return features;
}

int findDownbeatPhase (const BeatFeatures& features, int beatsPerBar, int hint)
{
    const auto n = (int) features.chordChange.size();
    const auto bar = juce::jlimit (1, 12, beatsPerBar);
    if (n < bar * 2)
        return juce::jlimit (0, bar - 1, hint);

    const auto lowMean = juce::jmax (1.0e-6f, std::accumulate (features.lowOnset.begin(), features.lowOnset.end(), 0.0f) / (float) n);
    const auto changeMean = juce::jmax (1.0e-6f, std::accumulate (features.chordChange.begin(), features.chordChange.end(), 0.0f) / (float) n);

    int best = juce::jlimit (0, bar - 1, hint);
    double bestScore = -1.0e9;
    for (int phase = 0; phase < bar; ++phase)
    {
        double sum = 0.0;
        int count = 0;
        for (int i = phase; i < n; i += bar, ++count)
            sum += features.chordChange[(size_t) i] / changeMean + 0.6 * features.lowOnset[(size_t) i] / lowMean;
        const auto score = count > 0 ? sum / count + (phase == hint ? 0.05 : 0.0) : -1.0e9;
        if (score > bestScore)
        {
            bestScore = score;
            best = phase;
        }
    }
    return best;
}

//==============================================================================
BarFeatures getBarFeatures (const BeatFeatures& beats, int firstDownbeat, int beatsPerBar, int numBars)
{
    BarFeatures bars;
    bars.beatsPerBar = beatsPerBar;
    const auto numBeats = (int) beats.levelDb.size();

    for (int b = 0; b < numBars; ++b)
    {
        std::vector<float> sequence ((size_t) (12 * beatsPerBar), 0.0f);
        std::array<float, 12> chroma {}, timbre {};
        double power = 0.0;
        int count = 0;

        for (int k = 0; k < beatsPerBar; ++k)
        {
            const auto i = firstDownbeat + b * beatsPerBar + k;
            if (i < 0 || i >= numBeats)
                continue;
            for (size_t c = 0; c < 12; ++c)
            {
                sequence[(size_t) k * 12 + c] = beats.chroma[(size_t) i][c];
                chroma[c] += beats.chroma[(size_t) i][c];
                timbre[c] += beats.timbre[(size_t) i][c];
            }
            power += std::pow (10.0, beats.levelDb[(size_t) i] / 10.0);
            ++count;
        }

        if (count > 0)
            for (auto& t : timbre)
                t /= (float) count;
        if (const auto peak = *std::max_element (chroma.begin(), chroma.end()); peak > 0.0f)
            for (auto& c : chroma)
                c /= peak;

        bars.chromaSequence.push_back (std::move (sequence));
        bars.chroma.push_back (chroma);
        bars.timbre.push_back (timbre);
        bars.levelDb.push_back (count > 0 ? (float) juce::jmax (-200.0, 10.0 * std::log10 (power / count + 1.0e-20)) : -200.0f);
    }

    return bars;
}

float getBarDistance (const BarFeatures& bars, int a, int b)
{
    const auto& sa = bars.chromaSequence[(size_t) a];
    const auto& sb = bars.chromaSequence[(size_t) b];
    const auto harmony = 1.0f - cosine (sa.data(), sb.data(), juce::jmin (sa.size(), sb.size()));
    const auto la = juce::jmax (silenceDb, bars.levelDb[(size_t) a]), lb = juce::jmax (silenceDb, bars.levelDb[(size_t) b]);
    return 2.0f * harmony + rmsDifference (bars.timbre[(size_t) a], bars.timbre[(size_t) b]) / 6.0f + std::abs (la - lb) / 6.0f;
}

//==============================================================================
std::vector<Section> findSections (const BarFeatures& bars, const Options& options)
{
    std::vector<Section> sections;
    const auto n = bars.size();
    if (n == 0)
        return sections;

    const auto minLength = juce::jmax (1, options.minSectionBars);
    const auto maxLength = juce::jmax (minLength, options.maxSectionBars);

    if (n <= minLength * 2)
    {
        Section whole;
        whole.firstBar = 0;
        whole.bars = n;
        sections.push_back (whole);
    }
    else
    {
        // Novelty at each bar line: how different the music before is from the music after
        // (means over 2, 4 and 8 bars), plus how much the bars stop repeating what came 4 or
        // 8 bars before (a part that cycles through its chords every 4 or 8 bars repeats).
        std::vector<float> novelty ((size_t) n + 1, 0.0f);
        for (int b = 1; b < n; ++b)
        {
            float sum = 0.0f;
            int count = 0;
            for (int length : { 2, 4, 8 })
            {
                const auto l = juce::jmin (length, b, n - b);
                if (l < 1 || (l < length && count > 0))
                    continue;
                sum += summaryDistance (summarise (bars, b - l, b), summarise (bars, b, b + l));
                ++count;
            }
            const auto mean = count > 0 ? sum / (float) count : 0.0f;

            auto repetition = -1.0f;
            for (int length : { 4, 8 })
                if (b - length >= 0 && b + length <= n)
                {
                    // Before the line, the music repeats itself `length` bars apart; after it,
                    // it doesn't carry that on.
                    const auto after = alignedDistance (bars, b - length, b, length);
                    repetition = repetition < 0.0f ? after : juce::jmin (repetition, after);
                }
            novelty[(size_t) b] = mean + juce::jmax (0.0f, repetition) * 0.5f;
        }

        const auto peak = *std::max_element (novelty.begin(), novelty.end());
        if (peak > 0.0f)
            for (auto& v : novelty)
                v /= peak;

        // The best set of boundaries: each section scores the novelty at its start, minus a
        // cost per section and a penalty for unusual lengths (dynamic programming over bars).
        constexpr float boundaryCost = 0.35f;
        std::vector<float> best ((size_t) n + 1, -1.0e9f);
        std::vector<int> from ((size_t) n + 1, -1);
        best[0] = 0.0f;

        for (int b = 1; b <= n; ++b)
            for (int a = juce::jmax (0, b - maxLength); a <= b - 1; ++a)
            {
                const auto length = b - a;
                const auto isLast = b == n, isFirst = a == 0;
                if (length < minLength && ! isLast && ! isFirst)
                    continue;
                if (best[(size_t) a] <= -1.0e8f)
                    continue;

                // The first and last sections often aren't whole phrases (pickups, a fade).
                const auto penalty = isLast || isFirst ? juce::jmin (lengthPenalty (length), 0.25f) : lengthPenalty (length);
                const auto score = best[(size_t) a] + (a > 0 ? novelty[(size_t) a] - boundaryCost : 0.0f) - penalty;
                if (score > best[(size_t) b])
                {
                    best[(size_t) b] = score;
                    from[(size_t) b] = a;
                }
            }

        std::vector<int> starts;
        for (int b = n; b > 0; b = from[(size_t) b])
        {
            if (from[(size_t) b] < 0)
            {
                starts.clear();
                starts.push_back (0);
                break;
            }
            starts.push_back (from[(size_t) b]);
        }
        std::reverse (starts.begin(), starts.end());

        for (size_t i = 0; i < starts.size(); ++i)
        {
            Section s;
            s.firstBar = starts[i];
            s.bars = (i + 1 < starts.size() ? starts[i + 1] : n) - starts[i];
            sections.push_back (s);
        }

        // A section made of the same 8-bar (or longer) part played twice, which shows no
        // novelty in between, becomes two. Shorter repeats stay whole: an 8-bar verse often
        // goes round a 4-bar chord cycle twice.
        for (size_t i = 0; i < sections.size(); ++i)
        {
            const auto length = sections[i].bars;
            if (length < 16 || length % 2 != 0)
                continue;
            const auto halfLength = length / 2;
            if (alignedDistance (bars, sections[i].firstBar, sections[i].firstBar + halfLength, halfLength) < 0.6f)
            {
                auto second = sections[i];
                second.firstBar += halfLength;
                second.bars = halfLength;
                sections[i].bars = halfLength;
                sections.insert (sections.begin() + (long) i + 1, second);
                --i; // look at the first half again
            }
        }
    }

    // Levels.
    for (auto& s : sections)
        s.loudnessDb = summarise (bars, s.firstBar, s.firstBar + s.bars).levelDb;

    // Letters: sections that sound alike, bar by bar, join (average linkage).
    const auto count = sections.size();
    std::vector<std::vector<float>> distance (count, std::vector<float> (count, 0.0f));
    for (size_t i = 0; i < count; ++i)
        for (size_t j = i + 1; j < count; ++j)
        {
            const auto length = juce::jmin (sections[i].bars, sections[j].bars);
            auto d = alignedDistance (bars, sections[i].firstBar, sections[j].firstBar, length);
            // Very different lengths are less likely the same part.
            const auto ratio = (float) juce::jmax (sections[i].bars, sections[j].bars) / (float) juce::jmax (1, length);
            if (ratio > 2.01f)
                d += 0.3f;
            distance[i][j] = distance[j][i] = d;
        }

    constexpr float joinBelow = 0.75f;
    std::vector<std::vector<size_t>> groups;
    for (size_t i = 0; i < count; ++i)
        groups.push_back ({ i });

    for (;;)
    {
        auto bestDistance = joinBelow;
        int bestA = -1, bestB = -1;
        for (size_t a = 0; a < groups.size(); ++a)
            for (size_t b = a + 1; b < groups.size(); ++b)
            {
                float sum = 0.0f;
                for (auto i : groups[a])
                    for (auto j : groups[b])
                        sum += distance[i][j];
                const auto mean = sum / (float) (groups[a].size() * groups[b].size());
                if (mean < bestDistance)
                {
                    bestDistance = mean;
                    bestA = (int) a;
                    bestB = (int) b;
                }
            }
        if (bestA < 0)
            break;
        auto& into = groups[(size_t) bestA];
        into.insert (into.end(), groups[(size_t) bestB].begin(), groups[(size_t) bestB].end());
        groups.erase (groups.begin() + bestB);
    }

    std::vector<int> groupOf (count, 0);
    for (size_t g = 0; g < groups.size(); ++g)
        for (auto i : groups[g])
            groupOf[i] = (int) g;

    // Letters in order of first appearance.
    std::map<int, int> letterOf;
    for (size_t i = 0; i < count; ++i)
        if (letterOf.find (groupOf[i]) == letterOf.end())
            letterOf[groupOf[i]] = (int) letterOf.size();
    for (size_t i = 0; i < count; ++i)
        sections[i].letter = letterOf[groupOf[i]];

    // Names.
    std::map<int, std::vector<size_t>> byLetter;
    for (size_t i = 0; i < count; ++i)
        byLetter[sections[i].letter].push_back (i);

    const auto isEdge = [count] (size_t i) { return i == 0 || i + 1 == count; };
    std::vector<int> repeated; // letters heard more than once, not only at the edges
    for (const auto& [letter, members] : byLetter)
        if (members.size() >= 2 && ! std::all_of (members.begin(), members.end(), isEdge))
            repeated.push_back (letter);

    const auto meanLevel = [&] (int letter)
    {
        double power = 0.0, length = 0.0;
        for (auto i : byLetter[letter])
        {
            power += std::pow (10.0, sections[i].loudnessDb / 10.0) * sections[i].bars;
            length += sections[i].bars;
        }
        return 10.0 * std::log10 (power / juce::jmax (1.0, length) + 1.0e-20);
    };

    auto chorus = -1;
    if (repeated.size() >= 2)
    {
        chorus = repeated.front();
        for (auto letter : repeated)
            if (meanLevel (letter) > meanLevel (chorus) + 0.25)
                chorus = letter;
    }

    for (size_t i = 0; i < count; ++i)
    {
        auto& s = sections[i];
        const auto& members = byLetter[s.letter];
        const auto once = members.size() == 1 || std::all_of (members.begin(), members.end(), isEdge);

        if (repeated.empty())
            s.kind = "part";
        else if (once && i == 0 && count > 1)
            s.kind = "intro";
        else if (once && i + 1 == count && count > 1)
            s.kind = "outro";
        else if (s.letter == chorus)
            s.kind = "chorus";
        else if (! once)
            s.kind = "verse";
        else
            s.kind = "bridge";
    }

    // "Verse 1", "Verse 2"... when a kind comes back; "Part A" when nothing repeats.
    std::map<juce::String, int> totals, seen;
    for (const auto& s : sections)
        ++totals[s.kind];
    for (auto& s : sections)
    {
        if (s.kind == "part")
        {
            s.name = "Part " + s.getLetterName();
            continue;
        }
        auto name = s.kind.substring (0, 1).toUpperCase() + s.kind.substring (1);
        if (totals[s.kind] > 1)
            name << " " << ++seen[s.kind];
        s.name = name;
    }

    return sections;
}

//==============================================================================
SongMap analyse (const juce::AudioBuffer<float>& audio, double sampleRate, const Options& options,
                 const std::function<bool()>& shouldContinue)
{
    SongMap map;
    map.beatsPerBar = juce::jlimit (2, 12, options.beatsPerBar);
    const auto seconds = audio.getNumSamples() / sampleRate;
    const auto keepGoing = [&] { return ! shouldContinue || shouldContinue(); };

    const auto tempo = rhythm::detectTempo (audio, sampleRate, map.beatsPerBar);
    if (! keepGoing())
    {
        map.cancelled = true;
        return map;
    }

    map.bpm = tempo.beats.size() >= 4 ? tempo.bpm : 0.0;
    map.tempoConfidence = tempo.confidence;
    map.beats = tempo.beats;

    // No steady beat: lay a straight 120 BPM grid over the audio, so the sections can still
    // be found (bars are then 2 seconds, not the music's).
    auto beats = map.beats;
    if (! map.hasBeats())
    {
        beats.clear();
        for (double t = 0.0; t < seconds; t += 0.5)
            beats.push_back (t);
    }

    const auto features = measureBeats (audio, sampleRate, beats, shouldContinue);
    if (! keepGoing() || features.levelDb.size() != beats.size())
    {
        map.cancelled = ! keepGoing();
        return map;
    }

    const auto bar = map.beatsPerBar;
    map.firstDownbeat = map.hasBeats() ? findDownbeatPhase (features, bar, tempo.firstDownbeat % bar) : 0;

    // Leave out the bars where nothing plays at the start (the beat tracker's first beats
    // may sit in a quiet lead-in).
    while (map.firstDownbeat + bar < (int) beats.size()
           && features.levelDb[(size_t) map.firstDownbeat] < silenceDb
           && features.levelDb[(size_t) (map.firstDownbeat + 1)] < silenceDb)
        map.firstDownbeat += bar;

    for (auto i = (size_t) map.firstDownbeat; i < beats.size(); i += (size_t) bar)
        map.downbeats.push_back (beats[i]);

    // The end of the last bar: one bar on (at the last beat spacing), within the audio.
    if (! map.downbeats.empty())
    {
        const auto lastIndex = (size_t) map.firstDownbeat + (map.downbeats.size() - 1) * (size_t) bar;
        const auto period = beats.size() >= 2 ? beats.back() - beats[beats.size() - 2] : 0.5;
        const auto lastBeat = beats.back();
        const auto end = beats[lastIndex] + bar * period;
        if (lastIndex + 1 < beats.size() || end <= seconds + period)
            map.downbeats.push_back (juce::jmin (end, juce::jmax (lastBeat + period, seconds)));
    }

    const auto numBars = map.getNumBars();
    if (numBars <= 0)
        return map;

    const auto bars = getBarFeatures (features, map.firstDownbeat, bar, numBars);
    map.sections = findSections (bars, options);

    for (auto& s : map.sections)
    {
        s.start = map.downbeats[(size_t) s.firstBar];
        s.end = map.downbeats[(size_t) juce::jmin (numBars, s.firstBar + s.bars)];
    }

    if (! map.hasBeats())
        map.downbeats.clear(); // the bars were only a grid for the sections

    return map;
}
}
