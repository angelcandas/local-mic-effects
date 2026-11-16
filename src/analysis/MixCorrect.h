#pragma once

#include "MixAssistant.h"

// Listen and correct: Auto Mix plans from how the tracks sound alone and predicts the
// result, but the effects it adds (EQ, compression, rides, reverb) change each track's
// loudness in ways the prediction only estimates. This writes the plan into a copy of the
// session, renders it once (each track as it plays in the mix, and the mix), measures what
// came out against what the plan aimed for, and works out the volume corrections that close
// the gap: the lead voice over the backing, background vocals under the lead, and the
// master's loudness held under -1 dBTP. Volume changes scale each track's render exactly,
// so the corrections are worked out from that one render in up to two passes, each one
// smaller than the last, stopping within 0.5 LU / 1 dB.
//
// Runs on any thread (the session is a copy; the offline render never touches the message
// thread). Mix and fit modes only: clean and master leave levels alone.
namespace analysis
{
struct MixReading
{
    juce::String what;     // "Voice over backing", "Master loudness"...
    juce::String unit;     // "LU", "LUFS", "dBTP"
    float aimed = 0.0f;    // what the plan aimed for (a limit for "dBTP")
    float got = 0.0f;      // measured in the render of the plan
    float after = 0.0f;    // with the corrections (worked out from the same render)
    bool corrected = false;
};

struct MixCorrection
{
    std::map<int, float> volumeDb;  // by track id (daw::Edit::masterId for the master): change to the applied volume, dB
    std::vector<MixReading> readings;
    float maskingDb = 0.0f;         // how much the rest still covers the lead's words (importance weighted, dB), as rendered
    bool hasMasking = false;
    int passes = 0;                 // correction passes that changed something (0..2)
    bool cancelled = false;
    MixStyle style = MixStyle::pop; // what it was measured for: a different style or mode makes it stale
    MixMode mode = MixMode::mix;
};

using CorrectionProgress = std::function<bool (double)>; // 0..1; false cancels

// `session` is a copy of the session's state as it is now (before the plan); `apply` the
// options the plan is applied with (amount, sliders, unticked moves, base volumes).
MixCorrection listenAndCorrect (const juce::ValueTree& session, const MixPlan&, const std::map<int, Role>& roles,
                                MixStyle, const MixOptions&, const ApplyOptions& apply, double sampleRate = 48000.0,
                                ProfileCache* cache = nullptr, const CorrectionProgress& = {});

// Folds the corrections into the plan: each one changes that track's volume move (or adds
// one), sized so that with `apply`'s amount and sliders the volume moves by the correction,
// and the readings go into the plan's notes ("aimed for / got"). Moves that are unticked or
// whose slider is at zero aren't changed. `currentVolumes`: the volumes the plan starts from.
void applyCorrection (MixPlan&, const MixCorrection&, const std::map<int, Role>& roles,
                      const std::map<int, float>& currentVolumes, float currentMasterVolume, const ApplyOptions& apply);
}
