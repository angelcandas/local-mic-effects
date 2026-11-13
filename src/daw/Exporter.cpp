#include "Exporter.h"

#include "DawEngine.h"

namespace daw
{
namespace
{
    constexpr int renderBlockSize = 1024;

    std::unique_ptr<juce::AudioFormatWriter> createWriter (const juce::File& file, double sampleRate, const juce::String& format,
                                                           int bitDepth, int oggQuality, juce::String& error)
    {
        file.getParentDirectory().createDirectory();
        file.deleteFile();

        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);

        if (! static_cast<juce::FileOutputStream&> (*stream).openedOk())
        {
            error = "Couldn't write " + file.getFullPathName() + ".";
            return {};
        }

        const auto options = juce::AudioFormatWriterOptions{}
                                 .withSampleRate (sampleRate)
                                 .withNumChannels (2)
                                 .withBitsPerSample (bitDepth)
                                 .withQualityOptionIndex (format == "ogg" ? oggQuality : format == "flac" ? 5 : 0); // flac: compression level, size only
        std::unique_ptr<juce::AudioFormatWriter> writer;

        if (format == "aiff")      writer = juce::AiffAudioFormat().createWriterFor (stream, options);
        else if (format == "flac") writer = juce::FlacAudioFormat().createWriterFor (stream, options);
        else if (format == "ogg")  writer = juce::OggVorbisAudioFormat().createWriterFor (stream, options);
        else                       writer = juce::WavAudioFormat().createWriterFor (stream, options);

        if (writer == nullptr)
        {
            error = "Couldn't create a " + format.toUpperCase() + " writer" + (format == "ogg" ? juce::String() : " for " + juce::String (bitDepth) + "-bit") + ".";
            file.deleteFile();
        }

        return writer;
    }
}

juce::String Exporter::formatExtension (const juce::String& format)
{
    return format == "aiff" ? "aiff" : format == "flac" ? "flac" : format == "ogg" ? "ogg" : "wav";
}

juce::Array<int> Exporter::formatBitDepths (const juce::String& format)
{
    return format == "aiff" || format == "flac" ? juce::Array<int> { 16, 24 }
         : format == "ogg"                       ? juce::Array<int> { 16 }
                                                 : juce::Array<int> { 16, 24, 32 };
}

juce::StringArray Exporter::oggQualityNames()
{
    return juce::OggVorbisAudioFormat().getQualityOptions();
}

Exporter::Exporter (Edit& edit, Settings s) : snapshot (edit.getState().createCopy()), settings (s)
{
    settings.format = formatExtension (settings.format);
    const auto depths = formatBitDepths (settings.format);
    if (! depths.contains (settings.bitDepth))
        settings.bitDepth = depths.contains (24) ? 24 : depths.getFirst();
    settings.oggQuality = juce::jlimit (0, juce::jmax (0, oggQualityNames().size() - 1), settings.oggQuality);
    settings.sampleRate = settings.sampleRate > 0.0 ? settings.sampleRate : 48000.0;
}

Exporter::Result Exporter::render (const juce::ValueTree& session, const juce::File& file, const Progress& progress,
                                   double progressFrom, double progressTo)
{
    Result result;

    // One full pass over the range with a fresh engine (so effects start clean). With
    // a writer it writes; without, it only measures the peak (for normalising).
    const auto runPass = [&] (juce::AudioFormatWriter* writer, float gain, double from, double to, float& peak) -> bool
    {
        Edit edit;
        edit.replaceState (session);

        DawEngine engine (edit, false);
        engine.prepare (settings.sampleRate, renderBlockSize);
        engine.loadAllNow();

        const auto rate = settings.sampleRate;
        const auto endSeconds = settings.end >= 0.0 ? settings.end : (double) engine.getLength() / rate;
        const auto first = (juce::int64) std::llround (juce::jmax (0.0, settings.start) * rate);
        const auto last = (juce::int64) std::llround ((endSeconds + juce::jmax (0.0, settings.tail)) * rate);
        const auto total = juce::jmax ((juce::int64) 0, last - first);

        result.seconds = (double) total / rate;
        engine.seek (first);
        engine.play();

        juce::AudioBuffer<float> buffer (2, renderBlockSize);
        juce::Random dither (0x5eed); // fixed seed: the same project exports the same file
        const auto ditherStep = 1.0f / 32768.0f;

        for (juce::int64 done = 0; done < total; done += renderBlockSize)
        {
            const auto numSamples = (int) juce::jmin ((juce::int64) renderBlockSize, total - done);
            engine.process (buffer, 2, numSamples);

            if (! juce::exactlyEqual (gain, 1.0f))
                buffer.applyGain (0, numSamples, gain);

            peak = juce::jmax (peak, buffer.getMagnitude (0, numSamples));

            if (writer != nullptr)
            {
                if (settings.bitDepth == 16 && settings.format != "ogg") // triangular dither, one LSB
                    for (int ch = 0; ch < 2; ++ch)
                        for (int i = 0; i < numSamples; ++i)
                            buffer.addSample (ch, i, (dither.nextFloat() - dither.nextFloat()) * ditherStep);

                if (! writer->writeFromAudioSampleBuffer (buffer, 0, numSamples))
                {
                    result.error = "Writing " + file.getFileName() + " failed (is the disk full?).";
                    return false;
                }
            }

            if (progress && ! progress (from + (to - from) * (double) (done + numSamples) / (double) juce::jmax ((juce::int64) 1, total)))
            {
                result.cancelled = true;
                return false;
            }
        }

        return true;
    };

    auto gain = 1.0f;
    const auto writeFrom = settings.normalise ? (progressFrom + progressTo) / 2.0 : progressFrom;

    if (settings.normalise)
    {
        float measured = 0.0f;
        if (! runPass (nullptr, 1.0f, progressFrom, writeFrom, measured))
            return result;

        if (measured > 1.0e-6f)
            gain = juce::Decibels::decibelsToGain (settings.normaliseTo) / measured;
    }

    auto writer = createWriter (file, settings.sampleRate, settings.format, settings.bitDepth, settings.oggQuality, result.error);
    if (writer == nullptr)
        return result;

    float peak = 0.0f;
    const auto ok = runPass (writer.get(), gain, writeFrom, progressTo, peak);
    writer.reset(); // flushes and closes the file

    if (! ok)
    {
        file.deleteFile(); // no half-written exports
        return result;
    }

    result.peakDecibels = juce::Decibels::gainToDecibels (peak, -100.0f);
    result.files.add (file);
    return result;
}

Exporter::Result Exporter::exportMix (const juce::File& file, Progress progress)
{
    return render (snapshot, file, progress, 0.0, 1.0);
}

Exporter::Result Exporter::exportStems (const juce::File& folder, Progress progress)
{
    Result combined;
    juce::Array<juce::ValueTree> tracks;

    for (const auto& child : snapshot)
        if (child.hasType (ids::TRACK))
            tracks.add (child);

    if (tracks.isEmpty())
    {
        combined.error = "There are no tracks to export.";
        return combined;
    }

    for (int t = 0; t < tracks.size(); ++t)
    {
        // This track alone (mute/solo of the others don't matter), with a bare master.
        auto session = snapshot.createCopy();
        const auto id = (int) tracks[t][ids::id];

        for (auto child : session)
        {
            if (child.hasType (ids::TRACK))
            {
                child.setProperty (ids::solo, false, nullptr);
                child.setProperty (ids::mute, (int) child[ids::id] != id, nullptr);
            }
            else if (child.hasType (ids::MASTER))
            {
                child.setProperty (ids::volume, 0.0f, nullptr);
                Edit::getInserts (child).removeAllChildren (nullptr);
            }
        }

        // Stems are the full length of the song, so they line up when imported elsewhere.
        const auto name = juce::File::createLegalFileName (juce::String (t + 1).paddedLeft ('0', 2) + " " + tracks[t][ids::name].toString());
        auto result = render (session, folder.getChildFile (name + "." + settings.format), progress,
                              (double) t / tracks.size(), (double) (t + 1) / tracks.size());

        if (result.error.isNotEmpty() || result.cancelled)
        {
            for (const auto& written : combined.files)
                written.deleteFile();

            return result;
        }

        combined.files.addArray (result.files);
        combined.seconds = result.seconds;
        combined.peakDecibels = juce::jmax (combined.peakDecibels, result.peakDecibels);
    }

    return combined;
}
}
