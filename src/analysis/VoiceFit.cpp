#include "VoiceFit.h"

#include <algorithm>
#include <cmath>

namespace analysis::fit
{
namespace
{
    float percentile (std::vector<float> values, double fraction)
    {
        if (values.empty())
            return -200.0f;
        const auto index = (size_t) juce::jlimit (0.0, (double) values.size() - 1.0, std::round (fraction * ((double) values.size() - 1.0)));
        std::nth_element (values.begin(), values.begin() + (long) index, values.end());
        return values[index];
    }

    juce::String db (float value)   { return juce::String (juce::roundToInt (value)) + " dB"; }
    juce::String dbfs (float value) { return juce::String (juce::roundToInt (value)) + " dBFS"; }
    juce::String signedDb (float value)
    {
        const auto rounded = std::round (value * 10.0f) / 10.0f;
        return (rounded > 0.0f ? "+" : "") + juce::String (rounded, 1) + " dB";
    }
}

Result plan (const Profile& voice, const Chain& chain, double minVoiceSeconds)
{
    Result result;
    // Loudness as the chain hears it played on both sides: a mono capture (Live's mic) reads
    // 3 dB under the same voice on a stereo track, where the factory chains were set.
    result.voiceLufs = voice.getLoudnessOver();
    if (! voice.stereo && result.voiceLufs > -100.0f)
        result.voiceLufs += 3.01f;

    const auto seconds = voice.getActiveSeconds();
    if (seconds < minVoiceSeconds || result.voiceLufs <= -100.0f)
    {
        result.error = "Heard only " + juce::String (seconds, 1) + " s of voice: fitting the levels needs at least "
                     + juce::String (minVoiceSeconds, 0) + " s.";
        return result;
    }

    // The trim to -18 LUFS, inside the input gain's range.
    const auto wanted = targetLufs - result.voiceLufs;
    result.inputGainDb = juce::jlimit (chain.minInputDb, chain.maxInputDb, std::round ((chain.inputGainDb + wanted) * 2.0f) / 2.0f);
    if (std::abs (result.inputGainDb - chain.inputGainDb) < 0.5f)
        result.inputGainDb = chain.inputGainDb;
    result.trimDb = result.inputGainDb - chain.inputGainDb;
    const auto trim = result.trimDb;

    if (juce::approximatelyEqual (trim, 0.0f))
        result.notes.add ("Your voice already arrives near " + juce::String (juce::roundToInt (targetLufs)) + " LUFS: the input gain stays at "
                          + signedDb (chain.inputGainDb) + ".");
    else
        result.changes.push_back ({ "Input", signedDb (result.inputGainDb) + " (" + signedDb (trim) + "): your voice arrived at "
                                             + juce::String (juce::roundToInt (result.voiceLufs)) + " LUFS and the chain is set for "
                                             + juce::String (juce::roundToInt (targetLufs)) + " LUFS" });
    if (std::abs (result.inputGainDb - (chain.inputGainDb + wanted)) >= 1.0f)
        result.notes.add ("The input gain can't go far enough (" + signedDb (result.inputGainDb) + " is its limit): "
                          + juce::String (wanted > 0.0f ? "turn the mic up" : "turn the mic down") + " at the interface or on the clip.");

    // The room between phrases, and the voice, both as they'll arrive after the trim.
    std::vector<float> quiet, loud;
    for (size_t i = 0; i < voice.rmsDb.size() && i < voice.active.size(); ++i)
        if (voice.rmsDb[i] > -100.0f)
            (voice.active[i] ? loud : quiet).push_back (voice.rmsDb[i]);
    const auto hasPause = quiet.size() >= (size_t) noiseFrames;
    result.noiseDb = hasPause ? percentile (quiet, 0.5) : -200.0f;

    if (chain.gate)
    {
        const auto floor = percentile (quiet, 0.9) + trim;
        const auto playing = percentile (loud, 0.5) + trim;
        if (! hasPause)
            result.notes.add ("There were no pauses to measure the room in: the gate stays as it was.");
        else if (playing - floor < 20.0f)
            result.notes.add ("The room is too close to the voice (" + db (playing - floor) + " under it) for the gate to tell them apart: it stays as it was.");
        else
        {
            const auto threshold = juce::jlimit (-70.0f, -20.0f, std::round (juce::jmin (floor + 12.0f, playing - 8.0f)));
            result.values.emplace_back ("gateThreshold", threshold);
            result.changes.push_back ({ "Gate", "From " + db (threshold) + ": the room between phrases (around " + dbfs (floor)
                                                + " after the trim) goes quiet, the voice stays open" });
        }
    }

    if (chain.classicDeEsser)
    {
        // Where the S's reach in 5-10 kHz while singing (the louder quarter gets cut).
        std::vector<float> levels;
        for (size_t i = 0; i < voice.bandsDb.size() && i < voice.active.size(); ++i)
            if (voice.active[i])
            {
                double power = 0.0;
                for (int b = 0; b < numBands; ++b)
                    if (getBandCentre (b) >= 5000.0 && getBandCentre (b) <= 10000.0)
                        power += std::pow (10.0, voice.bandsDb[i][(size_t) b] / 10.0);
                if (power > 0.0)
                    levels.push_back ((float) (10.0 * std::log10 (power)));
            }

        if (! levels.empty())
        {
            const auto threshold = juce::jlimit (-60.0f, 0.0f, std::round (percentile (levels, 0.75) + trim));
            result.values.emplace_back ("deessThreshold", threshold);
            result.changes.push_back ({ "De-esser", "From " + db (threshold) + ": the loudest quarter of your S's gets softened" });
        }
    }

    if (chain.denoise)
    {
        // The quietest half second with no voice in it, to learn the room from.
        float bestLevel = 0.0f;
        for (int i = 0; i + noiseFrames <= voice.getNumFrames() && i + noiseFrames <= (int) voice.active.size(); ++i)
        {
            float sum = 0.0f;
            bool usable = true;
            for (int k = i; k < i + noiseFrames && usable; ++k)
            {
                usable = ! voice.active[(size_t) k] && voice.rmsDb[(size_t) k] > -100.0f;
                sum += voice.rmsDb[(size_t) k];
            }
            if (usable && (result.noiseFrame < 0 || sum < bestLevel))
                result.noiseFrame = i, bestLevel = sum;
        }
        if (result.noiseFrame < 0)
            result.notes.add ("No pause was long enough to learn the room: the denoiser follows the noise by itself.");
    }

    return result;
}
}
