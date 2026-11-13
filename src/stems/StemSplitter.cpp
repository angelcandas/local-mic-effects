#include "StemSplitter.h"

#include <juce_cryptography/juce_cryptography.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <mutex>
#include <thread>

namespace stems
{
namespace
{
    // Stereo at modelSampleRate: mono is copied to both sides, more than two channels
    // keep the first two.
    juce::AudioBuffer<float> readForModel (juce::AudioFormatReader& reader)
    {
        const auto length = (int) reader.lengthInSamples;
        juce::AudioBuffer<float> source (2, length);
        reader.read (&source, 0, length, 0, true, true);
        if (reader.numChannels == 1)
            source.copyFrom (1, 0, source, 0, 0, length);

        if (juce::approximatelyEqual (reader.sampleRate, modelSampleRate))
            return source;

        const auto speed = reader.sampleRate / modelSampleRate;
        const auto outLength = (int) std::floor ((double) length / speed);
        juce::AudioBuffer<float> resampled (2, outLength);

        for (int ch = 0; ch < 2; ++ch)
        {
            juce::LagrangeInterpolator interpolator;
            interpolator.process (speed, source.getReadPointer (ch), resampled.getWritePointer (ch), outLength, length, 0);
        }

        return resampled;
    }

    // The weight of a chunk at sample i: 1 in its own part, ramping to 0 across the
    // overlap it shares with a neighbour, so neighbours crossfade.
    float chunkWeight (int i, int from, int to, int overlap, bool first, bool last)
    {
        auto w = 1.0f;
        if (! first && overlap > 0)
            w = juce::jmin (w, (float) (i - from) / (float) (2 * overlap));
        if (! last && overlap > 0)
            w = juce::jmin (w, (float) (to - i) / (float) (2 * overlap));
        return juce::jlimit (0.0f, 1.0f, w);
    }

    // Downloads `url` to `part` (replacing it), reporting progress up to 99%. Empty on success;
    // "cancelled" when progress asked to stop. Leaves no half file behind on failure.
    juce::String fetchFile (const juce::String& url, const juce::File& part, juce::int64 approxBytes,
                               const Progress& progress, const juce::String& what)
    {
        if (! part.getParentDirectory().createDirectory())
            return "Couldn't create " + part.getParentDirectory().getFullPathName() + ".";

        part.deleteFile();

        int status = 0;
        auto input = juce::URL (url).createInputStream (juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                                                            .withConnectionTimeoutMs (20000)
                                                            .withNumRedirectsToFollow (10)
                                                            .withStatusCode (&status));
        if (input == nullptr || (status != 0 && status != 200))
            return "Couldn't download " + what + " (" + (status != 0 ? "HTTP " + juce::String (status) : juce::String ("no connection"))
                 + "). Check the internet connection and try again.";

        const auto expected = input->getTotalLength() > 0 ? input->getTotalLength() : approxBytes;

        std::unique_ptr<juce::FileOutputStream> output (part.createOutputStream());
        if (output == nullptr)
            return "Couldn't write " + part.getFullPathName() + ".";

        juce::HeapBlock<char> block (1 << 16);
        juce::int64 received = 0;

        for (;;)
        {
            const auto n = input->read (block.get(), 1 << 16);
            if (n <= 0)
                break;

            if (! output->write (block.get(), (size_t) n))
            {
                output.reset();
                part.deleteFile();
                return "Couldn't write " + part.getFullPathName() + " (is the disk full?).";
            }

            received += n;
            if (progress != nullptr && ! progress (juce::jmin (0.99, (double) received / (double) expected)))
            {
                output.reset();
                part.deleteFile();
                return "cancelled";
            }
        }

        output.reset();
        return {};
    }
}

//==============================================================================
std::vector<juce::AudioBuffer<float>> separate (const juce::AudioBuffer<float>& stereo, const Separator& separator,
                                                const Progress& progress, const Settings& settings)
{
    const auto total = stereo.getNumSamples();
    const auto chunk = juce::jmax (1, (int) std::lround (settings.chunkSeconds * modelSampleRate));
    const auto overlap = juce::jmax (0, (int) std::lround (settings.overlapSeconds * modelSampleRate));
    const auto numChunks = juce::jmax (1, (total + chunk - 1) / chunk);

    // Chunk k owns [k * chunk, (k + 1) * chunk) and reads `overlap` either side of it.
    struct Piece { int from, to; std::vector<juce::AudioBuffer<float>> stems; };
    std::vector<Piece> pieces ((size_t) numChunks);
    for (int k = 0; k < numChunks; ++k)
        pieces[(size_t) k] = { juce::jmax (0, k * chunk - overlap), juce::jmin (total, (k + 1) * chunk + overlap), {} };

    std::vector<std::atomic<float>> done ((size_t) numChunks);
    for (auto& d : done)
        d = 0.0f;

    std::atomic<bool> cancelled { false };
    std::atomic<int> next { 0 };
    std::mutex progressLock;

    const auto report = [&]
    {
        double sum = 0.0;
        for (size_t k = 0; k < pieces.size(); ++k)
            sum += (double) done[k].load() * (pieces[k].to - pieces[k].from);

        double all = 0.0;
        for (const auto& p : pieces)
            all += p.to - p.from;

        const std::scoped_lock lock (progressLock);
        if (progress != nullptr && ! progress (all > 0.0 ? sum / all : 1.0))
            cancelled = true;
    };

    const auto work = [&]
    {
        for (int k; ! cancelled && (k = next++) < numChunks;)
        {
            auto& piece = pieces[(size_t) k];
            juce::AudioBuffer<float> input (2, piece.to - piece.from);
            for (int ch = 0; ch < 2; ++ch)
                input.copyFrom (ch, 0, stereo, ch, piece.from, input.getNumSamples());

            piece.stems = separator (input, [&, k] (double p)
            {
                done[(size_t) k] = (float) juce::jlimit (0.0, 1.0, p);
                report();
                return ! cancelled.load();
            });

            if (piece.stems.empty() || piece.stems.size() > (size_t) maxStems)
            {
                cancelled = true;
                break;
            }

            done[(size_t) k] = 1.0f;
            report();
        }
    };

    const auto threads = juce::jlimit (1, numChunks, settings.threads > 0 ? settings.threads
                                                                         : juce::jlimit (1, 4, juce::SystemStats::getNumPhysicalCpus()));
    std::vector<std::thread> pool;
    for (int t = 1; t < threads; ++t)
        pool.emplace_back (work);
    work();
    for (auto& t : pool)
        t.join();

    if (cancelled)
        return {};

    // Crossfade the chunks back together.
    const auto stemCount = pieces.front().stems.size();
    for (const auto& piece : pieces)
        if (piece.stems.size() != stemCount)
            return {};

    std::vector<juce::AudioBuffer<float>> result;
    for (size_t s = 0; s < stemCount; ++s)
    {
        result.emplace_back (2, total);
        result.back().clear();
    }

    std::vector<float> weightSum ((size_t) total, 0.0f);
    for (int k = 0; k < numChunks; ++k)
    {
        const auto& piece = pieces[(size_t) k];
        const auto length = piece.to - piece.from;

        for (int i = 0; i < length; ++i)
        {
            const auto w = chunkWeight (piece.from + i, piece.from, piece.to, overlap, k == 0, k == numChunks - 1);
            weightSum[(size_t) (piece.from + i)] += w;

            for (size_t s = 0; s < stemCount; ++s)
                for (int ch = 0; ch < 2; ++ch)
                    result[s].getWritePointer (ch)[piece.from + i]
                        += w * piece.stems[(size_t) s].getSample (ch, juce::jmin (i, piece.stems[(size_t) s].getNumSamples() - 1));
        }
    }

    for (int i = 0; i < total; ++i)
        if (const auto w = weightSum[(size_t) i]; w > 0.0f)
            for (auto& stem : result)
                for (int ch = 0; ch < 2; ++ch)
                    stem.getWritePointer (ch)[i] /= w;

    return result;
}

//==============================================================================
Result split (const juce::File& source, const juce::File& folder, const juce::String& name,
              const Separator& separator, const Progress& progress, const Settings& settings)
{
    Result result;

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    const std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (source));
    if (reader == nullptr || reader->lengthInSamples <= 0 || reader->sampleRate <= 0.0)
    {
        result.error = "Couldn't read " + source.getFileName() + ".";
        return result;
    }

    // Reading and resampling is the first 2%, writing the last 3%.
    const auto stage = [&progress] (double from, double to)
    {
        return [&progress, from, to] (double p) { return progress == nullptr || progress (from + (to - from) * p); };
    };

    const auto stereo = readForModel (*reader);
    if (! stage (0.0, 0.02) (1.0))
    {
        result.cancelled = true;
        return result;
    }

    const auto separated = separate (stereo, separator, stage (0.02, 0.97), settings);
    if (separated.empty())
    {
        result.cancelled = true;
        return result;
    }

    if (! folder.createDirectory())
    {
        result.error = "Couldn't create " + folder.getFullPathName() + ".";
        return result;
    }

    juce::WavAudioFormat wav;
    result.files.resize (separated.size());
    for (int s = 0; s < (int) separated.size(); ++s)
    {
        const auto file = folder.getNonexistentChildFile (juce::File::createLegalFileName (name + " - " + stemNames[(size_t) s]), ".wav", false);
        std::unique_ptr<juce::OutputStream> stream (file.createOutputStream());
        std::unique_ptr<juce::AudioFormatWriter> writer;

        if (stream != nullptr)
            writer = wav.createWriterFor (stream,
                                          juce::AudioFormatWriterOptions{}.withSampleRate (modelSampleRate)
                                                                          .withNumChannels (2)
                                                                          .withBitsPerSample (settings.bitDepth));

        const auto& audio = separated[(size_t) s];
        if (writer == nullptr || ! writer->writeFromAudioSampleBuffer (audio, 0, audio.getNumSamples()))
        {
            writer.reset();
            file.deleteFile();
            for (const auto& written : result.files)
                written.deleteFile();
            result.files.clear();
            result.error = "Couldn't write " + file.getFullPathName() + ".";
            return result;
        }

        result.files[(size_t) s] = file;
        stage (0.97, 1.0) ((s + 1.0) / (double) separated.size());
    }

    return result;
}

//==============================================================================
juce::File getDefaultCacheRoot()
{
    return Model::getDefaultFile().getParentDirectory().getChildFile ("StemCache");
}

juce::File getCacheFolder (const juce::File& source, Quality quality, const juce::File& root)
{
    const auto hash = juce::SHA256 (source).toHexString().substring (0, 20);
    return (root == juce::File() ? getDefaultCacheRoot() : root)
        .getChildFile (hash + "-" + qualityKeys[(size_t) quality]);
}

std::vector<juce::File> getCachedStems (const juce::File& folder, Quality quality)
{
    std::vector<juce::File> files;
    for (int s = 0; s < getStemCount (quality); ++s)
    {
        const auto file = folder.getChildFile (juce::String ("Stem - ") + stemNames[(size_t) s] + ".wav");
        if (! file.existsAsFile() || file.getSize() < 64)
            return {};
        files.push_back (file);
    }
    return files;
}

namespace
{
    void pruneCache (const juce::File& root, const juce::File& keep, juce::int64 maxBytes)
    {
        auto folders = root.findChildFiles (juce::File::findDirectories, false);
        const auto sizeOf = [] (const juce::File& folder)
        {
            juce::int64 total = 0;
            for (const auto& file : folder.findChildFiles (juce::File::findFiles, false))
                total += file.getSize();
            return total;
        };

        juce::int64 total = 0;
        for (const auto& folder : folders)
            total += sizeOf (folder);

        std::sort (folders.begin(), folders.end(), [] (const juce::File& a, const juce::File& b)
                   { return a.getLastModificationTime() < b.getLastModificationTime(); });

        for (const auto& folder : folders)
        {
            if (total <= maxBytes)
                break;
            if (folder == keep)
                continue;
            total -= sizeOf (folder);
            folder.deleteRecursively();
        }
    }
}

Result splitToCache (const juce::File& source, Quality quality, const Separator& separator, const Progress& progress,
                     const Settings& settings, const juce::File& rootArgument)
{
    const auto root = rootArgument == juce::File() ? getDefaultCacheRoot() : rootArgument;
    const auto folder = getCacheFolder (source, quality, root);

    Result result;
    result.files = getCachedStems (folder, quality);
    if (! result.files.empty())
    {
        folder.setLastModificationTime (juce::Time::getCurrentTime()); // used lately: pruned last
        if (progress != nullptr)
            progress (1.0);
        return result;
    }

    if (separator == nullptr)
    {
        result.error = "The stem splitter isn't loaded.";
        return result;
    }

    const auto temp = folder.getSiblingFile (folder.getFileName() + ".tmp");
    temp.deleteRecursively();
    result = split (source, temp, "Stem", separator, progress, settings);

    if (result.cancelled || result.error.isNotEmpty())
    {
        temp.deleteRecursively();
        result.files.clear();
        return result;
    }

    folder.deleteRecursively();
    if (! temp.moveFileTo (folder))
    {
        temp.deleteRecursively();
        result.files.clear();
        result.error = "Couldn't save the stems to " + folder.getFullPathName() + ".";
        return result;
    }

    result.files = getCachedStems (folder, quality);
    if (result.files.empty())
        result.error = "The separated stems couldn't be saved.";

    pruneCache (root, folder, (juce::int64) 4 * 1000 * 1000 * 1000);
    return result;
}

juce::String mixStems (const std::vector<juce::File>& files, const std::vector<float>& gainsDb,
                       const juce::File& destination, const Progress& progress)
{
    if (files.empty() || gainsDb.size() != files.size())
        return "There are no stems to mix.";

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();

    std::vector<std::unique_ptr<juce::AudioFormatReader>> readers;
    std::vector<float> gains;
    for (size_t i = 0; i < files.size(); ++i)
    {
        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (files[i]));
        if (reader == nullptr || reader->numChannels < 1)
            return "Couldn't read the stem " + files[i].getFileName() + ".";
        readers.push_back (std::move (reader));
        gains.push_back (gainsDb[i] <= -100.0f ? 0.0f : juce::Decibels::decibelsToGain (juce::jlimit (-100.0f, 24.0f, gainsDb[i])));
    }

    const auto rate = readers.front()->sampleRate;
    auto length = readers.front()->lengthInSamples;
    for (const auto& reader : readers)
        length = juce::jmin (length, reader->lengthInSamples);

    destination.getParentDirectory().createDirectory();
    destination.deleteFile();
    std::unique_ptr<juce::OutputStream> stream (destination.createOutputStream());
    if (stream == nullptr)
        return "Couldn't write " + destination.getFullPathName() + ".";

    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatWriter> writer (wav.createWriterFor (stream, juce::AudioFormatWriterOptions{}.withSampleRate (rate)
                                                                                                                .withNumChannels (2)
                                                                                                                .withBitsPerSample (32)));
    if (writer == nullptr)
        return "Couldn't write " + destination.getFullPathName() + ".";

    constexpr int block = 1 << 16;
    juce::AudioBuffer<float> sum (2, block), part (2, block);

    for (juce::int64 at = 0; at < length; at += block)
    {
        const auto n = (int) juce::jmin ((juce::int64) block, length - at);
        sum.clear();

        for (size_t i = 0; i < readers.size(); ++i)
        {
            if (gains[i] == 0.0f)
                continue;

            part.clear();
            readers[i]->read (&part, 0, n, at, true, true);
            if (readers[i]->numChannels == 1)
                part.copyFrom (1, 0, part, 0, 0, n);
            for (int ch = 0; ch < 2; ++ch)
                sum.addFrom (ch, 0, part, ch, 0, n, gains[i]);
        }

        if (! writer->writeFromAudioSampleBuffer (sum, 0, n))
        {
            writer.reset();
            destination.deleteFile();
            return "Couldn't write " + destination.getFullPathName() + " (is the disk full?).";
        }

        if (progress != nullptr && ! progress ((double) (at + n) / (double) length))
        {
            writer.reset();
            destination.deleteFile();
            return "cancelled";
        }
    }

    writer.reset();
    return {};
}

//==============================================================================
juce::File Model::getDefaultFile()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
#if JUCE_MAC
        .getChildFile ("Application Support")
#endif
        .getChildFile ("MicFX").getChildFile ("Models").getChildFile (fileName);
}

juce::String Model::verifyFile (const juce::File& file, const WeightFile& weights)
{
    if (! file.existsAsFile())
        return "The stem model isn't downloaded yet.";

    if (juce::SHA256 (file).toHexString() != juce::String (weights.sha256))
        return file.getFileName() + " isn't the expected Demucs model (its checksum doesn't match).";

    return {};
}

juce::String Model::verify (const juce::File& file)
{
    return verifyFile (file, getWeightFiles (Quality::fourStems).front());
}

std::vector<Model::WeightFile> Model::getWeightFiles (Quality quality)
{
    switch (quality)
    {
        case Quality::sixStems:
            return { { "ggml-model-htdemucs-6s-f16.bin", "09704f4ceae204e56e77d5eefd6ac71d7275be81fd507e6913371d59abcee856", 55 } };
        case Quality::best:
            // The fine-tuned models, one per stem: drums, bass, other, vocals.
            return { { "ggml-model-htdemucs_ft_drums-4s-f16.bin", "cda268755cc80deb5f4616a99f9d09f26bc5ce3882aa35e0bc504708a12604bb", 84 },
                     { "ggml-model-htdemucs_ft_bass-4s-f16.bin", "e6ded215aa15fb4eff7a37f3a51fd596ee17c94ae0ddb7ef3f2f6ecf8b86cdb5", 84 },
                     { "ggml-model-htdemucs_ft_other-4s-f16.bin", "cbc315571584d512e7853c9d40a499182393a7f4155cb8723a9040a685c2435b", 84 },
                     { "ggml-model-htdemucs_ft_vocals-4s-f16.bin", "19186500a45a551a034d96e9500415ebe73c8bd570bf55337ddc8cc8f53a9120", 84 } };
        case Quality::fourStems: break;
    }

    return { { fileName, sha256, approxMegabytes } };
}

juce::File Model::getFile (const WeightFile& weights)
{
    return getDefaultFile().getSiblingFile (weights.fileName);
}

bool Model::isDownloaded (Quality quality)
{
    for (const auto& weights : getWeightFiles (quality))
        if (verifyFile (getFile (weights), weights).isNotEmpty())
            return false;
    return true;
}

int Model::getMegabytes (Quality quality)
{
    auto total = 0;
    for (const auto& weights : getWeightFiles (quality))
        total += weights.megabytes;
    return total;
}

juce::String Model::downloadFile (const WeightFile& weights, const Progress& progress)
{
    const auto destination = getFile (weights);
    const auto part = destination.withFileExtension (".part");
    if (const auto problem = fetchFile (juce::String (baseUrl) + weights.fileName, part, (juce::int64) weights.megabytes * 1000 * 1000, progress, "the stem model"); problem.isNotEmpty())
        return problem;

    if (const auto problem = verifyFile (part, weights); problem.isNotEmpty())
    {
        part.deleteFile();
        return "The stem model download was incomplete or damaged. Try again.";
    }

    destination.deleteFile();
    if (! part.moveFileTo (destination))
        return "Couldn't save the stem model to " + destination.getFullPathName() + ".";

    if (progress != nullptr)
        progress (1.0);
    return {};
}

juce::String Model::download (const juce::File&, const Progress& progress)
{
    return downloadFile (getWeightFiles (Quality::fourStems).front(), progress);
}

juce::File GpuModel::getDefaultFile()
{
    return Model::getDefaultFile().getSiblingFile (GpuModel::fileName);
}

juce::String GpuModel::install (const juce::File& zip, const juce::File& destination, const juce::String& expectedSha256)
{
    if (! zip.existsAsFile() || juce::SHA256 (zip).toHexString() != expectedSha256)
        return "The GPU model download was incomplete or damaged. Try again.";

    const auto unpack = destination.getSiblingFile (".gpu-model-unpack");
    unpack.deleteRecursively();

    juce::ZipFile archive (zip);
    const auto extracted = archive.uncompressTo (unpack, true);
    const auto inner = unpack.getChildFile (fileName);
    if (extracted.failed() || ! inner.isDirectory())
    {
        unpack.deleteRecursively();
        return "Couldn't unpack the GPU model.";
    }

    destination.deleteRecursively();
    destination.withFileExtension (".mlmodelc").deleteRecursively(); // compiled from the old one
    const auto moved = inner.moveFileTo (destination);
    unpack.deleteRecursively();
    return moved ? juce::String() : "Couldn't save the GPU model to " + destination.getFullPathName() + ".";
}

juce::String GpuModel::download (const juce::File& destination, const Progress& progress)
{
    const auto part = destination.withFileExtension (".zip.part");
    if (const auto problem = fetchFile (downloadUrl, part, (juce::int64) approxMegabytes * 1000 * 1000, progress, "the GPU model"); problem.isNotEmpty())
        return problem;

    const auto problem = install (part, destination);
    part.deleteFile();

    if (problem.isEmpty() && progress != nullptr)
        progress (1.0);
    return problem;
}

double matchOnTestSignal (const Separator& candidate, const Separator& reference)
{
    constexpr int length = 343980;
    juce::AudioBuffer<float> signal (2, length);
    juce::uint32 noise = 12345;

    for (int ch = 0; ch < 2; ++ch)
    {
        auto* out = signal.getWritePointer (ch);
        for (int i = 0; i < length; ++i)
        {
            const auto t = (double) i / modelSampleRate;
            const auto tremolo = 0.6 + 0.4 * std::sin (2.0 * juce::MathConstants<double>::pi * 3.0 * t + ch);
            auto v = 0.0;
            for (const auto hz : { 110.0, 220.0, 440.0, 1320.0, 3000.0 })
                v += 0.08 * std::sin (2.0 * juce::MathConstants<double>::pi * hz * t + hz * 0.01 * ch);
            noise = noise * 1664525u + 1013904223u;
            const auto burst = std::fmod (t, 0.5) < 0.04 ? 0.25 : 0.01; // drum-like hits over a quiet bed
            v = v * tremolo + burst * ((double) (noise >> 8) / (double) (1u << 24) * 2.0 - 1.0);
            out[i] = (float) v;
        }
    }

    const auto wanted = candidate != nullptr && reference != nullptr;
    if (! wanted)
        return -std::numeric_limits<double>::infinity();

    const auto got = candidate (signal, {});
    const auto want = reference (signal, {});

    if (got.size() != want.size() || want.empty())
        return -std::numeric_limits<double>::infinity();

    double signalEnergy = 0.0, errorEnergy = 0.0;

    for (size_t s = 0; s < want.size(); ++s)
    {
        if (got[s].getNumChannels() != want[s].getNumChannels() || got[s].getNumSamples() != want[s].getNumSamples())
            return -std::numeric_limits<double>::infinity();

        for (int ch = 0; ch < want[s].getNumChannels(); ++ch)
            for (int i = 0; i < want[s].getNumSamples(); ++i)
            {
                const auto w = (double) want[s].getSample (ch, i), g = (double) got[s].getSample (ch, i);
                if (! std::isfinite (g))
                    return -std::numeric_limits<double>::infinity();
                signalEnergy += w * w;
                errorEnergy += (g - w) * (g - w);
            }
    }

    if (signalEnergy <= 0.0)
        return -std::numeric_limits<double>::infinity();
    return errorEnergy <= 0.0 ? 200.0 : 10.0 * std::log10 (signalEnergy / errorEnergy);
}

Loaded loadBest (const std::function<void (LoadPhase)>& onPhase, const Progress& progress, Quality quality)
{
    Loaded result;
    const auto setPhase = [&] (LoadPhase phase) { if (onPhase != nullptr) onPhase (phase); };
    const auto cancelled = [&] { return progress != nullptr && ! progress (0.0); };

    if (quality != Quality::fourStems)
    {
        // Six stems and Best quality: CPU only (the Core ML export covers the four-stem network).
        const auto files = Model::getWeightFiles (quality);
        std::vector<Model::WeightFile> missing;
        for (const auto& weights : files)
            if (Model::verifyFile (Model::getFile (weights), weights).isNotEmpty())
                missing.push_back (weights);

        if (! missing.empty())
            setPhase (LoadPhase::downloading);

        for (size_t i = 0; i < missing.size(); ++i)
        {
            const auto part = [&] (double p) { return progress == nullptr || progress ((double) (i + p) / (double) missing.size()); };
            if (const auto problem = Model::downloadFile (missing[i], part); problem.isNotEmpty())
            {
                result.cancelled = cancelled();
                result.error = result.cancelled ? juce::String() : problem;
                return result;
            }
        }

        setPhase (LoadPhase::loading);
        result.separator = Model::loadQuality (quality, result.error);
        return result;
    }

    // The CPU model, downloading it the first time. False with result.error / result.cancelled set.
    const auto loadCpu = [&] (Separator& separator, juce::String& error) -> bool
    {
        const auto model = Model::getDefaultFile();
        if (! model.existsAsFile() || Model::verify (model).isNotEmpty())
        {
            setPhase (LoadPhase::downloading);
            if (const auto problem = Model::download (model, progress); problem.isNotEmpty())
            {
                result.cancelled = cancelled();
                error = result.cancelled ? juce::String() : problem;
                return false;
            }
        }

        setPhase (LoadPhase::loading);
        separator = Model::load (model, error);
        return separator != nullptr;
    };

    // The GPU first, where Core ML is built in: its converted model downloads once, like the CPU one.
    if (const auto gpuModel = GpuModel::getDefaultFile(); GpuModel::isBuiltIn())
    {
        juce::String problem;

        if (! gpuModel.isDirectory())
        {
            setPhase (LoadPhase::downloading);
            problem = GpuModel::download (gpuModel, progress);

            if (cancelled())
            {
                result.cancelled = true;
                return result;
            }

            if (problem.isNotEmpty())
                problem = "couldn't get it: " + problem;
        }

        Separator gpuSeparator;

        if (problem.isEmpty())
        {
            setPhase (LoadPhase::loading);
            gpuSeparator = GpuModel::load (gpuModel, problem);
        }

        if (gpuSeparator != nullptr)
        {
            // A converted model that loads can still compute nonsense (the conversion is checked
            // on a virtual machine that can't run it). Compare it with the CPU model once on this Mac.
            const auto marker = gpuModel.getSiblingFile (juce::String (GpuModel::fileName) + ".verified");

            if (marker.existsAsFile() && marker.loadFileAsString().trim() == juce::String (GpuModel::sha256))
            {
                result.separator = gpuSeparator;
                result.gpu = true;
                return result;
            }

            Separator cpuSeparator;
            juce::String cpuError;

            if (! loadCpu (cpuSeparator, cpuError))
            {
                if (result.cancelled)
                    return result;

                // Nothing to check against, and no CPU to fall back on: the GPU is all there is.
                result.separator = gpuSeparator;
                result.gpu = true;
                return result;
            }

            const auto match = matchOnTestSignal (gpuSeparator, cpuSeparator);

            if (match >= minGpuMatchDb)
            {
                marker.replaceWithText (GpuModel::sha256);
                result.separator = gpuSeparator;
                result.gpu = true;
                return result;
            }

            result.separator = cpuSeparator;
            result.note = "The GPU model's stems didn't match the CPU's on a test (" + (std::isfinite (match) ? juce::String (match, 1) + " dB" : juce::String ("it failed"))
                          + "), so this split ran on the CPU.";
            return result;
        }

        result.note = "The GPU model didn't work (" + problem + "), so this split ran on the CPU.";
    }

    if (cancelled())
    {
        result.cancelled = true;
        return result;
    }

    juce::String error;
    loadCpu (result.separator, error);
    result.error = error;
    return result;
}

#if ! MICFX_HAS_COREML
bool GpuModel::isBuiltIn() { return false; }

Separator GpuModel::load (const juce::File&, juce::String& error)
{
    error = "The GPU model runs through Core ML, which this build doesn't have.";
    return nullptr;
}
#endif

#if ! MICFX_HAS_DEMUCS
bool Model::isBuiltIn() { return false; }

Separator Model::loadQuality (Quality, juce::String& error)
{
    error = "This build of MicFX doesn't include the stem splitter.";
    return nullptr;
}

Separator Model::load (const juce::File&, juce::String& error)
{
    error = "This build of MicFX doesn't include the stem splitter (libs/demucs.cpp and libs/eigen were missing when it was built).";
    return nullptr;
}
#endif
}
