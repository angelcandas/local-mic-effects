#include "Parameters.h"
#include "dsp/PitchEngine.h"
#include "dsp/FxModule.h"
#include "dsp/FxModules.h"

namespace
{
    using Layout = juce::AudioProcessorValueTreeState::ParameterLayout;

    template <typename Param, typename... Args>
    Param& add (Layout& layout, const juce::String& id, const juce::String& name, Args&&... args)
    {
        auto param = std::make_unique<Param> (juce::ParameterID { id, 1 }, name, std::forward<Args> (args)...);
        auto& ref = *param;
        layout.add (std::move (param));
        return ref;
    }

    // `centre` puts that value at the middle of the knob (log-like feel for Hz and ms).
    FloatParam& number (Layout& layout, const juce::String& id, const juce::String& name, float min, float max, float step,
                        float defaultValue, const char* label, float centre = 0.0f)
    {
        juce::NormalisableRange<float> range { min, max, step };
        if (centre > min && centre < max)
            range.setSkewForCentre (centre);

        return add<FloatParam> (layout, id, name, range, defaultValue,
                                juce::AudioParameterFloatAttributes{}.withLabel (label));
    }

    BoolParam& toggle (Layout& layout, const juce::String& id, const juce::String& name, bool defaultValue = false)
    {
        return add<BoolParam> (layout, id, name, defaultValue);
    }

    ChoiceParam& choice (Layout& layout, const juce::String& id, const juce::String& name, const juce::StringArray& choices, int defaultIndex = 0)
    {
        return add<ChoiceParam> (layout, id, name, choices, defaultIndex);
    }

    FloatParam& decibels (Layout& layout, const juce::String& id, const juce::String& name, float min, float max, float defaultValue)
    {
        return number (layout, id, name, min, max, 0.1f, defaultValue, "dB");
    }

    FloatParam& percent (Layout& layout, const juce::String& id, const juce::String& name, float defaultValue, float max = 100.0f)
    {
        return number (layout, id, name, 0.0f, max, 1.0f, defaultValue, "%");
    }

    FloatParam& hertz (Layout& layout, const juce::String& id, const juce::String& name, float min, float max, float defaultValue, float centre)
    {
        return number (layout, id, name, min, max, 0.01f, defaultValue, "Hz", centre);
    }

    // One bell of the parametric EQ: "peqBell<n>Freq", "...Gain" and "...Q".
    Parameters::ParaEq::Bell bell (Layout& layout, int n, float defaultFreq)
    {
        const auto id = "peqBell" + juce::String (n), name = "PEQ Bell " + juce::String (n);
        return { hertz (layout, id + "Freq", name + " Freq", 20.0f, 20000.0f, defaultFreq, 1000.0f),
                 decibels (layout, id + "Gain", name + " Gain", -18.0f, 18.0f, 0.0f),
                 number (layout, id + "Q", name + " Q", 0.1f, 18.0f, 0.01f, 1.0f, "", 1.5f) };
    }

    FloatParam& semitones (Layout& layout, const juce::String& id, const juce::String& name, float range, float step, float defaultValue)
    {
        return number (layout, id, name, -range, range, step, defaultValue, "st");
    }
}

Parameters::Parameters (Layout& l)
    : inputGain  (decibels (l, "inputGain", "Input Gain", -24.0f, 24.0f, 0.0f)),

      highPass   { toggle (l, "hpfOn", "High-Pass On", true),
                   hertz (l, "hpfFreq", "High-Pass Freq", 20.0f, 400.0f, 80.0f, 100.0f) },

      gate       { toggle (l, "gateOn", "Gate On", true),
                   decibels (l, "gateThreshold", "Gate Threshold", -80.0f, 0.0f, -55.0f) },

      autotune   { toggle (l, "tuneOn", "Autotune On"),
                   choice (l, "tuneKey", "Autotune Key", fx::PitchEngine::getKeyNames()),
                   choice (l, "tuneScale", "Autotune Scale", fx::PitchEngine::getScaleNames()),
                   number (l, "tuneSpeed", "Autotune Speed", 0.0f, 200.0f, 1.0f, 20.0f, "ms", 40.0f),
                   percent (l, "tuneAmount", "Autotune Amount", 100.0f) },

      pitchShift { toggle (l, "pitchOn", "Pitch Shift On"),
                   semitones (l, "pitchShift", "Pitch", 12.0f, 0.1f, 0.0f),
                   semitones (l, "pitchFormant", "Formant", 12.0f, 0.1f, 0.0f) },

      harmonizer { toggle (l, "harmonyOn", "Harmonizer On"),
                   semitones (l, "harmonyVoice1", "Harmony Voice 1", 12.0f, 1.0f, 4.0f),
                   semitones (l, "harmonyVoice2", "Harmony Voice 2", 12.0f, 1.0f, 7.0f),
                   percent (l, "harmonyMix", "Harmony Mix", 50.0f) },

      eq         { toggle (l, "eqOn", "EQ On"),
                   decibels (l, "eqLow", "EQ Low", -12.0f, 12.0f, 0.0f),
                   hertz (l, "eqMidFreq", "EQ Mid Freq", 200.0f, 5000.0f, 2500.0f, 1000.0f),
                   decibels (l, "eqMidGain", "EQ Mid Gain", -12.0f, 12.0f, 0.0f),
                   decibels (l, "eqHigh", "EQ High", -12.0f, 12.0f, 0.0f) },

      paraEq     { toggle (l, "peqOn", "Parametric EQ On"),
                   toggle (l, "peqLowCutOn", "PEQ Low Cut On"),
                   hertz (l, "peqLowCutFreq", "PEQ Low Cut", 20.0f, 1000.0f, 80.0f, 120.0f),
                   hertz (l, "peqLowFreq", "PEQ Low Freq", 30.0f, 1000.0f, 120.0f, 150.0f),
                   decibels (l, "peqLowGain", "PEQ Low Gain", -18.0f, 18.0f, 0.0f),
                   bell (l, 1, 250.0f), bell (l, 2, 1000.0f), bell (l, 3, 4000.0f),
                   hertz (l, "peqHighFreq", "PEQ High Freq", 1000.0f, 20000.0f, 8000.0f, 6000.0f),
                   decibels (l, "peqHighGain", "PEQ High Gain", -18.0f, 18.0f, 0.0f),
                   toggle (l, "peqHighCutOn", "PEQ High Cut On"),
                   hertz (l, "peqHighCutFreq", "PEQ High Cut", 1000.0f, 20000.0f, 18000.0f, 6000.0f) },

      compressor { toggle (l, "compOn", "Compressor On", true),
                   decibels (l, "compThreshold", "Comp Threshold", -60.0f, 0.0f, -18.0f),
                   number (l, "compRatio", "Comp Ratio", 1.0f, 20.0f, 0.1f, 3.0f, ":1", 4.0f) },

      deEsser    { toggle (l, "deessOn", "De-esser On"),
                   decibels (l, "deessThreshold", "De-esser Threshold", -60.0f, 0.0f, -30.0f),
                   hertz (l, "deessFreq", "De-esser Freq", 3000.0f, 10000.0f, 6000.0f, 5500.0f) },

      exciter    { toggle (l, "exciterOn", "Exciter On"),
                   hertz (l, "exciterFreq", "Exciter Freq", 1000.0f, 10000.0f, 3500.0f, 3000.0f),
                   percent (l, "exciterAmount", "Exciter Amount", 30.0f) },

      saturation { toggle (l, "satOn", "Saturation On"),
                   number (l, "satDrive", "Drive", 0.0f, 30.0f, 0.1f, 12.0f, "dB"),
                   percent (l, "satMix", "Saturation Mix", 100.0f) },

      radio      { toggle (l, "radioOn", "Radio On"),
                   hertz (l, "radioLow", "Radio Low Cut", 100.0f, 1500.0f, 400.0f, 400.0f),
                   hertz (l, "radioHigh", "Radio High Cut", 1000.0f, 8000.0f, 3200.0f, 3000.0f) },

      bitcrusher { toggle (l, "crushOn", "Bitcrusher On"),
                   number (l, "crushBits", "Bits", 2.0f, 16.0f, 0.1f, 8.0f, "bit"),
                   hertz (l, "crushRate", "Sample Rate", 1000.0f, 24000.0f, 8000.0f, 6000.0f),
                   percent (l, "crushMix", "Bitcrusher Mix", 100.0f) },

      robot      { toggle (l, "robotOn", "Robot On"),
                   hertz (l, "robotFreq", "Robot Freq", 10.0f, 1000.0f, 50.0f, 80.0f),
                   percent (l, "robotMix", "Robot Mix", 100.0f) },

      filter     { toggle (l, "filterOn", "Filter On"),
                   choice (l, "filterType", "Filter Type", { "Low-pass", "High-pass", "Band-pass" }),
                   hertz (l, "filterCutoff", "Filter Cutoff", 20.0f, 20000.0f, 1200.0f, 1000.0f),
                   percent (l, "filterResonance", "Filter Resonance", 30.0f) },

      autoWah    { toggle (l, "wahOn", "Auto-wah On"),
                   percent (l, "wahSensitivity", "Wah Sensitivity", 60.0f),
                   hertz (l, "wahFreq", "Wah Freq", 200.0f, 2000.0f, 400.0f, 600.0f),
                   percent (l, "wahMix", "Wah Mix", 100.0f) },

      tremolo    { toggle (l, "tremoloOn", "Tremolo On"),
                   hertz (l, "tremoloRate", "Tremolo Rate", 0.5f, 20.0f, 6.0f, 5.0f),
                   percent (l, "tremoloDepth", "Tremolo Depth", 60.0f) },

      vibrato    { toggle (l, "vibratoOn", "Vibrato On"),
                   hertz (l, "vibratoRate", "Vibrato Rate", 0.5f, 12.0f, 5.5f, 5.0f),
                   percent (l, "vibratoDepth", "Vibrato Depth", 40.0f) },

      chorus     { toggle (l, "chorusOn", "Chorus On"),
                   hertz (l, "chorusRate", "Chorus Rate", 0.05f, 5.0f, 0.8f, 1.0f),
                   percent (l, "chorusDepth", "Chorus Depth", 35.0f),
                   percent (l, "chorusMix", "Chorus Mix", 50.0f) },

      flanger    { toggle (l, "flangerOn", "Flanger On"),
                   hertz (l, "flangerRate", "Flanger Rate", 0.05f, 5.0f, 0.25f, 0.5f),
                   percent (l, "flangerDepth", "Flanger Depth", 70.0f),
                   percent (l, "flangerFeedback", "Flanger Feedback", 60.0f, 95.0f),
                   percent (l, "flangerMix", "Flanger Mix", 50.0f) },

      phaser     { toggle (l, "phaserOn", "Phaser On"),
                   hertz (l, "phaserRate", "Phaser Rate", 0.05f, 5.0f, 0.4f, 0.5f),
                   percent (l, "phaserDepth", "Phaser Depth", 70.0f),
                   percent (l, "phaserFeedback", "Phaser Feedback", 50.0f, 95.0f),
                   percent (l, "phaserMix", "Phaser Mix", 50.0f) },

      doubler    { toggle (l, "doublerOn", "Doubler On"),
                   percent (l, "doublerMix", "Doubler Mix", 50.0f),
                   percent (l, "doublerSpread", "Doubler Spread", 80.0f) },

      echo       { toggle (l, "delayOn", "Echo On"),
                   number (l, "delayTime", "Echo Time", 20.0f, 1500.0f, 1.0f, 350.0f, "ms", 300.0f),
                   percent (l, "delayFeedback", "Echo Feedback", 35.0f, 95.0f),
                   percent (l, "delayMix", "Echo Mix", 25.0f),
                   toggle (l, "delayPingPong", "Echo Ping-Pong") },

      reverb     { toggle (l, "reverbOn", "Reverb On", true),
                   percent (l, "reverbMix", "Reverb Mix", 25.0f),
                   percent (l, "reverbSize", "Reverb Size", 50.0f),
                   percent (l, "reverbDamping", "Reverb Damping", 50.0f),
                   percent (l, "reverbWidth", "Reverb Width", 100.0f) },

      width      { toggle (l, "widthOn", "Stereo Width On"),
                   percent (l, "widthAmount", "Stereo Width", 150.0f, 200.0f) },

      outputGain (decibels (l, "outputGain", "Output Gain", -24.0f, 24.0f, 0.0f))
{
    // Every effect's own input and output gain ("compressorInGain"...), after the rest
    // so hosts keep the parameter order they already had.
    const auto moduleGains = [&l] (fx::ModuleType type)
    {
        const juce::String key (fx::getModuleKey (type));
        juce::String name;
        for (auto c : key)
            name << (juce::CharacterFunctions::isUpperCase (c) ? juce::String (" ") : juce::String()) << juce::String::charToString (c);
        name = name.substring (0, 1).toUpperCase() + name.substring (1);

        decibels (l, key + "InGain", name + " In Gain", -fx::Module::maxGainDb, fx::Module::maxGainDb, 0.0f);
        decibels (l, key + "OutGain", name + " Out Gain", -fx::Module::maxGainDb, fx::Module::maxGainDb, 0.0f);
    };

    // (The denoiser, auto level, the resonance suppressor, the cabinet, the IR reverb, the amp,
    // the opto compressor and the plate came later: their gains are at the end with their other parameters.)
    for (int i = 0; i < fx::numModuleTypes; ++i)
        if (const auto type = (fx::ModuleType) i;
            type != fx::ModuleType::denoise && type != fx::ModuleType::autoLevel && type != fx::ModuleType::smooth
            && type != fx::ModuleType::cab && type != fx::ModuleType::irReverb && type != fx::ModuleType::amp
            && type != fx::ModuleType::opto && type != fx::ModuleType::plate)
            moduleGains (type);

    // Mastering effects (Mastering.h), added last so hosts keep the parameter order
    // they already had (their In/Out gains are in the loop above).
    toggle (l, "mbOn", "Multiband On");
    hertz (l, "mbLowFreq", "Multiband Low/Mid", 40.0f, 1000.0f, 200.0f, 200.0f);
    hertz (l, "mbHighFreq", "Multiband Mid/High", 1000.0f, 12000.0f, 3000.0f, 3000.0f);
    decibels (l, "mbLowThreshold", "Multiband Low Threshold", -48.0f, 0.0f, -18.0f);
    decibels (l, "mbMidThreshold", "Multiband Mid Threshold", -48.0f, 0.0f, -18.0f);
    decibels (l, "mbHighThreshold", "Multiband High Threshold", -48.0f, 0.0f, -18.0f);
    number (l, "mbRatio", "Multiband Ratio", 1.0f, 10.0f, 0.1f, 2.5f, ":1", 3.0f);
    number (l, "mbAttack", "Multiband Attack", 1.0f, 100.0f, 0.1f, 15.0f, "ms", 15.0f);
    number (l, "mbRelease", "Multiband Release", 20.0f, 1000.0f, 1.0f, 180.0f, "ms", 180.0f);
    decibels (l, "mbMakeup", "Multiband Makeup", -6.0f, 12.0f, 0.0f);

    toggle (l, "limOn", "Limiter On");
    decibels (l, "limDrive", "Limiter Drive", 0.0f, 24.0f, 0.0f);
    decibels (l, "limCeiling", "Limiter Ceiling", -12.0f, 0.0f, -1.0f);
    number (l, "limRelease", "Limiter Release", 10.0f, 1000.0f, 1.0f, 120.0f, "ms", 120.0f);

    toggle (l, "meqOn", "Mastering EQ On");
    hertz (l, "meqLowCut", "Mastering EQ Low Cut", 10.0f, 200.0f, 20.0f, 40.0f);
    hertz (l, "meqLowFreq", "Mastering EQ Low Freq", 30.0f, 300.0f, 100.0f, 100.0f);
    decibels (l, "meqLowGain", "Mastering EQ Low", -12.0f, 12.0f, 0.0f);
    hertz (l, "meqMidFreq", "Mastering EQ Mid Freq", 200.0f, 8000.0f, 1000.0f, 1000.0f);
    decibels (l, "meqMidGain", "Mastering EQ Mid", -12.0f, 12.0f, 0.0f);
    hertz (l, "meqHighFreq", "Mastering EQ High Freq", 2000.0f, 16000.0f, 8000.0f, 6000.0f);
    decibels (l, "meqHighGain", "Mastering EQ High", -12.0f, 12.0f, 0.0f);
    decibels (l, "meqAirGain", "Mastering EQ Air", -12.0f, 12.0f, 0.0f);

    toggle (l, "imgOn", "Imager On");
    percent (l, "imgWidth", "Imager Width", 120.0f, 200.0f);
    hertz (l, "imgMonoBelow", "Imager Mono Below", 0.0f, 500.0f, 120.0f, 120.0f);

    toggle (l, "loudOn", "Loudness Meter On");
    number (l, "loudTarget", "Loudness Target", -24.0f, -6.0f, 0.5f, -14.0f, "LUFS");

    // The compressor's timing and makeup, added later still (same reason).
    number (l, "compAttack", "Comp Attack", 0.5f, 100.0f, 0.1f, 10.0f, "ms", 10.0f);
    number (l, "compRelease", "Comp Release", 10.0f, 1000.0f, 1.0f, 120.0f, "ms", 120.0f);
    decibels (l, "compMakeup", "Comp Makeup", 0.0f, 24.0f, 0.0f);

    // Transient shaper and dynamic EQ (Dynamics.h), added after those.
    toggle (l, "tsOn", "Transient Shaper On");
    number (l, "tsAttack", "Transient Attack", -100.0f, 100.0f, 1.0f, 0.0f, "%");
    number (l, "tsSustain", "Transient Sustain", -100.0f, 100.0f, 1.0f, 0.0f, "%");

    toggle (l, "deqOn", "Dynamic EQ On");
    hertz (l, "deqFreq", "Dynamic EQ Freq", 40.0f, 16000.0f, 3000.0f, 1000.0f);
    number (l, "deqQ", "Dynamic EQ Q", 0.3f, 10.0f, 0.01f, 2.0f, "", 2.0f);
    decibels (l, "deqThreshold", "Dynamic EQ Threshold", -70.0f, 0.0f, -30.0f);
    decibels (l, "deqRange", "Dynamic EQ Range", 0.0f, 18.0f, 6.0f);

    // The spectral denoiser (SpectralDenoiser.h), after those. It runs right
    // after the high-pass in the chain.
    toggle (l, "denoiseOn", "Denoise On");
    number (l, "denoiseReduction", "Denoise Reduction", 0.0f, 40.0f, 0.1f, 12.0f, "dB");
    decibels (l, "denoiseThreshold", "Denoise Threshold", -10.0f, 10.0f, 0.0f);
    toggle (l, "denoiseAdaptive", "Denoise Adaptive", true);
    toggle (l, "denoiseLearn", "Denoise Learn");
    moduleGains (fx::ModuleType::denoise);

    // The harmonizer's four voices and chord following (PitchEngine), after those.
    choice (l, "harmonyMode", "Harmony Mode", fx::PitchEngine::getHarmonyModeNames(), 0);
    choice (l, "harmonyVoices", "Harmony Voices", { "1", "2", "3", "4" }, 1);
    choice (l, "harmonyStyle", "Harmony Style", fx::PitchEngine::getHarmonyStyleNames(), 0);
    choice (l, "harmonySource", "Harmony Chords From", fx::modules::Pitch::getHarmonySourceNames(), 0);
    semitones (l, "harmonyVoice3", "Harmony Voice 3", 12.0f, 1.0f, -5.0f);
    semitones (l, "harmonyVoice4", "Harmony Voice 4", 12.0f, 1.0f, 12.0f);
    percent (l, "harmonyHumanize", "Harmony Humanize", 35.0f);
    percent (l, "harmonySpread", "Harmony Spread", 50.0f);
    semitones (l, "harmonyFormant", "Harmony Formant", 12.0f, 0.1f, 0.0f);

    // Auto level (AutoLevel.h): rides the voice towards a loudness, after the compressor.
    toggle (l, "alOn", "Auto Level On");
    number (l, "alTarget", "Auto Level Target", -40.0f, -6.0f, 0.5f, -18.0f, "LUFS");
    decibels (l, "alRange", "Auto Level Range", 0.0f, 12.0f, 6.0f);
    number (l, "alSpeed", "Auto Level Speed", 100.0f, 2000.0f, 1.0f, 400.0f, "ms", 400.0f);
    choice (l, "alMode", "Auto Level Aim", { "Fixed level", "Over the backing" }, 0);
    number (l, "alOverBacking", "Auto Level Over Backing", -6.0f, 12.0f, 0.5f, 3.0f, "LU");
    moduleGains (fx::ModuleType::autoLevel);

    // The resonance suppressor (Smooth.h), after those. It runs after the parametric EQ.
    toggle (l, "smoothOn", "Smooth On");
    decibels (l, "smoothDepth", "Smooth Depth", 0.0f, fx::ResonanceSuppressor::maxDepthDb, 9.0f);
    decibels (l, "smoothThreshold", "Smooth Threshold", 0.0f, 18.0f, 6.0f);
    percent (l, "smoothSharpness", "Smooth Sharpness", 50.0f);
    number (l, "smoothFocus", "Smooth Focus", -100.0f, 100.0f, 1.0f, 0.0f, "%");
    hertz (l, "smoothLowHz", "Smooth From", 20.0f, 20000.0f, 20.0f, 1000.0f);
    hertz (l, "smoothHighHz", "Smooth To", 20.0f, 20000.0f, 20000.0f, 1000.0f);
    number (l, "smoothAttack", "Smooth Attack", 0.5f, 100.0f, 0.1f, 5.0f, "ms", 10.0f);
    number (l, "smoothRelease", "Smooth Release", 10.0f, 1000.0f, 1.0f, 100.0f, "ms", 120.0f);
    percent (l, "smoothMix", "Smooth Mix", 100.0f);
    toggle (l, "smoothDelta", "Smooth Delta");
    choice (l, "smoothStereo", "Smooth Stereo", { "Linked", "Mid/Side" }, 0);
    choice (l, "smoothResolution", "Smooth Resolution", { "Fast (5 ms)", "Balanced (11 ms)", "Quality (21 ms)" }, 1);
    moduleGains (fx::ModuleType::smooth);

    // Speaker cabinet (Cabinet.h) and convolution reverb (Convolver.h), after those. Their
    // impulse response files aren't parameters: see PluginProcessor's module files.
    toggle (l, "cabOn", "Cab On");
    choice (l, "cabModel", "Cab Model", fx::modules::Cab::getModelChoices(), (int) fx::Cabinet::Model::closed4x12);
    choice (l, "cabMic", "Cab Mic", fx::Cabinet::getMicNames(), (int) fx::Cabinet::Mic::offAxis);
    hertz (l, "cabLowCut", "Cab Low Cut", 20.0f, 500.0f, 20.0f, 100.0f);
    hertz (l, "cabHighCut", "Cab High Cut", 1000.0f, 20000.0f, 20000.0f, 6000.0f);
    percent (l, "cabMix", "Cab Mix", 100.0f);
    moduleGains (fx::ModuleType::cab);

    toggle (l, "irvOn", "IR Reverb On");
    choice (l, "irvSpace", "IR Reverb Space", fx::modules::IrReverb::getSpaceChoices(), 1);
    percent (l, "irvMix", "IR Reverb Mix", 25.0f);
    number (l, "irvPreDelay", "IR Reverb Pre-delay", 0.0f, 200.0f, 1.0f, 0.0f, "ms", 40.0f);
    hertz (l, "irvLowCut", "IR Reverb Low Cut", 20.0f, 1000.0f, 150.0f, 150.0f);
    hertz (l, "irvHighCut", "IR Reverb High Cut", 1000.0f, 20000.0f, 12000.0f, 6000.0f);
    moduleGains (fx::ModuleType::irReverb);

    // Guitar amp (GuitarAmp.h), after those. It runs before the cabinet.
    toggle (l, "ampOn", "Amp On");
    choice (l, "ampModel", "Amp Model", fx::GuitarAmp::getModelNames(), (int) fx::GuitarAmp::Model::crunch);
    number (l, "ampGain", "Amp Gain", 0.0f, 10.0f, 0.1f, 5.0f, "");
    number (l, "ampBass", "Amp Bass", 0.0f, 10.0f, 0.1f, 5.0f, "");
    number (l, "ampMid", "Amp Mid", 0.0f, 10.0f, 0.1f, 5.0f, "");
    number (l, "ampTreble", "Amp Treble", 0.0f, 10.0f, 0.1f, 5.0f, "");
    number (l, "ampPresence", "Amp Presence", 0.0f, 10.0f, 0.1f, 5.0f, "");
    number (l, "ampMaster", "Amp Master", 0.0f, 10.0f, 0.1f, 3.0f, "");
    decibels (l, "ampOutput", "Amp Output", -24.0f, 12.0f, 0.0f);
    moduleGains (fx::ModuleType::amp);

    // Voice dynamics (VocalDynamics.h), after those. New choices default to the original
    // behaviour, so saved sessions sound the same.
    choice (l, "compStyle", "Comp Style", { "Clean", "Punch", "Glue" }, 0);
    decibels (l, "compKnee", "Comp Knee", 0.0f, 24.0f, 0.0f);
    percent (l, "compMix", "Comp Mix", 100.0f);
    toggle (l, "compAutoMakeup", "Comp Auto Makeup");

    toggle (l, "optoOn", "Opto On");
    percent (l, "optoAmount", "Opto Amount", 30.0f);
    decibels (l, "optoOutput", "Opto Output", -12.0f, 24.0f, 0.0f);
    choice (l, "optoMode", "Opto Mode", { "Compress", "Limit" }, 0);
    percent (l, "optoMix", "Opto Mix", 100.0f);
    moduleGains (fx::ModuleType::opto);

    decibels (l, "gateRange", "Gate Range", 6.0f, 80.0f, 80.0f);
    number (l, "gateHold", "Gate Hold", 0.0f, 500.0f, 1.0f, 0.0f, "ms", 100.0f);
    number (l, "gateRelease", "Gate Release", 10.0f, 2000.0f, 1.0f, 150.0f, "ms", 200.0f);
    toggle (l, "gateFocus", "Gate Focus");

    choice (l, "deessMode", "De-esser Mode", { "Classic", "Smart" }, 0);
    percent (l, "deessAmount", "De-esser Amount", 50.0f);
    decibels (l, "deessRange", "De-esser Range", 0.0f, 24.0f, 24.0f);
    choice (l, "deessBand", "De-esser Band", { "Split", "Wide" }, 0);
    toggle (l, "deessListen", "De-esser Listen");

    // Space (Space.h), after those: the echo's tempo sync, duck and cuts, and the plate.
    choice (l, "delaySync", "Echo Sync", fx::modules::Echo::getSyncNames(), 0);
    percent (l, "delayDuck", "Echo Duck", 0.0f);
    hertz (l, "delayLowCut", "Echo Low Cut", 20.0f, 2000.0f, fx::Echo::defaultLowCutHz, 200.0f);
    hertz (l, "delayHighCut", "Echo High Cut", 1000.0f, 20000.0f, fx::Echo::defaultHighCutHz, 5000.0f);

    toggle (l, "plateOn", "Plate On");
    percent (l, "plateMix", "Plate Mix", 20.0f);
    number (l, "plateDecay", "Plate Decay", 0.3f, 6.0f, 0.01f, 1.8f, "s", 1.8f);
    number (l, "platePreDelay", "Plate Pre-delay", 0.0f, fx::PlateReverb::maxPreDelayMs, 1.0f, 20.0f, "ms", 40.0f);
    percent (l, "plateDamping", "Plate Damping", 40.0f);
    hertz (l, "plateLowCut", "Plate Low Cut", 20.0f, 1000.0f, 150.0f, 150.0f);
    hertz (l, "plateHighCut", "Plate High Cut", 1000.0f, 20000.0f, 9000.0f, 6000.0f);
    percent (l, "plateMod", "Plate Modulation", 30.0f);
    percent (l, "plateWidth", "Plate Width", 100.0f);
    percent (l, "plateDuck", "Plate Duck", 0.0f);
    moduleGains (fx::ModuleType::plate);
}
