#include "Chords.h"

#include <cmath>

namespace analysis
{
namespace
{
    // Each quality's notes (semitones above the root) and how strongly each is expected
    // to sound: root and third most, the fifth a bit less (it is also the root's third
    // harmonic, so it shows up anyway).
    struct Shape
    {
        std::vector<int> intervals;
        std::vector<float> weights;
        float prior; // the rarer chords need a clearer match
        const char* suffix;
    };

    const std::array<Shape, Chord::numQualities>& getShapes()
    {
        static const std::array<Shape, Chord::numQualities> shapes {{
            { { 0, 4, 7 },     { 1.0f, 1.0f, 0.85f },        1.00f, "" },
            { { 0, 3, 7 },     { 1.0f, 1.0f, 0.85f },        1.00f, "m" },
            { { 0, 4, 7, 10 }, { 1.0f, 1.0f, 0.85f, 0.8f },  0.94f, "7" },
            { { 0, 4, 7, 11 }, { 1.0f, 1.0f, 0.85f, 0.8f },  0.93f, "maj7" },
            { { 0, 3, 7, 10 }, { 1.0f, 1.0f, 0.85f, 0.8f },  0.94f, "m7" },
            { { 0, 3, 6 },     { 1.0f, 1.0f, 0.9f },         0.88f, "dim" },
            { { 0, 5, 7 },     { 1.0f, 1.0f, 0.85f },        0.88f, "sus4" },
            { { 0, 2, 7 },     { 1.0f, 1.0f, 0.85f },        0.88f, "sus2" },
            { { 0, 4, 8 },     { 1.0f, 1.0f, 0.9f },         0.85f, "aug" },
            { { 0, 7 },        { 1.0f, 0.85f },              0.82f, "5" },
        }};
        return shapes;
    }

    // Pre-computed pattern lengths, so scoring allocates nothing.
    const std::array<float, Chord::numQualities>& getShapeNorms()
    {
        static const auto norms = []
        {
            std::array<float, Chord::numQualities> result {};
            for (size_t q = 0; q < result.size(); ++q)
            {
                float squares = 0.0f;
                for (auto w : getShapes()[q].weights)
                    squares += w * w;
                result[q] = std::sqrt (squares);
            }
            return result;
        }();
        return norms;
    }

    int wrap (int pitchClass) noexcept { return ((pitchClass % 12) + 12) % 12; }

    const char* const noteNames[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
}

//==============================================================================
const std::vector<int>& Chord::getIntervals (int q)
{
    return getShapes()[(size_t) juce::jlimit (0, numQualities - 1, q)].intervals;
}

int Chord::getMask() const noexcept
{
    if (! isValid())
        return 0;

    int mask = 0;
    for (auto interval : getShapes()[(size_t) juce::jlimit (0, numQualities - 1, quality)].intervals)
        mask |= 1 << wrap (root + interval);
    return mask;
}

juce::String Chord::getName() const
{
    if (! isValid())
        return {};

    juce::String name (noteNames[root]);
    name << getShapes()[(size_t) juce::jlimit (0, numQualities - 1, quality)].suffix;

    if (bass >= 0 && bass != root)
        name << "/" << noteNames[wrap (bass)];

    return name;
}

Chord Chord::unpack (int packed) noexcept
{
    if (packed < 0)
        return {};

    Chord chord;
    chord.root = packed & 15;
    chord.quality = juce::jlimit (0, numQualities - 1, (packed >> 4) & 15);
    chord.bass = ((packed >> 8) & 31) - 1;
    if (chord.root > 11)
        return {};
    return chord;
}

int getKeyMask (int key, bool minor) noexcept
{
    static constexpr int major[] = { 0, 2, 4, 5, 7, 9, 11 }, naturalMinor[] = { 0, 2, 3, 5, 7, 8, 10 };
    if (key < 0)
        return 0;

    int mask = 0;
    for (auto degree : minor ? naturalMinor : major)
        mask |= 1 << wrap (key + degree);
    return mask;
}

//==============================================================================
float scoreChord (const std::array<float, 12>& chroma, int root, int quality, int bass, int keyMask) noexcept
{
    const auto& shape = getShapes()[(size_t) quality];

    float dot = 0.0f, squares = 0.0f;
    for (auto value : chroma)
        squares += value * value;

    if (squares <= 0.0f)
        return 0.0f;

    int mask = 0;
    for (size_t i = 0; i < shape.intervals.size(); ++i)
    {
        const auto pitchClass = wrap (root + shape.intervals[i]);
        dot += chroma[(size_t) pitchClass] * shape.weights[i];
        mask |= 1 << pitchClass;
    }

    auto score = dot / (std::sqrt (squares) * getShapeNorms()[(size_t) quality]) * shape.prior;

    // The lowest note: on the root is the usual case, another chord note an inversion,
    // anything else speaks against the chord.
    if (bass >= 0)
        score += bass == root ? 0.06f : ((mask >> bass) & 1) != 0 ? 0.02f : -0.04f;

    // The key's own chords are the likely ones.
    if (keyMask != 0 && (mask & ~keyMask) == 0)
        score += 0.03f;

    return score;
}

Chord matchChord (const std::array<float, 12>& chroma, int bass, float minScore, float* score, int keyMask) noexcept
{
    Chord best;
    auto bestScore = -1.0f;

    for (int root = 0; root < 12; ++root)
    {
        for (int quality = 0; quality < Chord::numQualities; ++quality)
        {
            const auto s = scoreChord (chroma, root, quality, bass, keyMask);
            if (s > bestScore)
            {
                bestScore = s;
                best.root = root;
                best.quality = quality;
            }
        }
    }

    if (score != nullptr)
        *score = juce::jmax (0.0f, bestScore);

    if (bestScore < minScore || bestScore <= 0.0f)
        return {};

    if (bass >= 0 && bass != best.root && best.contains (bass))
        best.bass = bass;

    return best;
}

Chord chordFromNotes (const std::bitset<128>& notes, int keyMask) noexcept
{
    std::array<float, 12> chroma {};
    int lowest = -1, distinct = 0;

    for (int note = 0; note < 128; ++note)
    {
        if (! notes[(size_t) note])
            continue;

        if (lowest < 0)
            lowest = note;
        if (chroma[(size_t) (note % 12)] == 0.0f)
            ++distinct;
        chroma[(size_t) (note % 12)] = 1.0f;
    }

    if (distinct == 0)
        return {};

    if (distinct == 1)
    {
        // A single key: the key's triad on it (major when the key is unknown).
        const auto root = lowest % 12;
        const auto has = [keyMask, root] (int interval) { return ((keyMask >> wrap (root + interval)) & 1) != 0; };

        Chord chord;
        chord.root = root;
        chord.quality = keyMask == 0 || (has (4) && has (7)) ? Chord::major
                      : has (3) && has (7)                   ? Chord::minor
                      : has (3) && has (6)                   ? Chord::diminished
                                                             : Chord::major;
        return chord;
    }

    return matchChord (chroma, lowest % 12, 0.0f, nullptr, keyMask);
}

std::array<float, 12> foldChroma (const Chroma& chroma, float tuningSemitones) noexcept
{
    std::array<float, 12> folded {};

    for (int bin = 0; bin < numChromaBins; ++bin)
    {
        const auto semitones = (float) bin / chromaBinsPerNote - tuningSemitones;
        folded[(size_t) wrap ((int) std::lround (semitones))] += chroma[(size_t) bin];
    }

    const auto peak = *std::max_element (folded.begin(), folded.end());
    if (peak > 0.0f)
        for (auto& value : folded)
            value /= peak;

    return folded;
}

//==============================================================================
namespace
{
    constexpr float bassTopHz = 260.0f;

    // The chroma of only the low notes: everything above bassTopHz silenced.
    bool computeBassChroma (const float* magnitudes, int fftSize, double sampleRate, std::vector<float>& scratch, Chroma& bass)
    {
        const auto bins = (size_t) fftSize / 2 + 1;
        std::copy (magnitudes, magnitudes + bins, scratch.begin());
        const auto top = (size_t) juce::jlimit (0, (int) bins, (int) std::ceil (bassTopHz * fftSize / sampleRate));
        std::fill (scratch.begin() + (std::ptrdiff_t) top, scratch.begin() + (std::ptrdiff_t) bins, 0.0f);
        return computeChroma (scratch.data(), fftSize, sampleRate, bass);
    }

    int strongest (const std::array<float, 12>& chroma) noexcept
    {
        const auto it = std::max_element (chroma.begin(), chroma.end());
        return *it > 0.0f ? (int) std::distance (chroma.begin(), it) : -1;
    }
}

std::vector<ChordSegment> detectChords (const juce::AudioBuffer<float>& audio, double sampleRate,
                                        const KeyEstimate* key, const std::function<bool()>& shouldContinue)
{
    const auto fftOrder = sampleRate > 60000.0 ? 14 : 13;
    const auto fftSize = 1 << fftOrder;
    constexpr double silenceDb = -50.0;

    const auto hop = juce::jmax (1, (int) std::lround (sampleRate * 0.1));
    const auto numSamples = audio.getNumSamples();
    const auto numChannels = juce::jmin (2, audio.getNumChannels());
    if (numSamples < hop || numChannels == 0)
        return {};

    juce::dsp::FFT fft (fftOrder);
    std::vector<float> window ((size_t) fftSize), data ((size_t) fftSize * 2), scratch ((size_t) fftSize / 2 + 1);
    for (int i = 0; i < fftSize; ++i)
        window[(size_t) i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) fftSize);

    const auto mono = [&] (int i)
    {
        auto value = audio.getSample (0, i);
        if (numChannels > 1)
            value = 0.5f * (value + audio.getSample (1, i));
        return value;
    };

    // Frame f covers the hop ending at (f + 1) * hop; it describes the window's middle.
    const auto numFrames = numSamples / hop;
    std::vector<Chroma> frames ((size_t) numFrames), bassFrames ((size_t) numFrames);
    std::vector<bool> use ((size_t) numFrames, false);

    for (int f = 0; f < numFrames; ++f)
    {
        if (shouldContinue && f % 50 == 0 && ! shouldContinue())
            return {};

        const auto end = (f + 1) * hop;
        double squares = 0.0;
        for (int i = end - hop; i < end; ++i)
            squares += juce::square ((double) mono (i));
        if (10.0 * std::log10 (squares / hop + 1.0e-20) < silenceDb)
            continue;

        // Centred on the hop's end so the frame describes the moment it's stamped with.
        const auto centre = end;
        for (int i = 0; i < fftSize; ++i)
        {
            const auto source = centre - fftSize / 2 + i;
            data[(size_t) i] = source >= 0 && source < numSamples ? mono (source) * window[(size_t) i] : 0.0f;
        }
        std::fill (data.begin() + fftSize, data.end(), 0.0f);
        fft.performFrequencyOnlyForwardTransform (data.data(), true);

        use[(size_t) f] = computeChroma (data.data(), fftSize, sampleRate, frames[(size_t) f]);
        if (use[(size_t) f])
            computeBassChroma (data.data(), fftSize, sampleRate, scratch, bassFrames[(size_t) f]);
    }

    // The song's tuning (and key, when not given) from all of it.
    const auto estimate = estimateKey (frames, &use);
    const auto tuning = estimate.tuningCents / 100.0f;
    const auto& songKey = key != nullptr && key->isValid() ? *key : estimate;
    const auto keyMask = songKey.isValid() ? getKeyMask (songKey.key, songKey.minor) : 0;

    // Note weights per frame, lightly smoothed with the neighbours (a chord lasts longer
    // than 100 ms; melody notes and drum hits come and go).
    std::vector<std::array<float, 12>> notes ((size_t) numFrames), basses ((size_t) numFrames);
    for (int f = 0; f < numFrames; ++f)
    {
        if (! use[(size_t) f])
            continue;

        Chroma sum {}, bassSum {};
        for (int g = juce::jmax (0, f - 1); g <= juce::jmin (numFrames - 1, f + 1); ++g)
        {
            if (! use[(size_t) g])
                continue;
            addChroma (sum, frames[(size_t) g], g == f ? 1.0f : 0.5f);
            addChroma (bassSum, bassFrames[(size_t) g], g == f ? 1.0f : 0.5f);
        }
        notes[(size_t) f] = foldChroma (sum, tuning);
        basses[(size_t) f] = foldChroma (bassSum, tuning);
    }

    // The best chord path: each frame scores every chord, staying costs nothing and a
    // switch costs `switchCost`, so a chord needs a few clear frames to take over.
    constexpr int numStates = 12 * Chord::numQualities;
    constexpr float switchCost = 0.35f;

    std::vector<float> total (numStates, 0.0f), next (numStates);
    std::vector<std::vector<short>> from ((size_t) numFrames, std::vector<short> (numStates));
    std::vector<float> frameScore ((size_t) numFrames * numStates, 0.0f);

    for (int f = 0; f < numFrames; ++f)
    {
        const auto bestPrevious = (int) std::distance (total.begin(), std::max_element (total.begin(), total.end()));

        for (int s = 0; s < numStates; ++s)
        {
            const auto score = use[(size_t) f] ? scoreChord (notes[(size_t) f], s % 12, s / 12, strongest (basses[(size_t) f]), keyMask)
                                               : 0.0f;
            frameScore[(size_t) f * numStates + (size_t) s] = score;

            const auto stay = total[(size_t) s];
            const auto change = total[(size_t) bestPrevious] - switchCost;
            next[(size_t) s] = juce::jmax (stay, change) + score;
            from[(size_t) f][(size_t) s] = (short) (stay >= change ? s : bestPrevious);
        }

        std::swap (total, next);
    }

    std::vector<int> path ((size_t) numFrames);
    auto state = (int) std::distance (total.begin(), std::max_element (total.begin(), total.end()));
    for (int f = numFrames - 1; f >= 0; --f)
    {
        path[(size_t) f] = state;
        state = from[(size_t) f][(size_t) state];
    }

    // Runs of one chord become segments; runs that are mostly silence or fit badly are
    // dropped (the chord before them carries on).
    std::vector<ChordSegment> segments;
    for (int f = 0; f < numFrames;)
    {
        auto g = f;
        while (g < numFrames && path[(size_t) g] == path[(size_t) f])
            ++g;

        const auto s = path[(size_t) f];
        float scoreSum = 0.0f;
        int used = 0;
        std::array<float, 12> bassSum {};
        for (int i = f; i < g; ++i)
        {
            if (! use[(size_t) i])
                continue;
            scoreSum += frameScore[(size_t) i * numStates + (size_t) s];
            ++used;
            for (size_t n = 0; n < 12; ++n)
                bassSum[n] += basses[(size_t) i][n];
        }

        if (used > 0 && scoreSum / (float) used >= 0.6f)
        {
            ChordSegment segment;
            segment.start = (juce::int64) f * hop + hop / 2;
            segment.chord.root = s % 12;
            segment.chord.quality = s / 12;

            const auto bass = strongest (bassSum);
            if (bass >= 0 && bass != segment.chord.root && segment.chord.contains (bass))
                segment.chord.bass = bass;

            if (segments.empty() || segments.back().chord != segment.chord)
                segments.push_back (segment);
        }

        f = g;
    }

    return segments;
}

Chord chordAt (const std::vector<ChordSegment>& segments, juce::int64 position) noexcept
{
    const auto it = std::upper_bound (segments.begin(), segments.end(), position,
                                      [] (juce::int64 p, const ChordSegment& s) { return p < s.start; });
    return it == segments.begin() ? Chord {} : std::prev (it)->chord;
}

//==============================================================================
void ChordFollower::prepare (double newSampleRate)
{
    sampleRate = newSampleRate;
    fftOrder = sampleRate > 60000.0 ? 14 : 13;
    fftSize = 1 << fftOrder;
    hop = fftSize / 8; // about 20 ms
    fft = std::make_unique<juce::dsp::FFT> (fftOrder);

    ring.assign ((size_t) fftSize, 0.0f);
    window.resize ((size_t) fftSize);
    for (int i = 0; i < fftSize; ++i)
        window[(size_t) i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) fftSize);
    data.assign ((size_t) fftSize * 2, 0.0f);
    bassData.assign ((size_t) fftSize / 2 + 1, 0.0f);

    // The note weights glide over about 120 ms.
    smoothing = std::exp (-(float) hop / (float) (0.12 * sampleRate));
    reset();
}

void ChordFollower::reset() noexcept
{
    std::fill (ring.begin(), ring.end(), 0.0f);
    writeIndex = sinceHop = filled = 0;
    smoothed.fill (0.0f);
    smoothedBass.fill (0.0f);
    candidate = {};
    candidateHops = 0;
    holdingNotes = false;
    current.store (-1, std::memory_order_relaxed);
}

void ChordFollower::setCurrent (const Chord& chord) noexcept
{
    current.store (chord.pack(), std::memory_order_relaxed);
}

void ChordFollower::setHeldNotes (const std::bitset<128>& notes) noexcept
{
    holdingNotes = notes.any();
    if (holdingNotes)
        if (const auto chord = chordFromNotes (notes, keyMask); chord.isValid())
            setCurrent (chord);
}

void ChordFollower::pushAudio (const float* samples, int numSamples) noexcept
{
    if (ring.empty())
        return;

    for (int i = 0; i < numSamples; ++i)
    {
        ring[(size_t) writeIndex] = samples[i];
        writeIndex = (writeIndex + 1) % fftSize;

        if (++sinceHop >= hop)
        {
            sinceHop = 0;
            filled = juce::jmin (fftSize, filled + hop);
            if (filled >= fftSize / 2)
                analyse();
        }
    }
}

void ChordFollower::analyse() noexcept
{
    // Quiet: keep the chord (a pause in the song shouldn't drop the harmonies' chord).
    double squares = 0.0;
    for (int i = 0; i < hop; ++i)
        squares += juce::square ((double) ring[(size_t) ((writeIndex - 1 - i + fftSize) % fftSize)]);
    if (10.0 * std::log10 (squares / hop + 1.0e-20) < -55.0)
        return;

    for (int i = 0; i < fftSize; ++i)
        data[(size_t) i] = ring[(size_t) ((writeIndex + i) % fftSize)] * window[(size_t) i];
    std::fill (data.begin() + fftSize, data.end(), 0.0f);
    fft->performFrequencyOnlyForwardTransform (data.data(), true);

    Chroma chroma {}, bass {};
    if (! computeChroma (data.data(), fftSize, sampleRate, chroma))
        return;
    const auto hasBass = computeBassChroma (data.data(), fftSize, sampleRate, bassData, bass);

    const auto frame = foldChroma (chroma);
    const auto bassFrame = foldChroma (bass);
    for (size_t n = 0; n < 12; ++n)
    {
        smoothed[n] = smoothing * smoothed[n] + (1.0f - smoothing) * frame[n];
        smoothedBass[n] = smoothing * smoothedBass[n] + (1.0f - smoothing) * (hasBass ? bassFrame[n] : 0.0f);
    }

    if (holdingNotes)
        return; // held notes decide

    const auto bassNote = strongest (smoothedBass);
    float bestScore = 0.0f;
    const auto best = matchChord (smoothed, bassNote, 0.6f, &bestScore, keyMask);

    if (! best.isValid())
    {
        candidateHops = 0;
        return;
    }

    const auto now = getChord();

    // The same chord, maybe over another bass note: follow the bass at once.
    if (now.isValid() && now.root == best.root && now.quality == best.quality)
    {
        candidateHops = 0;
        if (now.bass != best.bass)
            setCurrent (best);
        return;
    }

    // Another chord takes over once it's clearly better for a few hops in a row (~60 ms).
    if (now.isValid() && bestScore < scoreChord (smoothed, now.root, now.quality, bassNote, keyMask) + 0.02f)
    {
        candidateHops = 0;
        return;
    }

    if (candidate.root == best.root && candidate.quality == best.quality)
        ++candidateHops;
    else
    {
        candidate = best;
        candidateHops = 1;
    }

    if (candidateHops >= 3)
    {
        setCurrent (best);
        candidateHops = 0;
    }
}
} // namespace analysis
