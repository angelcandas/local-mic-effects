#include "MidiEffects.h"

#include "Edit.h"

#include <algorithm>
#include <cmath>

namespace daw::midifx
{
namespace
{
const juce::Identifier shapeId { "shape" }, scaleId { "scale" }, rootId { "root" }, snapId { "snap" },
                       rateId { "rate" }, patternId { "pattern" }, octavesId { "octaves" }, gateId { "gate" },
                       latchId { "latch" }, levelId { "level" }, onId { "on" };

constexpr Pattern allPatterns[] { Pattern::up, Pattern::down, Pattern::upDown, Pattern::played, Pattern::random, Pattern::together };

bool inMask (int mask, int note) noexcept { return ((mask >> (((note % 12) + 12) % 12)) & 1) != 0; }

// The nearest note in the scale (ties go down), or -1 for an empty scale.
int nearestInScale (int mask, int note) noexcept
{
    if ((mask & 0xfff) == 0)
        return -1;
    for (int d = 0; d < 12; ++d)
    {
        if (note - d >= 0 && inMask (mask, note - d)) return note - d;
        if (note + d <= 127 && inMask (mask, note + d)) return note + d;
    }
    return -1;
}

void noteOn (juce::MidiBuffer& out, int note, int velocity, int at)
{
    out.addEvent (juce::MidiMessage::noteOn (1, note, (juce::uint8) juce::jlimit (1, 127, velocity)), at);
}

void noteOff (juce::MidiBuffer& out, int note, int at)
{
    out.addEvent (juce::MidiMessage::noteOff (1, note), at);
}

//==============================================================================
// Chord and scale: each played note becomes a set of notes, ended with it.
void releaseMapped (Processor::Stage& s, int note, juce::MidiBuffer& out, int at)
{
    for (auto& m : s.mapped[(size_t) note])
    {
        if (m < 0)
            continue;
        auto& count = s.outCount[(size_t) m];
        if (count > 0 && --count == 0)
            noteOff (out, m, at);
        m = -1;
    }
}

void playMapped (Processor::Stage& s, int note, int velocity, const int* notes, const int* velocities, int count,
                 juce::MidiBuffer& out, int at)
{
    releaseMapped (s, note, out, at); // the same key again before its note-off
    int slot = 0;
    for (int i = 0; i < count && slot < (int) s.mapped[(size_t) note].size(); ++i)
    {
        const auto n = notes[i];
        if (n < 0 || n > 127)
            continue;
        bool already = false; // a shape can land twice on one note (e.g. octaves of a scale)
        for (int j = 0; j < slot; ++j)
            already = already || s.mapped[(size_t) note][(size_t) j] == n;
        if (already)
            continue;
        noteOn (out, n, velocities != nullptr ? velocities[i] : velocity, at);
        if (s.outCount[(size_t) n] < 255)
            ++s.outCount[(size_t) n];
        s.mapped[(size_t) note][(size_t) slot++] = (std::int8_t) n;
    }
}

void runScale (Processor::Stage& s, const Settings& fx, const juce::MidiBuffer& in, juce::MidiBuffer& out)
{
    for (const auto metadata : in)
    {
        const auto message = metadata.getMessage();
        const auto at = metadata.samplePosition;
        if (message.isNoteOn())
        {
            const auto n = message.getNoteNumber();
            const auto target = fx.drop ? (inMask (fx.scaleMask, n) ? n : -1) : nearestInScale (fx.scaleMask, n);
            if (target >= 0)
                playMapped (s, n, message.getVelocity(), &target, nullptr, 1, out, at);
            else
                releaseMapped (s, n, out, at);
        }
        else if (message.isNoteOff())
            releaseMapped (s, message.getNoteNumber(), out, at);
        else
            out.addEvent (message, at);
    }
}

// The notes of the chord a note starts.
int chordNotes (const Settings& fx, int note, int* notes)
{
    if (fx.inKeyNotes > 0 && inMask (fx.scaleMask, note) && (fx.scaleMask & 0xfff) != 0)
    {
        // Every other note of the scale above it: the triad (or seventh) of the key on it.
        int count = 0, degree = 0;
        notes[count++] = note;
        for (int n = note + 1; n <= note + 24 && count < fx.inKeyNotes; ++n)
            if (inMask (fx.scaleMask, n) && (++degree % 2) == 0)
                notes[count++] = n;
        return count;
    }

    // A fixed shape (a note outside the key with inKey: the major triad, or the seventh).
    if (fx.inKeyNotes > 0)
    {
        const int triad[] { 0, 4, 7, 10 };
        for (int i = 0; i < fx.inKeyNotes; ++i)
            notes[i] = note + triad[i];
        return fx.inKeyNotes;
    }

    for (int i = 0; i < fx.numIntervals; ++i)
        notes[i] = note + fx.intervals[(size_t) i];
    return fx.numIntervals;
}

void runChord (Processor::Stage& s, const Settings& fx, const juce::MidiBuffer& in, juce::MidiBuffer& out)
{
    for (const auto metadata : in)
    {
        const auto message = metadata.getMessage();
        const auto at = metadata.samplePosition;
        if (message.isNoteOn())
        {
            const auto n = message.getNoteNumber();
            int notes[7], velocities[7];
            const auto count = chordNotes (fx, n, notes);
            for (int i = 0; i < count; ++i)
                velocities[i] = i == 0 ? message.getVelocity() : (int) std::lround (message.getVelocity() * fx.level);
            playMapped (s, n, message.getVelocity(), notes, velocities, count, out, at);
        }
        else if (message.isNoteOff())
            releaseMapped (s, message.getNoteNumber(), out, at);
        else
            out.addEvent (message, at);
    }
}

//==============================================================================
// The arpeggiator.
struct Arp
{
    Processor::Stage& s;
    const Settings& fx;
    const Context& ctx;
    juce::MidiBuffer& out;
    std::int64_t clock; // the block's first sample on the processor's clock
    double samplesPerStep;

    // The first grid line at or after `offset` samples into the block (playing).
    std::int64_t gridAt (int offset) const
    {
        const auto beat = ctx.beat + offset * ctx.bpm / (60.0 * ctx.sampleRate);
        const auto phase = beat / fx.rate;
        auto frac = phase - std::floor (phase);
        if (frac < 1.0e-4 || frac > 1.0 - 1.0e-4)
            frac = frac > 0.5 ? 1.0 : 0.0;
        const auto toNext = frac <= 0.0 ? 0.0 : (1.0 - frac) * samplesPerStep;
        return clock + offset + (std::int64_t) std::llround (toNext);
    }

    void release (std::int64_t at)
    {
        for (int i = 0; i < s.numSounding; ++i)
            noteOff (out, s.sounding[(size_t) i], (int) (at - clock));
        s.numSounding = 0;
        s.offAt = -1;
    }

    void play (int note, int velocity, std::int64_t at)
    {
        while (note > 127) note -= 12;
        for (int i = 0; i < s.numSounding; ++i)
            if (s.sounding[(size_t) i] == note)
                return;
        if (s.numSounding >= (int) s.sounding.size())
            return;
        noteOn (out, note, velocity, (int) (at - clock));
        s.sounding[(size_t) s.numSounding++] = (std::int8_t) note;
    }

    void doStep (std::int64_t at)
    {
        release (at);
        if (s.numHeld == 0)
        {
            s.nextStep = -1;
            return;
        }

        // The held notes low to high (as played for that pattern), then up the octaves.
        std::array<Processor::Stage::Held, 32> order;
        std::copy (s.held.begin(), s.held.begin() + s.numHeld, order.begin());
        if (fx.pattern != Pattern::played)
            std::sort (order.begin(), order.begin() + s.numHeld, [] (auto x, auto y) { return x.note < y.note; });

        const auto octaves = juce::jlimit (1, 4, fx.octaves);
        const auto total = s.numHeld * octaves;
        const auto noteAt = [&] (int i) { return std::pair { order[(size_t) (i % s.numHeld)].note + 12 * (i / s.numHeld), (int) order[(size_t) (i % s.numHeld)].velocity }; };

        if (fx.pattern == Pattern::together)
        {
            const auto octave = s.step % octaves;
            for (int i = 0; i < s.numHeld; ++i)
                play (order[(size_t) i].note + 12 * octave, order[(size_t) i].velocity, at);
        }
        else
        {
            int index = 0;
            switch (fx.pattern)
            {
                case Pattern::down:   index = total - 1 - s.step % total; break;
                case Pattern::upDown:
                {
                    const auto period = juce::jmax (1, 2 * total - 2);
                    const auto p = s.step % period;
                    index = total < 2 ? 0 : (p < total ? p : period - p);
                    break;
                }
                case Pattern::random:
                    s.random ^= s.random << 13; s.random ^= s.random >> 17; s.random ^= s.random << 5;
                    index = (int) (s.random % (std::uint32_t) total);
                    break;
                case Pattern::up: case Pattern::played: case Pattern::together: default:
                    index = s.step % total;
                    break;
            }
            const auto [note, velocity] = noteAt (index);
            play (note, velocity, at);
        }

        ++s.step;
        s.lastStep = at;
        const auto length = (std::int64_t) std::llround (samplesPerStep);
        s.offAt = fx.gate >= 0.999f ? -1 : at + juce::jmax ((std::int64_t) 1, (std::int64_t) std::llround (samplesPerStep * fx.gate));
        s.nextStep = at + juce::jmax ((std::int64_t) 1, length);
    }

    // The steps and note ends before `limit` (on the processor's clock).
    void runUntil (std::int64_t limit)
    {
        for (;;)
        {
            const auto off = s.offAt >= 0 && s.offAt < limit ? s.offAt : -1;
            const auto next = s.nextStep >= 0 && s.nextStep < limit ? s.nextStep : -1;
            if (off < 0 && next < 0)
                return;
            if (off >= 0 && (next < 0 || off <= next))
                release (off);
            else
                doStep (next);
        }
    }

    void pressed (int note, int velocity, int offset)
    {
        if (s.latched && s.numDown == 0)
        {
            s.numHeld = 0; // a new chord replaces the latched one
            s.latched = false;
        }

        ++s.numDown;
        for (int i = 0; i < s.numHeld; ++i)
            if (s.held[(size_t) i].note == note)
                return;
        if (s.numHeld >= (int) s.held.size())
            return;

        const auto first = s.numHeld == 0;
        s.held[(size_t) s.numHeld++] = { (std::int8_t) note, (std::uint8_t) juce::jlimit (1, 127, velocity) };
        if (first)
        {
            s.step = 0;
            // On the beat grid while playing; straight away when stopped.
            s.nextStep = ctx.playing ? gridAt (offset) : clock + offset;
        }
    }

    void letGo (int note, int offset)
    {
        s.numDown = juce::jmax (0, s.numDown - 1);
        if (fx.latch)
        {
            if (s.numDown == 0)
                s.latched = true;
            return;
        }

        for (int i = 0; i < s.numHeld; ++i)
            if (s.held[(size_t) i].note == note)
            {
                std::copy (s.held.begin() + i + 1, s.held.begin() + s.numHeld, s.held.begin() + i);
                --s.numHeld;
                break;
            }

        if (s.numHeld == 0)
        {
            s.nextStep = -1;
            s.numDown = 0;
            if (s.offAt < 0) // legato: nothing would end it
                release (clock + offset);
        }
    }

    void run (const juce::MidiBuffer& in, int numSamples)
    {
        if (ctx.playing)
        {
            // Back on the grid each block (tempo changes, loops, jumps), without playing a
            // step twice where the last block ended on one.
            if (s.nextStep >= 0)
            {
                auto next = gridAt (0);
                if (s.lastStep >= 0 && next - s.lastStep < (std::int64_t) (samplesPerStep * 0.5))
                    next += (std::int64_t) std::llround (samplesPerStep);
                s.nextStep = next;
            }
        }
        else if (s.wasPlaying && s.latched)
        {
            // Stopping ends a latched arp.
            s.numHeld = s.numDown = 0;
            s.latched = false;
            s.nextStep = -1;
        }
        s.wasPlaying = ctx.playing;

        if (! fx.latch && s.latched)
        {
            // Latch switched off: the notes it kept go.
            s.numHeld = s.numDown = 0;
            s.latched = false;
            s.nextStep = -1;
        }

        for (const auto metadata : in)
        {
            const auto message = metadata.getMessage();
            const auto at = metadata.samplePosition;
            runUntil (clock + at);
            if (message.isNoteOn())
                pressed (message.getNoteNumber(), message.getVelocity(), at);
            else if (message.isNoteOff())
                letGo (message.getNoteNumber(), at);
            else
                out.addEvent (message, at);
        }
        runUntil (clock + numSamples);
    }
};
} // namespace

//==============================================================================
const char* typeKey (Type type) noexcept
{
    switch (type)
    {
        case Type::arp:   return "arp";
        case Type::chord: return "chord";
        case Type::scale: return "scale";
    }
    return "arp";
}

bool typeFromKey (const juce::String& key, Type& type) noexcept
{
    for (auto t : { Type::arp, Type::chord, Type::scale })
        if (key == typeKey (t))
        {
            type = t;
            return true;
        }
    return false;
}

const char* patternKey (Pattern pattern) noexcept
{
    switch (pattern)
    {
        case Pattern::up:       return "up";
        case Pattern::down:     return "down";
        case Pattern::upDown:   return "upDown";
        case Pattern::played:   return "played";
        case Pattern::random:   return "random";
        case Pattern::together: return "together";
    }
    return "up";
}

const std::vector<Shape>& getShapes()
{
    static const std::vector<Shape> shapes {
        { "key", "Triad in key", { 0 }, 0 },
        { "key7", "Seventh in key", { 0 }, 0 },
        { "major", "Major", { 0, 4, 7 }, 3 },
        { "minor", "Minor", { 0, 3, 7 }, 3 },
        { "power", "Power (5th + octave)", { 0, 7, 12 }, 3 },
        { "sus2", "Sus2", { 0, 2, 7 }, 3 },
        { "sus4", "Sus4", { 0, 5, 7 }, 3 },
        { "seventh", "7th", { 0, 4, 7, 10 }, 4 },
        { "major7", "Major 7th", { 0, 4, 7, 11 }, 4 },
        { "minor7", "Minor 7th", { 0, 3, 7, 10 }, 4 },
        { "add9", "Add 9", { 0, 4, 7, 14 }, 4 },
        { "octave", "Octave", { 0, 12 }, 2 },
        { "octaves", "Two octaves", { 0, 12, 24 }, 3 },
        { "fifth", "Fifth", { 0, 7 }, 2 },
    };
    return shapes;
}

const std::vector<Scale>& getScales()
{
    // Bit n = n semitones above the root.
    static const std::vector<Scale> scales {
        { "major", "Major", 0xAB5 },
        { "minor", "Minor", 0x5AD },
        { "harmonicMinor", "Harmonic minor", 0x9AD },
        { "dorian", "Dorian", 0x6AD },
        { "mixolydian", "Mixolydian", 0x6B5 },
        { "pentatonic", "Pentatonic major", 0x295 },
        { "pentatonicMinor", "Pentatonic minor", 0x4A9 },
        { "blues", "Blues", 0x4E9 },
    };
    return scales;
}

int rotateMask (int mask, int root) noexcept
{
    root = ((root % 12) + 12) % 12;
    mask &= 0xfff;
    return ((mask << root) | (mask >> (12 - root))) & 0xfff;
}

int getSongScaleMask (int key, bool minor) noexcept
{
    if (key < 0 || key > 11)
        return 0xAB5;
    return rotateMask (minor ? 0x5AD : 0xAB5, key);
}

Settings fromTree (const juce::ValueTree& tree, int songKey, bool songMinor)
{
    Settings fx;
    fx.id = (int) tree[ids::id];
    typeFromKey (tree[ids::type].toString(), fx.type);
    fx.on = (bool) tree.getProperty (onId, true);

    fx.rate = juce::jlimit (1.0 / 32.0, 4.0, (double) tree.getProperty (rateId, 0.25));
    const auto pattern = tree.getProperty (patternId, "up").toString();
    for (auto p : allPatterns)
        if (pattern == patternKey (p))
            fx.pattern = p;
    fx.octaves = juce::jlimit (1, 4, (int) tree.getProperty (octavesId, 1));
    fx.gate = juce::jlimit (0.05f, 1.0f, (float) (double) tree.getProperty (gateId, 0.8));
    fx.latch = (bool) tree.getProperty (latchId, false);

    const auto shape = tree.getProperty (shapeId, "key").toString();
    for (const auto& s : getShapes())
        if (shape == s.key)
        {
            fx.intervals = s.intervals;
            fx.numIntervals = s.count;
            fx.inKeyNotes = s.count == 0 ? (shape == "key7" ? 4 : 3) : 0;
        }
    fx.level = juce::jlimit (0.0f, 1.0f, (float) (double) tree.getProperty (levelId, 0.85));

    const auto scale = tree.getProperty (scaleId, "song").toString();
    fx.scaleMask = getSongScaleMask (songKey, songMinor);
    if (scale != "song")
        for (const auto& s : getScales())
            if (scale == s.key)
                fx.scaleMask = rotateMask (s.mask, (int) tree.getProperty (rootId, 0));
    fx.drop = tree.getProperty (snapId, "nearest").toString() == "drop";
    return fx;
}

Chain fromTrack (const juce::ValueTree& track, int songKey, bool songMinor)
{
    Chain chain;
    for (const auto& child : track)
        if (child.hasType (ids::MIDIFX) && (int) chain.size() < maxEffects)
            chain.push_back (fromTree (child, songKey, songMinor));
    return chain;
}

//==============================================================================
void Processor::Stage::reset() noexcept
{
    for (auto& m : mapped)
        m.fill (-1);
    outCount.fill (0);
    numHeld = numDown = numSounding = 0;
    latched = false;
    nextStep = offAt = lastStep = -1;
    step = 0;
    wasPlaying = false;
}

Processor::Processor()
{
    for (auto& stage : stages)
        stage.reset();
    prepare();
}

void Processor::prepare (int maxEventsHint)
{
    a.ensureSize ((size_t) maxEventsHint * 8);
    b.ensureSize ((size_t) maxEventsHint * 8);
}

void Processor::flush (juce::MidiBuffer& midi, int at) noexcept
{
    for (int n = 0; n < 128; ++n)
        if (out[(size_t) n])
            noteOff (midi, n, at);
    out.reset();
    for (auto& stage : stages)
        stage.reset();
}

void Processor::process (juce::MidiBuffer& midi, const Chain& chain, const Context& ctx, int numSamples) noexcept
{
    // The effects that play, as ids (one switched off is a different chain).
    std::array<int, maxEffects> now {};
    std::array<const Settings*, maxEffects> settings {};
    int count = 0;
    for (const auto& fx : chain)
        if (fx.on && count < maxEffects)
        {
            settings[(size_t) count] = &fx;
            now[(size_t) count++] = fx.id;
        }

    if (count != numLayout || ! std::equal (now.begin(), now.begin() + count, layout.begin()))
    {
        // A different chain: what the old one held ends here; notes still held go on
        // as they are (their note-offs find nothing to end).
        a.clear();
        flush (a, 0);
        a.addEvents (midi, 0, -1, 0);
        midi.swapWith (a);
        for (int i = 0; i < count; ++i)
        {
            stages[(size_t) i].id = now[(size_t) i];
            stages[(size_t) i].type = settings[(size_t) i]->type;
        }
        layout = now;
        numLayout = count;
    }

    active = count > 0;
    if (count == 0)
    {
        clock += numSamples;
        return;
    }

    const juce::MidiBuffer* source = &midi;
    juce::MidiBuffer* targets[] { &a, &b };
    juce::MidiBuffer* result = nullptr;

    for (int i = 0; i < count; ++i)
    {
        auto& target = *targets[i % 2];
        target.clear();
        auto& stage = stages[(size_t) i];
        const auto& fx = *settings[(size_t) i];

        switch (fx.type)
        {
            case Type::scale: runScale (stage, fx, *source, target); break;
            case Type::chord: runChord (stage, fx, *source, target); break;
            case Type::arp:
            {
                const auto bpm = juce::jlimit (20.0, 400.0, ctx.bpm);
                Arp arp { stage, fx, ctx, target, clock, juce::jmax (1.0, fx.rate * 60.0 / bpm * ctx.sampleRate) };
                arp.run (*source, numSamples);
                break;
            }
        }

        source = result = &target;
    }

    for (const auto metadata : *result)
    {
        const auto message = metadata.getMessage();
        if (message.isNoteOn())       out.set ((size_t) message.getNoteNumber());
        else if (message.isNoteOff()) out.reset ((size_t) message.getNoteNumber());
    }

    midi.swapWith (*result);
    clock += numSamples;
}
} // namespace daw::midifx
