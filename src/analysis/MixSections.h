#pragma once

#include "MixAssistant.h"

// Mix by section: a song isn't the same all the way through. A dense chorus needs the voice
// higher than a sparse verse to stay as clear, and a chorus feels bigger when the band
// lifts, opens up and gets a little more room. From the song's sections (the song map's
// ruler sections) and the tracks' measurements, Auto Mix adds per-section automation on
// top of its plan:
//
//   - the lead voice's level per section, so it sits as far over the band in every
//     section as over the whole song (measured, not a recipe), and the background vocals
//     with it;
//   - a lift in the chorus-like sections (the "chorus" sections, or the clearly louder
//     ones): the instruments up by the style's amount (0.5 to 1.5 dB), wider, more reverb;
//   - the verses a little closer and drier.
//
// Each change ramps in over half a bar before the section starts. Everything is written as
// ordinary automation (the volume lane, the reverb's mix or send lane, the width lane), so
// it can be seen and edited; the voice's phrase rides stay and the section level sits on
// top of them. Lanes you drew yourself (MixOptions::drawnRides) aren't touched.
// No sections: nothing changes, and the plan says so.
namespace analysis
{
// How much the band lifts in a chorus, per style (dB; 0 for none).
float getSectionLiftDb (MixStyle);

// Adds the section moves to `plan` (planMix calls it when options.bySection is on).
// `analysis` is at Point::source; `currentVolumes` as for planMix.
void planBySection (MixPlan&, const SessionAnalyser::Result& analysis, const std::map<int, Role>& roles,
                    const std::map<int, float>& currentVolumes, MixStyle, const MixOptions&);
}
