#include "MidiFileIO.h"

#include "Patterns.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace daw::midifile
{
namespace
{
    constexpr int ticksPerBeat = 480;
    constexpr double minNoteBeats = 1.0 / 64.0;
}

bool read (const juce::File& file, Song& song, juce::String& error)
{
    song = {};
    juce::FileInputStream input (file);
    if (! input.openedOk())
    {
        error = "Couldn't open " + file.getFileName() + ".";
        return false;
    }

    juce::MidiFile midi;
    if (! midi.readFrom (input))
    {
        error = file.getFileName() + " isn't a MIDI file.";
        return false;
    }

    const auto division = midi.getTimeFormat();
    if (division <= 0)
    {
        error = file.getFileName() + " counts time in frames, not beats; that kind of MIDI file isn't supported.";
        return false;
    }

    const auto toBeats = [division] (double ticks) { return ticks / (double) division; };
    const auto base = file.getFileNameWithoutExtension();
    std::map<double, double> tempoByBeat;
    bool signatureFound = false;

    for (int t = 0; t < midi.getNumTracks(); ++t)
    {
        juce::MidiMessageSequence sequence (*midi.getTrack (t));
        sequence.updateMatchedPairs();

        juce::String trackName;
        std::map<int, std::vector<Note>> byChannel;

        for (int i = 0; i < sequence.getNumEvents(); ++i)
        {
            const auto* event = sequence.getEventPointer (i);
            const auto& m = event->message;
            const auto beat = toBeats (m.getTimeStamp());

            if (m.isTrackNameEvent())
            {
                if (trackName.isEmpty())
                    trackName = m.getTextFromTextMetaEvent().trim();
            }
            else if (m.isTempoMetaEvent())
            {
                const auto seconds = m.getTempoSecondsPerQuarterNote();
                if (seconds > 0.0)
                    tempoByBeat[beat] = 60.0 / seconds;
            }
            else if (m.isTimeSignatureMetaEvent() && ! signatureFound)
            {
                int numerator = 4, denominator = 4;
                m.getTimeSignatureInfo (numerator, denominator);
                // A bar counts quarter notes here: 6/8 is two dotted quarters, 3 beats of eighths.
                song.beatsPerBar = juce::jlimit (2, 12, (int) std::lround (numerator * 4.0 / juce::jmax (1, denominator)));
                signatureFound = true;
            }
            else if (m.isNoteOn())
            {
                Note note;
                note.pitch = m.getNoteNumber();
                note.velocity = juce::jlimit (1, 127, (int) m.getVelocity());
                note.beat = beat;
                const auto* off = event->noteOffObject;
                note.beats = juce::jmax (minNoteBeats, off != nullptr ? toBeats (off->message.getTimeStamp()) - beat : 0.25);
                byChannel[m.getChannel()].push_back (note);
            }
        }

        for (auto& [channel, notes] : byChannel)
        {
            Part part;
            part.drums = channel == 10;
            part.name = trackName.isNotEmpty() ? trackName : base;
            if (byChannel.size() > 1)
                part.name << " ch " << channel;
            part.notes = std::move (notes);
            song.parts.push_back (std::move (part));
        }
    }

    if (song.parts.empty())
    {
        error = file.getFileName() + " has no notes.";
        return false;
    }

    for (const auto& [beat, bpm] : tempoByBeat)
        song.tempo.push_back ({ beat, bpm, false });

    return true;
}

bool write (const juce::File& file, const Song& song, juce::String& error)
{
    juce::MidiFile midi;
    midi.setTicksPerQuarterNote (ticksPerBeat);

    const auto ticks = [] (double beats) { return (double) std::llround (beats * ticksPerBeat); };

    juce::MidiMessageSequence conductor;
    conductor.addEvent (juce::MidiMessage::textMetaEvent (3, "Tempo"), 0.0);
    conductor.addEvent (juce::MidiMessage::timeSignatureMetaEvent (song.beatsPerBar, 4), 0.0);
    if (song.tempo.empty())
        conductor.addEvent (juce::MidiMessage::tempoMetaEvent (500000), 0.0);
    for (const auto& p : song.tempo)
        conductor.addEvent (juce::MidiMessage::tempoMetaEvent ((int) std::lround (60.0e6 / juce::jlimit (TempoMap::minBpm, TempoMap::maxBpm, p.bpm))),
                            ticks (p.beat));
    conductor.addEvent (juce::MidiMessage::endOfTrack(), 0.0);
    conductor.updateMatchedPairs();
    midi.addTrack (conductor);

    for (const auto& part : song.parts)
    {
        const auto channel = part.drums ? 10 : 1;
        juce::MidiMessageSequence sequence;
        sequence.addEvent (juce::MidiMessage::textMetaEvent (3, part.name), 0.0);

        double end = 0.0;
        for (const auto& n : part.notes)
        {
            const auto on = ticks (juce::jmax (0.0, n.beat));
            const auto off = juce::jmax (on + 1.0, ticks (juce::jmax (0.0, n.beat) + n.beats));
            sequence.addEvent (juce::MidiMessage::noteOn (channel, juce::jlimit (0, 127, n.pitch), (juce::uint8) juce::jlimit (1, 127, n.velocity)), on);
            sequence.addEvent (juce::MidiMessage::noteOff (channel, juce::jlimit (0, 127, n.pitch)), off);
            end = juce::jmax (end, off);
        }
        sequence.addEvent (juce::MidiMessage::endOfTrack(), end);
        // Note-offs before note-ons at the same tick, so a repeated note restarts.
        sequence.sort();
        midi.addTrack (sequence);
    }

    file.getParentDirectory().createDirectory();
    juce::TemporaryFile temp (file);
    {
        juce::FileOutputStream out (temp.getFile());
        if (! out.openedOk())
        {
            error = "Couldn't write " + file.getFileName() + ".";
            return false;
        }
        out.setPosition (0);
        out.truncate();
        if (! midi.writeTo (out, 1))
        {
            error = "Couldn't write " + file.getFileName() + ".";
            return false;
        }
    }

    if (! temp.overwriteTargetFileWithTemporary())
    {
        error = "Couldn't write " + file.getFileName() + ".";
        return false;
    }
    return true;
}

//==============================================================================
Placed addToEdit (Edit& edit, const Song& song, double startSeconds, int firstTrack, const Instruments& instruments, const juce::String& baseName)
{
    Placed placed;

    // An empty project takes the file's tempo (and bar length): the notes then sound as written.
    bool empty = true;
    for (const auto& track : edit.getTracks())
        for (const auto& child : track)
            if (child.hasType (ids::CLIP))
                empty = false;
    if (! edit.getPatternClips().isEmpty())
        empty = false;

    if (empty && ! song.tempo.empty())
    {
        edit.setTempoMap (TempoMap (song.tempo));
        placed.tempo = edit.getTempoMap().getPoints().front().bpm;
        edit.setBeatsPerBar (song.beatsPerBar);
    }

    const auto map = edit.getTempoMap();
    const auto bar = (double) edit.getBeatsPerBar();
    const auto startBeat = map.toBeats (juce::jmax (0.0, startSeconds));
    bool first = true;

    for (const auto& part : song.parts)
    {
        double endBeat = 0.0;
        for (const auto& n : part.notes)
            endBeat = juce::jmax (endBeat, n.beat + n.beats);
        const auto lengthBeats = juce::jmax (bar, std::ceil (endBeat / bar - 1.0e-6) * bar);

        int trackId = -1;
        if (first && ! part.drums && firstTrack >= 0 && Edit::isMidiTrack (edit.getTrack (firstTrack)))
            trackId = firstTrack;
        if (! part.drums)
            first = false;

        if (trackId < 0)
        {
            const auto& info = part.drums ? instruments.drums : instruments.melodic;
            const auto name = (part.name.isNotEmpty() ? part.name : baseName + " " + juce::String (placed.tracks.size() + 1)).substring (0, 60);
            trackId = (int) edit.addMidiTrack (name, info, -1)[ids::id];
        }

        const auto start = map.toSeconds (startBeat);
        auto clip = edit.addMidiClip (trackId, start, map.toSeconds (startBeat + lengthBeats) - start, part.name.isNotEmpty() ? part.name : baseName);
        if (! clip.isValid())
            continue;

        const auto clipId = (int) clip[ids::id];
        for (const auto& n : part.notes)
        {
            const auto on = map.toSeconds (startBeat + n.beat) - start;
            const auto length = map.toSeconds (startBeat + n.beat + n.beats) - start - on;
            edit.addNote (clipId, n.pitch, on, length, n.velocity);
        }

        placed.tracks.addIfNotAlreadyThere (trackId);
        placed.clips.add (clipId);
    }

    return placed;
}

//==============================================================================
namespace
{
    struct Timed { int pitch, velocity; double on, off; }; // seconds on the timeline

    void addClipNotes (const juce::ValueTree& clip, int trackTranspose, std::vector<Timed>& out)
    {
        const auto start = (double) clip[ids::start], offset = (double) clip[ids::offset], length = (double) clip[ids::length];
        const auto shift = trackTranspose + (int) std::lround ((double) clip.getProperty (ids::transpose, 0.0));
        for (const auto& note : clip)
        {
            if (! note.hasType (ids::NOTE))
                continue;
            const auto time = (double) note[ids::time];
            if (time < offset || time >= offset + length)
                continue;
            const auto on = start + time - offset;
            out.push_back ({ juce::jlimit (0, 127, (int) note[ids::pitch] + shift), juce::jlimit (1, 127, (int) note.getProperty (ids::velocity, 100)),
                             on, juce::jmin (start + length, on + (double) note[ids::length]) });
        }
    }

    // Beats from the bar line at or before the first note.
    Song build (const Edit& edit, const std::vector<Timed>& notes, const juce::String& name, bool drums)
    {
        Song song;
        song.beatsPerBar = edit.getBeatsPerBar();
        if (notes.empty())
            return song;

        const auto map = edit.getTempoMap();
        double first = 1.0e12;
        for (const auto& n : notes)
            first = juce::jmin (first, map.toBeats (n.on));
        const auto bar = (double) song.beatsPerBar;
        const auto origin = std::floor (first / bar + 1.0e-6) * bar;

        Part part;
        part.name = name;
        part.drums = drums;
        for (const auto& n : notes)
        {
            const auto beat = map.toBeats (n.on) - origin;
            part.notes.push_back ({ n.pitch, n.velocity, beat, juce::jmax (minNoteBeats, map.toBeats (n.off) - map.toBeats (n.on)) });
        }
        std::sort (part.notes.begin(), part.notes.end(), [] (const Note& a, const Note& b) { return a.beat < b.beat; });
        song.parts.push_back (std::move (part));

        // The tempo from the bar line on: what's in force there, then the points after it.
        song.tempo.push_back ({ 0.0, map.getBpmAtBeat (origin), false });
        for (const auto& p : map.getPoints())
            if (p.beat > origin + 1.0e-6)
                song.tempo.push_back ({ p.beat - origin, p.bpm, false });
        return song;
    }

    bool isDrumTrack (const juce::ValueTree& track)
    {
        const auto instrument = track.getChildWithName (ids::INSTRUMENT);
        return instrument.isValid() && instrument[ids::plugin].toString().endsWith (":beat");
    }
}

Song fromClip (const Edit& edit, int clipId)
{
    const auto clip = edit.getClip (clipId);
    if (! clip.isValid() || ! Edit::isMidiClip (clip))
        return {};

    const auto track = Edit::getOwnerTrack (clip);
    std::vector<Timed> notes;
    addClipNotes (clip, (int) std::lround ((double) track.getProperty (ids::transpose, 0.0)), notes);
    return build (edit, notes, clip[ids::name].toString(), isDrumTrack (track));
}

Song fromTrack (const Edit& edit, int trackId)
{
    const auto track = edit.getTrack (trackId);
    if (! track.isValid() || ! Edit::isMidiTrack (track))
        return {};

    const auto transpose = (int) std::lround ((double) track.getProperty (ids::transpose, 0.0));
    std::vector<Timed> notes;
    for (const auto& child : track)
        if (child.hasType (ids::CLIP) && Edit::isMidiClip (child) && ! (bool) child.getProperty (ids::mute, false))
            addClipNotes (child, transpose, notes);

    for (const auto& e : patterns::getBlockEvents (track.getParent(), trackId, edit.getTempoMap()))
        notes.push_back ({ juce::jlimit (0, 127, e.pitch + transpose), e.velocity, e.on, e.off });

    return build (edit, notes, track[ids::name].toString(), isDrumTrack (track));
}
}
