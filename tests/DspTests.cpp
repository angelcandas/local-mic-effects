// Headless checks for the DSP. Returns a non-zero exit code if anything fails.
//
//   cmake --build --preset debug --target MicFXTests
//   ./build/debug/MicFXTests_artefacts/Debug/MicFXTests
//
// Use the release preset for meaningful CPU numbers.

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "PluginProcessor.h"
#include "Presets.h"
#include "dsp/PitchDetector.h"
#include "dsp/PitchEngine.h"
#include "dsp/SpectralDenoiser.h"

// The real editor needs the WebView; the tests only exercise the processor.
juce::AudioProcessorEditor* PluginProcessor::createEditor() { return nullptr; }

namespace
{
    constexpr double sampleRate = 48000.0;
    constexpr int blockSize = 256;

    int failures = 0;

    void check (bool passed, const juce::String& description)
    {
        std::cout << (passed ? "  PASS  " : "  FAIL  ") << description << "\n";
        failures += passed ? 0 : 1;
    }

    // A buzzy, voice-like tone: 12 harmonics falling at 1/h.
    std::vector<float> makeVoice (float frequency, double seconds, float peak = 0.3f)
    {
        std::vector<float> samples ((size_t) (seconds * sampleRate));
        const auto twoPi = juce::MathConstants<double>::twoPi;

        for (size_t i = 0; i < samples.size(); ++i)
        {
            const auto t = (double) i / sampleRate;
            double value = 0.0;

            for (int h = 1; h <= 12; ++h)
                value += std::sin (twoPi * frequency * h * t) / h;

            samples[i] = (float) (value * peak / 2.0);
        }

        return samples;
    }

    // Median YIN estimate over the second half of the signal (after everything settles).
    float measurePitch (const std::vector<float>& signal)
    {
        fx::PitchDetector detector;
        detector.prepare (sampleRate);

        std::vector<float> estimates;
        const auto hop = 240;

        for (size_t start = 0; start + hop <= signal.size(); start += hop)
        {
            detector.push (signal.data() + start, hop);

            if (start > signal.size() / 2 && detector.getFrequency() > 0.0f)
                estimates.push_back (detector.getFrequency());
        }

        if (estimates.empty())
            return 0.0f;

        std::nth_element (estimates.begin(), estimates.begin() + (long) estimates.size() / 2, estimates.end());
        return estimates[estimates.size() / 2];
    }

    float centsBetween (float a, float b)
    {
        return a > 0.0f && b > 0.0f ? 1200.0f * std::log2 (a / b) : 99999.0f;
    }

    float noteToHz (float note) { return 440.0f * std::exp2 ((note - 69.0f) / 12.0f); }

    std::vector<float> runPitchEngine (const std::vector<float>& input, const fx::PitchEngine::Settings& settings)
    {
        fx::PitchEngine engine;
        engine.prepare ({ sampleRate, (juce::uint32) blockSize, 1 });

        auto output = input;

        for (size_t start = 0; start < output.size(); start += blockSize)
        {
            const auto length = std::min ((size_t) blockSize, output.size() - start);
            float* channels[] = { output.data() + start };
            juce::dsp::AudioBlock<float> block (channels, 1, length);
            engine.process (juce::dsp::ProcessContextReplacing<float> (block), settings);
        }

        return output;
    }

    //==========================================================================
    void testPitchDetector()
    {
        std::cout << "\nPitch detector\n";

        for (auto frequency : { 82.4f, 110.0f, 196.0f, 261.6f, 440.0f, 659.3f })
        {
            const auto measured = measurePitch (makeVoice (frequency, 0.5));
            const auto error = centsBetween (measured, frequency);
            check (std::abs (error) < 5.0f, juce::String (frequency) + " Hz -> " + juce::String (measured, 2)
                                                + " Hz (" + juce::String (error, 1) + " cents)");
        }

        check (measurePitch (std::vector<float> (24000, 0.0f)) == 0.0f, "silence is reported as unvoiced");
    }

    void testPitchShift()
    {
        std::cout << "\nPitch shift (PSOLA)\n";

        const auto input = makeVoice (220.0f, 1.5);

        for (auto semitones : { -12.0f, -9.0f, -5.0f, -2.0f, 3.0f, 7.0f, 12.0f })
        {
            fx::PitchEngine::Settings settings;
            settings.shiftOn = true;
            settings.semitones = semitones;

            const auto expected = 220.0f * std::exp2 (semitones / 12.0f);
            const auto measured = measurePitch (runPitchEngine (input, settings));
            const auto error = centsBetween (measured, expected);
            check (std::abs (error) < 15.0f, juce::String (semitones, 0) + " st: expected " + juce::String (expected, 1)
                                                 + " Hz, got " + juce::String (measured, 1) + " Hz (" + juce::String (error, 1) + " cents)");
        }

        fx::PitchEngine::Settings formantOnly;
        formantOnly.shiftOn = true;
        formantOnly.formantSemitones = 5.0f;
        const auto measured = measurePitch (runPitchEngine (input, formantOnly));
        check (std::abs (centsBetween (measured, 220.0f)) < 15.0f,
               "formant +5 st keeps the pitch at 220 Hz (got " + juce::String (measured, 1) + " Hz)");
    }

    void testAutotune()
    {
        std::cout << "\nAutotune\n";

        struct Case { float inputHz; int key; int scale; float amount; float expectedNote; const char* description; };

        const Case cases[] = {
            { 227.0f, 0, 0, 1.0f, 58.0f,  "227 Hz, chromatic -> A#3" },
            { 215.0f, 0, 0, 1.0f, 57.0f,  "215 Hz, chromatic -> A3" },
            { 236.0f, 0, 1, 1.0f, 59.0f,  "236 Hz, C major (A# not in scale) -> B3" },
            { 300.0f, 9, 2, 1.0f, 62.0f,  "300 Hz, A minor -> D4" },
            { 227.0f, 0, 0, 0.5f, 57.77f, "227 Hz, 50% amount -> halfway to A#3" },
        };

        for (const auto& c : cases)
        {
            fx::PitchEngine::Settings settings;
            settings.tuneOn = true;
            settings.key = c.key;
            settings.scale = c.scale;
            settings.retuneMs = 0.0f;
            settings.amount = c.amount;

            const auto expected = noteToHz (c.expectedNote);
            const auto measured = measurePitch (runPitchEngine (makeVoice (c.inputHz, 1.5), settings));
            const auto error = centsBetween (measured, expected);
            check (std::abs (error) < 15.0f, juce::String (c.description) + ": got " + juce::String (measured, 1)
                                                 + " Hz, expected " + juce::String (expected, 1) + " (" + juce::String (error, 1) + " cents)");
        }
    }

    //==========================================================================
    std::vector<float> makeNoise (double seconds, float rmsDb, int seed = 1)
    {
        juce::Random random (seed);
        std::vector<float> samples ((size_t) (seconds * sampleRate));
        const auto peak = juce::Decibels::decibelsToGain (rmsDb) * std::sqrt (3.0f); // uniform: rms = peak / sqrt 3

        for (auto& sample : samples)
            sample = peak * (2.0f * random.nextFloat() - 1.0f);

        return samples;
    }

    std::vector<float> mix (const std::vector<float>& a, const std::vector<float>& b)
    {
        auto result = a;
        for (size_t i = 0; i < result.size() && i < b.size(); ++i)
            result[i] += b[i];
        return result;
    }

    float rmsDb (const std::vector<float>& signal, size_t from, size_t to)
    {
        double sum = 0.0;
        for (auto i = from; i < to; ++i)
            sum += (double) signal[i] * signal[i];
        return juce::Decibels::gainToDecibels ((float) std::sqrt (sum / (double) juce::jmax ((size_t) 1, to - from)), -200.0f);
    }

    struct DenoiseSettings { float reductionDb = 20.0f, thresholdDb = 0.0f; bool adaptive = true; double learnSeconds = 0.0; };

    // Runs a mono signal through the denoiser in 256-sample blocks. With learnSeconds > 0,
    // Learn is held on for that long from the start.
    std::vector<float> runDenoiser (fx::SpectralDenoiser& denoiser, const std::vector<float>& input, const DenoiseSettings& settings)
    {
        auto output = input;
        const auto learnSamples = (size_t) (settings.learnSeconds * sampleRate);

        for (size_t start = 0; start < output.size(); start += blockSize)
        {
            denoiser.setParameters (settings.reductionDb, settings.thresholdDb, settings.adaptive, start < learnSamples);

            const auto length = std::min ((size_t) blockSize, output.size() - start);
            float* channels[] = { output.data() + start };
            juce::dsp::AudioBlock<float> block (channels, 1, length);
            denoiser.process (juce::dsp::ProcessContextReplacing<float> (block));
        }

        return output;
    }

    std::vector<float> runDenoiser (const std::vector<float>& input, const DenoiseSettings& settings)
    {
        fx::SpectralDenoiser denoiser;
        denoiser.prepare ({ sampleRate, (juce::uint32) blockSize, 1 });
        return runDenoiser (denoiser, input, settings);
    }

    // Level of the output minus the (latency-aligned) reference, relative to the reference.
    float errorDb (const std::vector<float>& output, const std::vector<float>& reference, int latency, size_t from)
    {
        std::vector<float> error (output.size(), 0.0f);
        for (auto i = from; i < output.size(); ++i)
            error[i] = output[i] - reference[i - (size_t) latency];

        return rmsDb (error, from, output.size()) - rmsDb (reference, from - (size_t) latency, output.size() - (size_t) latency);
    }

    void testDenoiser()
    {
        std::cout << "\nSpectral denoiser\n";

        fx::SpectralDenoiser probe;
        probe.prepare ({ sampleRate, (juce::uint32) blockSize, 1 });
        const auto latency = probe.getLatencySamples();
        check (latency == 512, "latency is one 512-sample frame at 48 kHz (" + juce::String (latency * 1000.0 / sampleRate, 1) + " ms)");

        // With no reduction allowed, the STFT must reconstruct the input exactly (delayed).
        {
            const auto input = mix (makeVoice (180.0f, 1.0), makeNoise (1.0, -40.0f));
            const auto output = runDenoiser (input, { 0.0f });
            const auto error = errorDb (output, input, latency, (size_t) latency * 2);
            check (error < -80.0f, "0 dB reduction is a clean delay (reconstruction error " + juce::String (error, 1) + " dB)");
        }

        const auto seconds = 4.0;
        const auto tail = (size_t) (2.0 * sampleRate); // measure the last 2 s, once the estimate has settled
        const auto length = (size_t) (seconds * sampleRate);

        // Steady hiss on its own is pushed down to (almost) the reduction floor.
        {
            const auto noise = makeNoise (seconds, -50.0f);
            const auto output = runDenoiser (noise, { 20.0f });
            const auto reduction = rmsDb (noise, tail, length) - rmsDb (output, tail, length);
            check (reduction > 15.0f, "adaptive: hiss alone reduced by " + juce::String (reduction, 1) + " dB (floor 20 dB)");
        }

        // Mains hum (50 Hz + harmonics) is tonal but stationary, so it's noise too.
        {
            std::vector<float> hum (length);
            for (size_t i = 0; i < length; ++i)
                for (int h = 1; h <= 6; ++h)
                    hum[i] += 0.004f / (float) h * std::sin (juce::MathConstants<float>::twoPi * 50.0f * (float) h * (float) i / (float) sampleRate);

            const auto output = runDenoiser (hum, { 20.0f });
            const auto reduction = rmsDb (hum, tail, length) - rmsDb (output, tail, length);
            check (reduction > 10.0f, "adaptive: 50 Hz hum reduced by " + juce::String (reduction, 1) + " dB");
        }

        // Voice over hiss (after a second of room noise, as when you switch it on before
        // singing): the noise goes, the voice stays. Error is measured against the clean
        // voice, so it counts both leftover noise and any damage to the voice.
        {
            auto voice = makeVoice (180.0f, seconds);
            std::fill (voice.begin(), voice.begin() + (long) sampleRate, 0.0f);
            const auto noise = makeNoise (seconds, -45.0f);
            const auto input = mix (voice, noise);
            const auto output = runDenoiser (input, { 20.0f });

            const auto before = errorDb (input, voice, 0, tail);
            const auto after = errorDb (output, voice, latency, tail);
            check (before - after > 6.0f, "adaptive: voice + hiss, noise-to-voice " + juce::String (before, 1) + " dB -> "
                                              + juce::String (after, 1) + " dB");

            const auto voiceChange = rmsDb (output, tail, length) - rmsDb (voice, tail - (size_t) latency, length - (size_t) latency);
            check (std::abs (voiceChange) < 1.0f, "adaptive: voice level kept (" + juce::String (voiceChange, 2) + " dB)");
        }

        // A note held for 7 s (with a little vibrato, like a singer) mustn't be mistaken
        // for noise and faded away.
        {
            const auto longSeconds = 8.0;
            const auto longLength = (size_t) (longSeconds * sampleRate);
            std::vector<float> voice (longLength, 0.0f);
            double phase = 0.0;

            for (auto i = (size_t) sampleRate; i < longLength; ++i)
            {
                const auto t = (double) i / sampleRate;
                phase += juce::MathConstants<double>::twoPi * 220.0 * std::exp2 (0.3 / 12.0 * std::sin (juce::MathConstants<double>::twoPi * 5.5 * t)) / sampleRate;

                double value = 0.0;
                for (int h = 1; h <= 12; ++h)
                    value += std::sin (phase * h) / h;
                voice[i] = (float) (value * 0.15);
            }

            const auto output = runDenoiser (mix (voice, makeNoise (longSeconds, -50.0f)), { 20.0f });
            const auto from = longLength - (size_t) sampleRate;
            const auto change = rmsDb (output, from, longLength) - rmsDb (voice, from - (size_t) latency, longLength - (size_t) latency);
            check (std::abs (change) < 1.5f, "adaptive: sustained note still at " + juce::String (change, 2) + " dB after 7 s");
        }

        // Learn: 1 s of room noise, then voice with Adaptive off uses the learnt profile.
        {
            const auto noise = makeNoise (seconds + 1.0, -45.0f, 7);
            auto voice = makeVoice (180.0f, seconds + 1.0);
            std::fill (voice.begin(), voice.begin() + (long) sampleRate, 0.0f);

            fx::SpectralDenoiser denoiser;
            denoiser.prepare ({ sampleRate, (juce::uint32) blockSize, 1 });
            const auto output = runDenoiser (denoiser, mix (voice, noise), { 20.0f, 0.0f, false, 1.0 });

            check (denoiser.getLearnedFrames() > 300, "learn: profile built from " + juce::String (denoiser.getLearnedFrames()) + " frames");

            const auto start = (size_t) (2.0 * sampleRate);
            const auto before = errorDb (mix (voice, noise), voice, 0, start);
            const auto after = errorDb (output, voice, latency, start);
            check (before - after > 6.0f, "learned profile: voice + hiss, noise-to-voice " + juce::String (before, 1) + " dB -> "
                                              + juce::String (after, 1) + " dB");
        }

        // The processor reports the latency only while the module is on.
        {
            PluginProcessor processor;
            processor.prepareToPlay (sampleRate, blockSize);
            const auto off = processor.getLatencySamples();

            if (auto* on = dynamic_cast<juce::AudioParameterBool*> (processor.getState().getParameter ("denoiseOn")))
                *on = true;

            processor.prepareToPlay (sampleRate, blockSize);
            check (off == 0 && processor.getLatencySamples() == latency,
                   "processor latency: " + juce::String (off) + " samples off, " + juce::String (processor.getLatencySamples()) + " on");
        }
    }

    //==========================================================================
    struct RunResult { bool finite = true; float peak = 0.0f, rmsDb = -100.0f; double realtimeFactor = 0.0; };

    RunResult runProcessor (PluginProcessor& processor, const std::vector<float>& voice)
    {
        processor.prepareToPlay (sampleRate, blockSize);

        juce::AudioBuffer<float> buffer (2, blockSize);
        juce::MidiBuffer midi;
        RunResult result;
        double sumSquares = 0.0;
        size_t counted = 0;

        const auto startTime = juce::Time::getMillisecondCounterHiRes();

        for (size_t start = 0; start + blockSize <= voice.size(); start += blockSize)
        {
            buffer.clear();
            buffer.copyFrom (0, 0, voice.data() + start, blockSize);
            processor.processBlock (buffer, midi);

            for (int ch = 0; ch < 2; ++ch)
            {
                const auto* data = buffer.getReadPointer (ch);

                for (int i = 0; i < blockSize; ++i)
                {
                    result.finite = result.finite && std::isfinite (data[i]);
                    result.peak = std::max (result.peak, std::abs (data[i]));

                    if (start > voice.size() / 2)
                    {
                        sumSquares += (double) data[i] * data[i];
                        ++counted;
                    }
                }
            }
        }

        const auto elapsedSeconds = (juce::Time::getMillisecondCounterHiRes() - startTime) / 1000.0;
        result.realtimeFactor = elapsedSeconds / ((double) voice.size() / sampleRate);
        result.rmsDb = juce::Decibels::gainToDecibels ((float) std::sqrt (sumSquares / (double) juce::jmax ((size_t) 1, counted)), -100.0f);
        return result;
    }

    void testPresets()
    {
        std::cout << "\nPresets (3 s of voice through the full processor)\n";

        PluginProcessor processor;
        const auto voice = makeVoice (180.0f, 3.0);

        std::map<juce::String, juce::RangedAudioParameter*> byId;
        for (auto* parameter : processor.getParameters())
            if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (parameter))
                byId[ranged->getParameterID()] = ranged;

        const auto& presets = getPresets();
        double worstFactor = 0.0;

        for (int index = 0; index < (int) presets.size(); ++index)
        {
            const auto& preset = presets[(size_t) index];

            for (const auto& [id, value] : preset.values)
            {
                const auto it = byId.find (id);
                const auto valid = it != byId.end()
                                && value >= it->second->getNormalisableRange().start
                                && value <= it->second->getNormalisableRange().end;
                if (! valid)
                    check (false, juce::String (preset.name) + ": '" + id + "' = " + juce::String (value) + " is not a valid parameter value");
            }

            processor.setCurrentProgram (index);
            const auto result = runProcessor (processor, voice);
            worstFactor = std::max (worstFactor, result.realtimeFactor);

            check (result.finite && result.peak <= 1.0f && result.rmsDb > -40.0f,
                   juce::String (preset.name).paddedRight (' ', 14) + " peak " + juce::String (result.peak, 3)
                       + ", rms " + juce::String (result.rmsDb, 1) + " dB, cpu " + juce::String (result.realtimeFactor * 100.0, 2) + "% of real time");
        }

        std::cout << "\nEvery module switched on at once\n";

        for (auto& [id, parameter] : byId)
            if (id.endsWith ("On") && dynamic_cast<juce::AudioParameterBool*> (parameter) != nullptr)
                parameter->setValueNotifyingHost (1.0f);

        const auto result = runProcessor (processor, voice);
        check (result.finite && result.peak <= 1.0f,
               "all 25 effects: peak " + juce::String (result.peak, 3) + ", cpu "
                   + juce::String (result.realtimeFactor * 100.0, 2) + "% of real time");

        std::cout << "\n  Worst preset CPU: " << juce::String (worstFactor * 100.0, 2) << "% of one core\n";
    }
}

int main()
{
    juce::ScopedJuceInitialiser_GUI juce;

    testPitchDetector();
    testPitchShift();
    testAutotune();
    testDenoiser();
    testPresets();

    std::cout << "\n" << (failures == 0 ? "All tests passed" : juce::String (failures) + " test(s) failed") << "\n";
    return failures == 0 ? 0 : 1;
}
