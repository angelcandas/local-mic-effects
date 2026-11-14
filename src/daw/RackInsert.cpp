#include "RackInsert.h"

namespace daw
{
RackInsert::RackInsert (int numChains)
    : fx::Module (fx::ModuleType::rack), power (addSwitch (onId, true))
{
    for (int c = 0; c < juce::jlimit (0, maxChains, numChains); ++c)
    {
        auto chain = std::make_unique<Chain>();
        chain->gain = &addValue (gainId (c).toRawUTF8(), 0.0f);
        chain->pan = &addValue (panId (c).toRawUTF8(), 0.0f);
        chain->on = &addChoice (onIdOf (c).toRawUTF8(), 1.0f);
        chain->solo = &addChoice (soloId (c).toRawUTF8(), 0.0f);
        chains.push_back (std::move (chain));
    }
}

void RackInsert::Delay::prepare (int delaySamples, int blockSize)
{
    samples = juce::jmax (0, delaySamples);
    buffer.setSize (2, samples > 0 ? samples + blockSize : 0);
    buffer.clear();
    write = 0;
}

void RackInsert::Delay::process (juce::dsp::AudioBlock<float>& block) noexcept
{
    if (samples <= 0)
        return;
    const auto size = buffer.getNumSamples();
    const auto numChannels = juce::jmin ((int) block.getNumChannels(), 2);
    for (size_t i = 0; i < block.getNumSamples(); ++i)
    {
        const auto read = (write - samples + size) % size;
        for (int ch = 0; ch < numChannels; ++ch)
        {
            auto* line = buffer.getWritePointer (ch);
            line[write] = block.getSample (ch, (int) i);
            block.setSample (ch, (int) i, line[read]);
        }
        write = (write + 1) % size;
    }
}

bool RackInsert::anySolo() const noexcept
{
    for (const auto& chain : chains)
        if (flag (*chain->solo))
            return true;
    return false;
}

std::pair<float, float> RackInsert::chainGains (const Chain& chain, bool soloing) noexcept
{
    // A chain that's off (or not soloed while another is) still runs, silent, so its
    // tails and envelopes go on and it comes back without a jump.
    const auto audible = flag (*chain.on) && (! soloing || flag (*chain.solo));
    const auto gain = audible ? juce::Decibels::decibelsToGain (get (*chain.gain), -100.0f) : 0.0f;
    const auto pan = juce::jlimit (-1.0f, 1.0f, get (*chain.pan) / 100.0f);
    return { gain * juce::jmin (1.0f, 1.0f - pan), gain * juce::jmin (1.0f, 1.0f + pan) };
}

void RackInsert::prepareModule (const juce::dsp::ProcessSpec& spec)
{
    const auto blockSize = (int) spec.maximumBlockSize;
    input.setSize (2, blockSize);

    latency = 0;
    for (auto& chain : chains)
    {
        chain->effects.prepare ({ spec.sampleRate, spec.maximumBlockSize, 2 });
        latency = juce::jmax (latency, chain->effects.getLatencySamples());
    }

    for (auto& chain : chains)
    {
        chain->buffer.setSize (2, blockSize);
        chain->delay.prepare (latency - chain->effects.getLatencySamples(), blockSize);
        for (auto* smoothed : { &chain->left, &chain->right })
            smoothed->reset (spec.sampleRate, 0.03);
        const auto [left, right] = chainGains (*chain, anySolo());
        chain->left.setCurrentAndTargetValue (left); // a rebuilt rack starts where it was
        chain->right.setCurrentAndTargetValue (right);
    }
    bypass.prepare (latency, blockSize);
}

void RackInsert::resetModule()
{
    for (auto& chain : chains)
    {
        chain->effects.reset();
        chain->delay.buffer.clear();
        chain->buffer.clear();
    }
    bypass.buffer.clear();
}

void RackInsert::render (const Context& context) noexcept
{
    auto& block = context.getOutputBlock();
    const auto numSamples = (int) block.getNumSamples();
    const auto numChannels = juce::jmin ((int) block.getNumChannels(), 2);
    if (numSamples > input.getNumSamples())
        return; // larger than prepared for: pass it through untouched

    // Switched off: round the chains, as late as they'd have been.
    if (! flag (power))
    {
        bypass.process (block);
        return;
    }

    for (int ch = 0; ch < 2; ++ch)
        input.copyFrom (ch, 0, block.getChannelPointer ((size_t) juce::jmin (ch, numChannels - 1)), numSamples);
    block.clear();

    const auto solo = anySolo();
    for (auto& chain : chains)
    {
        const auto [left, right] = chainGains (*chain, solo);
        chain->left.setTargetValue (left);
        chain->right.setTargetValue (right);

        for (int ch = 0; ch < 2; ++ch)
            chain->buffer.copyFrom (ch, 0, input, ch, 0, numSamples);
        juce::dsp::AudioBlock<float> chainBlock (chain->buffer.getArrayOfWritePointers(), 2, (size_t) numSamples);
        chain->effects.process (Context (chainBlock));
        chain->delay.process (chainBlock);

        const auto* l = chain->buffer.getReadPointer (0);
        const auto* r = chain->buffer.getReadPointer (1);
        auto* outL = block.getChannelPointer (0);
        auto* outR = block.getChannelPointer ((size_t) juce::jmin (1, numChannels - 1));
        for (int i = 0; i < numSamples; ++i)
        {
            const auto gl = chain->left.getNextValue(), gr = chain->right.getNextValue();
            if (numChannels > 1)
            {
                outL[i] += l[i] * gl;
                outR[i] += r[i] * gr;
            }
            else
            {
                outL[i] += 0.5f * (l[i] * gl + r[i] * gr);
            }
        }
    }

    // The dry line keeps up, so switching off later doesn't play stale samples.
    juce::dsp::AudioBlock<float> dry (input.getArrayOfWritePointers(), 2, (size_t) numSamples);
    bypass.process (dry);
}
}
