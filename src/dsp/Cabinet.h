#pragma once

#include <juce_dsp/juce_dsp.h>

#include "Biquad.h"

#include <array>
#include <vector>

namespace fx
{
//==============================================================================
// A guitar or bass speaker cabinet heard through a microphone, as a filter model: the
// low cut of the box (with its resonance), the cone's mids and presence peaks, and the
// steep top end of a guitar speaker. Our own voicings, not measurements of anyone's
// cabinet. Zero latency and nothing to load, so it switches at once; the cab module
// uses a convolver instead when the user loads an impulse response file.
class Cabinet
{
public:
    enum class Model { openBack1x12, combo2x12, closed4x12, bass4x10, small1x8 };
    enum class Mic { centre, offAxis, edge };

    static constexpr int numModels = 5;

    static juce::StringArray getModelNames()
    {
        return { "1x12 Open back", "2x12 Combo", "4x12 Closed back", "4x10 Bass", "1x8 Small" };
    }

    static juce::StringArray getMicNames() { return { "Centre", "Off-axis", "Edge" }; }

    void prepare (const juce::dsp::ProcessSpec& spec)
    {
        sampleRate = spec.sampleRate;
        designed = false;
        design();
        reset();
    }

    void reset() noexcept
    {
        for (auto& stage : stages)
            stage.reset();
    }

    // Recomputes the filters only when something changed. Audio thread safe.
    void setParameters (Model m, Mic mc, float lowCut, float highCut) noexcept
    {
        if (designed && m == model && mc == mic && juce::approximatelyEqual (lowCut, lowCutHz) && juce::approximatelyEqual (highCut, highCutHz))
            return;

        model = m;
        mic = mc;
        lowCutHz = lowCut;
        highCutHz = highCut;
        design();
    }

    void process (const juce::dsp::ProcessContextReplacing<float>& context) noexcept
    {
        auto& block = context.getOutputBlock();
        const auto numChannels = juce::jmin ((size_t) 2, block.getNumChannels());

        for (size_t ch = 0; ch < numChannels; ++ch)
        {
            auto* data = block.getChannelPointer (ch);
            for (size_t i = 0; i < block.getNumSamples(); ++i)
            {
                auto x = data[i] * trim;
                for (int s = 0; s < numStages; ++s)
                    x = stages[(size_t) s].process (ch, x);
                data[i] = x;
            }
        }
    }

    // The response with the current settings, in dB (for the card's picture).
    double getMagnitudeDb (double frequency) const noexcept
    {
        auto db = juce::Decibels::gainToDecibels ((double) trim);
        for (int s = 0; s < numStages; ++s)
            db += stages[(size_t) s].getMagnitudeDb (frequency, sampleRate);
        return db;
    }

private:
    struct Section
    {
        Biquad::Type type;
        float hz, q, db;
    };

    // Each voicing: low end (box resonance, then the cut below it), mids, presence and the
    // speaker's roll-off (two low-passes, steeper with a higher Q). Frequencies in Hz.
    static std::vector<Section> getVoicing (Model model)
    {
        using T = Biquad::Type;
        switch (model)
        {
            case Model::openBack1x12: // airy and open: little thump, bright
                return { { T::highPass, 85.0f, 0.8f, 0.0f }, { T::peak, 120.0f, 1.4f, 2.0f },
                         { T::peak, 450.0f, 0.9f, -2.5f }, { T::peak, 2300.0f, 1.6f, 4.0f }, { T::peak, 4200.0f, 2.5f, 2.0f },
                         { T::lowPass, 5600.0f, 0.55f, 0.0f }, { T::lowPass, 6400.0f, 1.2f, 0.0f } };
            case Model::combo2x12: // fuller, a forward upper mid
                return { { T::highPass, 75.0f, 0.9f, 0.0f }, { T::peak, 105.0f, 1.8f, 3.5f },
                         { T::peak, 520.0f, 1.0f, -2.0f }, { T::peak, 1600.0f, 1.2f, 2.5f }, { T::peak, 3100.0f, 2.0f, 4.0f },
                         { T::lowPass, 5000.0f, 0.55f, 0.0f }, { T::lowPass, 5800.0f, 1.3f, 0.0f } };
            case Model::closed4x12: // big closed box: tight thump, scooped low mids, cutting presence
                return { { T::highPass, 68.0f, 1.1f, 0.0f }, { T::peak, 95.0f, 2.0f, 5.5f },
                         { T::peak, 620.0f, 0.8f, -4.0f }, { T::peak, 2600.0f, 1.8f, 5.0f }, { T::peak, 3900.0f, 3.0f, 1.5f },
                         { T::lowPass, 4600.0f, 0.55f, 0.0f }, { T::lowPass, 5200.0f, 1.5f, 0.0f } };
            case Model::bass4x10: // deep low end, honk around 800 Hz tamed, a little growl
                return { { T::highPass, 38.0f, 0.8f, 0.0f }, { T::peak, 70.0f, 1.2f, 3.0f },
                         { T::peak, 800.0f, 1.0f, -3.0f }, { T::peak, 1800.0f, 1.4f, 2.0f }, { T::peak, 3000.0f, 2.0f, 1.0f },
                         { T::lowPass, 4200.0f, 0.55f, 0.0f }, { T::lowPass, 5000.0f, 1.0f, 0.0f } };
            case Model::small1x8: // practice amp: no lows, nasal mids, early roll-off
                return { { T::highPass, 160.0f, 1.0f, 0.0f }, { T::peak, 220.0f, 1.5f, 2.0f },
                         { T::peak, 1100.0f, 1.0f, 4.0f }, { T::peak, 2400.0f, 1.5f, 2.0f }, { T::peak, 3200.0f, 2.0f, 0.0f },
                         { T::lowPass, 3600.0f, 0.55f, 0.0f }, { T::lowPass, 4200.0f, 1.0f, 0.0f } };
        }
        return {};
    }

    void design() noexcept
    {
        designed = true;
        const auto voicing = getVoicing (model);

        // Mic placement: on the cone's centre is brighter and the roll-off starts later;
        // the edge of the cone is darker and rounder.
        const auto rollOff = mic == Mic::centre ? 1.15f : mic == Mic::edge ? 0.8f : 1.0f;
        const auto micShelfDb = mic == Mic::centre ? 2.5f : mic == Mic::edge ? -4.0f : 0.0f;

        numStages = 0;
        for (const auto& section : voicing)
        {
            const auto hz = section.type == Biquad::Type::lowPass ? section.hz * rollOff : section.hz;
            stages[(size_t) numStages++].setup (section.type, sampleRate, hz, section.q, section.db);
        }

        stages[(size_t) numStages++].setup (Biquad::Type::highShelf, sampleRate, 2800.0, 0.7, micShelfDb);

        const auto nyquistish = sampleRate * 0.45;
        if (lowCutHz > 21.0f)
            stages[(size_t) numStages++].setup (Biquad::Type::highPass, sampleRate, lowCutHz, 0.707);
        if (highCutHz < 19900.0f && highCutHz < nyquistish)
            stages[(size_t) numStages++].setup (Biquad::Type::lowPass, sampleRate, highCutHz, 0.707);

        // Keep the level steady from model to model: the average power gain over the
        // guitar's range (100 Hz - 4 kHz, log-spaced) is brought to 0 dB.
        trim = 1.0f;
        double power = 0.0;
        constexpr int points = 32;
        for (int i = 0; i < points; ++i)
        {
            const auto f = 100.0 * std::pow (40.0, (double) i / (points - 1));
            power += std::pow (10.0, getMagnitudeDb (f) / 10.0);
        }
        trim = (float) (1.0 / std::sqrt (juce::jmax (1.0e-6, power / points)));
    }

    static constexpr int maxStages = 12;
    std::array<Biquad, maxStages> stages;
    int numStages = 0;
    float trim = 1.0f;

    double sampleRate = 48000.0;
    Model model = Model::closed4x12;
    Mic mic = Mic::offAxis;
    float lowCutHz = 20.0f, highCutHz = 20000.0f;
    bool designed = false;
};
} // namespace fx
