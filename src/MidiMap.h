#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <map>

#include "Looper.h"

// MIDI learn for Live mode: pedals, knobs, keys and program changes drive any parameter
// and a set of actions (the looper's buttons, presets, mute, record, scenes).
//
// A mapping ties one source (a CC, note or program change on one channel) to one target:
//   "param:<parameter id>"   a CC moves it (knobs pick up: they act only once the knob
//                            reaches the current value, so nothing jumps); a key, a
//                            pedal press or a program change switches toggles, steps
//                            through choices and flips knobs between their two ends
//   "action:<name>"          on a key, pedal press (CC crossing 64 upwards) or program
//                            change. The looper actions run on the audio thread, at the
//                            start of the block, so a footswitch is as tight as the
//                            block; the others run on the message thread:
//                            looper.main, looper.stop, looper.undo, looper.redo,
//                            looper.clear, preset.next, preset.previous, mute,
//                            scene.next, scene.previous, scene.1 ... scene.8, and the
//                            page's own: record.
//
// Threading: process() runs on the audio thread and only reads the mapping table through
// a try-lock, so it never waits. Events reach the message thread through a lock-free
// FIFO, drained by a 60 Hz timer that learns, moves parameters and runs actions.
// Everything else is message thread. Mappings are saved in a JSON file as they change.
class MidiMap final : private juce::Timer
{
public:
    enum class Source { cc, note, program };

    struct Mapping
    {
        Source source = Source::cc;
        int channel = 1;   // 1..16
        int number = 0;    // CC number, note number or program
        juce::String target;

        bool sameSource (const Mapping& other) const noexcept
        {
            return source == other.source && channel == other.channel && number == other.number;
        }
    };

    // `file` keeps the mappings (an empty File keeps them in memory only, for tests).
    MidiMap (juce::AudioProcessorValueTreeState&, Looper&, juce::File file);
    ~MidiMap() override;

    static juce::File getDefaultFile();
    static juce::String describe (const Mapping&);   // "CC 64 · ch 1", "Note C3 · ch 2"...
    static bool isAudioThreadAction (const juce::String& target);

    // Message thread.
    std::vector<Mapping> getMappings() const;
    void setMappings (std::vector<Mapping>);
    void removeTarget (const juce::String& target);
    void clear()                                      { setMappings ({}); }

    // The next CC, note or program change that arrives is mapped to `target` (replacing
    // what that source or that target had). An empty target stops learning.
    void learn (const juce::String& target);
    juce::String getLearnTarget() const               { return learnTarget; }

    // Message thread callbacks.
    std::function<void (const Mapping&)> onLearned;
    std::function<void (const juce::String& action)> onAction; // actions this class doesn't run itself
    std::function<void (int delta)> onPresetStep;
    std::function<void()> onMuteToggle;
    std::function<void (const juce::String& action)> onScene; // "scene.next", "scene.previous", "scene.3"...

    // Audio thread: reads the block's MIDI (Live mode). Looper actions run here.
    void process (const juce::MidiBuffer&) noexcept;

    // The last MIDI message seen, for the page's activity light ("CC 64 · ch 1"), and a
    // counter that goes up with every message. Message thread.
    juce::String getLastEvent() const                 { return lastEvent; }
    int getEventCount() const noexcept                { return eventCount; }

    // Runs the queued events now (tests; the timer does this in the app).
    void drain();

    juce::var toVar() const;
    static std::vector<Mapping> fromVar (const juce::var&);

private:
    struct Event
    {
        Source source;
        int channel, number, value; // value: CC 0..127, velocity (0 = off), program 0
    };

    void timerCallback() override { drain(); }
    void apply (const Mapping&, const Event&);
    void publish(); // rebuilds the audio thread's table
    void save() const;

    static int key (Source s, int channel, int number) noexcept { return ((int) s << 12) | ((channel - 1) << 8) | number; }

    juce::AudioProcessorValueTreeState& state;
    Looper& looper;
    juce::File file;

    std::vector<Mapping> mappings;
    juce::String learnTarget;
    juce::String lastEvent;
    int eventCount = 0;

    // The audio thread's view: source key -> looper command, for the looper actions.
    struct AudioEntry { int key; Looper::Command command; };
    juce::SpinLock tableLock;
    std::array<AudioEntry, 64> table {};
    int tableSize = 0;
    std::array<juce::uint8, 16 * 128> lastCc {}; // for the "crossing 64" edge, audio thread

    // Message thread state for the CC pick-up and edges.
    std::array<juce::uint8, 16 * 128> lastCcMessage {};
    std::array<bool, 16 * 128> ccSeen {};
    std::map<juce::String, bool> pickedUp;
    std::map<juce::String, float> lastSet;

    juce::AbstractFifo fifo { 512 };
    std::array<Event, 512> events {};
};
