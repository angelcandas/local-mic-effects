#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_events/juce_events.h>

#include "AudioPool.h"
#include "CaptureRing.h"
#include "Recorder.h"
#include "Edit.h"
#include "Instruments.h"
#include "PluginInsert.h"
#include "dsp/FxChain.h"

#include <algorithm>
#include <bitset>
#include <map>
#include <optional>
#include <set>

namespace daw
{
// Plays an Edit: every track's clips through its insert chain, volume and pan, summed
// into the master bus, which has its own inserts, volume and pan. Tracks can also send
// some of their signal to return tracks (pre or post fader), or play into one entirely;
// returns run their own inserts and play into the master.
//
// Threading. The engine listens to the Edit on the message thread and keeps an immutable
// render graph (tracks, clips, the chain each track uses) for the audio thread. A change
// to the track or clip layout builds a new graph and hands it over with an atomic
// pointer swap; the audio thread never locks or waits, and old graphs are freed on the
// message thread. Mixer moves and insert values are atomics and don't need a new graph,
// so dragging a fader or knob is cheap and keeps effect tails ringing.
//
// Latency compensation: every track is delayed to match the track whose inserts add
// the most latency, so tracks stay in time with each other. Returns are lined up the
// same way: each is delayed to match the slowest, and what plays straight into the
// master waits for them too.
//
// Recording is always dry: the raw mic goes to a WAV file and becomes a clip on every
// armed track, and the track's inserts are applied on playback. Tracks with monitor on
// hear the mic live through their inserts.
//
// MIDI tracks play their MIDI clips through their instrument (Instruments.h) and then
// their inserts. Armed or monitored MIDI tracks also play the live MIDI input, and
// recording on an armed MIDI track turns what's played into a MIDI clip.
class DawEngine : private juce::ValueTree::Listener,
                  private juce::AsyncUpdater,
                  private juce::Timer
{
public:
    // An offline engine (for exports) runs entirely on the thread that drives it: no
    // timer, and replaced graphs are freed straight away.
    explicit DawEngine (Edit&, bool realtime = true);

    // Clip edges that cut into a file's audio fade over this long, so edits don't click.
    static constexpr double antiClickSeconds = 0.004;
    ~DawEngine() override;

    // With the audio stopped (e.g. from prepareToPlay). Called off the message thread,
    // the work is posted to it; until then the old graph keeps playing safely.
    void prepare (double sampleRate, int maximumBlockSize);
    double getSampleRate() const noexcept { return sampleRate; }

    // Message thread. Applies any pending change now instead of on the next message
    // loop pass (for tests, and before an offline render).
    void rebuildNow();
    int getRebuildCount() const noexcept { return rebuildCount; } // graphs built so far (for tests)

    // Message thread (or the offline engine's thread). Decodes every clip's audio now
    // and rebuilds, so playback is complete from the first block.
    void loadAllNow();

    AudioPool& getAudioPool() noexcept { return pool; }

    // Message thread. While a clip's warp markers or notes are being dragged, the engine
    // can play them before they're stored: a preview overrides the clip's map (the
    // effective one, as setEffectiveWarp takes it; linked partners follow) or its notes
    // until it's cancelled or the clip's own warp or notes change (the real edit landing,
    // an undo). Not an undo step and never saved. Each new preview makes the previous one's
    // render stale, so it's cancelled (see AudioPool::releaseUnusedWarps).
    void setPreviewWarp (int clipId, const warp::Map&);
    void setPreviewNotes (int clipId, const std::vector<analysis::pitch::Note>&);
    void cancelPreview (int clipId); // -1: every clip
    bool hasPreview (int clipId) const { return previews.count (clipId) > 0; }

    // The settings the clip plays with: Edit::getWarpSettings with any preview on top.
    warp::Settings getPlaybackSettings (const juce::ValueTree& clip) const;

    //==============================================================================
    // Transport, in samples at the session rate. Any thread.
    void play() noexcept                     { playing = true; }
    void stop() noexcept                     { playing = false; }
    void seek (juce::int64 sample) noexcept  { position = juce::jmax ((juce::int64) 0, sample); }
    bool isPlaying() const noexcept          { return playing.load(); }
    juce::int64 getPosition() const noexcept { return position.load(); }
    juce::int64 getLength() const noexcept   { return length.load(); } // end of the last clip
    // While the Edit's loop is on, playback wraps from its end to its start (while
    // recording too, when the recording started inside the loop: see startRecording).

    // Delay the inserts add (the slowest track's chain plus the master chain), in samples.
    int getLatencySamples() const noexcept   { return latency.load(); }

    // The chord the harmonizer inserts follow (Chords mode), heard from the tracks
    // without pitch inserts and the MIDI being played; invalid when none follows. Any thread.
    analysis::Chord getHarmonyChord() const noexcept { return analysis::Chord::unpack (harmonyChord.load (std::memory_order_relaxed)); }

    //==============================================================================
    // Capture. The dry inputs of the armed audio tracks are kept for the last few
    // minutes whether or not anything is recording, so a take nobody pressed Record
    // for can still be put on the tracks. The memory is only used for armed inputs.
    // Message thread unless noted.
    static constexpr int maxCaptureInputs = 4;
    static constexpr double defaultCaptureSeconds = 180.0;
    void setCaptureSeconds (double seconds) noexcept { captureLength = juce::jmax (0.0, seconds); }
    double getCaptureSeconds() const noexcept        { return captureLength.load(); }
    // Seconds that capture() would take now (the most of any armed input), -1 when off.
    double getCaptureAvailable() const noexcept;

    struct CaptureResult
    {
        juce::String error;
        double seconds = 0.0;
        int tracks = 0; // armed tracks that got a clip
    };

    // Writes the newest `seconds` (0 = all of it) of each armed input to a WAV in `folder`
    // and adds it to the armed tracks on that input, in one undo step. Where it sits: the
    // music it was played to, when the playhead had been running; else at the playhead.
    CaptureResult capture (const juce::File& folder, double seconds);

    //==============================================================================
    // Recording. Message thread.
    //
    // startRecording() needs at least one armed track. It starts the transport from
    // the playhead (after the count-in, if any) and writes the dry inputs to new WAVs in
    // `folder`: one file per input the armed tracks use (mono, or stereo for a stereo
    // track). stopRecording() stops the transport and adds each armed track's take as
    // a clip on it, in one undo step (see Edit::addRecording: over audio already there,
    // takes go into take lanes). Returns an error message, or empty.
    // Starting inside the loop with the loop on, playback keeps wrapping while it records
    // and every pass round the loop becomes a take.
    // Armed MIDI tracks record the MIDI input (no file): stopRecording() adds a MIDI clip
    // with what was played to each of them.
    juce::String startRecording (const juce::File& folder);
    Recorder::Take stopRecording();

    // Where a recorded file goes on the timeline: one pass, or with loop recording one
    // per time round the loop (a last pass shorter than this is dropped).
    static constexpr double minLoopPassSeconds = 0.5;
    std::vector<Edit::RecordedPass> getRecordedPasses (const Recorder::Take&, bool looped) const;
    bool isRecording() const noexcept { return recordingAudio.load() || countingIn.load() || recordingMidi.load(); }
    juce::int64 getRecordStart() const noexcept { return recordStart.load(); } // where the take begins

    // Interface input + output latency, so takes line up with what was heard.
    void setRecordingLatency (juce::int64 samples) noexcept { recordingLatency = samples; }
    // Output latency only, for MIDI takes: keys are played to what's heard.
    void setMidiRecordingLatency (juce::int64 samples) noexcept { midiRecordingLatency = samples; }

    //==============================================================================
    // Instruments (MIDI tracks). Message thread.
    //
    // The running instrument of a track (for its editor window), or null.
    InstrumentPtr getInstrument (int trackId) const;
    // Why a track's instrument didn't load (empty when it did).
    juce::String getInstrumentError (int trackId) const;
    // Writes every instrument's current settings into its INSTRUMENT `state`, so a save
    // or an export has them. Not an undoable edit.
    // Effect plugin inserts too (their INSERT `state`).
    void captureInstrumentStates();
    // An effect plugin insert's running plugin (for its window), or null; and why it
    // didn't load (empty when it did, or when there's no such insert).
    InstrumentPtr getInsertPlugin (int insertId) const;
    // The delay an insert adds right now, in samples (0 if it isn't playing).
    int getInsertLatencySamples (int insertId) const;
    juce::String getInsertPluginError (int insertId) const;
    // Builds the graph again when a plugin now reports a different delay than it did (a
    // look-ahead switched on in its window), so latency compensation follows. Message
    // thread; the DAW screen calls it on its timer.
    void checkPluginLatency();
    // Tries again to load the effect plugins that didn't load (after a scan found them).
    void retryMissingPlugins();
    // Live MIDI from code (the on-screen keyboard), as if from the MIDI input. Any thread.
    void addLiveMidi (const juce::MidiMessage&) noexcept;
    // Live MIDI for one track's instrument only, armed or not (auditioning a pattern row).
    void addLiveMidi (const juce::MidiMessage&, int trackId) noexcept;

    //==============================================================================
    // Pattern mode (the step sequencer's "Pattern" switch). Message thread.
    //
    // With a pattern id, every track plays just that pattern, as a loop from 0 one pass
    // long (no clips or pattern blocks), and the playhead moves to its start; the song
    // position comes back when it goes back to Song mode (-1). Offline engines (exports,
    // Auto Mix, stems) never set it, so they always play the song. Mute, solo, inserts,
    // sends and the click work as usual. Removing the pattern goes back to Song mode.
    void setPatternMode (int patternIdOrNone);
    int getPatternMode() const noexcept { return patternMode; }

    // Pattern recording (the looper): startRecording() in pattern mode records the MIDI
    // input into the pattern's parts for the armed MIDI tracks while it loops, pass after
    // pass. Each note goes into the pattern as soon as it ends (so it plays from the next
    // pass), its time wrapped into the pattern and, with `snap`, moved to the nearest step;
    // drum parts get one-step notes. A note already on that step isn't added twice.
    // stopRecording() or finishPatternRecording() (which keeps playing) makes everything
    // recorded one undo step. Armed audio tracks don't record in pattern mode. Message thread.
    void setPatternSnap (bool snap) noexcept { patternSnap = snap; }
    bool getPatternSnap() const noexcept     { return patternSnap; }
    bool isPatternRecording() const noexcept { return patternRecording.load(); }
    int finishPatternRecording(); // returns how many notes were recorded
    // Adds what has been played since the last call (the timer calls it). Public for tests.
    void collectPatternNotes();
    // Note numbers being played on the MIDI input right now (for the piano roll's keys).
    std::bitset<128> getHeldNotes() const noexcept;

    // Click on every beat at the project tempo while playing (accent on the first beat
    // of each bar, from the Edit's time signature).
    void setMetronome (bool on) noexcept    { metronome = on; }
    bool getMetronome() const noexcept      { return metronome.load(); }
    // Click loudness, 0 (silent) .. 1 (the default) .. 2.
    void setMetronomeVolume (float v) noexcept { metronomeVolume = juce::jlimit (0.0f, 2.0f, v); }
    float getMetronomeVolume() const noexcept  { return metronomeVolume.load(); }
    // Library preview: plays a decoded sound (at the engine's sample rate, one or two
    // channels) once, straight to the output, on top of whatever plays. It is not part of
    // the mix, a recording or an export. Message thread; a new preview replaces the last.
    // `loop`: it repeats until stopped; `delaySamples`: it starts that much later (a loop
    // previewed in tempo starts on the next bar while the song plays).
    void startPreview (juce::AudioBuffer<float>&& audio, float gain, bool loop = false, int delaySamples = 0);
    void stopPreview();
    void setPreviewGain (float gain) noexcept   { previewGain = juce::jlimit (0.0f, 2.0f, gain); }
    bool isPreviewing() const noexcept          { return previewPlaying.load(); }

    // Bars of click before recording starts (0 = none). The click plays during the
    // count-in even when the metronome is off.
    void setCountInBars (int bars) noexcept { countInBars = juce::jlimit (0, 4, bars); }
    int getCountInBars() const noexcept     { return countInBars.load(); }

    // Message thread. Peak level (linear) of a track's (or a return's) output, or the
    // master's with Edit::masterId, since the last call: left and right, or the louder.
    std::pair<float, float> takePeaks (int trackId);
    float takePeak (int trackId) { const auto [l, r] = takePeaks (trackId); return juce::jmax (l, r); }

    // Message thread. A track's (or the master's) insert chain: its peak levels (linear)
    // after the input gain and after the output gain since the last call, and each
    // insert's module by insert id, for the UI's live pictures.
    std::pair<float, float> takeChainPeaks (int trackId);
    void forEachInsert (int trackId, const std::function<void (int insertId, fx::Module&)>& visit);

    //==============================================================================
    // Audio thread. Renders numSamples into the first numChannels of `output`
    // (overwriting them) and advances the playhead while playing. Inserts keep running
    // while stopped, so reverb and echo tails ring out.
    // `inputs` are the audio interface's inputs (numInputs of them, numSamples long),
    // which tracks pick from (Edit::getTrackInput). They may alias output channels:
    // each chunk is read before it's overwritten.
    // `midi` is the MIDI input for this block (timestamps 0..numSamples), or null.
    void process (juce::AudioBuffer<float>& output, int numChannels, int numSamples,
                  const float* const* inputs, int numInputs, const juce::MidiBuffer* midi) noexcept;
    // The same with one input (a mono mic), or none when `input` is null.
    void process (juce::AudioBuffer<float>& output, int numChannels, int numSamples,
                  const float* input = nullptr, const juce::MidiBuffer* midi = nullptr) noexcept;

private:
    // A track's (or the master's) chain and mixer state. Shared between consecutive
    // graphs, so DSP state survives clip edits. Its audio-thread members are only used
    // by whichever graph is active.
    struct Strip
    {
        int id = 0;
        juce::String insertLayout; // insert ids in order: a different layout needs a new strip
        fx::FxChain chain;
        std::map<int, fx::Module*> modulesByInsertId;
        std::map<int, juce::ValueTree> insertTrees; // the INSERT each module was made for
        std::vector<fx::modules::Pitch*> pitches; // its autotune / harmonizer inserts
        int latencyAtBuild = 0; // the chain's delay when the current graph was built (message thread)

        std::atomic<float> targetLeft { 1.0f }, targetRight { 1.0f };
        std::atomic<float> targetDirect { 1.0f }; // tracks: 0 while a return is soloed (sends still play)
        std::atomic<float> peakLeft { 0.0f }, peakRight { 0.0f };
        std::atomic<bool> monitoring { false }; // mic in (audio tracks) or MIDI in (MIDI tracks)
        std::atomic<int> inputLeft { 0 }, inputRight { Edit::noInput }; // audio tracks: Edit::getTrackInput

        // MIDI tracks: the instrument (null if it didn't load) and what it was made from.
        InstrumentPtr instrument;
        juce::String instrumentError;
        juce::ValueTree instrumentTree;
        bool isMidi = false;

        // Effect plugin inserts, by insert id: the module (in `chain`) and its INSERT, or why
        // it didn't load (then the insert is left out of the chain and keeps its state).
        struct PluginSlot
        {
            PluginInsert* module = nullptr;
            juce::ValueTree tree;
            juce::String error;
        };
        std::map<int, PluginSlot> plugins;

        int getLatency() const { return chain.getLatencySamples() + (instrument != nullptr ? instrument->getLatencySamples() : 0); }

        // Audio thread.
        juce::SmoothedValue<float> left, right, direct;
        juce::AudioBuffer<float> buffer;
        juce::AudioBuffer<float> instrumentBuffer;
        juce::MidiBuffer midi;
        juce::MidiBuffer audition;          // addLiveMidi for this track, played next block
        std::bitset<128> sounding;          // notes the timeline has started and not ended
        juce::int64 nextBlock = -1;         // where playback would carry on; else it jumped
    };

    struct MidiEvent
    {
        juce::int64 time = 0; // timeline samples
        int note = 0, velocity = 0; // velocity 0 = note off
    };

    struct Clip
    {
        AudioPool::Audio audio;
        juce::int64 start = 0, offset = 0, length = 0, fadeIn = 0, fadeOut = 0;
        float gain = 1.0f;
    };

    // A send's level (linear, 0 when off or the track is muted). Shared between graphs
    // like strips, so moving a send knob doesn't need a new graph.
    struct SendLevel
    {
        std::atomic<float> target { 0.0f };
        juce::SmoothedValue<float> gain; // audio thread
    };

    // A delay line for latency compensation (audio thread).
    struct Delay
    {
        juce::AudioBuffer<float> line;
        int samples = 0, write = 0;

        void setDelay (int numSamples)
        {
            samples = numSamples;
            write = 0;
            if (samples > 0)
            {
                line.setSize (2, samples);
                line.clear();
            }
        }

        void process (juce::AudioBuffer<float>&, int numSamples) noexcept;
    };

    struct Send
    {
        std::shared_ptr<SendLevel> level;
        int bus = 0;     // index into Graph::returns
        bool pre = false;
    };

    struct Track
    {
        std::shared_ptr<Strip> strip;
        std::vector<Send> sends;
        int output = -1; // index into Graph::returns, or -1 for the master
        std::vector<Clip> clips;
        std::vector<MidiEvent> midiEvents; // MIDI tracks, in time order
        std::vector<std::pair<juce::int64, float>> ride; // volume automation: timeline sample, dB (Edit::RidePoint)

        // Effect automation (Edit::setInsertAutomation): each lane drives one value of one
        // of the strip's modules while playing, and puts back `base` when playback stops.
        struct Lane
        {
            fx::Module::Value* value = nullptr;
            std::vector<std::pair<juce::int64, float>> points; // timeline sample, value
            float base = 0.0f;
        };
        std::vector<Lane> lanes;
        bool automating = false;

        // Latency compensation, before the inserts (audio thread).
        Delay delay;
    };

    // A return track: what the tracks send it, through its own strip, into the master.
    struct Bus
    {
        std::shared_ptr<Strip> strip;
        std::vector<Send> sends; // group tracks only: into the plain returns, which come later
        Delay delay; // lines its output up with the slowest return (after its inserts)
    };

    struct Graph
    {
        std::vector<Track> tracks;
        std::vector<Bus> returns;
        std::shared_ptr<Strip> master;

        // What the tracks play straight into the master, delayed to line up with the
        // returns' effects when they add latency (otherwise unused).
        juce::AudioBuffer<float> direct;
        Delay directDelay;
        int blockSize = 512; // the strips' buffer size: the most one chunk can render
        bool fresh = true;   // not played yet: notes from the previous graph get their note-offs
        TempoMap tempo;      // for the click

        // Chord following for harmonizer inserts in Chords mode: the follower (shared
        // with the next graph, so it keeps listening across edits), what it listens to,
        // and the song key as a hint.
        std::shared_ptr<analysis::ChordFollower> chords;
        juce::AudioBuffer<float> listen;
        int keyMask = 0;
    };

    void followChords (Graph&, bool isPlaying, juce::int64 blockStart, int numSamples) noexcept;

    // ValueTree::Listener
    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override;
    // Markers don't change what plays, so they don't need a new graph.
    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree& child) override        { if (! child.hasType (ids::MARKER)) requestRebuild(); }
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree& child, int) override { if (child.hasType (ids::CLIP)) previews.erase ((int) child[ids::id]); if (! child.hasType (ids::MARKER)) requestRebuild(); }
    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override         { requestRebuild(); }
    void valueTreeRedirected (juce::ValueTree&) override                          { previews.clear(); requestRebuild(); }

    void handleAsyncUpdate() override { rebuild(); }
    void timerCallback() override     { collectGarbage(); collectPreviews(); drainRecordedMidi(); collectPatternNotes(); syncCapture(); }

    void rebuild();
    void requestRebuild(); // realtime: on the next message loop pass; offline: before the next block
    std::shared_ptr<Strip> getStrip (const juce::ValueTree& trackOrMaster);
    static void loadInsertFile (fx::Module&, const juce::ValueTree& insert);
    void updateMix();
    void setStripTargets (Strip&, const juce::ValueTree&, bool audible, bool jump);

    void publish (std::unique_ptr<Graph>);
    void collectGarbage();
    void collectPreviews();
    void renderPreview (juce::AudioBuffer<float>& output, int numChannels, int numSamples) noexcept;
    void retirePreview (juce::AudioBuffer<float>*) noexcept;

    static void renderClip (const Clip&, juce::AudioBuffer<float>&, juce::int64 blockStart, int numSamples) noexcept;
    void renderBlock (Graph&, juce::AudioBuffer<float>& output, int outputOffset, int numChannels, int numSamples,
                      const float* const* inputs, int numInputs, const juce::MidiBuffer* midi, int midiOffset) noexcept;
    void renderInstrument (Track&, bool isPlaying, bool fresh, juce::int64 blockStart, int numSamples,
                           const juce::MidiBuffer* midi, int midiOffset) noexcept;
    void recordMidi (const juce::MidiBuffer&, int midiOffset, int numSamples, juce::int64 blockStart) noexcept;
    void drainRecordedMidi();
    // A MIDI track's notes: its clips and the pattern blocks, or with `loopPattern` just
    // that pattern as a loop (pattern mode).
    static void buildMidiEvents (Track&, const juce::ValueTree& trackTree, double sampleRate, juce::int64& end,
                                 const TempoMap&, const juce::ValueTree& loopPattern);
    void renderClick (const TempoMap&, juce::AudioBuffer<float>&, int offset, int numChannels, int numSamples, juce::int64 blockStart) noexcept;
    void updateTempo();
    void updateLoop();

    Edit& edit;
    const bool realtime;
    bool rebuildPending = false; // offline only (see requestRebuild)
    std::atomic<int> rebuildCount { 0 };
    AudioPool pool;

    double sampleRate = 48000.0;
    int blockSize = 512;

    std::map<int, std::shared_ptr<Strip>> strips; // message thread: the current strip per id

    // While rebuild() runs: every insert module of the graph being replaced, by insert id,
    // with the INSERT it belongs to. A new chain takes over the module of an INSERT that
    // is still there (on any track: a moved insert keeps running), instead of a fresh one.
    struct RunningInsert
    {
        std::shared_ptr<fx::Module> module;
        juce::ValueTree tree;
    };
    std::map<int, RunningInsert> runningInserts;
    std::shared_ptr<fx::Module> takeRunningInsert (const juce::ValueTree& insert);
    std::map<int, AudioPool::Audio> lastClipAudio;
    std::map<int, warp::Map> lastClipMap; // the map of what each clip played in the last graph

    struct Preview
    {
        std::optional<warp::Map> map;
        std::optional<std::vector<analysis::pitch::Note>> notes;
    };
    std::map<int, Preview> previews; // by clip id
    std::map<std::pair<int, int>, std::shared_ptr<SendLevel>> sendLevels; // message thread: by (track id, return id) // message thread: what each clip played in the last graph

    std::atomic<Graph*> incoming { nullptr }; // built, not yet picked up by the audio thread
    Graph* active = nullptr;                  // audio thread

    // Graphs the audio thread has replaced, freed on the message thread.
    static constexpr int retiredCapacity = 64;
    juce::AbstractFifo retiredFifo { retiredCapacity };
    std::array<Graph*, retiredCapacity> retired {};

    std::shared_ptr<analysis::ChordFollower> chordFollower; // message thread: the current one, for new graphs
    std::atomic<int> harmonyChord { -1 };

    // The library preview: handed over like the graphs, and freed on the message thread.
    std::atomic<juce::AudioBuffer<float>*> previewIncoming { nullptr };
    juce::AudioBuffer<float>* previewActive = nullptr; // audio thread
    int previewPosition = 0;                           // audio thread
    bool previewLooping = false;                       // audio thread
    int previewDelay = 0;                              // audio thread
    std::atomic<bool> previewLoopIncoming { false };   // set with previewIncoming
    std::atomic<int> previewDelayIncoming { 0 };
    std::atomic<bool> previewStop { false }, previewPlaying { false };
    std::atomic<float> previewGain { 1.0f };
    static constexpr int previewRetiredCapacity = 8;
    juce::AbstractFifo previewRetiredFifo { previewRetiredCapacity };
    std::array<juce::AudioBuffer<float>*, previewRetiredCapacity> previewRetired {};

    std::atomic<bool> playing { false };
    std::atomic<juce::int64> position { 0 }, length { 0 };
    std::atomic<int> latency { 0 };
    std::shared_ptr<bool> alive = std::make_shared<bool> (true);

    // Audio takes: one recorder per input the armed tracks use. Made when first needed
    // (each has a disk thread) and kept; the audio thread writes those that are on.
    struct TakeInput
    {
        std::unique_ptr<Recorder> recorder;
        Edit::Input input;
        std::atomic<bool> on { false };
    };
    static constexpr int maxTakes = 16;
    std::array<TakeInput, maxTakes> takes;
    std::atomic<int> numTakes { 0 };
    struct CaptureSlot
    {
        std::atomic<int> left { Edit::noInput }, right { Edit::noInput };
        CaptureRing ring;
    };
    std::array<CaptureSlot, maxCaptureInputs> captures;
    std::atomic<double> captureLength { defaultCaptureSeconds };
    void syncCapture();

    std::atomic<bool> recordingAudio { false };
    std::vector<float> silence = std::vector<float> (8192, 0.0f); // a missing input (prepare() makes it a block long)
    std::atomic<bool> countingIn { false };
    std::atomic<juce::int64> recordStart { 0 };
    std::atomic<bool> loopRecording { false }; // recording inside the loop: playback wraps, each pass a take
    std::atomic<juce::int64> recordingLatency { 0 }, midiRecordingLatency { 0 };

    // MIDI recording: the audio thread pushes what's played (note on/off, sustain pedal)
    // with its timeline time; the message thread collects it.
    struct RecordedMidi { juce::int64 time; juce::uint8 status, data1, data2; };
    static constexpr int midiFifoCapacity = 8192;
    juce::AbstractFifo midiFifo { midiFifoCapacity };
    std::array<RecordedMidi, midiFifoCapacity> midiFifoData {};
    std::vector<RecordedMidi> recordedMidi; // message thread
    std::atomic<bool> recordingMidi { false };

    // Live MIDI from addLiveMidi(), merged into the next block.
    static constexpr int liveFifoCapacity = 512;
    juce::AbstractFifo liveFifo { liveFifoCapacity };
    std::array<juce::uint32, liveFifoCapacity> liveFifoData {};
    juce::MidiBuffer liveMidi; // audio thread
    std::array<std::atomic<juce::uint32>, 4> heldNotes {}; // 128 bits, set by the audio thread

    // addLiveMidi for one track: (track id, packed message), handed to its strip.
    static constexpr int auditionFifoCapacity = 256;
    juce::AbstractFifo auditionFifo { auditionFifoCapacity };
    std::array<std::pair<int, juce::uint32>, auditionFifoCapacity> auditionFifoData {};

    // Pattern mode: the pattern (message thread), the song position to go back to, and the
    // loop's length in samples (0 when off) for the audio thread.
    int patternMode = -1;
    juce::int64 songPosition = 0;
    std::atomic<juce::int64> patternLoopEnd { 0 };

    // Pattern recording (message thread, apart from the flag): the tracks it records for,
    // the notes being held (beat, velocity; beat < 0 when not held), the sustain pedal,
    // and the notes added so far (made one undo step at the end).
    std::atomic<bool> patternRecording { false };
    bool patternSnap = true;
    juce::Array<int> patternTracks;
    std::array<std::pair<double, int>, 128> patternHeld {};
    std::bitset<128> patternSustained;
    bool patternPedal = false;
    std::vector<int> patternRecorded;
    void addPatternNote (int pitch, double onBeat, double offBeat, int velocity);

    std::atomic<bool> loopOn { false };
    std::atomic<juce::int64> loopStart { 0 }, loopEnd { 0 };

    std::atomic<bool> metronome { false };
    std::atomic<int> countInBars { 0 };
    std::atomic<int> beatsPerBar { 4 };
    std::atomic<float> metronomeVolume { 1.0f };
    float clickPhase = 0.0f, clickIncrement = 0.0f, clickLevel = 0.0f, clickDecay = 0.0f; // audio thread

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DawEngine)
};
}
