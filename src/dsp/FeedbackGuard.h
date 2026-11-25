#pragma once

#include "Biquad.h"
#include "FxModule.h"

#include <juce_dsp/juce_dsp.h>

#include <array>
#include <atomic>
#include <cmath>
#include <vector>

// Feedback protection for Live mode: finds the ring of a mic picking up its own speaker
// and puts a narrow notch on it before it turns into a howl.
//
// What counts as feedback: a narrow peak (well above the bins around it) that holds the
// same frequency for more than about 150 ms while it keeps getting louder, and that isn't
// part of a sung note. A voice moves (vibrato, drift) and comes with its harmonics; a
// ring is one steady sine that grows. Peaks that sit on a harmonic series need longer and
// a bigger rise before they count, so a held note isn't notched.
//
// Up to 8 notches (Q 30). A new one starts at -6 dB and deepens 3 dB at a time, up to the
// maximum cut, while its frequency still grows (it reads the signal after the notches, so
// a notch that's deep enough stops it). Notches found while singing are let go after the
// room has been quiet for the release time. "Ring out" is a setup mode for before the show:
// no one sings, the level goes up slowly, and every ring it finds becomes a fixed notch
// that stays (with the session) until it's cleared.
namespace fx
{
class FeedbackGuard
{
public:
    static constexpr int maxNotches = 8;
    static constexpr int maxFixed = 6;          // two slots always stay free for the show
    static constexpr double notchQ = 30.0;
    static constexpr float firstCutDb = 6.0f, stepDb = 3.0f;
    static constexpr float quietLevel = 0.00316f; // -50 dBFS: below this the room counts as quiet

    struct Settings
    {
        float sensitivity = 0.5f;     // 0..1
        float maxCutDb = 12.0f;
        float releaseSeconds = 10.0f; // of quiet, before notches found while singing let go
        bool ringOut = false;
    };

    struct Notch
    {
        float hz = 0.0f, cutDb = 0.0f;
        bool fixed = false;
    };

    void setSettings (const Settings& s) noexcept { settings = s; }

    void prepare (double newSampleRate)
    {
        sampleRate = newSampleRate;
        const auto order = sampleRate <= 50000.0 ? 12 : sampleRate <= 100000.0 ? 13 : 14;
        fft = std::make_unique<juce::dsp::FFT> (order);
        fftSize = 1 << order;
        hop = fftSize / 8;
        ring.assign ((size_t) fftSize, 0.0f);
        frame.assign ((size_t) fftSize * 2, 0.0f);
        levels.assign ((size_t) fftSize / 2 + 1, -200.0f);
        window.resize ((size_t) fftSize);
        double sum = 0.0;
        for (int i = 0; i < fftSize; ++i)
        {
            window[(size_t) i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) fftSize);
            sum += window[(size_t) i];
        }
        normDb = 20.0f * std::log10 ((float) sum * 0.5f); // a full-scale sine reads 0 dB
        binHz = (float) (sampleRate / fftSize);
        hopSeconds = (float) hop / (float) sampleRate;
        reset();
    }

    // Forgets what it was following and the notches found while singing; fixed notches stay.
    void reset() noexcept
    {
        std::fill (ring.begin(), ring.end(), 0.0f);
        ringPos = 0;
        sinceHop = 0;
        candidates = {};
        quietSeconds = 0.0f;
        for (auto& slot : slots)
            if (! slot.fixed)
                slot = {};
        for (auto& slot : slots)
        {
            slot.current = slot.target;
            slot.filter.reset();
            slot.dirty = true;
        }
        publish();
    }

    // Message thread: the fixed notches (from the session) and clearing them. They take
    // effect on the next block.
    void setFixedNotches (const std::vector<Notch>& notches)
    {
        const juce::SpinLock::ScopedLockType lock (commandLock);
        pendingFixed = notches;
        if ((int) pendingFixed.size() > maxFixed)
            pendingFixed.resize ((size_t) maxFixed);
        fixedPending = true;
    }

    void clearNotches (bool alsoFixed)
    {
        const juce::SpinLock::ScopedLockType lock (commandLock);
        clearPending = true;
        clearFixed = clearFixed || alsoFixed;
        if (alsoFixed)
        {
            pendingFixed.clear();
            fixedPending = false;
        }
    }

    // Any thread: the notches in use right now (the fixed ones waiting to be applied
    // count as in use, so a save before the next block keeps them).
    std::vector<Notch> getNotches() const
    {
        {
            const juce::SpinLock::ScopedLockType lock (commandLock);
            if (fixedPending)
                return pendingFixed;
        }
        std::vector<Notch> result;
        for (const auto& slot : published)
        {
            const auto kind = slot.kind.load (std::memory_order_relaxed);
            if (kind != 0)
                result.push_back ({ slot.hz.load (std::memory_order_relaxed), slot.cut.load (std::memory_order_relaxed), kind == 2 });
        }
        return result;
    }

    std::vector<Notch> getFixedNotches() const
    {
        std::vector<Notch> fixed;
        for (const auto& notch : getNotches())
            if (notch.fixed)
                fixed.push_back (notch);
        return fixed;
    }

    // Rings caught since the last call (each new notch or deeper cut counts one). Any thread.
    int takeCatches() noexcept { return catches.exchange (0, std::memory_order_relaxed); }

    //==============================================================================
    // Audio thread. Notches every channel, then listens to the result (mono).
    void process (juce::dsp::AudioBlock<float>& block) noexcept
    {
        applyCommands();

        const auto channels = juce::jmin ((size_t) 2, block.getNumChannels());
        const auto numSamples = (int) block.getNumSamples();
        if (channels == 0 || fft == nullptr)
            return;

        constexpr int slice = 32;
        for (int start = 0; start < numSamples; start += slice)
        {
            const auto length = juce::jmin (slice, numSamples - start);
            glideNotches (length);

            float peakIn = 0.0f;
            for (size_t ch = 0; ch < channels; ++ch)
            {
                auto* data = block.getChannelPointer (ch) + start;
                for (int i = 0; i < length; ++i)
                {
                    peakIn = juce::jmax (peakIn, std::abs (data[i]));
                    auto x = data[i];
                    for (auto& slot : slots)
                        if (slot.current > 0.01f)
                            x = slot.filter.process (ch, x);
                    data[i] = x;
                }
            }

            quietSeconds = peakIn < quietLevel ? quietSeconds + (float) length / (float) sampleRate : 0.0f;

            for (int i = 0; i < length; ++i)
            {
                float mono = 0.0f;
                for (size_t ch = 0; ch < channels; ++ch)
                    mono += block.getChannelPointer (ch)[start + i];
                ring[(size_t) ringPos] = mono / (float) channels;
                ringPos = (ringPos + 1) % fftSize;
                if (++sinceHop >= hop)
                {
                    sinceHop = 0;
                    analyse();
                }
            }
        }

        publish();
    }

private:
    struct Peak
    {
        float hz = 0.0f, levelDb = -200.0f, aboveDb = 0.0f;
    };

    struct Candidate
    {
        bool used = false;
        float hz = 0.0f, lowHz = 0.0f, highHz = 0.0f;
        float levelDb = 0.0f, aboveDb = 0.0f;
        int frames = 0, missed = 0, harmonicFrames = 0;
        std::array<float, 128> history {}; // level per frame, by frame count

        void push (float db) noexcept { history[(size_t) (frames % (int) history.size())] = db; }
        // The level `back` frames before the latest (clamped to the first one followed).
        float levelAgo (int back) const noexcept
        {
            back = juce::jlimit (0, juce::jmin (frames - 1, (int) history.size() - 1), back);
            return history[(size_t) ((frames - 1 - back) % (int) history.size())];
        }
    };

    struct Slot
    {
        bool used = false, fixed = false, releasing = false, dirty = true;
        float hz = 0.0f, target = 0.0f, current = 0.0f;
        int age = 0; // analysis frames since it last caught a ring
        Biquad filter;
    };

    struct Published
    {
        std::atomic<int> kind { 0 }; // 0 free, 1 found while singing, 2 fixed
        std::atomic<float> hz { 0.0f }, cut { 0.0f };
    };

    void applyCommands() noexcept
    {
        const juce::SpinLock::ScopedTryLockType lock (commandLock);
        if (! lock.isLocked())
            return;

        if (clearPending)
        {
            for (auto& slot : slots)
                if (! slot.fixed || clearFixed)
                {
                    slot.releasing = slot.used;
                    slot.target = 0.0f;
                    slot.fixed = false;
                }
            candidates = {};
            clearPending = clearFixed = false;
        }

        if (fixedPending)
        {
            for (auto& slot : slots)
                if (slot.fixed)
                    slot = {};
            for (const auto& notch : pendingFixed)
                if (notch.hz > 20.0f && notch.hz < sampleRate * 0.45)
                    if (auto* slot = freeSlot (true))
                    {
                        *slot = {};
                        slot->used = slot->fixed = true;
                        slot->hz = notch.hz;
                        slot->target = juce::jlimit (0.0f, 24.0f, notch.cutDb);
                    }
            fixedPending = false;
        }
    }

    void glideNotches (int length) noexcept
    {
        const auto seconds = (float) length / (float) sampleRate;
        for (auto& slot : slots)
        {
            if (! slot.used && slot.current <= 0.0f)
                continue;

            const auto target = slot.used ? juce::jmin (slot.target, slot.fixed ? 24.0f : settings.maxCutDb) : 0.0f;
            const auto rate = target > slot.current ? 150.0f : (slot.releasing ? 4.0f : 30.0f); // dB per second
            const auto step = rate * seconds;
            const auto before = slot.current;
            slot.current = target > slot.current ? juce::jmin (target, slot.current + step) : juce::jmax (target, slot.current - step);

            if (slot.dirty || std::abs (slot.current - before) > 0.0f)
            {
                slot.filter.setup (Biquad::Type::peak, sampleRate, slot.hz, notchQ, -slot.current);
                slot.dirty = false;
            }

            if (slot.releasing && slot.current <= 0.0f)
            {
                slot = {};
                slot.filter.reset();
            }
        }
    }

    Slot* freeSlot (bool forFixed) noexcept
    {
        int fixedCount = 0;
        for (const auto& slot : slots)
            fixedCount += slot.used && slot.fixed ? 1 : 0;
        if (forFixed && fixedCount >= maxFixed)
            return nullptr;

        for (auto& slot : slots)
            if (! slot.used && slot.current <= 0.0f)
                return &slot;
        for (auto& slot : slots)
            if (! slot.used)
                return &slot; // still fading out
        // Full: take the place of the one found while singing that caught nothing longest.
        Slot* oldest = nullptr;
        for (auto& slot : slots)
            if (! slot.fixed && (oldest == nullptr || slot.age > oldest->age))
                oldest = &slot;
        return oldest;
    }

    //==============================================================================
    void analyse() noexcept
    {
        for (int i = 0; i < fftSize; ++i)
            frame[(size_t) i] = ring[(size_t) ((ringPos + i) % fftSize)] * window[(size_t) i];
        std::fill (frame.begin() + fftSize, frame.end(), 0.0f);
        fft->performFrequencyOnlyForwardTransform (frame.data(), true);

        const auto bins = fftSize / 2;
        for (int k = 0; k <= bins; ++k)
            levels[(size_t) k] = 20.0f * std::log10 (frame[(size_t) k] + 1.0e-9f) - normDb;

        findPeaks();
        follow();

        for (auto& slot : slots)
            if (slot.used)
                ++slot.age;

        // Notches found while singing let go once the room has been quiet long enough.
        if (quietSeconds >= settings.releaseSeconds && ! settings.ringOut)
            for (auto& slot : slots)
                if (slot.used && ! slot.fixed && ! slot.releasing)
                {
                    slot.releasing = true;
                    slot.used = false;
                }
    }

    float neighbourDb (int k) const noexcept
    {
        double power = 0.0;
        int count = 0;
        const auto last = (int) levels.size() - 1;
        for (int d = 4; d <= 16; ++d)
            for (const auto j : { k - d, k + d })
                if (j >= 1 && j < last)
                {
                    power += std::pow (10.0, levels[(size_t) j] / 10.0);
                    ++count;
                }
        return count > 0 ? (float) (10.0 * std::log10 (power / count + 1.0e-20)) : -200.0f;
    }

    void findPeaks() noexcept
    {
        numPeaks = 0;
        const auto kLow = juce::jmax (2, (int) std::ceil (80.0f / binHz));
        const auto kHigh = juce::jmin ((int) levels.size() - 18, (int) (juce::jmin (16000.0, sampleRate * 0.45) / binHz));

        for (int k = kLow; k <= kHigh; ++k)
        {
            const auto b = levels[(size_t) k];
            if (b < -70.0f || b <= levels[(size_t) k - 1] || b < levels[(size_t) k + 1])
                continue;

            const auto above = b - neighbourDb (k);
            if (above < 8.0f)
                continue;

            const auto a = levels[(size_t) k - 1], c = levels[(size_t) k + 1];
            const auto denominator = a - 2.0f * b + c;
            const auto p = std::abs (denominator) > 1.0e-6f ? juce::jlimit (-0.5f, 0.5f, 0.5f * (a - c) / denominator) : 0.0f;
            const Peak peak { ((float) k + p) * binHz, b - 0.25f * (a - c) * p, above };

            // Keep the strongest (by how far they stand out).
            if (numPeaks < (int) peaks.size())
                peaks[(size_t) numPeaks++] = peak;
            else
            {
                auto weakest = 0;
                for (int i = 1; i < numPeaks; ++i)
                    if (peaks[(size_t) i].aboveDb < peaks[(size_t) weakest].aboveDb)
                        weakest = i;
                if (peak.aboveDb > peaks[(size_t) weakest].aboveDb)
                    peaks[(size_t) weakest] = peak;
            }
        }
    }

    // Whether this peak sits on a harmonic series with other peaks: part of a sung (or
    // played) note rather than a lone ring. Only for a steady note (one with vibrato already
    // fails the steadiness test), so the harmonics line up closely; a ring that stands far
    // above the harmonics around it isn't part of them.
    bool isHarmonic (float hz, float levelDb) const noexcept
    {
        for (int n = 1; n <= 24; ++n)
        {
            const auto f0 = hz / (float) n;
            if (f0 < 70.0f)
                break;
            // A voice has its neighbouring harmonics; a ring that clips makes odd ones only
            // (3f, 5f), which this doesn't count.
            int partners = 0;
            bool neighbour = false;
            for (int i = 0; i < numPeaks; ++i)
            {
                const auto& other = peaks[(size_t) i];
                const auto m = (int) std::round (other.hz / f0);
                if (m < 1 || m > 40 || m == n)
                    continue;
                if (std::abs (other.hz - (float) m * f0) <= juce::jmax (binHz * 0.5f, other.hz * 0.004f))
                {
                    ++partners;
                    neighbour = neighbour || (std::abs (m - n) == 1 && other.levelDb >= levelDb - 24.0f);
                }
            }
            if (partners >= (n > 6 ? 3 : 2) && neighbour)
                return true;
        }
        return false;
    }

    bool isOvertoneOfNotch (float hz) const noexcept
    {
        for (const auto& slot : slots)
            if (slot.used && slot.hz > 0.0f)
                for (int m = 2; m <= 8; ++m)
                    if (std::abs (hz - (float) m * slot.hz) <= juce::jmax (binHz, hz * 0.004f))
                        return true;
        return false;
    }

    void follow() noexcept
    {
        const auto ringOut = settings.ringOut;
        const auto sensitivity = juce::jlimit (0.0f, 1.0f, settings.sensitivity);
        const auto standOut = juce::jmap (sensitivity, 26.0f, 12.0f) - (ringOut ? 4.0f : 0.0f);
        const auto holdFrames = (int) std::ceil (juce::jmap (sensitivity, 0.25f, 0.12f) * (ringOut ? 0.7f : 1.0f) / hopSeconds);
        const auto riseDb = ringOut ? 1.5f : 3.0f;

        for (auto& candidate : candidates)
            candidate.missed += candidate.used ? 1 : 0;

        for (int i = 0; i < numPeaks; ++i)
        {
            const auto& peak = peaks[(size_t) i];
            if (peak.aboveDb < standOut || peak.levelDb < -60.0f)
                continue;

            Candidate* match = nullptr;
            for (auto& candidate : candidates)
                if (candidate.used && std::abs (candidate.hz - peak.hz) <= binHz)
                    match = &candidate;

            if (match == nullptr)
            {
                for (auto& candidate : candidates)
                    if (! candidate.used)
                    {
                        match = &candidate;
                        break;
                    }
                if (match == nullptr)
                {
                    match = &candidates[0];
                    for (auto& candidate : candidates)
                        if (candidate.frames < match->frames)
                            match = &candidate;
                }
                *match = {};
                match->used = true;
                match->lowHz = match->highHz = peak.hz;
            }

            match->hz = peak.hz;
            match->lowHz = juce::jmin (match->lowHz, peak.hz);
            match->highHz = juce::jmax (match->highHz, peak.hz);
            match->levelDb = peak.levelDb;
            match->aboveDb = peak.aboveDb;
            match->missed = 0;
            match->push (peak.levelDb);
            ++match->frames;
            if (! ringOut && isHarmonic (peak.hz, peak.levelDb))
                ++match->harmonicFrames;
        }

        for (auto& candidate : candidates)
        {
            if (! candidate.used)
                continue;
            if (candidate.missed > 2)
            {
                candidate = {};
                continue;
            }
            if (candidate.missed > 0)
                continue;

            // A voice moves; a ring doesn't (a few cents, or a third of a bin).
            const auto steady = candidate.highHz - candidate.lowHz <= juce::jmax (binHz * 0.35f, candidate.hz * 0.0025f);
            if (! steady)
            {
                // Start again from here: a sung note that settles can still be followed.
                candidate.lowHz = candidate.highHz = candidate.hz;
                candidate.frames = 0;
                candidate.harmonicFrames = 0;
                candidate.push (candidate.levelDb);
                candidate.frames = 1;
                continue;
            }

            const auto partOfNote = candidate.harmonicFrames * 2 > candidate.frames;
            const auto needFrames = partOfNote ? holdFrames * 3 : holdFrames;
            const auto needRise = partOfNote ? riseDb * 2.0f : riseDb;
            if (candidate.frames < needFrames)
                continue;

            // Growing all the way through, not just a note starting (the analysis window
            // filling up reads as a fast rise at the start of anything).
            const auto window = juce::jmin (needFrames, (int) candidate.history.size() - 1);
            const auto rising = candidate.levelDb - candidate.levelAgo (window) >= needRise
                                && candidate.levelDb - candidate.levelAgo (window / 2) >= needRise * 0.5f;
            // A loud steady peak counts too (a howl that already hit the ceiling), unless it's an
            // overtone of a ring that's notched: the speaker clipping it, gone once it stops.
            const auto howling = ! partOfNote && candidate.aboveDb >= standOut + 12.0f && candidate.frames >= holdFrames * 2
                                 && ! isOvertoneOfNotch (candidate.hz);
            if (! rising && ! howling)
                continue;

            catchRing (candidate.hz);

            // Wait for the notch to work before judging this frequency again.
            candidate.frames = 0;
            candidate.harmonicFrames = 0;
            candidate.lowHz = candidate.highHz = candidate.hz;
            candidate.push (candidate.levelDb);
            candidate.frames = 1;
        }
    }

    void catchRing (float hz) noexcept
    {
        // Already notched there: deeper.
        for (auto& slot : slots)
            if (slot.used && std::abs (slot.hz - hz) <= juce::jmax (binHz, slot.hz / (float) notchQ))
            {
                const auto limit = slot.fixed ? 24.0f : settings.maxCutDb;
                if (slot.target < limit)
                {
                    slot.target = juce::jmin (limit, slot.target + stepDb);
                    catches.fetch_add (1, std::memory_order_relaxed);
                }
                if (settings.ringOut)
                    slot.fixed = true;
                slot.releasing = false;
                slot.age = 0;
                return;
            }

        auto* slot = freeSlot (settings.ringOut);
        if (slot == nullptr)
            slot = freeSlot (false); // ring out with every fixed place taken: a normal notch
        if (slot == nullptr)
            return;

        const auto fixed = settings.ringOut && [this]
        {
            int count = 0;
            for (const auto& s : slots)
                count += s.used && s.fixed ? 1 : 0;
            return count < maxFixed;
        }();

        *slot = {}; // a slot still fading out (or the oldest, when all are taken) starts over
        slot->used = true;
        slot->fixed = fixed;
        slot->hz = hz;
        slot->target = juce::jmin (firstCutDb, settings.maxCutDb);
        slot->filter.reset();
        catches.fetch_add (1, std::memory_order_relaxed);
    }

    void publish() noexcept
    {
        for (size_t i = 0; i < slots.size(); ++i)
        {
            const auto& slot = slots[i];
            const auto kind = slot.used ? (slot.fixed ? 2 : 1) : 0;
            published[i].kind.store (kind, std::memory_order_relaxed);
            published[i].hz.store (slot.hz, std::memory_order_relaxed);
            published[i].cut.store (slot.used ? juce::jmax (slot.current, juce::jmin (slot.target, slot.fixed ? 24.0f : settings.maxCutDb)) : 0.0f,
                                    std::memory_order_relaxed);
        }
    }

    Settings settings;
    double sampleRate = 48000.0;
    std::unique_ptr<juce::dsp::FFT> fft;
    int fftSize = 4096, hop = 512, ringPos = 0, sinceHop = 0, numPeaks = 0;
    float normDb = 0.0f, binHz = 11.7f, hopSeconds = 0.0107f, quietSeconds = 0.0f;
    std::vector<float> ring, frame, levels, window;
    std::array<Peak, 32> peaks {};
    std::array<Candidate, 16> candidates {};
    std::array<Slot, maxNotches> slots {};
    std::array<Published, maxNotches> published {};
    std::atomic<int> catches { 0 };

    juce::SpinLock commandLock;
    std::vector<Notch> pendingFixed;
    bool fixedPending = false, clearPending = false, clearFixed = false;
};

namespace modules
{
//==============================================================================
// Live mode's feedback protection (see FeedbackGuard above). Its values belong to the
// room, not to the sound: presets, scenes and setlist songs leave them alone (see
// Parameters::belongsToRoom). Ring out is a choice: it doesn't switch the module on.
class FeedbackGuardModule final : public Module
{
public:
    FeedbackGuardModule() : Module (ModuleType::feedbackGuard) {}

    Value& on          = addSwitch ("fbgOn");
    Value& sensitivity = addValue ("fbgSensitivity", 50.0f); // %
    Value& depth       = addValue ("fbgDepth", 12.0f);       // dB, the most one notch cuts
    Value& release     = addValue ("fbgRelease", 10.0f);     // s of quiet
    Value& ringOut     = addChoice ("fbgRingOut", 0.0f);

    FeedbackGuard& getGuard() noexcept { return guard; }

    // notches: [{ hz, cut, fixed }] in use; caught: rings caught since the last reading.
    void addReadings (juce::DynamicObject& readings) override
    {
        juce::Array<juce::var> list;
        for (const auto& notch : guard.getNotches())
        {
            auto* entry = new juce::DynamicObject();
            entry->setProperty ("hz", std::round (notch.hz * 10.0f) / 10.0f);
            entry->setProperty ("cut", std::round (notch.cutDb * 10.0f) / 10.0f);
            entry->setProperty ("fixed", notch.fixed);
            list.add (juce::var (entry));
        }
        readings.setProperty ("notches", list);
        readings.setProperty ("caught", guard.takeCatches());
        readings.setProperty ("ringOut", flag (ringOut));
    }

    // A rebuilt chain keeps the fixed notches.
    void carryStateFrom (const Module& previous) override
    {
        if (auto* other = dynamic_cast<const FeedbackGuardModule*> (&previous))
            guard.setFixedNotches (other->guard.getFixedNotches());
    }

private:
    void prepareModule (const juce::dsp::ProcessSpec& spec) override { guard.prepare (spec.sampleRate); }
    void resetModule() override { guard.reset(); }

    void update() noexcept override
    {
        FeedbackGuard::Settings s;
        s.sensitivity = fraction (sensitivity);
        s.maxCutDb = get (depth);
        s.releaseSeconds = get (release);
        s.ringOut = flag (ringOut);
        guard.setSettings (s);
    }

    void render (const Context& c) noexcept override
    {
        auto block = c.getOutputBlock();
        guard.process (block);
    }

    FeedbackGuard guard;
};
} // namespace modules
} // namespace fx
