#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

using FloatParam  = juce::AudioParameterFloat;
using BoolParam   = juce::AudioParameterBool;
using ChoiceParam = juce::AudioParameterChoice;

// Every parameter the processor exposes, grouped by module in signal-chain order.
// The string IDs (see Parameters.cpp) double as relay names in the web UI
// (ui/modules.js) and as keys in presets (Presets.cpp).
struct Parameters
{
    explicit Parameters (juce::AudioProcessorValueTreeState::ParameterLayout&);

    FloatParam& inputGain;

    struct HighPass   { BoolParam& on; FloatParam& freq; } highPass;
    struct Denoise    { BoolParam& on; FloatParam& reduction; FloatParam& threshold; BoolParam& adaptive; BoolParam& learn; } denoise;
    struct Gate       { BoolParam& on; FloatParam& threshold; } gate;
    struct Autotune   { BoolParam& on; ChoiceParam& key; ChoiceParam& scale; FloatParam& speed; FloatParam& amount; } autotune;
    struct PitchShift { BoolParam& on; FloatParam& semitones; FloatParam& formant; } pitchShift;
    struct Harmonizer { BoolParam& on; FloatParam& voice1; FloatParam& voice2; FloatParam& mix; } harmonizer;
    struct Eq         { BoolParam& on; FloatParam& low; FloatParam& midFreq; FloatParam& midGain; FloatParam& high; } eq;
    struct Compressor { BoolParam& on; FloatParam& threshold; FloatParam& ratio; } compressor;
    struct DeEsser    { BoolParam& on; FloatParam& threshold; FloatParam& freq; } deEsser;
    struct Exciter    { BoolParam& on; FloatParam& freq; FloatParam& amount; } exciter;
    struct Saturation { BoolParam& on; FloatParam& drive; FloatParam& mix; } saturation;
    struct Radio      { BoolParam& on; FloatParam& low; FloatParam& high; } radio;
    struct Bitcrusher { BoolParam& on; FloatParam& bits; FloatParam& rate; FloatParam& mix; } bitcrusher;
    struct Robot      { BoolParam& on; FloatParam& freq; FloatParam& mix; } robot;
    struct Filter     { BoolParam& on; ChoiceParam& type; FloatParam& cutoff; FloatParam& resonance; } filter;
    struct AutoWah    { BoolParam& on; FloatParam& sensitivity; FloatParam& freq; FloatParam& mix; } autoWah;
    struct Tremolo    { BoolParam& on; FloatParam& rate; FloatParam& depth; } tremolo;
    struct Vibrato    { BoolParam& on; FloatParam& rate; FloatParam& depth; } vibrato;
    struct Chorus     { BoolParam& on; FloatParam& rate; FloatParam& depth; FloatParam& mix; } chorus;
    struct Flanger    { BoolParam& on; FloatParam& rate; FloatParam& depth; FloatParam& feedback; FloatParam& mix; } flanger;
    struct Phaser     { BoolParam& on; FloatParam& rate; FloatParam& depth; FloatParam& feedback; FloatParam& mix; } phaser;
    struct Doubler    { BoolParam& on; FloatParam& mix; FloatParam& spread; } doubler;
    struct Echo       { BoolParam& on; FloatParam& time; FloatParam& feedback; FloatParam& mix; BoolParam& pingPong; } echo;
    struct Reverb     { BoolParam& on; FloatParam& mix; FloatParam& size; FloatParam& damping; FloatParam& width; } reverb;
    struct Width      { BoolParam& on; FloatParam& amount; } width;

    FloatParam& outputGain;
};
