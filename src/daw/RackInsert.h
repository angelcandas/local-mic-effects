#pragma once

#include "dsp/FxChain.h"

#include <juce_data_structures/juce_data_structures.h>

#include <memory>
#include <vector>

// A rack: parallel chains of effects as one DAW insert (Ableton's Audio Effect Rack).
//
// In the project it's an INSERT of type "rack" holding CHAIN children (each with a `name`
// and its own INSERTS). Its values are the rack's switch (`rackOn`), its in and out gains,
// and for each chain n (1-based) `chain<n>Gain` (dB), `chain<n>Pan` (-100..100),
// `chain<n>On` and `chain<n>Solo`, so they go through the same paths as any insert value
// (the card, undo, macros).
//
// In the engine every chain gets the rack's input; their outputs are delayed to the
// slowest chain (so chains with different latency stay aligned), panned, given their gain
// and summed. Soloed chains play alone. Switched off, the input goes round the chains,
// delayed by the rack's latency so the timing doesn't jump.
//
// The chains are built by the engine (DawEngine::getStrip), which also keeps every module
// in them by insert id, so their values, meters and files work as on any other insert.
namespace daw
{
class RackInsert final : public fx::Module
{
public:
    static constexpr const char* onId = "rackOn";
    static constexpr int maxChains = 16;

    static juce::String gainId (int chain) { return "chain" + juce::String (chain + 1) + "Gain"; }
    static juce::String panId (int chain)  { return "chain" + juce::String (chain + 1) + "Pan"; }
    static juce::String onIdOf (int chain) { return "chain" + juce::String (chain + 1) + "On"; }
    static juce::String soloId (int chain) { return "chain" + juce::String (chain + 1) + "Solo"; }

    // An empty rack with that many chains (each built after, into getChain()).
    explicit RackInsert (int numChains);

    int getNumChains() const noexcept { return (int) chains.size(); }
    fx::FxChain& getChain (int index) noexcept { return chains[(size_t) index]->effects; }

    // The slowest chain's delay (worked out when it's prepared).
    int getLatencySamples() const noexcept override { return latency; }

    // For the engine: the layout it was built for, and the modules in its chains by insert
    // id (a rebuild with the same layout keeps the running rack, chains and all).
    juce::String layout;
    struct Member { int insertId; std::shared_ptr<fx::Module> module; juce::ValueTree tree; };
    std::vector<Member> members;

protected:
    void prepareModule (const juce::dsp::ProcessSpec&) override;
    void resetModule() override;
    void update() noexcept override {}
    void render (const Context&) noexcept override;
    bool alwaysProcesses() const noexcept override { return true; } // its own bypass, see render()

private:
    struct Delay
    {
        juce::AudioBuffer<float> buffer;
        int write = 0, samples = 0;
        void prepare (int delaySamples, int blockSize);
        void process (juce::dsp::AudioBlock<float>& block) noexcept;
    };

    struct Chain
    {
        fx::FxChain effects;
        Value* gain = nullptr; Value* pan = nullptr; Value* on = nullptr; Value* solo = nullptr;
        juce::AudioBuffer<float> buffer;
        Delay delay;
        juce::SmoothedValue<float> left, right;
    };

    bool anySolo() const noexcept;
    static std::pair<float, float> chainGains (const Chain&, bool soloing) noexcept;

    Value& power;
    std::vector<std::unique_ptr<Chain>> chains;
    juce::AudioBuffer<float> input;
    Delay bypass; // the dry signal while switched off
    int latency = 0;
};
}
