#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <functional>
#include <vector>

//==============================================================================
// Undo for Live mode: every change to the Live sound (a knob gesture, a switch, a preset,
// a scene, a card reset, Sound check, MIDI learn moves) becomes one named step.
//
// It watches the parameters instead of being told about each edit, so every way of
// changing the sound is covered. A change is committed once the values have rested for
// a moment, which joins a knob drag or a scene glide into one step. The page may name
// the next step first (label), e.g. "Load preset Pop Tune".
//
// Message thread only.
class LiveHistory final : private juce::Timer
{
public:
    explicit LiveHistory (juce::AudioProcessorValueTreeState&);
    ~LiveHistory() override;

    // Names the next step (until it's committed or a second passes with no change).
    void label (const juce::String& name);

    // While held, changes pile up into one step that's committed (with this name) on
    // release, e.g. Sound check comparing before and after until you keep or revert.
    void hold (const juce::String& name);
    void release();

    bool undo();
    bool redo();
    juce::String getUndoName() const;
    juce::String getRedoName() const;

    // Commits a pending change at once (tests, and before undo/redo).
    void flush();

    // Forgets every step, e.g. after a state load (the loaded sound is the new start).
    void clear();

    // Called after every new step, undo or redo.
    std::function<void()> onChange;

    // The loaded preset's number, kept with each step so undo also puts its name back.
    std::function<int()> getPreset;
    std::function<void (int)> setPreset;

    static constexpr int maxSteps = 100;
    static constexpr int restMs = 350; // values unchanged this long end a step

private:
    using Snapshot = std::vector<float>; // normalised values, in params order

    struct Step
    {
        Snapshot before, after;
        juce::String name;
        int presetBefore = 0, presetAfter = 0;
    };

    void timerCallback() override;
    Snapshot take() const;
    void apply (const Snapshot&);
    juce::String describe (const Snapshot& before, const Snapshot& after) const;
    void commit (const Snapshot& now);

    juce::AudioProcessorValueTreeState& state;
    juce::Array<juce::RangedAudioParameter*> params;
    Snapshot committed, last;
    int committedPreset = 0;
    juce::uint32 lastChangeMs = 0;
    juce::String pendingLabel;
    juce::uint32 labelMs = 0;
    bool held = false;
    std::vector<Step> undoSteps, redoSteps;
};
