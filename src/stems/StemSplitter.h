#pragma once

#include <juce_audio_formats/juce_audio_formats.h>

#include <array>
#include <functional>
#include <memory>
#include <vector>

// Splits a song into four stems (drums, bass, other, vocals) with Demucs v4, the hybrid
// transformer model from Meta AI Research, run on the CPU by demucs.cpp (libs/demucs.cpp,
// MIT) on Eigen (libs/eigen, MPL 2.0). The weights (MIT) aren't in the app: they download
// once (about 84 MB) to the user's Application Support folder, and are checked against
// their SHA-256.
//
// The song is cut into overlapping chunks that several threads separate at once, then
// crossfaded back together. Everything here runs on a background thread; progress
// callbacks return false to stop.
namespace stems
{
// Demucs' output order.
enum class Stem { drums, bass, other, vocals };
inline constexpr int numStems = 4;
inline constexpr std::array<const char*, numStems> stemNames { "Drums", "Bass", "Other", "Vocals" };

inline constexpr double modelSampleRate = 44100.0; // what the model was trained on

// Progress 0..1; return false to cancel.
using Progress = std::function<bool (double)>;

// Separates one stereo chunk at modelSampleRate into numStems stereo buffers of the same
// length. Must be safe to call from several threads at once. Reports the chunk's own
// progress; returns an empty vector if progress asked it to stop.
using Separator = std::function<std::vector<juce::AudioBuffer<float>> (const juce::AudioBuffer<float>&, const Progress&)>;

//==============================================================================
// The Demucs weights.
struct Model
{
    static constexpr const char* fileName = "ggml-model-htdemucs-4s-f16.bin";
    static constexpr const char* downloadUrl = "https://huggingface.co/datasets/Retrobear/demucs.cpp/resolve/main/ggml-model-htdemucs-4s-f16.bin";
    static constexpr const char* sha256 = "72b17c42d308982ddb5069bc3bf48b81a5aac4cb6516e4366c0fa7cef6df0064";
    static constexpr int approxMegabytes = 84;

    // True when this build includes demucs.cpp (see CMakeLists.txt).
    static bool isBuiltIn();

    // ~/Library/Application Support/MicFX/Models/<fileName> (or the platform's equivalent).
    static juce::File getDefaultFile();

    // Empty if the file is the model we expect, or why not.
    static juce::String verify (const juce::File&);

    // Downloads to `destination` (via a .part file), then verifies it. Empty on success.
    static juce::String download (const juce::File& destination, const Progress&);

    // A separator that runs the model in `file`; null with `error` set if it can't load.
    // Loading takes a second or so and about 300 MB.
    static Separator load (const juce::File& file, juce::String& error);
};

//==============================================================================
// The same network on the GPU. On a Mac, Core ML runs the Demucs network (everything between
// the spectrogram and the final mask) on the GPU. The converted model isn't downloaded:
// The converted model downloads once (about 70 MB) next to the CPU one, like it. (The
// workflow and scripts/export_htdemucs_coreml.py make it.) Without it, or where Core ML isn't
// there, the CPU model above is used.
struct GpuModel
{
    static constexpr const char* fileName = "htdemucs-core.mlpackage";

    // The converted network, zipped, published by the "Core ML stem model" workflow
    // (.github/workflows/coreml-model.yml) as a release asset.
    static constexpr const char* downloadUrl = "https://github.com/angelcandas/local-mic-effects/releases/download/stem-model-v1/htdemucs-core.mlpackage.zip";
    static constexpr const char* sha256 = "0000000000000000000000000000000000000000000000000000000000000000";
    static constexpr int approxMegabytes = 70;

    // True when this build can run Core ML (macOS builds with the stem splitter).
    static bool isBuiltIn();

    // ~/Library/Application Support/MicFX/Models/htdemucs-core.mlpackage
    static juce::File getDefaultFile();

    // Downloads the converted model to `destination` (the .mlpackage folder), checks it against its
    // SHA-256 and unpacks it. Empty on success, "cancelled" if progress stopped it.
    static juce::String download (const juce::File& destination, const Progress&);

    // The unpacking half of download(): `zip` must match the checksum.
    static juce::String install (const juce::File& zip, const juce::File& destination, const juce::String& expectedSha256 = sha256);

    // A separator that runs the converted model on the GPU; null with `error` set if it
    // can't (the model is missing or doesn't load or run). Loading compiles the model the
    // first time and warms the GPU up with one prediction, which can take a minute.
    static Separator load (const juce::File& file, juce::String& error);
};

//==============================================================================
struct Settings
{
    int threads = 0;            // 0: up to 4, one per physical core
    double chunkSeconds = 30.0; // each thread separates this much at a time
    double overlapSeconds = 1.0; // crossfaded between chunks
    int bitDepth = 24;
};

struct Result
{
    std::array<juce::File, numStems> files; // in Stem order, when it worked
    juce::String error;
    bool cancelled = false;
};

// Separates `source` and writes "<name> - Vocals.wav" etc. into `folder`, at
// modelSampleRate (the DAW resamples them like any other file). Files are only left
// behind when every stem was written.
Result split (const juce::File& source, const juce::File& folder, const juce::String& name,
              const Separator&, const Progress&, const Settings& = {});

// The core of split(), on audio already in memory at modelSampleRate (stereo).
// Returns the numStems stems, or an empty vector when cancelled.
std::vector<juce::AudioBuffer<float>> separate (const juce::AudioBuffer<float>& stereo, const Separator&,
                                                const Progress&, const Settings& = {});
}
