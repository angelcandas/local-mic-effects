#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "analysis/Coach.h"
#include "analysis/Structure.h"
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
    //   "report"                   -> describeReport()
    //   "loop" { phrase, speed, semitones } loops one phrase (with a lead-in), slower and/or in
    //                              another key (rendered with the warp engine); "loopReady" follows
    //   "loop" { section, ... }    loops a section of the song (verse, chorus...: see "sections"),
    //                              with every phrase in it to sing
    //   "sections"                 finds the song's sections (analysis::structure) in the
    //                              background, once per song (kept in its cache folder); a
    //                              "practice" event { state: "sections", sections } follows
    //   "unloop"                   back to the whole song
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
        std::function<stems::Loaded (const std::function<void (stems::LoadPhase)>&, const stems::Progress&)> loadSeparator =
        [] (const std::function<void (stems::LoadPhase)>& onPhase, const stems::Progress& progress) { return stems::loadBest (onPhase, progress); };
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
    // Looping a phrase (static parts for the tests).
    static constexpr double loopLeadIn = 1.5, loopTail = 0.6; // seconds around the phrase

    // One phrase's notes as a song of their own: moved `offset` seconds earlier, played
    // `speed` times as fast and `semitones` higher.
    static analysis::coach::Song cutSong (const analysis::coach::Song&, int phrase, double offset, double speed, double semitones);

    // Phrases first..last (each kept as a phrase of its own), the same way.
    static analysis::coach::Song cutSong (const analysis::coach::Song&, int firstPhrase, int lastPhrase, double offset,
                                          double speed, double semitones);

    // The phrases that start inside a stretch of the song: { first, last }, or { -1, -1 }.
    static std::pair<int, int> getPhrasesIn (const analysis::coach::Song&, double from, double to);

    // The song's sections, as found (empty until "sections" has run). Tests may set them.
    const std::vector<analysis::structure::Section>& getSections() const noexcept { return sections; }
    void setSections (std::vector<analysis::structure::Section> s) { sections = std::move (s); sectionsKnown = true; }

    // Sections to and from the cache file's text (a line per section).
    static juce::String sectionsToText (const std::vector<analysis::structure::Section>&);
    static std::vector<analysis::structure::Section> sectionsFromText (const juce::String&);
    static constexpr const char* sectionsFileName = "sections-v1.txt";

    struct LoopAudio
    {
        juce::AudioBuffer<float> backing, guide; // empty when there was none
        bool cancelled = false;
    };

    // `from`..`to` seconds of the backing and the guide (either may be null), stretched and
    // transposed. The guide keeps its formants, so the voice keeps its character.
    static LoopAudio renderLoop (const juce::AudioBuffer<float>* backing, const juce::AudioBuffer<float>* guide, double sampleRate,
                                 double from, double to, double speed, double semitones,
                                 const std::function<bool()>& shouldContinue = {});

    // { score, pitch, timing, steady, lean, timingSeconds, scored, total, weak: [i], phrases: [...] }:
    // the pass so far (or the last loop pass).
    juce::var describeReport() const;

    bool isLooping() const noexcept { return loop.active; }

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
    struct LoopJob;
    struct SectionsJob;

    void startLoop (int phrase, double speed, double semitones);
    void startSectionLoop (int section, double speed, double semitones);
    void startRangeLoop (int firstPhrase, int lastPhrase, int section, double from, double to, double speed, double semitones);
    void findSections();
    juce::var describeSections() const;
    void finishLoop (LoopJob&);
    void endLoop (bool restoreTracks = true);

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
    analysis::coach::Song song;     // the target now (a loop's own while looping)
    analysis::coach::Song baseSong; // the whole song
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
    bool wasPlaying = false;

    // Looping a phrase.
    struct Loop
    {
        bool active = false;
        int phrase = -1;                  // the first phrase looped
        int lastPhrase = -1, section = -1;  // the last one; the section looped (-1: a phrase)
        double speed = 1.0, semitones = 0.0;
        double from = 0.0, to = 0.0;      // in the song
        int backingId = -1, guideId = -1; // rendered tracks (-1: looping the song's own tracks)
        std::vector<std::pair<int, bool>> muted; // the song's tracks muted meanwhile, and how they were
    } loop;
    std::shared_ptr<LoopJob> loopJob;

    // The song's sections.
    std::vector<analysis::structure::Section> sections;
    bool sectionsKnown = false;
    std::shared_ptr<SectionsJob> sectionsJob;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Practice)
};
