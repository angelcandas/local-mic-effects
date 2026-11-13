#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "analysis/MixAssistant.h"
#include "dsp/FxChain.h"

#include <atomic>
#include <functional>
#include <future>
#include <vector>

class Scenes;

// Sound check (Live mode): sing for 10-20 seconds and the Live chain is set for this
// voice, mic and room.
//
// The raw mic (after the input gain, before any effect: where the Practice view's PitchTap
// listens) is captured into a buffer allocated once. Then it is measured
// (analysis::analyse) and Auto Mix's "clean" plan is made for it in a style you pick; the
// plan's moves become Live parameter values (the module keys match: the low cut goes to
// the high-pass, then gate, parametric EQ, de-esser, compressor). On top of
// that: the denoiser when the room is noisy (its noise profile learnt from the quietest
// half second), a reverb by the style, and advice on the interface's input gain (peaks
// against -12 dBFS), which can't be set from here.
//
// The values are applied at once, each as one parameter gesture, and the values from
// before are kept: Before / After switches between them, Revert goes back, and the result
// can be saved as a scene.
//
// capture() runs on the audio thread and only writes into the buffer; everything else is
// message thread. The analysis runs on its own thread.
class SoundCheck
{
public:
    static constexpr double maxSeconds = 20.0, defaultSeconds = 15.0, minSeconds = 6.0;
    static constexpr double minVoiceSeconds = 4.0; // less voice than this is refused

    //==============================================================================
    // The plan, from a capture. Plain computation (the tests call it).
    struct Change
    {
        juce::String module; // the effect's name ("Gate"), or "Input" for advice
        juce::String text;
    };

    struct Plan
    {
        std::vector<std::pair<juce::String, float>> values; // parameter ID -> value (real units), in chain order
        std::vector<Change> changes;
        juce::StringArray notes;     // what it measured or left alone
        juce::String inputAdvice;    // about the interface's gain
        float peakDb = -200.0f;      // sample peak of the capture, dBFS
        float voiceLufs = -200.0f;   // loudness while singing
        float noiseDb = -200.0f;     // the room between phrases (dBFS RMS), -200 when there was no pause
        double voiceSeconds = 0.0;
        std::vector<float> noiseProfile; // for the denoiser (power per bin), empty when it isn't used
        juce::String error;          // set when nothing can be planned
    };

    static Plan makePlan (const float* mono, int numSamples, double sampleRate, analysis::MixStyle);

    //==============================================================================
    SoundCheck (juce::AudioProcessorValueTreeState&, fx::FxChain& liveChain, Scenes&);
    ~SoundCheck();

    // Audio stopped (the processor's prepareToPlay). Stops a capture that was running.
    void prepare (double sampleRate);

    // Audio thread: the mic after the input gain.
    void capture (const float* mono, int numSamples) noexcept;

    // The page's "soundCheck" function: (action, options) -> describe(). Actions:
    //   "state"
    //   "start" { seconds?, style? }  listen (style: an index into analysis::allStyles)
    //   "finish"                      stop listening early and set the chain
    //   "cancel"                      stop listening, change nothing
    //   "compare" { after }           hear the sound from before (false) or after (true)
    //   "revert"                      back to the sound from before, and close
    //   "keep"                        keep the new sound, and close
    //   "saveScene" { name? }         keep it and save it as a new scene
    juce::var handle (const juce::String& action, const juce::var& options);

    // About 30 times a second (the editor's timer). Calls onChange when there's news.
    void tick();
    std::function<void (const juce::var&)> onChange;

    // { phase: idle | listening | analysing | done, seconds, target, voiceSeconds, level,
    //   canFinish, style, styles, error, changes [{ module, text }], notes, advice, peak,
    //   comparing ("after" | "before"), scene }
    juce::var describe() const;

    enum class Phase { idle, listening, analysing, done };
    Phase getPhase() const noexcept { return phase; }

    // For the tests: wait for the analysis (when one runs) and handle its result.
    void waitForAnalysis();

private:
    void startAnalysis();
    void finishAnalysis (Plan);
    void setValues (const std::vector<std::pair<juce::String, float>>&);
    void fail (const juce::String& message);
    void scanLevels(); // the live level and voice seconds while listening

    juce::AudioProcessorValueTreeState& state;
    fx::FxChain& chain;
    Scenes& scenes;

    double sampleRate = 48000.0;
    juce::AudioBuffer<float> buffer; // maxSeconds, allocated at the first start
    int targetSamples = 0;

    // Audio thread <-> message thread.
    static constexpr int off = 0, capturing = 1, captured = 2;
    std::atomic<int> captureState { off };
    std::atomic<int> written { 0 };
    std::atomic<bool> finishRequested { false };

    Phase phase = Phase::idle;
    analysis::MixStyle style = analysis::MixStyle::pop;
    std::future<Plan> pending;

    // While listening.
    int scanned = 0, lastWritten = 0;
    double voiceSeconds = 0.0, levelDb = -100.0, stalledSeconds = 0.0;
    juce::uint32 lastTick = 0;
    int ticks = 0;

    // The result.
    Plan plan;
    std::vector<std::pair<juce::String, float>> before;
    bool comparingAfter = true;
    juce::String error, sceneName;
};
