#include "Analysis.h"

#include "daw/DawEngine.h"
#include <atomic>
#include <mutex>
#include <thread>
#include "daw/Edit.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <numeric>

namespace analysis
{
namespace
{
    constexpr float silenceDb = -200.0f;

    float toDb (double power) noexcept
    {
        return power > 1.0e-20 ? (float) (10.0 * std::log10 (power)) : silenceDb;
    }

    double fromDb (float db) noexcept
    {
        return db <= silenceDb ? 0.0 : std::pow (10.0, db / 10.0);
    }

    // BS.1770 loudness of a K-weighted mean square (channels summed).
    float toLufs (double power) noexcept
    {
        return power > 1.0e-20 ? (float) (-0.691 + 10.0 * std::log10 (power)) : silenceDb;
    }

    double fromLufs (float lufs) noexcept
    {
        return lufs <= silenceDb ? 0.0 : std::pow (10.0, (lufs + 0.691) / 10.0);
    }

    // The frames to average over: `mask`, or the profile's active frames.
    const std::vector<bool>& maskOrActive (const Profile& profile, const std::vector<bool>* mask)
    {
        return mask != nullptr ? *mask : profile.active;
    }
}

//==============================================================================
double getBandCentre (int band) noexcept { return 1000.0 * std::exp2 ((band - 16) / 3.0); }
double getBandLow (int band) noexcept    { return getBandCentre (band) * std::exp2 (-1.0 / 6.0); }
double getBandHigh (int band) noexcept   { return getBandCentre (band) * std::exp2 (1.0 / 6.0); }

int findBand (double frequency) noexcept
{
    if (frequency <= 0.0)
        return 0;

    return juce::jlimit (0, numBands - 1, (int) std::lround (3.0 * std::log2 (frequency / 1000.0)) + 16);
}

const Bands& getSpeechImportance() noexcept
{
    static const Bands weights = []
    {
        Bands w {};
        float total = 0.0f;

        for (int b = 0; b < numBands; ++b)
        {
            // A triangle in log frequency peaking at 2 kHz: 150 Hz and 8 kHz reach zero.
            const auto octaves = std::log2 (getBandCentre (b) / 2000.0);
            const auto span = octaves < 0.0 ? std::log2 (2000.0 / 150.0) : 2.0;
            w[(size_t) b] = (float) juce::jmax (0.0, 1.0 - std::abs (octaves) / span);
            total += w[(size_t) b];
        }

        for (auto& v : w)
            v /= total;

        return w;
    }();

    return weights;
}

//==============================================================================
double Profile::getActiveSeconds() const
{
    return (double) std::count (active.begin(), active.end(), true) * hopSeconds;
}

double Profile::getPitchedFraction() const
{
    int activeFrames = 0, pitched = 0;
    for (size_t i = 0; i < active.size() && i < pitchHz.size(); ++i)
        if (active[i])
        {
            ++activeFrames;
            pitched += pitchHz[i] > 0.0f ? 1 : 0;
        }

    return activeFrames > 0 ? (double) pitched / activeFrames : 0.0;
}

float Profile::getPitchPercentile (double fraction) const
{
    std::vector<float> values;
    for (size_t i = 0; i < active.size() && i < pitchHz.size(); ++i)
        if (active[i] && pitchHz[i] > 0.0f)
            values.push_back (pitchHz[i]);

    if (values.empty())
        return 0.0f;

    const auto nth = values.begin() + (long) std::lround (juce::jlimit (0.0, 1.0, fraction) * (double) (values.size() - 1));
    std::nth_element (values.begin(), nth, values.end());
    return *nth;
}

Bands Profile::getAverageSpectrum (const std::vector<bool>* mask) const
{
    const auto& use = maskOrActive (*this, mask);
    std::array<double, numBands> sum {};
    int count = 0;

    for (size_t i = 0; i < bandsDb.size() && i < use.size(); ++i)
    {
        if (! use[i])
            continue;

        for (size_t b = 0; b < (size_t) numBands; ++b)
            sum[b] += fromDb (bandsDb[i][b]);

        ++count;
    }

    Bands result;
    for (size_t b = 0; b < (size_t) numBands; ++b)
        result[b] = count > 0 ? toDb (sum[b] / count) : silenceDb;

    return result;
}

float Profile::getLoudnessOver (const std::vector<bool>* mask) const
{
    const auto& use = maskOrActive (*this, mask);
    double sum = 0.0;
    int count = 0;

    for (size_t i = 0; i < momentaryLufs.size() && i < use.size(); ++i)
        if (use[i])
        {
            sum += fromLufs (momentaryLufs[i]);
            ++count;
        }

    return count > 0 ? toLufs (sum / count) : silenceDb;
}

Chroma Profile::getChromaSum (const std::vector<bool>* mask) const
{
    return sumChroma (chroma, &maskOrActive (*this, mask));
}

//==============================================================================
// K-weighting as in BS.1770 (a high shelf and a high-pass), designed for any sample
// rate the way libebur128 does it, which reproduces the standard's 48 kHz coefficients.
void Analyser::KWeighting::setup (double sampleRate)
{
    {
        const auto f0 = 1681.974450955533, gain = 3.999843853973347, q = 0.7071752369554196;
        const auto k = std::tan (juce::MathConstants<double>::pi * f0 / sampleRate);
        const auto vh = std::pow (10.0, gain / 20.0);
        const auto vb = std::pow (vh, 0.4996667741545416);
        const auto a0 = 1.0 + k / q + k * k;
        shelf = { (vh + vb * k / q + k * k) / a0, 2.0 * (k * k - vh) / a0, (vh - vb * k / q + k * k) / a0,
                  2.0 * (k * k - 1.0) / a0, (1.0 - k / q + k * k) / a0 };
    }
    {
        const auto f0 = 38.13547087602444, q = 0.5003270373238773;
        const auto k = std::tan (juce::MathConstants<double>::pi * f0 / sampleRate);
        const auto a0 = 1.0 + k / q + k * k;
        highPass = { 1.0, -2.0, 1.0, 2.0 * (k * k - 1.0) / a0, (1.0 - k / q + k * k) / a0 };
    }

    state = {};
}

double Analyser::KWeighting::process (int channel, double x) noexcept
{
    auto& s = state[(size_t) channel];

    const auto y1 = shelf[0] * x + s[0];
    s[0] = shelf[1] * x - shelf[3] * y1 + s[1];
    s[1] = shelf[2] * x - shelf[4] * y1;

    const auto y2 = highPass[0] * y1 + s[2];
    s[2] = highPass[1] * y1 - highPass[3] * y2 + s[3];
    s[3] = highPass[2] * y1 - highPass[4] * y2;
    return y2;
}

double Analyser::getKWeightingDb (double frequency, double sampleRate)
{
    KWeighting k;
    k.setup (sampleRate);

    const auto response = [frequency, sampleRate] (const std::array<double, 5>& c)
    {
        const auto w = juce::MathConstants<double>::twoPi * frequency / sampleRate;
        const std::complex<double> z1 = std::polar (1.0, -w), z2 = z1 * z1;
        return std::norm ((c[0] + c[1] * z1 + c[2] * z2) / (1.0 + c[3] * z1 + c[4] * z2));
    };

    return 10.0 * std::log10 (response (k.shelf) * response (k.highPass));
}

Analyser::Analyser (double sampleRate)
    : hopSize (juce::jmax (1, (int) std::lround (sampleRate * Profile::hopSeconds)))
{
    profile.sampleRate = sampleRate;
    kWeighting.setup (sampleRate);
    pitch.prepare (sampleRate);

    history.assign ((size_t) fftSize, 0.0f);
    fftData.assign ((size_t) fftSize * 2, 0.0f);
    window.resize ((size_t) fftSize);

    for (int i = 0; i < fftSize; ++i)
    {
        window[(size_t) i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) fftSize);
        windowPower += juce::square ((double) window[(size_t) i]);
    }
}

void Analyser::push (const float* left, const float* right, int numSamples)
{
    stereo = stereo || right != nullptr;

    for (int i = 0; i < numSamples; ++i)
    {
        const auto l = left[i];
        const auto r = right != nullptr ? right[i] : l;

        hopWeighted[0] += juce::square (kWeighting.process (0, l));
        if (right != nullptr)
            hopWeighted[1] += juce::square (kWeighting.process (1, r));

        hopSquares += right != nullptr ? 0.5 * ((double) l * l + (double) r * r) : (double) l * l;
        peak = juce::jmax (peak, std::abs (l), std::abs (r));

        const auto mono = 0.5f * (l + r);
        pitch.push (&mono, 1);
        history[(size_t) historyPos] = mono;
        historyPos = (historyPos + 1) % fftSize;

        if (++hopFill == hopSize)
            endHop();
    }
}

void Analyser::endHop()
{
    const auto n = (double) hopSize;
    hopPowers.push_back ((hopWeighted[0] + hopWeighted[1]) / n);
    profile.rmsDb.push_back (toDb (hopSquares / n));
    profile.pitchHz.push_back (pitch.getFrequency());
    hopWeighted[0] = hopWeighted[1] = hopSquares = 0.0;
    hopFill = 0;

    // Momentary loudness: the last four hops (400 ms).
    const auto last = hopPowers.size();
    const auto first = last >= 4 ? last - 4 : (size_t) 0;
    profile.momentaryLufs.push_back (toLufs (std::accumulate (hopPowers.begin() + (long) first, hopPowers.end(), 0.0) / 4.0));

    // Spectrum of the last fftSize samples.
    for (int i = 0; i < fftSize; ++i)
        fftData[(size_t) i] = history[(size_t) ((historyPos + i) % fftSize)] * window[(size_t) i];
    std::fill (fftData.begin() + fftSize, fftData.end(), 0.0f);
    fft.performFrequencyOnlyForwardTransform (fftData.data(), true);

    // One-sided power per bin, scaled so the bins sum to the mean square of the signal.
    const auto binWidth = profile.sampleRate / fftSize;
    const auto scale = 2.0 / ((double) fftSize * windowPower);
    Bands bands;

    for (int b = 0; b < numBands; ++b)
    {
        const auto low = getBandLow (b), high = getBandHigh (b);
        const auto firstBin = juce::jmax (1, (int) std::floor (low / binWidth - 0.5));
        const auto lastBin = juce::jmin (fftSize / 2 - 1, (int) std::ceil (high / binWidth + 0.5));
        double power = 0.0;

        for (int k = firstBin; k <= lastBin; ++k)
        {
            // Each bin counts for the part of its width that lies inside the band.
            const auto overlap = juce::jmin (high, (k + 0.5) * binWidth) - juce::jmax (low, (k - 0.5) * binWidth);
            if (overlap > 0.0)
                power += juce::square ((double) fftData[(size_t) k]) * scale * overlap / binWidth;
        }

        bands[(size_t) b] = toDb (power);
    }

    profile.bandsDb.push_back (bands);

    Chroma chroma {};
    computeChroma (fftData.data(), fftSize, profile.sampleRate, chroma);
    profile.chroma.push_back (chroma);
}

Profile Analyser::finish()
{
    // A final part-hop counts if it's at least half a hop, scaled up to a full one.
    if (hopFill > hopSize / 2)
    {
        const auto factor = (double) hopSize / (double) hopFill;
        hopWeighted[0] *= factor;
        hopWeighted[1] *= factor;
        hopSquares *= factor;
        endHop();
    }

    auto result = profile;
    result.peakDb = peak > 0.0f ? juce::Decibels::gainToDecibels (peak, silenceDb) : silenceDb;

    // Integrated loudness: 400 ms blocks every 100 ms, gated at -70 LUFS and then at
    // 10 LU below the loudness of the blocks that passed (BS.1770-4).
    {
        std::vector<double> blocks;
        for (size_t i = 3; i < hopPowers.size(); ++i)
            blocks.push_back ((hopPowers[i] + hopPowers[i - 1] + hopPowers[i - 2] + hopPowers[i - 3]) / 4.0);

        const auto meanAbove = [&blocks] (double gate)
        {
            double sum = 0.0;
            int count = 0;
            for (auto z : blocks)
                if (z > gate) { sum += z; ++count; }
            return count > 0 ? sum / count : 0.0;
        };

        const auto absolute = fromLufs (-70.0f);
        const auto ungated = meanAbove (absolute);

        if (ungated > 0.0)
            result.integratedLufs = toLufs (meanAbove (juce::jmax (absolute, ungated * std::pow (10.0, -1.0))));
    }

    // Loudest 3 s.
    {
        const auto window3s = (size_t) 30;
        const auto count = juce::jmin (window3s, hopPowers.size());
        double sum = 0.0, loudest = 0.0;

        for (size_t i = 0; i < hopPowers.size(); ++i)
        {
            sum += hopPowers[i];
            if (i >= window3s)
                sum -= hopPowers[i - window3s];
            if (i + 1 >= count)
                loudest = juce::jmax (loudest, sum / (double) count);
        }

        result.shortTermMaxLufs = toLufs (loudest);
    }

    // Active: within 30 dB of the signal's loud parts (its 95th percentile hop) and
    // above -60 dBFS, so silence, room noise and bleed between phrases don't count.
    {
        auto sorted = result.rmsDb;
        float threshold = -60.0f;

        if (! sorted.empty())
        {
            const auto nth = sorted.begin() + (long) ((sorted.size() - 1) * 95 / 100);
            std::nth_element (sorted.begin(), nth, sorted.end());
            threshold = juce::jmax (threshold, *nth - 30.0f);
        }

        result.active.resize (result.rmsDb.size());
        for (size_t i = 0; i < result.rmsDb.size(); ++i)
            result.active[i] = result.rmsDb[i] > threshold;
    }

    return result;
}

Profile analyse (const juce::AudioBuffer<float>& buffer, double sampleRate)
{
    Analyser analyser (sampleRate);
    analyser.push (buffer.getReadPointer (0), buffer.getNumChannels() > 1 ? buffer.getReadPointer (1) : nullptr,
                   buffer.getNumSamples());
    return analyser.finish();
}

Profile analyseFile (const juce::File& file, const std::function<bool()>& shouldContinue)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));

    if (reader == nullptr || reader->sampleRate <= 0.0)
        return {};

    Analyser analyser (reader->sampleRate);
    constexpr int blockSize = 1 << 15;
    const auto channels = (int) juce::jmin (2u, reader->numChannels);
    juce::AudioBuffer<float> buffer (channels, blockSize);

    for (juce::int64 start = 0; start < reader->lengthInSamples; start += blockSize)
    {
        const auto count = (int) juce::jmin ((juce::int64) blockSize, reader->lengthInSamples - start);
        reader->read (&buffer, 0, count, start, true, channels > 1);
        analyser.push (buffer.getReadPointer (0), channels > 1 ? buffer.getReadPointer (1) : nullptr, count);

        if (shouldContinue && ! shouldContinue())
            return {};
    }

    return analyser.finish();
}

//==============================================================================
Masking measureMasking (const Profile& voice, const Profile& backing)
{
    Masking result;
    const auto frames = (size_t) juce::jmin (voice.getNumFrames(), backing.getNumFrames());
    std::vector<bool> mask (frames);

    for (size_t i = 0; i < frames; ++i)
        mask[i] = voice.active[i];

    result.overlapSeconds = (double) std::count (mask.begin(), mask.end(), true) * Profile::hopSeconds;
    if (result.overlapSeconds <= 0.0)
        return result;

    const auto voiceSpectrum = voice.getAverageSpectrum (&mask);
    const auto backingSpectrum = backing.getAverageSpectrum (&mask);
    const auto& importance = getSpeechImportance();
    float worst = -1000.0f;

    // Bands far below the voice's overall level don't matter either way: both sides
    // are floored 40 dB under the voice, so near-empty bands read as no masking.
    double voiceTotal = 0.0;
    for (auto db : voiceSpectrum)
        voiceTotal += fromDb (db);
    const auto floorDb = toDb (voiceTotal) - 40.0f;

    for (size_t b = 0; b < (size_t) numBands; ++b)
    {
        const auto difference = juce::jmax (backingSpectrum[b], floorDb) - juce::jmax (voiceSpectrum[b], floorDb);
        result.backingOverVoiceDb[b] = difference;
        result.importanceWeightedDb += importance[b] * difference;

        if (importance[b] >= 0.03f && difference > worst)
        {
            worst = difference;
            result.worstBand = (int) b;
        }
    }

    result.voiceOverBackingLu = voice.getLoudnessOver (&mask) - backing.getLoudnessOver (&mask);
    return result;
}

//==============================================================================
SessionAnalyser::SessionAnalyser (daw::Edit& edit, Point p, bool mix, double rate, bool tracksToo)
    : snapshot (edit.getState().createCopy()), point (p), includeMix (mix), includeTracks (tracksToo),
      sampleRate (rate > 0.0 ? rate : 48000.0)
{
}

Profile SessionAnalyser::render (const juce::ValueTree& session, const Progress& progress, double from, double to,
                                 bool& cancelled)
{
    constexpr int blockSize = 1024;

    daw::Edit edit;
    edit.replaceState (session);

    daw::DawEngine engine (edit, false);
    engine.prepare (sampleRate, blockSize);
    engine.loadAllNow();

    // Every render covers the whole song (the end of its last clip), so all the
    // profiles share one time grid.
    const auto total = (juce::int64) std::llround (songSeconds * sampleRate);
    engine.seek (0);
    engine.play();

    Analyser analyser (sampleRate);
    juce::AudioBuffer<float> buffer (2, blockSize);

    for (juce::int64 done = 0; done < total; done += blockSize)
    {
        const auto numSamples = (int) juce::jmin ((juce::int64) blockSize, total - done);
        engine.process (buffer, 2, numSamples);
        analyser.push (buffer.getReadPointer (0), buffer.getReadPointer (1), numSamples);

        if (progress && ! progress (from + (to - from) * (double) (done + numSamples) / (double) juce::jmax ((juce::int64) 1, total)))
        {
            cancelled = true;
            break;
        }
    }

    return analyser.finish();
}

SessionAnalyser::Result SessionAnalyser::run (Progress progress)
{
    Result result;

    // The song's length: the end of the last clip.
    songSeconds = 0.0;
    for (const auto& track : snapshot)
        for (const auto& clip : track)
            if (clip.hasType (daw::ids::CLIP))
                songSeconds = juce::jmax (songSeconds, (double) clip[daw::ids::start] + (double) clip[daw::ids::length]);

    juce::Array<juce::ValueTree> tracks;
    for (const auto& child : snapshot)
        if (child.hasType (daw::ids::TRACK) && includeTracks)
            tracks.add (child);

    // One render per track (and one for the mix), each on its own offline engine, as
    // fast as the computer goes: they run side by side on the CPU's cores.
    std::vector<juce::ValueTree> sessions;
    for (int t = 0; t < tracks.size(); ++t)
    {
        // This track alone (so other tracks' inserts don't add latency compensation),
        // through a bare master.
        auto session = snapshot.createCopy();
        const auto id = (int) tracks[t][daw::ids::id];

        for (int c = session.getNumChildren(); --c >= 0;)
        {
            auto child = session.getChild (c);

            if (child.hasType (daw::ids::TRACK) && (int) child[daw::ids::id] != id)
            {
                session.removeChild (c, nullptr);
            }
            else if (child.hasType (daw::ids::TRACK))
            {
                child.setProperty (daw::ids::mute, false, nullptr);
                child.setProperty (daw::ids::solo, false, nullptr);

                if (point == Point::source)
                {
                    child.setProperty (daw::ids::volume, 0.0f, nullptr);
                    child.setProperty (daw::ids::pan, 0.0f, nullptr);
                    child.removeProperty (daw::ids::ride, nullptr);
                    daw::Edit::getInserts (child).removeAllChildren (nullptr);
                }
            }
            else if (child.hasType (daw::ids::MASTER))
            {
                child.setProperty (daw::ids::volume, 0.0f, nullptr);
                daw::Edit::getInserts (child).removeAllChildren (nullptr);
            }
        }

        sessions.push_back (session);
    }
    if (includeMix)
        sessions.push_back (snapshot.createCopy());

    const auto jobs = sessions.size();
    std::vector<Profile> profiles (jobs);
    std::vector<std::atomic<double>> done (jobs);
    for (auto& d : done) d = 0.0;
    std::atomic<bool> cancelled { false };
    std::atomic<size_t> next { 0 };
    std::mutex reporting;

    const auto report = [&] (size_t job, double fraction)
    {
        done[job] = fraction;
        if (! progress)
            return ! cancelled.load();

        const std::scoped_lock lock (reporting); // the caller's callback, one thread at a time
        double sum = 0.0;
        for (const auto& d : done) sum += d.load();
        if (! progress (sum / (double) juce::jmax ((size_t) 1, jobs)))
            cancelled = true;
        return ! cancelled.load();
    };

    const auto work = [&]
    {
        for (size_t job; (job = next++) < jobs && ! cancelled.load();)
        {
            bool stopped = false;
            profiles[job] = render (sessions[job], [&report, job] (double p) { return report (job, p); }, 0.0, 1.0, stopped);
            if (stopped)
                cancelled = true;
        }
    };

    const auto threads = (size_t) juce::jlimit (1, maxThreads, juce::jmin ((int) jobs, (int) std::thread::hardware_concurrency() - 1));
    std::vector<std::thread> pool;
    for (size_t i = 1; i < threads; ++i)
        pool.emplace_back (work);
    work();
    for (auto& thread : pool)
        thread.join();

    if (cancelled.load())
    {
        result.cancelled = true;
        return result;
    }

    for (int t = 0; t < tracks.size(); ++t)
        result.tracks.emplace_back ((int) tracks[t][daw::ids::id], std::move (profiles[(size_t) t]));
    if (includeMix)
        result.mix = std::move (profiles.back());

    return result;
}
}
