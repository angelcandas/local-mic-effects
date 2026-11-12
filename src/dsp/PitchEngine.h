#pragma once

#include <juce_dsp/juce_dsp.h>

#include "PitchDetector.h"
#include "PsolaShifter.h"
#include "../analysis/Chords.h"

#include <atomic>
#include <cmath>

namespace fx
{
// Autotune, pitch/formant shift and a harmonizer of up to four voices, sharing one
// pitch detector. A voice is mono, so the block is mixed down, processed once and
// copied back to every channel; the harmony voices are then placed across the stereo
// field.
//
// The harmony voices pick their notes in one of three ways:
// - Intervals: a fixed number of semitones from the lead (the original harmonizer).
// - Scale: the interval as steps of the autotune key's scale, so a "third" is major or
//   minor as the key needs.
// - Chords: notes of the chord playing now (Settings::chord, from the backing track,
//   an instrument or the other tracks: see analysis::ChordFollower), above or below
//   the sung note.
// "Humanize" gives each voice its own small delay and pitch drift, so they sound like
// separate singers rather than copies of the lead.
class PitchEngine
{
public:
    struct Settings
    {
        bool  tuneOn = false;
        int   key = 0;            // 0 = C ... 11 = B
        int   scale = 0;          // index into getScaleNames()
        float retuneMs = 20.0f;   // 0 = instant ("hard tune")
        float amount = 1.0f;      // 0..1 of the correction applied

        bool  shiftOn = false;
        float semitones = 0.0f;
        float formantSemitones = 0.0f;

        bool  harmonyOn = false;
        float interval1 = 4.0f, interval2 = 7.0f; // semitones relative to the lead
        float interval3 = -5.0f, interval4 = 12.0f;
        float harmonyMix = 0.5f;
        int   harmonyMode = 0;    // getHarmonyModeNames()
        int   harmonyVoices = 2;  // 1..maxVoices
        int   harmonyStyle = 0;   // getHarmonyStyleNames() (Chords mode)
        float humanize = 0.35f;   // 0..1
        float spread = 0.5f;      // 0..1: how far apart the voices sit in stereo
        float voiceFormant = 0.0f;// semitones, the voices' formants against the lead's
        analysis::Chord chord;    // Chords mode: the chord now (set by the owner)

        bool isActive() const noexcept { return tuneOn || shiftOn || harmonyOn; }
    };

    static const juce::StringArray& getKeyNames()
    {
        static const juce::StringArray names { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
        return names;
    }

    static constexpr int maxVoices = 4;
    enum HarmonyMode { intervalMode, scaleMode, chordMode };
    enum HarmonyStyle { above, below, around, rootBelow };

    static const juce::StringArray& getHarmonyModeNames()
    {
        static const juce::StringArray names { "Intervals", "Scale", "Chords" };
        return names;
    }

    // Where the voices go in Chords mode: the next chord notes above the lead, below
    // it, alternating, or the chord's root (or bass note) under the lead with the
    // others above.
    static const juce::StringArray& getHarmonyStyleNames()
    {
        static const juce::StringArray names { "Above", "Below", "Around", "Root below" };
        return names;
    }

    static const juce::StringArray& getScaleNames()
    {
        static const juce::StringArray names { "Chromatic", "Major", "Minor", "Harmonic Minor",
                                               "Major Pentatonic", "Minor Pentatonic", "Blues" };
        return names;
    }

    void prepare (const juce::dsp::ProcessSpec& spec)
    {
        detector.prepare (spec.sampleRate);
        lead.prepare (spec.sampleRate);
        for (auto& voice : harmonies)
            voice.prepare (spec.sampleRate);
        for (auto& line : voiceDelay)
            line.assign ((size_t) juce::nextPowerOfTwo ((int) (spec.sampleRate * 0.064)), 0.0f);

        mono.assign (spec.maximumBlockSize, 0.0f);
        voiceOut.assign (spec.maximumBlockSize, 0.0f);
        sampleRate = spec.sampleRate;
        reset();
    }

    void reset()
    {
        detector.reset();
        lead.reset();
        for (auto& voice : harmonies)
            voice.reset();

        correction = 0.0f;
        targetNote = -1;
        leadWasOn = harmonyWasOn = false;
        detectedNote.store (-1.0f);
        tunedNote.store (-1);
        resetVoices();
    }

    void process (const juce::dsp::ProcessContextReplacing<float>& context, const Settings& settings) noexcept
    {
        auto& block = context.getOutputBlock();
        const auto numSamples  = (int) block.getNumSamples();
        const auto numChannels = block.getNumChannels();
        auto* voice = mono.data();

        jassert ((size_t) numSamples <= mono.size());

        juce::FloatVectorOperations::copy (voice, block.getChannelPointer (0), numSamples);
        for (size_t ch = 1; ch < numChannels; ++ch)
            juce::FloatVectorOperations::add (voice, block.getChannelPointer (ch), numSamples);
        if (numChannels > 1)
            juce::FloatVectorOperations::multiply (voice, 1.0f / (float) numChannels, numSamples);

        detector.push (voice, numSamples);
        const auto frequency = detector.getFrequency();
        const auto period = detector.getPeriodInSamples();

        updateCorrection (settings, frequency, (float) numSamples / (float) sampleRate);

        const auto shift = settings.shiftOn ? settings.semitones : 0.0f;
        const auto formantRatio = settings.shiftOn ? std::exp2 (settings.formantSemitones / 12.0f) : 1.0f;
        const auto leadRatio = std::exp2 ((correction + shift) / 12.0f);
        const auto leadOn = settings.tuneOn || settings.shiftOn;

        // Clear stale audio when a voice is switched back on.
        if (leadOn && ! leadWasOn)
            lead.reset();
        if (settings.harmonyOn != harmonyWasOn)
        {
            for (auto& harmony : harmonies)
                harmony.reset();
            resetVoices();
            activeVoices = 0;
        }

        leadWasOn = leadOn;
        harmonyWasOn = settings.harmonyOn;

        lead.setParameters (leadRatio, formantRatio, period);

        const auto numVoices = settings.harmonyOn ? juce::jlimit (1, maxVoices, settings.harmonyVoices) : 0;

        // A voice that joins starts clean (no audio left from when it last sang).
        for (int v = activeVoices; v < numVoices; ++v)
        {
            harmonies[(size_t) v].reset();
            std::fill (voiceDelay[(size_t) v].begin(), voiceDelay[(size_t) v].end(), 0.0f);
            voiceState[(size_t) v] = {};
        }
        for (int v = numVoices; v < activeVoices; ++v)
            voiceNotes[(size_t) v].store (-1);
        activeVoices = numVoices;
        const auto blockSeconds = (float) numSamples / (float) sampleRate;

        if (settings.harmonyOn)
            updateVoices (settings, frequency, shift, numVoices, blockSeconds);

        const auto voiceFormantRatio = formantRatio * std::exp2 (settings.voiceFormant / 12.0f);
        for (int v = 0; v < numVoices; ++v)
        {
            const auto cents = voiceState[(size_t) v].drift * settings.humanize;
            harmonies[(size_t) v].setParameters (leadRatio * std::exp2 (voiceState[(size_t) v].interval / 12.0f + cents / 1200.0f),
                                                 voiceFormantRatio, period);
        }

        // The lead into the block; `voice` keeps the dry input for the harmony voices.
        for (int i = 0; i < numSamples; ++i)
        {
            const auto input = voice[i];
            voiceOut[(size_t) i] = leadOn ? lead.processSample (input) : input;
        }

        for (size_t ch = 0; ch < numChannels; ++ch)
            juce::FloatVectorOperations::copy (block.getChannelPointer (ch), voiceOut.data(), numSamples);

        if (numVoices == 0)
            return;

        // Each voice: shifted, delayed by its own (drifting) amount, then panned.
        const auto harmonyGain = 0.7f * settings.harmonyMix * std::sqrt (2.0f / (float) numVoices);
        static constexpr std::array<float, maxVoices> pans { -0.8f, 0.8f, -0.4f, 0.4f };
        auto* left = block.getChannelPointer (0);
        auto* right = numChannels > 1 ? block.getChannelPointer (1) : nullptr;

        for (int v = 0; v < numVoices; ++v)
        {
            auto& shifter = harmonies[(size_t) v];
            auto& state = voiceState[(size_t) v];
            auto& line = voiceDelay[(size_t) v];
            const auto mask = (int) line.size() - 1;

            // Delay in samples: a few ms more for each voice, wandering slowly.
            const auto targetDelay = settings.humanize * (float) sampleRate * 0.001f
                                     * (6.0f + 6.0f * (float) v + 3.0f * state.wander);
            const auto delayStep = (targetDelay - state.delay) / (float) numSamples;

            for (int i = 0; i < numSamples; ++i)
            {
                line[(size_t) (state.write & mask)] = shifter.processSample (voice[i]);
                state.delay += delayStep;
                const auto readPosition = (float) state.write - state.delay;
                const auto index = (int) std::floor (readPosition);
                const auto frac = readPosition - (float) index;
                const auto a = line[(size_t) (index & mask)], b = line[(size_t) ((index + 1) & mask)];
                voiceOut[(size_t) i] = harmonyGain * (a + frac * (b - a));
                ++state.write;
            }

            // Equal-power pan; mono outputs just get the voice.
            const auto pan = numVoices == 1 ? 0.0f : pans[(size_t) v] * settings.spread;
            const auto angle = (pan + 1.0f) * juce::MathConstants<float>::pi * 0.25f;
            const auto gainLeft = right != nullptr ? std::sqrt (2.0f) * std::cos (angle) : 1.0f;
            const auto gainRight = std::sqrt (2.0f) * std::sin (angle);

            juce::FloatVectorOperations::addWithMultiply (left, voiceOut.data(), gainLeft, numSamples);
            if (right != nullptr)
                juce::FloatVectorOperations::addWithMultiply (right, voiceOut.data(), gainRight, numSamples);
            for (size_t ch = 2; ch < numChannels; ++ch)
                juce::FloatVectorOperations::add (block.getChannelPointer (ch), voiceOut.data(), numSamples);
        }
    }

    // For the UI tuner: the sung note (fractional MIDI, -1 when unvoiced) and the
    // note autotune is pulling towards (-1 when inactive).
    std::atomic<float> detectedNote { -1.0f };
    std::atomic<int> tunedNote { -1 };

    // For the UI: the note each harmony voice sings now (MIDI, -1 when silent or off)
    // and the chord the voices follow (analysis::Chord::pack, -1 for none).
    std::array<std::atomic<int>, maxVoices> voiceNotes { { { -1 }, { -1 }, { -1 }, { -1 } } };
    std::atomic<int> harmonyChord { -1 };

private:
    static constexpr int scaleMask (std::initializer_list<int> degrees)
    {
        int mask = 0;
        for (auto degree : degrees)
            mask |= 1 << degree;
        return mask;
    }

    static int getScaleMask (int scale) noexcept
    {
        static constexpr int masks[] = {
            0xfff,                                    // Chromatic
            scaleMask ({ 0, 2, 4, 5, 7, 9, 11 }),     // Major
            scaleMask ({ 0, 2, 3, 5, 7, 8, 10 }),     // Minor
            scaleMask ({ 0, 2, 3, 5, 7, 8, 11 }),     // Harmonic minor
            scaleMask ({ 0, 2, 4, 7, 9 }),            // Major pentatonic
            scaleMask ({ 0, 3, 5, 7, 10 }),           // Minor pentatonic
            scaleMask ({ 0, 3, 5, 6, 7, 10 }),        // Blues
        };

        return masks[juce::jlimit (0, (int) std::size (masks) - 1, scale)];
    }

    int nearestNoteInScale (float note, int key, int scale) const noexcept
    {
        const auto mask = getScaleMask (scale);
        const auto inScale = [&] (int n) { return ((mask >> (((n - key) % 12 + 12) % 12)) & 1) != 0; };

        auto best = -1;
        auto bestDistance = 100.0f;
        const auto base = (int) std::floor (note);

        for (auto n = base - 6; n <= base + 7; ++n)
        {
            const auto distance = std::abs (note - (float) n);

            if (inScale (n) && distance < bestDistance)
            {
                best = n;
                bestDistance = distance;
            }
        }

        // Hysteresis: stay on the current note until another is clearly closer,
        // so notes sung between two pitches don't warble back and forth.
        constexpr auto hysteresis = 0.2f;

        if (targetNote >= 0 && inScale (targetNote) && std::abs (note - (float) targetNote) < bestDistance + hysteresis)
            return targetNote;

        return best;
    }

    void updateCorrection (const Settings& settings, float frequency, float blockSeconds) noexcept
    {
        auto desired = 0.0f;

        if (settings.tuneOn && frequency > 0.0f)
        {
            const auto note = 69.0f + 12.0f * std::log2 (frequency / 440.0f);
            targetNote = nearestNoteInScale (note, settings.key, settings.scale);
            desired = ((float) targetNote - note) * settings.amount;
            detectedNote.store (note);
            tunedNote.store (targetNote);
        }
        else
        {
            targetNote = -1;
            detectedNote.store (frequency > 0.0f ? 69.0f + 12.0f * std::log2 (frequency / 440.0f) : -1.0f);
            tunedNote.store (-1);
        }

        const auto coefficient = settings.retuneMs < 0.5f ? 1.0f
                                                          : 1.0f - std::exp (-blockSeconds * 1000.0f / settings.retuneMs);
        correction += (desired - correction) * coefficient;
    }

    //==============================================================================
    struct VoiceState
    {
        float interval = 0.0f;      // semitones from the lead, gliding
        float drift = 0.0f, driftTarget = 0.0f; // cents at full humanize
        float wander = 0.0f, wanderTarget = 0.0f; // -1..1, the delay's slow movement
        float delay = 0.0f;         // samples
        float untilChange = 0.0f;   // seconds until new drift/wander targets
        juce::int64 write = 0;
        bool fresh = true;          // no note yet: jump straight to the first
    };

    void resetVoices() noexcept
    {
        for (auto& state : voiceState)
            state = {};
        for (auto& line : voiceDelay)
            std::fill (line.begin(), line.end(), 0.0f);
        for (auto& note : voiceNotes)
            note.store (-1);
        harmonyChord.store (-1);
        leadReference = -1;
        lastChord = {};
    }

    static int wrap12 (int n) noexcept { return ((n % 12) + 12) % 12; }

    // The in-scale note nearest `note` (below first on a tie).
    static int snapToScale (int note, int key, int mask) noexcept
    {
        for (int distance = 0; distance < 12; ++distance)
        {
            if (((mask >> wrap12 (note - distance - key)) & 1) != 0) return note - distance;
            if (((mask >> wrap12 (note + distance - key)) & 1) != 0) return note + distance;
        }
        return note;
    }

    // `steps` notes of the scale up (or down) from an in-scale note.
    static int stepInScale (int note, int steps, int key, int mask) noexcept
    {
        const auto direction = steps < 0 ? -1 : 1;
        for (int taken = 0; taken != steps; taken += direction)
        {
            note += direction;
            for (int guard = 0; guard < 12 && ((mask >> wrap12 (note - key)) & 1) == 0; ++guard)
                note += direction;
        }
        return note;
    }

    // The `count`th chord note strictly above (count > 0) or below (count < 0) a note.
    static int chordNote (const analysis::Chord& chord, int from, int count) noexcept
    {
        const auto direction = count < 0 ? -1 : 1;
        auto note = from;
        for (int found = 0; found != count;)
        {
            note += direction;
            if (chord.contains (note))
                found += direction;
        }
        return note;
    }

    // The note each voice sings, relative to the lead, for this block.
    void updateVoices (const Settings& settings, float frequency, float shift, int numVoices, float blockSeconds) noexcept
    {
        // The lead's sounding note (after autotune and shift), held with a little
        // hysteresis so a wavering note doesn't flip the harmony.
        if (frequency > 0.0f)
        {
            const auto sung = 69.0f + 12.0f * std::log2 (frequency / 440.0f) + correction + shift;
            if (leadReference < 0 || std::abs (sung - (float) leadReference) > 0.65f)
                leadReference = (int) std::lround (sung);
        }

        const auto chord = settings.chord.isValid() ? settings.chord : lastChord;
        lastChord = chord;
        harmonyChord.store (settings.harmonyMode == chordMode ? chord.pack() : -1);

        const float fixed[maxVoices] { settings.interval1, settings.interval2, settings.interval3, settings.interval4 };
        const auto scaleMask = getScaleMask (settings.scale);
        int scaleSize = 0;
        for (int n = 0; n < 12; ++n)
            scaleSize += (scaleMask >> n) & 1;
        scaleSize = juce::jmax (1, scaleSize);

        for (int v = 0; v < maxVoices; ++v)
        {
            auto& state = voiceState[(size_t) v];

            if (v >= numVoices || leadReference < 0)
            {
                voiceNotes[(size_t) v].store (-1);
                continue;
            }

            auto target = fixed[v];

            if (settings.harmonyMode == scaleMode)
            {
                const auto home = snapToScale (leadReference, settings.key, scaleMask);
                const auto steps = (int) std::lround (fixed[v] * (float) scaleSize / 12.0f);
                target = (float) (stepInScale (home, steps, settings.key, scaleMask) - leadReference);
            }
            else if (settings.harmonyMode == chordMode && chord.isValid())
            {
                target = (float) (chordTarget (chord, settings.harmonyStyle, v) - leadReference);
            }

            // Quick glide between notes (about 10 ms): no click, no audible slide.
            target = juce::jlimit (-24.0f, 24.0f, target);
            const auto glide = state.fresh ? 1.0f : 1.0f - std::exp (-blockSeconds / 0.01f);
            state.interval += (target - state.interval) * glide;
            state.fresh = false;
            voiceNotes[(size_t) v].store (frequency > 0.0f ? leadReference + (int) std::lround (target) : -1);

            // Humanize: new drift and wander targets every 0.3 - 0.8 s, glided to.
            state.untilChange -= blockSeconds;
            if (state.untilChange <= 0.0f)
            {
                state.untilChange = 0.3f + 0.5f * random.nextFloat();
                state.driftTarget = (random.nextFloat() * 2.0f - 1.0f) * 9.0f;
                state.wanderTarget = random.nextFloat() * 2.0f - 1.0f;
            }
            const auto slow = 1.0f - std::exp (-blockSeconds / 0.25f);
            state.drift += (state.driftTarget - state.drift) * slow;
            state.wander += (state.wanderTarget - state.wander) * slow;
        }
    }

    int chordTarget (const analysis::Chord& chord, int style, int v) const noexcept
    {
        switch (style)
        {
            case below:  return chordNote (chord, leadReference, -(v + 1));
            case around: return chordNote (chord, leadReference, v % 2 == 0 ? v / 2 + 1 : -(v / 2 + 1));
            case rootBelow:
            {
                if (v > 0)
                    return chordNote (chord, leadReference, v);

                // The bass note (or root) at least a fifth under the lead.
                const auto bass = chord.bass >= 0 ? chord.bass : chord.root;
                auto note = leadReference - 7;
                while (wrap12 (note) != bass)
                    --note;
                return note;
            }
            case above:
            default:     return chordNote (chord, leadReference, v + 1);
        }
    }

    PitchDetector detector;
    PsolaShifter lead;
    std::array<PsolaShifter, maxVoices> harmonies;
    std::array<VoiceState, maxVoices> voiceState;
    std::array<std::vector<float>, maxVoices> voiceDelay;
    juce::Random random { 0x51ab };
    int leadReference = -1, activeVoices = 0;
    analysis::Chord lastChord;

    std::vector<float> mono, voiceOut;
    double sampleRate = 44100.0;
    float correction = 0.0f;
    int targetNote = -1;
    bool leadWasOn = false, harmonyWasOn = false;
};
} // namespace fx
