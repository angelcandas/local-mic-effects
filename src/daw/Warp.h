#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include "analysis/Pitch.h"

#include <functional>
#include <memory>
#include <vector>

// Time warp and transpose for audio clips, rendered offline.
//
// A clip's recording is never changed. A warped or transposed clip plays a rendered
// copy of its file instead: the file run through a phase vocoder that follows the warp
// map (where each moment of the file lands) and shifts the pitch. Clip offsets and
// lengths are measured in that rendered copy, so trimming, splitting and moving a warped
// clip work exactly as for a plain one.
//
// Every algorithm stretches all channels of a file identically (the timing decisions are
// taken on the channels together), so a stereo or multi-mic recording stays in phase.
//
// Plain computation, no audio-thread constraints: rendering runs on the AudioPool's
// background thread.
namespace daw::warp
{
// Moves one moment of the file: `source` seconds into the file plays at `target` seconds
// into the rendered copy.
struct Marker
{
    double source = 0.0, target = 0.0;
    bool operator== (const Marker& other) const noexcept { return juce::exactlyEqual (source, other.source) && juce::exactlyEqual (target, other.target); }
};

// A piecewise-linear map from file time to rendered time, through the markers. Before
// the first marker and after the last, time runs at normal speed (so one marker just
// slides the audio). No markers: the identity.
class Map
{
public:
    Map() = default;

    // Sorts the markers and drops any that would make time run backwards or stall, or
    // stretch a stretch between two markers by more than 8x either way.
    explicit Map (std::vector<Marker>);

    const std::vector<Marker>& getMarkers() const noexcept { return markers; }
    bool isIdentity() const noexcept;

    double toTarget (double sourceSeconds) const noexcept;
    double toSource (double targetSeconds) const noexcept;

    // How fast the file plays at a moment of the rendered copy (1 = normal, 0.5 = half speed).
    double getRateAt (double targetSeconds) const noexcept;

    // "source:target;source:target..." in seconds, for the project file. Empty for the identity.
    juce::String toString() const;
    static Map fromString (const juce::String&);

    bool operator== (const Map& other) const noexcept { return markers == other.markers; }

    static constexpr double maxStretch = 8.0;

private:
    std::vector<Marker> markers;
};

// How a clip's audio is stretched and shifted. Each suits a different kind of source:
//   mix        full mixes, chords, pads, loops: a phase vocoder with a long window and
//              phase locking, so harmonies stay clean (the default)
//   solo       voices and solo instruments: the same window with twice the frames, so
//              a single line's glides and consonants stay smooth
//   drums      drums, plucked and percussive parts: a short window, and phases start
//              again at every attack so hits stay sharp
//   efficient  quick edits and background parts: overlap-add in the time domain (WSOLA),
//              much less work, a little rougher on dense material
//   slice      no stretching at all: the pieces between markers are cut and moved, each
//              playing at its own speed, with short fades (gaps stay silent)
//   varispeed  like tape: time and pitch change together, no processing artefacts
enum class Algorithm { mix, solo, drums, efficient, slice, varispeed };

// "mix", "solo"... for the project file and the page. Unknown text gives mix.
juce::String getAlgorithmKey (Algorithm);
Algorithm algorithmFromKey (const juce::String&);
juce::StringArray getAlgorithmKeys();

// The map that does `inner` then `outer`: a moment of the file goes through inner, and
// where it lands goes through outer. Exact for these piecewise-linear maps.
Map compose (const Map& inner, const Map& outer);

// Everything that shapes a clip's rendered copy.
struct Settings
{
    Map map;
    double semitones = 0.0; // the clip's and its track's transpose together (fractions are cents)
    Algorithm algorithm = Algorithm::mix;
    bool formant = false;   // keep the voice's character (its formants) when transposing

    // Files rendered together as one (full paths, sorted, this clip's own among them):
    // linked tracks, such as the mics of one drum kit. Every channel of every file gets the
    // same timing and phase decisions, so the mics stay in phase with each other.
    juce::StringArray linked;

    // The take's sung notes with their edits (the note pitch editor), in file time. Only
    // matters when one is edited; the voice's formants are then always kept, and each note
    // can move them on its own. Not for linked files.
    std::vector<analysis::pitch::Note> notes;
    bool hasNoteEdits() const noexcept { return analysis::pitch::anyEdited (notes); }

    bool isNeutral() const noexcept { return map.isIdentity() && std::abs (semitones) < 1.0e-4 && ! hasNoteEdits(); }

    // Identifies the rendered copy (with the file's path) in the AudioPool's cache.
    juce::String getKey() const;

    static constexpr double maxSemitones = 24.0;
};

// Rendering in pieces.
//
// A render is cut at fixed seams of the source (quiet spots about every 1.5 s, chosen from
// the audio alone, so they stay put whatever the settings) into pieces, each identified
// by a key made only of what shapes that stretch of the output: the algorithm, the
// transpose, the map around it (relative to where the piece lands, so audio that only
// slides keeps its key) and the note edits over it. A cache of pieces (the AudioPool's)
// then re-renders only what an edit changed: moving one note renders the one to three
// pieces under it, and the rest of the take comes from the cache. Each piece starts a
// little early (so the vocoder has settled by the seam) and runs a little past its end,
// and neighbours crossfade over 8 ms at the seam.

// The seams of a recording: sample positions, the first 0 and the last its length,
// rising. Remembered for the last few recordings (by a sparse sample of the audio, like
// analysis::pitch::analyseCached). Thread-safe.
std::vector<int> findSeams (const juce::AudioBuffer<float>&, double sampleRate);
std::shared_ptr<const std::vector<int>> findSeamsCached (const juce::AudioBuffer<float>&, double sampleRate);

// One render of a recording with some settings, worked out ready to render piece by
// piece. Making it can take a moment on a long take with note edits (the take's pitch is
// analysed, once), so do it off the message thread. The source stays shared with it.
// Rendering pieces is thread-safe: several threads can render different pieces at once.
class Render
{
public:
    Render (std::shared_ptr<const juce::AudioBuffer<float>> source, double sampleRate, const Settings&);
    ~Render();

    int getLength() const noexcept { return length; } // samples of the finished copy
    int getNumChannels() const noexcept;

    // False when the render is cheap and done whole (neutral settings, levels only, slice
    // or varispeed without a pitch move): then renderWhole() makes it.
    bool isSegmented() const noexcept { return ! pieces.empty(); }

    struct Piece
    {
        double from = 0.0, to = 0.0; // where it lands in the copy, seconds (within the copy)
        juce::String key;            // what shapes it (unique for the source)
    };
    const std::vector<Piece>& getPieces() const noexcept { return pieces; }

    // One piece's audio, a little longer than its stretch (the crossfade into the next).
    // Empty when `shouldContinue` stopped it.
    juce::AudioBuffer<float> renderPiece (size_t index, const std::function<bool()>& shouldContinue = {}) const;

    // The finished copy from every piece (in order), joined with crossfades at the seams.
    juce::AudioBuffer<float> assemble (const std::vector<std::shared_ptr<const juce::AudioBuffer<float>>>&) const;

    // The finished copy in one go, for a render that isn't segmented (or as a reference).
    juce::AudioBuffer<float> renderWhole (const std::function<bool()>& shouldContinue = {}) const;

    static constexpr double seamSpacing = 1.5, seamSearch = 0.4, crossfadeSeconds = 0.008;

    struct Context;

private:
    std::shared_ptr<const Context> context;
    std::vector<Piece> pieces;
    std::vector<double> starts, ends; // each piece's exact landing times, seconds (may be outside the copy)
    int length = 0;
};

// The settings' render in one go, without pieces: what the pieces are checked against.
juce::AudioBuffer<float> renderUnsegmented (const juce::AudioBuffer<float>& source, double sampleRate, const Settings&,
                                            const std::function<bool()>& shouldContinue = {});

// Linked files as one buffer (their channels one after the other, as long as the longest),
// and a render of it split back into each file's copy.
juce::AudioBuffer<float> combineLinked (const std::vector<const juce::AudioBuffer<float>*>& sources);
std::vector<juce::AudioBuffer<float>> splitLinked (const juce::AudioBuffer<float>& rendered, const std::vector<const juce::AudioBuffer<float>*>& sources,
                                                   double sampleRate, const Settings&);

// Several files warped as one (see Settings::linked): the channels of all of them go
// through render() together, and each comes back at its own length.
std::vector<juce::AudioBuffer<float>> renderLinked (const std::vector<const juce::AudioBuffer<float>*>& sources, double sampleRate,
                                                    const Settings&, const std::function<bool()>& shouldContinue = {});

// The file's audio warped and transposed with the settings' algorithm, rendered in pieces
// (see Render) one after the other. Its length is the map's target for the file's
// end (silence where the map starts the audio later than 0). `shouldContinue` is polled
// now and then; false stops early and returns an empty buffer.
juce::AudioBuffer<float> render (const juce::AudioBuffer<float>& source, double sampleRate, const Settings&,
                                 const std::function<bool()>& shouldContinue = {});
}
