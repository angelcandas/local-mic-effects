#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "analysis/Coach.h"
#include "stems/StemSplitter.h"

#include <functional>
#include <memory>
#include <vector>

class PluginProcessor;

// Practise singing any song (the Practice view in Live mode).
//
// Opening a song splits it into stems once: the voice becomes the guide (played at a
// level you choose, or muted) and the melody to sing (analysis::coach), the rest becomes
// the backing. Both go on the Live tracks, so the usual transport plays them. Everything
// is kept in a cache folder named after the file's contents, so opening the same song
// again is instant.
//
// While the view is open the processor's PitchTap follows the singer; every tick the
// readings go to the page (for the pitch trail) and each note and phrase is scored as it
// passes. Jumping back starts that part over.
//
// Message thread, except the preparation, which runs on its own thread.
class Practice
{
public:
    using Emit = std::function<void (const juce::Identifier&, const juce::var&)>;

    // `deviceLatency`: samples between a sound leaving the app and the singer's answer to
    // it coming back in (the audio interface's output + input latency).
    Practice (PluginProcessor&, juce::PropertiesFile& settings, Emit, std::function<juce::int64()> deviceLatency);
    ~Practice();

    // The page's "practice" function: ("action", { options }) -> result. Actions:
    //   "state"                    -> describe()
    //   "open" { split }           -> { ok }: picks a file; split: false when it's a voice on its own
    //   "openFile" { path, split } -> { ok, error }
    //   "cancel"                   stops a preparation
    //   "view" { open }            the view opened or closed (follows the singer while open)
    //   "guide" { level }          the guide vocal's level (0..1.5 gain, 0 mutes it)
    //   "reset"                    forgets this pass's scores
    juce::var handle (const juce::String& action, const juce::var& options);

    // About 30 times a second (the editor's timer).
    void tick();

    // { song, job, guideLevel, open, scores } for the page.
    juce::var describe() const;

    //==============================================================================
    // Preparing a song (background thread; static so the tests can call it).
    enum class Phase { reading, downloading, loading, splitting, listening };

    struct Prepared
    {
        analysis::coach::Song song;
        juce::File backing;  // the song without its voice; empty when the file was a voice alone
        juce::File guide;    // the voice
        juce::File folder;   // the cache folder
        double seconds = 0.0;
        juce::String error;
        bool cancelled = false;
        bool fromCache = false;
        juce::String note;   // e.g. why the GPU wasn't used
    };

    struct Options
    {
        bool split = true;
        juce::File cacheRoot = getCacheRoot();
        // Where the stem separator comes from (tests pass their own).
        std::function<stems::Loaded (const std::function<void (stems::LoadPhase)>&, const stems::Progress&)> loadSeparator = stems::loadBest;
    };

    using Progress = std::function<bool (Phase, double)>; // false cancels

    static Prepared prepare (const juce::File& source, const Options&, const Progress&);

    // ~/Library/Application Support/MicFX/Practice (or the platform's equivalent).
    static juce::File getCacheRoot();

    // The cache folder for a file: its contents' SHA-256 (first 20 hex digits), plus
    // "-voice" when it's used as a voice alone.
    static juce::File getCacheFolder (const juce::File& source, bool split, const juce::File& root);

    static constexpr const char* melodyFileName = "melody-v1.txt";
    static constexpr const char* backingFileName = "Backing (no voice).wav";
    static constexpr const char* guideFileName = "Guide voice.wav";

    //==============================================================================
    // Live scoring, exposed for the tests.
    const analysis::coach::Song& getSong() const noexcept { return song; }
    void setSong (analysis::coach::Song);
    // Pitch frames in song time, as tick() adds them while the song plays, then scores
    // every note and phrase that has ended by `songTime`.
    void addFrames (const std::vector<analysis::coach::Frame>&, double songTime);
    // The transport jumped: what was sung from `songTime` on is forgotten and scored again.
    void restartFrom (double songTime);
    const std::vector<analysis::coach::NoteScore>& getNoteScores() const noexcept { return noteScores; }
    const std::vector<analysis::coach::PhraseScore>& getPhraseScores() const noexcept { return phraseScores; }
    float getPassScore() const;

private:
    struct Job;

    void start (const juce::File&, bool split);
    void install (const Prepared&);
    void applyGuideLevel();
    juce::var describeSong() const;
    juce::var describeScores (bool all) const;
    juce::var describeJob() const;

    PluginProcessor& processor;
    juce::PropertiesFile& settings;
    Emit emit;
    std::function<juce::int64()> deviceLatency;

    std::shared_ptr<Job> job;
    std::unique_ptr<juce::FileChooser> chooser;

    // The song being practised.
    analysis::coach::Song song;
    juce::String songName;
    juce::File source, guideFile, backingFile;
    float guideLevel = 0.35f;
    bool guidePending = false; // the guide track hasn't loaded yet: set its level when it does
    bool viewOpen = false;

    // This pass.
    std::vector<analysis::coach::Frame> sung;
    std::vector<analysis::coach::NoteScore> noteScores;
    std::vector<analysis::coach::PhraseScore> phraseScores;
    int nextNote = 0, nextPhrase = 0;  // the first not scored yet
    std::vector<int> changedNotes, changedPhrases; // since the last "practiceScore" event
    bool scoresReset = false;
    double lastSongTime = 0.0;
    double lastPosition = -1.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Practice)
};
