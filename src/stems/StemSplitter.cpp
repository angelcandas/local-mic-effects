#include "StemSplitter.h"

#include <juce_cryptography/juce_cryptography.h>

#include <atomic>
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

            if (piece.stems.size() != (size_t) numStems)
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
    std::vector<juce::AudioBuffer<float>> result;
    for (int s = 0; s < numStems; ++s)
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

            for (int s = 0; s < numStems; ++s)
                for (int ch = 0; ch < 2; ++ch)
                    result[(size_t) s].getWritePointer (ch)[piece.from + i]
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
    if (separated.size() != (size_t) numStems)
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
    for (int s = 0; s < numStems; ++s)
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
            result.files = {};
            result.error = "Couldn't write " + file.getFullPathName() + ".";
            return result;
        }

        result.files[(size_t) s] = file;
        stage (0.97, 1.0) ((s + 1.0) / numStems);
    }

    return result;
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

juce::String Model::verify (const juce::File& file)
{
    if (! file.existsAsFile())
        return "The stem model isn't downloaded yet.";

    if (juce::SHA256 (file).toHexString() != juce::String (sha256))
        return file.getFileName() + " isn't the expected Demucs model (its checksum doesn't match).";

    return {};
}

juce::String Model::download (const juce::File& destination, const Progress& progress)
{
    if (! destination.getParentDirectory().createDirectory())
        return "Couldn't create " + destination.getParentDirectory().getFullPathName() + ".";

    const auto part = destination.withFileExtension (".part");
    part.deleteFile();

    int status = 0;
    auto input = juce::URL (downloadUrl).createInputStream (juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                                                                .withConnectionTimeoutMs (20000)
                                                                .withNumRedirectsToFollow (10)
                                                                .withStatusCode (&status));
    if (input == nullptr || (status != 0 && status != 200))
        return "Couldn't download the stem model (" + (status != 0 ? "HTTP " + juce::String (status) : juce::String ("no connection"))
             + "). Check the internet connection and try again.";

    const auto expected = input->getTotalLength() > 0 ? input->getTotalLength() : (juce::int64) approxMegabytes * 1000 * 1000;

    {
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
    }

    if (const auto problem = verify (part); problem.isNotEmpty())
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

juce::File GpuModel::getDefaultFile()
{
    return Model::getDefaultFile().getSiblingFile (GpuModel::fileName);
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

Separator Model::load (const juce::File&, juce::String& error)
{
    error = "This build of MicFX doesn't include the stem splitter (libs/demucs.cpp and libs/eigen were missing when it was built).";
    return nullptr;
}
#endif
}
