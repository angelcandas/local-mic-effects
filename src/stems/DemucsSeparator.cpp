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

namespace
{
    // One loaded network and how to run it on a chunk. `sources` is 4 or 6.
    struct Network
    {
        std::shared_ptr<demucscpp::demucs_model> model;
        int sources = 4;
    };

    // Runs the network on a stereo chunk. Empty when `progress` stopped it. `from`..`to` is the part of
    // the progress bar this network's pass covers.
    std::vector<juce::AudioBuffer<float>> run (const Network& network, const juce::AudioBuffer<float>& input,
                                               const Progress& progress, double from, double to)
    {
        const auto length = input.getNumSamples();
        Eigen::MatrixXf audio (2, length);
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < length; ++i)
                audio (ch, i) = input.getSample (ch, i);

        Eigen::Tensor3dXf out;
        try
        {
            // demucs_inference only reads the model, so the threads can share it.
            out = demucscpp::demucs_inference (*network.model, audio, [&] (float p, const std::string&)
            {
                if (progress != nullptr && ! progress (from + (to - from) * (double) p))
                    throw Stopped{};
            });
        }
        catch (const Stopped&)
        {
            return {};
        }

        std::vector<juce::AudioBuffer<float>> stems;
        const auto outLength = (int) juce::jmin ((Eigen::Index) length, (Eigen::Index) out.dimension (2));
        const auto count = (int) juce::jmin ((Eigen::Index) network.sources, (Eigen::Index) out.dimension (0));
        for (int s = 0; s < count; ++s)
        {
            auto& stem = stems.emplace_back (2, length);
            stem.clear();
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < outLength; ++i)
                    stem.setSample (ch, i, out (s, ch, i));
        }

        return stems;
    }

    std::shared_ptr<Network> loadNetwork (const juce::File& file, bool fourSources)
    {
        auto network = std::make_shared<Network>();
        network->model = std::make_shared<demucscpp::demucs_model>();
        if (! demucscpp::load_demucs_model (file.getFullPathName().toStdString(), network->model.get())
            || network->model->is_4sources != fourSources)
            return nullptr;
        network->sources = fourSources ? 4 : 6;
        return network;
    }
}

Separator Model::load (const juce::File& file, juce::String& error)
{
    if (error = verify (file); error.isNotEmpty())
        return nullptr;

    const auto network = loadNetwork (file, true);
    if (network == nullptr)
    {
        error = "Couldn't load the stem model from " + file.getFullPathName() + ".";
        return nullptr;
    }

    return [network] (const juce::AudioBuffer<float>& input, const Progress& progress)
    {
        return run (*network, input, progress, 0.0, 1.0);
    };
}

Separator Model::loadQuality (Quality quality, juce::String& error)
{
    if (quality == Quality::fourStems)
        return load (getFile (getWeightFiles (quality).front()), error);

    std::vector<std::shared_ptr<Network>> networks;
    for (const auto& weights : getWeightFiles (quality))
    {
        const auto file = getFile (weights);
        if (error = verifyFile (file, weights); error.isNotEmpty())
            return nullptr;

        const auto network = loadNetwork (file, quality == Quality::best);
        if (network == nullptr)
        {
            error = "Couldn't load the stem model from " + file.getFullPathName() + ".";
            return nullptr;
        }
        networks.push_back (network);
    }

    if (quality == Quality::sixStems)
        return [network = networks.front()] (const juce::AudioBuffer<float>& input, const Progress& progress)
        {
            return run (*network, input, progress, 0.0, 1.0);
        };

    // Best quality: each fine-tuned model is the best at its own stem (the bag of models in Demucs
    // gives each one weight 1 for its stem and 0 for the rest), so run all four and keep one stem
    // from each. The files are in Stem order: drums, bass, other, vocals.
    return [networks] (const juce::AudioBuffer<float>& input, const Progress& progress) -> std::vector<juce::AudioBuffer<float>>
    {
        std::vector<juce::AudioBuffer<float>> result;
        const auto count = (double) networks.size();

        for (size_t i = 0; i < networks.size(); ++i)
        {
            auto stems = run (*networks[i], input, progress, (double) i / count, (double) (i + 1) / count);
            if (stems.size() != 4)
                return {};
            result.push_back (std::move (stems[i]));
        }

        return result;
    };
}
}
