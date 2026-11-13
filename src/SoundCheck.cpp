#include "SoundCheck.h"

#include "Scenes.h"
#include "analysis/Analysis.h"
#include "dsp/FxModules.h"
#include "dsp/SpectralDenoiser.h"

#include <algorithm>
#include <cmath>

namespace
{
    float median (std::vector<float> values)
    {
        if (values.empty())
            return -200.0f;
        const auto middle = values.begin() + (long) values.size() / 2;
        std::nth_element (values.begin(), middle, values.end());
        return *middle;
    }

    juce::String moduleName (fx::ModuleType type)
    {
        using T = fx::ModuleType;
        switch (type)
        {
            case T::highPass:   return "High-pass";
            case T::denoise:    return "Denoise";
            case T::gate:       return "Gate";
            case T::eq:         return "EQ";
            case T::paraEq:     return "Parametric EQ";
            case T::smooth:     return "Smooth";
            case T::compressor: return "Compressor";
            case T::deEsser:    return "De-esser";
            case T::dynamicEq:  return "Dynamic EQ";
            case T::reverb:     return "Reverb";
            default:            return fx::getModuleKey (type);
        }
    }

    juce::String capitalise (const juce::String& text)
    {
        return text.substring (0, 1).toUpperCase() + text.substring (1);
    }

    juce::String formatDbfs (float db) { return juce::String (juce::roundToInt (db)) + " dBFS"; }
    juce::String whole (float value) { return juce::String (juce::roundToInt (value)); }

    // The room's noise profile for the denoiser, from a stretch with no voice in it: through
    // the same low cut the chain has before the denoiser, then learnt like its Learn button.
    std::vector<float> learnNoise (const float* mono, int start, int length, double sampleRate, float lowCutHz)
    {
        const auto preRoll = juce::jmin (start, (int) (sampleRate * 0.05));
        std::vector<float> filtered ((size_t) (length + preRoll));
        std::copy (mono + start - preRoll, mono + start + length, filtered.begin());

        if (lowCutHz > 0.0f)
        {
            juce::dsp::StateVariableTPTFilter<float> highPass; // the chain's high-pass
            highPass.setType (juce::dsp::StateVariableTPTFilterType::highpass);
            highPass.prepare ({ sampleRate, 512, 1 });
            highPass.setCutoffFrequency (lowCutHz);
            for (auto& sample : filtered)
                sample = highPass.processSample (0, sample);
        }

        std::vector<float> noise (filtered.begin() + preRoll, filtered.end());
        fx::SpectralDenoiser denoiser;
        denoiser.prepare ({ sampleRate, 512, 1 });

        const auto run = [&]
        {
            auto copy = noise;
            for (size_t done = 0; done < copy.size(); done += 512)
            {
                float* channels[] = { copy.data() + done };
                juce::dsp::AudioBlock<float> block (channels, 1, juce::jmin ((size_t) 512, copy.size() - done));
                denoiser.process (juce::dsp::ProcessContextReplacing<float> (block));
            }
        };

        denoiser.setParameters (12.0f, 0.0f, true, false);
        run(); // fills the frames
        denoiser.setParameters (12.0f, 0.0f, true, true);
        run(); // learns
        denoiser.setParameters (12.0f, 0.0f, true, false);
        run(); // publishes it (the same noise: the tracker stays where it learnt)

        std::vector<float> profile;
        if (! denoiser.copyProfile (profile))
            profile.clear();
        return profile;
    }
}

//==============================================================================
SoundCheck::Plan SoundCheck::makePlan (const float* mono, int numSamples, double sampleRate, analysis::MixStyle style)
{
    Plan result;

    if (mono == nullptr || numSamples <= 0)
    {
        result.error = "Nothing was heard from the mic.";
        return result;
    }

    float* channels[] = { const_cast<float*> (mono) };
    const juce::AudioBuffer<float> audio (channels, 1, numSamples);
    const auto profile = analysis::analyse (audio, sampleRate);

    result.peakDb = profile.peakDb;
    result.voiceSeconds = profile.getActiveSeconds();
    result.voiceLufs = profile.getLoudnessOver();

    if (result.voiceSeconds < minVoiceSeconds)
    {
        result.error = "Heard only " + juce::String (result.voiceSeconds, 1) + " s of singing. Sing for at least "
                     + juce::String ((int) minVoiceSeconds) + " seconds (a verse, or a scale up and down), close to the mic.";
        return result;
    }

    // The interface's gain: peaks near -12 dBFS leave room for the loudest notes.
    {
        const auto peak = result.peakDb;
        const auto by = juce::roundToInt (std::abs (peak + 12.0f));
        if (peak >= -0.5f)
            result.inputAdvice = "Your loudest notes clip (peaks hit 0 dBFS): turn the mic gain on your audio interface down about "
                               + juce::String (by) + " dB, so peaks sit near -12 dBFS.";
        else if (peak > -6.0f)
            result.inputAdvice = "Peaks reach " + formatDbfs (peak) + ": turn the mic gain on your audio interface down about "
                               + juce::String (by) + " dB, so the loudest notes have room (aim for peaks near -12 dBFS).";
        else if (peak < -24.0f)
            result.inputAdvice = "Peaks only reach " + formatDbfs (peak) + ": turn the mic gain on your audio interface up about "
                               + juce::String (by) + " dB (aim for peaks near -12 dBFS), so the voice stays well above the noise.";
        else
            result.inputAdvice = "Input level is good: peaks at " + formatDbfs (peak) + " (around -12 dBFS is ideal).";
    }

    // The room between phrases, and the voice.
    std::vector<float> quiet, loud;
    for (size_t i = 0; i < profile.rmsDb.size() && i < profile.active.size(); ++i)
        if (profile.rmsDb[i] > -100.0f)
            (profile.active[i] ? loud : quiet).push_back (profile.rmsDb[i]);
    const auto hasPause = quiet.size() >= 5;
    result.noiseDb = hasPause ? median (quiet) : -200.0f;
    const auto playingDb = median (loud);

    // Auto Mix's "clean" plan for the voice, in the chosen style.
    analysis::SessionAnalyser::Result measured;
    measured.tracks.push_back ({ 1, profile });
    analysis::MixOptions options;
    options.mode = analysis::MixMode::clean;
    options.focusTrackId = 1;
    options.trackNames[1] = "your voice";
    const auto mix = analysis::planMix (measured, { { 1, analysis::Role::voice } }, { { 1, 0.0f } }, 0.0f, style, options);

    std::vector<std::pair<juce::String, float>> values;
    const auto set = [&values] (const juce::String& id, float value)
    {
        for (auto& entry : values)
            if (entry.first == id)
                return void (entry.second = value);
        values.emplace_back (id, value);
    };
    const auto get = [&values] (const juce::String& id, float fallback)
    {
        for (const auto& entry : values)
            if (entry.first == id)
                return entry.second;
        return fallback;
    };
    const auto add = [&result] (const juce::String& module, const juce::String& text) { result.changes.push_back ({ module, capitalise (text) }); };

    const analysis::MixMove* humMove = nullptr;
    float lowCutHz = 0.0f;
    bool usesParaEq = false;

    // In chain order: high-pass, denoise, gate, EQs, dynamics.
    for (const auto& move : mix.moves)
        if (move.insert == fx::ModuleType::gate)
        {
            for (const auto& [id, value] : move.values)
                set (id, value);
            for (const auto& reason : move.reasons)
                add (moduleName (fx::ModuleType::gate), reason);
        }

    for (const auto& move : mix.moves)
    {
        if (! move.insert.has_value() || *move.insert == fx::ModuleType::gate)
            continue; // volumes and input gain aren't part of the Live chain's job here

        // The dynamic EQ is a DAW insert (Live runs it at the end of the chain, with no card).
        if (*move.insert == fx::ModuleType::dynamicEq)
            continue;

        if (*move.insert == fx::ModuleType::paraEq && move.instance > 0)
        {
            humMove = &move;
            continue;
        }

        if (*move.insert == fx::ModuleType::paraEq)
        {
            for (const auto& [id, value] : move.values)
                set (id, value);

            // The low cut goes to the Live high-pass (always first in the chain).
            if (get ("peqLowCutOn", 0.0f) >= 0.5f)
            {
                lowCutHz = juce::jlimit (20.0f, 400.0f, get ("peqLowCutFreq", 80.0f));
                set ("hpfOn", 1.0f);
                set ("hpfFreq", lowCutHz);
                set ("peqLowCutOn", 0.0f);
            }

            for (const auto& reason : move.reasons)
                add (reason.startsWith ("Low cut") ? "High-pass" : moduleName (fx::ModuleType::paraEq), reason);

            usesParaEq = false;
            for (const auto* id : { "peqLowGain", "peqHighGain", "peqBell1Gain", "peqBell2Gain", "peqBell3Gain" })
                usesParaEq = usesParaEq || std::abs (get (id, 0.0f)) >= 0.25f;
            set ("peqOn", usesParaEq ? 1.0f : 0.0f);
            continue;
        }

        for (const auto& [id, value] : move.values)
            set (id, value);
        for (const auto& reason : move.reasons)
            add (moduleName (*move.insert), reason);
    }

    // Mains hum: narrow cuts when the parametric EQ is free, otherwise the denoiser takes it.
    auto humToDenoiser = false;
    if (humMove != nullptr)
    {
        if (! usesParaEq)
        {
            for (const auto& [id, value] : humMove->values)
                if (id != "peqLowCutOn" && id != "peqLowCutFreq")
                    set (id, value);
            for (const auto& reason : humMove->reasons)
                add (moduleName (fx::ModuleType::paraEq), reason);
            usesParaEq = true;
        }
        else
        {
            humToDenoiser = true;
        }
    }

    // The simple EQ would only colour what the parametric one set.
    if (usesParaEq)
        set ("eqOn", 0.0f);

    // Denoise when the room is audible under the voice (or hums where the EQ can't cut).
    const auto noisy = hasPause && result.noiseDb > -62.0f;
    if (noisy || humToDenoiser)
    {
        const auto snr = playingDb - result.noiseDb;
        const auto reduction = (float) juce::roundToInt (juce::jlimit (8.0f, 18.0f, juce::jmin ((result.noiseDb + 70.0f) * 0.6f + 6.0f,
                                                                                               juce::jmax (8.0f, snr - 10.0f))));
        set ("denoiseOn", 1.0f);
        set ("denoiseReduction", hasPause ? reduction : 10.0f);
        set ("denoiseThreshold", 0.0f);
        set ("denoiseAdaptive", 1.0f);
        set ("denoiseLearn", 0.0f);

        // Learn from the quietest half second between phrases.
        const auto hop = (int) std::lround (sampleRate * analysis::Profile::hopSeconds);
        int best = -1;
        float bestLevel = 0.0f;
        for (int i = 0; i + 5 <= profile.getNumFrames(); ++i)
        {
            float sum = 0.0f;
            bool usable = true;
            for (int k = i; k < i + 5 && usable; ++k)
            {
                usable = ! profile.active[(size_t) k] && profile.rmsDb[(size_t) k] > -100.0f;
                sum += profile.rmsDb[(size_t) k];
            }
            if (usable && (best < 0 || sum < bestLevel))
                best = i, bestLevel = sum;
        }

        if (best >= 0 && (best + 5) * hop <= numSamples)
            result.noiseProfile = learnNoise (mono, best * hop, 5 * hop, sampleRate, lowCutHz);

        juce::String why = humToDenoiser ? "the mains hum (" + whole (profile.humHz) + " Hz) comes out here, since the EQ is busy with your voice"
                                         : "the room is audible between phrases (" + formatDbfs (result.noiseDb) + ")";
        add (moduleName (fx::ModuleType::denoise), "On, " + whole (get ("denoiseReduction", 12.0f)) + " dB: " + why
                                                   + (result.noiseProfile.empty() ? "; it follows the noise by itself (no pause long enough to learn it)"
                                                                                  : "; learnt from your quietest half second"));
    }
    else if (hasPause)
    {
        result.notes.add ("The room is quiet between phrases (" + formatDbfs (result.noiseDb) + "): no denoiser needed.");
    }
    else
    {
        result.notes.add ("You sang without a pause, so the room's noise couldn't be measured: leave a few gaps next time.");
    }

    // Space by the style (clean mode leaves it out; on stage the voice wants its room).
    {
        const auto targets = analysis::getTargets (style);
        if (targets.reverbMix > 0.0f)
        {
            set ("reverbOn", 1.0f);
            set ("reverbMix", targets.reverbMix);
            set ("reverbSize", targets.reverbSize);
            add (moduleName (fx::ModuleType::reverb), whole (targets.reverbMix) + "% in a " + whole (targets.reverbSize)
                                                      + "% room, for " + analysis::getStyleName (style));
        }
        else
        {
            set ("reverbOn", 0.0f);
            add (moduleName (fx::ModuleType::reverb), "Off: " + analysis::getStyleName (style) + " keeps the voice dry and up front");
        }
    }

    for (const auto& note : mix.notes)
        if (! note.startsWith ("Clean & polish") && ! note.contains ("no gaps to measure noise"))
            result.notes.add (capitalise (note));

    // Chain order, so the values read like the effect cards.
    const auto orderOf = [] (const juce::String& id)
    {
        static const juce::StringArray prefixes { "hpf", "denoise", "gate", "eq", "peq", "comp", "deess", "deq", "reverb" };
        for (int i = 0; i < prefixes.size(); ++i)
            if (id.startsWith (prefixes[i]))
                return i;
        return prefixes.size();
    };
    std::stable_sort (values.begin(), values.end(), [&] (const auto& a, const auto& b) { return orderOf (a.first) < orderOf (b.first); });
    result.values = std::move (values);
    return result;
}

//==============================================================================
SoundCheck::SoundCheck (juce::AudioProcessorValueTreeState& s, fx::FxChain& liveChain, Scenes& sc)
    : state (s), chain (liveChain), scenes (sc)
{
}

SoundCheck::~SoundCheck()
{
    if (pending.valid())
        pending.wait();
}

void SoundCheck::prepare (double newSampleRate)
{
    if (! juce::approximatelyEqual (newSampleRate, sampleRate))
        buffer.setSize (0, 0); // the next start allocates for the new rate

    sampleRate = newSampleRate;

    if (phase == Phase::listening)
    {
        captureState = off;
        phase = Phase::idle;
        error = "The audio device changed: start the sound check again.";
    }
}

void SoundCheck::capture (const float* mono, int numSamples) noexcept
{
    if (captureState.load (std::memory_order_acquire) != capturing || mono == nullptr)
        return;

    auto position = written.load (std::memory_order_relaxed);
    if (finishRequested.load (std::memory_order_acquire) || position >= targetSamples)
    {
        captureState.store (captured, std::memory_order_release);
        return;
    }

    const auto count = juce::jmin (numSamples, targetSamples - position);
    buffer.copyFrom (0, position, mono, count);

    // A restart in between (cancel + start) reset the position: leave it alone.
    if (written.compare_exchange_strong (position, position + count, std::memory_order_release) && position + count >= targetSamples)
        captureState.store (captured, std::memory_order_release);
}

//==============================================================================
juce::var SoundCheck::handle (const juce::String& action, const juce::var& options)
{
    if (action == "start" && phase != Phase::listening && phase != Phase::analysing)
    {
        if (phase == Phase::done)
            handle ("keep", {});

        const auto seconds = juce::jlimit (minSeconds, maxSeconds, (double) options.getProperty ("seconds", defaultSeconds));
        const auto styleIndex = (int) options.getProperty ("style", (int) style);
        if (juce::isPositiveAndBelow (styleIndex, (int) std::size (analysis::allStyles)))
            style = analysis::allStyles[(size_t) styleIndex];

        // Allocated once (the audio thread never sees this buffer resized: it's only
        // ever resized while no capture runs and before one starts).
        const auto capacity = (int) std::ceil (maxSeconds * sampleRate);
        if (buffer.getNumSamples() != capacity && captureState.load() != capturing)
            buffer.setSize (1, capacity);

        targetSamples = juce::jmin (capacity, (int) std::round (seconds * sampleRate));
        written = 0;
        finishRequested = false;
        scanned = lastWritten = 0;
        voiceSeconds = stalledSeconds = 0.0;
        levelDb = -100.0;
        error.clear();
        sceneName.clear();
        plan = {};
        before.clear();
        lastTick = juce::Time::getMillisecondCounter();
        phase = Phase::listening;
        captureState.store (capturing, std::memory_order_release);
    }
    else if (action == "finish" && phase == Phase::listening)
    {
        finishRequested = true;
    }
    else if (action == "cancel" && phase == Phase::listening)
    {
        captureState = off;
        phase = Phase::idle;
    }
    else if (action == "compare" && phase == Phase::done)
    {
        comparingAfter = (bool) options.getProperty ("after", true);
        setValues (comparingAfter ? plan.values : before);
    }
    else if (action == "revert" && phase == Phase::done)
    {
        setValues (before);
        phase = Phase::idle;
        before.clear();
    }
    else if ((action == "keep" || action == "saveScene") && phase == Phase::done)
    {
        if (! comparingAfter)
            setValues (plan.values);
        comparingAfter = true;

        if (action == "saveScene")
        {
            const auto name = options["name"].toString().trim();
            const auto index = scenes.add (name.isNotEmpty() ? name : "Sound check");
            if (index < 0)
                error = "There are already " + juce::String (Scenes::maxScenes) + " scenes: remove one to save this sound.";
            else
                sceneName = scenes.getName (index);
        }

        if (error.isEmpty())
        {
            phase = Phase::idle;
            before.clear();
        }
    }

    return describe();
}

void SoundCheck::tick()
{
    if (phase == Phase::listening)
    {
        const auto now = juce::Time::getMillisecondCounter();
        const auto elapsed = (now - lastTick) / 1000.0;
        lastTick = now;

        scanLevels();

        // No audio coming in (the device stopped): nothing will finish the capture.
        const auto position = written.load (std::memory_order_acquire);
        stalledSeconds = position == lastWritten ? stalledSeconds + elapsed : 0.0;
        lastWritten = position;

        if (captureState.load (std::memory_order_acquire) == captured)
        {
            startAnalysis();
        }
        else if (stalledSeconds > 2.0)
        {
            captureState = off;
            fail ("No sound is coming in from the mic. Check the audio device in Settings and try again.");
            return;
        }

        if (++ticks % 3 == 0 && onChange)
            onChange (describe());
    }
    else if (phase == Phase::analysing && pending.valid()
             && pending.wait_for (std::chrono::seconds (0)) == std::future_status::ready)
    {
        finishAnalysis (pending.get());
    }
}

void SoundCheck::waitForAnalysis()
{
    if (phase == Phase::listening && captureState.load() == captured)
        startAnalysis();
    if (phase == Phase::analysing && pending.valid())
    {
        pending.wait();
        finishAnalysis (pending.get());
    }
}

void SoundCheck::scanLevels()
{
    // The written part is never touched again by the audio thread, so it can be read here.
    const auto position = written.load (std::memory_order_acquire);
    const auto hop = juce::jmax (1, (int) std::lround (sampleRate * 0.1));

    while (scanned + hop <= position)
    {
        const auto rms = buffer.getRMSLevel (0, scanned, hop);
        levelDb = juce::Decibels::gainToDecibels (rms, -100.0f);
        // A rough count for the page (the analysis decides properly): louder than a quiet room.
        if (levelDb > -50.0)
            voiceSeconds += 0.1;
        scanned += hop;
    }
}

void SoundCheck::startAnalysis()
{
    phase = Phase::analysing;
    const auto length = juce::jmin (written.load (std::memory_order_acquire), buffer.getNumSamples());
    const auto* samples = buffer.getReadPointer (0);
    const auto rate = sampleRate;
    const auto chosen = style;
    pending = std::async (std::launch::async, [samples, length, rate, chosen]
                          { return makePlan (samples, length, rate, chosen); });
    if (onChange)
        onChange (describe());
}

void SoundCheck::finishAnalysis (Plan result)
{
    captureState = off;

    if (result.error.isNotEmpty())
    {
        plan = {};
        fail (result.error);
        return;
    }

    plan = std::move (result);

    // The values from before, for Before / After and Revert.
    before.clear();
    for (const auto& [id, value] : plan.values)
        if (auto* parameter = state.getParameter (id))
            before.emplace_back (id, parameter->convertFrom0to1 (parameter->getValue()));

    if (! plan.noiseProfile.empty())
        if (auto* denoise = chain.find<fx::modules::Denoise>())
            if (! denoise->offerProfile (plan.noiseProfile))
                plan.notes.add ("The denoiser couldn't take the room's profile (the sample rate changed?): it learns the room by itself.");

    setValues (plan.values);
    comparingAfter = true;
    phase = Phase::done;

    if (onChange)
        onChange (describe());
}

void SoundCheck::setValues (const std::vector<std::pair<juce::String, float>>& values)
{
    for (const auto& [id, value] : values)
    {
        if (auto* parameter = state.getParameter (id))
        {
            const auto normalised = parameter->convertTo0to1 (value);
            if (juce::approximatelyEqual (normalised, parameter->getValue()))
                continue;
            parameter->beginChangeGesture();
            parameter->setValueNotifyingHost (normalised);
            parameter->endChangeGesture();
        }
        else
        {
            jassertfalse; // a plan value with no Live parameter
        }
    }
}

void SoundCheck::fail (const juce::String& message)
{
    error = message;
    phase = Phase::idle;
    if (onChange)
        onChange (describe());
}

juce::var SoundCheck::describe() const
{
    auto* result = new juce::DynamicObject();
    const char* phases[] = { "idle", "listening", "analysing", "done" };
    result->setProperty ("phase", phases[(int) phase]);
    result->setProperty ("seconds", std::round (written.load() / sampleRate * 10.0) / 10.0);
    result->setProperty ("target", std::round (targetSamples / sampleRate * 10.0) / 10.0);
    result->setProperty ("voiceSeconds", std::round (voiceSeconds * 10.0) / 10.0);
    result->setProperty ("level", std::round (levelDb * 10.0) / 10.0);
    result->setProperty ("canFinish", phase == Phase::listening && voiceSeconds >= minVoiceSeconds);
    result->setProperty ("minVoiceSeconds", minVoiceSeconds);
    result->setProperty ("error", error);
    result->setProperty ("scene", sceneName);

    int styleIndex = 0;
    juce::Array<juce::var> styles;
    for (size_t i = 0; i < std::size (analysis::allStyles); ++i)
    {
        styles.add (analysis::getStyleName (analysis::allStyles[i]));
        if (analysis::allStyles[i] == style)
            styleIndex = (int) i;
    }
    result->setProperty ("style", styleIndex);
    result->setProperty ("styles", styles);

    if (phase == Phase::done)
    {
        juce::Array<juce::var> changes;
        for (const auto& change : plan.changes)
        {
            auto* entry = new juce::DynamicObject();
            entry->setProperty ("module", change.module);
            entry->setProperty ("text", change.text);
            changes.add (juce::var (entry));
        }
        result->setProperty ("changes", changes);
        juce::Array<juce::var> notes;
        for (const auto& note : plan.notes)
            notes.add (note);
        result->setProperty ("notes", notes);
        result->setProperty ("advice", plan.inputAdvice);
        result->setProperty ("peak", std::round (plan.peakDb * 10.0f) / 10.0f);
        result->setProperty ("noise", std::round (plan.noiseDb * 10.0f) / 10.0f);
        result->setProperty ("sung", std::round (plan.voiceSeconds * 10.0) / 10.0);
        result->setProperty ("comparing", comparingAfter ? "after" : "before");
    }

    return juce::var (result);
}
