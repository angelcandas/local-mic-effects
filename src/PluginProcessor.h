#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>

#include "Parameters.h"
#include "CaptureRing.h"
#include "Looper.h"
#include "MidiMap.h"
#include "PitchTap.h"
#include "LiveHistory.h"
#include "Scenes.h"
#include "Setlist.h"
#include "SoundCheck.h"
#include "Multitrack.h"
#include "Recorder.h"
#include "daw/DawEngine.h"
#include "daw/PluginHost.h"
#include "daw/Project.h"
#include "dsp/FxChain.h"

// Signal chain, in order (every module except gain/reverb/limiter is skipped when off):
//   input gain -> [effect chain] -> output gain -> safety limiter
// where the effect chain (fx::FxChain, every module type) runs:
//   high-pass -> gate -> autotune / pitch shift / harmonizer -> EQ -> compressor -> auto level
//   -> de-esser -> exciter -> saturation -> radio -> bitcrusher -> robot -> filter
//   -> auto-wah -> tremolo -> vibrato -> chorus -> flanger -> phaser -> doubler
//   -> echo -> reverb -> stereo width
// The looper records after the output gain and its playback joins after the limiter,
// ahead of the recorder and the backing track.
// The plugin parameters drive the chain's module values once per block.
class PluginProcessor final : public juce::AudioProcessor,
                              private juce::Timer
{
public:
    PluginProcessor();

    //==============================================================================
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;

    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    //==============================================================================
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override                       { return true; }

    const juce::String getName() const override           { return JucePlugin_Name; }
    bool acceptsMidi() const override                     { return true; } // MIDI learn (Live mode), MIDI tracks (DAW)
    bool producesMidi() const override                    { return false; }
    bool isMidiEffect() const override                    { return false; }
    double getTailLengthSeconds() const override          { return 5.0; }

    // Programs are the factory voice presets (see Presets.cpp).
    int getNumPrograms() override;
    int getCurrentProgram() override                      { return currentPreset; }
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    //==============================================================================
    juce::AudioProcessorValueTreeState& getState() noexcept { return state; }
    const fx::PitchEngine& getPitchEngine() const noexcept  { return pitchModule.getEngine(); }

    // The chord the harmonizer follows now (Live: the backing track or the instrument
    // input; DAW: the other tracks). Invalid when nothing is followed. Any thread.
    analysis::Chord getHarmonyChord() const noexcept;

    // The Live chain, for the effect cards' live readings (module meters are atomics).
    fx::FxChain& getLiveChain() noexcept                    { return chain; }

    // The order the Live chain's effects play in, as module keys, first to last. Set takes
    // any list: unknown keys are skipped, missing ones keep their standard places after
    // the listed ones (an empty list is the standard order). Autotune, pitch shift and
    // harmonizer are one module ("pitch"), so they move together. Kept in the plugin state
    // and in each setlist song; loading a factory preset sets its order. Message thread.
    juce::StringArray getChainOrder() const;
    void setChainOrder (const juce::StringArray& keys);

    // The tempo Live's synced echo follows: the echo card's tap (0 clears it), else the
    // host's, else the looper's. Any thread.
    void setEchoTapTempo (double bpm) noexcept               { echoTapTempo.store (bpm); }
    double getLiveTempo() noexcept;

    // Files the Live chain's effects play through (the cabinet's and the IR reverb's
    // impulse responses), by module key. Kept in the plugin state; the effect's choice
    // switches to "your file" when one loads. Returns why a file can't be used (empty
    // when it loaded); an empty File clears it. Message thread.
    juce::String setModuleFile (fx::ModuleType, const juce::File&);
    juce::File getModuleFile (fx::ModuleType) const;

    // Records the processed output (and optionally the dry mic) to WAV. Start/stop it
    // from the message thread; processBlock feeds it.
    Recorder& getRecorder() noexcept                        { return recorder; }

    // The Live mode looper: records the voice after the effects (before the safety
    // limiter) and plays its layers under it. Its playback is in recorded takes.
    Looper& getLooper() noexcept                            { return looper; }

    // Capture (Live mode): the last minutes of the processed voice (left, right) and the
    // raw mic, always kept, so a take nobody pressed Record for can still be saved.
    // setCaptureSeconds (message thread): how much to keep, 0 = off (and no memory used).
    // Channels of the ring: 0 and 1 the processed output, 2 the dry mic.
    static constexpr double defaultCaptureSeconds = 180.0;
    CaptureRing& getCapture() noexcept                      { return liveCapture; }
    void setCaptureSeconds (double seconds);
    double getCaptureSeconds() const noexcept               { return captureSeconds.load(); }

    // Scenes: snapshots of the Live sound, recalled with a glide. Message thread.
    Scenes& getScenes() noexcept                            { return scenes; }

    // Undo for Live mode: every change to the Live sound, one named step each. Message thread.
    LiveHistory& getLiveHistory() noexcept                  { return liveHistory; }

    // The setlist (standalone app): songs with their sound, scenes, tempo, key, backing
    // track and lyrics. Message thread.
    Setlist& getSetlist() noexcept                          { return setlist; }

    // MIDI learn: pedals, knobs and keys on parameters and actions (Live mode).
    MidiMap& getMidiMap() noexcept                          { return midiMap; }

    // The singer's pitch before the effects, for the Practice view (off until it opens).
    PitchTap& getPitchTap() noexcept                        { return pitchTap; }

    // Sound check: listens to the raw mic and sets the Live chain for it. Message thread.
    SoundCheck& getSoundCheck() noexcept                    { return soundCheck; }

    // Backing track + recorded takes, mixed in after the voice chain (so the music
    // isn't processed or recorded with the voice). Message thread only.
    Multitrack& getMultitrack() noexcept                    { return multitrack; }

    // DAW mode (standalone app): the mic feeds the DAW engine's tracks instead of the
    // Live chain. The session, its engine and its project file. Message thread.
    daw::Edit& getDawEdit() noexcept                         { return dawEdit; }
    daw::DawEngine& getDawEngine() noexcept                  { return dawEngine; }
    daw::Project& getDawProject() noexcept                   { return dawProject; }
    // Instrument plugins for MIDI tracks (one host shared by every MicFX in the process).
    daw::PluginHost& getPluginHost() noexcept                { return *pluginHost; }
    void setDawMode (bool on) noexcept                       { dawMode = on; updateLatency(); }
    bool isDawMode() const noexcept                          { return dawMode.load(); }

    // Peak levels (linear gain) since the UI last read them. Written by the audio
    // thread, drained with exchange (0) by the editor.
    std::atomic<float> inputPeak  { 0.0f };
    std::atomic<int> inputChannels { 0 }; // DAW mode: how many inputs the engine gets

    // The most audio interface inputs the app opens (DAW tracks pick from them).
    static constexpr int maxAppInputs = 16;
    std::atomic<float> outputPeak { 0.0f };

    std::atomic<int> currentPreset { 0 };

    // Live mode performance controls (not saved, not parameters):
    // outputMuted silences the output (panic button); dryCompare skips the effect chain
    // so you hear the plain mic, to compare.
    std::atomic<bool> outputMuted { false };
    std::atomic<bool> dryCompare { false };

    // Tells the host (and the recorder's take alignment) how late the output is: the
    // Live chain's delay (autotune, pitch shift), or the DAW engine's in DAW mode.
    // Runs on a timer, since switching autotune on or off changes it. Message thread.
    void updateLatency();

private:
    explicit PluginProcessor (juce::AudioProcessorValueTreeState::ParameterLayout layout);

    void updateDspParameters();

    void timerCallback() override { updateLatency(); }

    Parameters params;
    juce::AudioProcessorValueTreeState state;

    juce::dsp::Gain<float> inputGain, outputGain;
    fx::FxChain chain { fx::FxChain::getAllTypes() };
    std::atomic<double> echoTapTempo { 0.0 };
    fx::modules::Pitch& pitchModule;
    fx::modules::AutoLevel& autoLevelModule;
    juce::dsp::Limiter<float> limiter;

    Recorder recorder;
    CaptureRing liveCapture;
    std::atomic<double> captureSeconds { defaultCaptureSeconds };
    Multitrack multitrack;
    Looper looper;
    PitchTap pitchTap;
    Scenes scenes { *this, state };
    LiveHistory liveHistory { state };
    SoundCheck soundCheck { state, chain, scenes };
    Setlist setlist { *this, scenes, looper, multitrack };
    MidiMap midiMap { state, looper, juce::JUCEApplicationBase::isStandaloneApp() || wrapperType != wrapperType_Undefined
                                         ? MidiMap::getDefaultFile() : juce::File() };
    juce::AudioBuffer<float> loopInput; // the voice before the limiter, for the looper

    // The backing (the Multitrack's playback) on its own before it joins the voice, and
    // its momentary loudness, for auto level's "over the backing".
    juce::AudioBuffer<float> backingBuffer;
    fx::LoudnessMeter backingMeter;
    bool backingWasPlaying = false;

    juce::SharedResourcePointer<daw::PluginHost> pluginHost; // before the engine, which loads instruments through it
    daw::Edit dawEdit;
    daw::DawEngine dawEngine { dawEdit };
    daw::Project dawProject { dawEdit };
    std::atomic<bool> dawMode { false };

    // Live mode's chord for the harmonizer (Chords mode): from the backing track's
    // chord list, or by listening to the instrument input (the interface's 2nd input).
    // Keys held on a MIDI keyboard win while they're down.
    void followChords (const juce::AudioBuffer<float>&, int numIn, int numSamples, const juce::MidiBuffer&) noexcept;
    analysis::ChordFollower chordFollower;
    std::bitset<128> heldKeys; // Live MIDI notes down now (audio thread)

    void applyPeakGuard (juce::AudioBuffer<float>&, int numChannels, int numSamples) noexcept;
    // Transparent peak guard for the voice + playback sum: only acts above -1 dB, so
    // the music isn't coloured (juce::dsp::Limiter compresses from -10 dB).
    float mixGuardGain = 1.0f, mixGuardRelease = 0.0f;
    juce::AudioBuffer<float> dryInput; // the raw mic, kept for the recorder's dry file

    // Each chain value and the plugin parameter that drives it.
    struct Binding
    {
        enum class Kind { number, toggle, choice };

        fx::Module::Value* value;
        juce::RangedAudioParameter* parameter;
        Kind kind;
    };

    std::vector<Binding> bindings;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginProcessor)
};
