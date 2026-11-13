#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_dsp/juce_dsp.h>

#include "Biquad.h"

#include <memory>
#include <vector>

namespace fx
{
//==============================================================================
// Impulse response files (a cabinet, a room): checked on the message thread before the
// convolver gets them, so the background loader only ever sees files it can read.
struct ImpulseFile
{
    static juce::String getWildcard() { return "*.wav;*.aif;*.aiff;*.flac"; }

    // Longest file accepted. Longer files are refused rather than cut, since a cut room
    // tail sounds wrong (a cabinet only uses its first part anyway, see Convolver).
    static constexpr double maxSeconds = 20.0;

    // Empty when the file can be used, otherwise why it can't, in plain words.
    static juce::String check (const juce::File& file)
    {
        if (! file.existsAsFile())
            return file.getFileName() + " wasn't found.";

        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        const std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));

        if (reader == nullptr)
            return file.getFileName() + " isn't an audio file MicFX can read (use WAV, AIFF or FLAC).";

        if (reader->lengthInSamples < 8 || reader->sampleRate <= 0.0)
            return file.getFileName() + " is empty.";

        if ((double) reader->lengthInSamples / reader->sampleRate > maxSeconds)
            return file.getFileName() + " is longer than " + juce::String ((int) maxSeconds)
                   + " seconds: that's a recording, not an impulse response.";

        return {};
    }
};

//==============================================================================
// Convolution with a choice of impulse responses: built-in ones (audio files kept in
// memory for the life of the program) or a file the user picked. Zero latency (JUCE's
// non-uniform partitioned convolution), with a wet/dry mix.
//
// Threading: JUCE's Convolution must be told about a new IR on the audio thread. The
// message thread only leaves the file in a slot (setFile); the audio thread picks it up
// on the next block with a try-lock, and JUCE reads and resamples the file on its own
// background thread, then crossfades to it. An IR chosen before prepare() is loaded
// there and then, so an offline render starts with the right sound.
class Convolver
{
public:
    // A built-in IR: an audio file's bytes, which must outlive every Convolver.
    struct BuiltIn
    {
        const void* data = nullptr;
        size_t size = 0;
    };

    static constexpr int fileSource = -1;

    // headSize: the first partition (smaller is cheaper on short IRs, larger on long
    // ones); maxSeconds: only the first part of a longer file is used; trim: drop the
    // silence at the start of a file (right for a cabinet, wrong for a pre-delayed room).
    Convolver (int headSize, double maxSecondsToUse, bool trimFiles)
        : head (headSize), maxSeconds (maxSecondsToUse), trim (trimFiles) {}

    void setBuiltIns (std::vector<BuiltIn> irs) { builtIns = std::move (irs); }

    // Message thread.
    void setFile (const juce::File& file)
    {
        const juce::SpinLock::ScopedLockType lock (fileLock);
        pendingFile = file;
        ++fileVersion;
    }

    juce::File getFile() const
    {
        const juce::SpinLock::ScopedLockType lock (fileLock);
        return pendingFile;
    }

    // Audio thread (or before prepare): which IR, a built-in index or fileSource.
    void setSource (int source) noexcept { wantedSource = source; }

    // Whether there is anything to convolve with: without a file, the file source passes
    // the sound through untouched.
    bool hasImpulse() const noexcept { return loadedSource != noSource; }

    void prepare (const juce::dsp::ProcessSpec& spec)
    {
        if (convolution == nullptr)
            convolution = std::make_unique<juce::dsp::Convolution> (juce::dsp::Convolution::NonUniform { head });

        sampleRate = spec.sampleRate;
        preDelay.prepare (spec);
        preDelay.setMaximumDelayInSamples ((int) std::ceil (maxPreDelaySeconds * spec.sampleRate));
        designCuts();
        dry.setSize ((int) spec.numChannels, (int) spec.maximumBlockSize, false, false, true);
        loadedSource = noSource;
        loadedFileVersion = -1;
        pickUp (true); // before prepare: loaded at once
        convolution->prepare (spec);
        wetGain.reset (spec.sampleRate, 0.03);
        dryGain.reset (spec.sampleRate, 0.03);
        wetGain.setCurrentAndTargetValue (wetGain.getTargetValue());
        dryGain.setCurrentAndTargetValue (dryGain.getTargetValue());
    }

    void reset()
    {
        if (convolution != nullptr)
            convolution->reset();
        preDelay.reset();
        lowCutFilter.reset();
        highCutFilter.reset();
    }

    // Linear gains for the convolved and the untouched sound. JUCE scales every IR to
    // the same energy (-18 dB for a flat one), so the wet gain here is made up by that.
    void setMix (float wet, float dryLevel) noexcept
    {
        wetGain.setTargetValue (wet * normalisedMakeUp);
        dryGain.setTargetValue (dryLevel);
    }

    // Shapes only the convolved sound: a delay before it (a room's pre-delay) and a
    // low and high cut after it. 20 Hz / 20 kHz leave the ends alone. Audio thread.
    void setWetShape (float preDelayMs, float lowCutHz, float highCutHz) noexcept
    {
        preDelaySamples = juce::jlimit (0.0f, (float) maxPreDelaySeconds * (float) sampleRate - 1.0f, preDelayMs / 1000.0f * (float) sampleRate);

        if (! juce::approximatelyEqual (lowCutHz, lowCut) || ! juce::approximatelyEqual (highCutHz, highCut))
        {
            lowCut = lowCutHz;
            highCut = highCutHz;
            designCuts();
        }
    }

    void process (const juce::dsp::ProcessContextReplacing<float>& context) noexcept
    {
        if (convolution == nullptr)
            return;

        pickUp (false);

        auto& block = context.getOutputBlock();
        const auto numSamples = (int) block.getNumSamples();
        const auto numChannels = juce::jmin ((int) block.getNumChannels(), dry.getNumChannels());

        if (! hasImpulse() || numSamples > dry.getNumSamples())
            return;

        for (int ch = 0; ch < numChannels; ++ch)
            dry.copyFrom (ch, 0, block.getChannelPointer ((size_t) ch), numSamples);

        if (preDelaySamples >= 1.0f || preDelayWasOn)
        {
            preDelayWasOn = preDelaySamples >= 1.0f;
            preDelay.setDelay (preDelaySamples);
            for (int ch = 0; ch < numChannels; ++ch)
            {
                auto* data = block.getChannelPointer ((size_t) ch);
                for (int i = 0; i < numSamples; ++i)
                {
                    preDelay.pushSample (ch, data[i]);
                    data[i] = preDelay.popSample (ch);
                }
            }
        }

        convolution->process (context);

        for (int ch = 0; ch < numChannels; ++ch)
        {
            auto* out = block.getChannelPointer ((size_t) ch);
            if (useLowCut)
                for (int i = 0; i < numSamples; ++i)
                    out[i] = lowCutFilter.process ((size_t) ch, out[i]);
            if (useHighCut)
                for (int i = 0; i < numSamples; ++i)
                    out[i] = highCutFilter.process ((size_t) ch, out[i]);
        }

        for (int i = 0; i < numSamples; ++i)
        {
            const auto w = wetGain.getNextValue(), d = dryGain.getNextValue();
            for (int ch = 0; ch < numChannels; ++ch)
            {
                auto* out = block.getChannelPointer ((size_t) ch);
                out[i] = out[i] * w + dry.getSample (ch, i) * d;
            }
        }
    }

    int getCurrentIrSize() const { return convolution != nullptr ? convolution->getCurrentIRSize() : 0; }

    static constexpr double maxPreDelaySeconds = 0.25;

private:
    static constexpr int noSource = -2;
    static constexpr float normalisedMakeUp = 8.0f; // undoes JUCE's 0.125 / sqrt (energy)

    void designCuts() noexcept
    {
        useLowCut = lowCut > 21.0f;
        useHighCut = highCut < 19900.0f && highCut < sampleRate * 0.45;
        lowCutFilter.setup (Biquad::Type::highPass, sampleRate, lowCut, 0.707);
        highCutFilter.setup (Biquad::Type::lowPass, sampleRate, highCut, 0.707);
    }

    // Hands a newly chosen IR to JUCE. Wait-free: the loads only queue a command.
    void pickUp (bool force) noexcept
    {
        const auto source = wantedSource;

        if (source == fileSource)
        {
            const juce::SpinLock::ScopedTryLockType lock (fileLock);
            if (! lock.isLocked() || (! force && loadedSource == fileSource && loadedFileVersion == fileVersion))
                return;

            loadedFileVersion = fileVersion;

            if (pendingFile == juce::File())
            {
                loadedSource = noSource;
                return;
            }

            // Only the first maxSeconds (at the file's rate; 192 kHz covers any).
            const auto maxLength = (size_t) std::ceil (maxSeconds * 192000.0);
            convolution->loadImpulseResponse (pendingFile, juce::dsp::Convolution::Stereo::yes,
                                              trim ? juce::dsp::Convolution::Trim::yes : juce::dsp::Convolution::Trim::no,
                                              maxLength, juce::dsp::Convolution::Normalise::yes);
            loadedSource = fileSource;
            return;
        }

        if (! juce::isPositiveAndBelow (source, (int) builtIns.size()))
        {
            loadedSource = noSource;
            return;
        }

        if (! force && source == loadedSource)
            return;

        const auto& ir = builtIns[(size_t) source];
        convolution->loadImpulseResponse (ir.data, ir.size, juce::dsp::Convolution::Stereo::yes,
                                          juce::dsp::Convolution::Trim::no, 0, juce::dsp::Convolution::Normalise::yes);
        loadedSource = source;
    }

    const int head;
    const double maxSeconds;
    const bool trim;

    std::unique_ptr<juce::dsp::Convolution> convolution;
    std::vector<BuiltIn> builtIns;
    juce::AudioBuffer<float> dry;
    juce::SmoothedValue<float> wetGain { 1.0f }, dryGain { 0.0f };
    double sampleRate = 48000.0;

    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::None> preDelay { 4 };
    float preDelaySamples = 0.0f;
    bool preDelayWasOn = false;
    Biquad lowCutFilter, highCutFilter;
    float lowCut = 20.0f, highCut = 20000.0f;
    bool useLowCut = false, useHighCut = false;

    mutable juce::SpinLock fileLock;
    juce::File pendingFile; // under fileLock
    int fileVersion = 0;    // under fileLock

    int wantedSource = 0;
    int loadedSource = noSource;
    int loadedFileVersion = -1;
};

//==============================================================================
// Built-in rooms for the IR reverb, made here (no recorded spaces are shipped): early
// reflections, then a dense decorrelated tail per side whose lows, mids and highs die
// away at their own rates, like real rooms (highs first). Made once, at 48 kHz; JUCE
// resamples them to the session rate when it loads them.
namespace spaces
{
struct Space
{
    const char* name;
    float decaySeconds;   // mid-band RT60
    float lowDecay;       // RT60 of the lows, relative to the mids
    float highDecay;      // and of the highs
    float earlyMs;        // span of the early reflections (0: none, as a plate)
    float preDelayMs;     // before the first reflection
    float density;        // tail onset (ms): how fast the tail builds up
};

inline const std::array<Space, 5>& getSpaces()
{
    static const std::array<Space, 5> list { {
        { "Small room", 0.45f, 1.1f, 0.55f, 18.0f, 1.0f, 6.0f },
        { "Studio",     0.8f,  1.2f, 0.6f,  28.0f, 2.0f, 10.0f },
        { "Plate",      1.7f,  0.8f, 0.85f, 0.0f,  0.0f, 2.0f },
        { "Hall",       2.3f,  1.3f, 0.5f,  60.0f, 8.0f, 25.0f },
        { "Church",     4.0f,  1.4f, 0.45f, 90.0f, 15.0f, 40.0f },
    } };
    return list;
}

inline juce::AudioBuffer<float> makeImpulse (const Space& space, double rate, juce::uint32 seed)
{
    const auto length = (int) std::ceil ((space.decaySeconds * juce::jmax (1.0f, space.lowDecay) * 1.1f + space.preDelayMs / 1000.0f + 0.1f) * rate);
    juce::AudioBuffer<float> ir (2, length);
    ir.clear();

    juce::Random random ((juce::int64) seed);
    const auto preDelay = (int) (space.preDelayMs / 1000.0f * (float) rate);

    // A soft tick at the start so the direct path of the reverb isn't empty.
    for (int ch = 0; ch < 2; ++ch)
        ir.setSample (ch, preDelay, 0.25f);

    // Early reflections: sparse taps, falling in level, different per side.
    if (space.earlyMs > 0.0f)
    {
        for (int ch = 0; ch < 2; ++ch)
            for (int tap = 0; tap < 14; ++tap)
            {
                const auto at = 0.08f + 0.92f * std::pow (random.nextFloat(), 0.7f);
                const auto index = preDelay + (int) (at * space.earlyMs / 1000.0f * (float) rate);
                const auto level = (0.5f - 0.3f * at) * (random.nextBool() ? 1.0f : -1.0f);
                if (index < length)
                    ir.addSample (ch, index, level);
            }
    }

    // The tail: three bands of noise, each with its own decay. The bands come from
    // complementary low-pass / high-pass pairs (a 300 Hz and a 4 kHz split).
    const auto onset = juce::jmax (1.0f, space.density / 1000.0f * (float) rate);
    const auto tailStart = preDelay + (int) (space.earlyMs * 0.25f / 1000.0f * (float) rate);
    const auto rt = [&] (float scale) { return juce::jmax (0.05f, space.decaySeconds * scale); };
    const std::array<float, 3> decays { rt (space.lowDecay), rt (1.0f), rt (space.highDecay) };

    for (int ch = 0; ch < 2; ++ch)
    {
        Biquad lowSplit, highSplit, lowRest, highRest;
        lowSplit.setup (Biquad::Type::lowPass, rate, 300.0, 0.707);
        lowRest.setup (Biquad::Type::highPass, rate, 300.0, 0.707);
        highSplit.setup (Biquad::Type::highPass, rate, 4000.0, 0.707);
        highRest.setup (Biquad::Type::lowPass, rate, 4000.0, 0.707);

        auto* out = ir.getWritePointer (ch);
        for (int i = tailStart; i < length; ++i)
        {
            const auto t = (float) (i - tailStart) / (float) rate;
            const auto noise = random.nextFloat() * 2.0f - 1.0f;
            const auto low = lowSplit.process (0, noise);
            const auto high = highSplit.process (0, noise);
            const auto mid = highRest.process (0, lowRest.process (0, noise));
            const auto build = 1.0f - std::exp (-(float) (i - tailStart) / onset);

            // RT60: -60 dB after `decay` seconds.
            const auto env = [&] (float decay) { return std::exp (-6.9078f * t / decay); };
            out[i] += 0.35f * build * (low * env (decays[0]) + mid * env (decays[1]) + high * env (decays[2]));
        }
    }

    return ir;
}

// The rooms as WAV files in memory, made the first time they're asked for.
inline const std::vector<juce::MemoryBlock>& getSpaceFiles()
{
    static const std::vector<juce::MemoryBlock> files = []
    {
        constexpr double rate = 48000.0;
        std::vector<juce::MemoryBlock> made;

        for (size_t i = 0; i < getSpaces().size(); ++i)
        {
            const auto ir = makeImpulse (getSpaces()[i], rate, 0x5eed + (juce::uint32) i * 977u);
            juce::MemoryBlock bytes;
            std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::MemoryOutputStream> (bytes, false);

            juce::WavAudioFormat wav;
            if (auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions {}.withSampleRate (rate)
                                                                                             .withNumChannels (2)
                                                                                             .withBitsPerSample (24)))
                writer->writeFromAudioSampleBuffer (ir, 0, ir.getNumSamples());

            made.push_back (std::move (bytes));
        }

        return made;
    }();

    return files;
}

inline std::vector<Convolver::BuiltIn> getSpaceImpulses()
{
    std::vector<Convolver::BuiltIn> list;
    for (const auto& file : getSpaceFiles())
        list.push_back ({ file.getData(), file.getSize() });
    return list;
}
} // namespace spaces
} // namespace fx
