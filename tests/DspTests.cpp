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

#include "PluginProcessor.h"
#include "Presets.h"
#include "daw/DawEngine.h"
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
}

int main()
{
    juce::ScopedJuceInitialiser_GUI juce;

    testPitchDetector();
    testPitchShift();
    testAutotune();
    testPresets();
    testFxChain();
    testRecorder();
    testMultitrack();
    testDawEngine();

    std::cout << "\n" << (failures == 0 ? "All tests passed" : juce::String (failures) + " test(s) failed") << "\n";
    return failures == 0 ? 0 : 1;
}
