#include "FactoryChains.h"

#include "PresetLibrary.h"
#include "dsp/FxModules.h"

#include <deque>
#include <optional>

namespace factory
{
namespace
{
    // Pieces several chains share.
    Device highPass (float hz) { return { "highPass", { { "hpfFreq", hz } } }; }

    Device tune (float speed, float amount)
    {
        return { "pitch", { { "tuneOn", 1.0f }, { "pitchOn", 0.0f }, { "harmonyOn", 0.0f }, { "tuneSpeed", speed }, { "tuneAmount", amount } } };
    }

    Device smoothHarsh (float depth)
    {
        return { "smooth", { { "smoothDepth", depth }, { "smoothThreshold", 5.0f }, { "smoothSharpness", 60.0f }, { "smoothFocus", 40.0f },
                             { "smoothLowHz", 1500.0f }, { "smoothHighHz", 10000.0f } } };
    }

    Device smoothGentle()
    {
        return { "smooth", { { "smoothDepth", 5.0f }, { "smoothThreshold", 7.0f }, { "smoothSharpness", 50.0f },
                             { "smoothLowHz", 150.0f }, { "smoothHighHz", 16000.0f } } };
    }

    Device deEss (float amount, bool wide, float range = 10.0f, float freq = 6000.0f)
    {
        return { "deEsser", { { "deessMode", 1.0f }, { "deessAmount", amount }, { "deessBand", wide ? 1.0f : 0.0f },
                              { "deessRange", range }, { "deessFreq", freq } } };
    }

    Device opto (float amount, float output)
    {
        return { "opto", { { "optoAmount", amount }, { "optoOutput", output } } };
    }

    enum Style { clean = 0, punch = 1, glue = 2 };

    Device comp (Style style, float threshold, float ratio, float attack, float release, float makeup, float mix = 100.0f)
    {
        return { "compressor", { { "compStyle", (float) style }, { "compThreshold", threshold }, { "compRatio", ratio },
                                 { "compAttack", attack }, { "compRelease", release }, { "compKnee", style == glue ? 10.0f : 4.0f },
                                 { "compMakeup", makeup }, { "compMix", mix } } };
    }

    // Sync: 4 = 1/8, 6 = dotted 1/8, 7 = 1/4 (fx::modules::Echo::getSyncNames).
    Device echo (float sync, float timeMs, float feedback, float mix, float duck, float lowCut, float highCut, bool pingPong = false)
    {
        return { "echo", { { "delaySync", sync }, { "delayTime", timeMs }, { "delayFeedback", feedback }, { "delayMix", mix },
                           { "delayDuck", duck }, { "delayLowCut", lowCut }, { "delayHighCut", highCut }, { "delayPingPong", pingPong ? 1.0f : 0.0f } } };
    }

    Device plate (float decay, float preDelay, float mix, float duck, float damping = 40.0f, float mod = 30.0f, float lowCut = 200.0f,
                  float highCut = 9000.0f)
    {
        return { "plate", { { "plateDecay", decay }, { "platePreDelay", preDelay }, { "plateMix", mix }, { "plateDuck", duck },
                            { "plateDamping", damping }, { "plateMod", mod }, { "plateLowCut", lowCut }, { "plateHighCut", highCut } } };
    }

    // satType: 1 Tape, 2 Tube, 3 Clip.
    Device saturation (float type, float drive, float mix, float tone = 0.0f)
    {
        return { "saturation", { { "satType", type }, { "satDrive", drive }, { "satMix", mix }, { "satTone", tone } } };
    }

    Device microShift (float cents, float mix, float spread = 80.0f, float delay = 12.0f)
    {
        return { "doubler", { { "doublerMode", 1.0f }, { "doublerDetune", cents }, { "doublerMix", mix }, { "doublerSpread", spread }, { "doublerDelay", delay } } };
    }

    // The parametric EQ: low shelf, three bells, high shelf, optional cuts.
    struct Eq
    {
        float lowCut = 0.0f, lowShelfHz = 120.0f, lowShelfDb = 0.0f;
        float b1Hz = 250.0f, b1Db = 0.0f, b2Hz = 1000.0f, b2Db = 0.0f, b3Hz = 4000.0f, b3Db = 0.0f;
        float highShelfHz = 10000.0f, highShelfDb = 0.0f, highCut = 0.0f;
    };

    Device eq (const Eq& e)
    {
        return { "paraEq", { { "peqLowCutOn", e.lowCut > 0.0f ? 1.0f : 0.0f }, { "peqLowCutFreq", e.lowCut > 0.0f ? e.lowCut : 80.0f },
                             { "peqLowFreq", e.lowShelfHz }, { "peqLowGain", e.lowShelfDb },
                             { "peqBell1Freq", e.b1Hz }, { "peqBell1Gain", e.b1Db }, { "peqBell2Freq", e.b2Hz }, { "peqBell2Gain", e.b2Db },
                             { "peqBell3Freq", e.b3Hz }, { "peqBell3Gain", e.b3Db },
                             { "peqHighFreq", e.highShelfHz }, { "peqHighGain", e.highShelfDb },
                             { "peqHighCutOn", e.highCut > 0.0f ? 1.0f : 0.0f }, { "peqHighCutFreq", e.highCut > 0.0f ? e.highCut : 18000.0f } } };
    }

    Device gate (float threshold, float range, float hold, float release, bool focus)
    {
        return { "gate", { { "gateThreshold", threshold }, { "gateRange", range }, { "gateHold", hold }, { "gateRelease", release },
                           { "gateFocus", focus ? 1.0f : 0.0f } } };
    }

    Device exciter (float amount, float freq = 4000.0f) { return { "exciter", { { "exciterAmount", amount }, { "exciterFreq", freq } } }; }
    Device width (float amount) { return { "width", { { "widthAmount", amount } } }; }
    Device denoise (float reduction) { return { "denoise", { { "denoiseReduction", reduction } } }; }
    Device irReverb (float space, float mix) { return { "irReverb", { { "irvSpace", space }, { "irvMix", mix } } }; }

    // A trim on the chain's last effect, so it comes out about as loud as it went in.
    Device trimmed (Device device, float outDb)
    {
        device.values.push_back ({ nullptr, outDb }); // filled in below with the effect's own OutGain ID
        return device;
    }
}

const std::vector<Chain>& getChains()
{
    static const std::vector<Chain> chains = []
    {
        std::vector<Chain> list {
            { "Pop lead: modern", "Vocals", "Bright, tight and up front: two-stage compression, gentle tune, air, a tempo echo and a ducked plate",
              { highPass (90.0f), tune (25.0f, 90.0f), eq ({ .b1Db = -2.0f, .b3Hz = 3000.0f, .b3Db = 1.5f, .highShelfDb = 3.0f }), smoothHarsh (5.0f),
                comp (punch, -20.0f, 4.0f, 3.0f, 80.0f, 3.0f), opto (35.0f, 2.0f), deEss (50.0f, false),
                saturation (1.0f, 3.0f, 30.0f), microShift (8.0f, 15.0f), echo (4.0f, 250.0f, 20.0f, 15.0f, 60.0f, 300.0f, 5000.0f),
                trimmed (plate (1.8f, 40.0f, 16.0f, 30.0f), 1.7f) } },

            { "Pop ballad: intimate", "Vocals", "Close and warm: the opto levels first, a glue compressor, soft tune and a long, dark plate",
              { highPass (80.0f), tune (60.0f, 70.0f), eq ({ .b1Hz = 200.0f, .b1Db = -1.5f, .highShelfHz = 12000.0f, .highShelfDb = 3.0f }), smoothGentle(),
                opto (50.0f, 3.0f), comp (glue, -20.0f, 2.0f, 20.0f, 200.0f, 1.0f), deEss (40.0f, true),
                saturation (2.0f, 2.0f, 25.0f), echo (7.0f, 500.0f, 25.0f, 8.0f, 70.0f, 300.0f, 4500.0f),
                trimmed (plate (2.6f, 60.0f, 22.0f, 25.0f, 65.0f), 3.4f) } },

            { "Rap lead: up front", "Vocals", "Dry and punchy: hard compression, edge from the clipper, a slap and a tight booth",
              { highPass (100.0f), eq ({ .b1Hz = 300.0f, .b1Db = -3.0f, .b3Db = 2.0f, .highShelfDb = 2.0f }),
                comp (punch, -21.0f, 6.0f, 1.0f, 60.0f, 4.0f), opto (25.0f, 1.0f), deEss (55.0f, false),
                saturation (3.0f, 4.0f, 40.0f), echo (0.0f, 90.0f, 0.0f, 8.0f, 0.0f, 250.0f, 6000.0f),
                trimmed (irReverb (0.0f, 8.0f), 3.8f) } },

            { "Melodic trap: hard tune", "Vocals", "Hard-tuned and wide: fast tune, heavy compression, micro-shift double, dotted ping-pong and a dark plate",
              { highPass (110.0f), tune (0.0f, 100.0f), eq ({ .b1Db = -3.0f, .b3Hz = 5000.0f, .b3Db = 2.0f, .highShelfHz = 12000.0f, .highShelfDb = 4.0f }),
                comp (punch, -21.0f, 8.0f, 2.0f, 70.0f, 5.0f), deEss (60.0f, false), exciter (20.0f), microShift (10.0f, 20.0f),
                echo (6.0f, 375.0f, 35.0f, 18.0f, 65.0f, 300.0f, 4000.0f, true), plate (2.8f, 30.0f, 20.0f, 40.0f, 70.0f),
                trimmed (width (125.0f), 4.1f) } },

            { "R&B and soul: silk", "Vocals", "Smooth and silky: opto then a light punch, tube warmth, a subtle double and a ducked echo",
              { highPass (80.0f), tune (40.0f, 80.0f), eq ({ .lowShelfHz = 180.0f, .lowShelfDb = 1.0f, .b1Hz = 350.0f, .b1Db = -2.0f, .highShelfHz = 12000.0f, .highShelfDb = 3.5f }),
                smoothGentle(), opto (55.0f, 3.0f), comp (punch, -20.0f, 3.0f, 5.0f, 100.0f, 1.5f), deEss (50.0f, true),
                saturation (2.0f, 3.0f, 35.0f), microShift (6.0f, 10.0f), echo (6.0f, 375.0f, 25.0f, 12.0f, 60.0f, 300.0f, 4500.0f, true),
                trimmed (plate (2.0f, 60.0f, 18.0f, 30.0f), 4.7f) } },

            { "Rock lead: grit", "Vocals", "Gritty and forward: hard parallel compression, tube drive, a slap and a short plate",
              { highPass (100.0f), eq ({ .b1Hz = 400.0f, .b1Db = -2.0f, .b3Hz = 3000.0f, .b3Db = 3.0f }),
                comp (punch, -23.0f, 6.0f, 5.0f, 90.0f, 5.0f, 80.0f), deEss (40.0f, false), saturation (2.0f, 6.0f, 50.0f),
                echo (0.0f, 110.0f, 10.0f, 12.0f, 0.0f, 250.0f, 5000.0f), trimmed (plate (1.1f, 20.0f, 12.0f, 0.0f), 2.7f) } },

            { "Indie and lo-fi: dream", "Vocals", "Hazy and soft: dark top, tape drive, chorus and a second take, long echo and a moving plate",
              { highPass (120.0f), eq ({ .b2Hz = 2000.0f, .b2Db = 1.5f, .highCut = 9000.0f }), comp (glue, -24.0f, 4.0f, 15.0f, 150.0f, 3.0f),
                saturation (1.0f, 6.0f, 60.0f, -20.0f), { "chorus", { { "chorusDepth", 20.0f }, { "chorusMix", 20.0f } } },
                { "doubler", { { "doublerMode", 2.0f }, { "doublerMix", 25.0f } } }, echo (7.0f, 500.0f, 40.0f, 20.0f, 30.0f, 300.0f, 3000.0f),
                trimmed (plate (3.2f, 30.0f, 30.0f, 20.0f, 75.0f, 60.0f), 4.8f) } },

            { "Acoustic: natural", "Vocals", "Natural and open: light levelling, a gentle de-ess and a small room",
              { highPass (80.0f), eq ({ .b1Db = -1.5f, .highShelfDb = 1.5f }), opto (40.0f, 2.0f), deEss (30.0f, true),
                irReverb (1.0f, 14.0f), trimmed (plate (1.4f, 30.0f, 8.0f, 0.0f), 1.7f) } },

            { "Dance top line: bright and wide", "Vocals", "Big, bright and wide: heavy compression, sparkle, micro-shift stack, ping-pong and a ducked plate",
              { highPass (120.0f), tune (15.0f, 100.0f), eq ({ .b1Hz = 300.0f, .b1Db = -3.0f, .highShelfDb = 4.0f }), smoothHarsh (9.0f),
                comp (punch, -21.0f, 6.0f, 2.0f, 70.0f, 4.0f), opto (30.0f, 1.5f), deEss (60.0f, false), exciter (25.0f), microShift (12.0f, 25.0f),
                echo (4.0f, 250.0f, 30.0f, 16.0f, 70.0f, 300.0f, 5000.0f, true), plate (2.4f, 40.0f, 20.0f, 50.0f), trimmed (width (130.0f), 3.1f) } },

            { "Backing vocals: stack", "Vocals", "Sits behind the lead: less body and presence, firm de-essing, a wide stack and more reverb",
              { highPass (150.0f), eq ({ .b1Hz = 300.0f, .b1Db = -3.0f, .b3Hz = 3000.0f, .b3Db = -1.5f, .highShelfHz = 12000.0f, .highShelfDb = 2.0f }),
                comp (glue, -26.0f, 4.0f, 10.0f, 150.0f, 4.0f), deEss (70.0f, false, 12.0f), microShift (14.0f, 40.0f, 100.0f),
                plate (2.2f, 30.0f, 24.0f, 0.0f), trimmed (width (140.0f), 4.0f) } },

            { "Ad-libs and throws", "Vocals", "Thin and distant, made to throw: clipped edge, a long ducked echo and a wide plate",
              { highPass (200.0f), eq ({ .b2Hz = 1500.0f, .b2Db = 2.0f, .highCut = 7000.0f }), comp (punch, -22.0f, 6.0f, 3.0f, 80.0f, 4.0f),
                saturation (3.0f, 4.0f, 50.0f), echo (7.0f, 500.0f, 50.0f, 30.0f, 40.0f, 400.0f, 3500.0f), plate (2.5f, 30.0f, 22.0f, 0.0f),
                trimmed (width (140.0f), 4.3f) } },

            { "Podcast and spoken word", "Vocals", "Clear, even speech: noise taken out, a soft expander, warmth and steady level. No space",
              { highPass (80.0f), denoise (12.0f), gate (-50.0f, 10.0f, 150.0f, 250.0f, false),
                eq ({ .lowShelfHz = 150.0f, .lowShelfDb = 1.5f, .b1Hz = 350.0f, .b1Db = -2.0f, .b3Db = 1.5f }),
                comp (glue, -24.0f, 3.0f, 10.0f, 150.0f, 2.0f), opto (40.0f, 2.0f), trimmed (deEss (50.0f, true), 5.6f) } },

            { "Streamer and broadcast", "Vocals", "Radio-close: rich lows, presence, firm compression and a little tape",
              { highPass (70.0f), denoise (12.0f), gate (-46.0f, 14.0f, 120.0f, 200.0f, true),
                eq ({ .lowShelfHz = 120.0f, .lowShelfDb = 3.0f, .b1Hz = 300.0f, .b1Db = -2.0f, .b3Hz = 3000.0f, .b3Db = 2.0f, .highShelfDb = 2.0f }),
                comp (punch, -22.0f, 5.0f, 3.0f, 80.0f, 4.0f), opto (45.0f, 2.5f), deEss (55.0f, false), exciter (15.0f),
                trimmed (saturation (1.0f, 3.0f, 30.0f), 3.2f) } },

            { "Live stage: lead", "Vocals", "For a stage mic, no added delay: expander with Focus, low make-up gain, echo on the tap tempo, ducked plate",
              { highPass (120.0f), gate (-44.0f, 8.0f, 100.0f, 200.0f, true), eq ({ .b1Db = -2.5f, .b3Hz = 3000.0f, .b3Db = 1.5f }),
                comp (glue, -20.0f, 3.0f, 10.0f, 150.0f, 1.0f), deEss (40.0f, false), echo (7.0f, 500.0f, 25.0f, 12.0f, 70.0f, 300.0f, 4500.0f),
                trimmed (plate (1.5f, 30.0f, 14.0f, 40.0f), 3.4f) } },

            { "Live stage: ballad", "Vocals", "Stage ballad, no added delay: opto levelling, a longer plate and a quiet echo",
              { highPass (120.0f), gate (-44.0f, 8.0f, 100.0f, 200.0f, true), eq ({ .b1Db = -2.5f, .b3Hz = 3000.0f, .b3Db = 1.5f }),
                opto (45.0f, 2.0f), deEss (40.0f, false), echo (7.0f, 500.0f, 25.0f, 8.0f, 70.0f, 300.0f, 4500.0f),
                trimmed (plate (2.4f, 40.0f, 20.0f, 40.0f), 3.0f) } },

            { "Vocal bus: glue", "Vocals", "For a group of vocal tracks: gentle glue, smoothing, light tape and a ceiling",
              { eq ({ .lowCut = 80.0f, .b1Hz = 300.0f, .b1Db = -1.0f }), smoothGentle(), comp (glue, -20.0f, 2.0f, 30.0f, 200.0f, 1.0f),
                deEss (30.0f, true), saturation (1.0f, 1.5f, 100.0f), trimmed ({ "limiter", { { "limCeiling", -1.0f } } }, 2.1f) }, true },

            { "Vocal plate return", "Vocals/Returns", "A return track's plate: fully wet, 2 s, lows cut",
              { { "plate", { { "plateMix", 100.0f }, { "plateDecay", 2.0f }, { "platePreDelay", 40.0f }, { "plateLowCut", 250.0f } } } }, true },

            { "Vocal delay return", "Vocals/Returns", "A return track's echo: fully wet, quarter notes, ducked",
              { echo (7.0f, 500.0f, 30.0f, 100.0f, 60.0f, 300.0f, 4500.0f) }, true },
        };

        // Name each chain's trim after its last effect ("plateOutGain"...).
        for (auto& chain : list)
            for (auto& device : chain.devices)
                for (auto& [id, value] : device.values)
                    if (id == nullptr)
                    {
                        static std::deque<juce::String> names; // kept alive for the char pointers
                        names.push_back (juce::String (device.type) + "OutGain");
                        id = names.back().toRawUTF8();
                    }
        return list;
    }();

    return chains;
}

juce::ValueTree makeInserts (const juce::String& name)
{
    for (const auto& chain : getChains())
    {
        if (name != chain.name)
            continue;

        juce::ValueTree inserts ("INSERTS");
        inserts.setProperty ("name", juce::String (chain.name), nullptr);

        for (const auto& device : chain.devices)
        {
            std::optional<fx::ModuleType> type;
            for (int t = 0; t < fx::numModuleTypes; ++t)
                if (juce::String (device.type) == fx::getModuleKey ((fx::ModuleType) t))
                    type = (fx::ModuleType) t;

            auto module = type.has_value() ? fx::createModule (*type) : nullptr;
            if (module == nullptr)
                continue;

            juce::ValueTree insert ("INSERT");
            insert.setProperty ("type", juce::String (device.type), nullptr);

            // Defaults first, the first switch on, then the chain's settings.
            bool switched = false;
            for (int v = 0; v < module->getNumValues(); ++v)
            {
                auto value = module->getValue (v).load();
                if (module->isSwitch (v) && ! switched)
                {
                    value = 1.0f;
                    switched = true;
                }
                insert.setProperty (juce::Identifier (module->getValueId (v)), value, nullptr);
            }
            for (const auto& [id, value] : device.values)
                insert.setProperty (juce::Identifier (id), value, nullptr);

            inserts.appendChild (insert, nullptr);
        }

        return inserts;
    }

    return {};
}

juce::var listChains()
{
    juce::Array<juce::var> result;

    for (const auto& chain : getChains())
    {
        const auto inserts = makeInserts (chain.name);
        juce::Array<juce::var> effects;
        for (const auto& insert : inserts)
            effects.add (insert["type"]);

        auto* entry = new juce::DynamicObject();
        entry->setProperty ("name", juce::String (chain.name));
        entry->setProperty ("folder", juce::String (chain.folder));
        entry->setProperty ("description", juce::String (chain.description));
        entry->setProperty ("effects", effects);
        entry->setProperty ("live", ! chain.dawOnly && PresetLibrary::isLiveChain (inserts));
        entry->setProperty ("factory", true);
        result.add (juce::var (entry));
    }

    return result;
}
}
