// The Demucs side of StemSplitter: only built when libs/demucs.cpp and libs/eigen are
// there (MICFX_HAS_DEMUCS, see CMakeLists.txt).
#include "StemSplitter.h"

#include "model.hpp" // libs/demucs.cpp/src

namespace stems
{
namespace
{
    struct Stopped {}; // thrown from demucs.cpp's progress callback to stop it
}

bool Model::isBuiltIn() { return true; }

Separator Model::load (const juce::File& file, juce::String& error)
{
    if (error = verify (file); error.isNotEmpty())
        return nullptr;

    auto model = std::make_shared<demucscpp::demucs_model>();
    if (! demucscpp::load_demucs_model (file.getFullPathName().toStdString(), model.get()) || ! model->is_4sources)
    {
        error = "Couldn't load the stem model from " + file.getFullPathName() + ".";
        return nullptr;
    }

    // demucs_inference only reads the model, so the threads can share it.
    return [model] (const juce::AudioBuffer<float>& input, const Progress& progress) -> std::vector<juce::AudioBuffer<float>>
    {
        const auto length = input.getNumSamples();
        Eigen::MatrixXf audio (2, length);
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < length; ++i)
                audio (ch, i) = input.getSample (ch, i);

        Eigen::Tensor3dXf out;
        try
        {
            out = demucscpp::demucs_inference (*model, audio, [&progress] (float p, const std::string&)
            {
                if (progress != nullptr && ! progress ((double) p))
                    throw Stopped{};
            });
        }
        catch (const Stopped&)
        {
            return {};
        }

        std::vector<juce::AudioBuffer<float>> stems;
        const auto outLength = (int) juce::jmin ((Eigen::Index) length, (Eigen::Index) out.dimension (2));
        for (int s = 0; s < numStems; ++s)
        {
            auto& stem = stems.emplace_back (2, length);
            stem.clear();
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < outLength; ++i)
                    stem.setSample (ch, i, out (s, ch, i));
        }

        return stems;
    };
}
}
