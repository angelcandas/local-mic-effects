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
// Demucs' output order. The four-stem model gives the first four; the six-stem one adds guitar and piano.
enum class Stem { drums, bass, other, vocals, guitar, piano };
inline constexpr int numStems = 4; // what the four-stem network (and so the Core ML one) produces
inline constexpr int maxStems = 6;
inline constexpr std::array<const char*, maxStems> stemNames { "Drums", "Bass", "Other", "Vocals", "Guitar", "Piano" };

// How a song is separated: the standard four-stem model, the six-stem one (guitar and piano come
// out of "Other"; CPU only, a little less clean on the other four), or the four fine-tuned models
// (one per stem, about four times slower, cleanest; CPU only).
enum class Quality { fourStems, sixStems, best };
inline constexpr int numQualities = 3;
inline constexpr std::array<const char*, numQualities> qualityKeys { "4", "6", "best" };
inline constexpr std::array<const char*, numQualities> qualityLabels { "4 stems", "6 stems", "Best quality" };
inline int getStemCount (Quality q) { return q == Quality::sixStems ? 6 : 4; }
inline Quality getQualityFromKey (const juce::String& key)
{
    for (int i = 0; i < numQualities; ++i)
        if (key == qualityKeys[(size_t) i])
            return (Quality) i;
    return Quality::fourStems;
}

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

    // One weights file of a quality level; all come from the same dataset as the four-stem one.
    struct WeightFile { const char* fileName; const char* sha256; int megabytes; };
    static constexpr const char* baseUrl = "https://huggingface.co/datasets/Retrobear/demucs.cpp/resolve/main/";

    // The files `quality` needs: one, or four for Best quality (drums, bass, other, vocals, in that order).
    static std::vector<WeightFile> getWeightFiles (Quality);
    static juce::File getFile (const WeightFile&);
    static juce::String verifyFile (const juce::File&, const WeightFile&);
    // Downloads one file (via .part), verified. Empty on success.
    static juce::String downloadFile (const WeightFile&, const Progress&);
    // Every file of `quality` is downloaded and verified.
    static bool isDownloaded (Quality);
    static int getMegabytes (Quality);

    // A separator for `quality` (its files must be downloaded); null with `error` set if it can't load.
    // Best quality holds four models (about 1.2 GB).
    static Separator loadQuality (Quality, juce::String& error);

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
    static constexpr const char* sha256 = "00593052d106894aef3a8b59e894379509ab600c886b44c3625db7f6252bbbb9";
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
// The best separator this machine has, downloading its model the first time: for four stems the
// GPU model where Core ML is built in, else (or when that fails) the CPU one. The six-stem and
// Best quality models run on the CPU only.
struct Loaded
{
    Separator separator;  // null when it couldn't (see error) or was cancelled
    bool gpu = false;
    bool cancelled = false;
    juce::String error;   // why there's no separator
    juce::String note;    // why the GPU wasn't used, when it was meant to be
};

enum class LoadPhase { downloading, loading };

// How closely `candidate` reproduces `reference` on a fixed test signal (one 7.8 s segment of
// tones and bursts): the signal-to-error ratio over all stems in dB, higher is closer. Minus
// infinity when `candidate` fails or returns the wrong shape. loadBest() uses it to check the
// GPU model against the CPU one on this Mac before trusting it, once per installed model.
double matchOnTestSignal (const Separator& candidate, const Separator& reference);
constexpr double minGpuMatchDb = 15.0; // a broken model sits around 0 dB; fp16 against fp32 is far above this

// Background thread. `onPhase` says what it's doing (progress restarts at each phase);
// progress returning false cancels.
Loaded loadBest (const std::function<void (LoadPhase)>& onPhase, const Progress&, Quality = Quality::fourStems);

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
    std::vector<juce::File> files; // in Stem order (as many as the model has), when it worked
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

//==============================================================================
// Rebalance: the stems of a file are kept (24-bit WAVs) per file content and quality, so
// changing their levels again later needs no new separation. The folder is
// <root>/<hash of the file>-<quality key>/ ("Stem - Drums.wav" ...), root defaulting to
// ~/Library/Application Support/MicFX/StemCache.
juce::File getDefaultCacheRoot();
juce::File getCacheFolder (const juce::File& source, Quality, const juce::File& root = {});

// The stem files in Stem order, or an empty vector unless all of them are there.
std::vector<juce::File> getCachedStems (const juce::File& folder, Quality);

// Separates `source` into its cache folder (or finds it there already, without separating) and
// returns the files. Oldest cache folders are deleted past about 4 GB.
Result splitToCache (const juce::File& source, Quality, const Separator&, const Progress&,
                     const Settings& = {}, const juce::File& root = {});

// Sums `files` (stems, equal length) scaled by `gainsDb` (one per file, -100 or less mutes) into a
// 32-bit float stereo WAV at the stems' own sample rate, a block at a time. Empty on success.
juce::String mixStems (const std::vector<juce::File>& files, const std::vector<float>& gainsDb,
                       const juce::File& destination, const Progress&);
}
