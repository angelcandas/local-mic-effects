#include "TakeComp.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include "daw/Edit.h"

#include <cmath>
#include <map>
#include <numeric>

namespace analysis
{
namespace ids = daw::ids;

namespace
{
    constexpr double hop = Profile::hopSeconds;

    // Adds a clip's audio (mono, at analysisRate) into `out`, which starts at `from`.
    void addClip (juce::AudioFormatManager& formats, const juce::ValueTree& clip, double from, std::vector<float>& out,
                  const std::function<bool()>& shouldContinue)
    {
        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (juce::File (clip[ids::file].toString())));
        if (reader == nullptr || reader->sampleRate <= 0.0)
            return;

        const auto start = (double) clip[ids::start], offset = juce::jmax (0.0, (double) clip[ids::offset]);
        const auto length = (double) clip[ids::length];
        const auto gain = juce::Decibels::decibelsToGain ((float) clip.getProperty (ids::gain, 0.0f));
        const auto ratio = reader->sampleRate / analysisRate;
        const auto channels = (int) juce::jmin (2u, reader->numChannels);

        auto outPos = (juce::int64) std::llround ((start - from) * analysisRate);
        auto filePos = (juce::int64) std::llround (offset * reader->sampleRate);
        const auto fileEnd = juce::jmin (reader->lengthInSamples, filePos + (juce::int64) std::llround (length * reader->sampleRate));

        constexpr int blockOut = 1 << 14;
        const auto blockIn = (int) std::ceil (blockOut * ratio) + 8;
        juce::AudioBuffer<float> in (channels, blockIn);
        std::vector<float> mono ((size_t) blockIn), resampled ((size_t) blockOut);
        juce::LagrangeInterpolator interpolator;

        while (filePos < fileEnd && outPos < (juce::int64) out.size())
        {
            if (shouldContinue && ! shouldContinue())
                return;

            const auto wantOut = (int) juce::jmin ((juce::int64) blockOut, (juce::int64) out.size() - outPos);
            const auto count = (int) juce::jmin ((juce::int64) blockIn, fileEnd - filePos);
            in.clear();
            reader->read (&in, 0, count, filePos, true, channels > 1);

            for (int i = 0; i < count; ++i)
                mono[(size_t) i] = channels > 1 ? 0.5f * (in.getSample (0, i) + in.getSample (1, i)) : in.getSample (0, i);
            std::fill (mono.begin() + count, mono.end(), 0.0f);

            const auto produced = juce::jmin (wantOut, (int) std::floor (count / ratio));
            if (produced <= 0)
                break;
            const auto used = interpolator.process (ratio, mono.data(), resampled.data(), produced);

            for (int i = 0; i < produced; ++i)
                if (const auto at = outPos + i; at >= 0)
                    out[(size_t) at] += resampled[(size_t) i] * gain;

            outPos += produced;
            filePos += juce::jmax (1, used);
        }
    }

    float powerMeanLufs (const std::vector<float>& values)
    {
        if (values.empty())
            return -200.0f;
        double sum = 0.0;
        for (auto v : values)
            sum += std::pow (10.0, v / 10.0);
        return (float) (10.0 * std::log10 (juce::jmax (1.0e-20, sum / (double) values.size())));
    }

    struct Judged
    {
        bool eligible = false;
        float score = 0.0f;
        float pitchCents = 0.0f;  // average distance from the nearest note
        float missing = 0.0f;     // 0..1: how much less it sings than the take that sings most
        int clippedHops = 0;
        float levelPenalty = 0.0f;
        float pitchWeight = 0.0f;
    };

    bool isActive (const TakeAudio& take, int frame)
    {
        return frame < (int) take.profile.active.size() && take.profile.active[(size_t) frame] && take.covered[(size_t) frame];
    }
}

//==============================================================================
CompInput measureTakes (const juce::ValueTree& track, const std::function<bool()>& shouldContinue)
{
    CompInput input;

    double from = std::numeric_limits<double>::max(), to = 0.0;
    for (const auto& take : track)
        if (take.hasType (ids::TAKE))
            for (const auto& clip : take)
                if (clip.hasType (ids::CLIP))
                {
                    from = juce::jmin (from, (double) clip[ids::start]);
                    to = juce::jmax (to, (double) clip[ids::start] + (double) clip[ids::length]);
                }

    if (to <= from)
        return input;

    // On the 100 ms grid, so a phrase's cut is a whole number of hops from the start.
    from = std::floor (from / hop) * hop;
    input.start = from;
    const auto numSamples = (size_t) std::ceil ((to - from) * analysisRate);
    const auto hopSamples = (int) std::llround (hop * analysisRate);

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();

    for (const auto& take : track)
    {
        if (! take.hasType (ids::TAKE))
            continue;

        std::vector<float> audio (numSamples, 0.0f);
        bool any = false;

        TakeAudio measured;
        measured.takeId = (int) take[ids::id];
        measured.name = take[ids::name].toString();

        const auto numHops = (numSamples + (size_t) hopSamples - 1) / (size_t) hopSamples;
        measured.covered.assign (numHops, false);

        for (const auto& clip : take)
        {
            if (! clip.hasType (ids::CLIP) || (bool) clip.getProperty (ids::mute, false))
                continue;

            addClip (formats, clip, from, audio, shouldContinue);
            any = true;

            const auto s = (double) clip[ids::start] - from, e = s + (double) clip[ids::length];
            for (size_t i = 0; i < numHops; ++i)
                if (const auto middle = ((double) i + 0.5) * hop; middle >= s && middle < e)
                    measured.covered[i] = true;
        }

        if (shouldContinue && ! shouldContinue())
            return {};
        if (! any)
            continue;

        juce::AudioBuffer<float> buffer (1, (int) numSamples);
        buffer.copyFrom (0, 0, audio.data(), (int) numSamples);
        measured.profile = analyse (buffer, analysisRate);

        // The profile's hops end where the audio does: line the masks up with them.
        const auto frames = (size_t) measured.profile.getNumFrames();
        measured.covered.resize (frames, false);
        measured.peak.assign (frames, 0.0f);
        for (size_t i = 0; i < frames; ++i)
        {
            const auto a = i * (size_t) hopSamples, b = juce::jmin (numSamples, a + (size_t) hopSamples);
            for (auto j = a; j < b; ++j)
                measured.peak[i] = juce::jmax (measured.peak[i], std::abs (audio[j]));
        }

        input.takes.push_back (std::move (measured));
    }

    return input;
}

//==============================================================================
std::vector<double> findPhraseCuts (const CompInput& input)
{
    std::vector<double> cuts;
    if (input.takes.empty())
        return cuts;

    int frames = std::numeric_limits<int>::max();
    for (const auto& take : input.takes)
        frames = juce::jmin (frames, take.profile.getNumFrames());
    if (frames <= 0)
        return cuts;

    // Quiet: every take is silent there (or doesn't reach it).
    std::vector<bool> quiet ((size_t) frames, true);
    for (const auto& take : input.takes)
        for (int i = 0; i < frames; ++i)
            if (isActive (take, i))
                quiet[(size_t) i] = false;

    // Candidates: the middle of each quiet run, and wherever a take starts or stops.
    std::vector<int> candidates;
    for (int i = 0; i < frames;)
    {
        if (! quiet[(size_t) i])
        {
            ++i;
            continue;
        }
        auto j = i;
        while (j < frames && quiet[(size_t) j])
            ++j;
        if (i > 0 && j < frames)
            candidates.push_back ((i + j) / 2);
        i = j;
    }
    for (const auto& take : input.takes)
        for (int i = 1; i < frames; ++i)
            if (take.covered[(size_t) i] != take.covered[(size_t) i - 1])
                candidates.push_back (i);

    std::sort (candidates.begin(), candidates.end());
    candidates.erase (std::unique (candidates.begin(), candidates.end()), candidates.end());

    // Coverage edges must stay (a take can't play where it has no audio); the rest only
    // when the phrases either side are long enough.
    const auto isEdge = [&] (int frame)
    {
        return std::any_of (input.takes.begin(), input.takes.end(), [&] (const TakeAudio& take)
                            { return frame > 0 && take.covered[(size_t) frame] != take.covered[(size_t) frame - 1]; });
    };

    const auto minFrames = (int) std::llround (minPhraseSeconds / hop);
    std::vector<int> kept;
    auto last = 0;
    for (auto frame : candidates)
    {
        if (isEdge (frame) || (frame - last >= minFrames && frames - frame >= minFrames))
        {
            kept.push_back (frame);
            last = frame;
        }
    }

    // A phrase that runs on too long is cut at its quietest hop (all takes together).
    const auto maxFrames = (int) std::llround (maxPhraseSeconds / hop);
    std::vector<int> bounds { 0 };
    bounds.insert (bounds.end(), kept.begin(), kept.end());
    bounds.push_back (frames);

    std::vector<int> all;
    std::function<void (int, int)> split = [&] (int a, int b)
    {
        if (b - a <= maxFrames)
            return;

        auto best = -1;
        auto bestLevel = std::numeric_limits<double>::max();
        for (int i = a + minFrames; i <= b - minFrames; ++i)
        {
            double level = 0.0;
            for (const auto& take : input.takes)
                if (take.covered[(size_t) i])
                    level += std::pow (10.0, take.profile.rmsDb[(size_t) i] / 10.0);
            if (level < bestLevel)
            {
                bestLevel = level;
                best = i;
            }
        }
        if (best < 0)
            return;
        split (a, best);
        all.push_back (best);
        split (best, b);
    };

    for (size_t i = 0; i + 1 < bounds.size(); ++i)
    {
        if (i > 0)
            all.push_back (bounds[i]);
        split (bounds[i], bounds[i + 1]);
    }

    std::sort (all.begin(), all.end());
    for (auto frame : all)
        cuts.push_back (input.start + frame * hop);
    return cuts;
}

//==============================================================================
CompPlan planComp (const CompInput& input)
{
    CompPlan plan;
    const auto numTakes = (int) input.takes.size();
    if (numTakes < 2)
    {
        plan.summary = "Needs at least two takes to choose from.";
        return plan;
    }

    int frames = std::numeric_limits<int>::max();
    for (const auto& take : input.takes)
        frames = juce::jmin (frames, take.profile.getNumFrames());
    if (frames <= 0)
        return plan;

    // The takes' common tuning against A = 440: where most sung hops sit from their
    // nearest semitone (the peak of a histogram, refined by a circular mean around it),
    // so a song tuned a little off isn't "out", and the wrong notes don't pull it.
    std::vector<double> deviations;
    for (const auto& take : input.takes)
        for (int i = 0; i < frames; ++i)
            if (const auto hz = take.profile.pitchHz[(size_t) i]; hz > 0.0f && isActive (take, i))
            {
                const auto cents = 1200.0 * std::log2 (hz / 440.0);
                deviations.push_back (cents - 100.0 * std::round (cents / 100.0));
            }

    double tuning = 0.0;
    if (! deviations.empty())
    {
        constexpr int bins = 20; // 5 cents each, -50 .. 50
        std::array<int, bins> histogram {};
        for (auto d : deviations)
            ++histogram[(size_t) juce::jlimit (0, bins - 1, (int) std::floor ((d + 50.0) / 5.0))];

        auto peak = 0, peakCount = -1;
        for (int b = 0; b < bins; ++b)
            if (const auto count = histogram[(size_t) b] * 2 + histogram[(size_t) ((b + 1) % bins)] + histogram[(size_t) ((b + bins - 1) % bins)];
                count > peakCount)
            {
                peakCount = count;
                peak = b;
            }

        const auto centre = -50.0 + 5.0 * peak + 2.5;
        double sine = 0.0, cosine = 0.0;
        for (auto d : deviations)
        {
            auto offset = d - centre;
            offset -= 100.0 * std::round (offset / 100.0);
            if (std::abs (offset) <= 12.0)
            {
                sine += std::sin (juce::MathConstants<double>::twoPi * d / 100.0);
                cosine += std::cos (juce::MathConstants<double>::twoPi * d / 100.0);
            }
        }
        tuning = std::atan2 (sine, cosine) * 100.0 / juce::MathConstants<double>::twoPi;
    }

    std::array<bool, 12> inKey;
    inKey.fill (true);
    if (input.keyTonic >= 0)
    {
        static constexpr int major[] { 0, 2, 4, 5, 7, 9, 11 }, minor[] { 0, 2, 3, 5, 7, 8, 10 };
        inKey.fill (false);
        for (auto step : input.keyMinor ? minor : major)
            inKey[(size_t) ((input.keyTonic + step) % 12)] = true;
    }

    // Distance from the nearest note, in cents (a note out of the key counts 15 more).
    const auto pitchError = [&] (float hz)
    {
        const auto midi = 69.0 + (1200.0 * std::log2 (hz / 440.0) - tuning) / 100.0;
        const auto nearest = std::round (midi);
        const auto pitchClass = (((int) nearest % 12) + 12) % 12;
        return (float) juce::jmin (50.0, std::abs (midi - nearest) * 100.0 + (inKey[(size_t) pitchClass] ? 0.0 : 15.0));
    };

    // Phrases.
    std::vector<int> bounds { 0 };
    for (auto cut : findPhraseCuts (input))
        bounds.push_back ((int) std::llround ((cut - input.start) / hop));
    bounds.push_back (frames);
    const auto numPhrases = (int) bounds.size() - 1;
    plan.phrases = numPhrases;

    std::vector<std::vector<Judged>> judged ((size_t) numPhrases, std::vector<Judged> ((size_t) numTakes));

    for (int p = 0; p < numPhrases; ++p)
    {
        const auto a = bounds[(size_t) p], b = bounds[(size_t) p + 1];
        auto& row = judged[(size_t) p];

        float mostActive = 0.0f;
        int voiced = 0, active = 0;
        std::vector<float> loudness;

        for (int t = 0; t < numTakes; ++t)
        {
            const auto& take = input.takes[(size_t) t];
            auto& j = row[(size_t) t];

            int covered = 0, activeHops = 0, voicedHops = 0;
            double errorSum = 0.0;
            std::vector<float> lufs;

            for (int i = a; i < b; ++i)
            {
                if (! take.covered[(size_t) i])
                    continue;
                ++covered;
                if (take.peak[(size_t) i] >= 0.989f)
                    ++j.clippedHops;
                if (! isActive (take, i))
                    continue;
                ++activeHops;
                lufs.push_back (take.profile.momentaryLufs[(size_t) i]);
                if (const auto hz = take.profile.pitchHz[(size_t) i]; hz > 0.0f)
                {
                    ++voicedHops;
                    errorSum += pitchError (hz);
                }
            }

            j.eligible = covered >= (int) std::ceil (0.9 * (b - a));
            if (! j.eligible)
                continue;

            j.pitchCents = voicedHops > 0 ? (float) (errorSum / voicedHops) : 0.0f;
            j.missing = (float) activeHops / (float) juce::jmax (1, b - a); // the active share, for now
            mostActive = juce::jmax (mostActive, j.missing);
            voiced += voicedHops;
            active += activeHops;
            if (! lufs.empty())
                loudness.push_back (powerMeanLufs (lufs));
        }

        std::sort (loudness.begin(), loudness.end());
        const auto medianLoudness = loudness.empty() ? -200.0f : loudness[loudness.size() / 2];

        // Pitch counts where the takes are mostly sung (not spoken, not breaths).
        const auto pitchWeight = active > 0 && (float) voiced / (float) active >= 0.3f ? 1.0f : 0.0f;

        for (int t = 0; t < numTakes; ++t)
        {
            auto& j = row[(size_t) t];
            if (! j.eligible)
                continue;

            const auto share = j.missing;
            j.missing = mostActive > 0.0f ? juce::jmax (0.0f, mostActive - share) / mostActive : 0.0f;

            const auto& take = input.takes[(size_t) t];
            std::vector<float> lufs;
            for (int i = a; i < b; ++i)
                if (isActive (take, i))
                    lufs.push_back (take.profile.momentaryLufs[(size_t) i]);
            const auto level = lufs.empty() ? medianLoudness : powerMeanLufs (lufs);
            j.levelPenalty = juce::jlimit (0.0f, 1.0f, (medianLoudness - level - 4.0f) / 8.0f);
            j.pitchWeight = pitchWeight;

            j.score = 100.0f - pitchWeight * 60.0f * (j.pitchCents / 50.0f) - 30.0f * j.missing
                    - 10.0f * juce::jmin (1.0f, j.clippedHops / 3.0f) - 10.0f * j.levelPenalty;
        }
    }

    // One take per phrase: the best total score, less a cost for every switch. Phrases
    // no take covers break the chain (the comp is left as it is there).
    constexpr float impossible = -1.0e9f;
    std::vector<int> choice ((size_t) numPhrases, -1);

    for (int p = 0; p < numPhrases;)
    {
        auto q = p;
        while (q < numPhrases && std::any_of (judged[(size_t) q].begin(), judged[(size_t) q].end(), [] (const Judged& j) { return j.eligible; }))
            ++q;

        if (q == p)
        {
            ++p;
            continue;
        }

        const auto length = q - p;
        std::vector<std::vector<float>> best ((size_t) length, std::vector<float> ((size_t) numTakes, impossible));
        std::vector<std::vector<int>> from ((size_t) length, std::vector<int> ((size_t) numTakes, -1));

        for (int k = 0; k < length; ++k)
            for (int t = 0; t < numTakes; ++t)
            {
                const auto& j = judged[(size_t) (p + k)][(size_t) t];
                if (! j.eligible)
                    continue;
                if (k == 0)
                {
                    best[0][(size_t) t] = j.score;
                    continue;
                }
                for (int u = 0; u < numTakes; ++u)
                    if (const auto prev = best[(size_t) k - 1][(size_t) u]; prev > impossible)
                        if (const auto total = prev + j.score - (u != t ? switchCost : 0.0f); total > best[(size_t) k][(size_t) t])
                        {
                            best[(size_t) k][(size_t) t] = total;
                            from[(size_t) k][(size_t) t] = u;
                        }
            }

        auto t = (int) std::distance (best.back().begin(), std::max_element (best.back().begin(), best.back().end()));
        for (int k = length - 1; k >= 0; --k)
        {
            choice[(size_t) (p + k)] = t;
            t = from[(size_t) k][(size_t) t];
        }

        p = q;
    }

    // Why: what the chosen take does better than the next best one in that phrase.
    const auto reasonFor = [&] (int p, int t) -> juce::String
    {
        const auto& row = judged[(size_t) p];
        const auto& chosen = row[(size_t) t];
        const Judged* rival = nullptr;
        for (int u = 0; u < numTakes; ++u)
            if (u != t && row[(size_t) u].eligible && (rival == nullptr || row[(size_t) u].score > rival->score))
                rival = &row[(size_t) u];

        if (rival == nullptr)
            return "the only take with this part";

        const auto pitchGain = chosen.pitchWeight * 60.0f * (rival->pitchCents - chosen.pitchCents) / 50.0f;
        const auto lineGain = 30.0f * (rival->missing - chosen.missing);
        const auto clipGain = 10.0f * (juce::jmin (1.0f, rival->clippedHops / 3.0f) - juce::jmin (1.0f, chosen.clippedHops / 3.0f));
        const auto levelGain = 10.0f * (rival->levelPenalty - chosen.levelPenalty);
        const auto most = juce::jmax (pitchGain, lineGain, clipGain, levelGain);

        if (most < 2.0f)
            return "a close call: stays on this take to avoid a cut";
        if (most == lineGain)
            return "sings the whole line";
        if (most == clipGain)
            return "no clipping";
        if (most == levelGain)
            return "steadier on the mic";
        return "most in tune: " + juce::String (juce::roundToInt (chosen.pitchCents)) + juce::String::fromUTF8 ("\xc2\xa2") + " off on average (next best "
             + juce::String (juce::roundToInt (rival->pitchCents)) + juce::String::fromUTF8 ("\xc2\xa2") + ")";
    };

    for (int p = 0; p < numPhrases; ++p)
    {
        const auto t = choice[(size_t) p];
        if (t < 0)
            continue;

        const auto start = input.start + bounds[(size_t) p] * hop, end = input.start + bounds[(size_t) p + 1] * hop;
        const auto& take = input.takes[(size_t) t];

        if (! plan.picks.empty() && plan.picks.back().takeId == take.takeId && std::abs (plan.picks.back().end - start) < 1.0e-6)
        {
            auto& last = plan.picks.back();
            const auto before = last.end - last.start, added = end - start;
            last.score = (float) ((last.score * before + judged[(size_t) p][(size_t) t].score * added) / (before + added));
            last.end = end;
            continue;
        }

        plan.picks.push_back ({ start, end, take.takeId, take.name, judged[(size_t) p][(size_t) t].score, reasonFor (p, t) });
    }

    std::map<juce::String, int> uses;
    for (const auto& pick : plan.picks)
        ++uses[pick.takeName];

    juce::StringArray parts;
    for (const auto& take : input.takes)
        if (const auto it = uses.find (take.name); it != uses.end())
            parts.add (take.name + " (" + juce::String (it->second) + ")");

    plan.summary = plan.picks.empty() ? juce::String ("No take covers enough of the song to choose from.")
                 : "Judged " + juce::String (numPhrases) + " phrases; the comp uses " + parts.joinIntoString (", ") + ".";
    return plan;
}
}
