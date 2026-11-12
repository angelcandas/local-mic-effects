#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

// Scenes: named snapshots of the Live sound (every parameter but the input gain) for the
// parts of a song (verse, chorus, bridge). Recalling one glides there: knobs move
// smoothly over the glide time, effects that come on switch on at the start and effects
// that go off switch off at the end, and choices change at once.
//
// The scenes live in the plug-in state (a SCENES child of the parameter tree), so they're
// saved with the DAW session, the app's state, and the setlist songs. Message thread only;
// a 60 Hz timer runs the glide, so it works with the window closed.
class Scenes final : private juce::Timer
{
public:
    static constexpr int maxScenes = 8;

    Scenes (juce::AudioProcessor&, juce::AudioProcessorValueTreeState&);
    ~Scenes() override;

    int size() const;
    juce::String getName (int index) const;
    int getActive() const;                 // the last recalled or saved scene, or -1
    bool isModified() const;               // the sound differs from the active scene
    bool isGliding() const noexcept        { return ! moves.empty(); }

    double getGlideSeconds() const;
    void setGlideSeconds (double seconds); // 0..10

    // Saves the current sound as a new scene ("Scene N" if the name is empty); returns its
    // index, or -1 when there are already maxScenes.
    int add (const juce::String& name = {});
    void update (int index);               // the current sound replaces the scene's
    void rename (int index, const juce::String& name);
    void remove (int index);
    void recall (int index);
    void step (int delta);                 // the next / previous scene, wrapping

    // Every scene as [{ name }] plus active, modified, glide, gliding.
    juce::var describe() const;

    // Runs the glide forward (the timer does this in real time; tests call it directly).
    void advance (double seconds);

    // The scenes as a tree, and replacing them (a setlist song brings its own).
    juce::ValueTree copyScenes() const;
    void replaceScenes (const juce::ValueTree&);

private:
    struct Move
    {
        juce::RangedAudioParameter* parameter;
        float from, to;
        enum class When { glide, end } when;
    };

    void timerCallback() override;
    juce::ValueTree getTree() const;
    juce::String capture() const;
    static bool isSceneParameter (const juce::RangedAudioParameter&);

    juce::AudioProcessor& processor;
    juce::AudioProcessorValueTreeState& state;

    std::vector<Move> moves;
    double glideTime = 0.0, glideLength = 0.0;
    double lastTick = 0.0;
};
