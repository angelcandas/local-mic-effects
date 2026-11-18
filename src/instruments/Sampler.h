#pragma once

#include "InstrumentCore.h"

// Sampler: plays one audio sample across the keyboard (Keys mode: the root note plays it
// as recorded, other keys pitch it, with an ADSR, a filter and loop points) or as slices
// (Slices mode: key 36 plays the first slice, 37 the second and so on). "Slice to MIDI"
// in the DAW loads a clip's audio here with the clip's hit points as slices, and writes
// a MIDI clip that plays them back.
//
//     if (auto* s = micfx::inst::asSampler (processor.getEngine())) s->load (file, 0.0, 0.0, error);
namespace micfx::inst
{
struct SamplerSample
{
    std::vector<float> left, right; // right is empty for a mono sample
    double sampleRate = 44100.0;
    juce::String name;
    juce::File file;
    double regionStart = 0.0, regionEnd = 0.0; // the part of the file held, in file seconds
    std::vector<float> peaks;                  // 0..1, for the window

    int length() const noexcept { return (int) left.size(); }
    double seconds() const noexcept { return sampleRate > 0.0 ? (double) left.size() / sampleRate : 0.0; }
};

class SamplerInstrument
{
public:
    static constexpr int firstSliceNote = 36;
    static constexpr int maxSlices = 64;
    static constexpr double maxSampleSeconds = 60.0;

    virtual ~SamplerInstrument() = default;

    // Decodes the file (the part from regionStart to regionEnd, in file seconds; both 0:
    // all of it) on this thread and swaps it in. Slices are cleared. Safe while playing.
    virtual bool load (const juce::File&, double regionStart, double regionEnd, juce::String& error) = 0;
    virtual void clear() = 0;
    virtual std::shared_ptr<const SamplerSample> getSample() const = 0;

    // The slice starts in seconds from the start of the sample, ascending (at most maxSlices;
    // the first is the sample's start). Empty: no slices.
    virtual void setSlices (std::vector<double> starts) = 0;
    virtual std::vector<double> getSlices() const = 0;

    // The key's slice number (0-based), or -1 out of range.
    static int sliceForNote (int note) noexcept { return note - firstSliceNote; }
};

SamplerInstrument* asSampler (Engine&);
}
