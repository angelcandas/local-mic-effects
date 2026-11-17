#pragma once

#include <juce_audio_formats/juce_audio_formats.h>

extern "C"
{
#include <lame.h>
}

// An MP3 writer on top of LAME (libs/lame). JUCE has no MP3 encoder of its own.
//
// Quality steps (the index passed to the constructor, see qualityNames):
// constant bit rates, then two variable-rate settings.
namespace daw
{
class Mp3Writer final : public juce::AudioFormatWriter
{
public:
    static juce::StringArray qualityNames()
    {
        return { "128 kbps", "160 kbps", "192 kbps", "256 kbps", "320 kbps", "Variable, high (about 190 kbps)", "Variable, highest (about 245 kbps)" };
    }

    static constexpr int defaultQuality = 2;

    // Takes the stream when it succeeds. Returns null if LAME can't be set up.
    static std::unique_ptr<Mp3Writer> create (std::unique_ptr<juce::OutputStream>& stream, double sampleRate, int numChannels, int quality)
    {
        if (stream == nullptr || (numChannels != 1 && numChannels != 2))
            return {};

        auto* flags = lame_init();
        if (flags == nullptr)
            return {};

        static constexpr int bitRates[] = { 128, 160, 192, 256, 320 };
        quality = juce::jlimit (0, qualityNames().size() - 1, quality);

        lame_set_in_samplerate (flags, (int) std::lround (sampleRate));
        lame_set_num_channels (flags, numChannels);
        lame_set_mode (flags, numChannels == 1 ? MONO : JOINT_STEREO);
        lame_set_quality (flags, 2); // the algorithm: 2 = near-best, 5 = fast
        lame_set_write_id3tag_automatic (flags, 0);

        if (quality < 5)
        {
            lame_set_VBR (flags, vbr_off);
            lame_set_brate (flags, bitRates[quality]);
        }
        else
        {
            lame_set_VBR (flags, vbr_mtrh);
            lame_set_VBR_q (flags, quality == 5 ? 2 : 0);
        }

        if (lame_init_params (flags) < 0)
        {
            lame_close (flags);
            return {};
        }

        std::unique_ptr<Mp3Writer> writer (new Mp3Writer (stream.release(), sampleRate, (unsigned int) numChannels, flags));
        return writer;
    }

    ~Mp3Writer() override
    {
        std::vector<unsigned char> tail (7200);
        const auto n = lame_encode_flush (lame, tail.data(), (int) tail.size());

        if (n > 0 && output != nullptr)
            output->write (tail.data(), (size_t) n);

        if (output != nullptr)
            output->flush();

        lame_close (lame);
    }

    bool write (const int** samples, int numSamples) override
    {
        if (numSamples <= 0)
            return true;

        // LAME's worst case is 1.25 * samples + 7200 bytes.
        out.resize ((size_t) (numSamples * 5 / 4 + 7200));
        const auto* left = samples[0];
        const auto* right = numChannels > 1 ? samples[1] : samples[0];

        const auto n = lame_encode_buffer_int (lame, left, right, numSamples, out.data(), (int) out.size());
        return n >= 0 && (n == 0 || output->write (out.data(), (size_t) n));
    }

    bool flush() override { if (output != nullptr) output->flush(); return true; }

private:
    Mp3Writer (juce::OutputStream* stream, double rate, unsigned int channels, lame_global_flags* flags)
        : juce::AudioFormatWriter (stream, "MP3 file", rate, channels, 16), lame (flags) {}

    lame_global_flags* lame = nullptr;
    std::vector<unsigned char> out;
};
}
