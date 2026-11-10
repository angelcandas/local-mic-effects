#pragma once

#include "Effects.h"
#include "FxModule.h"
#include "PitchEngine.h"

#include <memory>

// Every effect as an fx::Module. Each one wraps a processor from Effects.h, PitchEngine.h
// or juce::dsp and declares its values with the same IDs and defaults as the plugin
// parameters in Parameters.cpp (a test checks they stay in sync).
namespace fx::modules
{
class HighPass final : public Module
{
public:
    HighPass() : Module (ModuleType::highPass) {}

    Value& on   = addSwitch ("hpfOn", true);
    Value& freq = addValue ("hpfFreq", 80.0f);

private:
    void prepareModule (const juce::dsp::ProcessSpec& spec) override
    {
        filter.prepare (spec);
        filter.setType (juce::dsp::StateVariableTPTFilterType::highpass);
    }

    void resetModule() override                    { filter.reset(); }
    void update() noexcept override                { filter.setCutoffFrequency (get (freq)); }
    void render (const Context& c) noexcept override { filter.process (c); }

    juce::dsp::StateVariableTPTFilter<float> filter;
};

class Gate final : public Module
{
public:
    Gate() : Module (ModuleType::gate) {}

    Value& on        = addSwitch ("gateOn", true);
    Value& threshold = addValue ("gateThreshold", -55.0f);

private:
    void prepareModule (const juce::dsp::ProcessSpec& spec) override
    {
        gate.prepare (spec);
        gate.setRatio (10.0f);
        gate.setAttack (1.0f);
        gate.setRelease (150.0f);
    }

    void resetModule() override                    { gate.reset(); }
    void update() noexcept override                { gate.setThreshold (get (threshold)); }
    void render (const Context& c) noexcept override { gate.process (c); }

    juce::dsp::NoiseGate<float> gate;
};

// Autotune, pitch/formant shift and harmonizer share one pitch detector, so they are
// one module with three switches.
class Pitch final : public Module
{
public:
    Pitch() : Module (ModuleType::pitch) {}

    Value& tuneOn        = addSwitch ("tuneOn");
    Value& tuneKey       = addValue ("tuneKey", 0.0f);
    Value& tuneScale     = addValue ("tuneScale", 0.0f);
    Value& tuneSpeed     = addValue ("tuneSpeed", 20.0f);
    Value& tuneAmount    = addValue ("tuneAmount", 100.0f);
    Value& shiftOn       = addSwitch ("pitchOn");
    Value& semitones     = addValue ("pitchShift", 0.0f);
    Value& formant       = addValue ("pitchFormant", 0.0f);
    Value& harmonyOn     = addSwitch ("harmonyOn");
    Value& harmonyVoice1 = addValue ("harmonyVoice1", 4.0f);
    Value& harmonyVoice2 = addValue ("harmonyVoice2", 7.0f);
    Value& harmonyMix    = addValue ("harmonyMix", 50.0f);

    // For the UI tuner.
    const PitchEngine& getEngine() const noexcept { return engine; }

private:
    void prepareModule (const juce::dsp::ProcessSpec& spec) override { engine.prepare (spec); }
    void resetModule() override                                      { engine.reset(); }

    void update() noexcept override
    {
        settings.tuneOn           = flag (tuneOn);
        settings.key              = index (tuneKey);
        settings.scale            = index (tuneScale);
        settings.retuneMs         = get (tuneSpeed);
        settings.amount           = fraction (tuneAmount);
        settings.shiftOn          = flag (shiftOn);
        settings.semitones        = get (semitones);
        settings.formantSemitones = get (formant);
        settings.harmonyOn        = flag (harmonyOn);
        settings.interval1        = get (harmonyVoice1);
        settings.interval2        = get (harmonyVoice2);
        settings.harmonyMix       = fraction (harmonyMix);
    }

    void render (const Context& c) noexcept override { engine.process (c, settings); }

    PitchEngine engine;
    PitchEngine::Settings settings;
};

class Eq final : public Module
{
public:
    Eq() : Module (ModuleType::eq) {}

    Value& on      = addSwitch ("eqOn");
    Value& low     = addValue ("eqLow", 0.0f);
    Value& midFreq = addValue ("eqMidFreq", 2500.0f);
    Value& midGain = addValue ("eqMidGain", 0.0f);
    Value& high    = addValue ("eqHigh", 0.0f);

private:
    void prepareModule (const juce::dsp::ProcessSpec& spec) override { eq.prepare (spec); }
    void resetModule() override                                      { eq.reset(); }
    void update() noexcept override { eq.setParameters (get (low), get (midFreq), get (midGain), get (high)); }
    void render (const Context& c) noexcept override                 { eq.process (c); }

    Equalizer eq;
};

class Compressor final : public Module
{
public:
    Compressor() : Module (ModuleType::compressor) {}

    Value& on        = addSwitch ("compOn", true);
    Value& threshold = addValue ("compThreshold", -18.0f);
    Value& ratio     = addValue ("compRatio", 3.0f);

private:
    void prepareModule (const juce::dsp::ProcessSpec& spec) override
    {
        compressor.prepare (spec);
        compressor.setAttack (10.0f);
        compressor.setRelease (120.0f);
    }

    void resetModule() override { compressor.reset(); }

    void update() noexcept override
    {
        compressor.setThreshold (get (threshold));
        compressor.setRatio (get (ratio));
    }

    void render (const Context& c) noexcept override { compressor.process (c); }

    juce::dsp::Compressor<float> compressor;
};

class DeEsser final : public Module
{
public:
    DeEsser() : Module (ModuleType::deEsser) {}

    Value& on        = addSwitch ("deessOn");
    Value& threshold = addValue ("deessThreshold", -30.0f);
    Value& freq      = addValue ("deessFreq", 6000.0f);

private:
    void prepareModule (const juce::dsp::ProcessSpec& spec) override { deEsser.prepare (spec); }
    void resetModule() override                                      { deEsser.reset(); }
    void update() noexcept override                  { deEsser.setParameters (get (threshold), get (freq)); }
    void render (const Context& c) noexcept override { deEsser.process (c); }

    fx::DeEsser deEsser;
};

class Exciter final : public Module
{
public:
    Exciter() : Module (ModuleType::exciter) {}

    Value& on     = addSwitch ("exciterOn");
    Value& freq   = addValue ("exciterFreq", 3500.0f);
    Value& amount = addValue ("exciterAmount", 30.0f);

private:
    void prepareModule (const juce::dsp::ProcessSpec& spec) override { exciter.prepare (spec); }
    void resetModule() override                                      { exciter.reset(); }
    void update() noexcept override                  { exciter.setParameters (get (freq), fraction (amount)); }
    void render (const Context& c) noexcept override { exciter.process (c); }

    fx::Exciter exciter;
};

class Saturation final : public Module
{
public:
    Saturation() : Module (ModuleType::saturation) {}

    Value& on    = addSwitch ("satOn");
    Value& drive = addValue ("satDrive", 12.0f);
    Value& mix   = addValue ("satMix", 100.0f);

private:
    void prepareModule (const juce::dsp::ProcessSpec& spec) override { saturation.prepare (spec); }
    void resetModule() override                                      { saturation.reset(); }
    void update() noexcept override                  { saturation.setParameters (get (drive), fraction (mix)); }
    void render (const Context& c) noexcept override { saturation.process (c); }

    fx::Saturation saturation;
};

class Radio final : public Module
{
public:
    Radio() : Module (ModuleType::radio) {}

    Value& on   = addSwitch ("radioOn");
    Value& low  = addValue ("radioLow", 400.0f);
    Value& high = addValue ("radioHigh", 3200.0f);

private:
    void prepareModule (const juce::dsp::ProcessSpec& spec) override { radio.prepare (spec); }
    void resetModule() override                                      { radio.reset(); }
    void update() noexcept override                  { radio.setParameters (get (low), get (high)); }
    void render (const Context& c) noexcept override { radio.process (c); }

    fx::Radio radio;
};

class Bitcrusher final : public Module
{
public:
    Bitcrusher() : Module (ModuleType::bitcrusher) {}

    Value& on   = addSwitch ("crushOn");
    Value& bits = addValue ("crushBits", 8.0f);
    Value& rate = addValue ("crushRate", 8000.0f);
    Value& mix  = addValue ("crushMix", 100.0f);

private:
    void prepareModule (const juce::dsp::ProcessSpec& spec) override { crusher.prepare (spec); }
    void resetModule() override                                      { crusher.reset(); }
    void update() noexcept override                  { crusher.setParameters (get (bits), get (rate), fraction (mix)); }
    void render (const Context& c) noexcept override { crusher.process (c); }

    fx::Bitcrusher crusher;
};

class Robot final : public Module
{
public:
    Robot() : Module (ModuleType::robot) {}

    Value& on   = addSwitch ("robotOn");
    Value& freq = addValue ("robotFreq", 50.0f);
    Value& mix  = addValue ("robotMix", 100.0f);

private:
    void prepareModule (const juce::dsp::ProcessSpec& spec) override { ring.prepare (spec); }
    void resetModule() override                                      { ring.reset(); }
    void update() noexcept override                  { ring.setParameters (get (freq), fraction (mix)); }
    void render (const Context& c) noexcept override { ring.process (c); }

    RingModulator ring;
};

class Filter final : public Module
{
public:
    Filter() : Module (ModuleType::filter) {}

    Value& on        = addSwitch ("filterOn");
    Value& type      = addValue ("filterType", 0.0f);
    Value& cutoff    = addValue ("filterCutoff", 1200.0f);
    Value& resonance = addValue ("filterResonance", 30.0f);

private:
    void prepareModule (const juce::dsp::ProcessSpec& spec) override { filter.prepare (spec); }
    void resetModule() override                                      { filter.reset(); }
    void update() noexcept override { filter.setParameters (index (type), get (cutoff), fraction (resonance)); }
    void render (const Context& c) noexcept override                 { filter.process (c); }

    ResonantFilter filter;
};

class AutoWah final : public Module
{
public:
    AutoWah() : Module (ModuleType::autoWah) {}

    Value& on          = addSwitch ("wahOn");
    Value& sensitivity = addValue ("wahSensitivity", 60.0f);
    Value& freq        = addValue ("wahFreq", 400.0f);
    Value& mix         = addValue ("wahMix", 100.0f);

private:
    void prepareModule (const juce::dsp::ProcessSpec& spec) override { wah.prepare (spec); }
    void resetModule() override                                      { wah.reset(); }
    void update() noexcept override { wah.setParameters (fraction (sensitivity), get (freq), fraction (mix)); }
    void render (const Context& c) noexcept override                 { wah.process (c); }

    fx::AutoWah wah;
};

class Tremolo final : public Module
{
public:
    Tremolo() : Module (ModuleType::tremolo) {}

    Value& on    = addSwitch ("tremoloOn");
    Value& rate  = addValue ("tremoloRate", 6.0f);
    Value& depth = addValue ("tremoloDepth", 60.0f);

private:
    void prepareModule (const juce::dsp::ProcessSpec& spec) override { tremolo.prepare (spec); }
    void resetModule() override                                      { tremolo.reset(); }
    void update() noexcept override                  { tremolo.setParameters (get (rate), fraction (depth)); }
    void render (const Context& c) noexcept override { tremolo.process (c); }

    fx::Tremolo tremolo;
};

class Vibrato final : public Module
{
public:
    Vibrato() : Module (ModuleType::vibrato) {}

    Value& on    = addSwitch ("vibratoOn");
    Value& rate  = addValue ("vibratoRate", 5.5f);
    Value& depth = addValue ("vibratoDepth", 40.0f);

private:
    void prepareModule (const juce::dsp::ProcessSpec& spec) override { vibrato.prepare (spec); }
    void resetModule() override                                      { vibrato.reset(); }
    void update() noexcept override                  { vibrato.setParameters (get (rate), fraction (depth)); }
    void render (const Context& c) noexcept override { vibrato.process (c); }

    fx::Vibrato vibrato;
};

class Chorus final : public Module
{
public:
    Chorus() : Module (ModuleType::chorus) {}

    Value& on    = addSwitch ("chorusOn");
    Value& rate  = addValue ("chorusRate", 0.8f);
    Value& depth = addValue ("chorusDepth", 35.0f);
    Value& mix   = addValue ("chorusMix", 50.0f);

private:
    void prepareModule (const juce::dsp::ProcessSpec& spec) override
    {
        chorus.prepare (spec);
        chorus.setCentreDelay (7.0f);
        chorus.setFeedback (0.0f);
    }

    void resetModule() override { chorus.reset(); }

    void update() noexcept override
    {
        chorus.setRate (get (rate));
        chorus.setDepth (fraction (depth));
        chorus.setMix (fraction (mix));
    }

    void render (const Context& c) noexcept override { chorus.process (c); }

    juce::dsp::Chorus<float> chorus;
};

// juce::dsp::Chorus with a short delay and feedback is a flanger.
class Flanger final : public Module
{
public:
    Flanger() : Module (ModuleType::flanger) {}

    Value& on       = addSwitch ("flangerOn");
    Value& rate     = addValue ("flangerRate", 0.25f);
    Value& depth    = addValue ("flangerDepth", 70.0f);
    Value& feedback = addValue ("flangerFeedback", 60.0f);
    Value& mix      = addValue ("flangerMix", 50.0f);

private:
    void prepareModule (const juce::dsp::ProcessSpec& spec) override
    {
        flanger.prepare (spec);
        flanger.setCentreDelay (2.5f);
    }

    void resetModule() override { flanger.reset(); }

    void update() noexcept override
    {
        // Depth 1 would sweep +/-10 ms; scaled to a flanger-sized +/-2 ms.
        flanger.setRate (get (rate));
        flanger.setDepth (0.2f * fraction (depth));
        flanger.setFeedback (fraction (feedback));
        flanger.setMix (fraction (mix));
    }

    void render (const Context& c) noexcept override { flanger.process (c); }

    juce::dsp::Chorus<float> flanger;
};

class Phaser final : public Module
{
public:
    Phaser() : Module (ModuleType::phaser) {}

    Value& on       = addSwitch ("phaserOn");
    Value& rate     = addValue ("phaserRate", 0.4f);
    Value& depth    = addValue ("phaserDepth", 70.0f);
    Value& feedback = addValue ("phaserFeedback", 50.0f);
    Value& mix      = addValue ("phaserMix", 50.0f);

private:
    void prepareModule (const juce::dsp::ProcessSpec& spec) override
    {
        phaser.prepare (spec);
        phaser.setCentreFrequency (1300.0f);
    }

    void resetModule() override { phaser.reset(); }

    void update() noexcept override
    {
        phaser.setRate (get (rate));
        phaser.setDepth (fraction (depth));
        phaser.setFeedback (fraction (feedback));
        phaser.setMix (fraction (mix));
    }

    void render (const Context& c) noexcept override { phaser.process (c); }

    juce::dsp::Phaser<float> phaser;
};

class Doubler final : public Module
{
public:
    Doubler() : Module (ModuleType::doubler) {}

    Value& on     = addSwitch ("doublerOn");
    Value& mix    = addValue ("doublerMix", 50.0f);
    Value& spread = addValue ("doublerSpread", 80.0f);

private:
    void prepareModule (const juce::dsp::ProcessSpec& spec) override { doubler.prepare (spec); }
    void resetModule() override                                      { doubler.reset(); }
    void update() noexcept override                  { doubler.setParameters (fraction (mix), fraction (spread)); }
    void render (const Context& c) noexcept override { doubler.process (c); }

    fx::Doubler doubler;
};

class Echo final : public Module
{
public:
    Echo() : Module (ModuleType::echo) {}

    Value& on       = addSwitch ("delayOn");
    Value& time     = addValue ("delayTime", 350.0f);
    Value& feedback = addValue ("delayFeedback", 35.0f);
    Value& mix      = addValue ("delayMix", 25.0f);
    Value& pingPong = addValue ("delayPingPong", 0.0f);

private:
    void prepareModule (const juce::dsp::ProcessSpec& spec) override { echo.prepare (spec); }
    void resetModule() override                                      { echo.reset(); }

    void update() noexcept override
    {
        echo.setParameters (get (time), fraction (feedback), fraction (mix), flag (pingPong));
    }

    void render (const Context& c) noexcept override { echo.process (c); }

    fx::Echo echo;
};

// Always runs: switching it off fades the wet signal out (juce::Reverb smooths gain
// changes) instead of cutting the tail abruptly.
class Reverb final : public Module
{
public:
    Reverb() : Module (ModuleType::reverb) {}

    Value& on      = addSwitch ("reverbOn", true);
    Value& mix     = addValue ("reverbMix", 25.0f);
    Value& size    = addValue ("reverbSize", 50.0f);
    Value& damping = addValue ("reverbDamping", 50.0f);
    Value& width   = addValue ("reverbWidth", 100.0f);

private:
    void prepareModule (const juce::dsp::ProcessSpec& spec) override { reverb.prepare (spec); }
    void resetModule() override                                      { reverb.reset(); }

    void update() noexcept override
    {
        const auto wet = flag (on) ? fraction (mix) : 0.0f;
        const auto angle = wet * juce::MathConstants<float>::halfPi;

        juce::dsp::Reverb::Parameters parameters;
        parameters.roomSize = fraction (size);
        parameters.damping  = fraction (damping);
        parameters.width    = fraction (width);
        // juce::Reverb scales dry by 2 and wet by 3 internally; dividing those out gives
        // an equal-power crossfade where 0% is the untouched dry signal.
        parameters.dryLevel = std::cos (angle) / 2.0f;
        parameters.wetLevel = std::sin (angle) / 3.0f;
        reverb.setParameters (parameters);
    }

    void render (const Context& c) noexcept override { reverb.process (c); }
    bool alwaysProcesses() const noexcept override   { return true; }

    juce::dsp::Reverb reverb;
};

class Width final : public Module
{
public:
    Width() : Module (ModuleType::width) {}

    Value& on     = addSwitch ("widthOn");
    Value& amount = addValue ("widthAmount", 150.0f);

private:
    void prepareModule (const juce::dsp::ProcessSpec& spec) override { width.prepare (spec); }
    void resetModule() override                                      { width.reset(); }
    void update() noexcept override                  { width.setParameters (fraction (amount)); }
    void render (const Context& c) noexcept override { width.process (c); }

    StereoWidth width;
};
}

namespace fx
{
inline std::unique_ptr<Module> createModule (ModuleType type)
{
    switch (type)
    {
        case ModuleType::highPass:   return std::make_unique<modules::HighPass>();
        case ModuleType::gate:       return std::make_unique<modules::Gate>();
        case ModuleType::pitch:      return std::make_unique<modules::Pitch>();
        case ModuleType::eq:         return std::make_unique<modules::Eq>();
        case ModuleType::compressor: return std::make_unique<modules::Compressor>();
        case ModuleType::deEsser:    return std::make_unique<modules::DeEsser>();
        case ModuleType::exciter:    return std::make_unique<modules::Exciter>();
        case ModuleType::saturation: return std::make_unique<modules::Saturation>();
        case ModuleType::radio:      return std::make_unique<modules::Radio>();
        case ModuleType::bitcrusher: return std::make_unique<modules::Bitcrusher>();
        case ModuleType::robot:      return std::make_unique<modules::Robot>();
        case ModuleType::filter:     return std::make_unique<modules::Filter>();
        case ModuleType::autoWah:    return std::make_unique<modules::AutoWah>();
        case ModuleType::tremolo:    return std::make_unique<modules::Tremolo>();
        case ModuleType::vibrato:    return std::make_unique<modules::Vibrato>();
        case ModuleType::chorus:     return std::make_unique<modules::Chorus>();
        case ModuleType::flanger:    return std::make_unique<modules::Flanger>();
        case ModuleType::phaser:     return std::make_unique<modules::Phaser>();
        case ModuleType::doubler:    return std::make_unique<modules::Doubler>();
        case ModuleType::echo:       return std::make_unique<modules::Echo>();
        case ModuleType::reverb:     return std::make_unique<modules::Reverb>();
        case ModuleType::width:      return std::make_unique<modules::Width>();
    }

    jassertfalse;
    return {};
}

// A stable name for each type, for project files and the UI.
inline const char* getModuleKey (ModuleType type)
{
    static constexpr const char* keys[] = {
        "highPass", "gate", "pitch", "eq", "compressor", "deEsser", "exciter", "saturation", "radio", "bitcrusher",
        "robot", "filter", "autoWah", "tremolo", "vibrato", "chorus", "flanger", "phaser", "doubler", "echo", "reverb", "width"
    };
    static_assert (std::size (keys) == numModuleTypes);
    return keys[(size_t) type];
}
}
