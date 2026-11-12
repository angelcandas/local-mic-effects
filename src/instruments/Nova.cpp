// Nova: an analog-style polysynth. Two oscillators (with unison), a sub and noise into
// a resonant filter with its own envelope, an LFO, glide and mono/legato modes, then
// chorus, echo and a room.
#include "Fx.h"
#include "Registry.h"

#include <map>

namespace micfx::inst
{
namespace
{
    enum Wave { saw, square, triangle, sine };

    // What the voices read, set once per block.
    struct NovaBlock
    {
        int wave1 = 0, wave2 = 0, unison = 1, filterType = 0;
        float level1 = 0, level2 = 0, pulseWidth = 0.5f, semi2 = 0, fine2 = 0, sub = 0, noise = 0, spread = 0;
        float cutoff = 1000, resonance = 0, drive = 0, envAmount = 0, keyTrack = 0;
        float fA = 0, fD = 0, fS = 0, fR = 0, aA = 0, aD = 0, aS = 0, aR = 0, velAmp = 0;
        float glideCoefficient = 1, bend = 0, lfoPitch = 0, lfoCutoff = 0, lfoAmp = 0;
        const float* lfo = nullptr; // per sample, -1..1, for the whole block
        const int* offset = nullptr; // where the voices are in the block
    };

    float oscillator (int wave, float phase, float dt, float width) noexcept
    {
        switch (wave)
        {
            case saw:      return 2.0f * phase - 1.0f - polyBlep (phase, dt);
            case square:
            {
                auto shifted = phase + 1.0f - width;
                shifted -= std::floor (shifted);
                return (phase < width ? 1.0f : -1.0f) + polyBlep (phase, dt) - polyBlep (shifted, dt);
            }
            case triangle: return 4.0f * std::abs (phase - 0.5f) - 1.0f;
            default:       return std::sin (juce::MathConstants<float>::twoPi * phase);
        }
    }

    constexpr float unisonDetune[3][5] = { { 0, 0, 0, 0, 0 }, { -1, 0, 1, 0, 0 }, { -1, -0.45f, 0, 0.5f, 1 } };
    constexpr int unisonCounts[3] = { 1, 3, 5 };

    class NovaVoice
    {
    public:
        const NovaBlock* block = nullptr;

        void prepare (double rate)
        {
            sampleRate = rate;
            amp.setSampleRate (rate);
            filterEnvelope.setSampleRate (rate);
        }

        void start (int note, float vel) { start (note, vel, (float) note, true); }

        void start (int note, float vel, float from, bool retrigger)
        {
            const auto wasActive = isActive();
            target = (float) note;
            current = from;
            velocity = vel;
            noteNumber = note;
            amp.setParameters (block->aA, block->aD, block->aS, block->aR);
            filterEnvelope.setParameters (block->fA, block->fD, block->fS, block->fR);
            if (retrigger || ! wasActive)
            {
                amp.noteOn();
                filterEnvelope.noteOn();
            }
            if (! wasActive)
            {
                noise.seed ((uint32_t) note * 977u + 13u);
                for (int u = 0; u < 5; ++u)
                {
                    phase1[u] = 0.13f * (float) u + 0.07f * (float) (note % 7);
                    phase2[u] = 0.29f * (float) u + 0.11f * (float) (note % 5);
                    phase1[u] -= std::floor (phase1[u]);
                    phase2[u] -= std::floor (phase2[u]);
                }
                phaseSub = 0.0f;
                for (auto& f : filters) f.reset();
            }
            counter = 0;
        }

        void glideTo (int note) { target = (float) note; noteNumber = note; }
        float getPitch() const { return current; }

        void release() { amp.noteOff(); filterEnvelope.noteOff(); }
        void kill()    { amp.reset(); filterEnvelope.reset(); }
        bool isActive() const { return amp.isActive(); }

        void render (float* left, float* right, int n)
        {
            const auto& b = *block;
            const auto units = unisonCounts[juce::jlimit (0, 2, b.unison)];
            const auto& detunes = unisonDetune[juce::jlimit (0, 2, b.unison)];
            const auto unitGain = 1.0f / std::sqrt ((float) units);
            const auto velocityGain = 1.0f - b.velAmp * (1.0f - velocity);
            const auto driveGain = 1.0f + 4.0f * b.drive;
            const auto driveMakeup = 1.0f / std::tanh (juce::jmax (0.2f, 0.6f * driveGain)) * 0.6f;

            float increments1[5] {}, increments2[5] {}, pansL[5] {}, pansR[5] {};
            for (int u = 0; u < units; ++u)
            {
                const auto pan = 0.5f + 0.5f * detunes[u] * juce::jmin (1.0f, b.spread * 1.4f);
                pansL[u] = std::cos (pan * juce::MathConstants<float>::halfPi) * unitGain;
                pansR[u] = std::sin (pan * juce::MathConstants<float>::halfPi) * unitGain;
            }

            // Envelope changes reach notes that are already sounding.
            amp.setParameters (b.aA, b.aD, b.aS, b.aR);
            filterEnvelope.setParameters (b.fA, b.fD, b.fS, b.fR);

            float subIncrement = 0.0f;
            for (int i = 0; i < n; ++i)
            {
                const auto lfo = b.lfo[*b.offset + i];

                // Pitch and filter move every 16 samples.
                if ((counter++ & 15) == 0)
                {
                    current += (target - current) * (1.0f - std::pow (1.0f - b.glideCoefficient, 16.0f));
                    const auto pitch = current + b.bend + lfo * b.lfoPitch;
                    const auto hz = midiToHz (pitch);
                    const auto hz2 = midiToHz (pitch + b.semi2 + b.fine2 * 0.01f);
                    for (int u = 0; u < units; ++u)
                    {
                        const auto detune = std::exp2 (detunes[u] * b.spread * 30.0f / 1200.0f);
                        increments1[u] = juce::jmin (0.45f, hz * detune / (float) sampleRate);
                        increments2[u] = juce::jmin (0.45f, hz2 / detune / (float) sampleRate);
                    }
                    subIncrement = hz * 0.5f / (float) sampleRate;

                    const auto envelopeDepth = b.envAmount * 6.0f * (0.6f + 0.4f * velocity);
                    const auto octaves = std::log2 (b.cutoff) + envelopeDepth * filterEnvelope.getValue()
                                       + b.keyTrack * (pitch - 60.0f) / 12.0f + lfo * b.lfoCutoff * 3.0f;
                    const auto cutoff = std::exp2 (juce::jlimit (4.3f, 14.4f, octaves));
                    const auto reso = b.resonance;
                    for (int ch = 0; ch < 2; ++ch)
                    {
                        filters[ch * 2].set (cutoff, b.filterType == 0 ? reso * 0.4f : reso, sampleRate);
                        filters[ch * 2 + 1].set (cutoff, reso, sampleRate);
                    }
                }

                // Oscillators.
                auto sumL = 0.0f, sumR = 0.0f;
                for (int u = 0; u < units; ++u)
                {
                    const auto o = b.level1 * oscillator (b.wave1, phase1[u], increments1[u], b.pulseWidth)
                                 + b.level2 * oscillator (b.wave2, phase2[u], increments2[u], b.pulseWidth);
                    sumL += o * pansL[u];
                    sumR += o * pansR[u];
                    phase1[u] += increments1[u]; phase1[u] -= std::floor (phase1[u]);
                    phase2[u] += increments2[u]; phase2[u] -= std::floor (phase2[u]);
                }
                const auto shared = b.sub * (phaseSub < 0.5f ? 0.7f : -0.7f) + b.noise * 0.5f * noise.next();
                phaseSub += subIncrement; phaseSub -= std::floor (phaseSub);
                sumL += shared * 0.7071f;
                sumR += shared * 0.7071f;

                if (b.drive > 0.001f)
                {
                    sumL = std::tanh (sumL * 0.6f * driveGain) * driveMakeup;
                    sumR = std::tanh (sumR * 0.6f * driveGain) * driveMakeup;
                }

                // Filter.
                float outL, outR;
                switch (b.filterType)
                {
                    case 0:  outL = filters[1].lowpass (filters[0].lowpass (sumL)); outR = filters[3].lowpass (filters[2].lowpass (sumR)); break;
                    case 1:  outL = filters[1].lowpass (sumL); outR = filters[3].lowpass (sumR); break;
                    case 2:  outL = 1.5f * filters[1].bandpass (sumL); outR = 1.5f * filters[3].bandpass (sumR); break;
                    default: outL = filters[1].highpass (sumL); outR = filters[3].highpass (sumR); break;
                }

                filterEnvelope.next();
                const auto gain = amp.next() * velocityGain * (1.0f - b.lfoAmp * 0.5f * (1.0f + lfo)) * 0.28f;
                left[i] += outL * gain;
                right[i] += outR * gain;

                if (! amp.isActive())
                    break;
            }
        }

    private:
        double sampleRate = 48000.0;
        Envelope amp, filterEnvelope;
        Svf filters[4];
        Noise noise;
        float phase1[5] {}, phase2[5] {}, phaseSub = 0.0f;
        float current = 60.0f, target = 60.0f, velocity = 1.0f;
        int noteNumber = 60;
        uint32_t counter = 0;
    };

    class NovaEngine final : public VoiceEngine<NovaVoice>
    {
    public:
        explicit NovaEngine (Params p) : VoiceEngine (16)
        {
            for (const auto* id : { "osc1Wave", "osc1Level", "pulseWidth", "osc2Wave", "osc2Semi", "osc2Fine", "osc2Level",
                                    "sub", "noise", "unison", "spread", "filterType", "cutoff", "resonance", "drive",
                                    "filterEnv", "keyTrack", "fAttack", "fDecay", "fSustain", "fRelease", "aAttack",
                                    "aDecay", "aSustain", "aRelease", "velAmp", "lfoShape", "lfoRate", "lfoPitch",
                                    "lfoCutoff", "lfoAmp", "mode", "glide", "bendRange", "chorus", "echo", "echoTime",
                                    "echoFeedback", "reverb" })
                values[id] = p (id);
            for (auto& slot : voices)
                slot.voice.block = &block;
            block.offset = &blockOffset;
        }

        double getTailSeconds() const override { return 3.0; }

    private:
        float v (const char* id) const { return values.at (id)->load (std::memory_order_relaxed); }
        int choiceOf (const char* id) const { return (int) std::round (v (id)); }

        void prepareEngine (double rate, int maxBlock) override
        {
            lfoBuffer.assign ((size_t) maxBlock + 1, 0.0f);
            chorus.prepare (rate);
            echo.prepare (rate);
            room.prepare (rate);
        }

        void resetEngine() override
        {
            chorus.reset();
            echo.reset();
            room.reset();
            heldNotes.clear();
        }

        void beginBlock (int n) override
        {
            if ((int) lfoBuffer.size() < n)
                lfoBuffer.resize ((size_t) n, 0.0f); // only if a host breaks its block size promise

            block.wave1 = choiceOf ("osc1Wave");      block.level1 = v ("osc1Level");
            block.wave2 = choiceOf ("osc2Wave");      block.level2 = v ("osc2Level");
            block.pulseWidth = v ("pulseWidth");      block.semi2 = std::round (v ("osc2Semi"));
            block.fine2 = v ("osc2Fine");             block.sub = v ("sub");
            block.noise = v ("noise");                block.unison = choiceOf ("unison");
            block.spread = v ("spread");              block.filterType = choiceOf ("filterType");
            block.cutoff = v ("cutoff");              block.resonance = v ("resonance");
            block.drive = v ("drive");                block.envAmount = v ("filterEnv");
            block.keyTrack = v ("keyTrack");
            block.fA = v ("fAttack"); block.fD = v ("fDecay"); block.fS = v ("fSustain"); block.fR = v ("fRelease");
            block.aA = v ("aAttack"); block.aD = v ("aDecay"); block.aS = v ("aSustain"); block.aR = v ("aRelease");
            block.velAmp = v ("velAmp");
            block.bend = pitchBend * v ("bendRange");
            block.lfoPitch = v ("lfoPitch") + modWheel * 0.5f;
            block.lfoCutoff = v ("lfoCutoff");
            block.lfoAmp = v ("lfoAmp");
            const auto glide = v ("glide");
            block.glideCoefficient = glide < 0.001f ? 1.0f : 1.0f - decayCoefficient (glide / 3.0f, sampleRate);

            // The LFO for the whole block.
            const auto shape = choiceOf ("lfoShape");
            const auto increment = v ("lfoRate") / (float) sampleRate;
            for (int i = 0; i < n; ++i)
            {
                float value;
                switch (shape)
                {
                    case 0:  value = std::sin (juce::MathConstants<float>::twoPi * lfoPhase); break;
                    case 1:  value = 4.0f * std::abs (lfoPhase - 0.5f) - 1.0f; break;
                    case 2:  value = 1.0f - 2.0f * lfoPhase; break;
                    case 3:  value = lfoPhase < 0.5f ? 1.0f : -1.0f; break;
                    default: value = sampleAndHold; break;
                }
                lfoBuffer[(size_t) i] = value;
                lfoPhase += increment;
                if (lfoPhase >= 1.0f)
                {
                    lfoPhase -= 1.0f;
                    sampleAndHold = lfoNoise.next();
                }
            }
            block.lfo = lfoBuffer.data();
        }

        void endBlock (juce::AudioBuffer<float>& buffer) override
        {
            auto* l = buffer.getWritePointer (0);
            auto* r = buffer.getWritePointer (1);
            const auto n = buffer.getNumSamples();
            chorus.process (l, r, n, v ("chorus"));
            echo.process (l, r, n, v ("echo") * 0.7f, v ("echoTime"), v ("echoFeedback"));
            room.process (l, r, n, v ("reverb"), 0.82f, 0.45f);
        }

        // Mono and legato: one voice; the newest held key plays.
        void noteOn (int note, float velocity) override
        {
            const auto mode = choiceOf ("mode");
            const auto glideFrom = lastNote < 0 ? (float) note : (v ("glide") > 0.001f ? lastPitch() : (float) note);
            lastNote = note;

            if (mode == 0)
            {
                VoiceEngine::noteOn (note, velocity);
                // Poly glide: the new voice slides in from the last note.
                for (auto& slot : voices)
                    if (slot.note == note && slot.held)
                        slot.voice.start (note, velocity, glideFrom, true);
                return;
            }

            heldNotes.removeFirstMatchingValue (note);
            heldNotes.add (note);
            auto& slot = voices.front();
            const auto legato = mode == 2 && slot.voice.isActive() && heldNotes.size() > 1;
            if (legato)
                slot.voice.glideTo (note);
            else
                slot.voice.start (note, velocity, slot.voice.isActive() ? slot.voice.getPitch() : glideFrom, true);
            slot.note = note;
            slot.held = true;
        }

        void noteOff (int note) override
        {
            if (choiceOf ("mode") == 0)
            {
                VoiceEngine::noteOff (note);
                return;
            }

            heldNotes.removeFirstMatchingValue (note);
            auto& slot = voices.front();
            if (slot.note != note)
                return;
            if (! heldNotes.isEmpty())
            {
                slot.voice.glideTo (heldNotes.getLast());
                slot.note = heldNotes.getLast();
            }
            else
            {
                slot.held = false;
                if (! sustain)
                    slot.voice.release();
            }
        }

        float lastPitch() const
        {
            const Slot* newest = nullptr;
            for (const auto& slot : voices)
                if (slot.voice.isActive() && (newest == nullptr || slot.age > newest->age))
                    newest = &slot;
            return newest != nullptr ? newest->voice.getPitch() : (float) lastNote;
        }

        std::map<juce::String, std::atomic<float>*> values;
        NovaBlock block;
        std::vector<float> lfoBuffer;
        float lfoPhase = 0.0f, sampleAndHold = 0.0f;
        Noise lfoNoise;
        Chorus chorus;
        Echo echo;
        Room room;
        juce::Array<int> heldNotes;
        int lastNote = -1;
    };
}

const Spec& getNovaSpec()
{
    static const Spec spec = []
    {
        Spec s;
        s.key = "nova";
        s.name = "Nova";
        s.tagline = "Analog polysynth";
        s.category = "Synth";
        s.accent = juce::Colour (0xff4fd6c4);
        s.lowestKey = 36;
        s.highestKey = 96;

        const juce::StringArray waves { "Saw", "Pulse", "Triangle", "Sine" };
        s.params = {
            choice ("osc1Wave", "Wave", waves, 0),
            knob ("osc1Level", "Level", 0.0f, 1.0f, 0.8f),
            knob ("pulseWidth", "Width", 0.05f, 0.5f, 0.5f),
            choice ("osc2Wave", "Wave", waves, 0),
            knob ("osc2Semi", "Pitch", -24.0f, 24.0f, 0.0f, Unit::semitones, 0.0f, 1.0f),
            knob ("osc2Fine", "Detune", -50.0f, 50.0f, 7.0f, Unit::cents),
            knob ("osc2Level", "Level", 0.0f, 1.0f, 0.5f),
            knob ("sub", "Sub", 0.0f, 1.0f, 0.0f),
            knob ("noise", "Noise", 0.0f, 1.0f, 0.0f),
            choice ("unison", "Unison", { "1", "3", "5" }, 0),
            knob ("spread", "Spread", 0.0f, 1.0f, 0.3f),
            choice ("filterType", "Type", { "LP 24", "LP 12", "Band", "High" }, 0),
            knob ("cutoff", "Cutoff", 30.0f, 18000.0f, 2500.0f, Unit::hertz, 1200.0f),
            knob ("resonance", "Reso", 0.0f, 1.0f, 0.2f),
            knob ("drive", "Drive", 0.0f, 1.0f, 0.0f),
            knob ("keyTrack", "Key track", 0.0f, 1.0f, 0.5f),
            knob ("filterEnv", "Amount", -1.0f, 1.0f, 0.35f, Unit::bipolar),
            knob ("fAttack", "Attack", 0.0005f, 8.0f, 0.005f, Unit::seconds, 0.4f),
            knob ("fDecay", "Decay", 0.01f, 8.0f, 0.5f, Unit::seconds, 0.8f),
            knob ("fSustain", "Sustain", 0.0f, 1.0f, 0.4f),
            knob ("fRelease", "Release", 0.01f, 8.0f, 0.4f, Unit::seconds, 0.8f),
            knob ("aAttack", "Attack", 0.0005f, 8.0f, 0.005f, Unit::seconds, 0.4f),
            knob ("aDecay", "Decay", 0.01f, 8.0f, 0.4f, Unit::seconds, 0.8f),
            knob ("aSustain", "Sustain", 0.0f, 1.0f, 0.8f),
            knob ("aRelease", "Release", 0.01f, 8.0f, 0.35f, Unit::seconds, 0.8f),
            knob ("velAmp", "Velocity", 0.0f, 1.0f, 0.6f),
            choice ("lfoShape", "Shape", { "Sine", "Triangle", "Saw", "Square", "Random" }, 0),
            knob ("lfoRate", "Rate", 0.05f, 20.0f, 4.0f, Unit::hertz, 3.0f),
            knob ("lfoPitch", "Vibrato", 0.0f, 1.0f, 0.0f),
            knob ("lfoCutoff", "Wah", 0.0f, 1.0f, 0.0f),
            knob ("lfoAmp", "Tremolo", 0.0f, 1.0f, 0.0f),
            choice ("mode", "Mode", { "Poly", "Mono", "Legato" }, 0),
            knob ("glide", "Glide", 0.0f, 1.5f, 0.0f, Unit::seconds, 0.2f),
            knob ("bendRange", "Bend", 0.0f, 12.0f, 2.0f, Unit::semitones, 0.0f, 1.0f),
            knob ("chorus", "Chorus", 0.0f, 1.0f, 0.0f),
            knob ("echo", "Echo", 0.0f, 1.0f, 0.0f),
            knob ("echoTime", "Time", 0.05f, 1.5f, 0.375f, Unit::seconds, 0.4f),
            knob ("echoFeedback", "Repeats", 0.0f, 0.9f, 0.35f),
            knob ("reverb", "Room", 0.0f, 1.0f, 0.15f),
        };

        s.rows = {
            { { "Oscillator 1", { "osc1Wave", "osc1Level", "pulseWidth" }, {} },
              { "Oscillator 2", { "osc2Wave", "osc2Semi", "osc2Fine", "osc2Level" }, {} },
              { "Mix", { "sub", "noise", "unison", "spread" }, {} },
              { "Voice", { "mode", "glide", "bendRange" }, {} } },
            { { "Filter", { "filterType", "cutoff", "resonance", "drive", "keyTrack" }, {}, { "filterType", "cutoff", "resonance" } },
              { "Filter envelope", { "filterEnv", "fAttack", "fDecay", "fSustain", "fRelease" }, { "fAttack", "fDecay", "fSustain", "fRelease" } },
              { "Amp envelope", { "aAttack", "aDecay", "aSustain", "aRelease", "velAmp" }, { "aAttack", "aDecay", "aSustain", "aRelease" } } },
            { { "LFO", { "lfoShape", "lfoRate", "lfoPitch", "lfoCutoff", "lfoAmp" }, {} },
              { "Effects", { "chorus", "echo", "echoTime", "echoFeedback", "reverb" }, {} } },
        };

        s.presets = {
            { "Warm Pad", { { "osc2Fine", 9.0f }, { "unison", 1.0f }, { "spread", 0.45f }, { "cutoff", 1400.0f }, { "resonance", 0.15f },
                            { "filterEnv", 0.15f }, { "fAttack", 1.2f }, { "fDecay", 2.0f }, { "fSustain", 0.6f }, { "fRelease", 1.5f },
                            { "aAttack", 0.8f }, { "aDecay", 1.0f }, { "aSustain", 0.85f }, { "aRelease", 1.6f },
                            { "lfoRate", 0.3f }, { "lfoCutoff", 0.1f }, { "chorus", 0.5f }, { "reverb", 0.45f } } },
            { "Strings", { { "osc2Fine", 12.0f }, { "unison", 1.0f }, { "spread", 0.55f }, { "cutoff", 3200.0f }, { "resonance", 0.05f },
                           { "filterEnv", 0.05f }, { "aAttack", 0.35f }, { "aDecay", 0.5f }, { "aSustain", 0.9f }, { "aRelease", 0.7f },
                           { "lfoRate", 5.2f }, { "lfoPitch", 0.03f }, { "chorus", 0.6f }, { "reverb", 0.35f } } },
            { "Synth Bass", { { "osc2Wave", 1.0f }, { "osc2Level", 0.4f }, { "sub", 0.7f }, { "cutoff", 380.0f }, { "resonance", 0.3f },
                              { "filterEnv", 0.45f }, { "fAttack", 0.001f }, { "fDecay", 0.35f }, { "fSustain", 0.2f }, { "fRelease", 0.2f },
                              { "aAttack", 0.002f }, { "aDecay", 0.5f }, { "aSustain", 0.8f }, { "aRelease", 0.12f },
                              { "keyTrack", 0.3f }, { "mode", 1.0f }, { "glide", 0.03f }, { "drive", 0.3f }, { "reverb", 0.0f } } },
            { "Solo Lead", { { "osc2Wave", 1.0f }, { "osc2Fine", 6.0f }, { "cutoff", 2500.0f }, { "resonance", 0.25f }, { "filterEnv", 0.3f },
                             { "fDecay", 0.4f }, { "fSustain", 0.5f }, { "aSustain", 0.85f }, { "aRelease", 0.25f },
                             { "mode", 2.0f }, { "glide", 0.08f }, { "lfoPitch", 0.15f }, { "lfoRate", 5.5f },
                             { "echo", 0.25f }, { "echoFeedback", 0.35f }, { "reverb", 0.2f } } },
            { "Brass", { { "osc2Fine", 5.0f }, { "cutoff", 900.0f }, { "resonance", 0.1f }, { "filterEnv", 0.5f }, { "fAttack", 0.06f },
                         { "fDecay", 0.6f }, { "fSustain", 0.45f }, { "fRelease", 0.3f }, { "aAttack", 0.04f }, { "aDecay", 0.4f },
                         { "aSustain", 0.85f }, { "aRelease", 0.25f }, { "chorus", 0.2f }, { "reverb", 0.25f } } },
            { "Pluck Arp", { { "osc2Wave", 1.0f }, { "cutoff", 600.0f }, { "resonance", 0.35f }, { "filterEnv", 0.65f }, { "fAttack", 0.001f },
                             { "fDecay", 0.25f }, { "fSustain", 0.0f }, { "fRelease", 0.25f }, { "aAttack", 0.001f }, { "aDecay", 0.5f },
                             { "aSustain", 0.0f }, { "aRelease", 0.3f }, { "echo", 0.3f }, { "echoTime", 0.25f }, { "echoFeedback", 0.45f },
                             { "reverb", 0.3f } } },
            { "Dream Keys", { { "osc1Wave", 2.0f }, { "osc2Wave", 3.0f }, { "osc2Semi", 12.0f }, { "osc2Fine", 0.0f }, { "osc2Level", 0.3f },
                              { "cutoff", 2400.0f }, { "filterEnv", 0.2f }, { "aAttack", 0.005f }, { "aDecay", 1.6f }, { "aSustain", 0.3f },
                              { "aRelease", 0.8f }, { "chorus", 0.45f }, { "reverb", 0.4f } } },
            { "Supersaw", { { "osc2Fine", 0.0f }, { "osc2Level", 0.6f }, { "unison", 2.0f }, { "spread", 0.6f }, { "cutoff", 5000.0f },
                            { "filterEnv", 0.1f }, { "aRelease", 0.5f }, { "chorus", 0.3f }, { "reverb", 0.3f } } },
            { "Sub Bass", { { "osc1Wave", 3.0f }, { "osc1Level", 0.9f }, { "osc2Wave", 2.0f }, { "osc2Level", 0.15f }, { "cutoff", 600.0f },
                            { "filterEnv", 0.1f }, { "resonance", 0.0f }, { "aAttack", 0.003f }, { "aSustain", 1.0f }, { "aRelease", 0.1f },
                            { "mode", 1.0f }, { "reverb", 0.0f } } },
            { "Init", {} },
        };

        s.makeEngine = [] (Params p) { return std::make_unique<NovaEngine> (p); };
        return s;
    }();
    return spec;
}
}
