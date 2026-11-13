#pragma once

#include <juce_core/juce_core.h>

#include "dsp/PitchDetector.h"

#include <atomic>
#include <cmath>
#include <vector>

// The singer's pitch for the Practice view: the mic before any effect, tracked every 5 ms
// with YIN, each reading stamped with where the backing track was at that moment.
//
// process() runs on the audio thread and only writes into a lock-free FIFO; read() drains
// it on the message thread. It does nothing (and costs nothing) while switched off.
class PitchTap
{
public:
    struct Reading
    {
        juce::int64 position = 0; // multitrack timeline, samples (the middle of the analysed window)
        float midi = 0.0f;        // 0 when nothing pitched is sung
        float levelDb = -100.0f;  // RMS of the hop
    };

    static constexpr double hopSeconds = 0.005;
    static constexpr int capacity = 4096; // 20 s of readings, far more than a UI frame needs

    // Message thread, before playback (or with the tap off).
    void prepare (double newSampleRate)
    {
        sampleRate = newSampleRate;
        detector.prepare (sampleRate);
        detector.setAutoAnalyse (false);
        hopSamples = juce::jmax (1, (int) std::round (sampleRate * hopSeconds));
        windowDelay = (juce::int64) std::round (sampleRate * 0.007); // YIN looks at the last ~14 ms
        counter = 0;
        sumSquares = 0.0f;
        fifo.reset();
    }

    void setEnabled (bool on) noexcept { enabled = on; }
    bool isEnabled() const noexcept    { return enabled.load(); }
    double getSampleRate() const noexcept { return sampleRate; }

    // Audio thread. `timelinePosition` is where the block's first sample sits on the
    // multitrack timeline (it stays put while the transport is stopped).
    void process (const float* mono, int numSamples, juce::int64 timelinePosition) noexcept
    {
        if (! enabled.load() || mono == nullptr)
            return;

        for (int done = 0; done < numSamples;)
        {
            const auto chunk = juce::jmin (numSamples - done, hopSamples - counter);
            detector.push (mono + done, chunk);
            for (int i = 0; i < chunk; ++i)
                sumSquares += mono[done + i] * mono[done + i];
            done += chunk;
            counter += chunk;

            if (counter < hopSamples)
                continue;

            detector.analyseNow();
            const auto frequency = detector.getFrequency();
            Reading reading;
            reading.position = timelinePosition + done - windowDelay;
            reading.midi = frequency > 0.0f ? 69.0f + 12.0f * std::log2 (frequency / 440.0f) : 0.0f;
            reading.levelDb = juce::Decibels::gainToDecibels (std::sqrt (sumSquares / (float) hopSamples), -100.0f);
            counter = 0;
            sumSquares = 0.0f;

            if (const auto scope = fifo.write (1); scope.blockSize1 > 0)
                readings[(size_t) scope.startIndex1] = reading;
            // (full: the page isn't reading, so the reading is dropped)
        }
    }

    // Message thread. Appends every reading since the last call.
    void read (std::vector<Reading>& out)
    {
        const auto scope = fifo.read (fifo.getNumReady());
        for (int i = 0; i < scope.blockSize1; ++i) out.push_back (readings[(size_t) (scope.startIndex1 + i)]);
        for (int i = 0; i < scope.blockSize2; ++i) out.push_back (readings[(size_t) (scope.startIndex2 + i)]);
    }

private:
    fx::PitchDetector detector;
    std::atomic<bool> enabled { false };
    double sampleRate = 48000.0;
    int hopSamples = 240, counter = 0;
    juce::int64 windowDelay = 0;
    float sumSquares = 0.0f;

    juce::AbstractFifo fifo { capacity };
    std::vector<Reading> readings = std::vector<Reading> ((size_t) capacity);
};
