#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_data_structures/juce_data_structures.h>

#include <array>
#include <bitset>
#include <cstdint>
#include <vector>

// MIDI effects: what a MIDI track's notes go through before its instrument plays them
// (market item 13). Three kinds, in the order the track lists them:
//
//   arp    an arpeggiator: plays the held notes one after another on a beat grid, up,
//          down, up and down, as played, at random or all together, over 1 to 4 octaves.
//   chord  adds notes above each note: a fixed shape (major, minor, 7th...) or the triad or
//          seventh chord of the song key that the note starts.
//   scale  keeps notes in a scale: the song key's (PROJECT key/keyMinor) or one of its own,
//          moving a note outside it to the nearest scale note or dropping it.
//
// Stored as MIDIFX children of the TRACK (see Edit.h), applied to the timeline's notes,
// live MIDI and auditions alike, so a render or a freeze sounds as playback does. The
// settings are built on the message thread with each engine graph (Settings, from the
// tree); the running state (held notes, the arp's place) lives with the track's strip on
// the audio thread (Processor), so edits elsewhere don't restart it.
namespace daw::midifx
{
enum class Type { arp, chord, scale };

// Arp patterns, as stored (MIDIFX `pattern`).
enum class Pattern { up, down, upDown, played, random, together };

// One effect's settings.
struct Settings
{
    int id = 0;
    Type type = Type::arp;
    bool on = true;

    // arp
    double rate = 0.25;      // beats per step (0.25 = sixteenths)
    Pattern pattern = Pattern::up;
    int octaves = 1;         // 1..4
    float gate = 0.8f;       // a note's length as a part of its step (0.05..1; 1 = legato)
    bool latch = false;      // keep playing the last notes after they're let go (until new ones or a stop)

    // chord
    std::array<int, 6> intervals { 0, 4, 7, 0, 0, 0 }; // semitones above the note (the first is 0)
    int numIntervals = 3;
    int inKeyNotes = 0;      // 3 or 4: stack the song key's notes instead (triad / seventh); 0: the intervals
    float level = 1.0f;      // the added notes' velocity, as a part of the played note's

    // scale (and chord inKey): the scale's pitch classes (bit 0 = C) and what to do outside it
    int scaleMask = 0xAB5;   // C major
    bool drop = false;       // true: notes outside the scale are left out; false: moved to the nearest
};

using Chain = std::vector<Settings>;

// Most effects a track keeps (more are ignored).
inline constexpr int maxEffects = 8;

// The names stored in the tree and shown on the page.
const char* typeKey (Type) noexcept;
bool typeFromKey (const juce::String&, Type&) noexcept;
const char* patternKey (Pattern) noexcept;

// Chord shapes (MIDIFX `shape`): key, name and intervals. "key" and "key7" stack the
// song key's notes.
struct Shape { const char* key; const char* name; std::array<int, 6> intervals; int count; };
const std::vector<Shape>& getShapes();

// Scales (MIDIFX `scale`): "song" (the song key; C major when it has none) or one of these,
// on MIDIFX `root` (0 = C).
struct Scale { const char* key; const char* name; int mask; };
const std::vector<Scale>& getScales();
int rotateMask (int mask, int root) noexcept; // a mask on C moved up to `root`
int getSongScaleMask (int key, bool minor) noexcept; // key -1: none (C major)

// A MIDIFX tree's settings. `songKey` -1 when the project has none.
Settings fromTree (const juce::ValueTree& midiFx, int songKey, bool songMinor);
Chain fromTrack (const juce::ValueTree& track, int songKey, bool songMinor);

// What the audio thread hands the chain each block.
struct Context
{
    double sampleRate = 48000.0;
    bool playing = false;
    double beat = 0.0;  // the beat at the block's first sample (playing)
    double bpm = 120.0; // the tempo there
};

// The running chain of one track. Audio thread, except prepare (before playing).
class Processor
{
public:
    Processor();
    void prepare (int maxEventsHint = 2048);

    // `midi` (the block's events, in time order) through the chain, in place. A chain with
    // other effects than last time (added, removed, switched on/off, moved) first ends what
    // the old one left sounding.
    void process (juce::MidiBuffer& midi, const Chain&, const Context&, int numSamples) noexcept;

    // Ends every note the chain started (note-offs into `midi` at `at`) and forgets the rest.
    void flush (juce::MidiBuffer& midi, int at) noexcept;

    bool isActive() const noexcept { return active; }

    // One effect's running state (public for the stage functions in the .cpp).
    struct Stage
    {
        int id = -1;
        Type type = Type::arp;
        // chord/scale: what each played note became (-1: nothing / unused)
        std::array<std::array<std::int8_t, 7>, 128> mapped {};
        std::array<std::uint8_t, 128> outCount {}; // how many played notes hold each output note

        // arp
        struct Held { std::int8_t note; std::uint8_t velocity; };
        std::array<Held, 32> held {}; // in the order they were played
        int numHeld = 0, numDown = 0; // numDown: keys still held (latched notes stay in `held`)
        bool latched = false;         // the held notes stay after their keys are let go
        std::array<std::int8_t, 8> sounding {}; int numSounding = 0; // the arp's notes now on
        std::int64_t nextStep = -1, offAt = -1, lastStep = -1; // on the processor's clock
        int step = 0;
        std::uint32_t random = 0x9e3779b9u;
        bool wasPlaying = false;

        void reset() noexcept;
    };

private:
    std::array<Stage, maxEffects> stages;
    std::array<int, maxEffects> layout {}; // the ids (negative when off) of the chain last block
    int numLayout = 0;
    std::bitset<128> out;                  // notes on at the chain's output
    juce::MidiBuffer a, b;
    std::int64_t clock = 0;                // samples processed
    bool active = false;
};
} // namespace daw::midifx
