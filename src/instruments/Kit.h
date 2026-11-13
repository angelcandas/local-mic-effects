#pragma once

#include "InstrumentCore.h"

// Kit: a sampler drum kit. Sixteen pads on the General MIDI drum notes 36-51, each with
// a sample the user loads (a file on disk), its own level, pan, tune, decay and choke
// group. No samples ship with it: the kit starts empty.
//
// The window, the DAW screen and the tests reach the samples through KitSampler:
//     if (auto* kit = micfx::inst::asKit (processor.getEngine())) kit->loadSample (0, file, error);
namespace micfx::inst
{
// One loaded sample: decoded to float (at most 2 channels, at most maxSampleSeconds long).
struct KitSample
{
    std::vector<float> left, right; // right is empty for a mono sample
    double sampleRate = 44100.0;
    juce::String name;              // the file name without its extension
    juce::File file;
    std::vector<float> peaks;       // for the window's waveforms (0..1)

    int length() const noexcept { return (int) left.size(); }
    double seconds() const noexcept { return sampleRate > 0.0 ? (double) left.size() / sampleRate : 0.0; }
};

class KitSampler
{
public:
    static constexpr int numPads = 16;
    static constexpr int firstNote = 36;
    static constexpr double maxSampleSeconds = 20.0;

    virtual ~KitSampler() = default;

    // Decodes the file on this thread and swaps it into the pad. Safe while playing.
    virtual bool loadSample (int pad, const juce::File& file, juce::String& error) = 0;
    virtual void clearPad (int pad) = 0;
    virtual std::shared_ptr<const KitSample> getSample (int pad) const = 0;

    // The file a pad was loaded from, also when it can't be found any more (then no sample).
    virtual juce::File getPadFile (int pad) const = 0;

    // The pad's brightness for its flash: set to the velocity when it is struck, taken once.
    virtual float takeFlash (int pad) = 0;

    static int padForNote (int note) noexcept { return note - firstNote; }
    static int noteForPad (int pad) noexcept  { return firstNote + pad; }
    static bool isPad (int pad) noexcept      { return pad >= 0 && pad < numPads; }

    // The General MIDI drum name of a pad's note ("Kick", "Snare", ...), shown on empty pads.
    static juce::String hintName (int pad);
};

KitSampler* asKit (Engine&);

// Where to look for a sample whose saved path is gone (the project's folder, say): a file
// of the same name is searched in these folders (and below them). Any thread.
void setKitSearchFolders (const juce::Array<juce::File>&);
}
