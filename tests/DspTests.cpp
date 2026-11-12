// Headless checks for the DSP. Returns a non-zero exit code if anything fails.
//
//   cmake --build --preset debug --target MicFXTests
//   ./build/debug/MicFXTests_artefacts/Debug/MicFXTests
//
// Use the release preset for meaningful CPU numbers.

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_cryptography/juce_cryptography.h>

#include <set>
#include <thread>

#include "DawBridge.h"
#include "PluginProcessor.h"
#include "analysis/Analysis.h"
#include "dsp/Biquad.h"
#include "analysis/Key.h"
#include "analysis/MixAssistant.h"
#include "Presets.h"
#include "daw/DawEngine.h"
#include "daw/ClipTools.h"
#include "daw/Exporter.h"
#include "daw/Instruments.h"
#include "daw/PluginHost.h"
#include "daw/Project.h"
#include "daw/Warp.h"
#include "daw/Quantize.h"
#include "analysis/Beats.h"
#include "dsp/FxChain.h"
#include "dsp/PitchDetector.h"
#include "dsp/PitchEngine.h"
#include "stems/StemSplitter.h"
#if MICFX_HAS_DEMUCS
 #include "stems/DemucsPipeline.h"
#endif

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
            {
                auto& module = *full.find (type);
                for (int v = 0; v < module.getNumValues(); ++v)
                    if (module.isSwitch (v))
                    {
                        module.getValue (v).store (1.0f);
                        break;
                    }
            }

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


    juce::AudioBuffer<float> makeHits (const std::vector<double>& times, double seconds, int accentEvery = 0);
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
        check (roles[guitar] == analysis::Role::guitar && roles[voice] == analysis::Role::voice && roles[roomy] == analysis::Role::backingVocal,
               "guesses the guitar and the harmony vocal from their names");

        analysis::MixOptions options; // the effects these checks are about (the style's character has its own test)
        options.saturation = options.echo = options.exciter = options.width = false;
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
                   && guitarEq->values.at ("peqLowCutFreq") >= 80.0f && guitarEq->values.at ("peqLowCutFreq") <= 150.0f,
               "guitar: low cut under its lowest note (196 Hz), leaving the low end to the bass");
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
        eqOnly.saturation = eqOnly.echo = eqOnly.exciter = eqOnly.width = false;
        const auto limited = analysis::planMix (source, roles, volumes, 0.0f, analysis::MixStyle::ballad, eqOnly);
        bool onlyEq = true;
        for (const auto& move : limited.moves)
            onlyEq = onlyEq && (! move.insert.has_value() || *move.insert == fx::ModuleType::paraEq);
        analysis::MixOptions none;
        none.eq = none.compressor = none.deEsser = none.reverb = false;
        none.saturation = none.echo = none.exciter = none.width = false;
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

    // Drums: a kick (a falling 60 Hz thump) on every beat, a noise hat between, 120 BPM.
    juce::AudioBuffer<float> makeDrumLoop (double seconds)
    {
        juce::AudioBuffer<float> buffer (2, (int) (seconds * sampleRate));
        buffer.clear();
        juce::Random random (5);
        const auto twoPi = juce::MathConstants<double>::twoPi;

        for (int i = 0; i < buffer.getNumSamples(); ++i)
        {
            const auto t = i / sampleRate;
            const auto sinceKick = std::fmod (t, 0.5), sinceHat = std::fmod (t + 0.25, 0.5);
            const auto kick = 0.6 * std::sin (twoPi * (50.0 + 60.0 * std::exp (-sinceKick * 30.0)) * sinceKick) * std::exp (-sinceKick * 12.0);
            const auto hat = 0.15 * (random.nextFloat() * 2.0 - 1.0) * std::exp (-sinceHat * 60.0);
            for (int ch = 0; ch < 2; ++ch)
                buffer.setSample (ch, i, (float) (kick + hat));
        }

        return buffer;
    }

    // A bass line: 55 and 73 Hz notes, a bar each, a little uneven.
    juce::AudioBuffer<float> makeBassLine (double seconds)
    {
        juce::AudioBuffer<float> buffer (2, (int) (seconds * sampleRate));
        const auto twoPi = juce::MathConstants<double>::twoPi;

        for (int i = 0; i < buffer.getNumSamples(); ++i)
        {
            const auto t = i / sampleRate;
            const auto bar = (int) std::floor (t / 2.0);
            const auto f = bar % 2 == 0 ? 55.0 : 73.4;
            const auto level = bar % 3 == 0 ? 0.35 : 0.2;
            const auto value = level * (std::sin (twoPi * f * t) + 0.3 * std::sin (twoPi * 2.0 * f * t)) * (0.6 + 0.4 * std::exp (-std::fmod (t, 0.5) * 6.0));
            for (int ch = 0; ch < 2; ++ch)
                buffer.setSample (ch, i, (float) value);
        }

        return buffer;
    }

    // Auto Mix with every track tagged: the names, a guess from the sound for drums and
    // bass, and what each role gets (background vocals under the lead, bass and drums
    // only a rumble cut).
    void testAutoMixRoles()
    {
        std::cout << "\nAuto Mix track roles\n";
        using analysis::Role;

        const std::pair<const char*, std::optional<Role>> names[] = {
            { "Lead vocal", Role::voice }, { "Voz", Role::voice }, { "Backing vocals", Role::backingVocal },
            { "Harmony 2", Role::backingVocal }, { "Coros", Role::backingVocal }, { "BGV", Role::backingVocal },
            { "Kick", Role::drums }, { "My song - Drums", Role::drums }, { "Overheads", Role::drums }, { "Bajo", Role::bass },
            { "Bass DI", Role::bass }, { "Lead guitar", Role::guitar }, { "Acoustic", Role::guitar }, { "Rhodes", Role::piano },
            { "Piano", Role::piano }, { "Karaoke", Role::backing }, { "Backing track", Role::backing }, { "Instrumental", Role::backing },
            { "My song - Other", Role::instrument }, { "Strings", Role::pad }, { "Synth lead", Role::synth },
            { "Track 3", std::nullopt }, { "Audio 1", std::nullopt }
        };
        juce::StringArray wrong;
        for (const auto& [name, expected] : names)
            if (analysis::guessRoleFromName (name) != expected)
                wrong.add (name);
        check (wrong.isEmpty(), "guesses roles from track names" + (wrong.isEmpty() ? juce::String() : " (wrong: " + wrong.joinIntoString (", ") + ")"));

        bool keysRoundTrip = true;
        for (auto role : analysis::allRoles)
            keysRoundTrip = keysRoundTrip && analysis::getRoleFromKey (analysis::getRoleKey (role)) == role;
        check (keysRoundTrip && analysis::getRoleFromKey ("backing") == Role::backing, "role keys round-trip (old 'backing' still reads)");

        const auto folder = juce::File::createTempFile ("micfx-roles");
        folder.createDirectory();
        const auto file = [&] (const char* name, const juce::AudioBuffer<float>& audio)
        {
            const auto f = folder.getChildFile (name);
            writeWav (f, audio, sampleRate);
            return f;
        };

        daw::Edit edit;
        const auto add = [&] (const char* name, const juce::File& f)
        {
            const auto id = (int) edit.addTrack (name)[daw::ids::id];
            edit.addClip (id, f, 0.0, 12.0);
            return id;
        };

        auto harmony = makeSungLine (12.0);
        harmony.applyGain (0.8f);
        const auto voice = add ("Track 1", file ("voice.wav", makeSungLine (12.0)));
        const auto bgv = add ("Track 2", file ("harmony.wav", harmony));
        const auto drums = add ("Track 3", file ("drums.wav", makeDrumLoop (12.0)));
        const auto bass = add ("Track 4", file ("bass.wav", makeBassLine (12.0)));
        const auto guitar = add ("Track 5", file ("guitar.wav", makePlucks (12.0)));

        const auto source = analysis::SessionAnalyser (edit, analysis::SessionAnalyser::Point::source, false).run();
        std::map<int, Role> guessed;
        for (const auto& [id, profile] : source.tracks)
            guessed[id] = analysis::guessRole (edit.getTrack (id)[daw::ids::name].toString(), profile, 12.0);
        check (guessed[drums] == Role::drums && guessed[bass] == Role::bass,
               "guesses drums and bass from the sound (" + juce::String (analysis::getRoleKey (guessed[drums])) + ", "
                   + analysis::getRoleKey (guessed[bass]) + ")");

        const std::map<int, Role> roles { { voice, Role::voice }, { bgv, Role::backingVocal }, { drums, Role::drums },
                                          { bass, Role::bass }, { guitar, Role::guitar } };
        std::map<int, float> volumes;
        for (const auto& [id, role] : roles)
            volumes[id] = 0.0f;
        const auto plan = analysis::planMix (source, roles, volumes, 0.0f, analysis::MixStyle::pop);

        for (const auto& move : plan.moves)
            for (const auto& reason : move.reasons)
                std::cout << "        " << (move.trackId == daw::Edit::masterId ? juce::String ("Master") : analysis::getRoleName (roles.at (move.trackId)))
                          << ": " << reason << "\n";
        for (const auto& note : plan.notes)
            std::cout << "        note: " << note << "\n";

        const auto find = [&] (int track, std::optional<fx::ModuleType> type) -> const analysis::MixMove*
        {
            for (const auto& move : plan.moves)
                if (move.trackId == track && move.insert == type)
                    return &move;
            return nullptr;
        };
        const auto value = [&] (int track, fx::ModuleType type, const char* id)
        {
            const auto* move = find (track, type);
            return move != nullptr && move->values.count (id) > 0 ? move->values.at (id) : -1000.0f;
        };

        check (value (bgv, fx::ModuleType::paraEq, "peqLowCutFreq") >= 100.0f
                   && value (bgv, fx::ModuleType::paraEq, "peqLowCutFreq") > value (voice, fx::ModuleType::paraEq, "peqLowCutFreq"),
               "background vocals: a higher low cut than the lead's");
        check (value (bgv, fx::ModuleType::reverb, "reverbMix") > value (voice, fx::ModuleType::reverb, "reverbMix"),
               "background vocals: more reverb than the lead");
        check (juce::exactlyEqual (value (bass, fx::ModuleType::paraEq, "peqLowCutFreq"), 30.0f)
                   && juce::exactlyEqual (value (drums, fx::ModuleType::paraEq, "peqLowCutFreq"), 30.0f),
               "bass and drums: only a rumble cut (30 Hz)");
        check (find (bass, fx::ModuleType::compressor) != nullptr, "bass: a compressor keeps the low end even");
        check (value (guitar, fx::ModuleType::paraEq, "peqLowCutFreq") >= 80.0f, "guitar: low cut from 80 Hz up");
        check (find (bass, fx::ModuleType::reverb) == nullptr && find (drums, fx::ModuleType::reverb) == nullptr,
               "instruments get no reverb");

        // Applied: the background vocals land about the style's LU under the lead.
        analysis::applyMix (edit, plan);
        const auto after = analysis::SessionAnalyser (edit, analysis::SessionAnalyser::Point::stem, false).run();
        const analysis::Profile* leadAfter = nullptr;
        const analysis::Profile* bgvAfter = nullptr;
        for (const auto& [id, profile] : after.tracks)
        {
            if (id == voice) leadAfter = &profile;
            if (id == bgv)   bgvAfter = &profile;
        }
        const auto under = leadAfter->getLoudnessOver() - bgvAfter->getLoudnessOver();
        const auto target = analysis::getTargets (analysis::MixStyle::pop).backingVocalUnderLeadLu;
        check (std::abs (under - target) < 1.5f,
               "background vocals sit " + juce::String (under, 1) + " LU under the lead (target " + juce::String (target, 1) + ")");

        // More styles: each its own name and character.
        {
            juce::StringArray styleNames;
            for (auto style : analysis::allStyles)
                styleNames.addIfNotAlreadyThere (analysis::getStyleName (style));
            check (styleNames.size() == (int) std::size (analysis::allStyles) && styleNames.size() >= 10, "ten mix styles, each named");
        }

        analysis::MixOptions electronicOptions;
        electronicOptions.tempo = 100.0;
        const auto electronic = analysis::planMix (source, roles, volumes, 0.0f, analysis::MixStyle::electronic, electronicOptions);
        const auto electronicValue = [&] (int track, fx::ModuleType type, const char* id)
        {
            for (const auto& move : electronic.moves)
                if (move.trackId == track && move.insert == type && move.values.count (id) > 0)
                    return move.values.at (id);
            return -1000.0f;
        };
        check (juce::exactlyEqual (electronicValue (voice, fx::ModuleType::echo, "delayTime"), 450.0f)
                   && electronicValue (voice, fx::ModuleType::echo, "delayMix") > 0.0f,
               "electronic: a dotted-eighth echo on the lead (450 ms at 100 BPM)");
        check (electronicValue (bgv, fx::ModuleType::echo, "delayMix") < -999.0f, "no echo on the background vocals");
        check (electronicValue (voice, fx::ModuleType::saturation, "satDrive") > 0.0f
                   && electronicValue (bass, fx::ModuleType::saturation, "satDrive") > 0.0f,
               "electronic: saturation on the voice and the bass");
        check (electronicValue (guitar, fx::ModuleType::width, "widthAmount") > 100.0f
                   && electronicValue (bass, fx::ModuleType::width, "widthAmount") < -999.0f
                   && electronicValue (drums, fx::ModuleType::width, "widthAmount") < -999.0f,
               "electronic: wider guitar, bass and drums stay centred");
        {
            const auto coaching = analysis::planMix (source, roles, volumes, 0.0f, analysis::MixStyle::coaching);
            bool plain = true;
            for (const auto& move : coaching.moves)
                plain = plain && (! move.insert.has_value() || (*move.insert != fx::ModuleType::echo && *move.insert != fx::ModuleType::saturation
                                                                && *move.insert != fx::ModuleType::exciter && *move.insert != fx::ModuleType::width));
            check (plain, "coaching: no echo, saturation, exciter or width");
        }

        // Normalize: each track to its role's level first (input gain), then the rest.
        {
            analysis::MixOptions normalizeOnly;
            normalizeOnly.eq = normalizeOnly.compressor = normalizeOnly.deEsser = normalizeOnly.reverb = false;
            normalizeOnly.saturation = normalizeOnly.echo = normalizeOnly.exciter = normalizeOnly.width = false;
            normalizeOnly.normalize = true;

            const auto normalized = analysis::planMix (source, roles, volumes, 0.0f, analysis::MixStyle::rock, normalizeOnly);
            int trims = 0;
            for (const auto& move : normalized.moves)
                trims += move.values.count ("inGain") > 0 ? 1 : 0;
            check (trims >= 3 && normalized.moves.front().values.count ("inGain") > 0, "normalize: input gain moves first ("
                       + juce::String (trims) + " tracks)");

            // The session without the Auto Mix applied above: no inserts, unity volumes.
            daw::Edit target;
            target.replaceState (edit.getState().createCopy());
            for (const auto& track : target.getTracks())
            {
                auto inserts = daw::Edit::getInserts (track);
                inserts.removeAllChildren (nullptr);
                target.setTrackVolume ((int) track[daw::ids::id], 0.0f);
            }
            analysis::applyMix (target, normalized);
            const auto levels = analysis::SessionAnalyser (target, analysis::SessionAnalyser::Point::stem, false).run();
            const auto targets = analysis::getTargets (analysis::MixStyle::rock);
            juce::String detail;
            bool near = true;
            for (const auto& [id, profile] : levels.tracks)
            {
                const auto role = roles.at (id);
                if (role != Role::drums && role != Role::bass && role != Role::guitar)
                    continue;
                const auto wanted = role == Role::drums ? targets.drumsLufs : role == Role::bass ? targets.bassLufs : targets.guitarLufs;
                const auto peakLimited = profile.peakDb > -1.6f;
                near = near && (peakLimited || std::abs (profile.integratedLufs - wanted) < 1.0f);
                detail << analysis::getRoleName (role) << " " << juce::String (profile.integratedLufs, 1) << " (" << juce::String (wanted, 0) << ") ";
            }
            check (near, "normalize: instruments land on their levels: " + detail.trim());

            // Half the amount: half the width change too.
            analysis::MixPlan wide;
            analysis::MixMove width;
            width.trackId = guitar;
            width.insert = fx::ModuleType::width;
            width.values = { { "widthOn", 1.0f }, { "widthAmount", 140.0f } };
            wide.moves.push_back (width);
            analysis::applyMix (target, wide, 0.5f);
            const auto guitarInserts = daw::Edit::getInserts (target.getTrack (guitar));
            check (juce::exactlyEqual ((float) guitarInserts.getChild (guitarInserts.getNumChildren() - 1)["widthAmount"], 120.0f),
                   "amount 50%: width halfway from as it is");
        }

        folder.deleteRecursively();
    }

    // Auto Mix listens offline: every track renders on its own engine, side by side, far
    // faster than playing the song.
    void testAnalysisSpeed()
    {
        std::cout << "\nAuto Mix analysis speed\n";
        const auto folder = juce::File::createTempFile ("micfx-speed");
        folder.createDirectory();
        const auto voiceFile = folder.getChildFile ("voice.wav"), backingFile = folder.getChildFile ("backing.wav");
        writeWav (voiceFile, makeSungLine (30.0), sampleRate);
        writeWav (backingFile, makeBacking (30.0), sampleRate);

        daw::Edit edit;
        std::vector<int> ids;
        for (int t = 0; t < 6; ++t)
        {
            const auto id = (int) edit.addTrack ("Track " + juce::String (t + 1))[daw::ids::id];
            edit.addClip (id, t % 2 == 0 ? voiceFile : backingFile, 0.0, 30.0);
            ids.push_back (id);
        }

        const auto started = juce::Time::getMillisecondCounterHiRes();
        double lastProgress = 0.0;
        bool monotonic = true;
        const auto result = analysis::SessionAnalyser (edit, analysis::SessionAnalyser::Point::source, true)
                                .run ([&] (double p) { monotonic = monotonic && p >= lastProgress - 1.0e-9; lastProgress = p; return true; });
        const auto seconds = (juce::Time::getMillisecondCounterHiRes() - started) / 1000.0;

        bool inOrder = result.tracks.size() == ids.size() && result.mix.has_value();
        for (size_t t = 0; inOrder && t < ids.size(); ++t)
            inOrder = result.tracks[t].first == ids[t] && result.tracks[t].second.getNumFrames() >= 299
                   && std::abs (result.tracks[t].second.integratedLufs - result.tracks[t % 2].second.integratedLufs) < 0.01f;
        check (inOrder, "6 tracks and the mix, in track order, the same results side by side");
        check (monotonic && std::abs (lastProgress - 1.0) < 1.0e-6, "progress only goes up, to 100%");
        // A cache: listening again renders only what changed.
        {
            analysis::ProfileCache cache;
            analysis::SessionAnalyser first (edit, analysis::SessionAnalyser::Point::source, false);
            const auto fresh = first.run ({}, &cache);
            analysis::SessionAnalyser again (edit, analysis::SessionAnalyser::Point::source, false);
            const auto cached = again.run ({}, &cache);
            bool same = cached.tracks.size() == fresh.tracks.size();
            for (size_t t = 0; same && t < fresh.tracks.size(); ++t)
                same = cached.tracks[t].first == fresh.tracks[t].first
                    && juce::exactlyEqual (cached.tracks[t].second.integratedLufs, fresh.tracks[t].second.integratedLufs)
                    && cached.tracks[t].second.rmsDb == fresh.tracks[t].second.rmsDb;
            check (first.getLastRenderCount() == 6 && again.getLastRenderCount() == 0 && same,
                   "listening again renders nothing new, with the same results (" + juce::String (again.getLastRenderCount()) + " renders)");

            edit.setTrackVolume (ids[1], -6.0f);
            const auto first0 = edit.getTrack (ids[0]);
            edit.addInsert (ids[2], fx::ModuleType::reverb);
            analysis::SessionAnalyser faders (edit, analysis::SessionAnalyser::Point::source, false);
            faders.run ({}, &cache);
            check (faders.getLastRenderCount() == 0, "a fader or an effect doesn't change what's measured at the source");

            edit.setClipGain ((int) first0.getChildWithName (daw::ids::CLIP)[daw::ids::id], -3.0f);
            analysis::SessionAnalyser moved (edit, analysis::SessionAnalyser::Point::source, false);
            moved.run ({}, &cache);
            check (moved.getLastRenderCount() == 1, "changing one clip renders that track again (" + juce::String (moved.getLastRenderCount()) + ")");
        }

        std::cout << "        6 tracks x 30 s + mix in " << juce::String (seconds, 2) << " s ("
                  << juce::String (7.0 * 30.0 / juce::jmax (0.001, seconds), 0) << "x real time per track, "
                  << (int) std::thread::hardware_concurrency() << " cores, this build)\n";
        folder.deleteRecursively();
    }

    // Volume automation: the Edit's ride, the engine playing it, and Auto Mix writing
    // a vocal ride and a dip on the backing while the voice sings.
    void testAutoMixModes()
    {
        std::cout << "\nAuto Mix modes (fit into backing, clean & polish)\n";

        daw::Edit edit;
        const auto folder = juce::File::createTempFile ("micfx-modes");
        folder.createDirectory();

        // A voice recorded with 50 Hz mains hum (and its 2nd and 3rd harmonics) and some
        // hiss, heard between the phrases; a backing that plays all through.
        auto sung = makeSungLine (15.0, 3.0, 1.5);
        {
            juce::Random random (7);
            const auto twoPi = juce::MathConstants<double>::twoPi;
            for (int i = 0; i < sung.getNumSamples(); ++i)
            {
                const auto t = i / sampleRate;
                const auto noise = 0.002 * std::sin (twoPi * 50.0 * t) + 0.0015 * std::sin (twoPi * 100.0 * t)
                                 + 0.001 * std::sin (twoPi * 150.0 * t) + 0.002 * (random.nextFloat() * 2.0 - 1.0);
                for (int ch = 0; ch < 2; ++ch)
                    sung.addSample (ch, i, (float) noise);
            }
        }
        const auto voiceFile = folder.getChildFile ("voice.wav"), backingFile = folder.getChildFile ("backing.wav");
        writeWav (voiceFile, sung, sampleRate);
        writeWav (backingFile, makeBacking (15.0), sampleRate);
        const auto voice = (int) edit.addTrack ("Vocal")[daw::ids::id];
        const auto backing = (int) edit.addTrack ("Backing")[daw::ids::id];
        edit.addClip (voice, voiceFile, 0.0, 15.0);
        edit.addClip (backing, backingFile, 0.0, 15.0);

        const auto source = analysis::SessionAnalyser (edit, analysis::SessionAnalyser::Point::source, false).run();
        const auto& voiceProfile = source.tracks[0].second;
        const auto& backingProfile = source.tracks[1].second;
        check (juce::exactlyEqual (voiceProfile.humHz, 50.0f) && voiceProfile.humHarmonicsDb[0] > 15.0f,
               "hum found on the voice: " + juce::String (voiceProfile.humHz, 0) + " Hz, lines "
                   + juce::String (voiceProfile.humHarmonicsDb[0], 0) + " / " + juce::String (voiceProfile.humHarmonicsDb[1], 0) + " / "
                   + juce::String (voiceProfile.humHarmonicsDb[2], 0) + " dB over the hiss");
        check (backingProfile.humHz <= 0.0f, "no hum on the backing (its 110 Hz note is near 100 Hz, but not on it)");

        const std::map<int, analysis::Role> roles { { voice, analysis::Role::voice }, { backing, analysis::Role::backing } };
        const std::map<int, float> volumes { { voice, 0.0f }, { backing, 0.0f } };
        const auto print = [] (const analysis::MixPlan& plan)
        {
            for (const auto& move : plan.moves)
                for (const auto& reason : move.reasons)
                    std::cout << "        " << move.trackId << (move.insert ? juce::String (" ") + fx::getModuleKey (*move.insert) : juce::String())
                              << ": " << reason << "\n";
            for (const auto& note : plan.notes)
                std::cout << "        note: " << note << "\n";
        };

        // Clean & polish the voice (the lead, when no track is picked).
        analysis::MixOptions clean;
        clean.mode = analysis::MixMode::clean;
        const auto cleanPlan = analysis::planMix (source, roles, volumes, 0.0f, analysis::MixStyle::pop, clean);
        print (cleanPlan);

        bool onlyVoice = true, levels = false, space = false;
        const analysis::MixMove* hum = nullptr;
        const analysis::MixMove* gate = nullptr;
        for (const auto& move : cleanPlan.moves)
        {
            onlyVoice = onlyVoice && move.trackId == voice;
            levels = levels || ! move.insert.has_value();
            space = space || move.insert == fx::ModuleType::reverb || move.insert == fx::ModuleType::echo;
            if (move.insert == fx::ModuleType::paraEq && move.instance == 1) hum = &move;
            if (move.insert == fx::ModuleType::gate) gate = &move;
        }
        check (onlyVoice && ! levels && ! space, "clean: only the voice, no volume, ride or space");
        check (hum != nullptr && hum->values.count ("peqBell1Freq") > 0 && juce::exactlyEqual (hum->values.at ("peqBell1Freq"), 50.0f)
                   && hum->values.at ("peqBell1Gain") <= -6.0f && juce::exactlyEqual (hum->values.at ("peqBell2Freq"), 100.0f)
                   && juce::exactlyEqual (hum->values.at ("peqBell3Freq"), 150.0f),
               "clean: narrow cuts at 50, 100 and 150 Hz in a second EQ");
        check (gate != nullptr && gate->values.at ("gateThreshold") > -60.0f && gate->values.at ("gateThreshold") < -35.0f,
               "clean: a gate over the noise between phrases ("
                   + (gate != nullptr ? juce::String (gate->values.at ("gateThreshold"), 0) : juce::String ("none")) + " dB)");

        // Applied: the gate first, the hum EQ right after the voice's own EQ.
        analysis::applyMix (edit, cleanPlan);
        {
            juce::StringArray chain;
            for (const auto& insert : daw::Edit::getInserts (edit.getTrack (voice)))
                chain.add (insert[daw::ids::type].toString());
            std::cout << "        chain: " << chain.joinIntoString (", ") << "\n";
            check (chain.size() >= 3 && chain[0] == "gate" && chain[1] == "paraEq" && chain[2] == "paraEq", "clean: gate, the EQ, then the hum EQ");

            const auto stem = analysis::SessionAnalyser (edit, analysis::SessionAnalyser::Point::stem, false).run();
            const auto breath = voiceProfile.rmsDb[22], after = stem.tracks[0].second.rmsDb[22]; // 2.2 s: between lines
            check (after < breath - 15.0f, "clean: the breaths go quiet (" + juce::String (breath, 0) + " dB, then " + juce::String (after, 0) + ")");
        }
        edit.getUndoManager().undo();

        // Without the gate, the hum EQ alone takes the hum lines down.
        {
            auto humOnly = cleanPlan;
            humOnly.moves.erase (std::remove_if (humOnly.moves.begin(), humOnly.moves.end(),
                                                 [] (const analysis::MixMove& m) { return m.insert != fx::ModuleType::paraEq || m.instance != 1; }),
                                 humOnly.moves.end());
            analysis::applyMix (edit, humOnly);
            const auto stem = analysis::SessionAnalyser (edit, analysis::SessionAnalyser::Point::stem, false).run();
            const auto& p = stem.tracks[0].second;
            check (p.getHumProminenceDb() < voiceProfile.getHumProminenceDb() - 6.0f,
                   "clean: the hum lines come down (" + juce::String (voiceProfile.getHumProminenceDb(), 0) + " dB over the hiss, then "
                       + juce::String (p.getHumProminenceDb(), 0) + ")");
            edit.getUndoManager().undo();
        }

        // Fit the voice into the backing: the voice gets its treatment and level, the
        // backing only makes room, the master stays.
        analysis::MixOptions fit;
        fit.mode = analysis::MixMode::fit;
        fit.focusTrackId = voice;
        const auto fitPlan = analysis::planMix (source, roles, volumes, 0.0f, analysis::MixStyle::pop, fit);
        print (fitPlan);
        bool master = false, voiceVolume = false, backingOnlyRoom = true;
        for (const auto& move : fitPlan.moves)
        {
            master = master || move.trackId == daw::Edit::masterId;
            voiceVolume = voiceVolume || (move.trackId == voice && ! move.insert.has_value() && move.values.count ("volume") > 0);
            if (move.trackId == backing)
                backingOnlyRoom = backingOnlyRoom && (! move.ride.empty() || move.insert == fx::ModuleType::paraEq);
        }
        check (! master && voiceVolume && backingOnlyRoom, "fit: the voice's level, only a dip and a ride on the backing, the master stays");

        // An instrument in focus: it gets the level (under the backing), the voice is left alone.
        {
            const auto guitarFile = folder.getChildFile ("guitar.wav");
            writeWav (guitarFile, makeTones ({ { 15.0, 196.0, 0.1f, 0.1f } }), sampleRate);
            const auto guitar = (int) edit.addTrack ("Guitar")[daw::ids::id];
            edit.addClip (guitar, guitarFile, 0.0, 15.0);
            const auto withGuitar = analysis::SessionAnalyser (edit, analysis::SessionAnalyser::Point::source, false).run();
            auto guitarRoles = roles;
            guitarRoles[guitar] = analysis::Role::guitar;
            auto guitarVolumes = volumes;
            guitarVolumes[guitar] = 0.0f;
            fit.focusTrackId = guitar;
            const auto guitarPlan = analysis::planMix (withGuitar, guitarRoles, guitarVolumes, 0.0f, analysis::MixStyle::pop, fit);
            print (guitarPlan);
            bool guitarVolume = false, voiceLevel = false, masterMove = false;
            for (const auto& move : guitarPlan.moves)
            {
                guitarVolume = guitarVolume || (move.trackId == guitar && ! move.insert.has_value() && move.values.count ("volume") > 0);
                voiceLevel = voiceLevel || (move.trackId == voice && ! move.insert.has_value() && move.ride.empty());
                masterMove = masterMove || move.trackId == daw::Edit::masterId;
            }
            check (guitarVolume && ! voiceLevel && ! masterMove, "fit, guitar in focus: its level; the voice's and the master's stay");
        }

        check (analysis::getModeFromKey (analysis::getModeKey (analysis::MixMode::clean)) == analysis::MixMode::clean
                   && analysis::getModeFromKey ("nonsense") == analysis::MixMode::mix,
               "mode keys round-trip");
    }

    void testAutoMixSynths()
    {
        std::cout << "\nAuto Mix on MIDI tracks (roles from the instrument, colour)\n";

        using analysis::Role;
        check (analysis::guessRoleFromInstrument ("builtin:pad", "Warm Pad") == Role::pad
                   && analysis::guessRoleFromInstrument ("builtin:strings", "Strings") == Role::pad
                   && analysis::guessRoleFromInstrument ("builtin:lead", "Lead") == Role::synth
                   && analysis::guessRoleFromInstrument ("builtin:bass", "Synth Bass") == Role::bass
                   && analysis::guessRoleFromInstrument ("builtin:keys", "Keys") == Role::piano
                   && analysis::guessRoleFromInstrument ("builtin:organ", "Organ") == Role::piano,
               "built-in synths: pad, strings, lead, bass, keys, organ");
        check (analysis::guessRoleFromInstrument ("VST3-Serum", "Serum") == Role::synth
                   && analysis::guessRoleFromInstrument ("VST3-x", "Superior Drummer 3") == Role::drums
                   && analysis::guessRoleFromInstrument ("VST3-y", "Trilian Bass") == Role::bass
                   && ! analysis::guessRoleFromInstrument ("VST3-z", "Thing").has_value(),
               "plugins by their names (Serum, Superior Drummer, Trilian Bass; nothing from \"Thing\")");
        check (analysis::guessRoleFromName ("Strings") == Role::pad && analysis::guessRoleFromName ("Synth arp") == Role::synth
                   && analysis::guessRoleFromName ("Synth bass") == Role::bass,
               "track names: Strings is a pad, Synth arp a synth, Synth bass a bass");
        check (analysis::getRoleFromKey (analysis::getRoleKey (Role::pad)) == Role::pad
                   && analysis::getRoleFromKey (analysis::getRoleKey (Role::synth)) == Role::synth,
               "pad and synth keys round-trip");

        // A voice, a pad, a synth lead and a synth bass; the three synths are MIDI tracks.
        daw::Edit edit;
        const auto folder = juce::File::createTempFile ("micfx-synths");
        folder.createDirectory();
        const auto add = [&] (const juce::String& name, const juce::AudioBuffer<float>& audio)
        {
            const auto file = folder.getChildFile (name + ".wav");
            writeWav (file, audio, sampleRate);
            const auto id = (int) edit.addTrack (name)[daw::ids::id];
            edit.addClip (id, file, 0.0, 12.0);
            return id;
        };
        const auto voice = add ("Vocal", makeSungLine (12.0, 3.0, 1.5));
        const auto pad = add ("Pad", makeTones ({ { 12.0, 330.0, 0.1f, 0.1f } }));
        const auto lead = add ("Lead", makeTones ({ { 12.0, 880.0, 0.05f, 0.05f } }));
        const auto bass = add ("Bass", makeTones ({ { 12.0, 55.0, 0.2f, 0.2f } }));

        const auto source = analysis::SessionAnalyser (edit, analysis::SessionAnalyser::Point::source, false).run();
        const std::map<int, Role> roles { { voice, Role::voice }, { pad, Role::pad }, { lead, Role::synth }, { bass, Role::bass } };
        const std::map<int, float> volumes { { voice, 0.0f }, { pad, 0.0f }, { lead, 0.0f }, { bass, 0.0f } };

        analysis::MixOptions options;
        options.midiTracks = { pad, lead, bass };
        const auto plan = analysis::planMix (source, roles, volumes, 0.0f, analysis::MixStyle::ballad, options);
        const auto has = [&plan] (int id, fx::ModuleType type, const char* value = nullptr, float expected = 0.0f)
        {
            for (const auto& move : plan.moves)
                if (move.trackId == id && move.insert == type
                    && (value == nullptr || (move.values.count (value) > 0 && juce::approximatelyEqual (move.values.at (value), expected))))
                    return true;
            return false;
        };
        for (const auto& move : plan.moves)
            if (move.trackId != voice)
                for (const auto& reason : move.reasons)
                    std::cout << "        " << move.trackId << ": " << reason << "\n";

        check (has (pad, fx::ModuleType::chorus) && has (pad, fx::ModuleType::width, "widthAmount", 140.0f), "pad: chorus and 140% width");
        check (has (lead, fx::ModuleType::echo) && has (lead, fx::ModuleType::saturation), "synth lead: echo and drive");
        check (has (bass, fx::ModuleType::saturation) && has (bass, fx::ModuleType::width, "widthAmount", 0.0f), "synth bass: drive and mono");

        // Audio tracks with the same roles, or colour off: none of it.
        options.colour = false;
        const auto plain = analysis::planMix (source, roles, volumes, 0.0f, analysis::MixStyle::ballad, options);
        bool none = true;
        for (const auto& move : plain.moves)
            none = none && move.insert != fx::ModuleType::chorus && ! (move.trackId == lead && move.insert == fx::ModuleType::echo);
        check (none, "no colour when it's off");
    }

    void testAutoMixRides()
    {
        std::cout << "\nVolume rides (automation)\n";

        daw::Edit edit;
        const auto folder = juce::File::createTempFile ("micfx-rides");
        folder.createDirectory();

        // The Edit: points in time order, straight lines between them, holds at the ends.
        const auto tone = (int) edit.addTrack ("Tone")[daw::ids::id];
        edit.getUndoManager().beginNewTransaction();
        edit.setVolumeRide (tone, { { 2.0, -12.0f }, { 0.0, 0.0f }, { 1.0, 0.0f } });
        const auto ride = daw::Edit::getVolumeRide (edit.getTrack (tone));
        check (ride.size() == 3 && juce::exactlyEqual (ride[1].time, 1.0) && juce::exactlyEqual (daw::Edit::getRideDbAt (ride, 1.5), -6.0f)
                   && juce::exactlyEqual (daw::Edit::getRideDbAt (ride, 9.0), -12.0f),
               "ride: sorted, straight lines, holds after the last point");
        edit.getUndoManager().undo();
        check (edit.getTrack (tone).isValid() && ! edit.getTrack (tone).hasProperty (daw::ids::ride), "ride: one undo step");
        edit.setVolumeRide (tone, { { 0.0, 0.0f }, { 1.0, 0.0f }, { 2.0, -12.0f } });

        // The engine plays it (offline, like an export).
        const auto toneFile = folder.getChildFile ("tone.wav");
        writeWav (toneFile, makeTones ({ { 4.0, 440.0, 0.3f, 0.3f } }), sampleRate);
        edit.addClip (tone, toneFile, 0.0, 4.0);
        {
            const auto played = analysis::SessionAnalyser (edit, analysis::SessionAnalyser::Point::stem, false).run();
            const auto& rms = played.tracks.front().second.rmsDb;
            const auto drop = rms[5] - rms[30];
            check (std::abs (drop - 12.0f) < 0.5f, "the engine plays the ride (" + juce::String (drop, 1) + " dB down after 2 s)");

            const auto raw = analysis::SessionAnalyser (edit, analysis::SessionAnalyser::Point::source, false).run();
            const auto& rawRms = raw.tracks.front().second.rmsDb;
            check (std::abs (rawRms[5] - rawRms[30]) < 0.5f, "Auto Mix measures the tracks without their rides");
        }
        edit.removeTrack (tone);

        // Auto Mix: a voice singing 1.5 s lines with 1.5 s breaths, loud and soft lines in
        // turn, over a backing that plays all through.
        const auto voiceFile = folder.getChildFile ("voice.wav"), backingFile = folder.getChildFile ("backing.wav");
        writeWav (voiceFile, makeSungLine (15.0, 3.0, 1.5), sampleRate);
        writeWav (backingFile, makeBacking (15.0), sampleRate);
        const auto voice = (int) edit.addTrack ("Vocal")[daw::ids::id];
        const auto backing = (int) edit.addTrack ("Backing")[daw::ids::id];
        edit.addClip (voice, voiceFile, 0.0, 15.0);
        edit.addClip (backing, backingFile, 0.0, 15.0);

        const auto source = analysis::SessionAnalyser (edit, analysis::SessionAnalyser::Point::source, false).run();
        const std::map<int, analysis::Role> roles { { voice, analysis::Role::voice }, { backing, analysis::Role::backing } };
        const std::map<int, float> volumes { { voice, 0.0f }, { backing, 0.0f } };
        const auto plan = analysis::planMix (source, roles, volumes, 0.0f, analysis::MixStyle::pop);

        const analysis::MixMove* voiceRide = nullptr;
        const analysis::MixMove* duck = nullptr;
        for (const auto& move : plan.moves)
        {
            if (! move.ride.empty() && move.trackId == voice) voiceRide = &move;
            if (! move.ride.empty() && move.trackId == backing) duck = &move;
            if (! move.ride.empty())
                for (const auto& reason : move.reasons)
                    std::cout << "        " << (move.trackId == voice ? "voice: " : "backing: ") << reason << "\n";
        }
        check (voiceRide != nullptr && voiceRide->ride.size() >= 4, "a ride on the voice");
        check (duck != nullptr, "a dip on the backing while the voice sings");

        analysis::MixOptions noRides;
        noRides.rides = false;
        bool none = true;
        for (const auto& move : analysis::planMix (source, roles, volumes, 0.0f, analysis::MixStyle::pop, noRides).moves)
            none = none && move.ride.empty();
        check (none, "no rides when they're off");

        // Applied: the soft lines come up towards the loud ones, and the backing is lower
        // while the voice sings than in the breaths.
        const auto lineSpread = [] (const analysis::Profile& p)
        {
            // Mean level of each 1.5 s line (0.3 .. 1.2 s into each 3 s period).
            std::vector<float> lines;
            for (int line = 0; line < 5; ++line)
            {
                double sum = 0.0;
                for (int f = line * 30 + 3; f < line * 30 + 12; ++f) sum += p.rmsDb[(size_t) f];
                lines.push_back ((float) (sum / 9.0));
            }
            return *std::max_element (lines.begin(), lines.end()) - *std::min_element (lines.begin(), lines.end());
        };

        analysis::MixOptions ridesOnly;
        ridesOnly.eq = ridesOnly.compressor = ridesOnly.deEsser = ridesOnly.reverb = false;
        ridesOnly.saturation = ridesOnly.echo = ridesOnly.exciter = ridesOnly.width = false;
        const auto ridePlan = analysis::planMix (source, roles, volumes, 0.0f, analysis::MixStyle::pop, ridesOnly);
        const auto before = analysis::SessionAnalyser (edit, analysis::SessionAnalyser::Point::stem, false).run();
        analysis::applyMix (edit, ridePlan);
        const auto after = analysis::SessionAnalyser (edit, analysis::SessionAnalyser::Point::stem, false).run();
        const auto spreadBefore = lineSpread (before.tracks[0].second), spreadAfter = lineSpread (after.tracks[0].second);
        check (spreadAfter < spreadBefore - 3.0f, "the voice's lines even out: " + juce::String (spreadBefore, 1) + " dB apart, then "
                                                     + juce::String (spreadAfter, 1));

        const auto& backingRms = after.tracks[1].second.rmsDb;
        const auto singingLevel = backingRms[36], breathLevel = backingRms[52]; // 3.6 s (singing), 5.2 s (breath)
        const auto dip = breathLevel - singingLevel;
        const auto target = analysis::getTargets (analysis::MixStyle::pop).duckDb;
        check (std::abs (dip - target) < 0.4f, "the backing dips " + juce::String (dip, 1) + " dB while the voice sings (target "
                                                  + juce::String (target, 1) + ")");

        // Half the amount: half the ride.
        edit.getUndoManager().undo();
        analysis::applyMix (edit, ridePlan, 0.5f);
        const auto half = daw::Edit::getVolumeRide (edit.getTrack (backing));
        const auto lowest = std::min_element (half.begin(), half.end(), [] (auto& a, auto& b) { return a.db < b.db; });
        check (lowest != half.end() && std::abs (lowest->db + target / 2.0f) < 0.06f, "amount 50% halves the rides");

        folder.deleteRecursively();
    }

    // Chords (MIDI notes) played one after another, each note a soft harmonic tone,
    // `cents` away from A = 440 Hz tuning. Optional noise bursts on every beat stand in
    // for drums.
    juce::AudioBuffer<float> makeChords (const std::vector<std::vector<int>>& chords, double rate,
                                         double secondsEach = 1.5, double cents = 0.0, bool drums = false)
    {
        const auto chordSamples = (int) (secondsEach * rate);
        juce::AudioBuffer<float> buffer (2, chordSamples * (int) chords.size());
        buffer.clear();
        juce::Random random (7);
        const auto twoPi = juce::MathConstants<double>::twoPi;

        for (size_t c = 0; c < chords.size(); ++c)
        {
            for (auto note : chords[c])
            {
                const auto frequency = 440.0 * std::pow (2.0, (note - 69 + cents / 100.0) / 12.0);
                for (int h = 1; h <= 6 && frequency * h < rate / 2; ++h)
                {
                    const auto step = twoPi * frequency * h / rate;
                    double phase = random.nextDouble() * twoPi;
                    for (int i = 0; i < chordSamples; ++i, phase += step)
                    {
                        const auto envelope = juce::jmin (1.0, i / (0.01 * rate)) * std::exp (-1.2 * i / rate);
                        const auto value = (float) (0.06 / h * envelope * std::sin (phase));
                        buffer.addSample (0, (int) c * chordSamples + i, value);
                        buffer.addSample (1, (int) c * chordSamples + i, value);
                    }
                }
            }
        }

        if (drums)
        {
            const auto beat = (int) (0.375 * rate);
            for (int start = 0; start < buffer.getNumSamples(); start += beat)
                for (int i = 0; i < (int) (0.08 * rate) && start + i < buffer.getNumSamples(); ++i)
                {
                    const auto value = 0.3f * (random.nextFloat() * 2.0f - 1.0f) * std::exp (-(float) i / (float) (0.02 * rate));
                    buffer.addSample (0, start + i, value);
                    buffer.addSample (1, start + i, value);
                }
        }

        return buffer;
    }

    // A chord as MIDI notes: a bass note an octave below plus the triad (or seventh).
    std::vector<int> chord (int root, std::initializer_list<int> intervals)
    {
        std::vector<int> notes { root - 12 };
        for (auto interval : intervals)
            notes.push_back (root + interval);
        return notes;
    }

    void testKeyDetection()
    {
        std::cout << "\nKey detection\n";
        constexpr double rate = 24000.0;
        const std::initializer_list<int> maj { 0, 4, 7 }, min { 0, 3, 7 }, dom7 { 0, 4, 7, 10 };

        // Pop I-V-vi-IV in all 12 major keys, and i-iv-V7-i in all 12 minor keys.
        int majorRight = 0, minorRight = 0;
        juce::String misses;
        for (int tonic = 0; tonic < 12; ++tonic)
        {
            const auto root = 57 + tonic; // A3 .. G#4
            const auto majorKey = analysis::detectKey (makeChords ({ chord (root, maj), chord (root + 7 - 12, maj),
                                                                     chord (root + 9 - 12, min), chord (root + 5 - 12, maj),
                                                                     chord (root, maj) }, rate), rate);
            const auto minorKey = analysis::detectKey (makeChords ({ chord (root, min), chord (root + 5 - 12, min),
                                                                     chord (root + 7 - 12, dom7), chord (root, min) }, rate), rate);
            const auto expected = (root % 12);

            if (majorKey.key == expected && ! majorKey.minor) ++majorRight;
            else misses << " " << analysis::KeyEstimate::getName (expected, false) << " read as " << majorKey.getName() << ";";
            if (minorKey.key == expected && minorKey.minor) ++minorRight;
            else misses << " " << analysis::KeyEstimate::getName (expected, true) << " read as " << minorKey.getName() << ";";
        }
        check (majorRight == 12, "I-V-vi-IV: " + juce::String (majorRight) + " of 12 major keys right" + misses);
        check (minorRight == 12, "i-iv-V7-i: " + juce::String (minorRight) + " of 12 minor keys right");

        // A clear key is confident, and its runner-up is a neighbour.
        {
            const auto key = analysis::detectKey (makeChords ({ chord (60, maj), chord (65 - 12, maj), chord (67 - 12, dom7), chord (60, maj) }, rate), rate);
            check (key.getName() == "C major" && key.confidence > 0.3f && std::abs (key.tuningCents) < 5.0f,
                   "C-F-G7-C: " + key.getName() + ", confidence " + juce::String (key.confidence, 2) + ", tuning "
                       + juce::String (key.tuningCents, 1) + " cents, or " + analysis::KeyEstimate::getName (key.runnerUpKey, key.runnerUpMinor));
            check (key.getAutotuneScale() == fx::PitchEngine::getScaleNames().indexOf ("Major"), "major maps to the autotune's Major scale");
        }

        // Off A = 440: a 432 Hz recording (-32 cents) and one 40 cents sharp keep their key.
        for (auto cents : { -31.8, 40.0 })
        {
            const auto key = analysis::detectKey (makeChords ({ chord (64, min), chord (69 - 12, min), chord (71 - 12, dom7), chord (64, min) },
                                                              rate, 1.5, cents), rate);
            check (key.getName() == "E minor" && std::abs (key.tuningCents - (float) cents) < 12.0f,
                   "E minor at " + juce::String (cents, 1) + " cents: " + key.getName() + ", tuning read as "
                       + juce::String (key.tuningCents, 1) + " cents");
        }

        // Drums on every beat don't throw it.
        {
            const auto key = analysis::detectKey (makeChords ({ chord (62, maj), chord (69 - 12, maj), chord (71 - 12, min), chord (67 - 12, maj) },
                                                              rate, 1.5, 0.0, true), rate);
            check (key.getName() == "D major", "D-A-Bm-G with drums: " + key.getName());
        }

        // No harmony, no key: silence and white noise.
        {
            juce::AudioBuffer<float> silence (2, (int) (3 * rate));
            silence.clear();
            juce::AudioBuffer<float> noise (1, (int) (3 * rate));
            juce::Random random (3);
            for (int i = 0; i < noise.getNumSamples(); ++i)
                noise.setSample (0, i, 0.2f * (random.nextFloat() * 2.0f - 1.0f));
            const auto quiet = analysis::detectKey (silence, rate), hiss = analysis::detectKey (noise, rate);
            check (! quiet.isValid() && (! hiss.isValid() || hiss.confidence < 0.3f),
                   "silence has no key, noise has none or an unsure one (" + hiss.getName() + " " + juce::String (hiss.confidence, 2) + ")");
        }

        // The Auto Mix analyser keeps the chroma too, so a session's profiles give the key.
        {
            const auto audio = makeChords ({ chord (67, maj), chord (62 - 12, dom7), chord (64 - 12, min), chord (60 - 12, maj), chord (67, maj) }, sampleRate);
            const auto profile = analysis::analyse (audio, sampleRate);
            const auto key = profile.getKey();
            check (key.getName() == "G major" && profile.chroma.size() == profile.rmsDb.size(),
                   "Profile::getKey on G-D7-Em-C-G: " + key.getName());
        }
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
        existing.saturation = existing.exciter = existing.width = false;
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
    //==========================================================================
    // Fundamental frequency (80..1000 Hz) over a stretch of a channel, from the first
    // strong autocorrelation peak, refined by parabolic interpolation.
    double estimateFrequency (const std::vector<float>& channel, size_t from, size_t to)
    {
        to = std::min (to, channel.size());
        const auto n = (int) (to - from);
        const auto minLag = (int) (sampleRate / 1000.0), maxLag = (int) (sampleRate / 80.0);
        if (n <= maxLag * 2)
            return 0.0;

        std::vector<double> r ((size_t) maxLag + 2, 0.0);
        for (int lag = 0; lag <= maxLag + 1; ++lag)
            for (int i = 0; i + lag < n; ++i)
                r[(size_t) lag] += (double) channel[from + (size_t) i] * channel[from + (size_t) (i + lag)];

        if (r[0] <= 0.0)
            return 0.0;

        double best = 0.0;
        int bestLag = 0;
        for (int lag = minLag; lag <= maxLag; ++lag)
        {
            if (r[(size_t) lag] > r[(size_t) lag - 1] && r[(size_t) lag] >= r[(size_t) lag + 1] && r[(size_t) lag] > 0.8 * r[0] * (n - lag) / n)
            {
                best = r[(size_t) lag];
                bestLag = lag;
                break;
            }
        }

        if (bestLag == 0 || best <= 0.0)
            return 0.0;

        const auto a = r[(size_t) bestLag - 1], b = r[(size_t) bestLag], c = r[(size_t) bestLag + 1];
        const auto shift = 0.5 * (a - c) / (a - 2.0 * b + c);
        return sampleRate / ((double) bestLag + shift);
    }

    // Plays the engine with MIDI input given as (sample, message) pairs; returns {left, right}.
    std::array<std::vector<float>, 2> renderWithMidi (daw::DawEngine& engine, double seconds,
                                                      const std::vector<std::pair<int, juce::MidiMessage>>& input,
                                                      juce::int64 firstSample = 0)
    {
        std::array<std::vector<float>, 2> out;
        juce::AudioBuffer<float> buffer (2, blockSize);
        juce::MidiBuffer midi;

        for (int done = 0; done < (int) (seconds * sampleRate); done += blockSize)
        {
            midi.clear();
            for (const auto& [at, message] : input)
                if (at - firstSample >= done && at - firstSample < done + blockSize)
                    midi.addEvent (message, (int) (at - firstSample - done));

            engine.process (buffer, 2, blockSize, nullptr, &midi);
            for (int ch = 0; ch < 2; ++ch)
                out[(size_t) ch].insert (out[(size_t) ch].end(), buffer.getReadPointer (ch), buffer.getReadPointer (ch) + blockSize);
        }

        return out;
    }

    void testMidiTracks()
    {
        std::cout << "\nMIDI tracks (notes, built-in instruments, recording, projects, export)\n";

        const auto folder = juce::File::getSpecialLocation (juce::File::tempDirectory).getNonexistentChildFile ("micfx-midi", {});
        folder.createDirectory();

        const auto builtins = daw::instruments::getBuiltins();
        check (builtins.size() >= 5 && builtins[0].id == "builtin:keys" && daw::instruments::findBuiltin ("builtin:pad").has_value(),
               "built-in instruments are listed (" + juce::String (builtins.size()) + ")");

        daw::Edit edit;
        daw::DawEngine engine (edit);
        engine.prepare (sampleRate, blockSize);

        const auto keys = (int) edit.addMidiTrack ("Keys", daw::instruments::getDefault())[daw::ids::id];
        check (daw::Edit::isMidiTrack (edit.getTrack (keys)) && edit.getInstrument (keys)[daw::ids::plugin].toString() == "builtin:keys",
               "a MIDI track has an instrument");

        const auto clip = (int) edit.addMidiClip (keys, 0.5, 1.0)[daw::ids::id];
        const auto note = (int) edit.addNote (clip, 69, 0.25, 0.5, 100)[daw::ids::id];
        check (clip > 0 && note > 0 && edit.addMidiClip (edit.addTrack ("Audio")[daw::ids::id], 0.0, 1.0).isValid() == false,
               "MIDI clips go on MIDI tracks only");
        engine.rebuildNow();

        check (engine.getInstrument (keys) != nullptr && engine.getInstrumentError (keys).isEmpty(), "the engine makes the instrument");
        check (engine.getLength() == (juce::int64) (1.5 * sampleRate), "a MIDI clip counts towards the song's length");

        engine.play();
        auto out = render (engine, 2.0);
        engine.stop();

        const auto at = [] (double seconds) { return (size_t) (seconds * sampleRate); };
        check (rms (out[0], 0, at (0.75)) == 0.0f, "silent before the note");
        check (rms (out[0], at (0.8), at (1.2)) > 0.01f && rms (out[1], at (0.8), at (1.2)) > 0.01f,
               "the note plays at the clip's start + its time (rms " + juce::String (rms (out[0], at (0.8), at (1.2)), 4) + ")");
        const auto f = estimateFrequency (out[0], at (0.8), at (1.2));
        check (std::abs (f - 440.0) < 3.0, "the built-in synth plays the note's pitch (" + juce::String (f, 1) + " Hz)");
        check (rms (out[0], at (1.8), at (2.0)) < 1.0e-4f, "the note ends (with its release)");

        edit.getTrack (keys).setProperty (daw::ids::transpose, 12.0, nullptr);
        engine.rebuildNow();
        engine.seek (0);
        engine.play();
        out = render (engine, 1.3);
        engine.stop();
        const auto up = estimateFrequency (out[0], at (0.8), at (1.2));
        check (std::abs (up - 880.0) < 6.0, "the track's transpose moves its MIDI notes (" + juce::String (up, 1) + " Hz)");
        edit.getTrack (keys).removeProperty (daw::ids::transpose, nullptr);
        engine.rebuildNow();
        render (engine, 1.0); // let the release ring out

        // Outside the clip's window: not played.
        edit.getUndoManager().beginNewTransaction();
        edit.trimClipStart (clip, 0.9); // the window now starts after the note's start
        engine.rebuildNow();
        engine.seek (0);
        engine.play();
        out = render (engine, 2.0);
        engine.stop();
        check (rms (out[0], 0, at (2.0)) == 0.0f, "a note that starts before the clip's window doesn't play");
        edit.getUndoManager().undo();
        check (std::abs ((double) edit.getClip (clip)[daw::ids::start] - 0.5) < 1.0e-9, "(undo the trim)");

        // Stopping mid-note: the note stops.
        engine.rebuildNow();
        engine.seek (0);
        engine.play();
        render (engine, 0.9);
        engine.stop();
        out = render (engine, 1.0);
        check (rms (out[0], at (0.6), at (1.0)) < 1.0e-4f, "stopping releases the notes that were playing");

        // Split and duplicate copy the notes with ids of their own.
        const auto right = edit.splitClip (clip, 1.0);
        const auto copy = edit.duplicateClip (clip);
        std::set<int> noteIds;
        int notes = 0;
        for (const auto& c : edit.getTrack (keys))
            for (const auto& n : c)
                if (n.hasType (daw::ids::NOTE))
                {
                    noteIds.insert ((int) n[daw::ids::id]);
                    ++notes;
                }
        check (right > 0 && copy > 0 && notes == 3 && (int) noteIds.size() == 3, "split and duplicate give copied notes their own ids");
        edit.removeClip (right);
        edit.removeClip (copy);

        // Live MIDI: an armed MIDI track plays the MIDI input, even while stopped.
        edit.setTrackArm (keys, true);
        out = renderWithMidi (engine, 0.5, { { 0, juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100) },
                                             { (int) at (0.3), juce::MidiMessage::noteOff (1, 60) } });
        check (rms (out[0], at (0.05), at (0.25)) > 0.01f, "an armed MIDI track plays the MIDI input live");

        engine.addLiveMidi (juce::MidiMessage::noteOn (1, 64, (juce::uint8) 90));
        out = render (engine, 0.1);
        check (rms (out[0], at (0.02), at (0.1)) > 0.01f && engine.getHeldNotes()[64], "notes from the on-screen keyboard play too");
        engine.addLiveMidi (juce::MidiMessage::noteOff (1, 64));
        render (engine, 0.6);
        check (! engine.getHeldNotes()[64], "and stop");

        // Recording MIDI: armed MIDI tracks get a clip with what was played.
        engine.setMidiRecordingLatency (480); // 10 ms
        engine.seek ((juce::int64) (2.0 * sampleRate));
        check (engine.startRecording (folder).isEmpty() && engine.isRecording(), "records with only a MIDI track armed");

        const auto start = (int) (2.0 * sampleRate);
        renderWithMidi (engine, 1.5,
                        { { start + (int) at (0.25), juce::MidiMessage::noteOn (1, 62, (juce::uint8) 80) },
                          { start + (int) at (0.5), juce::MidiMessage::controllerEvent (1, 64, 127) },  // pedal down
                          { start + (int) at (0.6), juce::MidiMessage::noteOff (1, 62) },               // held by the pedal
                          { start + (int) at (0.7), juce::MidiMessage::noteOn (1, 65, (juce::uint8) 110) },
                          { start + (int) at (0.8), juce::MidiMessage::noteOff (1, 65) },
                          { start + (int) at (1.0), juce::MidiMessage::controllerEvent (1, 64, 0) } },  // pedal up
                        start);
        engine.stopRecording();

        juce::ValueTree take;
        for (const auto& c : edit.getTrack (keys))
            if (c.hasType (daw::ids::CLIP) && std::abs ((double) c[daw::ids::start] - 2.0) < 1.0e-6)
                take = c;

        check (take.isValid() && daw::Edit::isMidiClip (take) && std::abs ((double) take[daw::ids::length] - 1.5) < 0.01,
               "the take is a MIDI clip from the record start to the stop");

        if (take.isValid())
        {
            const auto first = take.getChild (0), second = take.getChild (1);
            check (take.getNumChildren() == 2 && (int) first[daw::ids::pitch] == 62 && (int) first[daw::ids::velocity] == 80
                       && std::abs ((double) first[daw::ids::time] - 0.24) < 0.002,
                   "notes are recorded with their pitch, velocity and time (minus the output latency): "
                       + juce::String ((double) first[daw::ids::time], 3) + " s");
            check (std::abs ((double) first[daw::ids::length] - 0.75) < 0.002 && std::abs ((double) second[daw::ids::length] - 0.3) < 0.002,
                   "the sustain pedal holds notes until it lifts");
        }

        check (edit.getUndoManager().getUndoDescription() == "Record", "the take is one undo step");
        edit.setTrackArm (keys, false);

        // Projects: MIDI tracks, instruments and notes survive a save and open.
        {
            daw::Project project (edit);
            check (project.createNew (folder.getChildFile ("Song")).isEmpty(), "new project");
            const auto instrument = edit.addMidiTrack ("Pad", *daw::instruments::findBuiltin ("builtin:pad"));
            const auto padClip = edit.addMidiClip ((int) instrument[daw::ids::id], 0.0, 2.0);
            edit.addNote ((int) padClip[daw::ids::id], 48, 0.0, 1.0, 64);
            check (project.save().isEmpty() && project.getMissingFiles().isEmpty(), "saves (MIDI clips aren't missing files)");

            daw::Edit reopened;
            daw::Project second (reopened);
            check (second.open (project.getFolder()).isEmpty(), "opens");

            juce::ValueTree pad;
            for (const auto& t : reopened.getTracks())
                if (t[daw::ids::name].toString() == "Pad")
                    pad = t;

            const auto reNote = pad.getChildWithName (daw::ids::CLIP).getChildWithName (daw::ids::NOTE);
            check (daw::Edit::isMidiTrack (pad) && pad.getChildWithName (daw::ids::INSTRUMENT)[daw::ids::plugin].toString() == "builtin:pad"
                       && daw::Edit::isMidiClip (pad.getChildWithName (daw::ids::CLIP)) && (int) reNote[daw::ids::pitch] == 48
                       && second.getMissingFiles().isEmpty(),
                   "MIDI tracks, instruments and notes come back from the project file");

            // Export renders the instruments offline.
            daw::Exporter::Settings settings;
            settings.end = 1.0;
            settings.tail = 0.5;
            daw::Exporter exporter (reopened, settings);
            const auto result = exporter.exportMix (folder.getChildFile ("midi.wav"));
            check (result.error.isEmpty() && result.peakDecibels > -40.0f,
                   "the mix export plays the MIDI tracks (peak " + juce::String (result.peakDecibels, 1) + " dB)");
            second.close();
            project.close();
        }

        folder.deleteRecursively();
    }

    void testPluginHosting()
    {
        std::cout << "\nInstrument plugins (scan, load, play, keep settings)\n";

        const auto pluginFile = juce::File (MICFX_TEST_SYNTH);
        check (pluginFile.exists(), "the test instrument was built: " + pluginFile.getFullPathName());

        const auto data = juce::File::getSpecialLocation (juce::File::tempDirectory).getNonexistentChildFile ("micfx-plugins", {});
        const auto plugins = data.getChildFile ("Plugins");
        plugins.createDirectory();
        pluginFile.copyDirectoryTo (plugins.getChildFile (pluginFile.getFileName()));

        {
            daw::PluginHost host (data);
            host.setExtraFolders ({ plugins.getFullPathName() });
            host.startScan();

            for (int i = 0; i < 600 && host.isScanning(); ++i)
                juce::Thread::sleep (50);

            juce::Optional<daw::Edit::InstrumentInfo> found;
            for (const auto& info : host.getInstruments())
                if (info.name == "MicFX Test Synth")
                    found = info;

            check (! host.isScanning() && found.hasValue() && found->format == "VST3",
                   "a scan finds a VST3 instrument in an added folder" + (host.getFailedFiles().isEmpty() ? juce::String() : " (failed: " + host.getFailedFiles().joinIntoString (", ") + ")"));

            if (! found.hasValue())
            {
                data.deleteRecursively();
                return;
            }

            check (host.findInstrument (found->id).has_value() && host.getInstruments()[0].format == "builtin",
                   "the list has the built-in synths first, then plugins");

            daw::Edit edit;
            daw::DawEngine engine (edit);
            engine.prepare (sampleRate, blockSize);

            const auto track = (int) edit.addMidiTrack ("Plugin", *found)[daw::ids::id];
            const auto clip = (int) edit.addMidiClip (track, 0.0, 1.0)[daw::ids::id];
            edit.addNote (clip, 57, 0.1, 0.5); // A3, 220 Hz
            engine.rebuildNow();

            auto instrument = engine.getInstrument (track);
            check (instrument != nullptr && engine.getInstrumentError (track).isEmpty(),
                   "a MIDI track loads the plugin " + engine.getInstrumentError (track));

            if (instrument == nullptr)
            {
                data.deleteRecursively();
                return;
            }

            engine.play();
            auto out = render (engine, 1.0);
            engine.stop();
            const auto f = estimateFrequency (out[0], (size_t) (0.15 * sampleRate), (size_t) (0.55 * sampleRate));
            check (std::abs (f - 220.0) < 2.0 && rms (out[0], 0, (size_t) (0.09 * sampleRate)) == 0.0f
                       && rms (out[0], (size_t) (0.62 * sampleRate), (size_t) (1.0 * sampleRate)) == 0.0f,
                   "the plugin plays the clip's note on time (" + juce::String (f, 1) + " Hz)");

            // Its settings are kept: change the level, capture, reload.
            auto* level = instrument->getParameters()[0];
            level->setValueNotifyingHost (0.25f);
            instrument.reset();
            engine.captureInstrumentStates();
            const auto state = edit.getInstrument (track)[daw::ids::state].toString();
            check (state.isNotEmpty(), "the plugin's settings are stored in the project");

            daw::Edit copy;
            copy.replaceState (edit.getState().createCopy());
            daw::DawEngine second (copy);
            second.prepare (sampleRate, blockSize);
            second.rebuildNow();
            const auto restored = second.getInstrument (track);
            check (restored != nullptr && std::abs (restored->getParameters()[0]->getValue() - 0.25f) < 1.0e-3f,
                   "and come back when the project loads");

            // A plugin that isn't installed: the track stays, silent, with the reason.
            daw::Edit missing;
            daw::DawEngine third (missing);
            third.prepare (sampleRate, blockSize);
            auto info = *found;
            info.id = "VST3-Not Installed-0-0";
            info.name = "Not Installed";
            info.description = {};
            const auto lost = (int) missing.addMidiTrack ("Lost", info)[daw::ids::id];
            third.rebuildNow();
            check (third.getInstrument (lost) == nullptr && third.getInstrumentError (lost).contains ("isn't installed"),
                   "a missing plugin leaves the track silent and says why");

            // Offline: the export loads its own copy of the plugin.
            daw::Exporter::Settings settings;
            settings.end = 1.0;
            settings.tail = 0.0;
            daw::Exporter exporter (edit, settings);
            const auto result = exporter.exportMix (data.getChildFile ("plugin.wav"));
            check (result.error.isEmpty() && std::abs (result.peakDecibels - juce::Decibels::gainToDecibels (0.25f)) < 0.5f,
                   "the export plays the plugin with its settings (peak " + juce::String (result.peakDecibels, 1) + " dB)");
        }

        data.deleteRecursively();
    }

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

        // Warp and transpose through the page's commands: the session describes them, and
        // the clip's waveform and length come from its rendered copy.
        {
            const auto clipId = (int) imported["clip"];
            auto markers = juce::Array<juce::var> { juce::var (juce::Array<juce::var> { 0.0, 0.0 }), juce::var (juce::Array<juce::var> { 0.5, 1.0 }) };
            check (ok (run ("setWarp", args ({ { "id", clipId }, { "markers", markers } })))
                       && ok (run ("setClip", args ({ { "id", clipId }, { "transpose", 3.0 } })))
                       && ok (run ("setTrack", args ({ { "id", vocal }, { "transpose", -1.5 } }))),
                   "setWarp, clip transpose and track transpose complete");
            processor.getDawEngine().loadAllNow();
            const auto warped = lastSession();
            const auto clip = warped["tracks"][0]["clips"][0];
            const auto peaks = run ("peaks", args ({ { "clip", clipId } }));
            check (clip["warp"].size() == 2 && std::abs ((double) clip["transpose"] - 3.0) < 1e-9
                       && std::abs ((double) warped["tracks"][0]["transpose"] + 1.5) < 1e-9
                       && clip["audioKey"].toString().contains ("#")
                       && std::abs ((double) clip["fileLength"] - (songSeconds + 0.5)) < 0.01
                       && peaks["peaks"].isArray(),
                   "the session shows the warp, both transposes and the rendered length (" + clip["fileLength"].toString() + " s)");
            check (ok (run ("setClip", args ({ { "id", clipId }, { "stretch", "solo" }, { "formant", true } }))),
                   "setClip takes a stretch algorithm and formants");
            const auto chosen = lastSession()["tracks"][0]["clips"][0];
            check (chosen["stretch"].toString() == "solo" && (bool) chosen["formant"]
                       && chosen["audioKey"].toString() != clip["audioKey"].toString(),
                   "the session shows them, and the clip plays another rendered copy");
            run ("undo");
            check (lastSession()["tracks"][0]["clips"][0]["stretch"].toString() == "mix", "and they undo");
            run ("undo"); run ("undo"); run ("undo");
            check (! edit.getClip (clipId).hasProperty (daw::ids::warp), "and each undoes");
        }

        // Rhythm commands: tempo from a leading track (lined up with the bars), quantize
        // to the grid and to its groove, clear the warp. Each is one undo step.
        {
            std::vector<double> beats;
            for (int b = 0; b < 24; ++b)
                beats.push_back (0.25 + b * 0.5);
            const auto drumsFile = folder.getChildFile ("drums.wav");
            writeWav (drumsFile, makeHits (beats, 12.5, 4), sampleRate);
            std::vector<double> loose;
            for (int b = 0; b < 10; ++b)
                loose.push_back (0.27 + b * 0.5 + (b % 2 == 0 ? 0.03 : -0.02));
            const auto voiceFile = folder.getChildFile ("loose.wav");
            writeWav (voiceFile, makeHits (loose, 6.0), sampleRate);

            const auto drums = bridge.importAudio (drumsFile, -1);
            const auto voice = bridge.importAudio (voiceFile, -1);
            const auto drumsTrack = (int) drums["track"], voiceTrack = (int) voice["track"];
            const auto otherClip = (int) edit.getTracks()[0].getChildWithName (daw::ids::CLIP)[daw::ids::id];
            const auto otherStart = (double) edit.getClip (otherClip)[daw::ids::start];

            const auto detected = run ("detectTempo", args ({ { "track", drumsTrack }, { "apply", true }, { "lineUp", true } }));
            check (ok (detected) && std::abs ((double) detected["bpm"] - 120.0) < 0.3 && std::abs (edit.getTempo() - (double) detected["bpm"]) < 1e-9
                       && std::abs ((double) edit.getClip (otherClip)[daw::ids::start] - otherStart - (double) detected["shift"]) < 1e-9,
                   "detectTempo sets the tempo from the leading track and slides the song onto the bars ("
                       + detected["bpm"].toString() + " BPM, shift " + detected["shift"].toString() + " s)");
            run ("undo");
            check (std::abs ((double) edit.getClip (otherClip)[daw::ids::start] - otherStart) < 1e-9, "and undoes in one step");

            check (! ok (run ("detectTempo", args ({ { "track", 9999 } })))
                       && ! ok (run ("quantize", args ({ { "track", 9999 } }))), "rhythm commands need a track");

            edit.setTempo (120.0);
            const auto quantized = run ("quantize", args ({ { "track", voiceTrack }, { "steps", 0.5 } }));
            const auto voiceClip = (int) edit.getTrack (voiceTrack).getChildWithName (daw::ids::CLIP)[daw::ids::id];
            check (ok (quantized) && (int) quantized["onsets"] == 10 && (int) quantized["moved"] == 5 // half were played on the grid
                       && edit.getClip (voiceClip).hasProperty (daw::ids::warp),
                   "quantize warps a track's clips to the grid (" + quantized["moved"].toString() + " onsets moved)");

            const auto grooved = run ("quantize", args ({ { "track", voiceTrack }, { "groove", drumsTrack }, { "steps", 1.0 }, { "strength", 0.8 } }));
            check (ok (grooved) && (int) grooved["moved"] == 5, "quantize to the leading track's groove");

            check (ok (run ("clearWarp", args ({ { "clips", juce::Array<juce::var> { voiceClip } } })))
                       && ! edit.getClip (voiceClip).hasProperty (daw::ids::warp), "clearWarp puts the clip back as recorded");

            run ("removeTrack", args ({ { "id", voiceTrack } }));
            run ("removeTrack", args ({ { "id", drumsTrack } }));
        }

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

        // The library sidebar: browse a folder (audio files and subfolders only), then drop
        // a file at a time on a track (importFile).
        {
            const auto library = folder.getChildFile ("Library");
            library.getChildFile ("Loops").createDirectory();
            writeWav (library.getChildFile ("Loops").getChildFile ("Beat.wav"), songAudio, 44100.0);
            writeWav (library.getChildFile ("b.wav"), songAudio, 44100.0);
            writeWav (library.getChildFile ("a.wav"), songAudio, 44100.0);
            library.getChildFile ("readme.txt").replaceWithText ("not audio");

            const auto listing = run ("browse", args ({ { "path", library.getFullPathName() } }));
            check (ok (listing) && listing["folders"].size() == 1 && listing["folders"][0]["name"].toString() == "Loops"
                       && listing["files"].size() == 2 && listing["files"][0]["name"].toString() == "a.wav"
                       && listing["files"][1]["name"].toString() == "b.wav",
                   "Browse lists a folder's subfolders and audio files, sorted, without other files");
            check (! ok (run ("browse", args ({ { "path", library.getChildFile ("gone").getFullPathName() } }))),
                   "Browse reports a folder that isn't there");
            check (ok (run ("places")) && run ("places")["places"].isArray(), "Places answers with the library's places");

            const auto clipsBefore = edit.getTrack (vocal).getNumChildren();
            const auto dropped = run ("importFile", args ({ { "file", library.getChildFile ("a.wav").getFullPathName() },
                                                            { "track", vocal }, { "start", 4.0 } }));
            check (ok (dropped) && (int) dropped["track"] == vocal && std::abs ((double) dropped["start"] - 4.0) < 1.0e-9
                       && edit.getTrack (vocal).getNumChildren() == clipsBefore + 1,
                   "Dropping a file from the library puts a clip where it was dropped");
            run ("undo");
            check (! ok (run ("importFile", args ({ { "file", "relative.wav" }, { "track", vocal } }))),
                   "importFile refuses a path that isn't a file");
        }

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

        // MIDI tracks: instruments for the library, a MIDI track, a clip, notes.
        {
            const auto list = run ("instruments");
            check ((bool) list["ok"] && list["instruments"].size() >= 7 && list["instruments"][0]["kind"].toString() == "builtin",
                   "\"instruments\" lists the built-in synths (for the library)");

            const auto added = run ("addInstrument", args ({ { "track", -1 }, { "instrument", "builtin:bass" } }));
            const auto midiTrack = (int) added["track"];
            auto session = lastSession();
            juce::var described;
            for (int i = 0; i < session["tracks"].size(); ++i)
                if ((int) session["tracks"][i]["id"] == midiTrack)
                    described = session["tracks"][i];
            check ((bool) added["ok"] && described["kind"].toString() == "midi" && described["instrument"]["name"].toString() == "Synth Bass",
                   "dropping an instrument below the tracks makes a MIDI track with it");

            const auto clipId = (int) run ("addMidiClip", args ({ { "track", midiTrack }, { "start", 1.0 }, { "length", 2.0 } }));
            auto notes = juce::Array<juce::var> { args ({ { "pitch", 60 }, { "time", 0.0 }, { "length", 0.5 }, { "velocity", 90 } }),
                                                  args ({ { "pitch", 64 }, { "time", 0.5 }, { "length", 0.5 } }) };
            const auto edited = run ("editNotes", args ({ { "clip", clipId }, { "add", notes } }));
            check (clipId > 0 && (bool) edited["ok"] && edited["added"].size() == 2, "a MIDI clip takes notes");

            const auto firstNote = (int) edited["added"][0];
            run ("editNotes", args ({ { "clip", clipId }, { "change", juce::Array<juce::var> { args ({ { "id", firstNote }, { "pitch", 62 } }) } },
                                      { "remove", juce::Array<juce::var> { edited["added"][1] } } }));
            session = lastSession();
            juce::var clip;
            for (int i = 0; i < session["tracks"].size(); ++i)
                if ((int) session["tracks"][i]["id"] == midiTrack)
                    clip = session["tracks"][i]["clips"][0];
            check ((bool) clip["midi"] && ! (bool) clip["missing"] && clip["notes"].size() == 1 && (int) clip["notes"][0]["pitch"] == 62
                       && (int) clip["notes"][0]["velocity"] == 90,
                   "notes can be changed and removed, and the session describes them");

            check (! (bool) run ("normaliseClip", args ({ { "id", clipId } }))["ok"], "audio-only clip tools refuse MIDI clips");
            const auto pasted = (int) run ("pasteClip", args ({ { "midi", true }, { "track", midiTrack }, { "start", 4.0 }, { "length", 1.0 },
                                                                 { "offset", 0.0 }, { "notes", juce::Array<juce::var> { args ({ { "pitch", 70 }, { "time", 0.1 }, { "length", 0.2 } }) } } }));
            check (pasted > 0 && daw::Edit::isMidiClip (edit.getClip (pasted)) && edit.getClip (pasted).getNumChildren() == 1,
                   "a copied MIDI clip pastes with its notes");

            const auto swapped = run ("addInstrument", args ({ { "track", midiTrack }, { "instrument", "builtin:organ" } }));
            check ((int) swapped["track"] == midiTrack && edit.getInstrument (midiTrack)[daw::ids::name].toString() == "Organ",
                   "dropping an instrument on a MIDI track swaps its instrument");
            check (! (bool) run ("addInstrument", args ({ { "track", -1 }, { "instrument", "VST3-Nothing" } }))["ok"],
                   "an unknown instrument is refused");
            check ((bool) run ("liveNote", args ({ { "note", 60 }, { "velocity", 100 } }))["ok"]
                       && (bool) run ("liveNote", args ({ { "note", 60 }, { "velocity", 0 } }))["ok"], "liveNote plays notes");
        }

        // Starting the app again reopens the last project.
        {
            PluginProcessor again;
            again.prepareToPlay (sampleRate, blockSize);
            DawBridge reopened (again, settings, [] (const juce::Identifier&, const juce::var&) {}, [] { return (juce::int64) 0; });
            check (again.getDawProject().getFolder() == legacy && again.isDawMode() && again.getDawProject().isAutosaving(),
                   "the app reopens the last project in DAW mode, autosaving");
            again.getDawProject().close();
        }

        // Auto Mix tags: guessed from the name before listening, picked by hand, saved
        // on the track, and "auto" guesses again.
        {
            const auto kick = (int) edit.addTrack ("Kick")[daw::ids::id];
            const auto plain = (int) edit.addTrack ("Track 9")[daw::ids::id];
            const auto roleOf = [&] (const juce::var& tags, int trackId)
            {
                for (const auto& track : *tags["tracks"].getArray())
                    if ((int) track["id"] == trackId)
                        return track["role"].toString() + ((bool) track["tagged"] ? " (tagged)" : "");
                return juce::String ("missing");
            };

            auto tags = run ("autoMix", args ({ { "action", "tags" } }));
            check (roleOf (tags, kick) == "drums" && roleOf (tags, plain).isEmpty() && tags["roles"].getArray()->size() == 11,
                   "tags: guessed from the names before listening (" + roleOf (tags, kick) + ", '" + roleOf (tags, plain) + "')");

            tags = run ("autoMix", args ({ { "action", "tag" }, { "track", plain }, { "role", "background" } }));
            check (roleOf (tags, plain) == "background (tagged)" && edit.getTrack (plain)[daw::ids::mixRole].toString() == "background",
                   "tagging a track saves its role on it");

            tags = run ("autoMix", args ({ { "action", "tag" }, { "track", plain }, { "role", "auto" } }));
            check (roleOf (tags, plain).isEmpty() && ! edit.getTrack (plain).hasProperty (daw::ids::mixRole), "auto forgets the tag");
            edit.removeTrack (kick);
            edit.removeTrack (plain);
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

    // The DAW screen's key panel: detect the key from the tracks, keep it in the session,
    // and point the autotunes at it.
    void testDawKey()
    {
        std::cout << "\nSong key in DAW mode\n";

        const auto folder = juce::File::getSpecialLocation (juce::File::tempDirectory).getNonexistentChildFile ("micfx-key", {});
        folder.createDirectory();

        juce::PropertiesFile::Options options;
        options.applicationName = "MicFXTests";
        options.filenameSuffix = ".settings";
        juce::PropertiesFile settings (folder.getChildFile ("test.settings"), options);

        PluginProcessor processor;
        processor.prepareToPlay (sampleRate, blockSize);
        auto& edit = processor.getDawEdit();

        std::vector<std::pair<juce::String, juce::var>> events;
        DawBridge::availableForTesting = true;
        DawBridge bridge (processor, settings,
                          [&] (const juce::Identifier& event, const juce::var& payload) { events.emplace_back (event.toString(), payload); },
                          [] { return (juce::int64) 0; });

        const auto run = [&] (const juce::String& command, std::initializer_list<std::pair<const char*, juce::var>> properties = {})
        {
            auto* object = new juce::DynamicObject();
            for (const auto& [name, value] : properties)
                object->setProperty (name, value);
            juce::var result;
            bridge.handle (command, juce::var (object), [&] (juce::var r) { result = r; });
            return result;
        };

        run ("mode", { { "daw", true } });

        // A backing in F minor and a "Vocal" singing a melody that hangs on F#: the
        // voice is left out, so the key comes from the backing.
        const auto backing = folder.getChildFile ("backing.wav"), vocal = folder.getChildFile ("vocal.wav");
        writeWav (backing, makeChords ({ chord (65, { 0, 3, 7 }), chord (70 - 12, { 0, 3, 7 }), chord (72 - 12, { 0, 4, 7, 10 }),
                                         chord (65, { 0, 3, 7 }) }, sampleRate), sampleRate);
        writeWav (vocal, makeChords ({ { 66 }, { 66 }, { 73 }, { 66 } }, sampleRate), sampleRate);

        const auto vocalTrack = (int) edit.getTracks()[0][daw::ids::id];
        edit.getTrack (vocalTrack).setProperty (daw::ids::name, "Vocal", nullptr);
        bridge.importAudio (vocal, vocalTrack);
        bridge.importAudio (backing, -1);
        const auto autotune = (int) run ("addInsert", { { "track", vocalTrack }, { "type", "pitch" } });
        edit.setInsertValue (autotune, "tuneScale", 6.0f); // Blues

        // Detect: runs in the background, then "dawKey" brings the result.
        check ((bool) run ("key", { { "action", "detect" } })["ok"], "detect starts");
        juce::var result;
        for (int i = 0; i < 1000 && ! result.isObject(); ++i)
        {
            events.clear();
            bridge.tick();
            for (const auto& [name, payload] : events)
                if (name == "dawKey" && payload["state"].toString() == "ready")
                    result = payload;
            if (! result.isObject())
                juce::Thread::sleep (10);
        }

        const auto session = juce::JSON::parse (juce::JSON::toString (bridge.describeSession()));
        check (result["name"].toString() == "F minor" && edit.getKey() == 5 && edit.isKeyMinor()
                   && session["key"]["name"].toString() == "F minor",
               "detected " + result["name"].toString() + " from the backing (the session now says "
                   + session["key"]["name"].toString() + ")");

        bool vocalLeftOut = false;
        for (const auto& track : *result["tracks"].getArray())
            if ((int) track["id"] == vocalTrack)
                vocalLeftOut = (bool) track["voice"] && ! (bool) track["used"];
        check (vocalLeftOut && result["chroma"].size() == 12, "the vocal track is listed but left out of the key");

        edit.getUndoManager().undo();
        check (edit.getKey() == -1, "undo forgets the detected key");
        edit.getUndoManager().redo();

        // Autotune to the key: the key changes; Blues fits a minor song, so it stays.
        const auto applied = run ("key", { { "action", "autotune" } });
        const auto insert = edit.getInsert (autotune);
        check ((int) applied["count"] == 1 && (int) (float) insert["tuneKey"] == 5 && (int) (float) insert["tuneScale"] == 6,
               "autotune set to F, keeping its Blues scale (" + juce::String ((int) applied["count"]) + " autotune)");

        // A major key replaces a minor-only scale with Major; new autotunes start in key.
        run ("key", { { "action", "set" }, { "key", 7 }, { "minor", false } });
        run ("key", { { "action", "autotune" } });
        const auto fresh = (int) run ("addInsert", { { "track", vocalTrack }, { "type", "pitch" } });
        check ((int) (float) edit.getInsert (autotune)["tuneScale"] == 1 && (int) (float) edit.getInsert (fresh)["tuneKey"] == 7
                   && (int) (float) edit.getInsert (fresh)["tuneScale"] == 1,
               "G major by hand: Blues becomes Major, and a new autotune starts in G major");

        // The key survives saving and opening the project.
        {
            const auto projectFolder = folder.getChildFile ("Song");
            check ((bool) bridge.saveProjectAs (projectFolder)["ok"], "saved");
            edit.setKey (-1, false);
            const auto saved = processor.getDawProject().getFolder();
            processor.getDawProject().close();
            bridge.openProject (saved);
            check (edit.getKey() == 7 && ! edit.isKeyMinor() && edit.getState()[daw::ids::key].isInt(), "the key is saved in the project");
            processor.getDawProject().close();
        }

        DawBridge::availableForTesting = false;
        folder.deleteRecursively();
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

    // Frequency of a steady tone from its zero crossings, between `from` and `to` seconds.
    double measureFrequency (const juce::AudioBuffer<float>& audio, double from, double to)
    {
        const auto* x = audio.getReadPointer (0);
        const auto a = (int) (from * sampleRate), b = juce::jmin (audio.getNumSamples(), (int) (to * sampleRate));
        int first = -1, last = -1, crossings = 0;

        for (int i = a + 1; i < b; ++i)
        {
            if (x[i - 1] < 0.0f && x[i] >= 0.0f)
            {
                if (first < 0) first = i; else ++crossings;
                last = i;
            }
        }

        return crossings > 0 ? crossings * sampleRate / (last - first) : 0.0;
    }

    // Where the loudest sample is, in seconds.
    double findPeak (const juce::AudioBuffer<float>& audio, double from, double to)
    {
        const auto* x = audio.getReadPointer (0);
        auto best = (int) (from * sampleRate);
        for (auto i = best; i < juce::jmin (audio.getNumSamples(), (int) (to * sampleRate)); ++i)
            if (std::abs (x[i]) > std::abs (x[best]))
                best = i;
        return best / sampleRate;
    }

    void testWarp()
    {
        std::cout << "\nTime warp and transpose (clips stay dry)\n";
        using daw::warp::Map;
        using daw::warp::Marker;

        // The map.
        const Map map ({ { 1.0, 1.5 }, { 0.5, 0.5 }, { 1.2, 1.4 }, { 2.0, 2.5 } }); // { 1.2, 1.4 } would run backwards
        check (map.getMarkers().size() == 3, "markers are sorted and one that runs time backwards is dropped");
        check (std::abs (map.toTarget (0.25) - 0.25) < 1e-9 && std::abs (map.toTarget (0.75) - 1.0) < 1e-9
                   && std::abs (map.toTarget (3.0) - 3.5) < 1e-9,
               "time runs at normal speed outside the markers and stretches between them");
        auto roundTrip = true;
        for (double s = 0.0; s < 3.0; s += 0.137)
            roundTrip = roundTrip && std::abs (map.toSource (map.toTarget (s)) - s) < 1e-9;
        check (roundTrip, "toSource undoes toTarget");
        check (Map::fromString (map.toString()) == map && Map().toString().isEmpty() && Map::fromString ({}).isIdentity(),
               "a map saves to text and back; no markers is the identity");
        check (std::abs (map.getRateAt (0.75) - 0.5) < 1e-9, "the rate between markers (half speed when stretched 2x)");

        // A voice-like tone, and the same through a map that only slides it later.
        const auto voice = makeVoice (220.0f, 2.0);
        juce::AudioBuffer<float> source (2, (int) voice.size());
        for (int ch = 0; ch < 2; ++ch)
            source.copyFrom (ch, 0, voice.data(), (int) voice.size());

        daw::warp::Settings slide;
        slide.map = Map ({ { 0.0, 0.25 } });
        const auto slid = daw::warp::render (source, sampleRate, slide);
        double worst = 0.0;
        const auto offset = (int) (0.25 * sampleRate);
        for (int i = (int) (0.3 * sampleRate); i < (int) (2.0 * sampleRate); ++i)
            worst = std::max (worst, (double) std::abs (slid.getSample (1, i) - source.getSample (1, i - offset)));
        check (slid.getNumSamples() == (int) (2.25 * sampleRate) && rms (std::vector<float> (slid.getReadPointer (0), slid.getReadPointer (0) + offset - 4096), 0, (size_t) offset - 4096) < 1e-4f,
               "sliding the audio later adds silence in front");
        check (worst < 0.01, "audio that only moves comes through unchanged (worst error " + juce::String (worst, 4) + ")");

        // Stretch the first second to two: the pitch stays, the length grows.
        juce::AudioBuffer<float> sine (1, (int) (2.0 * sampleRate));
        for (int i = 0; i < sine.getNumSamples(); ++i)
            sine.setSample (0, i, 0.5f * (float) std::sin (juce::MathConstants<double>::twoPi * 440.0 * i / sampleRate));

        daw::warp::Settings stretch;
        stretch.map = Map ({ { 0.0, 0.0 }, { 1.0, 2.0 } });
        const auto stretched = daw::warp::render (sine, sampleRate, stretch);
        const auto slowPitch = measureFrequency (stretched, 0.3, 1.7);
        check (stretched.getNumSamples() == (int) (3.0 * sampleRate), "stretching the first second to two makes 3 s");
        check (std::abs (slowPitch - 440.0) < 1.0, "time stretch keeps the pitch (" + juce::String (slowPitch, 1) + " Hz)");
        check (rms (std::vector<float> (stretched.getReadPointer (0), stretched.getReadPointer (0) + stretched.getNumSamples()),
                    (size_t) (0.3 * sampleRate), (size_t) (2.7 * sampleRate)) > 0.3f,
               "and keeps the level steady");

        // Transpose: an octave up, then a fifth down, same length.
        for (const auto& [semitones, expected] : { std::pair { 12.0, 880.0 }, std::pair { -7.0, 440.0 * std::pow (2.0, -7.0 / 12.0) } })
        {
            daw::warp::Settings transpose;
            transpose.semitones = semitones;
            const auto shifted = daw::warp::render (sine, sampleRate, transpose);
            const auto pitch = measureFrequency (shifted, 0.3, 1.7);
            check (shifted.getNumSamples() == sine.getNumSamples() && std::abs (pitch - expected) < expected * 0.005,
                   juce::String (semitones, 0) + " semitones: " + juce::String (pitch, 1) + " Hz (expected " + juce::String (expected, 1) + ")");
        }

        // A hit moved by a marker lands where the marker says, and the one before stays.
        juce::AudioBuffer<float> hits (1, (int) (2.0 * sampleRate));
        hits.clear();
        for (const auto at : { 0.5, 1.0 })
            for (int i = 0; i < (int) (0.08 * sampleRate); ++i)
                hits.setSample (0, (int) (at * sampleRate) + i,
                                0.8f * (float) std::exp (-i / (0.01 * sampleRate)) * (float) std::sin (0.2 * i));

        daw::warp::Settings nudge;
        nudge.map = Map ({ { 0.5, 0.5 }, { 1.0, 1.1 } });
        const auto nudged = daw::warp::render (hits, sampleRate, nudge);
        const auto firstHit = findPeak (nudged, 0.3, 0.8), secondHit = findPeak (nudged, 0.9, 1.4);
        check (std::abs (firstHit - 0.5) < 0.01 && std::abs (secondHit - 1.1) < 0.01,
               "a hit moved from 1.0 s to 1.1 s lands there (" + juce::String (firstHit, 3) + " s, " + juce::String (secondHit, 3) + " s)");

        // Every algorithm: a stretch keeps the length (and the pitch, except varispeed), and
        // a moved hit lands where its marker says.
        for (const auto& key : daw::warp::getAlgorithmKeys())
        {
            const auto algorithm = daw::warp::algorithmFromKey (key);
            check (daw::warp::getAlgorithmKey (algorithm) == key, key + ": its key reads back");

            daw::warp::Settings longer;
            longer.map = Map ({ { 0.0, 0.0 }, { 1.0, 2.0 } });
            longer.algorithm = algorithm;
            const auto out = daw::warp::render (sine, sampleRate, longer);
            const auto pitch = measureFrequency (out, 0.3, 1.7);
            const auto expected = algorithm == daw::warp::Algorithm::varispeed ? 220.0 : 440.0;
            const auto level = rms (std::vector<float> (out.getReadPointer (0), out.getReadPointer (0) + out.getNumSamples()),
                                    (size_t) (0.3 * sampleRate), (size_t) (1.7 * sampleRate));
            if (algorithm != daw::warp::Algorithm::slice) // slicing leaves the stretched part's end silent
                check (out.getNumSamples() == (int) (3.0 * sampleRate) && std::abs (pitch - expected) < expected * 0.005 && level > 0.3f,
                       key + ": stretched 2x, " + juce::String (pitch, 1) + " Hz (expected " + juce::String (expected, 0)
                           + "), level " + juce::String (level, 2));

            daw::warp::Settings moved;
            moved.map = Map ({ { 0.5, 0.5 }, { 1.0, 1.1 } });
            moved.algorithm = algorithm;
            const auto hitsOut = daw::warp::render (hits, sampleRate, moved);
            const auto a = findPeak (hitsOut, 0.3, 0.8), b = findPeak (hitsOut, 0.9, 1.4);
            check (std::abs (a - 0.5) < 0.01 && std::abs (b - 1.1) < 0.01,
                   key + ": a hit moved to 1.1 s lands there (" + juce::String (a, 3) + " s, " + juce::String (b, 3) + " s)");
        }

        // Slice and efficient move audio that only slides without touching it.
        for (const auto algorithm : { daw::warp::Algorithm::slice, daw::warp::Algorithm::efficient })
        {
            daw::warp::Settings slideOnly = slide;
            slideOnly.algorithm = algorithm;
            const auto out = daw::warp::render (source, sampleRate, slideOnly);
            double error = 0.0;
            for (int i = offset + 2048; i < (int) (2.0 * sampleRate); ++i)
                error = std::max (error, (double) std::abs (out.getSample (1, i) - source.getSample (1, i - offset)));
            check (error < 1.0e-4, daw::warp::getAlgorithmKey (algorithm) + ": a slide comes through unchanged (worst " + juce::String (error, 6) + ")");
        }

        // Formants: a buzz through a resonance at 1 kHz, up 5 semitones. Without formants the
        // resonance moves up with the pitch; with them it stays near 1 kHz.
        {
            juce::AudioBuffer<float> buzz (1, (int) (2.0 * sampleRate));
            fx::Biquad resonance, second;
            resonance.setup (fx::Biquad::Type::bandPass, sampleRate, 1000.0, 4.0);
            second.setup (fx::Biquad::Type::bandPass, sampleRate, 1000.0, 4.0);
            const auto period = sampleRate / 140.0;
            double phase = 0.0;
            for (int i = 0; i < buzz.getNumSamples(); ++i)
            {
                phase += 1.0;
                auto x = 0.0f;
                if (phase >= period) { phase -= period; x = 1.0f; }
                buzz.setSample (0, i, 2.0f * second.process (0, resonance.process (0, x)));
            }

            // Where the spectrum's energy sits, 300 Hz..5 kHz.
            const auto centroid = [&] (const juce::AudioBuffer<float>& audio)
            {
                constexpr int order = 15, size = 1 << order;
                juce::dsp::FFT fft (order);
                std::vector<float> data ((size_t) size * 2, 0.0f);
                const auto start = (int) (0.4 * sampleRate);
                for (int i = 0; i < size && start + i < audio.getNumSamples(); ++i)
                    data[(size_t) i] = audio.getSample (0, start + i) * (0.5f - 0.5f * (float) std::cos (juce::MathConstants<double>::twoPi * i / size));
                fft.performFrequencyOnlyForwardTransform (data.data());
                double sum = 0.0, weighted = 0.0;
                for (int k = 1; k < size / 2; ++k)
                    if (const auto f = k * sampleRate / size; f > 300.0 && f < 5000.0)
                    {
                        const auto e = (double) data[(size_t) k] * data[(size_t) k];
                        sum += e;
                        weighted += e * f;
                    }
                return weighted / juce::jmax (1.0e-12, sum);
            };

            const auto original = centroid (buzz);
            for (const auto algorithm : { daw::warp::Algorithm::mix, daw::warp::Algorithm::solo })
            {
                daw::warp::Settings plain, kept;
                plain.semitones = kept.semitones = 5.0;
                plain.algorithm = kept.algorithm = algorithm;
                kept.formant = true;
                const auto moved = centroid (daw::warp::render (buzz, sampleRate, plain));
                const auto stayed = centroid (daw::warp::render (buzz, sampleRate, kept));
                check (moved > original * 1.15 && std::abs (stayed / original - 1.0) < 0.08,
                       daw::warp::getAlgorithmKey (algorithm) + ": +5 st moves the resonance to " + juce::String (moved, 0)
                           + " Hz, with formants kept it stays at " + juce::String (stayed, 0) + " Hz (from " + juce::String (original, 0) + ")");
            }
        }

        // Speed (for information: use the release build for real numbers).
        {
            juce::AudioBuffer<float> noise (2, (int) (20.0 * sampleRate));
            juce::Random random (1);
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < noise.getNumSamples(); ++i)
                    noise.setSample (ch, i, random.nextFloat() - 0.5f);

            daw::warp::Settings up;
            up.semitones = 2.0;
            const auto started = juce::Time::getMillisecondCounterHiRes();
            daw::warp::render (noise, sampleRate, up);
            std::cout << "  INFO  20 s of stereo transposed in "
                      << juce::String ((juce::Time::getMillisecondCounterHiRes() - started) / 1000.0, 2) << " s\n";
        }

        // In the engine: a transposed clip plays its rendered copy; the file is untouched.
        const auto folder = juce::File::createTempFile ("micfx-warp");
        folder.createDirectory();
        const auto file = folder.getChildFile ("sine.wav");
        writeWav (file, sine, sampleRate);
        const auto before = file.getSize();

        daw::Edit edit;
        daw::DawEngine engine (edit, false);
        engine.prepare (sampleRate, blockSize);
        const auto track = (int) edit.addTrack ("Voice")[daw::ids::id];
        const auto clip = (int) edit.addClip (track, file, 0.0, 2.0)[daw::ids::id];

        const auto play = [&]
        {
            engine.loadAllNow();
            engine.seek (0);
            engine.play();
            juce::AudioBuffer<float> out (2, (int) (2.0 * sampleRate));
            for (int done = 0; done < out.getNumSamples(); done += blockSize)
            {
                juce::AudioBuffer<float> block (2, blockSize);
                engine.process (block, 2, blockSize);
                for (int ch = 0; ch < 2; ++ch)
                    out.copyFrom (ch, done, block, ch, 0, juce::jmin (blockSize, out.getNumSamples() - done));
            }
            engine.stop();
            return out;
        };

        edit.setClipTranspose (clip, 12.0);
        auto out = play();
        check (std::abs (measureFrequency (out, 0.3, 1.7) - 880.0) < 5.0, "a clip transposed +12 plays an octave up");

        edit.setClipTranspose (clip, 0.0);
        edit.setTrackTranspose (track, -12.0);
        out = play();
        check (std::abs (measureFrequency (out, 0.3, 1.7) - 220.0) < 2.0, "the track's transpose applies to its clips");

        edit.setTrackTranspose (track, 5.0);
        edit.setClipTranspose (clip, -5.0);
        out = play();
        check (std::abs (measureFrequency (out, 0.3, 1.7) - 440.0) < 2.0, "clip and track transposes add up");
        check (file.getSize() == before, "the recording itself is never changed");

        edit.setClipWarp (clip, Map ({ { 0.0, 0.0 }, { 0.5, 1.0 } }));
        const auto right = edit.splitClip (clip, 1.5);
        check (daw::Edit::getClipWarp (edit.getClip (right)) == daw::Edit::getClipWarp (edit.getClip (clip)) && ! daw::Edit::getClipWarp (edit.getClip (clip)).isIdentity(),
               "a split keeps the warp on both halves");
        edit.setClipWarp (clip, Map());
        check (! edit.getClip (clip).hasProperty (daw::ids::warp), "clearing the warp removes it from the project");

        folder.deleteRecursively();
    }

    // Percussive hits (a decaying tone with a click) at the given times; the first of
    // every `accentEvery` is louder.
    juce::AudioBuffer<float> makeHits (const std::vector<double>& times, double seconds, int accentEvery)
    {
        juce::AudioBuffer<float> audio (1, (int) (seconds * sampleRate));
        audio.clear();
        juce::Random random (7);

        for (size_t h = 0; h < times.size(); ++h)
        {
            const auto level = accentEvery > 0 && h % (size_t) accentEvery == 0 ? 0.8f : 0.4f;
            const auto start = (int) std::round (times[h] * sampleRate);
            for (int i = 0; i < (int) (0.15 * sampleRate) && start + i < audio.getNumSamples(); ++i)
            {
                const auto decay = (float) std::exp (-i / (0.03 * sampleRate));
                const auto click = i < (int) (0.004 * sampleRate) ? (random.nextFloat() - 0.5f) : 0.0f;
                audio.addSample (0, start + i, level * decay * (0.6f * (float) std::sin (0.04 * i) + click));
            }
        }

        return audio;
    }

    void testRhythm()
    {
        std::cout << "\nRhythm: onsets, tempo, grooves and audio quantize\n";
        namespace rhythm = analysis::rhythm;

        // Onsets land on the attacks.
        const std::vector<double> times { 0.5, 0.83, 1.4, 1.62, 2.3 };
        const auto hits = makeHits (times, 3.0);
        const auto onsets = rhythm::findOnsets (hits, sampleRate);
        auto worst = 0.0;
        for (size_t i = 0; i < times.size() && i < onsets.size(); ++i)
            worst = std::max (worst, std::abs (onsets[i].time - times[i]));
        check (onsets.size() == times.size() && worst < 0.003,
               "finds every hit, within 3 ms (" + juce::String ((int) onsets.size()) + " found, worst " + juce::String (worst * 1000.0, 1) + " ms)");
        check (rhythm::findOnsets (hits, sampleRate, 1.0, 2.0).size() == 2, "only between the times asked for");

        // Tempo: 100 BPM played a little loosely, accent on beat one, starting at 0.3 s.
        std::vector<double> beats;
        juce::Random jitter (3);
        for (int b = 0; b < 40; ++b)
            beats.push_back (0.3 + b * 0.6 + (jitter.nextDouble() - 0.5) * 0.02);
        const auto song = makeHits (beats, 25.0, 4);
        const auto tempo = rhythm::detectTempo (song, sampleRate, 4);
        check (std::abs (tempo.bpm - 100.0) < 0.5, "detects 100 BPM (" + juce::String (tempo.bpm, 2) + ")");
        auto followed = tempo.beats.size() >= 38;
        for (const auto b : tempo.beats)
            followed = followed && std::any_of (beats.begin(), beats.end(), [b] (double t) { return std::abs (t - b) < 0.02; });
        check (followed, "follows the played beats (" + juce::String ((int) tempo.beats.size()) + " beats)");
        check (tempo.firstDownbeat == 0 && std::abs (tempo.gridStart - 0.3) < 0.02, "finds the accented downbeat and where the grid starts");

        // Grids.
        const auto grid = rhythm::makeGrid (120.0, 0.5, 1.0, 2.0);
        check (std::find_if (grid.begin(), grid.end(), [] (double t) { return std::abs (t - 1.25) < 1e-9; }) != grid.end()
                   && grid.front() < 1.0 && grid.back() > 2.0, "a straight eighth-note grid at 120 BPM");
        const auto swung = rhythm::makeGrid (120.0, 0.5, 0.0, 1.0, 0.0, 1.0);
        check (std::find_if (swung.begin(), swung.end(), [] (double t) { return std::abs (t - (0.25 + 0.25 / 3.0)) < 1e-9; }) != swung.end(),
               "full swing moves the off-beat eighths to the triplet");
        const auto groove = rhythm::makeGroove ({ 1.0, 1.5, 2.1, 2.6 }, 0.5, 0.0, 3.0);
        check (std::find_if (groove.begin(), groove.end(), [] (double t) { return std::abs (t - 1.8) < 1e-9; }) != groove.end()
                   && groove.front() <= 0.0 && groove.back() >= 3.0,
               "a groove divides the played beats and carries on past them");

        // Quantize a sloppy take on the timeline to the project's beat grid.
        const auto folder = juce::File::createTempFile ("micfx-rhythm");
        folder.createDirectory();
        const std::vector<double> played { 0.53, 0.97, 1.52, 2.04, 2.47, 3.03 }; // meant for every 0.5 s
        const auto takeFile = folder.getChildFile ("take.wav");
        writeWav (takeFile, makeHits (played, 4.0), sampleRate);

        daw::Edit edit;
        daw::AudioPool pool;
        pool.setSampleRate (sampleRate);
        edit.setTempo (120.0);
        const auto track = (int) edit.addTrack ("Drums")[daw::ids::id];
        const auto clip = (int) edit.addClip (track, takeFile, 1.0, 4.0)[daw::ids::id]; // the take starts at bar 1 + 2 beats

        const auto onTimeline = [&]
        {
            double rate = 0.0;
            const auto audio = daw::rhythm::renderTrack (edit, pool, track, rate, 1);
            std::vector<double> found;
            for (const auto& onset : rhythm::findOnsets (audio, rate))
                found.push_back (onset.time);
            return found;
        };

        const auto beatGrid = rhythm::makeGrid (120.0, 1.0, 0.0, 6.0);
        auto result = daw::rhythm::quantizeClips (edit, pool, { clip }, beatGrid, {});
        auto after = onTimeline();
        worst = 0.0;
        for (size_t i = 0; i < after.size(); ++i)
            worst = std::max (worst, std::abs (after[i] - (1.5 + 0.5 * (double) i)));
        check (result.onsets == 6 && result.moved == 6 && after.size() == 6 && worst < 0.004,
               "quantize puts every hit on the beat (worst " + juce::String (worst * 1000.0, 1) + " ms)");
        check (daw::AudioPool::getWarpKey (takeFile, daw::Edit::getWarpSettings (edit.getClip (clip))).contains ("#")
                   && takeFile.getSize() > 0, "by warping the clip; the take itself is untouched");

        daw::rhythm::Options half;
        half.strength = 0.5;
        daw::rhythm::quantizeClips (edit, pool, { clip }, beatGrid, half);
        after = onTimeline();
        check (after.size() == 6 && std::abs (after[0] - 1.515) < 0.004 && std::abs (after[1] - 1.985) < 0.004,
               "50% strength moves them halfway, from the unwarped take (" + juce::String (after[0], 3) + ", " + juce::String (after[1], 3) + ")");

        // Split pieces of one take share one warp (and one rendered copy).
        edit.setClipWarp (clip, {});
        const auto right = edit.splitClip (clip, 3.25);
        daw::rhythm::quantizeClips (edit, pool, daw::rhythm::getClipIds (edit, track), beatGrid, {});
        check (daw::Edit::getClipWarp (edit.getClip (clip)) == daw::Edit::getClipWarp (edit.getClip (right))
                   && ! daw::Edit::getClipWarp (edit.getClip (right)).isIdentity(),
               "pieces of one take are quantized together");

        // A groove from a leading track, and lining the song up with the bar lines.
        daw::Edit song2;
        const auto leaderFile = folder.getChildFile ("leader.wav");
        writeWav (leaderFile, song, sampleRate);
        const auto leader = (int) song2.addTrack ("Guitar")[daw::ids::id];
        song2.addClip (leader, leaderFile, 0.0, 25.0);
        const auto analysed = daw::rhythm::analyseTrack (song2, pool, leader);
        check (std::abs (analysed.bpm - 100.0) < 0.5, "analyses the leading track on the timeline (" + juce::String (analysed.bpm, 2) + " BPM)");

        song2.setTempo (analysed.bpm);
        const auto shift = daw::rhythm::getLineUpShift (song2, analysed);
        const auto bar = 4 * 60.0 / analysed.bpm;
        check (std::abs (shift - (bar - 0.3)) < 0.02, "lines the first downbeat up with a bar line, never before 0 s (shift "
                                                         + juce::String (shift, 3) + " s)");
        daw::rhythm::shiftSong (song2, shift);
        check (std::abs ((double) song2.getTracks()[0].getChildWithName (daw::ids::CLIP)[daw::ids::start] - shift) < 1e-9,
               "shifting the song moves its clips");

        folder.deleteRecursively();
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

    // Live readings for the UI: per-module levels and spectra, and the chain's own
    // input and output gain (DAW tracks) with its meters.
    void testLiveMeters()
    {
        std::cout << "\nLive effect readings and chain gains\n";

        const auto sineBlock = [] (juce::AudioBuffer<float>& buffer, double frequency, float level, int& phase)
        {
            for (int i = 0; i < buffer.getNumSamples(); ++i, ++phase)
                for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
                    buffer.setSample (ch, i, level * (float) std::sin (juce::MathConstants<double>::twoPi * frequency * phase / sampleRate));
        };

        // A compressor hit hard: less comes out than goes in.
        {
            fx::FxChain chain { { fx::ModuleType::compressor } };
            chain[0].findValue ("compThreshold")->store (-30.0f);
            chain[0].findValue ("compRatio")->store (8.0f);
            chain.prepare ({ sampleRate, (juce::uint32) blockSize, 2 });

            juce::AudioBuffer<float> buffer (2, blockSize);
            int phase = 0;
            for (int b = 0; b < 100; ++b)
            {
                sineBlock (buffer, 440.0, 0.5f, phase);
                juce::dsp::AudioBlock<float> block (buffer);
                chain.process (fx::Module::Context (block));
                if (b == 98) { chain[0].takeInputPeak(); chain[0].takeOutputPeak(); }
            }

            const auto in = chain[0].takeInputPeak(), out = chain[0].takeOutputPeak();
            check (std::abs (in - 0.5f) < 0.01f, "a module reports the peak going in (" + juce::String (in, 3) + ")");
            check (out < in * 0.5f, "and coming out: the compressor's reduction shows (" + juce::String (juce::Decibels::gainToDecibels (out / in), 1) + " dB)");
            check (chain[0].takeInputPeak() == 0.0f, "reading a peak resets it");

            fx::SpectrumAnalyser analyser;
            check (fx::describeMeter (chain[0], analyser)["spectrum"].isVoid(), "dynamics readings carry no spectrum");
        }

        // An EQ's readings carry the spectrum before and after it, once the UI asks.
        {
            fx::FxChain chain { { fx::ModuleType::paraEq } };
            chain[0].findValue ("peqOn")->store (1.0f);
            chain.prepare ({ sampleRate, (juce::uint32) blockSize, 2 });

            juce::AudioBuffer<float> buffer (2, blockSize);
            int phase = 0;
            const auto run = [&] (int blocks)
            {
                for (int b = 0; b < blocks; ++b)
                {
                    sineBlock (buffer, 1000.0, 0.5f, phase);
                    juce::dsp::AudioBlock<float> block (buffer);
                    chain.process (fx::Module::Context (block));
                }
            };

            run (20);
            check (! chain[0].getScope().isWanted(), "nothing is captured for a spectrum until the UI asks");

            fx::SpectrumAnalyser analyser;
            fx::describeMeter (chain[0], analyser); // asks
            run (20);
            const auto spectrum = fx::describeMeter (chain[0], analyser)["spectrum"];
            const auto* post = spectrum["post"].getArray();

            int loudest = -1;
            float loudestDb = -1000.0f;
            for (int b = 0; post != nullptr && b < post->size(); ++b)
                if ((float) (*post)[b] > loudestDb)
                    loudestDb = (float) (*post)[b], loudest = b;

            const auto centre = 20.0 * std::pow (1000.0, (loudest + 0.5) / fx::SpectrumAnalyser::numBands);
            check (post != nullptr && post->size() == fx::SpectrumAnalyser::numBands && spectrum["pre"].getArray() != nullptr,
                   "the spectrum has before and after bands");
            check (centre > 800.0 && centre < 1250.0 && loudestDb > -12.0f,
                   "a 1 kHz tone peaks in the 1 kHz band (" + juce::String (centre, 0) + " Hz, " + juce::String (loudestDb, 0) + " dB)");
            check ((float) (*post)[2] < loudestDb - 40.0f, "and the low bands stay quiet");

            chain[0].findValue ("peqOn")->store (0.0f);
            check (fx::describeMeter (chain[0], analyser).isVoid(), "a module that's off has no readings");
        }

        // Every effect's own input and output gain: in before it, out after it, and
        // nothing while it's off.
        {
            fx::FxChain chain { { fx::ModuleType::saturation } };
            auto& module = chain[0];
            module.findValue ("satOn")->store (1.0f);
            module.findValue ("satMix")->store (0.0f); // a clean pass, so the gains are all that change
            module.getInputGain().store (6.0f);
            module.getOutputGain().store (-12.0f);
            check (module.findValue ("saturationInGain") == &module.getInputGain()
                   && module.findValue ("saturationOutGain") == &module.getOutputGain(), "each effect has <key>InGain and <key>OutGain values");
            chain.prepare ({ sampleRate, (juce::uint32) blockSize, 2 });

            juce::AudioBuffer<float> buffer (2, blockSize);
            int phase = 0;
            const auto run = [&]
            {
                for (int b = 0; b < 40; ++b)
                {
                    sineBlock (buffer, 440.0, 0.25f, phase);
                    juce::dsp::AudioBlock<float> block (buffer);
                    chain.process (fx::Module::Context (block));
                }
                return juce::Decibels::gainToDecibels (buffer.getMagnitude (0, blockSize) / 0.25f);
            };

            const auto onDb = run();
            const auto in = module.takeInputPeak();
            check (std::abs (juce::Decibels::gainToDecibels (in / 0.25f) - 6.0f) < 0.2f, "an effect's input gain sets what it hears");
            check (std::abs (onDb + 6.0f) < 0.2f, "and its output gain what it passes on (" + juce::String (onDb, 1) + " dB)");

            module.findValue ("satOn")->store (0.0f);
            check (std::abs (run()) < 0.05f, "switched off, its gains are bypassed too");
        }

        // The chain's input and output gain.
        {
            fx::FxChain chain { { fx::ModuleType::eq } };
            chain.setGains (6.0f, -12.0f);
            chain.prepare ({ sampleRate, (juce::uint32) blockSize, 2 });

            juce::AudioBuffer<float> buffer (2, blockSize);
            int phase = 0;
            for (int b = 0; b < 40; ++b)
            {
                sineBlock (buffer, 440.0, 0.25f, phase);
                juce::dsp::AudioBlock<float> block (buffer);
                chain.process (fx::Module::Context (block));
            }

            const auto in = chain.takeInputPeak(), out = chain.takeOutputPeak();
            check (std::abs (juce::Decibels::gainToDecibels (in / 0.25f) - 6.0f) < 0.2f, "the chain's input gain raises what the effects get");
            check (std::abs (juce::Decibels::gainToDecibels (buffer.getMagnitude (0, blockSize) / 0.25f) + 6.0f) < 0.2f
                   && std::abs (juce::Decibels::gainToDecibels (out / 0.25f) + 6.0f) < 0.2f,
                   "and its output gain sets what leaves (+6 - 12 = -6 dB)");
        }

        // DAW tracks: gains are saved on the track and reach its chain.
        {
            daw::Edit edit;
            daw::DawEngine engine (edit);
            engine.prepare (sampleRate, blockSize);

            const auto vocal = (int) edit.addTrack ("Vocal")[daw::ids::id];
            edit.addInsert (vocal, fx::ModuleType::compressor);
            edit.setTrackMonitor (vocal, true);
            edit.setChainGain (vocal, false, 6.0f);
            edit.setChainGain (vocal, true, 40.0f);
            check (juce::approximatelyEqual ((float) edit.getTrack (vocal)[daw::ids::outGain], daw::Edit::maxChainGain), "chain gains are limited to +/-24 dB");
            edit.setChainGain (vocal, true, -6.0f);
            engine.rebuildNow();

            juce::AudioBuffer<float> output (2, blockSize);
            std::vector<float> mic ((size_t) blockSize, 0.0f);
            for (int b = 0; b < 40; ++b)
            {
                for (int i = 0; i < blockSize; ++i)
                    mic[(size_t) i] = 0.1f * (float) std::sin (juce::MathConstants<double>::twoPi * 220.0 * (b * blockSize + i) / sampleRate);
                engine.process (output, 2, blockSize, mic.data());
            }

            const auto [in, out] = engine.takeChainPeaks (vocal);
            check (std::abs (juce::Decibels::gainToDecibels (in / 0.1f) - 6.0f) < 0.3f, "a track's chain input gain is applied before its inserts");
            check (out > 0.0f && out < in, "and its output gain after them");

            int inserts = 0;
            engine.forEachInsert (vocal, [&] (int, fx::Module& module) { inserts += module.takeInputPeak() > 0.0f ? 1 : 0; });
            check (inserts == 1, "each insert's readings can be reached by insert id");
        }
    }
}

// Split into stems, without the model: a stand-in separator checks the chunking,
// crossfades, threads, cancelling and file writing, and the DAW side puts the stems on
// tracks. (The Demucs separator itself needs the downloaded model; see the README.)
static void testStems()
{
    std::cout << "\nSplit into stems\n";

    // Stand-in: stem s is (s + 1) / 10 of the input, so the stems sum to the mix.
    std::atomic<int> calls { 0 };
    const stems::Separator fake = [&calls] (const juce::AudioBuffer<float>& input, const stems::Progress& progress)
    {
        ++calls;
        std::vector<juce::AudioBuffer<float>> out;
        for (int s = 0; s < stems::numStems; ++s)
        {
            auto& stem = out.emplace_back (input);
            stem.applyGain ((float) (s + 1) / 10.0f);
        }
        return progress (1.0) ? out : std::vector<juce::AudioBuffer<float>>();
    };

    juce::Random random (7);
    juce::AudioBuffer<float> mix (2, (int) (75.3 * stems::modelSampleRate));
    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < mix.getNumSamples(); ++i)
            mix.setSample (ch, i, random.nextFloat() - 0.5f);

    stems::Settings settings;
    settings.threads = 3;
    settings.chunkSeconds = 20.0;

    std::vector<double> reported;
    std::mutex reportedLock;
    const auto separated = stems::separate (mix, fake, [&] (double p) { const std::scoped_lock l (reportedLock); reported.push_back (p); return true; },
                                            settings);

    auto worst = 0.0f;
    if (separated.size() == (size_t) stems::numStems)
        for (int s = 0; s < stems::numStems; ++s)
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < mix.getNumSamples(); ++i)
                    worst = std::max (worst, std::abs (separated[(size_t) s].getSample (ch, i) - mix.getSample (ch, i) * (s + 1) / 10.0f));

    check (separated.size() == (size_t) stems::numStems && separated[0].getNumSamples() == mix.getNumSamples() && worst < 1.0e-5f,
           "chunks crossfade back into seamless stems (worst error " + juce::String (worst, 8) + ")");
    check (calls == 4, "a 75 s song separates as four 20 s chunks");
    check (! reported.empty() && std::abs (reported.back() - 1.0) < 1.0e-9, "progress ends at 100%");

    // Cancelling stops the work and returns nothing.
    calls = 0;
    std::atomic<int> asked { 0 };
    const auto cancelled = stems::separate (mix, fake, [&] (double) { return ++asked < 2; }, settings);
    check (cancelled.empty() && calls < 4, "cancelling stops the split");

    // split(): a mono 48 kHz file becomes four stereo 44.1 kHz WAVs named after the song.
    const auto folder = juce::File::getSpecialLocation (juce::File::tempDirectory).getNonexistentChildFile ("micfx-stems", {});
    folder.createDirectory();
    const auto song = folder.getChildFile ("Song.wav");
    juce::AudioBuffer<float> mono (1, (int) (3.0 * sampleRate));
    for (int i = 0; i < mono.getNumSamples(); ++i)
        mono.setSample (0, i, 0.5f * (float) std::sin (juce::MathConstants<double>::twoPi * 440.0 * i / sampleRate));
    writeWav (song, mono, sampleRate);

    const auto out = folder.getChildFile ("Stems");
    const auto result = stems::split (song, out, "Song", fake, [] (double) { return true; }, settings);
    auto filesOk = result.error.isEmpty() && ! result.cancelled;
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    for (int s = 0; s < stems::numStems && filesOk; ++s)
    {
        const auto& file = result.files[(size_t) s];
        const std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
        filesOk = reader != nullptr && file.getFileName() == "Song - " + juce::String (stems::stemNames[(size_t) s]) + ".wav"
               && reader->numChannels == 2 && juce::approximatelyEqual (reader->sampleRate, stems::modelSampleRate)
               && std::abs ((double) reader->lengthInSamples / reader->sampleRate - 3.0) < 0.001;
    }
    check (filesOk, "split writes Song - Drums/Bass/Other/Vocals.wav, stereo at 44.1 kHz, as long as the song");

    const auto unreadable = stems::split (folder.getChildFile ("missing.wav"), out, "Missing", fake, nullptr, settings);
    check (unreadable.error.contains ("missing.wav"), "split reports a file it can't read");

    // The model check rejects anything that isn't the expected download.
    const auto bogus = folder.getChildFile (stems::Model::fileName);
    bogus.replaceWithText ("not a model");
    check (stems::Model::verify (bogus).isNotEmpty() && stems::Model::verify (folder.getChildFile ("nothing.bin")).isNotEmpty(),
           "a wrong or missing model file is refused");
    juce::String loadError;
    check (stems::Model::load (bogus, loadError) == nullptr && loadError.isNotEmpty(), "loading a wrong model fails with a message");

    // The GPU model arrives as a zip of the .mlpackage folder: checked, then unpacked in place.
    {
        const auto packageName = juce::String (stems::GpuModel::fileName);
        const auto source = folder.getChildFile ("gpu-source");
        source.getChildFile (packageName).getChildFile ("Data").createDirectory();
        source.getChildFile (packageName).getChildFile ("Manifest.json").replaceWithText ("{}");
        source.getChildFile (packageName).getChildFile ("Data").getChildFile ("weight.bin").replaceWithText ("weights");

        juce::ZipFile::Builder builder;
        builder.addFile (source.getChildFile (packageName).getChildFile ("Manifest.json"), 9, packageName + "/Manifest.json");
        builder.addFile (source.getChildFile (packageName).getChildFile ("Data").getChildFile ("weight.bin"), 9, packageName + "/Data/weight.bin");
        const auto zip = folder.getChildFile ("gpu.zip");
        {
            juce::FileOutputStream zipOut (zip);
            zipOut.setPosition (0);
            zipOut.truncate();
            builder.writeToStream (zipOut, nullptr);
        }

        const auto installed = folder.getChildFile ("gpu-models").getChildFile (packageName);
        folder.getChildFile ("gpu-models").createDirectory();
        check (stems::GpuModel::install (zip, installed, juce::SHA256 (zip).toHexString()).isEmpty()
                   && installed.getChildFile ("Manifest.json").loadFileAsString() == "{}"
                   && installed.getChildFile ("Data").getChildFile ("weight.bin").loadFileAsString() == "weights",
               "the GPU model zip unpacks into the .mlpackage folder");

        installed.deleteRecursively();
        check (stems::GpuModel::install (zip, installed, "0000").isNotEmpty() && ! installed.exists(),
               "a GPU model zip with the wrong checksum is refused and leaves nothing");
    }

    // DawBridge: the stems land on four tracks under the song, lined up with its clip, and
    // the song's clip is muted. One undo takes it all back.
    {
        juce::PropertiesFile::Options options;
        options.applicationName = "MicFXTests";
        options.filenameSuffix = ".settings";
        juce::PropertiesFile props (folder.getChildFile ("test.settings"), options);

        PluginProcessor processor;
        processor.prepareToPlay (sampleRate, blockSize);
        auto& edit = processor.getDawEdit();
        DawBridge::availableForTesting = true;
        DawBridge bridge (processor, props, [] (const juce::Identifier&, const juce::var&) {}, [] { return (juce::int64) 0; });
        bridge.handle ("mode", juce::JSON::parse ("{\"daw\": true}"), [] (juce::var) {});

        const auto first = (int) edit.getTracks()[0][daw::ids::id];
        const auto last = (int) edit.addTrack ("Keys")[daw::ids::id];
        const auto clip = edit.addClip (first, song, 2.0, 1.5, 0.5);
        edit.setClipGain ((int) clip[daw::ids::id], -3.0f);
        edit.getUndoManager().beginNewTransaction();

        const auto added = bridge.addStemTracks (clip.createCopy(), first, result.files);
        const auto tracks = edit.getTracks();
        auto lined = (bool) added["ok"] && tracks.size() == 6 && (int) tracks[5][daw::ids::id] == last;
        const char* order[] { "Vocals", "Drums", "Bass", "Other" };
        for (int i = 0; i < 4 && lined; ++i)
        {
            const auto stemClip = tracks[i + 1].getChildWithName (daw::ids::CLIP);
            lined = tracks[i + 1][daw::ids::name].toString() == order[i] && stemClip.isValid()
                 && (double) stemClip[daw::ids::start] == 2.0 && (double) stemClip[daw::ids::offset] == 0.5
                 && (double) stemClip[daw::ids::length] == 1.5 && (float) stemClip[daw::ids::gain] == -3.0f
                 && stemClip[daw::ids::file].toString().contains (order[i]);
        }
        check (lined, "the stems go on Vocals, Drums, Bass and Other tracks under the song, lined up with its clip");
        check ((bool) edit.getClip ((int) clip[daw::ids::id])[daw::ids::mute], "the song's clip is muted");

        edit.getUndoManager().undo();
        check (edit.getTracks().size() == 2 && ! (bool) edit.getClip ((int) clip[daw::ids::id]).getProperty (daw::ids::mute, false),
               "one undo removes the stem tracks and unmutes the song");

        juce::var started;
        bridge.handle ("stems", juce::JSON::parse ("{\"action\": \"start\", \"id\": -5}"), [&] (juce::var r) { started = r; });
        check (! (bool) started["ok"] && started["error"].toString().isNotEmpty(), "Split into stems without a clip says why");
        DawBridge::availableForTesting = false;
    }

    folder.deleteRecursively();
}

#if MICFX_HAS_DEMUCS
// The code round the network (spectrogram, normalising, segments, mask, inverse spectrogram,
// crossfade), with a stand-in network that hands source 0 its own spectrogram back. If that
// is all right, source 0 comes out as the input.
static void testStemPipeline()
{
    std::cout << "\nStem splitter: the code round the network\n";

    constexpr int F = stems::CoreNetwork::freqBins, T = stems::CoreNetwork::frames;
    std::atomic<int> runs { 0 };

    auto core = std::make_shared<stems::CoreNetwork>();
    core->run = [&runs] (const float* x, const float*, float* xOut, float* xtOut)
    {
        ++runs;
        std::fill (xOut, xOut + (size_t) stems::numStems * 4 * F * T, 0.0f);
        std::copy_n (x, (size_t) 4 * F * T, xOut); // source 0, all four channels
        std::fill (xtOut, xtOut + (size_t) stems::numStems * 2 * stems::CoreNetwork::segmentSamples, 0.0f);
        return juce::String();
    };

    const auto separator = stems::makeSeparator (core);

    // 21.3 s: three full segments' worth and a short last one. Different tones left and right.
    const auto length = (int) (21.3 * stems::modelSampleRate);
    juce::AudioBuffer<float> input (2, length);
    for (int i = 0; i < length; ++i)
    {
        const auto t = i / stems::modelSampleRate;
        input.setSample (0, i, 0.3f * (float) (std::sin (juce::MathConstants<double>::twoPi * 330.0 * t) + 0.5 * std::sin (juce::MathConstants<double>::twoPi * 1250.0 * t)));
        input.setSample (1, i, 0.25f * (float) std::sin (juce::MathConstants<double>::twoPi * 550.0 * t));
    }

    const auto out = separator (input, [] (double) { return true; });
    auto worst = 0.0f;
    const auto edge = 4096; // the spectrogram's own edges are lossy, as in Demucs itself
    if (out.size() == (size_t) stems::numStems && out[0].getNumSamples() == length)
        for (int ch = 0; ch < 2; ++ch)
            for (int i = edge; i < length - edge; ++i)
                worst = std::max (worst, std::abs (out[0].getSample (ch, i) - input.getSample (ch, i)));

    check (out.size() == (size_t) stems::numStems && worst < 2.0e-3f,
           "a network that returns its input gives the song back, left and right in place (worst " + juce::String (worst, 5) + ")");
    check (runs == 4, "21.3 s runs the network on four overlapping 7.8 s segments (" + juce::String (runs.load()) + ")");

    // Cancelling between segments stops the work.
    runs = 0;
    std::atomic<int> asked { 0 };
    const auto cancelled = separator (input, [&] (double) { return ++asked < 3; });
    check (cancelled.empty() && runs < 4, "cancelling stops it between segments");

    // A network that fails stops the split rather than returning silence.
    auto broken = std::make_shared<stems::CoreNetwork>();
    broken->run = [] (const float*, const float*, float*, float*) { return juce::String ("the GPU said no"); };
    check (stems::makeSeparator (broken) (input, nullptr).empty(), "a failing network gives no stems");

    // A silent song doesn't divide by zero.
    juce::AudioBuffer<float> silence (2, 100000);
    silence.clear();
    const auto quiet = separator (silence, nullptr);
    auto finite = quiet.size() == (size_t) stems::numStems;
    for (const auto& stem : quiet)
        for (int i = 0; i < stem.getNumSamples() && finite; ++i)
            finite = std::isfinite (stem.getSample (0, i)) && std::isfinite (stem.getSample (1, i));
    check (finite, "a silent song gives finite stems");

    check (! stems::GpuModel::isBuiltIn() || stems::GpuModel::getDefaultFile().getFileName() == stems::GpuModel::fileName,
           "the GPU model lives in the Models folder, next to the CPU one");
    juce::String gpuError;
    check (stems::GpuModel::load (juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("no-such-model.mlpackage"), gpuError) == nullptr
               && gpuError.isNotEmpty(),
           "a missing GPU model fails with a message, so the CPU is used");
}
#endif

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
    testMidiTracks();
    testPluginHosting();
    testProjects();
    testExport();
    testClipEditing();
    testDawMode();
    testClipTools();
    testWarp();
    testRhythm();
    testTrackTools();
    testDawBridge();
    testLiveControls();
    testAnalysis();
    testMixAssistant();
    testKeyDetection();
    testDawKey();
    testAutoMixEffects();
    testAutoMixRoles();
    testAutoMixRides();
    testAutoMixModes();
    testAutoMixSynths();
    testAnalysisSpeed();
    testLatency();
    testSmoothing();
    testStems();
#if MICFX_HAS_DEMUCS
    testStemPipeline();
#endif
    testLiveMeters();

    std::cout << "\n" << (failures == 0 ? "All tests passed" : juce::String (failures) + " test(s) failed") << "\n";
    return failures == 0 ? 0 : 1;
}
