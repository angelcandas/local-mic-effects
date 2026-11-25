#pragma once

#include "Biquad.h"
#include "FxModule.h"
#include "../analysis/Chords.h"

#include <array>
#include <atomic>
#include <bitset>
#include <cmath>

namespace fx
{
//==============================================================================
// A channel vocoder: the voice (the modulator) is split into 16, 24 or 32 bands, each
// band's level is followed, and those levels shape the same bands of a built-in synth
// (the carrier), so the synth "speaks" with the voice's vowels.
//
// - Bands: log-spaced 100 Hz..8 kHz, two cascaded band-pass biquads each (24 dB/oct).
// - Carrier: up to 8 voices of saw, pulse or ensemble (three detuned saws), PolyBLEP so
//   they don't alias; notes glide in over 5 ms and fade over 80 ms when they change.
//   Each carrier band is levelled against its own envelope (capped at +26 dB), so the
//   voice's spectral shape comes through whatever the synth's own tilt.
// - Formant: the synth's bands sit higher or lower than the voice's (±12 semitones),
//   a smaller or bigger "mouth".
// - Unvoiced: s, t, f and sh have no pitch for the synth to carry; when the voice's
//   highs dominate, its own top end (over 3.5 kHz) is let through so words stay clear.
//
// The notes come from setNotes() (the audio thread, before each block): keys held on a
// MIDI keyboard, else the song's chord, else a drone on the key (see Source).
class Vocoder
{
public:
    static constexpr int maxBands = 32, maxVoices = 8;
    static constexpr float lowHz = 100.0f, highHz = 8000.0f;

    enum class Carrier { saw, pulse, ensemble };
    enum Source { keysThenChords, chordsOnly, drone };

    struct Settings
    {
        int bands = 24;
        Carrier carrier = Carrier::saw;
        int octave = 0;           // -2..2
        float formant = 0.0f;     // semitones
        float releaseMs = 60.0f;  // band envelopes
        float unvoiced = 0.5f;    // 0..1
        float mix = 1.0f;         // 0..1
    };

    void prepare (const juce::dsp::ProcessSpec& spec) noexcept
    {
        sampleRate = spec.sampleRate;
        attackCoefficient = coefficient (0.002);
        fadeIn = (float) (1.0 / (0.005 * sampleRate));
        fadeOut = (float) (1.0 / (0.08 * sampleRate));
        sibilanceHigh.setup (Biquad::Type::highPass, sampleRate, 3500.0, 0.707);
        sibilanceFollow = coefficient (0.01);
        configured = {};
        update (settings);
        reset();
    }

    void reset() noexcept
    {
        for (auto& band : bands)
        {
            for (auto& f : band.analysis) f.reset();
            for (auto& f : band.synthesis) f.reset();
            band.modEnvelope = band.carEnvelope = 0.0f;
        }
        for (auto& voice : voices)
            voice = {};
        sibilanceHigh.reset();
        highEnvelope = fullEnvelope = 0.0f;
    }

    // Any thread that owns the processing (the module's update()).
    void update (const Settings& s) noexcept
    {
        settings = s;
        settings.bands = juce::jlimit (4, maxBands, s.bands);
        releaseCoefficient = coefficient (juce::jmax (1.0f, s.releaseMs) * 0.001);

        if (configured.bands != settings.bands || std::abs (configured.formant - settings.formant) > 1.0e-3f)
        {
            const auto count = settings.bands;
            const auto span = std::log2 (highHz / lowHz);
            const auto octavesPerBand = span / (float) count;
            // A band-pass Q whose -3 dB edges meet the neighbours'; cascaded twice it's a
            // little narrower, which keeps vowels distinct.
            const auto q = 1.0 / (2.0 * std::sinh (std::log (2.0) / 2.0 * octavesPerBand));
            const auto shift = std::pow (2.0f, settings.formant / 12.0f);
            makeup = std::sqrt ((float) count) * 0.9f;

            for (int b = 0; b < count; ++b)
            {
                const auto centre = lowHz * std::pow (2.0f, ((float) b + 0.5f) * octavesPerBand);
                auto& band = bands[(size_t) b];
                for (auto& f : band.analysis)
                    f.setup (Biquad::Type::bandPass, sampleRate, centre, q);
                const auto shifted = juce::jlimit (40.0, sampleRate * 0.45, (double) (centre * shift));
                for (auto& f : band.synthesis)
                    f.setup (Biquad::Type::bandPass, sampleRate, shifted, q);
            }
            configured.bands = settings.bands;
            configured.formant = settings.formant;
        }
    }

    // The notes to play (MIDI note numbers, up to 8; the lowest win). Audio thread.
    void setNotes (const std::bitset<128>& notes) noexcept
    {
        std::array<int, maxVoices> wanted {};
        int count = 0;
        for (int n = 0; n < 128 && count < maxVoices; ++n)
            if (notes[(size_t) n])
                wanted[(size_t) count++] = juce::jlimit (0, 127, n + 12 * settings.octave);

        // Voices already on a wanted note keep it; the rest fade, and free voices take
        // the new notes.
        std::array<bool, maxVoices> taken {};
        for (auto& voice : voices)
        {
            bool keep = false;
            for (int i = 0; i < count && ! keep; ++i)
                if (! taken[(size_t) i] && voice.note == wanted[(size_t) i] && voice.gate)
                    keep = taken[(size_t) i] = true;
            if (! keep)
                voice.gate = false;
        }
        for (int i = 0; i < count; ++i)
        {
            if (taken[(size_t) i])
                continue;
            Voice* chosen = nullptr;
            for (auto& voice : voices) // a silent voice, else the quietest fading one
                if (! voice.gate && (chosen == nullptr || voice.level < chosen->level))
                    chosen = &voice;
            if (chosen == nullptr)
                break;
            chosen->note = wanted[(size_t) i];
            chosen->gate = true;
            const auto hz = 440.0 * std::pow (2.0, (wanted[(size_t) i] - 69) / 12.0);
            for (int o = 0; o < 3; ++o)
            {
                static constexpr double detuneCents[] = { 0.0, -9.0, 8.0 };
                chosen->increment[(size_t) o] = hz * std::pow (2.0, detuneCents[o] / 1200.0) / sampleRate;
                if (chosen->level <= 0.0f)
                    chosen->phase[(size_t) o] = 0.37 * o; // spread so the ensemble doesn't start in phase
            }
        }
        playing = count;
    }

    int getNumNotes() const noexcept { return playing; }

    // The voice's band levels (dB) right now, for the UI. Any thread.
    std::array<std::atomic<float>, maxBands> bandLevelDb {};
    std::atomic<float> sibilance { 0.0f }; // 0..1, how much of the voice's top end passes now

    void process (const juce::dsp::ProcessContextReplacing<float>& context) noexcept
    {
        auto& block = context.getOutputBlock();
        const auto numChannels = block.getNumChannels();
        const auto numSamples = (int) block.getNumSamples();
        if (numChannels == 0)
            return;

        auto* left = block.getChannelPointer (0);
        auto* right = numChannels > 1 ? block.getChannelPointer (1) : nullptr;
        const auto count = settings.bands;
        const auto maxGain = 20.0f; // +26 dB, levelling a quiet carrier band

        for (int i = 0; i < numSamples; ++i)
        {
            const auto dry = right != nullptr ? 0.5f * (left[i] + right[i]) : left[i];
            const auto carrier = renderCarrier();

            float wet = 0.0f;
            for (int b = 0; b < count; ++b)
            {
                auto& band = bands[(size_t) b];
                auto m = band.analysis[1].process (0, band.analysis[0].process (0, dry));
                auto c = band.synthesis[1].process (0, band.synthesis[0].process (0, carrier));
                follow (band.modEnvelope, std::abs (m));
                follow (band.carEnvelope, std::abs (c));
                const auto gain = juce::jmin (maxGain, band.modEnvelope / (band.carEnvelope + 1.0e-5f));
                wet += c * gain;
            }
            wet *= makeup * 0.42f;

            // Unvoiced: the voice's highs against its whole level.
            const auto high = sibilanceHigh.process (0, dry);
            highEnvelope += sibilanceFollow * (std::abs (high) - highEnvelope);
            fullEnvelope += sibilanceFollow * (std::abs (dry) - fullEnvelope);
            const auto ratio = highEnvelope / (fullEnvelope + 1.0e-6f);
            const auto noisy = juce::jlimit (0.0f, 1.0f, (ratio - 0.25f) / 0.35f);
            wet += high * noisy * settings.unvoiced * 1.5f;
            lastSibilance = noisy;

            const auto out = dry * (1.0f - settings.mix) + wet * settings.mix;
            left[i] = out;
            if (right != nullptr)
                right[i] = out; // mono in, the same on both sides
        }

        for (size_t ch = 2; ch < numChannels; ++ch)
            block.getSingleChannelBlock (ch).copyFrom (block.getSingleChannelBlock (0));

        for (int b = 0; b < maxBands; ++b)
            bandLevelDb[(size_t) b].store (b < count ? juce::Decibels::gainToDecibels (bands[(size_t) b].modEnvelope * 2.0f, -90.0f) : -90.0f,
                                           std::memory_order_relaxed);
        sibilance.store (lastSibilance * settings.unvoiced, std::memory_order_relaxed);
    }

private:
    struct Band
    {
        std::array<Biquad, 2> analysis, synthesis;
        float modEnvelope = 0.0f, carEnvelope = 0.0f;
    };

    struct Voice
    {
        int note = -1;
        bool gate = false;
        float level = 0.0f;
        std::array<double, 3> phase {}, increment {};
    };

    float coefficient (double seconds) const noexcept { return (float) (1.0 - std::exp (-1.0 / (seconds * sampleRate))); }

    void follow (float& envelope, float x) const noexcept
    {
        envelope += (x > envelope ? attackCoefficient : releaseCoefficient) * (x - envelope);
    }

    static float polyBlep (double t, double dt) noexcept
    {
        if (t < dt)       { t /= dt; return (float) (t + t - t * t - 1.0); }
        if (t > 1.0 - dt) { t = (t - 1.0) / dt; return (float) (t * t + t + t + 1.0); }
        return 0.0f;
    }

    float renderCarrier() noexcept
    {
        float sum = 0.0f;
        for (auto& voice : voices)
        {
            voice.level = voice.gate ? juce::jmin (1.0f, voice.level + fadeIn) : juce::jmax (0.0f, voice.level - fadeOut);
            if (voice.level <= 0.0f)
                continue;

            float s = 0.0f;
            const auto oscillators = settings.carrier == Carrier::ensemble ? 3 : 1;
            for (int o = 0; o < oscillators; ++o)
            {
                auto& phase = voice.phase[(size_t) o];
                const auto dt = voice.increment[(size_t) o];
                auto saw = (float) (2.0 * phase - 1.0) - polyBlep (phase, dt);
                if (settings.carrier == Carrier::pulse)
                {
                    // A 30 % pulse: two saws, one shifted.
                    auto shifted = phase + 0.3;
                    if (shifted >= 1.0) shifted -= 1.0;
                    saw = saw - ((float) (2.0 * shifted - 1.0) - polyBlep (shifted, dt));
                }
                s += saw;
                phase += dt;
                if (phase >= 1.0) phase -= 1.0;
            }
            sum += s * voice.level / (float) oscillators;
        }
        return sum * 0.3f;
    }

    Settings settings;
    struct { int bands = 0; float formant = 1000.0f; } configured;
    double sampleRate = 48000.0;
    float attackCoefficient = 0.01f, releaseCoefficient = 0.001f, fadeIn = 0.004f, fadeOut = 0.0003f;
    float makeup = 4.0f, sibilanceFollow = 0.002f, highEnvelope = 0.0f, fullEnvelope = 0.0f, lastSibilance = 0.0f;
    Biquad sibilanceHigh;
    std::array<Band, maxBands> bands {};
    std::array<Voice, maxVoices> voices {};
    int playing = 0;
};

//==============================================================================
// Which notes the carrier plays: keys held, else the chord voiced around C3, else a
// drone (root, fifth, octave) on the key. `key` is 0..11, or -1 for none (then C).
inline std::bitset<128> pickVocoderNotes (int source, const std::bitset<128>& held, const analysis::Chord& chord, int key) noexcept
{
    std::bitset<128> notes;
    if (source == Vocoder::keysThenChords && held.any())
        return held;

    if (source != Vocoder::drone && chord.isValid())
    {
        const auto root = 48 + chord.root;
        notes.set ((size_t) root);
        const auto mask = chord.getMask();
        for (int i = 1; i < 12; ++i)
            if ((mask >> ((chord.root + i) % 12)) & 1)
                notes.set ((size_t) (root + i));
        if (chord.bass >= 0 && chord.bass != chord.root)
            notes.set ((size_t) (36 + chord.bass));
        return notes;
    }

    const auto k = key >= 0 ? key % 12 : 0;
    notes.set ((size_t) (48 + k));
    notes.set ((size_t) (55 + k));
    notes.set ((size_t) (60 + k));
    return notes;
}

namespace modules
{
//==============================================================================
// The vocoder as an effect. Live mode gives it the keys held on a MIDI keyboard and the
// backing track's chords; in the DAW, the keys, the MIDI tracks' notes and the chord
// track (or the chords heard in the other tracks).
class VocoderModule final : public Module
{
public:
    VocoderModule() : Module (ModuleType::vocoder) {}

    Value& on       = addSwitch ("vocoderOn");
    Value& bands    = addChoice ("vocoderBands", 1.0f);    // 16, 24, 32
    Value& carrier  = addChoice ("vocoderCarrier", 0.0f);  // Saw, Pulse, Ensemble
    Value& source   = addChoice ("vocoderNotes", 0.0f);    // Vocoder::Source
    Value& octave   = addChoice ("vocoderOctave", 2.0f);   // -2..+2
    Value& formant  = addValue ("vocoderFormant", 0.0f);   // semitones
    Value& release  = addValue ("vocoderRelease", 60.0f);  // ms
    Value& unvoiced = addValue ("vocoderUnvoiced", 50.0f); // %
    Value& mix      = addValue ("vocoderMix", 100.0f);     // %

    static const juce::StringArray& getBandNames()    { static const juce::StringArray n { "16", "24", "32" }; return n; }
    static const juce::StringArray& getCarrierNames() { static const juce::StringArray n { "Saw", "Pulse", "Ensemble" }; return n; }
    static const juce::StringArray& getSourceNames()  { static const juce::StringArray n { "Keys, then chords", "Chords", "Drone on the key" }; return n; }
    static const juce::StringArray& getOctaveNames()  { static const juce::StringArray n { "-2", "-1", "0", "+1", "+2" }; return n; }

    // Audio thread, before each block: what's held, the chord, the key (-1: none).
    void setMusic (const std::bitset<128>& held, const analysis::Chord& chord, int key) noexcept
    {
        const auto notes = pickVocoderNotes (index (source), held, chord, key);
        if (notes != lastNotes || index (octave) != lastOctave)
        {
            lastNotes = notes;
            lastOctave = index (octave);
            vocoder.update (makeSettings());
            vocoder.setNotes (notes);
            publishNotes (notes);
        }
        chordNow.store (chord.pack(), std::memory_order_relaxed);
        fromKeys.store (index (source) == Vocoder::keysThenChords && held.any(), std::memory_order_relaxed);
    }

    const Vocoder& getVocoder() const noexcept { return vocoder; }

    // bands: the voice's level per band (dB), unvoiced: 0..1, notes: [MIDI], playing:
    // "keys" / "chord" / "drone".
    void addReadings (juce::DynamicObject& readings) override
    {
        juce::Array<juce::var> levels;
        const auto count = std::array<int, 3> { 16, 24, 32 }[(size_t) juce::jlimit (0, 2, index (bands))];
        for (int b = 0; b < count; ++b)
            levels.add (std::round (vocoder.bandLevelDb[(size_t) b].load (std::memory_order_relaxed) * 10.0f) / 10.0f);
        readings.setProperty ("bands", levels);
        readings.setProperty ("unvoiced", std::round (vocoder.sibilance.load (std::memory_order_relaxed) * 100.0f) / 100.0f);

        juce::Array<juce::var> notes;
        for (const auto& note : shownNotes)
            if (const auto n = note.load (std::memory_order_relaxed); n >= 0)
                notes.add (n);
        readings.setProperty ("notes", notes);
        const auto chord = analysis::Chord::unpack (chordNow.load (std::memory_order_relaxed));
        readings.setProperty ("chord", chord.getName());
        readings.setProperty ("playing", fromKeys.load() ? "keys" : index (source) != Vocoder::drone && chord.isValid() ? "chord" : "drone");
    }

private:
    // The notes the synth plays (after the octave), for the UI.
    void publishNotes (const std::bitset<128>& notes) noexcept
    {
        size_t i = 0;
        for (int n = 0; n < 128 && i < shownNotes.size(); ++n)
            if (notes[(size_t) n])
                shownNotes[i++].store (juce::jlimit (0, 127, n + 12 * (juce::jlimit (0, 4, index (octave)) - 2)), std::memory_order_relaxed);
        for (; i < shownNotes.size(); ++i)
            shownNotes[i].store (-1, std::memory_order_relaxed);
    }

    Vocoder::Settings makeSettings() const noexcept
    {
        Vocoder::Settings s;
        s.bands = std::array<int, 3> { 16, 24, 32 }[(size_t) juce::jlimit (0, 2, index (bands))];
        s.carrier = (Vocoder::Carrier) juce::jlimit (0, 2, index (carrier));
        s.octave = juce::jlimit (0, 4, index (octave)) - 2;
        s.formant = get (formant);
        s.releaseMs = get (release);
        s.unvoiced = fraction (unvoiced);
        s.mix = fraction (mix);
        return s;
    }

    void prepareModule (const juce::dsp::ProcessSpec& spec) override
    {
        vocoder.update (makeSettings());
        vocoder.prepare (spec);
        vocoder.setNotes (lastNotes);
        publishNotes (lastNotes);
    }

    void resetModule() override
    {
        vocoder.reset();
        vocoder.setNotes (lastNotes);
    }

    void update() noexcept override { vocoder.update (makeSettings()); }
    void render (const Context& c) noexcept override { vocoder.process (c); }

    Vocoder vocoder;
    std::bitset<128> lastNotes = pickVocoderNotes (Vocoder::drone, {}, {}, 0);
    int lastOctave = 2;
    std::array<std::atomic<int>, Vocoder::maxVoices> shownNotes { -1, -1, -1, -1, -1, -1, -1, -1 };
    std::atomic<int> chordNow { -1 };
    std::atomic<bool> fromKeys { false };
};
}
}
