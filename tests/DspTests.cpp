// Headless checks for the DSP. Returns a non-zero exit code if anything fails.
//
//   cmake --build --preset debug --target MicFXTests
//   ./build/debug/MicFXTests_artefacts/Debug/MicFXTests
//
// Use the release preset for meaningful CPU numbers.

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <set>
#include <thread>

#include "DawBridge.h"
#include "PluginProcessor.h"
#include "analysis/Analysis.h"
#include "analysis/MixAssistant.h"
#include "Presets.h"
#include "daw/DawEngine.h"
#include "daw/ClipTools.h"
#include "daw/Exporter.h"
#include "daw/Project.h"
#include "dsp/FxChain.h"
#include "dsp/PitchDetector.h"
#include "dsp/PitchEngine.h"

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
               "all 24 effects: peak " + juce::String (result.peak, 3) + ", cpu "
                   + juce::String (result.realtimeFactor * 100.0, 2) + "% of real time");

        std::cout << "\n  Worst preset CPU: " << juce::String (worstFactor * 100.0, 2) << "% of one core\n";
    }

    //==========================================================================
    // Runs `signal` (mono, copied to both channels) through a chain; returns both channels.
    std::vector<float> runChain (fx::FxChain& chain, const std::vector<float>& signal)
    {
        chain.prepare ({ sampleRate, (juce::uint32) blockSize, 2 });

        std::vector<float> output;
        juce::AudioBuffer<float> buffer (2, blockSize);

        for (size_t start = 0; start + blockSize <= signal.size(); start += blockSize)
        {
            for (int ch = 0; ch < 2; ++ch)
                buffer.copyFrom (ch, 0, signal.data() + start, blockSize);

            juce::dsp::AudioBlock<float> block (buffer);
            chain.process (fx::Module::Context (block));

            for (int ch = 0; ch < 2; ++ch)
                output.insert (output.end(), buffer.getReadPointer (ch), buffer.getReadPointer (ch) + blockSize);
        }

        return output;
    }

    void switchAll (fx::FxChain& chain, bool on)
    {
        for (int m = 0; m < chain.size(); ++m)
            for (int v = 0; v < chain[m].getNumValues(); ++v)
                if (chain[m].isSwitch (v))
                    chain[m].getValue (v).store (on ? 1.0f : 0.0f);
    }

    void testFxChain()
    {
        std::cout << "\nEffect chain (the building block for track inserts)\n";

        // Every module value is a plugin parameter with the same default, and every
        // parameter except the input/output gains belongs to exactly one module.
        {
            PluginProcessor processor;
            std::map<juce::String, juce::RangedAudioParameter*> byId;
            for (auto* parameter : processor.getParameters())
                if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (parameter))
                    byId[ranged->getParameterID()] = ranged;

            fx::FxChain chain (fx::FxChain::getAllTypes());
            std::set<juce::String> covered { "inputGain", "outputGain" };
            juce::StringArray problems;

            for (int m = 0; m < chain.size(); ++m)
            {
                for (int v = 0; v < chain[m].getNumValues(); ++v)
                {
                    const auto& id = chain[m].getValueId (v);
                    const auto it = byId.find (id);

                    if (it == byId.end())
                    {
                        problems.add (id + " has no plugin parameter");
                        continue;
                    }

                    const auto* parameter = it->second;
                    const auto expected = parameter->convertFrom0to1 (parameter->getDefaultValue());

                    if (std::abs (expected - chain[m].getValue (v).load()) > 1.0e-4f)
                        problems.add (id + " default " + juce::String (chain[m].getValue (v).load()) + " != " + juce::String (expected));

                    if (! covered.insert (id).second)
                        problems.add (id + " is used by two modules");
                }
            }

            for (const auto& [id, parameter] : byId)
                if (covered.count (id) == 0)
                    problems.add (id + " isn't driven by any module");

            for (const auto& problem : problems)
                std::cout << "        " << problem << "\n";

            check (problems.isEmpty(), "module values match the plugin parameters (IDs and defaults)");
        }

        const auto voice = makeVoice (180.0f, 1.0);

        // An insert chain holding a few modules sounds the same as the full chain with
        // only those modules switched on, so track inserts reuse Live mode's sound.
        {
            const std::vector<fx::ModuleType> inserts { fx::ModuleType::eq, fx::ModuleType::compressor,
                                                        fx::ModuleType::echo, fx::ModuleType::reverb };
            fx::FxChain full (fx::FxChain::getAllTypes()), partial (inserts);
            switchAll (full, false);
            switchAll (partial, true);

            for (auto type : inserts)
                full.find (type)->getValue (0).store (1.0f);

            check (runChain (full, voice) == runChain (partial, voice), "an insert chain matches the full chain with the same modules on");
        }

        // Chains don't share state: one chain processing doesn't change another's output.
        {
            fx::FxChain a (fx::FxChain::getAllTypes()), b (fx::FxChain::getAllTypes()), reference (fx::FxChain::getAllTypes());

            for (auto* chain : { &a, &b, &reference })
            {
                chain->setValue ("delayOn", 1.0f);
                chain->setValue ("chorusOn", 1.0f);
                chain->setValue ("tuneOn", 1.0f);
            }

            const auto expected = runChain (reference, voice);

            // Interleave a and b block by block, a fed with a different signal.
            const auto other = makeVoice (310.0f, 1.0);
            a.prepare ({ sampleRate, (juce::uint32) blockSize, 2 });
            b.prepare ({ sampleRate, (juce::uint32) blockSize, 2 });
            juce::AudioBuffer<float> bufferA (2, blockSize), bufferB (2, blockSize);
            std::vector<float> outputB;

            for (size_t start = 0; start + blockSize <= voice.size(); start += blockSize)
            {
                for (int ch = 0; ch < 2; ++ch)
                {
                    bufferA.copyFrom (ch, 0, other.data() + start, blockSize);
                    bufferB.copyFrom (ch, 0, voice.data() + start, blockSize);
                }

                juce::dsp::AudioBlock<float> blockA (bufferA), blockB (bufferB);
                a.process (fx::Module::Context (blockA));
                b.process (fx::Module::Context (blockB));

                for (int ch = 0; ch < 2; ++ch)
                    outputB.insert (outputB.end(), bufferB.getReadPointer (ch), bufferB.getReadPointer (ch) + blockSize);
            }

            check (outputB == expected, "two chains running side by side don't affect each other");
        }

        // Switched-off modules leave the signal untouched (reverb off is a pure dry path,
        // once its dry gain has faded in from the first prepare).
        {
            fx::FxChain chain (fx::FxChain::getAllTypes());
            switchAll (chain, false);
            const auto output = runChain (chain, voice);
            float worst = 0.0f;

            for (size_t i = (size_t) (0.1 * sampleRate); i < voice.size() / blockSize * blockSize; ++i)
                worst = std::max (worst, std::abs (output[(i / blockSize) * blockSize * 2 + i % blockSize] - voice[i]));

            check (worst < 1.0e-5f, "every module off passes the signal through (max error " + juce::String (worst, 7) + ")");
        }

        {
            fx::FxChain chain (fx::FxChain::getAllTypes());
            check (! chain.setValue ("noSuchValue", 1.0f) && chain.setValue ("eqLow", 3.0f)
                       && chain.find (fx::ModuleType::eq)->findValue ("eqLow")->load() == 3.0f,
                   "values are set by plugin parameter ID");
        }
    }

    //==========================================================================
    // Gain (dB) of a sine at `frequency` through `chain`, measured on the left channel
    // after the filters settle.
    double measureGainDb (fx::FxChain& chain, double frequency)
    {
        std::vector<float> sine ((size_t) (0.5 * sampleRate));
        for (size_t i = 0; i < sine.size(); ++i)
            sine[i] = (float) (0.25 * std::sin (juce::MathConstants<double>::twoPi * frequency * (double) i / sampleRate));

        const auto output = runChain (chain, sine);
        double in = 0.0, out = 0.0;
        const auto blocks = sine.size() / blockSize;

        for (size_t b = blocks / 2; b < blocks; ++b)
            for (size_t i = 0; i < (size_t) blockSize; ++i)
            {
                in  += juce::square ((double) sine[b * blockSize + i]);
                out += juce::square ((double) output[b * blockSize * 2 + i]);
            }

        return 10.0 * std::log10 (juce::jmax (1.0e-30, out) / in);
    }

    void testParametricEq()
    {
        std::cout << "\nParametric EQ\n";

        fx::FxChain chain ({ fx::ModuleType::paraEq });
        auto& eq = *chain.find<fx::modules::ParaEq>();
        chain.setValue ("peqOn", 1.0f);

        // Flat: every band at 0 dB and both cuts off passes the signal through.
        {
            const auto voice = makeVoice (180.0f, 0.5);
            const auto output = runChain (chain, voice);
            float worst = 0.0f;
            for (size_t i = 0; i < voice.size() / blockSize * blockSize; ++i)
                worst = std::max (worst, std::abs (output[(i / blockSize) * blockSize * 2 + i % blockSize] - voice[i]));
            check (worst < 1.0e-5f, "flat settings pass the signal through (max error " + juce::String (worst, 7) + ")");
        }

        // A bell boosts by its gain at its centre and leaves far-away frequencies alone.
        chain.setValue ("peqBell2Freq", 1000.0f);
        chain.setValue ("peqBell2Gain", 6.0f);
        chain.setValue ("peqBell2Q", 2.0f);
        {
            const auto centre = measureGainDb (chain, 1000.0), far = measureGainDb (chain, 100.0);
            check (std::abs (centre - 6.0) < 0.2 && std::abs (far) < 0.2,
                   "bell: +" + juce::String (centre, 2) + " dB at 1 kHz, " + juce::String (far, 2) + " dB at 100 Hz");
        }

        // The low cut is 24 dB/octave: -3 dB at the corner, about -48 dB two octaves below.
        chain.setValue ("peqBell2Gain", 0.0f);
        chain.setValue ("peqLowCutOn", 1.0f);
        chain.setValue ("peqLowCutFreq", 200.0f);
        {
            const auto corner = measureGainDb (chain, 200.0), below = measureGainDb (chain, 50.0);
            check (std::abs (corner + 3.0) < 0.3 && below < -44.0,
                   "low cut: " + juce::String (corner, 2) + " dB at the corner, " + juce::String (below, 1) + " dB two octaves below");
        }

        // The low cut toggle is a band setting, not the module's switch.
        {
            fx::FxChain other ({ fx::ModuleType::paraEq });
            other.setValue ("peqLowCutOn", 1.0f);
            check (! other[0].isOn(), "switching a cut on doesn't switch the EQ on");
        }

        // getMagnitudeDb() (used to plan Auto Mix moves) matches what the EQ does.
        chain.setValue ("peqLowCutFreq", 60.0f);
        chain.setValue ("peqLowGain", -4.0f);
        chain.setValue ("peqLowFreq", 150.0f);
        chain.setValue ("peqBell1Freq", 320.0f);
        chain.setValue ("peqBell1Gain", -5.0f);
        chain.setValue ("peqBell1Q", 1.4f);
        chain.setValue ("peqBell3Freq", 3500.0f);
        chain.setValue ("peqBell3Gain", 3.0f);
        chain.setValue ("peqBell3Q", 0.8f);
        chain.setValue ("peqHighGain", 2.5f);
        chain.setValue ("peqHighCutOn", 1.0f);
        chain.setValue ("peqHighCutFreq", 15000.0f);
        {
            double worst = 0.0;
            for (auto f : { 40.0, 90.0, 150.0, 320.0, 700.0, 1500.0, 3500.0, 7000.0, 12000.0, 16000.0 })
            {
                const auto predicted = fx::ParametricEq::getMagnitudeDb (eq.getSettings(), f, sampleRate);
                worst = std::max (worst, std::abs (predicted - measureGainDb (chain, f)));
            }
            check (worst < 0.3, "predicted response matches the measured one (worst " + juce::String (worst, 3) + " dB)");
        }
    }


    //==========================================================================
    // Stereo sines: each part is { seconds, frequency, amplitude per channel }.
    void writeWav (const juce::File&, const juce::AudioBuffer<float>&, double rate);

    struct TonePart { double seconds, frequency; float left, right; };

    juce::AudioBuffer<float> makeTones (std::initializer_list<TonePart> parts)
    {
        double total = 0.0;
        for (auto& part : parts) total += part.seconds;

        juce::AudioBuffer<float> buffer (2, (int) std::lround (total * sampleRate));
        buffer.clear();
        int start = 0;

        for (auto& part : parts)
        {
            const auto length = (int) std::lround (part.seconds * sampleRate);
            for (int i = 0; i < length && start + i < buffer.getNumSamples(); ++i)
            {
                const auto v = (float) std::sin (juce::MathConstants<double>::twoPi * part.frequency * i / sampleRate);
                buffer.setSample (0, start + i, part.left * v);
                buffer.setSample (1, start + i, part.right * v);
            }
            start += length;
        }

        return buffer;
    }

    void testAnalysis()
    {
        std::cout << "\nAnalysis (loudness, spectrum, activity, masking)\n";

        // BS.1770: a 0 dBFS 997 Hz sine reads -3.01 LUFS in one channel, 0 in both.
        {
            const auto both = analysis::analyse (makeTones ({ { 5.0, 997.0, 1.0f, 1.0f } }), sampleRate);
            const auto left = analysis::analyse (makeTones ({ { 5.0, 997.0, 1.0f, 0.0f } }), sampleRate);
            check (std::abs (both.integratedLufs) < 0.1f && std::abs (left.integratedLufs + 3.01f) < 0.1f,
                   "997 Hz full-scale sine: " + juce::String (both.integratedLufs, 2) + " LUFS stereo, "
                       + juce::String (left.integratedLufs, 2) + " LUFS one channel");
        }

        // Gating: silence and a much quieter part don't pull the integrated loudness down.
        {
            const auto withSilence = analysis::analyse (makeTones ({ { 4.0, 997.0, 0.1f, 0.1f }, { 4.0, 997.0, 0.0f, 0.0f } }), sampleRate);
            const auto withQuiet = analysis::analyse (makeTones ({ { 4.0, 997.0, 0.1f, 0.1f }, { 4.0, 997.0, 0.01f, 0.01f } }), sampleRate);
            check (std::abs (withSilence.integratedLufs + 20.0f) < 0.2f && std::abs (withQuiet.integratedLufs + 20.0f) < 0.3f,
                   "gating: -20 LUFS tone plus silence reads " + juce::String (withSilence.integratedLufs, 2)
                       + ", plus a -40 part reads " + juce::String (withQuiet.integratedLufs, 2));
            check (std::abs (withQuiet.shortTermMaxLufs + 20.0f) < 0.2f && std::abs (withQuiet.peakDb + 20.0f) < 0.1f,
                   "short-term max and sample peak");
        }

        // Spectrum: a sine lands in its own band at its mean-square level.
        {
            for (auto frequency : { 63.0, 250.0, 3150.0, 12500.0 })
            {
                const auto profile = analysis::analyse (makeTones ({ { 2.0, frequency, 0.5f, 0.5f } }), sampleRate);
                const auto spectrum = profile.getAverageSpectrum();
                const auto band = analysis::findBand (frequency);
                double total = 0.0;
                for (auto db : spectrum) total += std::pow (10.0, db / 10.0);

                const auto expected = 10.0f * std::log10 (0.125f); // a 0.5 sine's mean square
                check (std::abs (spectrum[(size_t) band] - expected) < 0.5f && std::abs (10.0 * std::log10 (total) - expected) < 0.5,
                       juce::String (frequency, 0) + " Hz sine: " + juce::String (spectrum[(size_t) band], 2) + " dB in its band (expected "
                           + juce::String (expected, 2) + "), " + juce::String (10.0 * std::log10 (total), 2) + " dB in all bands");
            }
        }

        // Activity: only the parts with sound count.
        {
            const auto profile = analysis::analyse (makeTones ({ { 1.0, 440.0, 0.3f, 0.3f }, { 1.5, 440.0, 0.0f, 0.0f },
                                                                 { 1.0, 440.0, 0.1f, 0.1f }, { 0.5, 440.0, 0.0f, 0.0f } }), sampleRate);
            check (std::abs (profile.getActiveSeconds() - 2.0) <= 0.35, "active for " + juce::String (profile.getActiveSeconds(), 1) + " s of 2 s of sound");
        }

        // Masking: a backing 6 dB louder than the voice at the voice's frequency, plus a
        // low note the voice doesn't use. Only counted while the voice sings.
        {
            const auto voice = analysis::analyse (makeTones ({ { 3.0, 2000.0, 0.1f, 0.1f }, { 3.0, 2000.0, 0.0f, 0.0f } }), sampleRate);
            auto backingAudio = makeTones ({ { 6.0, 2000.0, 0.2f, 0.2f } });
            const auto low = makeTones ({ { 6.0, 125.0, 0.2f, 0.2f } });
            for (int ch = 0; ch < 2; ++ch)
                backingAudio.addFrom (ch, 0, low, ch, 0, low.getNumSamples());
            const auto backing = analysis::analyse (backingAudio, sampleRate);

            const auto masking = analysis::measureMasking (voice, backing);
            const auto at2k = masking.backingOverVoiceDb[(size_t) analysis::findBand (2000.0)];
            check (std::abs (at2k - 6.02f) < 0.5f && masking.worstBand == analysis::findBand (2000.0),
                   "backing covers the voice by " + juce::String (at2k, 2) + " dB at 2 kHz, the worst band");
            check (std::abs (masking.overlapSeconds - 3.0) <= 0.35, "masking counts only while the voice sings ("
                       + juce::String (masking.overlapSeconds, 1) + " s)");
            const auto expected = analysis::analyse (makeTones ({ { 3.0, 2000.0, 0.1f, 0.1f } }), sampleRate).integratedLufs - backing.integratedLufs;
            check (std::abs (masking.voiceOverBackingLu - expected) < 0.3f,
                   "voice sits " + juce::String (masking.voiceOverBackingLu, 2) + " LU against the backing (expected " + juce::String (expected, 2) + ")");
        }

        // A DAW session: each track's source (no inserts, unity fader) or its stem.
        {
            const auto folder = juce::File::createTempFile ("micfx-analysis");
            folder.createDirectory();
            const auto voiceFile = folder.getChildFile ("voice.wav"), backingFile = folder.getChildFile ("backing.wav");
            writeWav (voiceFile, makeTones ({ { 2.0, 997.0, 0.1f, 0.1f } }), sampleRate);
            writeWav (backingFile, makeTones ({ { 4.0, 220.0, 0.2f, 0.2f } }), sampleRate);

            daw::Edit edit;
            const auto voice = (int) edit.addTrack ("Voice")[daw::ids::id];
            const auto backing = (int) edit.addTrack ("Backing")[daw::ids::id];
            edit.addClip (voice, voiceFile, 1.0, 2.0);
            edit.addClip (backing, backingFile, 0.0, 4.0);
            edit.setTrackVolume (voice, -10.0f);
            edit.setTrackMute (backing, true);
            const auto eq = (int) edit.addInsert (voice, fx::ModuleType::eq)[daw::ids::id];
            edit.setInsertValue (eq, "eqMidFreq", 997.0f);
            edit.setInsertValue (eq, "eqMidGain", 6.0f);

            auto source = analysis::SessionAnalyser (edit, analysis::SessionAnalyser::Point::source, true).run();
            auto stem = analysis::SessionAnalyser (edit, analysis::SessionAnalyser::Point::stem, false).run();

            const auto& voiceSource = source.tracks[0].second;
            const auto frames = voiceSource.getNumFrames();
            check (source.tracks.size() == 2 && source.tracks[0].first == voice && source.mix.has_value()
                       && source.tracks[1].second.getNumFrames() == frames && source.mix->getNumFrames() == frames
                       && std::abs (frames - 40) <= 1,
                   "one profile per track and one for the mix, all on the same 4 s grid");
            const auto clipAlone = analysis::analyse (makeTones ({ { 1.0, 997.0, 0.0f, 0.0f }, { 2.0, 997.0, 0.1f, 0.1f }, { 1.0, 997.0, 0.0f, 0.0f } }),
                                                      sampleRate).integratedLufs;
            check (std::abs (voiceSource.integratedLufs - clipAlone) < 0.1f && std::abs (voiceSource.getActiveSeconds() - 2.0) <= 0.35,
                   "source: the voice's clip without its fader or inserts (" + juce::String (voiceSource.integratedLufs, 2) + " LUFS)");
            check (std::abs (source.tracks[1].second.integratedLufs - analysis::analyse (makeTones ({ { 4.0, 220.0, 0.2f, 0.2f } }), sampleRate).integratedLufs) < 0.2f,
                   "a muted track is still analysed");
            const auto stemLufs = stem.tracks[0].second.integratedLufs;
            check (std::abs (stemLufs - (voiceSource.integratedLufs - 10.0f + 6.0f)) < 0.3f && ! stem.mix.has_value(),
                   "stem: through the track's EQ (+6 dB) and fader (-10 dB): " + juce::String (stemLufs - voiceSource.integratedLufs, 2) + " dB");
            check (std::abs (source.mix->integratedLufs - stem.tracks[0].second.integratedLufs) < 0.3f,
                   "mix: the session as it plays (the voice's stem, backing muted)");

            folder.deleteRecursively();
        }
    }

    // A sung line: phrases of a 160 Hz voice-like tone (harmonics at 1/h) with a muddy
    // second harmonic (+9.5 dB at 320 Hz), alternating loud and soft, with gaps.
    juce::AudioBuffer<float> makeSungLine (double seconds, double period = 1.25, double sung = 1.0)
    {
        juce::AudioBuffer<float> buffer (2, (int) (seconds * sampleRate));
        buffer.clear();
        const auto twoPi = juce::MathConstants<double>::twoPi;

        for (int i = 0; i < buffer.getNumSamples(); ++i)
        {
            const auto t = i / sampleRate;
            const auto phrase = (int) std::floor (t / period);
            if (std::fmod (t, period) > sung)
                continue; // a breath between phrases

            const auto level = phrase % 2 == 0 ? 0.3 : 0.07;
            const auto f0 = 160.0 * (1.0 + 0.06 * (phrase % 3)); // a few different notes
            double value = 0.0;
            for (int h = 1; h <= 20; ++h)
                value += std::sin (twoPi * f0 * h * t) / h * (h == 2 ? 3.0 : 1.0);

            for (int ch = 0; ch < 2; ++ch)
                buffer.setSample (ch, i, (float) (value * level / 3.0));
        }

        return buffer;
    }

    // A backing: a low note, a loud part right where the voice's words are (2.5 kHz),
    // some top end and a little noise, all the way through.
    juce::AudioBuffer<float> makeBacking (double seconds)
    {
        juce::AudioBuffer<float> buffer (2, (int) (seconds * sampleRate));
        juce::Random random (42);
        const auto twoPi = juce::MathConstants<double>::twoPi;

        for (int i = 0; i < buffer.getNumSamples(); ++i)
        {
            const auto t = i / sampleRate;
            const auto value = 0.1 * std::sin (twoPi * 110.0 * t) + 0.06 * std::sin (twoPi * 2500.0 * t)
                             + 0.02 * std::sin (twoPi * 6000.0 * t) + 0.01 * (random.nextFloat() * 2.0 - 1.0);
            for (int ch = 0; ch < 2; ++ch)
                buffer.setSample (ch, i, (float) value);
        }

        return buffer;
    }

    // A guitar-ish part: plucked 196 Hz notes (decaying harmonics) every half second,
    // a loud bar then a quiet one, so its level swings; a little rumble at 30 Hz.
    juce::AudioBuffer<float> makePlucks (double seconds)
    {
        juce::AudioBuffer<float> buffer (2, (int) (seconds * sampleRate));
        const auto twoPi = juce::MathConstants<double>::twoPi;

        for (int i = 0; i < buffer.getNumSamples(); ++i)
        {
            const auto t = i / sampleRate;
            const auto since = std::fmod (t, 0.5);
            const auto level = (int) std::floor (t / 2.0) % 2 == 0 ? 0.25 : 0.04;
            double value = 0.0;
            for (int h = 1; h <= 12; ++h)
                value += std::sin (twoPi * 196.0 * h * t) / h * std::exp (-since * (3.0 + h));
            value = value * level + 0.002 * std::sin (twoPi * 30.0 * t);
            for (int ch = 0; ch < 2; ++ch)
                buffer.setSample (ch, i, (float) value);
        }

        return buffer;
    }

    // Auto Mix adding effects: reverb on a dry voice (at the end of its chain), EQ and a
    // compressor on an instrument, none on a voice that rings already, and only the
    // effects it's allowed.
    void testAutoMixEffects()
    {
        std::cout << "\nAuto Mix adds effects\n";

        const auto folder = juce::File::createTempFile ("micfx-automix-fx");
        folder.createDirectory();
        const auto voiceFile = folder.getChildFile ("voice.wav"), guitarFile = folder.getChildFile ("guitar.wav"),
                   backingFile = folder.getChildFile ("backing.wav"), roomyFile = folder.getChildFile ("roomy.wav");
        writeWav (voiceFile, makeSungLine (12.0, 2.5, 1.5), sampleRate);
        writeWav (guitarFile, makePlucks (12.0), sampleRate);
        writeWav (backingFile, makeBacking (12.0), sampleRate);
        {
            auto roomy = makeSungLine (12.0, 2.5, 1.5);
            fx::FxChain room ({ fx::ModuleType::reverb });
            room.setValue ("reverbMix", 45.0f);
            room.setValue ("reverbSize", 90.0f);
            room.setValue ("reverbDamping", 20.0f);
            room.prepare ({ sampleRate, (juce::uint32) roomy.getNumSamples(), 2 });
            juce::dsp::AudioBlock<float> block (roomy);
            room.process (fx::Module::Context (block));
            writeWav (roomyFile, roomy, sampleRate);
        }

        daw::Edit edit;
        const auto voice = (int) edit.addTrack ("Vocal")[daw::ids::id];
        const auto guitar = (int) edit.addTrack ("Guitar")[daw::ids::id];
        const auto backing = (int) edit.addTrack ("Backing")[daw::ids::id];
        const auto roomy = (int) edit.addTrack ("Harmony vocal")[daw::ids::id];
        edit.addClip (voice, voiceFile, 0.0, 12.0);
        edit.addClip (guitar, guitarFile, 0.0, 12.0);
        edit.addClip (backing, backingFile, 0.0, 12.0);
        edit.addClip (roomy, roomyFile, 0.0, 12.0);
        const auto own = (int) edit.addInsert (guitar, fx::ModuleType::chorus)[daw::ids::id];
        juce::ignoreUnused (own);

        const auto source = analysis::SessionAnalyser (edit, analysis::SessionAnalyser::Point::source, false).run();
        std::map<int, analysis::Role> roles;
        std::map<int, float> volumes;
        for (const auto& [id, profile] : source.tracks)
        {
            roles[id] = analysis::guessRole (edit.getTrack (id)[daw::ids::name].toString(), profile, 12.0);
            volumes[id] = 0.0f;
        }
        check (roles[guitar] == analysis::Role::instrument && roles[voice] == analysis::Role::voice && roles[roomy] == analysis::Role::voice,
               "guesses the guitar is an instrument");

        analysis::MixOptions options;
        options.existingInserts[guitar] = { fx::ModuleType::chorus };
        const auto plan = analysis::planMix (source, roles, volumes, 0.0f, analysis::MixStyle::ballad, options);

        for (const auto& move : plan.moves)
            for (const auto& reason : move.reasons)
                std::cout << "        " << edit.getTrack (move.trackId)[daw::ids::name].toString() << ": " << reason << "\n";
        for (const auto& note : plan.notes)
            std::cout << "        note: " << note << "\n";

        const auto find = [] (const analysis::MixPlan& p, int track, fx::ModuleType type) -> const analysis::MixMove*
        {
            for (const auto& move : p.moves)
                if (move.trackId == track && move.insert == type)
                    return &move;
            return nullptr;
        };

        const auto* reverb = find (plan, voice, fx::ModuleType::reverb);
        check (reverb != nullptr && reverb->atEnd && juce::exactlyEqual (reverb->values.at ("reverbMix"), 20.0f),
               "dry voice: a ballad-sized reverb");
        check (find (plan, roomy, fx::ModuleType::reverb) == nullptr && find (plan, roomy, fx::ModuleType::paraEq) != nullptr,
               "a voice that rings on already gets no reverb (but still its EQ)");

        const auto* guitarEq = find (plan, guitar, fx::ModuleType::paraEq);
        check (guitarEq != nullptr && juce::exactlyEqual (guitarEq->values.at ("peqLowCutOn"), 1.0f)
                   && guitarEq->values.at ("peqLowCutFreq") >= 60.0f && guitarEq->values.at ("peqLowCutFreq") <= 120.0f,
               "instrument: low cut under its lowest note (196 Hz)");
        check (find (plan, guitar, fx::ModuleType::compressor) != nullptr, "instrument: compresses its loud and quiet bars");
        check (find (plan, backing, fx::ModuleType::compressor) == nullptr && find (plan, backing, fx::ModuleType::reverb) == nullptr,
               "a full backing gets no compressor or reverb");

        // Applied: the reverb goes after the track's own effects, the rest before them.
        edit.addInsert (voice, fx::ModuleType::chorus);
        analysis::applyMix (edit, plan);
        const auto voiceInserts = daw::Edit::getInserts (edit.getTrack (voice));
        const auto guitarInserts = daw::Edit::getInserts (edit.getTrack (guitar));
        check (voiceInserts.getChild (voiceInserts.getNumChildren() - 1)[daw::ids::type].toString() == "reverb"
                   && voiceInserts.getChild (voiceInserts.getNumChildren() - 2)[daw::ids::type].toString() == "chorus"
                   && voiceInserts.getChild (0)[daw::ids::type].toString() == "paraEq",
               "reverb goes at the end of the voice's chain, EQ at the start");
        check (guitarInserts.getNumChildren() == 3 && guitarInserts.getChild (0)[daw::ids::type].toString() == "paraEq"
                   && guitarInserts.getChild (1)[daw::ids::type].toString() == "compressor"
                   && guitarInserts.getChild (2)[daw::ids::type].toString() == "chorus",
               "instrument: EQ and compressor before its own chorus");

        // Half the amount: half the reverb.
        edit.getUndoManager().undo();
        analysis::applyMix (edit, plan, 0.5f);
        {
            const auto inserts = daw::Edit::getInserts (edit.getTrack (voice));
            check (juce::exactlyEqual ((float) inserts.getChild (inserts.getNumChildren() - 1)["reverbMix"], 10.0f),
                   "amount 50% halves the reverb");
        }

        // Only what it's allowed to add.
        analysis::MixOptions eqOnly;
        eqOnly.compressor = eqOnly.deEsser = eqOnly.reverb = false;
        const auto limited = analysis::planMix (source, roles, volumes, 0.0f, analysis::MixStyle::ballad, eqOnly);
        bool onlyEq = true;
        for (const auto& move : limited.moves)
            onlyEq = onlyEq && (! move.insert.has_value() || *move.insert == fx::ModuleType::paraEq);
        analysis::MixOptions none;
        none.eq = none.compressor = none.deEsser = none.reverb = false;
        const auto volumesOnly = analysis::planMix (source, roles, volumes, 0.0f, analysis::MixStyle::ballad, none);
        bool onlyVolumes = ! volumesOnly.moves.empty();
        for (const auto& move : volumesOnly.moves)
            onlyVolumes = onlyVolumes && ! move.insert.has_value();
        check (onlyEq && find (limited, voice, fx::ModuleType::paraEq) != nullptr && onlyVolumes,
               "adds only the effects you allow (volumes always)");

        // Coaching keeps the voice dry.
        const auto coaching = analysis::planMix (source, roles, volumes, 0.0f, analysis::MixStyle::coaching, options);
        check (find (coaching, voice, fx::ModuleType::reverb) == nullptr, "coaching: no reverb");

        folder.deleteRecursively();
    }

    void testMixAssistant()
    {
        std::cout << "\nAuto Mix (mix assistant)\n";

        const auto folder = juce::File::createTempFile ("micfx-automix");
        folder.createDirectory();
        const auto voiceFile = folder.getChildFile ("voice.wav"), backingFile = folder.getChildFile ("backing.wav");
        writeWav (voiceFile, makeSungLine (10.0), sampleRate);
        writeWav (backingFile, makeBacking (10.0), sampleRate);

        daw::Edit edit;
        const auto voice = (int) edit.addTrack ("Track 1")[daw::ids::id];
        const auto backing = (int) edit.addTrack ("Backing")[daw::ids::id];
        edit.addClip (voice, voiceFile, 0.0, 10.0);
        edit.addClip (backing, backingFile, 0.0, 10.0);
        const auto reverb = (int) edit.addInsert (voice, fx::ModuleType::reverb)[daw::ids::id];
        edit.setInsertValue (reverb, "reverbMix", 0.0f);

        const auto source = analysis::SessionAnalyser (edit, analysis::SessionAnalyser::Point::source, false).run();
        const auto& voiceProfile = source.tracks[0].second;
        const auto& backingProfile = source.tracks[1].second;

        check (analysis::guessRole ("Track 1", voiceProfile, 10.0) == analysis::Role::voice
                   && analysis::guessRole ("Backing", backingProfile, 10.0) == analysis::Role::backing
                   && analysis::guessRole ("Lead vocal", backingProfile, 10.0) == analysis::Role::voice,
               "guesses voice and backing (pitched " + juce::String (voiceProfile.getPitchedFraction() * 100.0, 0) + "% of the voice)");

        const std::map<int, analysis::Role> roles { { voice, analysis::Role::voice }, { backing, analysis::Role::backing } };
        analysis::MixOptions existing; // as DawBridge passes them: the voice has its own reverb
        existing.existingInserts[voice] = { fx::ModuleType::reverb };
        const auto plan = analysis::planMix (source, roles, { { voice, 0.0f }, { backing, 0.0f } }, 0.0f, analysis::MixStyle::pop, existing);

        for (const auto& move : plan.moves)
            for (const auto& reason : move.reasons)
                std::cout << "        " << (move.trackId == voice ? "voice:   " : move.trackId == backing ? "backing: " : "master:  ") << reason << "\n";
        for (const auto& note : plan.notes)
            std::cout << "        note: " << note << "\n";

        const auto find = [&] (int track, std::optional<fx::ModuleType> type) -> const analysis::MixMove*
        {
            for (const auto& move : plan.moves)
                if (move.trackId == track && move.insert == type)
                    return &move;
            return nullptr;
        };

        const auto* voiceEq = find (voice, fx::ModuleType::paraEq);
        check (voiceEq != nullptr && std::abs (voiceEq->values.at ("peqLowCutFreq") - 120.0f) <= 12.0f,
               "voice: low cut under the lowest note (160 Hz)");
        check (voiceEq != nullptr && voiceEq->values.at ("peqBell1Gain") <= -2.0f
                   && std::abs (std::log2 (voiceEq->values.at ("peqBell1Freq") / 320.0f)) <= 0.34f,
               "voice: cuts the muddy 320 Hz");

        const auto* compressor = find (voice, fx::ModuleType::compressor);
        check (compressor != nullptr && compressor->values.at ("compRatio") >= 3.0f, "voice: compresses the loud and soft phrases");

        const auto* backingEq = find (backing, fx::ModuleType::paraEq);
        check (backingEq != nullptr && backingEq->values.at ("peqBell2Gain") <= -1.0f
                   && std::abs (std::log2 (backingEq->values.at ("peqBell2Freq") / 2500.0f)) <= 0.34f,
               "backing: dips where it covers the words (2.5 kHz)");

        // Applied, the voice lands near the style's balance and the mix near its target.
        analysis::applyMix (edit, plan);
        const auto inserts = daw::Edit::getInserts (edit.getTrack (voice));
        check (inserts.getNumChildren() == 3 + (find (voice, fx::ModuleType::deEsser) != nullptr ? 1 : 0)
                   && inserts.getChild (0)[daw::ids::type].toString() == "paraEq"
                   && inserts.getChild (inserts.getNumChildren() - 1)[daw::ids::type].toString() == "reverb",
               "adds its inserts before the track's own effects");
        check (juce::exactlyEqual ((float) inserts.getChild (inserts.getNumChildren() - 1)["reverbMix"], 0.0f),
               "leaves the voice's own reverb as it was");

        const auto after = analysis::SessionAnalyser (edit, analysis::SessionAnalyser::Point::stem, true).run();
        const auto masking = analysis::measureMasking (after.tracks[0].second, after.tracks[1].second);
        const auto target = analysis::getTargets (analysis::MixStyle::pop).voiceOverBackingLu;
        check (std::abs (masking.voiceOverBackingLu - target) < 1.5f,
               "voice sits " + juce::String (masking.voiceOverBackingLu, 2) + " LU over the backing (target " + juce::String (target, 1)
                   + ", predicted " + juce::String (plan.voiceOverBackingLu, 2) + ")");
        check (after.mix.has_value() && after.mix->peakDb < -0.5f && std::abs (after.mix->integratedLufs - plan.mixLufs) < 2.0f,
               "mix at " + juce::String (after.mix->integratedLufs, 1) + " LUFS (predicted " + juce::String (plan.mixLufs, 1)
                   + "), peak " + juce::String (after.mix->peakDb, 1) + " dBFS");

        // One undo step takes it all back.
        edit.getUndoManager().undo();
        check (daw::Edit::getInserts (edit.getTrack (voice)).getNumChildren() == 1 && daw::Edit::getInserts (edit.getTrack (backing)).getNumChildren() == 0
                   && juce::exactlyEqual ((float) edit.getTrack (voice)[daw::ids::volume], 0.0f)
                   && juce::exactlyEqual ((float) edit.getMaster()[daw::ids::volume], 0.0f),
               "one undo removes the whole Auto Mix");

        // Reference match: a darker, quieter mix is brought towards a brighter, louder song.
        {
            auto referenceAudio = makeBacking (6.0);
            fx::FxChain bright ({ fx::ModuleType::paraEq });
            bright.setValue ("peqOn", 1.0f);
            bright.setValue ("peqHighFreq", 8000.0f);
            bright.setValue ("peqHighGain", 4.0f);
            bright.prepare ({ sampleRate, (juce::uint32) referenceAudio.getNumSamples(), 2 });
            juce::dsp::AudioBlock<float> block (referenceAudio);
            bright.process (fx::Module::Context (block));
            referenceAudio.applyGain (0.25f);

            const auto referenceFile = folder.getChildFile ("reference.wav");
            writeWav (referenceFile, referenceAudio, sampleRate);
            const auto reference = analysis::analyseFile (referenceFile);

            auto quiet = makeBacking (6.0);
            quiet.applyGain (0.1f);
            const auto quietMix = analysis::analyse (quiet, sampleRate);
            const auto match = analysis::planReferenceMatch (quietMix, reference, 0.0f);

            for (const auto& move : match.moves)
                for (const auto& reason : move.reasons)
                    std::cout << "        reference: " << reason << "\n";

            const auto& eqValues = match.moves.at (0).values;
            const auto& volume = match.moves.at (1).values.at ("volume");
            const auto expectedVolume = reference.integratedLufs - quietMix.integratedLufs;
            check (reference.getNumFrames() == 60 && eqValues.at ("peqHighGain") >= 2.0f && std::abs (eqValues.at ("peqLowGain")) <= 1.0f,
                   "reference: brightens the top end to match (" + juce::String (eqValues.at ("peqHighGain"), 1) + " dB), leaves the lows");
            check (volume > expectedVolume - 4.0f && volume < expectedVolume + 1.0f && std::abs (match.mixLufs - reference.integratedLufs) < 1.0f,
                   "reference: master up " + juce::String (volume, 1) + " dB towards its loudness (" + juce::String (reference.integratedLufs, 1) + " LUFS)");
        }

        // Amount 0 changes nothing you can hear.
        analysis::applyMix (edit, plan, 0.0f);
        const auto eqInsert = daw::Edit::getInserts (edit.getTrack (voice)).getChild (0);
        check (juce::exactlyEqual ((float) eqInsert["peqBell1Gain"], 0.0f) && juce::exactlyEqual ((float) edit.getTrack (voice)[daw::ids::volume], 0.0f),
               "amount 0 leaves the gains and volumes neutral");

        folder.deleteRecursively();
    }

    //==========================================================================
    // Reads a whole WAV file; empty if it can't be opened.
    juce::AudioBuffer<float> readWav (const juce::File& file, double& fileRate)
    {
        juce::WavAudioFormat format;
        std::unique_ptr<juce::AudioFormatReader> reader (format.createReaderFor (file.createInputStream().release(), true));

        if (reader == nullptr)
            return {};

        fileRate = reader->sampleRate;
        juce::AudioBuffer<float> contents ((int) reader->numChannels, (int) reader->lengthInSamples);
        reader->read (&contents, 0, contents.getNumSamples(), 0, true, true);
        return contents;
    }

    float maxDifference (const float* a, const float* b, int length)
    {
        float worst = 0.0f;
        for (int i = 0; i < length; ++i)
            worst = std::max (worst, std::abs (a[i] - b[i]));
        return worst;
    }

    void testRecorder()
    {
        std::cout << "\nRecorder\n";

        PluginProcessor processor;
        processor.setCurrentProgram (1); // any preset, so the output differs from the dry mic
        processor.prepareToPlay (sampleRate, blockSize);

        const auto folder = juce::File::createTempFile ("micfx-recordings");
        auto& recorder = processor.getRecorder();
        const auto error = recorder.start (folder, true);
        check (error.isEmpty() && recorder.isRecording(), "starts recording" + (error.isEmpty() ? "" : ": " + error));

        const auto voice = makeVoice (200.0f, 1.0);
        const auto numBlocks = (int) voice.size() / blockSize;
        juce::AudioBuffer<float> expected (2, numBlocks * blockSize);
        juce::AudioBuffer<float> buffer (2, blockSize);
        juce::MidiBuffer midi;

        for (int b = 0; b < numBlocks; ++b)
        {
            buffer.clear();
            buffer.copyFrom (0, 0, voice.data() + b * blockSize, blockSize);
            processor.processBlock (buffer, midi);

            for (int ch = 0; ch < 2; ++ch)
                expected.copyFrom (ch, b * blockSize, buffer, ch, 0, blockSize);
        }

        const auto take = recorder.stop();
        check (! recorder.isRecording() && ! take.droppedAudio, "stops without dropping audio");
        check (std::abs (take.seconds - numBlocks * blockSize / sampleRate) < 1e-6,
               "reports the take length (" + juce::String (take.seconds, 3) + " s)");

        double wetRate = 0.0, dryRate = 0.0;
        const auto wet = readWav (take.file, wetRate);
        const auto dry = readWav (take.dryFile, dryRate);
        const auto length = numBlocks * blockSize;

        // 24-bit files: anything closer than ~1e-6 is quantisation.
        check (wet.getNumChannels() == 2 && wet.getNumSamples() == length && juce::exactlyEqual (wetRate, sampleRate)
                   && maxDifference (wet.getReadPointer (0), expected.getReadPointer (0), length) < 1e-5f
                   && maxDifference (wet.getReadPointer (1), expected.getReadPointer (1), length) < 1e-5f,
               "output file matches the processed audio (" + take.file.getFileName() + ")");
        check (dry.getNumChannels() == 1 && dry.getNumSamples() == length && juce::exactlyEqual (dryRate, sampleRate)
                   && maxDifference (dry.getReadPointer (0), voice.data(), length) < 1e-5f,
               "dry file matches the mic input (" + take.dryFile.getFileName() + ")");

        // Audio processed while not recording isn't captured.
        processor.processBlock (buffer, midi);
        check (readWav (take.file, wetRate).getNumSamples() == length, "nothing is written after stop");

        folder.deleteRecursively();
    }

    //==========================================================================
    void writeWav (const juce::File& file, const juce::AudioBuffer<float>& audio, double rate)
    {
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
        auto writer = juce::WavAudioFormat().createWriterFor (stream, juce::AudioFormatWriterOptions{}
                                                                          .withSampleRate (rate)
                                                                          .withNumChannels (audio.getNumChannels())
                                                                          .withBitsPerSample (24));
        writer->writeFromAudioSampleBuffer (audio, 0, audio.getNumSamples());
    }

    // Runs silent mic input through the processor and returns the left output.
    std::vector<float> playSilence (PluginProcessor& processor, int numBlocks)
    {
        std::vector<float> out;
        juce::AudioBuffer<float> buffer (2, blockSize);
        juce::MidiBuffer midi;

        for (int b = 0; b < numBlocks; ++b)
        {
            buffer.clear();
            processor.processBlock (buffer, midi);
            out.insert (out.end(), buffer.getReadPointer (0), buffer.getReadPointer (0) + blockSize);
        }

        return out;
    }

    float rms (const std::vector<float>& x, size_t from, size_t to)
    {
        double sum = 0.0;
        for (auto i = from; i < to; ++i)
            sum += (double) x[i] * x[i];
        return (float) std::sqrt (sum / (double) juce::jmax ((size_t) 1, to - from));
    }

    void testMultitrack()
    {
        std::cout << "\nMulti-track player\n";

        const auto folder = juce::File::createTempFile ("micfx-multitrack");
        folder.createDirectory();

        // 2 s of 300 Hz at 44.1 kHz, so loading has to resample to the 48 kHz session.
        const double fileRate = 44100.0;
        juce::AudioBuffer<float> song (2, (int) (2.0 * fileRate));
        for (int i = 0; i < song.getNumSamples(); ++i)
            for (int ch = 0; ch < 2; ++ch)
                song.setSample (ch, i, 0.25f * (float) std::sin (juce::MathConstants<double>::twoPi * 300.0 * i / fileRate));

        const auto songFile = folder.getChildFile ("backing.wav");
        writeWav (songFile, song, fileRate);

        PluginProcessor processor;
        processor.prepareToPlay (sampleRate, blockSize);
        auto& tracks = processor.getMultitrack();

        const auto backingId = tracks.loadNow (songFile, false, 0);
        check (backingId > 0 && tracks.getTracks().size() == 1, "loads a 44.1 kHz WAV as the backing track");
        check (std::abs ((double) tracks.getLength() / sampleRate - 2.0) < 0.01,
               "resampled to 48 kHz keeps its 2 s length (" + juce::String ((double) tracks.getLength() / sampleRate, 3) + " s)");
        check (tracks.loadNow (folder.getChildFile ("missing.wav"), false, 0) == -1, "a missing file is rejected");

        check (rms (playSilence (processor, 20), 0, 20 * blockSize) < 1e-6f, "silent while the transport is stopped");

        tracks.play();
        const auto played = playSilence (processor, 100);
        const auto expectedRms = 0.25f / std::sqrt (2.0f);
        const auto measuredRms = rms (played, 10 * blockSize, played.size());
        check (std::abs (measuredRms - expectedRms) < 0.01f, "plays the backing track into the output (rms "
                                                                 + juce::String (measuredRms, 3) + ")");
        check (std::abs (centsBetween (measurePitch (played), 300.0f)) < 5.0f, "at the right pitch after resampling");
        check (tracks.getPosition() == 100 * blockSize, "the playhead advances with the audio");

        tracks.setTrackMuted (backingId, true);
        check (rms (playSilence (processor, 10), 0, 10 * blockSize) < 1e-6f, "mute silences the track");
        tracks.setTrackMuted (backingId, false);

        tracks.seek ((juce::int64) (3.0 * sampleRate));
        check (rms (playSilence (processor, 10), 0, 10 * blockSize) < 1e-6f, "silent past the end of the song");

        // Record a take while the song plays from 0.5 s: it remembers where it started.
        tracks.seek ((juce::int64) (0.5 * sampleRate));
        auto& recorder = processor.getRecorder();
        recorder.start (folder, false);

        const auto voice = makeVoice (200.0f, 0.5);
        juce::AudioBuffer<float> buffer (2, blockSize);
        juce::MidiBuffer midi;

        for (size_t start = 0; start + blockSize <= voice.size(); start += blockSize)
        {
            buffer.clear();
            buffer.copyFrom (0, 0, voice.data() + start, blockSize);
            processor.processBlock (buffer, midi);
        }

        const auto take = recorder.stop();
        check (take.timelineStart == (juce::int64) (0.5 * sampleRate),
               "the take starts where the playhead was (" + juce::String (take.timelineStart) + ")");

        double takeRate = 0.0;
        const auto takeAudio = readWav (take.file, takeRate);
        const auto voiceOnly = rms (std::vector<float> (takeAudio.getReadPointer (0), takeAudio.getReadPointer (0) + takeAudio.getNumSamples()),
                                    0, (size_t) takeAudio.getNumSamples());
        const auto dryVoice = rms (voice, 0, voice.size());
        check (std::abs (centsBetween (measurePitch (std::vector<float> (takeAudio.getReadPointer (0), takeAudio.getReadPointer (0) + takeAudio.getNumSamples())), 200.0f)) < 10.0f
                   && voiceOnly < dryVoice * 2.0f,
               "the take holds the voice, not the backing track");

        const auto takeId = tracks.loadNow (take.file, true, take.timelineStart);
        tracks.setTrackMuted (backingId, true);
        processor.prepareToPlay (sampleRate, blockSize); // clear the reverb tail of the voice
        tracks.seek (0);
        const auto withTake = playSilence (processor, (int) (1.2 * sampleRate) / blockSize);
        const auto takeStart = (size_t) take.timelineStart;
        check (takeId > 0 && rms (withTake, 0, takeStart - blockSize) < 1e-6f && rms (withTake, takeStart + blockSize, takeStart + 10 * blockSize) > 0.01f,
               "the take plays back on its own row at its position");

        tracks.removeTrack (takeId);
        tracks.removeTrack (backingId);
        check (tracks.getTracks().empty() && tracks.getLength() == 0, "removing tracks empties the session");

        tracks.pause();
        folder.deleteRecursively();
    }
    //==========================================================================
    // Renders `seconds` of the engine in device-sized blocks; returns {left, right}.
    std::array<std::vector<float>, 2> render (daw::DawEngine& engine, double seconds)
    {
        std::array<std::vector<float>, 2> out;
        juce::AudioBuffer<float> buffer (2, blockSize);

        for (int done = 0; done < (int) (seconds * sampleRate); done += blockSize)
        {
            engine.process (buffer, 2, blockSize);
            for (int ch = 0; ch < 2; ++ch)
                out[(size_t) ch].insert (out[(size_t) ch].end(), buffer.getReadPointer (ch), buffer.getReadPointer (ch) + blockSize);
        }

        return out;
    }

    void testDawEngine()
    {
        std::cout << "\nDAW engine (tracks, inserts, master bus)\n";

        const auto folder = juce::File::createTempFile ("micfx-daw");
        folder.createDirectory();

        juce::AudioBuffer<float> tone (2, (int) sampleRate);
        for (int i = 0; i < tone.getNumSamples(); ++i)
            for (int ch = 0; ch < 2; ++ch)
                tone.setSample (ch, i, 0.4f * (float) std::sin (juce::MathConstants<double>::twoPi * 330.0 * i / sampleRate));

        const auto toneFile = folder.getChildFile ("tone.wav");
        writeWav (toneFile, tone, sampleRate);

        const auto slowFile = folder.getChildFile ("tone-44k.wav");
        writeWav (slowFile, tone, 44100.0);

        daw::Edit edit;
        daw::DawEngine engine (edit);
        engine.prepare (sampleRate, blockSize);

        const auto source = engine.getAudioPool().loadNow (toneFile);
        check (source != nullptr && source->getNumSamples() == (int) sampleRate, "loads a clip's audio");

        const auto resampled = engine.getAudioPool().loadNow (slowFile);
        // The file holds 48000 samples at 44.1 kHz (1.088 s): 52245 samples at 48 kHz.
        const auto expectedLength = (int) std::floor (sampleRate * sampleRate / 44100.0);
        check (resampled != nullptr && std::abs (resampled->getNumSamples() - expectedLength) <= 1,
               "a 44.1 kHz file is resampled to the session rate");

        const auto vocal = (int) edit.addTrack ("Vocal")[daw::ids::id];
        edit.addClip (vocal, toneFile, 0.5, 1.0);
        engine.rebuildNow();
        check (engine.getLength() == (juce::int64) (1.5 * sampleRate), "the session ends where the last clip ends");

        const auto clipStart = (size_t) (0.5 * sampleRate);
        const auto matchesSource = [&] (const std::vector<float>& channel, float gain, float tolerance)
        {
            float worst = 0.0f;
            for (size_t i = 0; i < (size_t) source->getNumSamples(); ++i)
                worst = std::max (worst, std::abs (channel[clipStart + i] - gain * source->getSample (0, (int) i)));
            return worst <= tolerance;
        };

        // Stopped: silence, playhead stays put.
        auto out = render (engine, 0.1);
        check (rms (out[0], 0, out[0].size()) == 0.0f && engine.getPosition() == 0, "silent and still while stopped");

        engine.play();
        out = render (engine, 2.0);
        engine.stop();

        check (rms (out[0], 0, clipStart) == 0.0f && matchesSource (out[0], 1.0f, 0.0f) && matchesSource (out[1], 1.0f, 0.0f),
               "a clip plays sample-exact at its position");

        const auto playFrom = [&] (double seconds)
        {
            engine.seek (0);
            engine.play();
            auto result = render (engine, seconds);
            engine.stop();
            return result;
        };

        const auto clipRms = [&] (const std::vector<float>& channel)
        {
            return rms (channel, clipStart + 4800, clipStart + 43200); // skip edges and smoothing
        };

        const auto unity = clipRms (out[0]);

        edit.getUndoManager().beginNewTransaction();
        edit.setTrackVolume (vocal, -6.0f);
        out = playFrom (1.6);
        check (std::abs (clipRms (out[0]) / unity - juce::Decibels::decibelsToGain (-6.0f)) < 0.001f, "track volume (-6 dB)");

        edit.getUndoManager().undo();
        engine.rebuildNow();
        out = playFrom (1.6);
        check (std::abs (clipRms (out[0]) / unity - 1.0f) < 0.001f, "undo restores the volume");

        edit.setTrackPan (vocal, -1.0f);
        out = playFrom (1.6);
        check (std::abs (clipRms (out[0]) / unity - 1.0f) < 0.001f && clipRms (out[1]) < 1.0e-4f, "pan hard left");
        edit.setTrackPan (vocal, 0.0f);

        const auto backing = (int) edit.addTrack ("Backing")[daw::ids::id];
        edit.addClip (backing, toneFile, 0.5, 1.0);
        engine.rebuildNow();
        out = playFrom (1.6);
        check (matchesSource (out[0], 2.0f, 1.0e-6f), "two tracks sum on the master bus");

        edit.setTrackMute (backing, true);
        out = playFrom (1.6);
        check (std::abs (clipRms (out[0]) / unity - 1.0f) < 0.001f, "mute takes a track out of the mix");

        edit.setTrackMute (backing, false);
        edit.setTrackSolo (backing, true);
        out = playFrom (1.6);
        check (std::abs (clipRms (out[0]) / unity - 1.0f) < 0.001f, "solo plays only the soloed track");
        edit.setTrackSolo (backing, false);
        edit.removeTrack (backing);
        engine.rebuildNow();

        // A track insert sounds like the same module run on its own.
        const auto eq = (int) edit.addInsert (vocal, fx::ModuleType::eq)[daw::ids::id];
        edit.setInsertValue (eq, "eqHigh", -12.0f);
        edit.setInsertValue (eq, "eqLow", 6.0f);
        engine.rebuildNow();
        out = playFrom (1.6);

        fx::FxChain reference ({ fx::ModuleType::eq });
        reference.setValue ("eqOn", 1.0f);
        reference.setValue ("eqHigh", -12.0f);
        reference.setValue ("eqLow", 6.0f);
        std::vector<float> clipSignal ((size_t) (1.6 * sampleRate) / blockSize * blockSize + blockSize, 0.0f);
        for (int i = 0; i < source->getNumSamples() && clipStart + (size_t) i < clipSignal.size(); ++i)
            clipSignal[clipStart + (size_t) i] = source->getSample (0, i);
        const auto expected = runChain (reference, clipSignal);

        float worst = 0.0f;
        for (size_t i = 0; i < out[0].size() && i < expected.size() / 2; ++i)
            worst = std::max (worst, std::abs (out[0][i] - expected[(i / blockSize) * blockSize * 2 + i % blockSize]));
        check (worst < 1.0e-6f, "a track insert matches the module run on its own (max error " + juce::String (worst, 8) + ")");

        // Changing an insert value reaches the running module without a rebuild.
        edit.setInsertValue (eq, "eqOn", 0.0f);
        out = playFrom (1.6);
        check (matchesSource (out[0], 1.0f, 0.0f), "switching an insert off from the edit bypasses it");

        edit.removeInsert (eq);
        engine.rebuildNow();

        // Master bus: its inserts and volume apply to the sum.
        edit.setMasterVolume (-100.0f);
        out = playFrom (1.6);
        check (rms (out[0], clipStart + 4800, clipStart + 43200) < 1.0e-6f, "master volume");
        edit.setMasterVolume (0.0f);

        edit.addInsert (daw::Edit::masterId, fx::ModuleType::radio);
        engine.rebuildNow();
        out = playFrom (1.6);
        check (std::abs (clipRms (out[0]) / unity - 1.0f) > 0.05f && std::isfinite (clipRms (out[0])), "master inserts process the mix");

        check (engine.takePeak (vocal) > 0.3f && engine.takePeak (daw::Edit::masterId) > 0.0f
                   && engine.takePeak (vocal) == 0.0f, "track and master meters (reset after reading)");

        // Real-time safety: edit the session while another thread renders non-stop.
        {
            std::atomic<bool> running { true };
            std::atomic<bool> finite { true };
            engine.play();

            std::thread audio ([&]
            {
                juce::AudioBuffer<float> buffer (2, blockSize);
                while (running)
                {
                    engine.process (buffer, 2, blockSize);
                    if (! std::isfinite (buffer.getMagnitude (0, blockSize)))
                        finite = false;
                    if (engine.getPosition() > engine.getLength())
                        engine.seek (0);
                }
            });

            juce::Random random (42);
            for (int i = 0; i < 300; ++i)
            {
                const auto track = (int) edit.addTrack ("T" + juce::String (i))[daw::ids::id];
                edit.addClip (track, toneFile, random.nextDouble(), 0.5);
                edit.addInsert (track, (fx::ModuleType) random.nextInt (fx::numModuleTypes));
                edit.setTrackVolume (track, -12.0f);
                engine.rebuildNow();

                if (edit.getTracks().size() > 6)
                    edit.removeTrack ((int) edit.getTracks()[1][daw::ids::id]);
            }

            running = false;
            audio.join();
            engine.stop();
            check (finite, "300 edits while the audio thread renders: no glitches into NaN, no crash");
        }

        // The whole session reloads from a copy of its tree (what opening a project does).
        {
            daw::Edit copy;
            copy.replaceState (edit.getState().createCopy());
            check (copy.getTracks().size() == edit.getTracks().size()
                       && daw::Edit::getInserts (copy.getMaster()).getNumChildren() == 1
                       && copy.addTrack ("New")[daw::ids::id].operator int() > (int) edit.getTracks().getLast()[daw::ids::id],
                   "a session copies into a new edit, and new ids don't collide");
        }

        folder.deleteRecursively();
    }
    void testDawRecording()
    {
        std::cout << "\nDAW recording (dry takes, monitoring, metronome)\n";

        const auto folder = juce::File::createTempFile ("micfx-daw-rec");
        folder.createDirectory();

        daw::Edit edit;
        daw::DawEngine engine (edit);
        engine.prepare (sampleRate, blockSize);

        const auto vocal = (int) edit.addTrack ("Vocal")[daw::ids::id];
        const auto second = (int) edit.addTrack ("Double")[daw::ids::id];
        const auto eq = (int) edit.addInsert (vocal, fx::ModuleType::saturation)[daw::ids::id];
        juce::ignoreUnused (eq);
        engine.rebuildNow();

        check (engine.startRecording (folder).isNotEmpty() && ! engine.isRecording(), "refuses to record with no armed track");

        edit.setTrackArm (vocal, true);
        edit.setTrackArm (second, true);
        edit.setTrackMonitor (vocal, true);
        engine.setRecordingLatency (300);
        engine.seek ((juce::int64) sampleRate); // record from 1 s

        const auto mic = makeVoice (200.0f, 1.0);
        juce::AudioBuffer<float> buffer (2, blockSize);
        float monitored = 0.0f;

        check (engine.startRecording (folder).isEmpty() && engine.isRecording(), "starts recording on the armed tracks");

        for (size_t start = 0; start + blockSize <= mic.size(); start += blockSize)
        {
            buffer.copyFrom (0, 0, mic.data() + start, blockSize); // input aliases the left output, as in the app
            engine.process (buffer, 2, blockSize, buffer.getReadPointer (0));
            monitored = std::max (monitored, buffer.getMagnitude (1, 0, blockSize));
        }

        const auto take = engine.stopRecording();
        engine.rebuildNow();

        check (! engine.isRecording() && ! engine.isPlaying(), "stopping ends the take and the transport");
        check (monitored > 0.05f, "the monitored track plays the mic live (peak " + juce::String (monitored, 3) + ")");

        double fileRate = 0.0;
        const auto recorded = readWav (take.file, fileRate);
        const auto length = (int) (mic.size() / blockSize * blockSize);
        check (recorded.getNumChannels() == 1 && recorded.getNumSamples() == length
                   && maxDifference (recorded.getReadPointer (0), mic.data(), length) < 1.0e-6f,
               "the take is the dry mic (the saturation insert isn't printed)");

        const auto clips = [&] (int trackId)
        {
            juce::Array<juce::ValueTree> result;
            for (const auto& child : edit.getTrack (trackId))
                if (child.hasType (daw::ids::CLIP))
                    result.add (child);
            return result;
        };

        const auto expectedStart = (sampleRate - 300.0) / sampleRate;
        check (clips (vocal).size() == 1 && clips (second).size() == 1
                   && std::abs ((double) clips (vocal)[0][daw::ids::start] - expectedStart) < 1.0e-9,
               "the take lands on every armed track, at the playhead minus the round trip");

        edit.getUndoManager().undo();
        check (clips (vocal).isEmpty() && clips (second).isEmpty(), "one undo removes the take from every track");
        edit.getUndoManager().redo();

        // Playback applies the track's inserts to the dry take: the saturated vocal differs
        // from the plain second track.
        edit.setTrackArm (vocal, false);
        edit.setTrackArm (second, false);
        edit.setTrackMonitor (vocal, false);
        edit.setTrackMute (second, true);
        engine.getAudioPool().loadNow (take.file);
        engine.rebuildNow();
        engine.seek (0);
        engine.play();
        const auto wet = render (engine, 2.0);
        engine.stop();

        edit.setTrackMute (second, false);
        edit.setTrackMute (vocal, true);
        engine.seek (0);
        engine.play();
        const auto dry = render (engine, 2.0);
        engine.stop();

        const auto from = (size_t) (1.2 * sampleRate), to = (size_t) (1.8 * sampleRate);
        check (rms (dry[0], from, to) > 0.05f && std::abs (rms (wet[0], from, to) - rms (dry[0], from, to)) > 0.01f,
               "playback applies the track's inserts to the dry take");

        // Metronome: a click on every beat at the project tempo.
        edit.setTrackMute (second, true);
        edit.setTempo (120.0);
        engine.setMetronome (true);
        engine.seek (0);
        engine.play();
        const auto clicks = render (engine, 1.01);
        engine.stop();
        engine.setMetronome (false);

        const auto spb = (size_t) (sampleRate / 2.0);
        check (std::abs (clicks[0][1] ) > 0.0f && std::abs (clicks[0][spb + 1]) > 0.0f
                   && rms (clicks[0], spb / 2 - 100, spb / 2 + 100) < 1.0e-4f,
               "metronome clicks on the beat at 120 BPM");

        // Time signature: in 3/4 the accent (higher, louder click) is on every third beat.
        {
            edit.setBeatsPerBar (3);
            engine.setMetronome (true);
            engine.seek (0);
            engine.play();
            const auto bar = render (engine, 2.1);
            engine.stop();
            const auto peakAt = [&] (size_t beat) { return std::abs (*std::max_element (bar[0].begin() + (long) (beat * spb), bar[0].begin() + (long) (beat * spb + 200),
                                                                                        [] (float a, float b) { return std::abs (a) < std::abs (b); })); };
            check (peakAt (3) > peakAt (1) * 1.3f && peakAt (3) > peakAt (2) * 1.3f && peakAt (0) > peakAt (1) * 1.3f,
                   "3/4: the click accents beats 1 and 4, not 2 and 3");

            engine.setMetronomeVolume (0.5f);
            engine.seek (0);
            engine.play();
            const auto quieter = render (engine, 0.2);
            engine.stop();
            const auto loud = rms (bar[0], 0, 400), soft = rms (quieter[0], 0, 400);
            check (std::abs (soft / loud - 0.5f) < 0.05f, "click volume scales the click (" + juce::String (soft / loud, 2) + ")");
            engine.setMetronomeVolume (1.0f);
            engine.setMetronome (false);
            edit.setBeatsPerBar (4);
        }

        // Count-in: one bar of clicks before the take starts, not recorded.
        edit.setTrackArm (second, true);
        engine.setCountInBars (1);
        engine.seek (0);
        engine.startRecording (folder);
        const auto countIn = (int) (4 * spb);
        juce::AudioBuffer<float> silence (2, blockSize);
        int blocks = 0;
        float countInPeak = 0.0f;

        for (; blocks * blockSize < countIn + (int) sampleRate / 2; ++blocks)
        {
            silence.clear();
            const float* mute = silence.getReadPointer (1); // silent mic (not the click on the left)
            engine.process (silence, 2, blockSize, mute);

            if (blocks * blockSize < countIn)
                countInPeak = std::max (countInPeak, silence.getMagnitude (0, 0, blockSize));
        }

        const auto countTake = engine.stopRecording();
        const auto countFile = readWav (countTake.file, fileRate);
        check (countFile.getNumSamples() > 0 && std::abs (countFile.getNumSamples() - (blocks * blockSize - countIn)) <= blockSize
                   && countTake.timelineStart == 0 && countInPeak > 0.1f,
               "count-in clicks for a bar first, and the take starts at the playhead");

        folder.deleteRecursively();
    }
    void testProjects()
    {
        std::cout << "\nDAW projects (save, open, autosave, recovery)\n";

        const auto root = juce::File::createTempFile ("micfx-projects");
        root.createDirectory();

        // An audio file that lives outside any project, as an import would.
        juce::AudioBuffer<float> tone (1, (int) sampleRate / 2);
        for (int i = 0; i < tone.getNumSamples(); ++i)
            tone.setSample (0, i, 0.3f * (float) std::sin (0.05 * i));
        const auto outside = root.getChildFile ("loop.wav");
        writeWav (outside, tone, sampleRate);

        daw::Edit edit;
        daw::Project project (edit);

        check (project.createNew (root.getChildFile ("Song")).isEmpty()
                   && project.getFolder().getFileName() == "Song.micfx"
                   && project.getAudioFolder().isDirectory() && project.getFolder().getChildFile ("project.xml").existsAsFile(),
               "creates a project folder (Song.micfx with Audio, Backups, Exports)");

        const auto imported = project.importAudio (outside);
        check (imported.isAChildOf (project.getAudioFolder()) && imported.getSize() == outside.getSize(),
               "importing copies the file into Audio/");

        const auto track = (int) edit.addTrack ("Vocal")[daw::ids::id];
        edit.setTrackVolume (track, -3.5f);
        edit.setTrackPan (track, 0.25f);
        edit.addClip (track, imported, 1.25, 0.5);
        const auto insert = (int) edit.addInsert (track, fx::ModuleType::echo)[daw::ids::id];
        edit.setInsertValue (insert, "delayTime", 420.0f);
        edit.addInsert (daw::Edit::masterId, fx::ModuleType::compressor);
        edit.setTempo (96.0);
        edit.setBeatsPerBar (3);
        const auto chorus = (int) edit.addMarker (8.5, "Chorus")[daw::ids::id];
        edit.addMarker (2.0);
        edit.setMarkerName (chorus, "  Chorus 1 ");

        check (project.hasUnsavedChanges() && project.save().isEmpty() && ! project.hasUnsavedChanges(), "saves, and tracks unsaved changes");

        const auto xmlText = project.getFolder().getChildFile ("project.xml").loadFileAsString();
        check (xmlText.contains ("file=\"Audio/loop.wav\""), "clip files inside the project are stored relative");

        const auto saved = edit.getState().createCopy();
        project.close();

        // Move the whole folder, then open it: relative paths still resolve.
        const auto moved = root.getChildFile ("Moved.micfx");
        project.getFolder(); // closed: empty
        check (root.getChildFile ("Song.micfx").moveFileTo (moved), "(moved the project folder)");

        daw::Edit reopened;
        daw::Project second (reopened);
        check (second.open (moved).isEmpty(), "opens a project");

        const auto reTrack = reopened.getTrack (track);
        const auto reClip = reTrack.getChildWithName (daw::ids::CLIP);
        check (reopened.getTracks().size() == 1 && (float) reTrack[daw::ids::volume] == -3.5f
                   && (float) reTrack[daw::ids::pan] == 0.25f && juce::approximatelyEqual (reopened.getTempo(), 96.0)
                   && (double) reClip[daw::ids::start] == 1.25
                   && (float) reopened.getInsert (insert)[juce::Identifier ("delayTime")] == 420.0f
                   && daw::Edit::getInserts (reopened.getMaster()).getNumChildren() == 1,
               "tracks, clips, inserts, master and tempo come back as saved");
        check (juce::File (reClip[daw::ids::file].toString()) == moved.getChildFile ("Audio/loop.wav") && second.getMissingFiles().isEmpty(),
               "a moved project still finds its audio");
        check (! reopened.getUndoManager().canUndo() && ! second.hasUnsavedChanges(), "opening starts with a clean undo history");

        {
            const auto markers = reopened.getMarkers();
            check (reopened.getBeatsPerBar() == 3 && markers.size() == 2 && (double) markers[0][daw::ids::time] == 2.0
                       && markers[0][daw::ids::name].toString() == "Marker 2" && markers[1][daw::ids::name].toString() == "Chorus 1"
                       && (double) markers[1][daw::ids::time] == 8.5,
                   "the time signature and markers come back, markers in time order");
            reopened.setMarkerTime (chorus, 1.0);
            check (reopened.getMarkers()[0][daw::ids::id].equals (chorus), "moving a marker keeps the list in time order");
            reopened.removeMarker (chorus);
            reopened.getUndoManager().undo();
            check (reopened.getMarkers().size() == 2, "deleting a marker can be undone");
        }

        // The engine plays the reopened project.
        {
            daw::DawEngine engine (reopened);
            engine.prepare (sampleRate, blockSize);
            engine.getAudioPool().loadNow (juce::File (reClip[daw::ids::file].toString()));
            engine.rebuildNow();
            engine.seek ((juce::int64) (1.25 * sampleRate));
            engine.play();
            const auto out = render (engine, 0.3);
            check (rms (out[0], 2400, out[0].size()) > 0.05f, "the reopened project plays");
        }

        // Save as copies the audio into the new folder.
        const auto copy = root.getChildFile ("Copy");
        check (second.saveAs (copy).isEmpty() && second.getFolder().getFileName() == "Copy.micfx"
                   && second.getAudioFolder().getChildFile ("loop.wav").existsAsFile()
                   && juce::File (reopened.getTrack (track).getChildWithName (daw::ids::CLIP)[daw::ids::file].toString())
                          .isAChildOf (second.getFolder()),
               "save as copies the audio and switches to the new folder");

        // Autosave and crash recovery: a backup newer than the save, and no clean close.
        reopened.setTrackVolume (track, -6.0f);
        check (second.writeBackup().isEmpty() && second.getBackupsFolder().findChildFiles (juce::File::findFiles, false, "*.xml").size() == 1,
               "autosave writes a backup");

        for (int i = 0; i < daw::Project::numBackups + 2; ++i)
            second.writeBackup();
        check (second.getBackupsFolder().findChildFiles (juce::File::findFiles, false, "*.xml").size() == daw::Project::numBackups,
               "keeps the newest " + juce::String (daw::Project::numBackups) + " backups");

        const auto crashedFolder = second.getFolder();

        // Backups older than the last save aren't offered, even after a crash.
        second.save();
        {
            daw::Edit afterSave;
            daw::Project probe (afterSave);
            check (probe.open (crashedFolder).isEmpty() && ! probe.getRecoverableBackup().exists(),
                   "backups older than the last save aren't offered");
        }

        second.save(); // the probe's close removed the lock; the open session writes it again
        reopened.setTrackVolume (track, -9.0f);
        second.writeBackup();
        // Simulate a crash: no close(), the lock file stays. A fresh app opens the project.
        {
            daw::Edit afterCrash;
            daw::Project third (afterCrash);
            check (third.open (crashedFolder).isEmpty() && third.getRecoverableBackup().existsAsFile(),
                   "after a crash, opening offers the newest backup");
            check (third.restoreBackup (third.getRecoverableBackup()).isEmpty()
                       && (float) afterCrash.getTrack (track)[daw::ids::volume] == -9.0f && third.hasUnsavedChanges(),
                   "restoring the backup brings back the unsaved change");
            third.close();

            daw::Edit clean;
            daw::Project fourth (clean);
            check (fourth.open (crashedFolder).isEmpty() && ! fourth.getRecoverableBackup().exists(),
                   "after a clean close, nothing to recover");
        }

        // Projects from a newer version, and folders that aren't projects, are refused.
        {
            daw::Edit future;
            future.getState().setProperty (daw::ids::version, daw::Edit::formatVersion + 1, nullptr);
            const auto futureFolder = root.getChildFile ("Future.micfx");
            futureFolder.createDirectory();
            future.getState().createXml()->writeTo (futureFolder.getChildFile ("project.xml"));

            daw::Edit target;
            daw::Project loader (target);
            check (loader.open (futureFolder).contains ("newer version") && loader.open (root).isNotEmpty() && ! loader.isOpen(),
                   "refuses newer-format projects and non-project folders");
        }

        root.deleteRecursively();
    }
    void testExport()
    {
        std::cout << "\nDAW export (mixdown and stems)\n";

        const auto folder = juce::File::createTempFile ("micfx-export");
        folder.createDirectory();

        juce::AudioBuffer<float> tone (1, (int) sampleRate);
        for (int i = 0; i < tone.getNumSamples(); ++i)
            tone.setSample (0, i, 0.3f * (float) std::sin (juce::MathConstants<double>::twoPi * 220.0 * i / sampleRate));
        const auto toneFile = folder.getChildFile ("tone.wav");
        writeWav (toneFile, tone, sampleRate);

        daw::Edit edit;
        const auto a = (int) edit.addTrack ("Lead")[daw::ids::id];
        const auto b = (int) edit.addTrack ("Harmony/2")[daw::ids::id];
        edit.addClip (a, toneFile, 0.5, 1.0);
        edit.addClip (b, toneFile, 1.0, 1.0);
        edit.setTrackPan (b, 0.5f);
        const auto echo = (int) edit.addInsert (a, fx::ModuleType::echo)[daw::ids::id];
        edit.setInsertValue (echo, "delayTime", 250.0f);
        edit.addInsert (daw::Edit::masterId, fx::ModuleType::eq);
        edit.setInsertValue ((int) daw::Edit::getInserts (edit.getMaster()).getChild (0)[daw::ids::id], "eqHigh", -6.0f);

        // What real-time playback sounds like, for comparison.
        std::array<std::vector<float>, 2> live;
        {
            daw::DawEngine engine (edit);
            engine.prepare (sampleRate, blockSize);
            engine.loadAllNow();
            engine.play();
            live = render (engine, 3.0);
        }

        daw::Exporter::Settings settings;
        settings.sampleRate = sampleRate;
        settings.bitDepth = 32;
        settings.tail = 1.0;

        std::vector<double> progress;
        const auto mixFile = folder.getChildFile ("Exports/mix.wav");
        const auto mix = daw::Exporter (edit, settings).exportMix (mixFile, [&] (double p) { progress.push_back (p); return true; });

        double fileRate = 0.0;
        const auto written = readWav (mixFile, fileRate);
        const auto expectedLength = (int) (3.0 * sampleRate); // last clip ends at 2 s, plus 1 s tail
        check (mix.error.isEmpty() && written.getNumChannels() == 2 && written.getNumSamples() == expectedLength
                   && juce::exactlyEqual (fileRate, sampleRate),
               "exports the mix to a stereo WAV: the song plus the tail (" + juce::String (written.getNumSamples()) + " samples)");

        float worst = 0.0f;
        for (int ch = 0; ch < 2; ++ch)
            worst = std::max (worst, maxDifference (written.getReadPointer (ch), live[(size_t) ch].data(), expectedLength));
        check (worst < 1.0e-5f, "the export matches real-time playback (max error " + juce::String (worst, 7) + ")");

        check (! progress.empty() && std::is_sorted (progress.begin(), progress.end()) && juce::approximatelyEqual (progress.back(), 1.0),
               "reports progress up to 100%");

        // 16-bit with normalising.
        settings.bitDepth = 16;
        settings.normalise = true;
        settings.normaliseTo = -1.0f;
        const auto loudFile = folder.getChildFile ("Exports/loud.wav");
        const auto loud = daw::Exporter (edit, settings).exportMix (loudFile);
        const auto loudAudio = readWav (loudFile, fileRate);
        const auto loudPeak = juce::Decibels::gainToDecibels (loudAudio.getMagnitude (0, loudAudio.getNumSamples()));
        check (loud.error.isEmpty() && std::abs (loudPeak + 1.0f) < 0.05f, "normalises a 16-bit export to -1 dBFS (peak " + juce::String (loudPeak, 2) + " dB)");

        // Stems: one file per track, which sum to the mix without the master bus.
        settings.bitDepth = 32;
        settings.normalise = false;
        const auto stems = daw::Exporter (edit, settings).exportStems (folder.getChildFile ("Stems"));
        check (stems.error.isEmpty() && stems.files.size() == 2 && stems.files[0].getFileName() == "01 Lead.wav"
                   && stems.files[1].getFileName().startsWith ("02 Harmony") && ! stems.files[1].getFileName().containsChar ('/'),
               "exports one stem per track, named after it");

        {
            daw::Edit bare;
            bare.replaceState (edit.getState().createCopy());
            daw::Edit::getInserts (bare.getMaster()).removeAllChildren (nullptr);
            const auto bareFile = folder.getChildFile ("bare.wav");
            daw::Exporter (bare, settings).exportMix (bareFile);

            const auto bareMix = readWav (bareFile, fileRate);
            const auto stem1 = readWav (stems.files[0], fileRate), stem2 = readWav (stems.files[1], fileRate);
            float sumError = 0.0f;
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < bareMix.getNumSamples(); ++i)
                    sumError = std::max (sumError, std::abs (stem1.getSample (ch, i) + stem2.getSample (ch, i) - bareMix.getSample (ch, i)));
            check (stem1.getNumSamples() == bareMix.getNumSamples() && sumError < 1.0e-5f, "the stems sum to the mix before the master bus");
        }

        // A range, and cancelling.
        settings.start = 1.0;
        settings.end = 1.5;
        settings.tail = 0.0;
        const auto rangeFile = folder.getChildFile ("range.wav");
        daw::Exporter (edit, settings).exportMix (rangeFile);
        check (readWav (rangeFile, fileRate).getNumSamples() == (int) (0.5 * sampleRate), "exports just a time range");

        const auto cancelFile = folder.getChildFile ("cancelled.wav");
        const auto cancelled = daw::Exporter (edit, settings).exportMix (cancelFile, [] (double p) { return p < 0.3; });
        check (cancelled.cancelled && ! cancelFile.exists(), "cancelling stops and removes the partial file");

        folder.deleteRecursively();
    }
    // The DAW screen's commands, through DawBridge as the page sends them: tracks, import,
    // new / save as / open (the file chooser's part is skipped: the tests call what runs
    // after a file is picked), and the session events the page redraws from.
    void testDawBridge()
    {
        std::cout << "\nDAW screen commands (DawBridge)\n";

        const auto folder = juce::File::getSpecialLocation (juce::File::tempDirectory).getNonexistentChildFile ("micfx-bridge", {});
        folder.createDirectory();

        juce::PropertiesFile::Options options;
        options.applicationName = "MicFXTests";
        options.filenameSuffix = ".settings";
        juce::PropertiesFile settings (folder.getChildFile ("test.settings"), options);

        PluginProcessor processor;
        processor.prepareToPlay (sampleRate, blockSize);
        auto& edit = processor.getDawEdit();
        auto& project = processor.getDawProject();

        std::vector<std::pair<juce::String, juce::var>> events;
        DawBridge::availableForTesting = true;
        DawBridge bridge (processor, settings,
                          [&] (const juce::Identifier& event, const juce::var& payload) { events.emplace_back (event.toString(), payload); },
                          [] { return (juce::int64) 0; });

        // Runs a command and returns what it completed with (every command completes).
        const auto run = [&] (const juce::String& command, juce::var args = juce::var (new juce::DynamicObject()))
        {
            juce::var result;
            auto completed = false;
            bridge.handle (command, args, [&] (juce::var r) { result = r; completed = true; });
            return completed ? result : juce::var ("never completed");
        };
        const auto args = [] (std::initializer_list<std::pair<const char*, juce::var>> properties)
        {
            auto* object = new juce::DynamicObject();
            for (const auto& [name, value] : properties)
                object->setProperty (name, value);
            return juce::var (object);
        };
        // The last "dawSession" the page got, after a tick (the editor's 30 Hz timer), parsed
        // back from JSON the way the WebView receives it.
        const auto lastSession = [&]
        {
            events.clear();
            bridge.tick();
            for (const auto& [name, payload] : events)
                if (name == "dawSession")
                    return juce::JSON::parse (juce::JSON::toString (payload, true));
            return juce::var();
        };
        const auto trackCount = [] (const juce::var& session) { return session["tracks"].size(); };
        const auto ok = [] (const juce::var& result) { return result.isObject() && (bool) result["ok"]; };

        run ("mode", args ({ { "daw", true } }));
        auto session = lastSession();
        check (processor.isDawMode() && session["mode"].toString() == "daw" && trackCount (session) == 1,
               "DAW mode starts with one track");

        // Add track: completes with the new id, and the page gets a session with it.
        const auto added = run ("addTrack");
        session = lastSession();
        check (added.isInt() && edit.getTrack ((int) added).isValid() && trackCount (session) == 2
                   && (int) session["tracks"][1]["id"] == (int) added && session["tracks"][1]["name"].toString() == "Track 2",
               "Add track adds a track and sends it to the page");
        check (session.isObject() && juce::JSON::toString (session).isNotEmpty() && ! juce::JSON::toString (session).contains ("undefined"),
               "the session is valid JSON");
        run ("undo");
        check (trackCount (lastSession()) == 1, "Add track undoes in one step");
        run ("redo");

        // Import (no project open): the file is used where it is, on the chosen track.
        const auto song = folder.getChildFile ("Song.wav");
        const auto songAudio = makeTones ({ { 2.5, 330.0, 0.2f, 0.2f } });
        writeWav (song, songAudio, 44100.0); // not the device rate
        const auto songSeconds = songAudio.getNumSamples() / 44100.0;
        const auto vocal = (int) edit.getTracks()[0][daw::ids::id];
        auto imported = bridge.importAudio (song, vocal);
        session = lastSession();
        check (ok (imported) && (int) imported["track"] == vocal && session["tracks"][0]["clips"].size() == 1
                   && std::abs ((double) session["tracks"][0]["clips"][0]["length"] - songSeconds) < 1.0e-6
                   && session["tracks"][0]["clips"][0]["file"].toString() == song.getFullPathName(),
               "Import puts the file on the selected track, at its full length");

        // ...and with no track picked, on a new track named after the file.
        imported = bridge.importAudio (song, -1);
        session = lastSession();
        check (ok (imported) && trackCount (session) == 3 && session["tracks"][2]["name"].toString() == "Song"
                   && session["tracks"][2]["clips"].size() == 1,
               "Import with no track selected adds a track for the file");
        run ("undo");
        check (trackCount (lastSession()) == 2, "Import undoes in one step (clip and its new track)");

        const auto notAudio = folder.getChildFile ("notes.wav");
        notAudio.replaceWithText ("not audio");
        imported = bridge.importAudio (notAudio, vocal);
        check (! ok (imported) && imported["error"].toString().contains ("notes.wav") && edit.getTrack (vocal).getNumChildren() == 2,
               "Import reports a file it can't read, and adds nothing");

        // New project: an empty session with one track, saved, autosaving.
        const auto projects = folder.getChildFile ("Projects");
        projects.createDirectory();
        auto result = bridge.newProject (projects.getChildFile ("First"));
        session = lastSession();
        const auto first = projects.getChildFile ("First.micfx");
        check (ok (result) && project.getFolder() == first && first.getChildFile ("project.xml").existsAsFile()
                   && trackCount (session) == 1 && session["project"]["name"].toString() == "First"
                   && settings.getValue ("dawProject") == first.getFullPathName(),
               "New project creates First.micfx with one track and remembers it");
        check (project.isAutosaving(), "autosave runs after New project");

        // Import into a project copies the file into its Audio folder.
        bridge.handle ("addTrack", juce::var (new juce::DynamicObject()), [] (juce::var) {});
        imported = bridge.importAudio (song, -1);
        const auto copied = first.getChildFile ("Audio").getChildFile ("Song.wav");
        check (ok (imported) && copied.existsAsFile() && edit.getClip ((int) imported["clip"])[daw::ids::file].toString() == copied.getFullPathName(),
               "Import into a project copies the file into Audio/");
        check (ok (run ("save")) && ! project.hasUnsavedChanges(), "Save writes the project");

        // Save as: a copy with its own audio, now the open project.
        result = bridge.saveProjectAs (projects.getChildFile ("Second"));
        const auto second = projects.getChildFile ("Second.micfx");
        check (ok (result) && project.getFolder() == second && second.getChildFile ("Audio").getChildFile ("Song.wav").existsAsFile(),
               "Save as copies the project and its audio");

        // Open: whatever the file chooser hands back (the folder, project.xml, a file inside).
        for (const auto& picked : { first, first.getChildFile ("project.xml"), copied })
        {
            bridge.newProject (projects.getChildFile ("Scratch"));
            result = bridge.openProject (picked);
            session = lastSession();
            check (ok (result) && project.getFolder() == first && trackCount (session) == 3
                       && session["tracks"][2]["clips"].size() == 1 && ! (bool) session["tracks"][2]["clips"][0]["missing"]
                       && session["project"]["name"].toString() == "First" && settings.getValue ("dawProject") == first.getFullPathName(),
                   "Open works when the chooser returns " + picked.getRelativePathFrom (projects));
        }

        // A project read back from XML must reach the page typed like one built in memory:
        // volumes as text ("0.0") broke the page's drawing, so nothing showed after Open.
        const auto typed = [] (const juce::var& track)
        {
            return ! track["volume"].isString() && ! track["pan"].isString() && track["mute"].isBool() && track["arm"].isBool()
                && track["id"].isInt() && ! track["clips"][0]["start"].isString() && ! track["clips"][0]["length"].isString();
        };
        check (typed (session["tracks"][2]) && ! session["master"]["volume"].isString() && ! session["loop"]["end"].isString()
                   && edit.getTracks()[0][daw::ids::volume].isDouble() && edit.getTracks()[0][daw::ids::mute].isBool(),
               "an opened project has numbers and switches, not text");
        check (project.isAutosaving(), "autosave keeps running after Open");

        // Picking something that isn't a project: an error, and the open project stays.
        result = bridge.openProject (projects);
        check (! ok (result) && result["error"].toString().contains ("isn't a MicFX project") && project.getFolder() == first,
               "Open explains when the pick isn't a project, and keeps the current one");

        // An older project: no version, no effects lists, a folder without .micfx, an
        // absolute clip path, a clip without an id.
        const auto legacy = projects.getChildFile ("Old Song");
        legacy.createDirectory();
        legacy.getChildFile ("project.xml").replaceWithText (
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<PROJECT tempo=\"100.0\">\n"
            "  <MASTER id=\"0\" volume=\"0.0\"/>\n"
            "  <TRACK id=\"3\" name=\"Lead\" volume=\"-3.0\" pan=\"0.0\" mute=\"1\">\n"
            "    <CLIP name=\"Song\" file=\"" + song.getFullPathName() + "\" start=\"1.0\" offset=\"0.0\" length=\"2.0\"/>\n"
            "  </TRACK>\n"
            "</PROJECT>\n");
        result = bridge.openProject (legacy.getChildFile ("project.xml"));
        session = lastSession();
        check (ok (result) && trackCount (session) == 1 && session["tracks"][0]["name"].toString() == "Lead"
                   && session["tracks"][0]["clips"].size() == 1 && (int) session["tracks"][0]["clips"][0]["id"] > 3
                   && typed (session["tracks"][0]) && session["tracks"][0]["mute"] == juce::var (true)
                   && juce::approximatelyEqual ((double) session["tracks"][0]["volume"], -3.0),
               "Opens an older project (no version, no effects lists, clip without an id)");

        const auto legacyTrack = (int) session["tracks"][0]["id"];
        const auto insert = run ("addInsert", args ({ { "track", legacyTrack }, { "type", "compressor" } }));
        check (insert.isInt() && (int) insert > 0 && lastSession()["tracks"][0]["inserts"].size() == 1,
               "an older project's tracks take effects");
        check (trackCount (session) == 1 && run ("addTrack").isInt() && trackCount (lastSession()) == 2,
               "Add track works in an older project");

        // Starting the app again reopens the last project.
        {
            PluginProcessor again;
            again.prepareToPlay (sampleRate, blockSize);
            DawBridge reopened (again, settings, [] (const juce::Identifier&, const juce::var&) {}, [] { return (juce::int64) 0; });
            check (again.getDawProject().getFolder() == legacy && again.isDawMode() && again.getDawProject().isAutosaving(),
                   "the app reopens the last project in DAW mode, autosaving");
            again.getDawProject().close();
        }

        project.close();
        DawBridge::availableForTesting = false;
        folder.deleteRecursively();
    }

    void testDawMode()
    {
        std::cout << "\nDAW mode in the processor\n";

        PluginProcessor processor;
        processor.prepareToPlay (sampleRate, blockSize);

        auto& edit = processor.getDawEdit();
        const auto vocal = (int) edit.addTrack ("Vocal")[daw::ids::id];
        edit.setTrackMonitor (vocal, true);
        processor.getDawEngine().rebuildNow();

        const auto mic = makeVoice (200.0f, 1.0);
        const auto run = [&]
        {
            std::vector<float> left, right;
            juce::AudioBuffer<float> buffer (2, blockSize);
            juce::MidiBuffer midi;

            for (size_t start = 0; start + blockSize <= mic.size(); start += blockSize)
            {
                buffer.clear();
                buffer.copyFrom (0, 0, mic.data() + start, blockSize); // the mic arrives on channel 0
                processor.processBlock (buffer, midi);
                left.insert (left.end(), buffer.getReadPointer (0), buffer.getReadPointer (0) + blockSize);
                right.insert (right.end(), buffer.getReadPointer (1), buffer.getReadPointer (1) + blockSize);
            }

            return std::pair { left, right };
        };

        processor.setDawMode (true);
        const auto [left, right] = run();
        const auto skip = (size_t) (0.1 * sampleRate); // past the gain smoothing
        const auto inLevel = rms (mic, skip, left.size());
        const auto ratio = rms (left, skip, left.size()) / inLevel;

        float error = 0.0f;
        for (auto i = skip; i < left.size(); ++i)
            error = std::max ({ error, std::abs (left[i] - mic[i] * ratio), std::abs (right[i] - left[i]) });

        check (ratio > 0.5f && ratio < 1.5f && error < 1.0e-3f,
               "the mic plays through the monitored track, centred and untouched (gain " + juce::String (ratio, 3)
                   + ", error " + juce::String (error, 6) + ")");

        edit.setTrackMonitor (vocal, false);
        const auto [quietLeft, quietRight] = run();
        check (rms (quietLeft, skip, quietLeft.size()) < 1.0e-6f && rms (quietRight, skip, quietRight.size()) < 1.0e-6f,
               "nothing is heard when no track monitors the mic");

        processor.setDawMode (false);
        const auto [liveLeft, liveRight] = run();
        juce::ignoreUnused (liveRight);
        check (rms (liveLeft, skip, liveLeft.size()) > 0.01f, "back in Live mode the mic goes through the Live chain again");
    }

    void testLiveControls()
    {
        std::cout << "\nLive controls (panic mute, dry compare)\n";

        PluginProcessor processor;
        processor.prepareToPlay (sampleRate, blockSize);
        processor.getState().getParameter ("satOn")->setValueNotifyingHost (1.0f);
        processor.getState().getParameter ("satDrive")->setValueNotifyingHost (1.0f);
        processor.getState().getParameter ("reverbOn")->setValueNotifyingHost (1.0f);

        const auto mic = makeVoice (200.0f, 0.5);
        const auto run = [&]
        {
            std::vector<float> out;
            juce::AudioBuffer<float> buffer (2, blockSize);
            juce::MidiBuffer midi;
            for (size_t start = 0; start + blockSize <= mic.size(); start += blockSize)
            {
                buffer.clear();
                buffer.copyFrom (0, 0, mic.data() + start, blockSize);
                processor.processBlock (buffer, midi);
                out.insert (out.end(), buffer.getReadPointer (0), buffer.getReadPointer (0) + blockSize);
            }
            return out;
        };

        const auto skip = (size_t) (0.15 * sampleRate);
        const auto wet = run();
        processor.dryCompare = true;
        const auto dry = run();
        processor.dryCompare = false;

        // The reference: every effect switched off (input/output gain and the limiter still run).
        juce::StringArray wasOn;
        for (auto* p : processor.getParameters())
            if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (p); ranged != nullptr && ranged->getParameterID().endsWith ("On") && ranged->getValue() > 0.5f)
            {
                wasOn.add (ranged->getParameterID());
                ranged->setValueNotifyingHost (0.0f);
            }
        const auto plain = run();
        for (const auto& id : wasOn)
            processor.getState().getParameter (id)->setValueNotifyingHost (1.0f);

        float worstDry = 0.0f, worstWet = 0.0f;
        for (auto i = skip; i < dry.size(); ++i)
        {
            worstDry = std::max (worstDry, std::abs (dry[i] - plain[i]));
            worstWet = std::max (worstWet, std::abs (wet[i] - plain[i]));
        }
        check (worstDry < 1.0e-3f && worstWet > 0.05f, "dry compare skips the effects (dry error " + juce::String (worstDry, 5)
                                                         + ", with effects " + juce::String (worstWet, 3) + ")");

        processor.outputMuted = true;
        const auto muted = run();
        processor.outputMuted = false;
        check (rms (muted, 0, muted.size()) == 0.0f, "the panic mute silences the output");
        check (rms (run(), skip, mic.size() - blockSize) > 0.01f, "and unmuting brings it back");
    }

    void testTrackTools()
    {
        std::cout << "\nDAW track tools (duplicate, copy effects, solo, clear)\n";

        daw::Edit edit;
        const auto a = (int) edit.addTrack ("Lead")[daw::ids::id];
        const auto b = (int) edit.addTrack ("Backing")[daw::ids::id];
        edit.setTrackVolume (a, -4.0f);
        edit.setTrackArm (a, true);
        edit.addClip (a, juce::File ("/tmp/x.wav"), 1.0, 2.0);
        const auto echo = (int) edit.addInsert (a, fx::ModuleType::echo)[daw::ids::id];
        edit.setInsertValue (echo, "delayTime", 300.0f);
        edit.addInsert (a, fx::ModuleType::reverb);

        edit.getUndoManager().beginNewTransaction();
        const auto copy = edit.duplicateTrack (a);
        const auto copyTree = edit.getTrack (copy);
        const auto copyInserts = daw::Edit::getInserts (copyTree);
        check (copy > 0 && edit.getTracks().size() == 3 && edit.getTracks()[1] == copyTree
                   && copyTree[daw::ids::name].toString() == "Lead copy" && (float) copyTree[daw::ids::volume] == -4.0f
                   && ! (bool) copyTree[daw::ids::arm] && copyInserts.getNumChildren() == 2
                   && (float) copyInserts.getChild (0)[juce::Identifier ("delayTime")] == 300.0f
                   && copyTree.getChildWithName (daw::ids::CLIP).isValid(),
               "duplicating a track copies its clips, effects and mix settings, right below it");
        check ((int) copyInserts.getChild (0)[daw::ids::id] != echo
                   && (int) copyTree.getChildWithName (daw::ids::CLIP)[daw::ids::id] != (int) edit.getTrack (a).getChildWithName (daw::ids::CLIP)[daw::ids::id],
               "the copy gets its own ids");
        edit.getUndoManager().undo();
        check (edit.getTracks().size() == 2, "duplicate is one undo step");

        edit.addInsert (b, fx::ModuleType::compressor);
        edit.copyInserts (a, b);
        const auto bInserts = daw::Edit::getInserts (edit.getTrack (b));
        check (bInserts.getNumChildren() == 2 && bInserts.getChild (0)[daw::ids::type].toString() == daw::Edit::getInserts (edit.getTrack (a)).getChild (0)[daw::ids::type].toString()
                   && (float) bInserts.getChild (0)[juce::Identifier ("delayTime")] == 300.0f,
               "pasting effects replaces the other track's chain with a copy");
        edit.copyInserts (a, daw::Edit::masterId);
        check (daw::Edit::getInserts (edit.getMaster()).getNumChildren() == 2, "effects can be pasted on the master");

        edit.setTrackMute (b, true);
        edit.soloExclusive (b);
        check ((bool) edit.getTrack (b)[daw::ids::solo] && ! (bool) edit.getTrack (a)[daw::ids::solo], "exclusive solo");
        edit.clearMuteAndSolo();
        check (! (bool) edit.getTrack (b)[daw::ids::solo] && ! (bool) edit.getTrack (b)[daw::ids::mute], "clear every mute and solo");
    }

    void testClipTools()
    {
        std::cout << "\nDAW clip tools (mute, normalise, strip silence, anti-click edges)\n";

        const auto folder = juce::File::createTempFile ("micfx-cliptools");
        folder.createDirectory();

        // 0.5 s silence, 1 s tone (with a 0.1 s gap in it), 1 s silence, 0.5 s tone, 0.5 s silence.
        juce::AudioBuffer<float> take (1, (int) (sampleRate * 3.5));
        take.clear();
        const auto tone = [&] (double from, double to, float level)
        {
            for (auto i = (int) (from * sampleRate); i < (int) (to * sampleRate); ++i)
                take.setSample (0, i, level * (float) std::sin (0.06 * i));
        };
        tone (0.5, 1.0, 0.25f);
        tone (1.1, 1.5, 0.25f);
        tone (2.5, 3.0, 0.5f);
        const auto file = folder.getChildFile ("take.wav");
        writeWav (file, take, sampleRate);

        daw::Edit edit;
        daw::DawEngine engine (edit);
        engine.prepare (sampleRate, blockSize);
        const auto audio = engine.getAudioPool().loadNow (file);

        const auto total = (juce::int64) take.getNumSamples();
        const auto gain = daw::cliptools::getNormaliseGain (*audio, 0, total);
        check (std::abs (gain - (-1.0f - juce::Decibels::gainToDecibels (0.5f))) < 0.1f, "normalise sets the gain so the peak lands on -1 dB ("
                                                                                             + juce::String (gain, 2) + " dB)");
        check (daw::cliptools::getNormaliseGain (*audio, 0, (juce::int64) (0.4 * sampleRate)) == 0.0f, "normalising silence leaves the gain alone");

        const auto regions = daw::cliptools::findSound (*audio, 0, total, sampleRate);
        check (regions.size() == 2 && std::abs (regions[0].start - 0.44) < 0.03 && std::abs (regions[0].end - 1.56) < 0.03
                   && std::abs (regions[1].start - 2.44) < 0.03 && std::abs (regions[1].end - 3.06) < 0.03,
               "finds the two phrases, bridging the short gap and padding the edges");

        const auto track = (int) edit.addTrack ("Vox")[daw::ids::id];
        const auto clip = (int) edit.addClip (track, file, 1.0, 3.5)[daw::ids::id];
        std::vector<std::pair<double, double>> parts;
        for (const auto& r : regions)
            parts.emplace_back (r.start, r.end);
        edit.getUndoManager().beginNewTransaction();
        const auto created = edit.keepClipParts (clip, parts);
        const auto first = edit.getClip (created[0]), second = edit.getClip (created[1]);
        check (created.size() == 2 && ! edit.getClip (clip).isValid()
                   && std::abs ((double) first[daw::ids::start] - (1.0 + regions[0].start)) < 1e-9
                   && std::abs ((double) second[daw::ids::offset] - regions[1].start) < 1e-9
                   && (double) first[daw::ids::fadeIn] > 0.0 && (double) second[daw::ids::fadeOut] > 0.0,
               "strip silence keeps the phrases as clips in place, with short fades");
        edit.getUndoManager().undo();
        check (edit.getClip (clip).isValid() && edit.getTracks()[0].getNumChildren() == 2, "strip silence is one undo step");

        // Muting a clip silences it but keeps it.
        engine.loadAllNow();
        const auto loudness = [&]
        {
            engine.seek ((juce::int64) (3.5 * sampleRate));
            engine.play();
            const auto out = render (engine, 0.5);
            engine.stop();
            return rms (out[0], 0, out[0].size());
        };
        const auto before = loudness();
        edit.setClipMute (clip, true);
        engine.rebuildNow();
        check (before > 0.1f && loudness() < 1.0e-6f && edit.getClip (clip).isValid(), "a muted clip is silent but stays");
        edit.setClipMute (clip, false);

        // A clip that starts inside its file fades in over a few ms instead of clicking.
        edit.trimClipStart (clip, 1.0 + 2.6); // in the middle of the loud tone
        engine.rebuildNow();
        engine.seek ((juce::int64) (3.6 * sampleRate));
        engine.play();
        const auto edge = render (engine, 0.02);
        engine.stop();
        const auto fade = (size_t) (daw::DawEngine::antiClickSeconds * sampleRate);
        check (std::abs (edge[0][0]) < 0.01f && rms (edge[0], fade, fade * 3) > 0.2f,
               "a clip cut into the audio fades in at its edge (no click)");

        folder.deleteRecursively();
    }

    void testClipEditing()
    {
        std::cout << "\nDAW clip editing (move, trim, split, fades, snap, loop)\n";

        const auto folder = juce::File::createTempFile ("micfx-edit");
        folder.createDirectory();

        // A ramp makes it easy to see which part of the file plays where.
        juce::AudioBuffer<float> ramp (1, (int) sampleRate * 2);
        for (int i = 0; i < ramp.getNumSamples(); ++i)
            ramp.setSample (0, i, 0.1f + 0.4f * (float) i / (float) ramp.getNumSamples());
        const auto rampFile = folder.getChildFile ("ramp.wav");
        writeWav (rampFile, ramp, sampleRate);

        daw::Edit edit;
        daw::DawEngine engine (edit);
        engine.prepare (sampleRate, blockSize);
        const auto source = engine.getAudioPool().loadNow (rampFile);

        const auto a = (int) edit.addTrack ("A")[daw::ids::id];
        const auto b = (int) edit.addTrack ("B")[daw::ids::id];
        const auto clip = (int) edit.addClip (a, rampFile, 1.0, 2.0)[daw::ids::id];
        const auto get = [&] (int id, const juce::Identifier& p) { return (double) edit.getClip (id)[p]; };

        edit.getUndoManager().beginNewTransaction();
        edit.trimClipStart (clip, 1.5);
        check (juce::approximatelyEqual (get (clip, daw::ids::start), 1.5) && juce::approximatelyEqual (get (clip, daw::ids::offset), 0.5)
                   && juce::approximatelyEqual (get (clip, daw::ids::length), 1.5),
               "trimming the start keeps the audio in place");

        edit.trimClipStart (clip, 0.2);
        check (juce::approximatelyEqual (get (clip, daw::ids::start), 1.0) && juce::approximatelyEqual (get (clip, daw::ids::offset), 0.0),
               "can't trim before the start of the file");

        edit.trimClipEnd (clip, 2.5);
        check (juce::approximatelyEqual (get (clip, daw::ids::length), 1.5), "trimming the end");

        const auto right = edit.splitClip (clip, 2.0);
        check (right > 0 && juce::approximatelyEqual (get (clip, daw::ids::length), 1.0)
                   && juce::approximatelyEqual (get (right, daw::ids::start), 2.0) && juce::approximatelyEqual (get (right, daw::ids::offset), 1.0)
                   && juce::approximatelyEqual (get (right, daw::ids::length), 0.5),
               "split makes two clips that play the same audio as before");
        check (edit.splitClip (clip, 5.0) == -1, "splitting outside a clip does nothing");

        // The split pair plays exactly like the unsplit clip.
        engine.loadAllNow();
        engine.seek (0);
        engine.play();
        auto out = render (engine, 2.6);
        engine.stop();

        // (The clip's trimmed end cuts into the file, so its last few ms fade out: compare up to there.)
        float worst = 0.0f;
        for (int i = 0; i < (int) ((1.5 - daw::DawEngine::antiClickSeconds) * sampleRate); ++i)
            worst = std::max (worst, std::abs (out[0][(size_t) sampleRate + (size_t) i] - source->getSample (0, i)));
        check (worst == 0.0f, "the split is seamless (sample-exact across the cut)");

        edit.moveClip (right, 0.25, b);
        check (edit.getClip (right).getParent() == edit.getTrack (b) && juce::approximatelyEqual (get (right, daw::ids::start), 0.25),
               "moves a clip in time and to another track");

        const auto copy = edit.duplicateClip (clip);
        check (copy > 0 && juce::approximatelyEqual (get (copy, daw::ids::start), 2.0), "duplicate lands right after the original");

        edit.setClipFades (copy, 0.8, 0.8);
        check (juce::approximatelyEqual (get (copy, daw::ids::fadeIn), 0.8) && std::abs (get (copy, daw::ids::fadeOut) - 0.2) < 1.0e-9,
               "fades never overlap past the clip");

        edit.setClipGain (copy, -6.0f);
        edit.getUndoManager().beginNewTransaction();
        edit.getUndoManager().undo();
        check (edit.getTrack (a).getNumChildren() == 2 // INSERTS + the original clip
                   && ! edit.getClip (right).isValid() && ! edit.getClip (copy).isValid()
                   && juce::approximatelyEqual (get (clip, daw::ids::length), 2.0),
               "one undo reverts the whole editing gesture");
        edit.getUndoManager().redo();

        // Fades shape the audio: silent at the clip's start, full level after the fade.
        edit.setClipFades (clip, 0.5, 0.0);
        engine.loadAllNow();
        engine.seek (0);
        engine.play();
        out = render (engine, 1.8);
        engine.stop();
        const auto at = [&] (double seconds) { return out[0][(size_t) (seconds * sampleRate)]; };
        check (std::abs (at (1.0)) < 1.0e-6f && std::abs (at (1.25) - 0.5f * source->getSample (0, (int) (0.25 * sampleRate))) < 1.0e-4f
                   && juce::approximatelyEqual (at (1.6), source->getSample (0, (int) (0.6 * sampleRate))),
               "a fade-in ramps the clip up");

        edit.setTempo (120.0);
        check (juce::approximatelyEqual (edit.snapToGrid (1.13, 1.0), 1.0) && juce::approximatelyEqual (edit.snapToGrid (1.3, 1.0), 1.5)
                   && juce::approximatelyEqual (edit.snapToGrid (3.9, 4.0), 4.0),
               "snaps to beats and bars at the project tempo");

        // Loop: playback wraps from the loop end back to its start, sample-exact.
        edit.setClipFades (clip, 0.0, 0.0);
        edit.setLoop (true, 1.0, 1.1);
        engine.loadAllNow();
        engine.seek ((juce::int64) sampleRate);
        engine.play();
        out = render (engine, 0.35);
        engine.stop();

        const auto loopLength = (size_t) (0.1 * sampleRate);
        worst = 0.0f;
        for (size_t i = 0; i < out[0].size(); ++i)
            worst = std::max (worst, std::abs (out[0][i] - source->getSample (0, (int) (i % loopLength))));
        check (worst == 0.0f && engine.getPosition() < (juce::int64) (1.1 * sampleRate), "loops between the loop points");

        edit.setLoop (false, 1.0, 1.1);
        folder.deleteRecursively();
    }

    void testLatency()
    {
        std::cout << "\nLatency (autotune and pitch shift)\n";

        const auto nominal = fx::PsolaShifter::getNominalLatency (sampleRate);

        fx::modules::Pitch pitch;
        pitch.prepare ({ sampleRate, (juce::uint32) blockSize, 2 });
        check (pitch.getLatencySamples() == 0, "no delay while the pitch effects are off");
        pitch.harmonyOn.store (1.0f);
        check (pitch.getLatencySamples() == 0, "the harmonizer alone leaves the lead undelayed");
        pitch.harmonyOn.store (0.0f);
        pitch.tuneOn.store (1.0f);
        check (pitch.getLatencySamples() == nominal, "autotune reports " + juce::String (nominal) + " samples ("
                                                         + juce::String (1000.0 * nominal / sampleRate, 2) + " ms)");
        pitch.tuneOn.store (0.0f);
        pitch.shiftOn.store (1.0f);
        check (pitch.getLatencySamples() == nominal, "pitch shift reports the same delay");

        // The real delay of a 200 Hz voice through the shifter (at 0 semitones the output is
        // the input, delayed). A steady tone only shows it modulo one period, which is enough
        // here: the delay is shorter than a period.
        const auto input = makeVoice ((float) fx::PsolaShifter::nominalVoiceHz, 1.0);
        fx::PitchEngine::Settings settings;
        settings.shiftOn = true;
        const auto output = runPitchEngine (input, settings);
        const auto period = (int) std::lround (sampleRate / fx::PsolaShifter::nominalVoiceHz);

        auto measured = 0;
        auto bestError = std::numeric_limits<double>::max();
        for (int delay = 0; delay < period; ++delay)
        {
            double error = 0.0;
            for (size_t i = 24000; i < 40000; ++i)
                error += juce::square ((double) output[i] - (double) input[i - (size_t) delay]);
            if (error < bestError)
            {
                bestError = error;
                measured = delay;
            }
        }

        // It wanders within half a period of the reported value, depending on where the
        // first grain landed.
        check (std::abs (measured - nominal) <= period / 2, "a 200 Hz voice comes out " + juce::String (measured)
                                                              + " samples late (reported " + juce::String (nominal) + ")");

        // The plugin tells the host.
        PluginProcessor processor;
        processor.prepareToPlay (sampleRate, blockSize);
        check (processor.getLatencySamples() == 0, "the plugin reports no latency by default");

        processor.getState().getParameter ("tuneOn")->setValueNotifyingHost (1.0f);
        juce::AudioBuffer<float> buffer (2, blockSize);
        buffer.clear();
        juce::MidiBuffer midi;
        processor.processBlock (buffer, midi);
        processor.updateLatency();
        check (processor.getLatencySamples() == nominal, "with autotune on, the plugin reports its delay to the host");

        processor.getState().getParameter ("tuneOn")->setValueNotifyingHost (0.0f);
        processor.processBlock (buffer, midi);
        processor.updateLatency();
        check (processor.getLatencySamples() == 0, "and goes back to 0 when it's switched off");

        // DAW mode: a track with autotune delays the others to match.
        const auto folder = juce::File::createTempFile ("micfx-latency");
        folder.createDirectory();
        juce::AudioBuffer<float> click (1, (int) sampleRate / 2);
        click.clear();
        click.setSample (0, 0, 0.5f);
        const auto clickFile = folder.getChildFile ("click.wav");
        writeWav (clickFile, click, sampleRate);

        daw::Edit edit;
        daw::DawEngine engine (edit);
        engine.prepare (sampleRate, blockSize);
        const auto plain = (int) edit.addTrack ("Backing")[daw::ids::id];
        const auto tuned = (int) edit.addTrack ("Vocal")[daw::ids::id];
        edit.addClip (plain, clickFile, 0.1, 0.5);
        const auto tune = (int) edit.addInsert (tuned, fx::ModuleType::pitch)[daw::ids::id];
        engine.loadAllNow();
        check (engine.getLatencySamples() == nominal, "a track with autotune sets the session's delay");

        const auto clickAt = [&]
        {
            engine.seek (0);
            engine.play();
            const auto out = render (engine, 0.3);
            engine.stop();
            for (size_t i = 0; i < out[0].size(); ++i)
                if (std::abs (out[0][i]) > 0.1f)
                    return (int) i - (int) (0.1 * sampleRate);
            return -1;
        };

        check (clickAt() == nominal, "the other tracks are delayed by the same amount");

        edit.setInsertValue (tune, "tuneOn", 0.0f);
        engine.rebuildNow();
        check (engine.getLatencySamples() == 0 && clickAt() == 0, "switching autotune off removes the delay");

        folder.deleteRecursively();
    }

    // A module that only multiplies by its "gain" value, to watch the smoothing.
    struct GainModule final : public fx::Module
    {
        GainModule() : Module (fx::ModuleType::width) {}

        Value& on   = addSwitch ("on", true);
        Value& gain = addValue ("gain", 0.0f);
        Value& mode = addChoice ("mode", 0.0f);

        float seenMode = 0.0f;

    private:
        void prepareModule (const juce::dsp::ProcessSpec&) override {}
        void resetModule() override {}
        void update() noexcept override { g = get (gain); seenMode = get (mode); }
        void render (const Context& c) noexcept override { c.getOutputBlock().multiplyBy (g); }

        float g = 0.0f;
    };

    void testSmoothing()
    {
        std::cout << "\nSmooth knob changes\n";

        GainModule module;
        module.prepare ({ sampleRate, (juce::uint32) blockSize, 1 });

        const auto run = [&] (double seconds)
        {
            std::vector<float> out;
            juce::AudioBuffer<float> buffer (1, blockSize);

            for (int done = 0; done < (int) (seconds * sampleRate); done += blockSize)
            {
                for (int i = 0; i < blockSize; ++i)
                    buffer.setSample (0, i, 1.0f);
                juce::dsp::AudioBlock<float> block (buffer);
                module.process (fx::Module::Context (block));
                out.insert (out.end(), buffer.getReadPointer (0), buffer.getReadPointer (0) + blockSize);
            }

            return out;
        };

        run (0.05);
        module.gain.store (1.0f);
        module.mode.store (2.0f);
        const auto out = run (0.3);

        float biggestStep = out[0];
        bool rising = true;
        for (size_t i = 1; i < out.size(); ++i)
        {
            biggestStep = std::max (biggestStep, out[i] - out[i - 1]);
            rising = rising && out[i] >= out[i - 1];
        }

        check (rising && biggestStep < 0.05f, "a knob jump from 0 to 1 glides up in small steps (largest "
                                                  + juce::String (biggestStep, 3) + ")");
        check (out[(size_t) (0.04 * sampleRate)] > 0.9f && out.back() == 1.0f,
               "and gets there quickly (" + juce::String (out[(size_t) (0.04 * sampleRate)], 3) + " after 40 ms, exactly 1 later)");
        check (juce::approximatelyEqual (module.seenMode, 2.0f), "choices change at once");

        module.gain.store (0.25f);
        module.reset();
        check (run (0.01).front() == 0.25f, "after a reset the value starts at its target");

        // Every value that's a switch or a list on the plugin steps; knobs glide.
        PluginProcessor processor;
        fx::FxChain chain { fx::FxChain::getAllTypes() };
        bool allMatch = true;

        for (int m = 0; m < chain.size(); ++m)
            for (int v = 0; v < chain[m].getNumValues(); ++v)
            {
                auto* parameter = processor.getState().getParameter (chain[m].getValueId (v));
                const auto stepped = dynamic_cast<juce::AudioParameterBool*> (parameter) != nullptr
                                  || dynamic_cast<juce::AudioParameterChoice*> (parameter) != nullptr;

                if (parameter != nullptr && stepped == chain[m].getValue (v).isSmoothed())
                {
                    allMatch = false;
                    std::cout << "        " << chain[m].getValueId (v) << ": "
                              << (stepped ? "a switch or list, but it glides" : "a knob, but it steps") << "\n";
                }
            }

        check (allMatch, "switches and lists step, knobs glide");
    }
}

int main()
{
    juce::ScopedJuceInitialiser_GUI juce;

    testPitchDetector();
    testPitchShift();
    testAutotune();
    testPresets();
    testFxChain();
    testParametricEq();
    testRecorder();
    testMultitrack();
    testDawEngine();
    testDawRecording();
    testProjects();
    testExport();
    testClipEditing();
    testDawMode();
    testClipTools();
    testTrackTools();
    testDawBridge();
    testLiveControls();
    testAnalysis();
    testMixAssistant();
    testAutoMixEffects();
    testLatency();
    testSmoothing();

    std::cout << "\n" << (failures == 0 ? "All tests passed" : juce::String (failures) + " test(s) failed") << "\n";
    return failures == 0 ? 0 : 1;
}
