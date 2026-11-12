#include "Presets.h"

const std::vector<Preset>& getPresets()
{
    static const std::vector<Preset> presets {
        //======================================================================== Clean
        { "Natural", "Clean", {
            { "reverbMix", 12 }, { "reverbSize", 35 } } },

        { "Podcast", "Clean", {
            { "hpfFreq", 90 }, { "denoiseOn", 1 }, { "denoiseReduction", 15 }, { "gateThreshold", -50 },
            { "eqOn", 1 }, { "eqLow", 2 }, { "eqMidFreq", 3000 }, { "eqMidGain", 2.5f }, { "eqHigh", 1.5f },
            { "compThreshold", -22 }, { "compRatio", 4 },
            { "deessOn", 1 }, { "deessThreshold", -32 },
            { "reverbOn", 0 }, { "outputGain", 2 } } },

        { "Radio Host", "Clean", {
            { "hpfFreq", 70 },
            { "eqOn", 1 }, { "eqLow", 4 }, { "eqMidFreq", 2500 }, { "eqMidGain", 2 }, { "eqHigh", 3 },
            { "compThreshold", -26 }, { "compRatio", 6 },
            { "deessOn", 1 }, { "exciterOn", 1 }, { "exciterAmount", 25 },
            { "satOn", 1 }, { "satDrive", 4 }, { "satMix", 30 },
            { "reverbOn", 0 }, { "outputGain", 3 } } },

        { "Streamer", "Clean", {
            { "hpfFreq", 100 }, { "denoiseOn", 1 }, { "denoiseReduction", 12 }, { "gateThreshold", -48 },
            { "eqOn", 1 }, { "eqLow", 1 }, { "eqMidFreq", 3500 }, { "eqMidGain", 3 }, { "eqHigh", 2 },
            { "compThreshold", -20 }, { "compRatio", 4 },
            { "deessOn", 1 }, { "exciterOn", 1 }, { "exciterAmount", 20 },
            { "reverbMix", 8 }, { "reverbSize", 30 } } },

        //======================================================================== Tuned
        { "Pop Tune", "Tuned", {
            { "tuneOn", 1 }, { "tuneSpeed", 35 },
            { "eqOn", 1 }, { "eqMidFreq", 3000 }, { "eqMidGain", 2 }, { "eqHigh", 2.5f },
            { "compThreshold", -22 }, { "compRatio", 4 }, { "deessOn", 1 },
            { "chorusOn", 1 }, { "chorusDepth", 20 }, { "chorusMix", 20 },
            { "delayOn", 1 }, { "delayTime", 320 }, { "delayFeedback", 25 }, { "delayMix", 15 }, { "delayPingPong", 1 },
            { "reverbMix", 22 }, { "reverbSize", 60 } } },

        { "Hard Tune", "Tuned", {
            { "tuneOn", 1 }, { "tuneSpeed", 0 },
            { "eqOn", 1 }, { "eqMidFreq", 3000 }, { "eqMidGain", 3 }, { "eqHigh", 2 },
            { "compThreshold", -24 }, { "compRatio", 5 }, { "deessOn", 1 },
            { "doublerOn", 1 }, { "doublerMix", 25 },
            { "delayOn", 1 }, { "delayTime", 280 }, { "delayFeedback", 20 }, { "delayMix", 12 }, { "delayPingPong", 1 },
            { "reverbMix", 18 }, { "widthOn", 1 }, { "widthAmount", 140 } } },

        { "Choir", "Tuned", {
            { "tuneOn", 1 }, { "tuneSpeed", 60 },
            { "harmonyOn", 1 }, { "harmonyVoice1", 4 }, { "harmonyVoice2", 7 }, { "harmonyMix", 60 },
            { "chorusOn", 1 }, { "chorusDepth", 40 }, { "chorusMix", 35 },
            { "reverbMix", 35 }, { "reverbSize", 80 } } },

        { "Octaves", "Tuned", {
            { "harmonyOn", 1 }, { "harmonyVoice1", -12 }, { "harmonyVoice2", 12 }, { "harmonyMix", 45 },
            { "doublerOn", 1 }, { "doublerMix", 30 },
            { "reverbMix", 20 } } },

        //======================================================================== Characters
        { "Deep Voice", "Characters", {
            { "pitchOn", 1 }, { "pitchShift", -4 }, { "pitchFormant", -3 },
            { "eqOn", 1 }, { "eqLow", 3 },
            { "compThreshold", -20 }, { "reverbMix", 10 } } },

        { "Chipmunk", "Characters", {
            { "pitchOn", 1 }, { "pitchShift", 7 }, { "pitchFormant", 7 },
            { "reverbOn", 0 } } },

        { "Demon", "Characters", {
            { "pitchOn", 1 }, { "pitchShift", -9 }, { "pitchFormant", -6 },
            { "harmonyOn", 1 }, { "harmonyVoice1", -12 }, { "harmonyVoice2", -5 }, { "harmonyMix", 50 },
            { "eqOn", 1 }, { "eqLow", 4 },
            { "satOn", 1 }, { "satDrive", 10 }, { "satMix", 40 },
            { "reverbMix", 35 }, { "reverbSize", 75 }, { "reverbDamping", 70 } } },

        { "Robot", "Characters", {
            { "tuneOn", 1 }, { "tuneSpeed", 0 },
            { "robotOn", 1 }, { "robotFreq", 45 },
            { "eqOn", 1 }, { "eqMidFreq", 1500 }, { "eqMidGain", 4 },
            { "delayOn", 1 }, { "delayTime", 60 }, { "delayFeedback", 45 }, { "delayMix", 20 },
            { "reverbMix", 10 }, { "reverbSize", 20 } } },

        { "Alien", "Characters", {
            { "pitchOn", 1 }, { "pitchShift", 3 }, { "pitchFormant", 5 },
            { "robotOn", 1 }, { "robotFreq", 320 }, { "robotMix", 45 },
            { "flangerOn", 1 }, { "flangerRate", 0.8f }, { "flangerFeedback", 70 }, { "flangerMix", 45 },
            { "reverbMix", 25 } } },

        { "Ghost", "Characters", {
            { "pitchOn", 1 }, { "pitchFormant", 2 },
            { "eqOn", 1 }, { "eqLow", -6 }, { "eqHigh", 3 },
            { "vibratoOn", 1 }, { "vibratoRate", 4 }, { "vibratoDepth", 35 },
            { "flangerOn", 1 }, { "flangerRate", 0.1f }, { "flangerFeedback", 50 }, { "flangerMix", 35 },
            { "delayOn", 1 }, { "delayTime", 450 }, { "delayFeedback", 45 }, { "delayMix", 25 },
            { "reverbMix", 55 }, { "reverbSize", 90 }, { "reverbDamping", 30 } } },

        { "Wah Funk", "Characters", {
            { "wahOn", 1 }, { "wahSensitivity", 70 }, { "wahFreq", 350 }, { "wahMix", 75 },
            { "phaserOn", 1 }, { "phaserRate", 0.3f }, { "phaserMix", 35 },
            { "delayOn", 1 }, { "delayTime", 180 }, { "delayFeedback", 20 }, { "delayMix", 15 }, { "delayPingPong", 1 },
            { "reverbMix", 15 }, { "outputGain", 5 } } },

        //======================================================================== Lo-fi
        { "Telephone", "Lo-fi", {
            { "radioOn", 1 }, { "radioLow", 450 }, { "radioHigh", 3200 },
            { "satOn", 1 }, { "satDrive", 8 }, { "satMix", 50 },
            { "compThreshold", -24 }, { "compRatio", 6 },
            { "reverbOn", 0 }, { "outputGain", 3 } } },

        { "Walkie-Talkie", "Lo-fi", {
            { "gateThreshold", -42 },
            { "compThreshold", -28 }, { "compRatio", 8 },
            { "satOn", 1 }, { "satDrive", 18 }, { "satMix", 80 },
            { "radioOn", 1 }, { "radioLow", 700 }, { "radioHigh", 2600 },
            { "crushOn", 1 }, { "crushBits", 7 }, { "crushRate", 7000 }, { "crushMix", 60 },
            { "reverbOn", 0 }, { "outputGain", 5 } } },

        { "Megaphone", "Lo-fi", {
            { "eqOn", 1 }, { "eqMidFreq", 1800 }, { "eqMidGain", 6 },
            { "satOn", 1 }, { "satDrive", 24 },
            { "radioOn", 1 }, { "radioLow", 600 }, { "radioHigh", 4000 },
            { "reverbMix", 15 }, { "reverbSize", 45 }, { "outputGain", 3 } } },

        { "8-Bit", "Lo-fi", {
            { "tuneOn", 1 }, { "tuneSpeed", 0 },
            { "crushOn", 1 }, { "crushBits", 5 }, { "crushRate", 5000 },
            { "reverbOn", 0 } } },

        { "Underwater", "Lo-fi", {
            { "filterOn", 1 }, { "filterCutoff", 500 }, { "filterResonance", 50 },
            { "vibratoOn", 1 }, { "vibratoRate", 1.5f }, { "vibratoDepth", 60 },
            { "chorusOn", 1 }, { "chorusRate", 0.3f }, { "chorusDepth", 70 }, { "chorusMix", 50 },
            { "reverbMix", 40 }, { "reverbSize", 70 }, { "reverbDamping", 80 } } },

        { "Tape", "Lo-fi", {
            { "satOn", 1 }, { "satDrive", 10 }, { "satMix", 60 },
            { "crushOn", 1 }, { "crushBits", 12 }, { "crushRate", 16000 }, { "crushMix", 30 },
            { "filterOn", 1 }, { "filterCutoff", 3500 }, { "filterResonance", 10 },
            { "vibratoOn", 1 }, { "vibratoRate", 0.6f }, { "vibratoDepth", 15 },
            { "reverbMix", 15 } } },

        //======================================================================== Space
        { "Stadium", "Space", {
            { "eqOn", 1 }, { "eqHigh", 2 },
            { "delayOn", 1 }, { "delayTime", 420 }, { "delayFeedback", 35 }, { "delayMix", 22 }, { "delayPingPong", 1 },
            { "reverbMix", 40 }, { "reverbSize", 85 }, { "reverbDamping", 40 },
            { "widthOn", 1 }, { "widthAmount", 150 } } },

        { "Cathedral", "Space", {
            { "eqOn", 1 }, { "eqLow", -2 },
            { "reverbMix", 55 }, { "reverbSize", 100 }, { "reverbDamping", 25 } } },

        { "Astronaut", "Space", {
            { "satOn", 1 }, { "satDrive", 6 }, { "satMix", 40 },
            { "radioOn", 1 }, { "radioLow", 350 }, { "radioHigh", 3500 },
            { "delayOn", 1 }, { "delayTime", 380 }, { "delayFeedback", 40 }, { "delayMix", 25 },
            { "reverbMix", 30 }, { "reverbSize", 80 } } },

        // Added later, at the end so hosts' saved program numbers still match.
        // Harmonies on the backing track's chords (load a song in the Live player).
        { "Chord Choir", "Tuned", {
            { "tuneOn", 1 }, { "tuneSpeed", 50 }, { "tuneAmount", 80 },
            { "harmonyOn", 1 }, { "harmonyMode", 2 }, { "harmonyVoices", 2 }, { "harmonyStyle", 2 },
            { "harmonyMix", 50 }, { "harmonyHumanize", 45 }, { "harmonySpread", 70 },
            { "doublerOn", 1 }, { "doublerMix", 15 },
            { "reverbMix", 30 }, { "reverbSize", 70 } } },
    };

    return presets;
}

void applyPreset (juce::AudioProcessor& processor, const Preset& preset)
{
    for (auto* parameter : processor.getParameters())
    {
        auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (parameter);

        if (ranged == nullptr || ranged->getParameterID() == "inputGain")
            continue;

        auto target = ranged->getDefaultValue();

        for (const auto& [id, value] : preset.values)
            if (ranged->getParameterID() == id)
                target = ranged->convertTo0to1 (value);

        if (! juce::approximatelyEqual (ranged->getValue(), target))
        {
            ranged->beginChangeGesture();
            ranged->setValueNotifyingHost (target);
            ranged->endChangeGesture();
        }
    }
}
