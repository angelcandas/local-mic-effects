#pragma once

#include "Key.h"

#include <juce_dsp/juce_dsp.h>

#include <array>
#include <atomic>
#include <bitset>
#include <functional>
#include <vector>

// Chords, found locally from the same chroma as the song key (Key.h): each chroma frame
// is compared with the note pattern of every chord on every root, and the best match
// wins. Used by the live harmonies (PitchEngine), which sing chord notes.
//
// Two ways in:
// - detectChords: a whole recording (a backing track), offline. Every 100 ms frame gets
//   a chord; a best path through the frames (switching chords costs a little, so short
//   blips don't split a held chord) gives the chord segments.
// - ChordFollower: live audio (an instrument input, the song as it plays) and/or held
//   MIDI notes, on the audio thread, with nothing allocated after prepare().
namespace analysis
{
//==============================================================================
struct Chord
{
    enum Quality
    {
        major, minor, dominant7, major7, minor7, diminished, sus4, sus2, augmented, power,
        numQualities
    };

    int root = -1;       // 0 = C ... 11 = B; -1 = no chord
    int quality = major;
    int bass = -1;       // the lowest note when it isn't the root (C/E), else -1

    bool isValid() const noexcept { return root >= 0; }

    // Pitch classes as bits (bit 0 = C).
    int getMask() const noexcept;
    bool contains (int pitchClass) const noexcept { return isValid() && ((getMask() >> (((pitchClass % 12) + 12) % 12)) & 1) != 0; }

    // "C", "Am", "G7", "Fmaj7", "Dm7", "Bdim", "Dsus4", "Asus2", "Caug", "E5", "C/E"; "" when invalid.
    juce::String getName() const;

    // Semitones above the root of each quality's notes (root first).
    static const std::vector<int>& getIntervals (int quality);

    // One int for atomics and the UI: -1 when invalid.
    int pack() const noexcept { return isValid() ? root | (quality << 4) | ((bass + 1) << 8) : -1; }
    static Chord unpack (int packed) noexcept;

    bool operator== (const Chord& other) const noexcept { return root == other.root && (root < 0 || (quality == other.quality && bass == other.bass)); }
    bool operator!= (const Chord& other) const noexcept { return ! operator== (other); }
};

// How well each chord fits 12 note weights (any scale), 0..1 (cosine similarity with
// the chord's note pattern, a little less for the rarer chords). `bass` is the lowest
// sounding note class or -1; a chord on it scores a little more.
// `keyMask` (bit 0 = C, 0 = unknown) favours chords made of the key's notes a little.
float scoreChord (const std::array<float, 12>& chroma, int root, int quality, int bass = -1, int keyMask = 0) noexcept;

// The best chord for these note weights, with its score; root -1 when nothing fits
// (silence, or the score is under `minScore`).
Chord matchChord (const std::array<float, 12>& chroma, int bass = -1, float minScore = 0.6f,
                  float* score = nullptr, int keyMask = 0) noexcept;

// A key's pitch classes as bits (bit 0 = C): the major or natural minor scale.
int getKeyMask (int key, bool minor) noexcept;

// The chord a set of held notes (MIDI note numbers) makes. One note alone becomes the
// triad on it from the key (`keyMask` bits = the key's pitch classes, 0 = none: major).
Chord chordFromNotes (const std::bitset<128>& notes, int keyMask = 0) noexcept;

// 36 chroma bins (thirds of a semitone) folded to 12 notes around the tuned note
// centres (`tuningSemitones` = KeyEstimate::tuningCents / 100), strongest note = 1.
std::array<float, 12> foldChroma (const Chroma& chroma, float tuningSemitones = 0.0f) noexcept;

//==============================================================================
struct ChordSegment
{
    juce::int64 start = 0; // samples
    Chord chord;
};

// The chords of a whole recording, in time order (contiguous; a segment lasts until the
// next one starts). Quiet or chordless stretches are left out (they continue the chord
// before them). `key`, when given, slightly favours the key's own chords.
std::vector<ChordSegment> detectChords (const juce::AudioBuffer<float>&, double sampleRate,
                                        const KeyEstimate* key = nullptr,
                                        const std::function<bool()>& shouldContinue = {});

// The chord at a sample position (the segment it falls in), or none before the first.
Chord chordAt (const std::vector<ChordSegment>&, juce::int64 position) noexcept;

//==============================================================================
// Follows the chords of live audio and/or held MIDI notes. prepare() on any thread
// before the audio starts; the rest on the audio thread (getChord also from others).
// Held notes win while any are down; otherwise the audio decides, and a chord stays
// until another one has been clear for a moment (gaps and silence keep the last).
class ChordFollower
{
public:
    void prepare (double sampleRate);
    void reset() noexcept;

    // Mono audio to listen to.
    void pushAudio (const float* samples, int numSamples) noexcept;

    // The notes held right now (MIDI note numbers).
    void setHeldNotes (const std::bitset<128>& notes) noexcept;

    // The key's pitch classes (bit 0 = C; 0 = unknown): a single held note becomes the
    // key's triad on it, and audio slightly prefers the key's chords.
    void setKeyMask (int mask) noexcept { keyMask = mask; }

    // Forces the chord (an offline chord list driving it).
    void setChord (const Chord& chord) noexcept { setCurrent (chord); }

    Chord getChord() const noexcept { return Chord::unpack (current.load (std::memory_order_relaxed)); }

private:
    void analyse() noexcept;
    void setCurrent (const Chord&) noexcept;

    double sampleRate = 48000.0;
    int fftOrder = 13, fftSize = 8192, hop = 1024;
    std::unique_ptr<juce::dsp::FFT> fft;
    std::vector<float> ring, window, data, bassData;
    int writeIndex = 0, sinceHop = 0, filled = 0;

    std::array<float, 12> smoothed {}, smoothedBass {};
    float smoothing = 0.8f;
    int keyMask = 0;

    Chord candidate;
    int candidateHops = 0;
    bool holdingNotes = false;

    std::atomic<int> current { -1 };
};
} // namespace analysis
