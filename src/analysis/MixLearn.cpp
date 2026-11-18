#include "MixLearn.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <cmath>

namespace analysis::learn
{
namespace
{
constexpr float silenceDb = -200.0f;

juce::AudioFormatManager& getFormats()
{
    struct Formats : juce::AudioFormatManager
    {
        Formats() { registerBasicFormats(); }
    };
    static Formats formats;
    return formats;
}

float toDb (double power) { return power > 1.0e-20 ? (float) (10.0 * std::log10 (power)) : silenceDb; }

void writeInt16 (juce::OutputStream& out, float value, float scale)
{
    const auto v = juce::jlimit (-32767.0f, 32767.0f, std::round (value * scale));
    out.writeShort ((short) v);
}
}

FileBands measureFile (const juce::File& file)
{
    FileBands result;
    std::unique_ptr<juce::AudioFormatReader> reader (getFormats().createReaderFor (file));
    if (reader == nullptr || reader->lengthInSamples <= 0 || reader->sampleRate <= 0.0)
        return result;

    const auto sr = reader->sampleRate;
    const auto stereo = reader->numChannels >= 2;
    result.sampleRate = sr;
    result.channels = stereo ? 2 : 1;

    constexpr int order = 13, size = 1 << order;
    juce::dsp::FFT fft (order);
    std::vector<float> window ((size_t) size);
    double windowPower = 0.0;
    for (int i = 0; i < size; ++i)
    {
        window[(size_t) i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) size);
        windowPower += (double) window[(size_t) i] * window[(size_t) i];
    }
    const auto norm = 2.0 / ((double) size * windowPower);

    // Each bin's share of each band (bins straddling an edge are split by overlap).
    const auto binWidth = sr / size;
    std::vector<std::vector<std::pair<int, double>>> bandBins ((size_t) numBands);
    for (int b = 0; b < numBands; ++b)
    {
        const auto low = getBandLow (b), high = juce::jmin (getBandHigh (b), sr * 0.5);
        if (low >= high)
            continue;
        for (int k = juce::jmax (1, (int) std::floor (low / binWidth)); k <= juce::jmin (size / 2, (int) std::ceil (high / binWidth)); ++k)
        {
            const auto f0 = (k - 0.5) * binWidth, f1 = (k + 0.5) * binWidth;
            const auto overlap = juce::jmin (f1, high) - juce::jmax (f0, low);
            if (overlap > 0.0)
                bandBins[(size_t) b].push_back ({ k, overlap / binWidth });
        }
    }

    const auto hop = juce::jmax (1, (int) std::round (Profile::hopSeconds * sr));
    const auto total = reader->lengthInSamples;
    const auto numHops = (int) (total / hop);
    std::vector<float> ring[2] { std::vector<float> ((size_t) size, 0.0f), std::vector<float> ((size_t) size, 0.0f) };
    int ringPos = 0;
    juce::AudioBuffer<float> block (stereo ? 2 : 1, hop);
    std::vector<float> data[2] { std::vector<float> ((size_t) size * 2), std::vector<float> ((size_t) size * 2) };

    for (int h = 0; h < numHops; ++h)
    {
        block.clear();
        reader->read (&block, 0, hop, (juce::int64) h * hop, true, stereo);

        double sumSquares = 0.0;
        float peak = 0.0f;
        for (int c = 0; c < block.getNumChannels(); ++c)
        {
            const auto* x = block.getReadPointer (c);
            for (int i = 0; i < hop; ++i)
            {
                sumSquares += (double) x[i] * x[i];
                peak = juce::jmax (peak, std::abs (x[i]));
            }
        }
        // Same scale as Profile::rmsDb: mean square of the channels' average.
        result.rmsDb.push_back (toDb (sumSquares / (hop * block.getNumChannels())));
        result.peakDb.push_back (peak > 0.0f ? juce::Decibels::gainToDecibels (peak, silenceDb) : silenceDb);

        for (int c = 0; c < block.getNumChannels(); ++c)
        {
            const auto* x = block.getReadPointer (c);
            auto pos = ringPos;
            for (int i = 0; i < hop; ++i)
            {
                ring[c][(size_t) pos] = x[i];
                pos = (pos + 1) % size;
            }
        }
        ringPos = (ringPos + hop) % size;

        for (int c = 0; c < block.getNumChannels(); ++c)
        {
            auto& d = data[c];
            std::fill (d.begin(), d.end(), 0.0f);
            for (int i = 0; i < size; ++i)
                d[(size_t) i] = ring[c][(size_t) ((ringPos + i) % size)] * window[(size_t) i];
            fft.performRealOnlyForwardTransform (d.data(), true);
        }

        Bands left {}, right {}, corr {};
        for (int b = 0; b < numBands; ++b)
        {
            double pl = 0.0, pr = 0.0, cross = 0.0;
            for (const auto& [k, share] : bandBins[(size_t) b])
            {
                const auto lr = (double) data[0][(size_t) (2 * k)], li = (double) data[0][(size_t) (2 * k + 1)];
                pl += share * (lr * lr + li * li);
                if (stereo)
                {
                    const auto rr = (double) data[1][(size_t) (2 * k)], ri = (double) data[1][(size_t) (2 * k + 1)];
                    pr += share * (rr * rr + ri * ri);
                    cross += share * (lr * rr + li * ri);
                }
            }
            left[(size_t) b] = toDb (pl * norm);
            if (stereo)
            {
                right[(size_t) b] = toDb (pr * norm);
                corr[(size_t) b] = pl > 0.0 && pr > 0.0 ? (float) juce::jlimit (-1.0, 1.0, cross / std::sqrt (pl * pr)) : 1.0f;
            }
        }
        result.left.push_back (left);
        if (stereo)
        {
            result.right.push_back (right);
            result.correlation.push_back (corr);
        }
    }
    return result;
}

bool writeSong (const bench::Song& song, const std::map<juce::String, Role>& roles, const juce::File& out,
                const std::function<void (const juce::String&)>& log)
{
    struct Entry { juce::String name, kind, role; FileBands bands; };
    std::vector<Entry> entries;

    for (const auto& file : song.tracks)
    {
        const auto name = file.getFileNameWithoutExtension();
        auto bands = measureFile (file);
        if (bands.getNumHops() == 0)
        {
            if (log) log ("  learn: couldn't read " + file.getFileName());
            continue;
        }
        const auto it = roles.find (name);
        entries.push_back ({ name, "track", getRoleKey (it != roles.end() ? it->second : Role::other), std::move (bands) });
    }
    if (song.mix.existsAsFile())
    {
        auto bands = measureFile (song.mix);
        if (bands.getNumHops() > 0)
            entries.push_back ({ "mix", "mix", {}, std::move (bands) });
    }
    if (entries.empty())
        return false;

    juce::Array<juce::var> files;
    for (const auto& e : entries)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("name", e.name);
        o->setProperty ("kind", e.kind);
        o->setProperty ("role", e.role);
        o->setProperty ("sampleRate", e.bands.sampleRate);
        o->setProperty ("channels", e.bands.channels);
        o->setProperty ("hops", e.bands.getNumHops());
        files.add (juce::var (o));
    }
    juce::Array<juce::var> centres;
    for (int b = 0; b < numBands; ++b)
        centres.add (getBandCentre (b));

    auto* header = new juce::DynamicObject();
    header->setProperty ("song", song.name);
    header->setProperty ("style", song.style.has_value() ? bench::getStyleKey (*song.style) : juce::String());
    header->setProperty ("tempo", song.tempo);
    header->setProperty ("hop", Profile::hopSeconds);
    header->setProperty ("bands", centres);
    header->setProperty ("files", files);
    const auto json = juce::JSON::toString (juce::var (header), true).toStdString();

    out.deleteFile();
    juce::FileOutputStream fileStream (out);
    if (! fileStream.openedOk())
        return false;
    {
        juce::GZIPCompressorOutputStream zip (fileStream, 6);
        zip.write ("MLF1", 4);
        zip.writeInt ((int) json.size());
        zip.write (json.data(), json.size());
        for (const auto& e : entries)
        {
            const auto& f = e.bands;
            for (const auto& row : f.left)
                for (const auto v : row) writeInt16 (zip, v, 100.0f);
            for (const auto& row : f.right)
                for (const auto v : row) writeInt16 (zip, v, 100.0f);
            for (const auto& row : f.correlation)
                for (const auto v : row) writeInt16 (zip, v, 10000.0f);
            for (const auto v : f.peakDb) writeInt16 (zip, v, 100.0f);
            for (const auto v : f.rmsDb) writeInt16 (zip, v, 100.0f);
        }
        zip.flush();
    }
    fileStream.flush();
    if (log) log ("  learn: wrote " + out.getFileName() + " (" + juce::File::descriptionOfSizeInBytes (out.getSize()) + ")");
    return true;
}
}
