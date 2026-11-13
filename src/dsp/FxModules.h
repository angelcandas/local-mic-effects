#pragma once

#include "Effects.h"
#include "ParametricEq.h"
#include "FxModule.h"
#include "Dynamics.h"
#include "AutoLevel.h"
#include "Mastering.h"
#include "PitchEngine.h"
#include "SpectralDenoiser.h"
#include "Smooth.h"
#include "Cabinet.h"
#include "Convolver.h"

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

// Spectral noise reduction (SpectralDenoiser.h): hiss, hum and room noise taken out
// band by band, also under the voice. Learn is a choice rather than a switch: it doesn't
// turn the module on, it makes it average what it hears into the noise profile.
class Denoise final : public Module
{
public:
    Denoise() : Module (ModuleType::denoise) {}

    Value& on        = addSwitch ("denoiseOn");
    Value& reduction = addValue ("denoiseReduction", 12.0f);
    Value& threshold = addValue ("denoiseThreshold", 0.0f);
    Value& adaptive  = addChoice ("denoiseAdaptive", 1.0f);
    Value& learn     = addChoice ("denoiseLearn", 0.0f);

    // One FFT frame while it's on.
    int getLatencySamples() const noexcept override { return flag (on) ? latency.load (std::memory_order_relaxed) : 0; }

    const SpectralDenoiser& getDenoiser() const noexcept { return denoiser; }

    // A noise profile learnt elsewhere (Sound check), taken while running. Message thread.
    bool offerProfile (const std::vector<float>& powerPerBin) { return denoiser.offerProfile (powerPerBin); }

    // noise: the profile per band (dB, 60 Hz..16 kHz log-spaced), reduction: what each
    // band is turned down by right now (dB), learning: seconds learnt while Learn is on.
    void addReadings (juce::DynamicObject& readings) override
    {
        juce::Array<juce::var> noise, cut;

        for (int b = 0; b < SpectralDenoiser::numBands; ++b)
        {
            noise.add (std::round (denoiser.noiseBandsDb[(size_t) b].load (std::memory_order_relaxed) * 10.0f) / 10.0f);
            cut.add (std::round (denoiser.reductionBandsDb[(size_t) b].load (std::memory_order_relaxed) * 10.0f) / 10.0f);
        }

        readings.setProperty ("noise", noise);
        readings.setProperty ("reduction", cut);

        if (flag (learn))
            readings.setProperty ("learning", std::round (denoiser.getLearnedSeconds() * 10.0) / 10.0);
    }

    void carryStateFrom (const Module& previous) override
    {
        if (auto* other = dynamic_cast<const Denoise*> (&previous))
        {
            std::vector<float> profile;
            if (other->denoiser.copyProfile (profile))
                denoiser.seedProfile (std::move (profile));
        }
    }

private:
    void prepareModule (const juce::dsp::ProcessSpec& spec) override
    {
        denoiser.prepare (spec);
        latency.store (denoiser.getLatencySamples(), std::memory_order_relaxed);
    }

    void resetModule() override { denoiser.reset(); }

    void update() noexcept override
    {
        denoiser.setParameters (get (reduction), get (threshold), flag (adaptive), flag (learn));
    }

    void render (const Context& c) noexcept override { denoiser.process (c); }

    SpectralDenoiser denoiser;
    std::atomic<int> latency { SpectralDenoiser::getFrameSize (48000.0) };
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
    Value& tuneKey       = addChoice ("tuneKey", 0.0f);
    Value& tuneScale     = addChoice ("tuneScale", 0.0f);
    Value& tuneSpeed     = addValue ("tuneSpeed", 20.0f);
    Value& tuneAmount    = addValue ("tuneAmount", 100.0f);
    Value& shiftOn       = addSwitch ("pitchOn");
    Value& semitones     = addValue ("pitchShift", 0.0f);
    Value& formant       = addValue ("pitchFormant", 0.0f);
    Value& harmonyOn     = addSwitch ("harmonyOn");
    Value& harmonyVoice1 = addValue ("harmonyVoice1", 4.0f);
    Value& harmonyVoice2 = addValue ("harmonyVoice2", 7.0f);
    Value& harmonyMix    = addValue ("harmonyMix", 50.0f);
    Value& harmonyMode     = addChoice ("harmonyMode", 0.0f);
    Value& harmonyVoices   = addChoice ("harmonyVoices", 1.0f);   // index: 0 = one voice ... 3 = four
    Value& harmonyStyle    = addChoice ("harmonyStyle", 0.0f);
    Value& harmonySource   = addChoice ("harmonySource", 0.0f);   // Live: getHarmonySourceNames()
    Value& harmonyVoice3   = addValue ("harmonyVoice3", -5.0f);
    Value& harmonyVoice4   = addValue ("harmonyVoice4", 12.0f);
    Value& harmonyHumanize = addValue ("harmonyHumanize", 35.0f);
    Value& harmonySpread   = addValue ("harmonySpread", 50.0f);
    Value& harmonyFormant  = addValue ("harmonyFormant", 0.0f);

    // Where Live mode hears the chords (the DAW listens to the other tracks). Keys held
    // on a MIDI keyboard win over the others while they're down.
    enum HarmonySource { backingTrack, instrumentInput, midiKeys };
    static const juce::StringArray& getHarmonySourceNames()
    {
        static const juce::StringArray names { "Backing track", "Instrument input", "MIDI keys" };
        return names;
    }

    // For the UI tuner.
    const PitchEngine& getEngine() const noexcept { return engine; }

    // Whether the harmonizer wants a chord now (on, in Chords mode). Audio thread.
    bool isFollowingChords() const noexcept
    {
        return flag (harmonyOn) && index (harmonyMode) == PitchEngine::chordMode;
    }

    int getHarmonySource() const noexcept { return index (harmonySource); }

    // The chord the harmony voices follow in Chords mode, set by whoever listens to the
    // music (audio thread, before each block).
    void setChord (const analysis::Chord& chord) noexcept { settings.chord = chord; }

    // Autotune and pitch shift delay the voice (see PsolaShifter). The harmonizer alone
    // doesn't: the lead stays dry and only the harmony voices lag a little behind it.
    int getLatencySamples() const noexcept override
    {
        return flag (tuneOn) || flag (shiftOn) ? PsolaShifter::getNominalLatency (sampleRate) : 0;
    }

private:
    void prepareModule (const juce::dsp::ProcessSpec& spec) override
    {
        sampleRate = spec.sampleRate;
        engine.prepare (spec);
    }
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
        settings.harmonyMode      = index (harmonyMode);
        settings.harmonyVoices    = index (harmonyVoices) + 1;
        settings.harmonyStyle     = index (harmonyStyle);
        settings.interval3        = get (harmonyVoice3);
        settings.interval4        = get (harmonyVoice4);
        settings.humanize         = fraction (harmonyHumanize);
        settings.spread           = fraction (harmonySpread);
        settings.voiceFormant     = get (harmonyFormant);
    }

    void render (const Context& c) noexcept override { engine.process (c, settings); }

    PitchEngine engine;
    PitchEngine::Settings settings;
    double sampleRate = 48000.0;
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

class ParaEq final : public Module
{
public:
    ParaEq() : Module (ModuleType::paraEq) {}

    Value& on          = addSwitch ("peqOn");
    Value& lowCutOn    = addChoice ("peqLowCutOn", 0.0f); // a value, not a switch: it doesn't switch the module on
    Value& lowCutFreq  = addValue ("peqLowCutFreq", 80.0f);
    Value& lowFreq     = addValue ("peqLowFreq", 120.0f);
    Value& lowGain     = addValue ("peqLowGain", 0.0f);
    Value& bell1Freq   = addValue ("peqBell1Freq", 250.0f);
    Value& bell1Gain   = addValue ("peqBell1Gain", 0.0f);
    Value& bell1Q      = addValue ("peqBell1Q", 1.0f);
    Value& bell2Freq   = addValue ("peqBell2Freq", 1000.0f);
    Value& bell2Gain   = addValue ("peqBell2Gain", 0.0f);
    Value& bell2Q      = addValue ("peqBell2Q", 1.0f);
    Value& bell3Freq   = addValue ("peqBell3Freq", 4000.0f);
    Value& bell3Gain   = addValue ("peqBell3Gain", 0.0f);
    Value& bell3Q      = addValue ("peqBell3Q", 1.0f);
    Value& highFreq    = addValue ("peqHighFreq", 8000.0f);
    Value& highGain    = addValue ("peqHighGain", 0.0f);
    Value& highCutOn   = addChoice ("peqHighCutOn", 0.0f);
    Value& highCutFreq = addValue ("peqHighCutFreq", 18000.0f);

    // The values as EQ settings (for the analysis, which plans moves on the same curve):
    // what the knobs are set to, not where a glide towards them has got to.
    ParametricEq::Settings getSettings() const noexcept
    {
        return makeSettings ([] (const Value& v) { return v.load (std::memory_order_relaxed); });
    }

private:
    template <typename Read>
    ParametricEq::Settings makeSettings (Read read) const noexcept
    {
        ParametricEq::Settings s;
        s.lowCutOn    = read (lowCutOn) >= 0.5f;
        s.lowCutHz    = read (lowCutFreq);
        s.lowShelfHz  = read (lowFreq);
        s.lowShelfDb  = read (lowGain);
        s.bells[0]    = { read (bell1Freq), read (bell1Gain), read (bell1Q) };
        s.bells[1]    = { read (bell2Freq), read (bell2Gain), read (bell2Q) };
        s.bells[2]    = { read (bell3Freq), read (bell3Gain), read (bell3Q) };
        s.highShelfHz = read (highFreq);
        s.highShelfDb = read (highGain);
        s.highCutOn   = read (highCutOn) >= 0.5f;
        s.highCutHz   = read (highCutFreq);
        return s;
    }

    void prepareModule (const juce::dsp::ProcessSpec& spec) override { eq.prepare (spec); }
    void resetModule() override                                      { eq.reset(); }
    void update() noexcept override                                  { eq.setSettings (makeSettings ([] (const Value& v) { return get (v); })); }
    void render (const Context& c) noexcept override                 { eq.process (c); }

    ParametricEq eq;
};

class Compressor final : public Module
{
public:
    Compressor() : Module (ModuleType::compressor) {}

    Value& on        = addSwitch ("compOn", true);
    Value& threshold = addValue ("compThreshold", -18.0f);
    Value& ratio     = addValue ("compRatio", 3.0f);
    Value& attack    = addValue ("compAttack", 10.0f);  // ms
    Value& release   = addValue ("compRelease", 120.0f); // ms
    Value& makeup    = addValue ("compMakeup", 0.0f);    // dB

private:
    void prepareModule (const juce::dsp::ProcessSpec& spec) override { compressor.prepare (spec); }

    void resetModule() override { compressor.reset(); }

    void update() noexcept override
    {
        compressor.setThreshold (get (threshold));
        compressor.setRatio (get (ratio));
        compressor.setAttack (juce::jmax (0.1f, get (attack)));
        compressor.setRelease (juce::jmax (1.0f, get (release)));
        makeupGain = juce::Decibels::decibelsToGain (get (makeup));
    }

    void render (const Context& c) noexcept override
    {
        compressor.process (c);
        if (makeupGain != 1.0f)
            c.getOutputBlock().multiplyBy (makeupGain);
    }

    juce::dsp::Compressor<float> compressor;
    float makeupGain = 1.0f;
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
    Value& type      = addChoice ("filterType", 0.0f);
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
    Value& pingPong = addChoice ("delayPingPong", 0.0f);

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
// Resonance suppressor (Smooth.h): narrow peaks that stick out of the sound from moment
// to moment are turned down while they stick out. Delta, stereo and resolution are
// choices: they don't switch the module on.
class Smooth final : public Module
{
public:
    Smooth() : Module (ModuleType::smooth) {}

    Value& on         = addSwitch ("smoothOn");
    Value& depth      = addValue ("smoothDepth", 9.0f);        // dB
    Value& threshold  = addValue ("smoothThreshold", 6.0f);    // dB over the envelope
    Value& sharpness  = addValue ("smoothSharpness", 50.0f);   // %
    Value& tilt       = addValue ("smoothFocus", 0.0f);        // %, -100 lows .. +100 highs
    Value& lowHz      = addValue ("smoothLowHz", 20.0f);
    Value& highHz     = addValue ("smoothHighHz", 20000.0f);
    Value& attack     = addValue ("smoothAttack", 5.0f);       // ms
    Value& release    = addValue ("smoothRelease", 100.0f);    // ms
    Value& mix        = addValue ("smoothMix", 100.0f);        // %
    Value& delta      = addChoice ("smoothDelta", 0.0f);
    Value& stereo     = addChoice ("smoothStereo", 0.0f);      // ResonanceSuppressor::Stereo
    Value& resolution = addChoice ("smoothResolution", 1.0f);  // ResonanceSuppressor::Resolution

    // One FFT frame while it's on (5, 11 or 21 ms, by Resolution).
    int getLatencySamples() const noexcept override
    {
        if (! flag (on))
            return 0;

        const auto r = (ResonanceSuppressor::Resolution) juce::jlimit (0, 2, index (resolution));
        return ResonanceSuppressor::getFrameSize (r, rate.load (std::memory_order_relaxed));
    }

    const ResonanceSuppressor& getSuppressor() const noexcept { return suppressor; }

    // reduction: the cut per band right now (dB, deepest bin, 40 Hz..18 kHz log-spaced),
    // deepest: the deepest cut since the last reading.
    void addReadings (juce::DynamicObject& readings) override
    {
        juce::Array<juce::var> cut;
        for (auto& band : suppressor.reductionBandsDb)
            cut.add (std::round (band.load (std::memory_order_relaxed) * 10.0f) / 10.0f);

        readings.setProperty ("reduction", cut);
        readings.setProperty ("deepest", std::round (suppressor.takeDeepestCut() * 10.0f) / 10.0f);
    }

private:
    void prepareModule (const juce::dsp::ProcessSpec& spec) override
    {
        rate.store (spec.sampleRate, std::memory_order_relaxed);
        suppressor.setSettings (makeSettings());
        suppressor.prepare (spec);
    }

    void resetModule() override { suppressor.reset(); }

    void update() noexcept override { suppressor.setSettings (makeSettings()); }

    ResonanceSuppressor::Settings makeSettings() const noexcept
    {
        ResonanceSuppressor::Settings s;
        s.depthDb = get (depth);
        s.thresholdDb = get (threshold);
        s.sharpness = fraction (sharpness);
        s.tilt = juce::jlimit (-1.0f, 1.0f, fraction (tilt));
        s.lowHz = get (lowHz);
        s.highHz = get (highHz);
        s.attackMs = get (attack);
        s.releaseMs = get (release);
        s.mix = fraction (mix);
        s.delta = flag (delta);
        s.stereo = index (stereo) == 1 ? ResonanceSuppressor::Stereo::midSide : ResonanceSuppressor::Stereo::linked;
        s.resolution = (ResonanceSuppressor::Resolution) juce::jlimit (0, 2, index (resolution));
        return s;
    }

    void render (const Context& c) noexcept override { suppressor.process (c); }

    ResonanceSuppressor suppressor;
    std::atomic<double> rate { 48000.0 };
};

// A speaker cabinet (Cabinet.h): one of the built-in models, or an impulse response file
// the user loads (Convolver.h), for a guitar or bass played straight in or after an amp.
// Model and mic are choices: they don't switch the module on.
class Cab final : public Module
{
public:
    Cab() : Module (ModuleType::cab) {}

    Value& on      = addSwitch ("cabOn");
    Value& model   = addChoice ("cabModel", (float) Cabinet::Model::closed4x12); // last entry: your IR file
    Value& mic     = addChoice ("cabMic", (float) Cabinet::Mic::offAxis);
    Value& lowCut  = addValue ("cabLowCut", 20.0f);    // Hz
    Value& highCut = addValue ("cabHighCut", 20000.0f); // Hz
    Value& mix     = addValue ("cabMix", 100.0f);      // %

    static constexpr int fileModel = Cabinet::numModels;

    static juce::StringArray getModelChoices()
    {
        auto names = Cabinet::getModelNames();
        names.add ("Your IR file");
        return names;
    }

    // An impulse response longer than this is cut: a cabinet's sound is in its first
    // few hundred milliseconds, the rest is the room it was measured in.
    static constexpr double maxIrSeconds = 0.5;

    bool usesFile() const noexcept override              { return true; }
    juce::String getFileWildcard() const override        { return ImpulseFile::getWildcard(); }
    juce::File getFile() const override                  { return convolver.getFile(); }
    FileChoice getFileChoice() const override            { return { "cabModel", (float) fileModel }; }

    juce::String loadFile (const juce::File& file) override
    {
        if (file != juce::File())
            if (const auto error = ImpulseFile::check (file); error.isNotEmpty())
                return error;

        convolver.setFile (file);
        fileResponse = file != juce::File() ? measureResponse (file) : std::vector<float>();
        return {};
    }

    // response: the cabinet's frequency response (dB at pictureBands log-spaced points,
    // 40 Hz - 16 kHz), the built-in model's or the loaded file's; file: its name.
    void addReadings (juce::DynamicObject& readings) override
    {
        juce::Array<juce::var> response;
        const auto usingFile = index (model) >= fileModel;

        if (usingFile)
        {
            for (auto db : fileResponse)
                response.add (std::round (db * 10.0f) / 10.0f);
        }
        else
        {
            picture.setParameters (getModel(), getMic(), get (lowCut), get (highCut));
            for (int i = 0; i < pictureBands; ++i)
                response.add (std::round (picture.getMagnitudeDb (getPictureHz (i)) * 10.0) / 10.0);
        }

        readings.setProperty ("response", response);
        readings.setProperty ("file", getFile().getFileName());
    }

    static constexpr int pictureBands = 48;
    static double getPictureHz (int i) { return 40.0 * std::pow (400.0, (double) i / (pictureBands - 1)); }

private:
    Cabinet::Model getModel() const noexcept { return (Cabinet::Model) juce::jlimit (0, Cabinet::numModels - 1, index (model)); }
    Cabinet::Mic getMic() const noexcept     { return (Cabinet::Mic) juce::jlimit (0, 2, index (mic)); }

    void prepareModule (const juce::dsp::ProcessSpec& spec) override
    {
        picture.prepare (spec);
        cabinet.prepare (spec);
        dry.setSize ((int) spec.numChannels, (int) spec.maximumBlockSize, false, false, true);
        update();
        convolver.prepare (spec);
    }

    void resetModule() override
    {
        cabinet.reset();
        convolver.reset();
    }

    void update() noexcept override
    {
        cabinet.setParameters (getModel(), getMic(), get (lowCut), get (highCut));
        convolver.setSource (Convolver::fileSource);
        convolver.setWetShape (0.0f, get (lowCut), get (highCut));
        convolver.setMix (fraction (mix), 1.0f - fraction (mix));
    }

    void render (const Context& c) noexcept override
    {
        if (index (model) >= fileModel)
        {
            convolver.process (c);
            return;
        }

        auto& block = c.getOutputBlock();
        const auto numSamples = (int) block.getNumSamples();
        const auto wet = fraction (mix);
        const auto keepDry = wet < 0.999f && numSamples <= dry.getNumSamples();
        const auto numChannels = juce::jmin ((int) block.getNumChannels(), dry.getNumChannels());

        if (keepDry)
            for (int ch = 0; ch < numChannels; ++ch)
                dry.copyFrom (ch, 0, block.getChannelPointer ((size_t) ch), numSamples);

        cabinet.process (c);

        if (keepDry)
            for (int ch = 0; ch < numChannels; ++ch)
            {
                auto* out = block.getChannelPointer ((size_t) ch);
                juce::FloatVectorOperations::multiply (out, wet, numSamples);
                juce::FloatVectorOperations::addWithMultiply (out, dry.getReadPointer (ch), 1.0f - wet, numSamples);
            }
    }

    // The file's response for the picture: its first part, first channel, through an
    // FFT, averaged into the picture's bands and centred on its 100 Hz - 4 kHz average.
    static std::vector<float> measureResponse (const juce::File& file)
    {
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        const std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
        if (reader == nullptr)
            return {};

        constexpr int order = 14, size = 1 << order;
        std::vector<float> data ((size_t) size * 2, 0.0f);
        juce::AudioBuffer<float> ir (1, (int) juce::jmin ((juce::int64) size, reader->lengthInSamples));
        reader->read (&ir, 0, ir.getNumSamples(), 0, true, false);
        std::copy_n (ir.getReadPointer (0), ir.getNumSamples(), data.begin());

        juce::dsp::FFT fft (order);
        fft.performFrequencyOnlyForwardTransform (data.data());

        std::vector<float> bands;
        double centre = 0.0;
        int counted = 0;
        for (int i = 0; i < pictureBands; ++i)
        {
            const auto lo = getPictureHz (i) / std::pow (400.0, 0.5 / (pictureBands - 1));
            const auto hi = getPictureHz (i) * std::pow (400.0, 0.5 / (pictureBands - 1));
            double power = 0.0;
            int bins = 0;
            for (auto bin = juce::jmax (1, (int) (lo / reader->sampleRate * size)); bin <= juce::jmin (size / 2, (int) std::ceil (hi / reader->sampleRate * size)); ++bin, ++bins)
                power += (double) data[(size_t) bin] * data[(size_t) bin];

            const auto db = (float) (10.0 * std::log10 (juce::jmax (1.0e-12, power / juce::jmax (1, bins))));
            bands.push_back (db);
            if (getPictureHz (i) >= 100.0 && getPictureHz (i) <= 4000.0)
            {
                centre += db;
                ++counted;
            }
        }

        for (auto& db : bands)
            db = juce::jlimit (-60.0f, 24.0f, db - (float) (centre / juce::jmax (1, counted)));
        return bands;
    }

    Cabinet cabinet, picture; // picture: a copy on the message thread for the card
    Convolver convolver { 128, maxIrSeconds, true };
    juce::AudioBuffer<float> dry;
    std::vector<float> fileResponse; // message thread
};

// A convolution reverb (Convolver.h): built-in rooms made by MicFX, or a room, hall or
// plate impulse response file the user loads. Pre-delay and low/high cut shape the reverb
// only; the dry sound passes untouched.
class IrReverb final : public Module
{
public:
    IrReverb() : Module (ModuleType::irReverb) { convolver.setBuiltIns (spaces::getSpaceImpulses()); }

    Value& on       = addSwitch ("irvOn");
    Value& space    = addChoice ("irvSpace", 1.0f); // spaces::getSpaces(), last entry: your IR file
    Value& mix      = addValue ("irvMix", 25.0f);   // %
    Value& preDelay = addValue ("irvPreDelay", 0.0f); // ms
    Value& lowCut   = addValue ("irvLowCut", 150.0f);  // Hz
    Value& highCut  = addValue ("irvHighCut", 12000.0f); // Hz

    static int getFileSpace() { return (int) spaces::getSpaces().size(); }

    static juce::StringArray getSpaceChoices()
    {
        juce::StringArray names;
        for (const auto& s : spaces::getSpaces())
            names.add (s.name);
        names.add ("Your IR file");
        return names;
    }

    bool usesFile() const noexcept override              { return true; }
    juce::String getFileWildcard() const override        { return ImpulseFile::getWildcard(); }
    juce::File getFile() const override                  { return convolver.getFile(); }
    FileChoice getFileChoice() const override            { return { "irvSpace", (float) getFileSpace() }; }

    juce::String loadFile (const juce::File& file) override
    {
        if (file != juce::File())
            if (const auto error = ImpulseFile::check (file); error.isNotEmpty())
                return error;

        convolver.setFile (file);
        return {};
    }

    void addReadings (juce::DynamicObject& readings) override
    {
        readings.setProperty ("file", getFile().getFileName());
        readings.setProperty ("seconds", std::round (convolver.getCurrentIrSize() / juce::jmax (1.0, getSampleRate()) * 100.0) / 100.0);
    }

private:
    void prepareModule (const juce::dsp::ProcessSpec& spec) override
    {
        update();
        convolver.prepare (spec);
    }

    void resetModule() override { convolver.reset(); }

    void update() noexcept override
    {
        const auto chosen = index (space);
        convolver.setSource (chosen >= getFileSpace() ? Convolver::fileSource : juce::jmax (0, chosen));
        convolver.setWetShape (get (preDelay), get (lowCut), get (highCut));
        // An equal-power crossfade, as the other reverb: 0% is the untouched dry sound.
        // (The rooms carry more energy than the dry sound; 0.7 brings them level.)
        const auto angle = fraction (mix) * juce::MathConstants<float>::halfPi;
        convolver.setMix (0.7f * std::sin (angle), std::cos (angle));
    }

    void render (const Context& c) noexcept override { convolver.process (c); }

    Convolver convolver { 512, ImpulseFile::maxSeconds, false };
};

}

namespace fx
{
inline std::unique_ptr<Module> createModule (ModuleType type)
{
    switch (type)
    {
        case ModuleType::highPass:   return std::make_unique<modules::HighPass>();
        case ModuleType::denoise:    return std::make_unique<modules::Denoise>();
        case ModuleType::gate:       return std::make_unique<modules::Gate>();
        case ModuleType::pitch:      return std::make_unique<modules::Pitch>();
        case ModuleType::eq:         return std::make_unique<modules::Eq>();
        case ModuleType::paraEq:     return std::make_unique<modules::ParaEq>();
        case ModuleType::smooth:     return std::make_unique<modules::Smooth>();
        case ModuleType::compressor: return std::make_unique<modules::Compressor>();
        case ModuleType::autoLevel:  return std::make_unique<modules::AutoLevel>();
        case ModuleType::deEsser:    return std::make_unique<modules::DeEsser>();
        case ModuleType::exciter:    return std::make_unique<modules::Exciter>();
        case ModuleType::saturation: return std::make_unique<modules::Saturation>();
        case ModuleType::radio:      return std::make_unique<modules::Radio>();
        case ModuleType::bitcrusher: return std::make_unique<modules::Bitcrusher>();
        case ModuleType::robot:      return std::make_unique<modules::Robot>();
        case ModuleType::filter:     return std::make_unique<modules::Filter>();
        case ModuleType::autoWah:    return std::make_unique<modules::AutoWah>();
        case ModuleType::cab:        return std::make_unique<modules::Cab>();
        case ModuleType::tremolo:    return std::make_unique<modules::Tremolo>();
        case ModuleType::vibrato:    return std::make_unique<modules::Vibrato>();
        case ModuleType::chorus:     return std::make_unique<modules::Chorus>();
        case ModuleType::flanger:    return std::make_unique<modules::Flanger>();
        case ModuleType::phaser:     return std::make_unique<modules::Phaser>();
        case ModuleType::doubler:    return std::make_unique<modules::Doubler>();
        case ModuleType::echo:       return std::make_unique<modules::Echo>();
        case ModuleType::reverb:     return std::make_unique<modules::Reverb>();
        case ModuleType::irReverb:   return std::make_unique<modules::IrReverb>();
        case ModuleType::width:      return std::make_unique<modules::Width>();
        case ModuleType::multiband:  return std::make_unique<modules::Multiband>();
        case ModuleType::limiter:    return std::make_unique<modules::Limiter>();
        case ModuleType::masterEq:   return std::make_unique<modules::MasterEq>();
        case ModuleType::imager:     return std::make_unique<modules::Imager>();
        case ModuleType::loudness:   return std::make_unique<modules::Loudness>();
        case ModuleType::transient:  return std::make_unique<modules::Transient>();
        case ModuleType::dynamicEq:  return std::make_unique<modules::DynamicEq>();
    }

    jassertfalse;
    return {};
}

}
