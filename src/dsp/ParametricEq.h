#pragma once

#include "Biquad.h"

#include <juce_dsp/juce_dsp.h>

#include <array>
#include <vector>

namespace fx
{
// Seven-band parametric EQ for mixing: a low cut, a low shelf, three bells with their
// own Q, a high shelf and a high cut. The cuts are 24 dB/octave Butterworth filters.
//
// The bands are plain RBJ biquads (Biquad.h), so getMagnitudeDb() gives the exact
// response for any settings. The Auto Mix analysis uses it to plan EQ moves, and the
// UI draws the same curve (ui/fxviews.js).
class ParametricEq
{
public:
    static constexpr int numBells = 3;

    struct Bell
    {
        float hz = 1000.0f, db = 0.0f, q = 1.0f;
        bool operator== (const Bell&) const = default;
    };

    struct Settings
    {
        bool  lowCutOn = false;
        float lowCutHz = 80.0f;
        float lowShelfHz = 120.0f, lowShelfDb = 0.0f;
        std::array<Bell, numBells> bells { { { 250.0f, 0.0f, 1.0f }, { 1000.0f, 0.0f, 1.0f }, { 4000.0f, 0.0f, 1.0f } } };
        float highShelfHz = 8000.0f, highShelfDb = 0.0f;
        bool  highCutOn = false;
        float highCutHz = 18000.0f;

        bool operator== (const Settings&) const = default;
    };

    void prepare (const juce::dsp::ProcessSpec& spec)
    {
        sampleRate = spec.sampleRate;
        design (filters, current, sampleRate);
        reset();
    }

    void reset() noexcept
    {
        for (auto& filter : filters)
            filter.reset();
    }

    // Audio thread. Coefficients are only recomputed when something changed.
    void setSettings (const Settings& settings) noexcept
    {
        if (settings == current)
            return;

        // A cut switching on starts from silence, not from a stale state.
        if (settings.lowCutOn && ! current.lowCutOn)
            filters[lowCut1].reset(), filters[lowCut2].reset();
        if (settings.highCutOn && ! current.highCutOn)
            filters[highCut1].reset(), filters[highCut2].reset();

        current = settings;
        design (filters, current, sampleRate);
    }

    const Settings& getSettings() const noexcept { return current; }

    void process (const juce::dsp::ProcessContextReplacing<float>& context) noexcept
    {
        auto& block = context.getOutputBlock();
        const auto first = current.lowCutOn ? lowCut1 : lowShelf;
        const auto last  = current.highCutOn ? highCut2 : highShelf;

        for (size_t ch = 0; ch < juce::jmin ((size_t) 2, block.getNumChannels()); ++ch)
        {
            auto* data = block.getChannelPointer (ch);

            for (size_t i = 0; i < block.getNumSamples(); ++i)
            {
                auto x = data[i];
                for (int f = first; f <= last; ++f)
                    x = filters[(size_t) f].process (ch, x);
                data[i] = x;
            }
        }
    }

    // The response of `settings` at `frequency`, in dB.
    static double getMagnitudeDb (const Settings& settings, double frequency, double sampleRate)
    {
        Filters bank;
        design (bank, settings, sampleRate);
        return getMagnitudeDb (bank, settings, frequency, sampleRate);
    }

    // The same for many frequencies at once (designs the filters once).
    static std::vector<double> getMagnitudeDb (const Settings& settings, const std::vector<double>& frequencies,
                                               double sampleRate)
    {
        Filters bank;
        design (bank, settings, sampleRate);
        std::vector<double> result;
        result.reserve (frequencies.size());
        for (auto f : frequencies)
            result.push_back (getMagnitudeDb (bank, settings, f, sampleRate));
        return result;
    }

private:
    enum { lowCut1, lowCut2, lowShelf, bell1, bell2, bell3, highShelf, highCut1, highCut2, numFilters };
    using Filters = std::array<Biquad, numFilters>;

    // Q of the two stages of a 4th-order Butterworth filter.
    static constexpr double butterworthQ1 = 0.5411961, butterworthQ2 = 1.3065630;

    static void design (Filters& bank, const Settings& s, double rate) noexcept
    {
        bank[lowCut1].setup (Biquad::Type::highPass, rate, s.lowCutHz, butterworthQ1);
        bank[lowCut2].setup (Biquad::Type::highPass, rate, s.lowCutHz, butterworthQ2);
        bank[lowShelf].setup (Biquad::Type::lowShelf, rate, s.lowShelfHz, 0.707, s.lowShelfDb);

        for (int b = 0; b < numBells; ++b)
        {
            const auto& bell = s.bells[(size_t) b];
            bank[(size_t) (bell1 + b)].setup (Biquad::Type::peak, rate, bell.hz, juce::jlimit (0.1f, 20.0f, bell.q), bell.db);
        }

        bank[highShelf].setup (Biquad::Type::highShelf, rate, s.highShelfHz, 0.707, s.highShelfDb);
        bank[highCut1].setup (Biquad::Type::lowPass, rate, s.highCutHz, butterworthQ1);
        bank[highCut2].setup (Biquad::Type::lowPass, rate, s.highCutHz, butterworthQ2);
    }

    static double getMagnitudeDb (const Filters& bank, const Settings& s, double frequency, double rate) noexcept
    {
        double total = 0.0;
        for (int f = 0; f < numFilters; ++f)
        {
            if (! s.lowCutOn && (f == lowCut1 || f == lowCut2))   continue;
            if (! s.highCutOn && (f == highCut1 || f == highCut2)) continue;
            total += bank[(size_t) f].getMagnitudeDb (frequency, rate);
        }
        return total;
    }

    double sampleRate = 48000.0;
    Settings current;
    Filters filters;
};
} // namespace fx
