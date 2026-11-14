#pragma once

#include <juce_data_structures/juce_data_structures.h>

#include <array>
#include <atomic>
#include <memory>

//==============================================================================
// Live macros: eight host parameters (macro1..macro8) that each turn any number of the
// Live effects' values, each between its own min and max (min above max turns it the
// other way). A value follows one macro at a time.
//
// The table has one slot per Live effect value (the processor's bindings, in order).
// The message thread maps and unmaps; the audio thread reads the slots lock-free and,
// after the parameter bindings, writes each mapped value from its macro, so a host's
// macro automation is heard even while rendering offline.
class LiveMacros
{
public:
    static constexpr int numMacros = 8;

    explicit LiveMacros (size_t numValues) : slots (new Slot[numValues]), size (numValues) {}

    size_t getNumValues() const noexcept { return size; }

    // Message thread.
    void map (size_t value, int macro, float min, float max)
    {
        if (value >= size || macro < 0 || macro >= numMacros)
            return;
        slots[value].min.store (min, std::memory_order_relaxed);
        slots[value].max.store (max, std::memory_order_relaxed);
        slots[value].macro.store (macro, std::memory_order_release);
        updateCount();
    }

    void unmap (size_t value)
    {
        if (value < size)
            slots[value].macro.store (-1, std::memory_order_release);
        updateCount();
    }

    void unmapAll()
    {
        for (size_t v = 0; v < size; ++v)
            slots[v].macro.store (-1, std::memory_order_release);
        updateCount();
    }

    // -1: the value follows no macro.
    int getMacro (size_t value) const noexcept { return value < size ? slots[value].macro.load (std::memory_order_acquire) : -1; }
    float getMin (size_t value) const noexcept { return slots[value].min.load (std::memory_order_relaxed); }
    float getMax (size_t value) const noexcept { return slots[value].max.load (std::memory_order_relaxed); }
    bool anyMapped() const noexcept { return mapped.load (std::memory_order_acquire) > 0; }

    // Where a mapped value is with its macro at `macroValue` (0..1).
    float target (size_t value, float macroValue) const noexcept
    {
        const auto lo = getMin (value), hi = getMax (value);
        return lo + (hi - lo) * juce::jlimit (0.0f, 1.0f, macroValue);
    }

    // Names (message thread only); empty: "Macro n".
    std::array<juce::String, numMacros> names;

private:
    struct Slot
    {
        std::atomic<int> macro { -1 };
        std::atomic<float> min { 0.0f }, max { 1.0f };
    };

    void updateCount()
    {
        int count = 0;
        for (size_t v = 0; v < size; ++v)
            count += slots[v].macro.load (std::memory_order_relaxed) >= 0 ? 1 : 0;
        mapped.store (count, std::memory_order_release);
    }

    std::unique_ptr<Slot[]> slots;
    size_t size = 0;
    std::atomic<int> mapped { 0 };
};
