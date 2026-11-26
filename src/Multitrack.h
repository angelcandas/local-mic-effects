#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_data_structures/juce_data_structures.h>
#include <juce_events/juce_events.h>

#include "analysis/Chords.h"

#include <map>

// A small multi-track player for singing along: a backing track plus the takes you
// record, laid out on one timeline and mixed into the output while the transport plays.
//
// Threading: everything except process() runs on the message thread. Files are decoded
// (and resampled to the device rate) on a background thread, then installed on the
// message thread. The audio thread reads the track list through a try-lock and never
// waits; it skips mixing for a block if the list is being swapped at that moment.
class Multitrack
{
public:
    // A track whose audio is written while it already plays (Practice's key and speed
    // change renders it piece by piece on other threads). The audio thread plays only the
    // blocks marked ready; the rest stays silent until its piece lands.
    struct Fill
    {
        static constexpr int blockSize = 1024;

        explicit Fill (int numSamples);

        // Track samples from..to (clamped) are all ready to play.
        bool isReady (juce::int64 from, juce::int64 to) const noexcept;
        void markReady (juce::int64 from, juce::int64 to) noexcept; // whole blocks inside from..to
        void markBlock (int block) noexcept { if (juce::isPositiveAndBelow (block, numBlocks)) ready[(size_t) block].store (true, std::memory_order_release); }
        bool isBlockReady (int block) const noexcept { return ready[(size_t) block].load (std::memory_order_acquire); }

        const int numSamples, numBlocks;
        std::unique_ptr<std::atomic<bool>[]> ready;
        std::atomic<bool> complete { false };     // every block ready (no more checks)
        std::vector<float*> channels;             // the track's audio, for the writers
    };

    struct Track
    {
        int id = 0;
        juce::String name;
        juce::File file;
        bool isTake = false;
        juce::AudioBuffer<float> audio; // at the session sample rate
        juce::int64 start = 0;          // timeline position of the first sample (may be negative)
        juce::Array<float> peaks;       // max |sample| per peakSeconds, for drawing waveforms
        std::vector<analysis::ChordSegment> chords; // backing tracks: the song's chords (track samples)

        std::atomic<float> gain { 1.0f };
        std::atomic<bool> muted { false };

        std::shared_ptr<Fill> fill;     // set when the audio is still being written

        juce::int64 end() const noexcept { return start + audio.getNumSamples(); }
    };

    static constexpr double peakSeconds = 0.02;

    Multitrack();
    ~Multitrack();

    // Message thread. A sample-rate change reloads every track at the new rate.
    void prepare (double sampleRate);

    // Message thread. Decode in the background; the track appears when it's ready
    // and onTracksChanged fires. Errors are reported through onError.
    void loadBackingTrack (const juce::File&);
    void addTake (const juce::File&, juce::int64 startSample);
    // A take that was kept before, with its volume and mute.
    void addTake (const juce::File&, juce::int64 startSample, float gain, bool muted);

    // Audio already in memory at the session rate (e.g. a rendered copy), as a backing
    // track named `name`. Returns its id.
    int addAudio (const juce::String& name, juce::AudioBuffer<float>&&, juce::int64 startSample);

    // An empty track of `numSamples` (silent until written through the returned Fill),
    // added muted or not. Returns its id. Call finishFill once every sample is written.
    int addFilling (const juce::String& name, int numChannels, int numSamples, juce::int64 startSample, bool muted,
                    std::shared_ptr<Fill>& fill);
    // The track is complete: plays like any other from now on (and its waveform is drawn).
    void finishFill (int id);

    // Synchronous version for tests. Returns the new track's id, or -1.
    int loadNow (const juce::File&, bool isTake, juce::int64 startSample);

    // The sing-along rows that come from files (backing track and takes), with their place,
    // volume and mute, so they come back after a restart. restore replaces every row;
    // files that are gone are left out. Message thread.
    juce::ValueTree saveSession() const;
    void restoreSession (const juce::ValueTree&);

    void removeTrack (int id);
    void setTrackGain (int id, float gain);
    void setTrackMuted (int id, bool muted);

    std::vector<std::shared_ptr<const Track>> getTracks() const { return { tracks.begin(), tracks.end() }; }
    int getNumLoading() const noexcept { return numLoading; }

    // Transport. Positions are in samples at the session rate.
    void play() noexcept                     { playing = true; }
    void pause() noexcept                    { playing = false; }
    void seek (juce::int64 sample) noexcept  { position = juce::jmax ((juce::int64) 0, sample); }
    bool isPlaying() const noexcept          { return playing.load(); }

    // Playback that reaches `end` goes back to `start` (samples). end <= start: no loop.
    void setLoop (juce::int64 start, juce::int64 end) noexcept { loopStart = start; loopEnd = end; }
    void clearLoop() noexcept                { loopEnd = 0; loopStart = 0; }
    bool isLooping() const noexcept          { return loopEnd.load() > loopStart.load(); }
    juce::int64 getLoopStart() const noexcept { return loopStart.load(); }
    juce::int64 getLoopEnd() const noexcept  { return loopEnd.load(); }
    juce::int64 getPosition() const noexcept { return position.load(); }
    juce::int64 getLength() const noexcept   { return length.load(); }
    double getSampleRate() const noexcept    { return sampleRate; }

    // Audio thread. Adds every unmuted track under the playhead into `buffer` and
    // advances the playhead. Returns the timeline position of the block's first sample.
    juce::int64 process (juce::AudioBuffer<float>& buffer, int numChannels, int numSamples) noexcept;

    // Audio thread. The backing track's chord at a timeline position (found when the
    // track loaded), or none: no backing track, outside it, or the list is being swapped.
    analysis::Chord getBackingChord (juce::int64 timelinePosition) noexcept;

    std::function<void()> onTracksChanged;
    std::function<void (const juce::String&)> onError;

    // Any thread. Reads a whole file (mono or stereo, at most 30 minutes) resampled to
    // targetRate. On failure `audio` is empty and `error` says why.
    struct Decoded
    {
        juce::AudioBuffer<float> audio;
        juce::String error;
    };

    static Decoded decode (const juce::File&, double targetRate);

private:
    void startLoad (const juce::File&, bool isTake, juce::int64 startSample, int id);
    std::shared_ptr<Track> makeTrack (const juce::File&, bool isTake, juce::int64 startSample, int id, juce::AudioBuffer<float>&&);
    void install (std::shared_ptr<Track>);
    void publish(); // rebuilds the audio thread's track list
    void mix (juce::AudioBuffer<float>&, int numChannels, int offset, juce::int64 from, int numSamples) noexcept;

    double sampleRate = 48000.0;
    int nextId = 1;

    std::vector<std::shared_ptr<Track>> tracks; // message thread, ordered by id
    std::map<int, std::pair<float, bool>> pendingMix; // gain and mute for rows still loading

    juce::SpinLock liveLock;
    std::vector<Track*> live; // what the audio thread mixes

    std::atomic<bool> playing { false };
    std::atomic<juce::int64> position { 0 };
    std::atomic<juce::int64> length { 0 };
    std::atomic<juce::int64> loopStart { 0 }, loopEnd { 0 };
    std::atomic<int> numLoading { 0 };

    juce::ThreadPool loader { juce::ThreadPoolOptions{}.withThreadName ("MicFX track loader").withNumberOfThreads (1) };
    std::shared_ptr<bool> alive = std::make_shared<bool> (true);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Multitrack)
};
