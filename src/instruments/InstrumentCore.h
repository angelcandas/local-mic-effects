#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <atomic>
#include <cmath>
#include <functional>
#include <memory>
#include <vector>

// The MicFX instruments (Nova, Grand, ...): what each one is made of.
//
// An instrument is described by a Spec: its parameters, how its window lays them out,
// its factory presets and the Engine that makes its sound. InstrumentProcessor turns a
// Spec into a juce::AudioProcessor (the built-in instruments of MIDI tracks, and the
// VST3/AU plugins), and InstrumentEditor draws its window from the same Spec.
namespace micfx::inst
{
//==============================================================================
// Parameters.
enum class Unit { none, percent, seconds, hertz, decibels, semitones, cents, octaves, multiplier, bipolar };

struct ParamSpec
{
    const char* id;
    const char* name;
    float min = 0.0f, max = 1.0f, def = 0.0f;
    Unit unit = Unit::percent;
    float centre = 0.0f;        // when set (min < centre < max): the value halfway along the knob
    float step = 0.0f;          // 1 for whole numbers
    juce::StringArray choices;  // a choice (shown as a menu) when not empty
    bool toggle = false;        // an on/off switch
    bool vertical = false;      // a vertical slider (drawbars) instead of a knob
};

// Shorthands for the instruments' parameter lists.
inline ParamSpec knob (const char* id, const char* name, float min, float max, float def, Unit unit = Unit::percent,
                       float centre = 0.0f, float step = 0.0f)
{
    ParamSpec p;
    p.id = id; p.name = name; p.min = min; p.max = max; p.def = def; p.unit = unit; p.centre = centre; p.step = step;
    return p;
}

inline ParamSpec choice (const char* id, const char* name, juce::StringArray choices, int def = 0)
{
    ParamSpec p;
    p.id = id; p.name = name; p.min = 0.0f; p.max = (float) (choices.size() - 1); p.def = (float) def;
    p.unit = Unit::none; p.step = 1.0f; p.choices = std::move (choices);
    return p;
}

inline ParamSpec toggle (const char* id, const char* name, bool def = false)
{
    ParamSpec p;
    p.id = id; p.name = name; p.min = 0.0f; p.max = 1.0f; p.def = def ? 1.0f : 0.0f; p.unit = Unit::none; p.step = 1.0f;
    p.toggle = true;
    return p;
}

inline ParamSpec drawbar (const char* id, const char* name, float def)
{
    auto p = knob (id, name, 0.0f, 8.0f, def, Unit::none, 0.0f, 1.0f);
    p.vertical = true;
    return p;
}

juce::String formatValue (const ParamSpec&, float value);

//==============================================================================
// The window: rows of sections, each a titled group of controls.
struct Section
{
    Section (juce::String t, juce::StringArray p, juce::StringArray e = {}, juce::StringArray f = {})
        : title (std::move (t)), params (std::move (p)), envelope (std::move (e)), filter (std::move (f)) {}

    juce::String title;
    juce::StringArray params;
    juce::StringArray envelope; // attack, decay, sustain, release ids: draws the envelope's shape
    juce::StringArray filter;   // type (LP 24, LP 12, band, high), cutoff, resonance ids: draws its response
};

using Row = std::vector<Section>;

struct Preset
{
    juce::String name;
    std::vector<std::pair<const char*, float>> values; // the rest keep their defaults
};

//==============================================================================
// Reads parameter values on the audio thread (choices read as their index).
class Params
{
public:
    explicit Params (juce::AudioProcessorValueTreeState& s) : state (s) {}

    std::atomic<float>* operator() (const char* id) const
    {
        auto* p = state.getRawParameterValue (id);
        jassert (p != nullptr); // a parameter the Spec doesn't have
        return p;
    }

private:
    juce::AudioProcessorValueTreeState& state;
};

// Makes the sound: renders MIDI into a cleared stereo buffer.
class Engine
{
public:
    virtual ~Engine() = default;
    virtual void prepare (double sampleRate, int maxBlockSize) = 0;
    virtual void reset() = 0;
    virtual void process (juce::AudioBuffer<float>& stereo, const juce::MidiBuffer& midi) = 0;
    virtual double getTailSeconds() const { return 2.0; }
};

struct Spec
{
    juce::String key;      // "nova": "builtin:nova" in projects
    juce::String name;     // "Nova"
    juce::String tagline;  // "Analog polysynth"
    juce::String category; // "Synth", "Piano", ... (the VST3 category and the library's grouping)
    juce::Colour accent;
    std::vector<ParamSpec> params;
    std::vector<Row> rows;
    std::vector<Preset> presets;
    std::function<std::unique_ptr<Engine> (Params)> makeEngine;
    int lowestKey = 36, highestKey = 96; // the window's keyboard
    std::function<juce::String (int note)> keyName; // a name on each key (a drum kit's), or C octave labels
};

//==============================================================================
// Small DSP shared by the engines.
inline float midiToHz (float note) noexcept { return 440.0f * std::exp2 ((note - 69.0f) / 12.0f); }

inline float decayCoefficient (float seconds, double sampleRate) noexcept
{
    return (float) std::exp (-1.0 / (juce::jmax (1.0e-4, (double) seconds) * sampleRate));
}

// Band-limited step correction (PolyBLEP) for saws and squares.
inline float polyBlep (float t, float dt) noexcept
{
    if (t < dt)        { t /= dt; return t + t - t * t - 1.0f; }
    if (t > 1.0f - dt) { t = (t - 1.0f) / dt; return t * t + t + t + 1.0f; }
    return 0.0f;
}

// A fast white noise source.
struct Noise
{
    uint32_t state = 0x12345678u;
    void seed (uint32_t s) noexcept { state = s * 747796405u + 2891336453u; if (state == 0) state = 1; }
    float next() noexcept
    {
        state ^= state << 13; state ^= state >> 17; state ^= state << 5;
        return (float) (int32_t) state * (1.0f / 2147483648.0f);
    }
};

// ADSR with analog-style curves: the attack rises towards an overshoot, the decay and
// release fall exponentially. Times are roughly how long each stage takes.
class Envelope
{
public:
    void setSampleRate (double rate) noexcept { sampleRate = rate; }

    void setParameters (float attack, float decay, float sustain, float release) noexcept
    {
        constexpr float overshoot = 1.3f;
        const auto attackTau = juce::jmax (0.0005f, attack) / std::log (overshoot / (overshoot - 1.0f));
        attackCoefficient = 1.0f - (float) std::exp (-1.0 / (attackTau * sampleRate));
        decayCoefficientValue = decayCoefficient (juce::jmax (0.001f, decay) / 4.6f, sampleRate);
        releaseCoefficient = decayCoefficient (juce::jmax (0.002f, release) / 4.6f, sampleRate);
        sustainLevel = juce::jlimit (0.0f, 1.0f, sustain);
    }

    void noteOn() noexcept  { stage = Stage::attack; }
    void noteOff() noexcept { if (stage != Stage::idle) stage = Stage::release; }
    void reset() noexcept   { stage = Stage::idle; value = 0.0f; }
    bool isActive() const noexcept { return stage != Stage::idle; }
    bool isReleasing() const noexcept { return stage == Stage::release; }
    float getValue() const noexcept { return value; }

    float next() noexcept
    {
        switch (stage)
        {
            case Stage::attack:
                value += (1.3f - value) * attackCoefficient;
                if (value >= 1.0f) { value = 1.0f; stage = Stage::decay; }
                break;
            case Stage::decay:
                value = sustainLevel + (value - sustainLevel) * decayCoefficientValue;
                break;
            case Stage::release:
                value *= releaseCoefficient;
                if (value < 1.0e-4f) { value = 0.0f; stage = Stage::idle; }
                break;
            case Stage::idle:
                break;
        }
        return value;
    }

private:
    enum class Stage { idle, attack, decay, release };
    Stage stage = Stage::idle;
    double sampleRate = 48000.0;
    float value = 0.0f, attackCoefficient = 0.01f, decayCoefficientValue = 0.999f, releaseCoefficient = 0.999f,
          sustainLevel = 1.0f;
};

// A zero-delay-feedback state variable filter (one channel).
struct Svf
{
    float ic1 = 0.0f, ic2 = 0.0f, a1 = 0.0f, a2 = 0.0f, a3 = 0.0f, k = 1.4f;

    void set (float cutoff, float resonance, double sampleRate) noexcept
    {
        const auto g = std::tan (juce::MathConstants<float>::pi
                                 * juce::jlimit (10.0f, (float) sampleRate * 0.49f, cutoff) / (float) sampleRate);
        k = 2.0f - 1.96f * juce::jlimit (0.0f, 1.0f, resonance);
        a1 = 1.0f / (1.0f + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
    }

    void reset() noexcept { ic1 = ic2 = 0.0f; }

    // Returns the low-pass; bp/hp when asked.
    float process (float v0, float& bp, float& hp) noexcept
    {
        const auto v3 = v0 - ic2;
        const auto v1 = a1 * ic1 + a2 * v3;
        const auto v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2.0f * v1 - ic1;
        ic2 = 2.0f * v2 - ic2;
        bp = v1;
        hp = v0 - k * v1 - v2;
        return v2;
    }

    float lowpass (float v0) noexcept { float bp, hp; return process (v0, bp, hp); }
    float bandpass (float v0) noexcept { float bp, hp; process (v0, bp, hp); return bp; }
    float highpass (float v0) noexcept { float bp, hp; process (v0, bp, hp); return hp; }
};

//==============================================================================
// A polyphonic engine: splits the block at each MIDI event, keeps the sustain pedal,
// pitch bend and mod wheel, and gives notes to voices (stealing the oldest when all
// are busy). Voice needs:
//   void prepare (double rate);  void start (int note, float velocity);  void release();
//   void kill();  bool isActive() const;  void render (float* left, float* right, int n);
template <typename Voice>
class VoiceEngine : public Engine
{
public:
    explicit VoiceEngine (int numVoices) : voices ((size_t) numVoices) {}

    void prepare (double rate, int maxBlockSize) override
    {
        sampleRate = rate;
        for (auto& v : voices)
            v.voice.prepare (rate);
        prepareEngine (rate, maxBlockSize);
        reset();
    }

    void reset() override
    {
        for (auto& v : voices)
        {
            v.voice.kill();
            v.note = -1;
            v.held = false;
        }
        sustain = false;
        pitchBend = 0.0f;
        modWheel = 0.0f;
        resetEngine();
    }

    void process (juce::AudioBuffer<float>& buffer, const juce::MidiBuffer& midi) override
    {
        const auto numSamples = buffer.getNumSamples();
        auto* left = buffer.getWritePointer (0);
        auto* right = buffer.getWritePointer (buffer.getNumChannels() > 1 ? 1 : 0);

        beginBlock (numSamples);

        int position = 0;
        for (const auto metadata : midi)
        {
            const auto at = juce::jlimit (0, numSamples, metadata.samplePosition);
            if (at > position)
            {
                blockOffset = position;
                renderVoices (left + position, right + position, at - position);
                position = at;
            }
            handle (metadata.getMessage());
        }

        blockOffset = position;
        if (position < numSamples)
            renderVoices (left + position, right + position, numSamples - position);

        if (right == left)
            juce::FloatVectorOperations::multiply (left, 0.5f, numSamples);

        endBlock (buffer);
    }

    int countActiveVoices() const
    {
        int n = 0;
        for (const auto& v : voices)
            n += v.voice.isActive() ? 1 : 0;
        return n;
    }

protected:
    struct Slot
    {
        Voice voice;
        int note = -1;
        bool held = false;      // the key is down (else the pedal or the release holds it)
        uint32_t age = 0;
    };

    // For the engine: per-block parameter reads, the effects after the voices.
    virtual void prepareEngine (double, int) {}
    virtual void resetEngine() {}
    virtual void beginBlock (int) {}
    virtual void endBlock (juce::AudioBuffer<float>&) {}
    virtual void onController (int, int) {}

    virtual void noteOn (int note, float velocity)
    {
        // A key struck again while it still sounds: the old one fades as the new one starts.
        for (auto& v : voices)
            if (v.note == note && v.voice.isActive())
            {
                v.voice.release();
                v.held = false;
                v.note = -1;
            }

        auto& slot = findFreeVoice();
        slot.note = note;
        slot.held = true;
        slot.age = ++counter;
        slot.voice.start (note, velocity);
    }

    virtual void noteOff (int note)
    {
        for (auto& v : voices)
            if (v.note == note && v.held)
            {
                v.held = false;
                if (! sustain)
                    v.voice.release();
            }
    }

    void allNotesOff (bool hard)
    {
        for (auto& v : voices)
        {
            v.held = false;
            if (hard) v.voice.kill(); else v.voice.release();
        }
    }

    Slot& findFreeVoice()
    {
        Slot* best = nullptr;
        for (auto& v : voices)
            if (! v.voice.isActive())
                return v;

        // All busy: the oldest released voice, else the oldest one.
        for (auto& v : voices)
            if (! v.held && (best == nullptr || v.age < best->age))
                best = &v;
        if (best == nullptr)
            for (auto& v : voices)
                if (best == nullptr || v.age < best->age)
                    best = &v;
        best->voice.kill();
        return *best;
    }

    void renderVoices (float* left, float* right, int n)
    {
        for (auto& v : voices)
            if (v.voice.isActive())
                v.voice.render (left, right, n);
            else
                v.note = -1;
    }

    void handle (const juce::MidiMessage& m)
    {
        if (m.isNoteOn())
            noteOn (m.getNoteNumber(), m.getFloatVelocity());
        else if (m.isNoteOff())
            noteOff (m.getNoteNumber());
        else if (m.isPitchWheel())
            pitchBend = ((float) m.getPitchWheelValue() - 8192.0f) / 8192.0f;
        else if (m.isSustainPedalOn())
            sustain = true;
        else if (m.isSustainPedalOff())
        {
            sustain = false;
            for (auto& v : voices)
                if (! v.held && v.voice.isActive())
                    v.voice.release();
        }
        else if (m.isAllNotesOff() || m.isAllSoundOff())
            allNotesOff (m.isAllSoundOff());
        else if (m.isController())
        {
            if (m.getControllerNumber() == 1)
                modWheel = (float) m.getControllerValue() / 127.0f;
            onController (m.getControllerNumber(), m.getControllerValue());
        }
    }

    std::vector<Slot> voices;
    double sampleRate = 48000.0;
    bool sustain = false;
    float pitchBend = 0.0f; // -1..1
    float modWheel = 0.0f;  // 0..1
    uint32_t counter = 0;
    int blockOffset = 0; // where in the block the voices are rendering now
};
}
