#include "StarterContent.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <cmath>

namespace micfx::starter
{
namespace
{
    constexpr int version = 1;
    constexpr double rate = 44100.0;
    constexpr double twoPi = 6.283185307179586;

    using Buf = std::vector<float>;

    Buf make (double seconds) { return Buf ((size_t) (seconds * rate), 0.0f); }
    double timeAt (size_t i) { return (double) i / rate; }
    double decayEnv (double t, double decay) { return std::exp (-t / decay); }

    // One-pole filters over a buffer.
    void lowPass (Buf& b, double hz)
    {
        const auto a = 1.0 - std::exp (-twoPi * hz / rate);
        double y = 0.0;
        for (auto& v : b) { y += a * (v - y); v = (float) y; }
    }

    void highPass (Buf& b, double hz)
    {
        const auto a = 1.0 - std::exp (-twoPi * hz / rate);
        double y = 0.0;
        for (auto& v : b) { y += a * (v - y); v = (float) (v - y); }
    }

    Buf noise (double seconds, int seed)
    {
        juce::Random random (seed);
        auto b = make (seconds);
        for (auto& v : b) v = random.nextFloat() * 2.0f - 1.0f;
        return b;
    }

    void add (Buf& into, const Buf& from, double gain = 1.0, size_t offset = 0)
    {
        for (size_t i = 0; i < from.size() && i + offset < into.size(); ++i)
            into[i + offset] += (float) (from[i] * gain);
    }

    // A pitch that falls from `from` to `to` (exponentially, `sweep` seconds), as a decaying sine.
    Buf sweptSine (double seconds, double from, double to, double sweep, double decay)
    {
        auto b = make (seconds);
        double phase = 0.0;
        for (size_t i = 0; i < b.size(); ++i)
        {
            const auto t = timeAt (i);
            phase += twoPi * (to + (from - to) * std::exp (-t / sweep)) / rate;
            b[i] = (float) (std::sin (phase) * decayEnv (t, decay));
        }
        return b;
    }

    Buf shaped (Buf b, double decay)
    {
        for (size_t i = 0; i < b.size(); ++i)
            b[i] *= (float) decayEnv (timeAt (i), decay);
        return b;
    }

    // Six square waves at unrelated pitches: the metallic part of hats and cymbals.
    Buf metal (double seconds, double scale)
    {
        static const double freqs[] = { 205.3, 304.4, 369.6, 522.7, 540.0, 800.0 };
        auto b = make (seconds);
        for (const auto f : freqs)
        {
            double phase = 0.0;
            for (auto& v : b)
            {
                phase += f * scale / rate;
                phase -= std::floor (phase);
                v += phase < 0.5 ? 0.2f : -0.2f;
            }
        }
        return b;
    }

    struct Kit
    {
        const char* name;
        double kickFrom, kickTo, kickSweep, kickDecay, kickClick;
        double snareTone, snareNoise, snareDecay, snareHz;
        double clapDecay;
        double hatTone, hatShort, hatLong;
        double tomHz, tomDecay;
        double crashDecay, rideDecay;
    };

    const Kit kits[] = {
        { "Studio Kit", 160.0, 52.0, 0.035, 0.32, 0.5,  0.6, 0.8, 0.20, 190.0, 0.10, 1.0, 0.045, 0.35, 100.0, 0.30, 1.4, 0.9 },
        { "808 Kit",    130.0, 42.0, 0.060, 0.75, 0.15, 0.9, 0.5, 0.26, 170.0, 0.16, 1.5, 0.035, 0.22, 80.0,  0.55, 1.0, 0.6 },
    };

    // The names in the Kit instrument's pad order (pads 1-16 on notes 36-51).
    const char* const padNames[16] = { "Kick", "Rim", "Snare", "Clap", "Snare Soft", "Floor Tom", "Hat Closed", "Low Tom",
                                       "Hat Pedal", "Mid Tom", "Hat Open", "High Tom", "Crash", "Tom Hi", "Ride", "Ride Bell" };

    Buf makeSound (const Kit& k, int pad)
    {
        switch (pad)
        {
            case 0: // Kick
            {
                auto b = sweptSine (0.9, k.kickFrom, k.kickTo, k.kickSweep, k.kickDecay);
                add (b, shaped (noise (0.9, 11), 0.003), k.kickClick * 0.4);
                for (auto& v : b) v = (float) std::tanh (1.6 * v);
                return b;
            }
            case 1: // Rim
            {
                auto b = shaped (sweptSine (0.2, 1900.0, 1200.0, 0.01, 0.02), 0.02);
                add (b, shaped (sweptSine (0.2, 520.0, 440.0, 0.02, 0.03), 0.03), 0.7);
                add (b, shaped (noise (0.2, 12), 0.004), 0.5);
                return b;
            }
            case 2: case 4: // Snare, soft snare
            {
                const auto soft = pad == 4;
                const auto hz = k.snareHz * (soft ? 0.92 : 1.0);
                auto b = shaped (sweptSine (0.6, hz * 1.5, hz, 0.02, 0.07), 0.07);
                add (b, shaped (sweptSine (0.6, hz * 1.9, hz * 1.75, 0.02, 0.05), 0.05), 0.4);
                for (auto& v : b) v *= (float) k.snareTone;
                auto n = noise (0.6, soft ? 14 : 13);
                highPass (n, 1400.0);
                add (b, shaped (n, k.snareDecay * (soft ? 0.7 : 1.0)), k.snareNoise * (soft ? 0.7 : 1.0));
                return b;
            }
            case 3: // Clap: four short bursts and a tail
            {
                auto b = make (0.6);
                auto n = noise (0.6, 15);
                highPass (n, 700.0);
                lowPass (n, 3800.0);
                const double starts[] = { 0.0, 0.012, 0.025, 0.039 };
                for (const auto s : starts)
                {
                    Buf burst (n.begin(), n.begin() + (long) (0.03 * rate));
                    add (b, shaped (burst, 0.006), 1.0, (size_t) (s * rate));
                }
                Buf tail (n.begin() + (long) (0.05 * rate), n.end());
                add (b, shaped (tail, k.clapDecay), 0.9, (size_t) (0.042 * rate));
                return b;
            }
            case 5: case 7: case 9: case 11: case 13: // Toms, from low to high
            {
                const auto step = (pad - 5) / 2;
                const auto hz = k.tomHz * std::pow (1.28, step);
                auto b = sweptSine (1.2, hz * 1.5, hz, 0.04, k.tomDecay * (1.0 - 0.1 * step));
                add (b, shaped (noise (1.2, 16 + step), 0.004), 0.25);
                return b;
            }
            case 6: case 8: case 10: // Hats: closed, pedal, open
            {
                const auto decay = pad == 6 ? k.hatShort : pad == 8 ? k.hatShort * 1.8 : k.hatLong;
                auto b = metal (1.0, 1.0 + (pad == 8 ? 0.03 : 0.0));
                highPass (b, 6500.0);
                auto n = noise (1.0, 20 + pad);
                highPass (n, 7500.0);
                add (b, n, 0.35);
                return shaped (b, decay);
            }
            case 12: // Crash
            {
                auto b = metal (2.5, 1.37);
                add (b, noise (2.5, 30), 0.5);
                highPass (b, 3200.0);
                for (size_t i = 0; i < b.size(); ++i)
                    b[i] *= (float) (decayEnv (timeAt (i), k.crashDecay) * (1.0 - std::exp (-timeAt (i) / 0.002)));
                return b;
            }
            case 14: // Ride
            {
                auto b = metal (1.6, 1.8);
                highPass (b, 4500.0);
                add (b, shaped (noise (1.6, 31), 0.03), 0.4);
                auto ping = shaped (sweptSine (1.6, 3300.0, 3000.0, 0.01, 0.08), 0.08);
                add (b, ping, 0.4);
                return shaped (b, k.rideDecay);
            }
            default: // Ride bell
            {
                auto b = make (1.6);
                const double partials[][2] = { { 560.0, 1.0 }, { 1130.0, 0.6 }, { 1690.0, 0.45 }, { 2420.0, 0.35 }, { 3560.0, 0.25 } };
                for (const auto& p : partials)
                    add (b, shaped (sweptSine (1.6, p[0], p[0], 0.1, 0.5 / (1.0 + p[0] / 2000.0)), 10.0), p[1]);
                add (b, shaped (noise (1.6, 32), 0.004), 0.3);
                return b;
            }
        }
    }

    // Peak 0.85, a short fade at the end so nothing clicks, 16-bit mono.
    bool writeWav (const juce::File& file, Buf b, double peakGain)
    {
        float peak = 0.0f;
        for (const auto v : b) peak = juce::jmax (peak, std::abs (v));
        if (peak <= 0.0f) return false;
        const auto fade = (size_t) (0.01 * rate);
        for (size_t i = 0; i < b.size(); ++i)
        {
            auto g = (float) (0.85 * peakGain / peak);
            if (i + fade > b.size()) g *= (float) (b.size() - i) / (float) fade;
            b[i] *= g;
        }

        // Trim the silent end.
        auto end = b.size();
        while (end > 1000 && std::abs (b[end - 1]) < 0.0005f) --end;
        b.resize (end);

        file.getParentDirectory().createDirectory();
        file.deleteFile();
        std::unique_ptr<juce::FileOutputStream> stream (file.createOutputStream());
        if (stream == nullptr) return false;
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::AudioFormatWriter> writer (wav.createWriterFor (stream.get(), rate, 1, 16, {}, 0));
        if (writer == nullptr) return false;
        stream.release();
        juce::AudioBuffer<float> buffer (1, (int) b.size());
        buffer.copyFrom (0, 0, b.data(), (int) b.size());
        return writer->writeFromAudioSampleBuffer (buffer, 0, buffer.getNumSamples());
    }

    // Relative levels: hats and cymbals sit under the drums.
    double levelOf (int pad)
    {
        switch (pad)
        {
            case 6: case 8: return 0.55;
            case 10: return 0.6;
            case 12: case 14: case 15: return 0.65;
            case 1: return 0.7;
            default: return 1.0;
        }
    }

    juce::File kitFolder (const juce::String& kit) { return getFolder().getChildFile (kit); }
    juce::File soundFile (const juce::String& kit, int pad) { return kitFolder (kit).getChildFile (juce::String (padNames[pad]) + ".wav"); }
    juce::File versionFile() { return getFolder().getChildFile ("version.txt"); }

    //==========================================================================
    // Patterns. A drum bar is 16 sixteenths: x hit, X accent, o soft; anything else is a rest.
    struct Row { int pitch; const char* steps; };

    Pattern drums (const char* name, double bars, std::initializer_list<Row> rows)
    {
        Pattern p { name, "Drums", 4.0 * bars, {}, true };
        for (const auto& row : rows)
        {
            const juce::String steps (row.steps);
            for (int i = 0; i < steps.length(); ++i)
            {
                const auto c = steps[i];
                const auto velocity = c == 'X' ? 118 : c == 'x' ? 100 : c == 'o' ? 55 : 0;
                if (velocity > 0)
                    p.notes.push_back ({ row.pitch, i * 0.25, 0.2, velocity });
            }
        }
        return p;
    }

    void chord (std::vector<Note>& out, double time, double length, std::initializer_list<int> pitches, int velocity = 85)
    {
        for (const auto pitch : pitches)
            out.push_back ({ pitch, time, length, velocity });
    }

    std::vector<Pattern> buildPatterns()
    {
        constexpr int K = 36, RIM = 37, SN = 38, CL = 39, HH = 42, PH = 44, OH = 46, CR = 48, T1 = 41, T2 = 43, T3 = 45, T4 = 47;
        std::vector<Pattern> list;

        list.push_back (drums ("Rock Beat", 1, { { K, "x.......x.x....." }, { SN, "....x.......x..." }, { HH, "x.x.x.x.x.x.x.x." } }));
        list.push_back (drums ("Pop Backbeat", 1, { { K, "x.....x...x....." }, { SN, "....x.......x..." }, { HH, "xoxoxoxoxoxoxoxo" } }));
        list.push_back (drums ("Boom Bap", 1, { { K, "x.....x...x....." }, { SN, "....X.......X..." }, { HH, "x.x.x.x.x.x.x.xo" }, { SN, "..............o." } }));
        list.push_back (drums ("Four on the Floor", 1, { { K, "x...x...x...x..." }, { CL, "....x.......x..." }, { OH, "..x...x...x...x." }, { HH, "x.x.x.x.x.x.x.x." } }));
        list.push_back (drums ("Half-time Ballad", 2, { { K, "x.......x.x.....x.......x.x....." }, { RIM, "........x.......x.......x......." },
                                                       { SN, "........x.......x......." }, { HH, "x.x.x.x.x.x.x.x.x.x.x.x.x.x.x.x." } }));
        list.push_back (drums ("Trap Hats", 1, { { K, "x.......x.x....." }, { SN, "........x......." }, { CL, "........x......." },
                                                 { HH, "xxoxxoxxoxxxxoxo" } }));
        list.push_back (drums ("Funk Groove", 1, { { K, "x..x..x...x.x..." }, { SN, "....X..o.o..X..o" }, { HH, "xoxoxoxoxoxoxoxo" }, { OH, "..............x." } }));
        list.push_back (drums ("Shuffle", 1, { { K, "x.....x.x.....x." }, { SN, "....x.......x..." }, { HH, "x..x..x..x..x..x" } }));
        list.push_back (drums ("Tom Fill", 1, { { SN, "xxxx............" }, { T4, "....xx.........." }, { T3, "......xx........" },
                                                { T2, "........xx......" }, { T1, "..........xx...." }, { CR, "............X..." }, { K, "............x..." } }));

        // Bass: A minor, an octave and a bit under middle C.
        auto bass = [&] (const char* name, std::initializer_list<Note> notes)
        {
            Pattern p { name, "Bass", 4.0, {}, false };
            for (const auto& n : notes) p.notes.push_back (n);
            list.push_back (p);
        };
        bass ("Root Eighths", { { 33, 0.0, 0.45, 100 }, { 33, 0.5, 0.45, 80 }, { 33, 1.0, 0.45, 95 }, { 33, 1.5, 0.45, 80 }, { 33, 2.0, 0.45, 100 },
                                { 33, 2.5, 0.45, 80 }, { 36, 3.0, 0.45, 95 }, { 38, 3.5, 0.45, 85 } });
        bass ("Funk Bass", { { 33, 0.0, 0.2, 110 }, { 45, 0.5, 0.2, 80 }, { 33, 0.75, 0.2, 95 }, { 36, 1.5, 0.2, 100 }, { 33, 2.0, 0.2, 110 },
                             { 45, 2.5, 0.2, 80 }, { 40, 3.0, 0.2, 100 }, { 38, 3.25, 0.2, 90 }, { 36, 3.5, 0.45, 100 } });
        bass ("Disco Octaves", { { 33, 0.0, 0.22, 100 }, { 45, 0.5, 0.22, 100 }, { 33, 1.0, 0.22, 100 }, { 45, 1.5, 0.22, 100 }, { 33, 2.0, 0.22, 100 },
                                 { 45, 2.5, 0.22, 100 }, { 33, 3.0, 0.22, 100 }, { 45, 3.5, 0.22, 100 } });
        bass ("Slow Sub", { { 33, 0.0, 3.4, 105 } });

        // Chords: block chords, four bars each.
        auto progression = [&] (const char* name, std::initializer_list<std::initializer_list<int>> chords, int velocity)
        {
            Pattern p { name, "Chords", 16.0, {}, false };
            double t = 0.0;
            for (const auto& c : chords) { chord (p.notes, t, 3.9, c, velocity); t += 4.0; }
            list.push_back (p);
        };
        progression ("Pop Progression (C G Am F)", { { 60, 64, 67 }, { 55, 59, 62, 67 }, { 57, 60, 64 }, { 53, 57, 60, 65 } }, 82);
        progression ("Sad Minor (Am F C G)", { { 57, 60, 64 }, { 53, 57, 60 }, { 60, 64, 67 }, { 55, 59, 62 } }, 78);
        progression ("Jazz ii-V-I (Dm7 G7 Cmaj7)", { { 50, 53, 57, 60 }, { 55, 59, 62, 65 }, { 48, 52, 55, 59 }, { 48, 52, 55, 59 } }, 76);
        progression ("Gospel Lift (C Am F G)", { { 48, 55, 60, 64, 67 }, { 45, 52, 57, 60, 64 }, { 41, 53, 57, 60, 65 }, { 43, 55, 59, 62, 67 } }, 88);

        {
            Pattern p { "Arpeggio (Am)", "Chords", 4.0, {}, false };
            const int up[] = { 57, 60, 64, 69, 64, 60, 64, 60 };
            for (int i = 0; i < 8; ++i)
                p.notes.push_back ({ up[i], i * 0.5, 0.45, i % 2 == 0 ? 90 : 72 });
            list.push_back (p);
        }
        {
            Pattern p { "Pentatonic Hook", "Melody", 8.0, {}, false };
            const Note hook[] = { { 69, 0.0, 0.5, 100 }, { 72, 0.5, 0.5, 90 }, { 74, 1.0, 1.0, 100 }, { 72, 2.0, 0.5, 90 }, { 69, 2.5, 0.5, 85 },
                                  { 67, 3.0, 1.0, 95 }, { 64, 4.0, 0.5, 100 }, { 67, 4.5, 0.5, 90 }, { 69, 5.0, 1.5, 100 }, { 67, 6.5, 0.5, 80 },
                                  { 64, 7.0, 0.9, 90 } };
            for (const auto& n : hook) p.notes.push_back (n);
            list.push_back (p);
        }
        return list;
    }
}

juce::File getFolder()
{
   #if JUCE_MAC
    const auto root = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("Application Support/MicFX");
   #else
    const auto root = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("MicFX");
   #endif
    return root.getChildFile ("Starter");
}

juce::StringArray getKitNames()
{
    juce::StringArray names;
    for (const auto& k : kits) names.add (k.name);
    return names;
}

std::vector<juce::File> getKitFiles (const juce::String& kit)
{
    std::vector<juce::File> files;
    if (! getKitNames().contains (kit)) return files;
    for (int pad = 0; pad < 16; ++pad)
    {
        const auto file = soundFile (kit, pad);
        files.push_back (file.existsAsFile() ? file : juce::File());
    }
    return files;
}

bool ensure()
{
    static juce::CriticalSection lock;
    const juce::ScopedLock scoped (lock);

    const auto current = versionFile().existsAsFile() && versionFile().loadFileAsString().trim().getIntValue() == version;
    auto complete = current;
    if (complete)
        for (const auto& k : kits)
            for (int pad = 0; pad < 16 && complete; ++pad)
                complete = soundFile (k.name, pad).existsAsFile();
    if (complete)
        return true;

    for (const auto& k : kits)
        for (int pad = 0; pad < 16; ++pad)
            if (! writeWav (soundFile (k.name, pad), makeSound (k, pad), levelOf (pad)))
                return false;
    return versionFile().replaceWithText (juce::String (version) + "\n");
}

const std::vector<Pattern>& getPatterns()
{
    static const auto list = buildPatterns();
    return list;
}
}
