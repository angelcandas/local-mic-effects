#include "Patterns.h"

// The pattern editing part of Edit (declared in Edit.h), and how patterns play.
namespace daw
{
namespace
{
    // Pattern colours, picked in turn for new patterns.
    constexpr const char* patternColours[] { "#e8875b", "#5bb3e8", "#9be85b", "#e85b9b", "#e8d35b", "#9b7be8", "#5be8c4", "#e85b5b" };

    juce::String rowsToString (const juce::Array<int>& notes)
    {
        juce::StringArray list;
        for (auto note : notes)
            list.add (juce::String (juce::jlimit (0, 127, note)));
        return list.joinIntoString (",");
    }
}

//==============================================================================
juce::ValueTree Edit::addPattern (const juce::String& name, double lengthBeats)
{
    const auto patterns = getPatterns();
    juce::ValueTree pattern (ids::PATTERN);
    pattern.setProperty (ids::id, takeNextId(), nullptr);
    pattern.setProperty (ids::name, name.isNotEmpty() ? name.substring (0, 60) : "Pattern " + juce::String (patterns.size() + 1), nullptr);
    pattern.setProperty (ids::colour, patternColours[(size_t) patterns.size() % std::size (patternColours)], nullptr);
    pattern.setProperty (ids::length, juce::jlimit (minPatternBeats, maxPatternBeats, lengthBeats > 0.0 ? lengthBeats : (double) getBeatsPerBar()), nullptr);
    pattern.setProperty (ids::step, 0.25, nullptr);
    pattern.setProperty (ids::swing, 0.0, nullptr);
    state.appendChild (pattern, undoTarget);
    return pattern;
}

int Edit::duplicatePattern (int patternId)
{
    const auto source = getPattern (patternId);
    if (! source.isValid())
        return -1;

    auto copy = source.createCopy();
    renumber (copy);
    copy.setProperty (ids::name, (source[ids::name].toString() + " copy").substring (0, 60), nullptr);
    state.addChild (copy, state.indexOf (source) + 1, undoTarget);
    return (int) copy[ids::id];
}

void Edit::removePattern (int patternId)
{
    auto pattern = getPattern (patternId);
    if (! pattern.isValid())
        return;

    for (const auto& clip : getPatternClips())
        if ((int) clip[ids::pattern] == patternId)
            state.removeChild (clip, undoTarget);

    state.removeChild (pattern, undoTarget);
}

juce::ValueTree Edit::getPattern (int patternId) const
{
    for (const auto& child : state)
        if (child.hasType (ids::PATTERN) && (int) child[ids::id] == patternId)
            return child;
    return {};
}

juce::Array<juce::ValueTree> Edit::getPatterns() const
{
    juce::Array<juce::ValueTree> result;
    for (const auto& child : state)
        if (child.hasType (ids::PATTERN))
            result.add (child);
    return result;
}

void Edit::setPatternName (int patternId, const juce::String& name)
{
    if (auto pattern = getPattern (patternId); pattern.isValid() && name.trim().isNotEmpty())
        pattern.setProperty (ids::name, name.trim().substring (0, 60), undoTarget);
}

void Edit::setPatternColour (int patternId, const juce::String& colour)
{
    if (auto pattern = getPattern (patternId); pattern.isValid())
        pattern.setProperty (ids::colour, colour.substring (0, 32), undoTarget);
}

void Edit::setPatternLength (int patternId, double beats)
{
    if (auto pattern = getPattern (patternId); pattern.isValid())
        pattern.setProperty (ids::length, juce::jlimit (minPatternBeats, maxPatternBeats, beats), undoTarget);
}

void Edit::setPatternStep (int patternId, double beats)
{
    if (auto pattern = getPattern (patternId); pattern.isValid())
        pattern.setProperty (ids::step, juce::jlimit (minStepBeats, maxStepBeats, beats), undoTarget);
}

void Edit::setPatternSwing (int patternId, double swing)
{
    if (auto pattern = getPattern (patternId); pattern.isValid())
        pattern.setProperty (ids::swing, juce::jlimit (0.0, 1.0, swing), undoTarget);
}

double Edit::getPatternLength (const juce::ValueTree& pattern)
{
    return juce::jlimit (minPatternBeats, maxPatternBeats, (double) pattern.getProperty (ids::length, 4.0));
}

double Edit::getPatternStep (const juce::ValueTree& pattern)
{
    return juce::jlimit (minStepBeats, maxStepBeats, (double) pattern.getProperty (ids::step, 0.25));
}

//==============================================================================
juce::ValueTree Edit::getPart (int patternId, int trackId) const
{
    for (const auto& part : getPattern (patternId))
        if (part.hasType (ids::PART) && (int) part[ids::track] == trackId)
            return part;
    return {};
}

juce::ValueTree Edit::ensurePart (int patternId, int trackId)
{
    if (auto part = getPart (patternId, trackId); part.isValid())
        return part;

    auto pattern = getPattern (patternId);
    if (! pattern.isValid() || ! isMidiTrack (getTrack (trackId)))
        return {};

    juce::ValueTree part (ids::PART);
    part.setProperty (ids::track, trackId, nullptr);
    part.setProperty (ids::drum, false, nullptr);
    part.setProperty (ids::stepNote, defaultStepNote, nullptr);
    pattern.appendChild (part, undoTarget);
    return part;
}

void Edit::setPartDrum (int patternId, int trackId, bool drum)
{
    if (auto part = ensurePart (patternId, trackId); part.isValid())
        part.setProperty (ids::drum, drum, undoTarget);
}

void Edit::setPartRows (int patternId, int trackId, const juce::Array<int>& notes)
{
    if (auto part = ensurePart (patternId, trackId); part.isValid())
        part.setProperty (ids::rows, rowsToString (notes), undoTarget);
}

void Edit::setPartStepNote (int patternId, int trackId, int note)
{
    if (auto part = ensurePart (patternId, trackId); part.isValid())
        part.setProperty (ids::stepNote, juce::jlimit (0, 127, note), undoTarget);
}

void Edit::setPartMute (int patternId, int trackId, bool mute)
{
    if (auto part = ensurePart (patternId, trackId); part.isValid())
        part.setProperty (ids::mute, mute, undoTarget);
}

juce::Array<int> Edit::getPartRows (const juce::ValueTree& part)
{
    juce::Array<int> notes;
    for (const auto& item : juce::StringArray::fromTokens (part[ids::rows].toString(), ",", {}))
        if (item.trim().isNotEmpty())
            notes.addIfNotAlreadyThere (juce::jlimit (0, 127, item.trim().getIntValue()));
    return notes;
}

juce::ValueTree Edit::addPatternNote (int patternId, int trackId, int pitch, double time, double length, int velocity)
{
    auto part = ensurePart (patternId, trackId);
    if (! part.isValid())
        return {};

    juce::ValueTree note (ids::NOTE);
    note.setProperty (ids::id, takeNextId(), nullptr);
    note.setProperty (ids::pitch, juce::jlimit (0, 127, pitch), nullptr);
    note.setProperty (ids::time, juce::jmax (0.0, time), nullptr);
    note.setProperty (ids::length, juce::jmax (0.001, length), nullptr);
    note.setProperty (ids::velocity, juce::jlimit (1, 127, velocity), nullptr);
    part.appendChild (note, undoTarget);
    return note;
}

void Edit::setSteps (int patternId, int trackId, int pitch, const std::vector<std::pair<int, int>>& steps)
{
    auto part = ensurePart (patternId, trackId);
    if (! part.isValid())
        return;

    const auto pattern = part.getParent();
    const auto step = getPatternStep (pattern);
    const auto count = (int) std::ceil (getPatternLength (pattern) / step - 1.0e-6);
    pitch = juce::jlimit (0, 127, pitch);

    for (const auto& [index, velocity] : steps)
    {
        if (! juce::isPositiveAndBelow (index, count))
            continue;

        const auto at = index * step;

        for (int i = part.getNumChildren(); --i >= 0;)
        {
            const auto note = part.getChild (i);
            if (note.hasType (ids::NOTE) && (int) note[ids::pitch] == pitch && std::abs ((double) note[ids::time] - at) < step * 0.5)
                part.removeChild (i, undoTarget);
        }

        if (velocity > 0)
            addPatternNote (patternId, trackId, pitch, at, step, velocity);
    }
}

//==============================================================================
juce::ValueTree Edit::addPatternClip (int patternId, double startBeats, double lengthBeats, int row)
{
    const auto pattern = getPattern (patternId);
    if (! pattern.isValid())
        return {};

    juce::ValueTree clip (ids::PATTERNCLIP);
    clip.setProperty (ids::id, takeNextId(), nullptr);
    clip.setProperty (ids::pattern, patternId, nullptr);
    clip.setProperty (ids::start, juce::jmax (0.0, startBeats), nullptr);
    clip.setProperty (ids::length, juce::jmax (minPatternBeats, lengthBeats > 0.0 ? lengthBeats : getPatternLength (pattern)), nullptr);
    clip.setProperty (ids::row, juce::jlimit (0, maxPatternRows - 1, row), nullptr);
    state.appendChild (clip, undoTarget);
    return clip;
}

void Edit::setPatternClip (int clipId, double startBeats, double lengthBeats, int row)
{
    auto clip = getPatternClip (clipId);
    if (! clip.isValid())
        return;

    clip.setProperty (ids::start, juce::jmax (0.0, startBeats), undoTarget);
    clip.setProperty (ids::length, juce::jmax (minPatternBeats, lengthBeats), undoTarget);
    clip.setProperty (ids::row, juce::jlimit (0, maxPatternRows - 1, row), undoTarget);
}

void Edit::setPatternClipMute (int clipId, bool mute)
{
    if (auto clip = getPatternClip (clipId); clip.isValid())
        clip.setProperty (ids::mute, mute, undoTarget);
}

void Edit::removePatternClip (int clipId)
{
    if (auto clip = getPatternClip (clipId); clip.isValid())
        state.removeChild (clip, undoTarget);
}

juce::ValueTree Edit::getPatternClip (int clipId) const
{
    for (const auto& child : state)
        if (child.hasType (ids::PATTERNCLIP) && (int) child[ids::id] == clipId)
            return child;
    return {};
}

juce::Array<juce::ValueTree> Edit::getPatternClips() const
{
    juce::Array<juce::ValueTree> result;
    for (const auto& child : state)
        if (child.hasType (ids::PATTERNCLIP))
            result.add (child);
    return result;
}

int Edit::makePatternUnique (int clipId)
{
    auto clip = getPatternClip (clipId);
    if (! clip.isValid())
        return -1;

    const auto copy = duplicatePattern ((int) clip[ids::pattern]);
    if (copy >= 0)
        clip.setProperty (ids::pattern, copy, undoTarget);
    return copy;
}

juce::Array<int> Edit::patternClipToMidiClips (int clipId)
{
    juce::Array<int> added;
    const auto block = getPatternClip (clipId);
    if (! block.isValid())
        return added;

    const auto pattern = getPattern ((int) block[ids::pattern]);
    const auto tempo = getTempoMap();
    const auto startBeat = juce::jmax (0.0, (double) block[ids::start]);
    const auto start = tempo.toSeconds (startBeat);
    const auto end = tempo.toSeconds (startBeat + juce::jmax (0.0, (double) block[ids::length]));

    // Just this block's notes: the block on its own, in a project-shaped tree.
    juce::ValueTree single (ids::PROJECT);
    single.appendChild (pattern.createCopy(), nullptr);
    auto unmuted = block.createCopy();
    unmuted.removeProperty (ids::mute, nullptr);
    single.appendChild (unmuted, nullptr);

    for (const auto& part : pattern)
    {
        if (! part.hasType (ids::PART))
            continue;

        const auto trackId = (int) part[ids::track];
        const auto events = patterns::getBlockEvents (single, trackId, tempo);
        if (events.empty() || ! isMidiTrack (getTrack (trackId)))
            continue;

        auto clip = addMidiClip (trackId, start, end - start, pattern[ids::name].toString());
        if ((bool) block.getProperty (ids::mute, false))
            clip.setProperty (ids::mute, true, undoTarget);

        for (const auto& e : events)
            addNote ((int) clip[ids::id], e.pitch, e.on - start, e.off - e.on, e.velocity);

        added.add ((int) clip[ids::id]);
    }

    removePatternClip (clipId);
    return added;
}

int Edit::midiClipToPattern (int clipId)
{
    const auto clip = getClip (clipId);
    if (! clip.isValid() || ! isMidiClip (clip))
        return -1;

    const auto track = getOwnerTrack (clip);
    const auto tempo = getTempoMap();
    const auto start = (double) clip[ids::start], offset = (double) clip[ids::offset], length = (double) clip[ids::length];
    const auto startBeat = tempo.toBeats (start);
    const auto lengthBeats = tempo.toBeats (start + length) - startBeat;
    const auto transpose = (int) std::lround ((double) clip.getProperty (ids::transpose, 0.0));

    const auto pattern = addPattern (clip[ids::name].toString(), std::ceil (lengthBeats - 1.0e-6));
    const auto patternId = (int) pattern[ids::id];
    ensurePart (patternId, (int) track[ids::id]);

    for (const auto& note : clip)
    {
        if (! note.hasType (ids::NOTE))
            continue;

        // Only what plays: notes that start inside the clip's window, cut at its edge.
        const auto time = (double) note[ids::time];
        if (time < offset || time >= offset + length)
            continue;

        const auto on = start + time - offset;
        const auto off = juce::jmin (start + length, on + (double) note[ids::length]);
        const auto beat = tempo.toBeats (on) - startBeat;
        addPatternNote (patternId, (int) track[ids::id], (int) note[ids::pitch] + transpose, beat,
                        tempo.toBeats (off) - tempo.toBeats (on), (int) note.getProperty (ids::velocity, 100));
    }

    addPatternClip (patternId, startBeat, lengthBeats, 0);
    if ((bool) clip.getProperty (ids::mute, false))
        setPatternClipMute ((int) getPatternClips().getLast()[ids::id], true);
    removeClip (clipId);
    return patternId;
}

//==============================================================================
namespace patterns
{
std::vector<Event> getPatternEvents (const juce::ValueTree& pattern, int trackId)
{
    std::vector<Event> events;
    juce::ValueTree part;
    for (const auto& child : pattern)
        if (child.hasType (ids::PART) && (int) child[ids::track] == trackId)
            part = child;

    if (! part.isValid() || (bool) part.getProperty (ids::mute, false))
        return events;

    const auto length = Edit::getPatternLength (pattern);
    const auto step = Edit::getPatternStep (pattern);
    const auto swing = step < 1.0 ? juce::jlimit (0.0, 1.0, (double) pattern.getProperty (ids::swing, 0.0)) : 0.0;

    for (const auto& note : part)
    {
        if (! note.hasType (ids::NOTE))
            continue;

        const auto time = juce::jmax (0.0, (double) note[ids::time]);
        if (time >= length)
            continue;

        auto on = time;
        if (swing > 0.0)
        {
            const auto index = (juce::int64) std::llround (time / step);
            if ((index & 1) != 0 && std::abs (time - (double) index * step) < step * 0.25)
                on += swing * step / 3.0;
        }

        events.push_back ({ juce::jlimit (0, 127, (int) note[ids::pitch]), juce::jlimit (1, 127, (int) note.getProperty (ids::velocity, 100)),
                            on, on + juce::jmax (0.001, (double) note[ids::length]) });
    }

    std::stable_sort (events.begin(), events.end(), [] (const Event& a, const Event& b) { return a.on < b.on; });
    return events;
}

std::vector<Event> getBlockEvents (const juce::ValueTree& project, int trackId, const TempoMap& tempo)
{
    std::vector<Event> result;

    for (const auto& block : project)
    {
        if (! block.hasType (ids::PATTERNCLIP) || (bool) block.getProperty (ids::mute, false))
            continue;

        juce::ValueTree pattern;
        for (const auto& child : project)
            if (child.hasType (ids::PATTERN) && (int) child[ids::id] == (int) block[ids::pattern])
                pattern = child;

        if (! pattern.isValid())
            continue;

        const auto events = getPatternEvents (pattern, trackId);
        if (events.empty())
            continue;

        const auto passLength = Edit::getPatternLength (pattern);
        const auto start = juce::jmax (0.0, (double) block[ids::start]);
        const auto length = juce::jmax (0.0, (double) block[ids::length]);

        for (double pass = 0.0; pass < length - 1.0e-9; pass += passLength)
            for (const auto& e : events)
            {
                if (pass + e.on >= length - 1.0e-9)
                    continue;

                const auto off = juce::jmin (length, pass + e.off);
                result.push_back ({ e.pitch, e.velocity, tempo.toSeconds (start + pass + e.on), tempo.toSeconds (start + off) });
            }
    }

    return result;
}

double getLoopSeconds (const juce::ValueTree& pattern, const TempoMap& tempo)
{
    return tempo.toSeconds (Edit::getPatternLength (pattern));
}

std::vector<Event> getLoopEvents (const juce::ValueTree& pattern, int trackId, const TempoMap& tempo)
{
    std::vector<Event> result;
    const auto length = Edit::getPatternLength (pattern);
    const auto loop = getLoopSeconds (pattern, tempo);

    for (const auto& e : getPatternEvents (pattern, trackId))
    {
        const auto on = tempo.toSeconds (e.on), off = tempo.toSeconds (juce::jmin (length, e.off));
        result.push_back ({ e.pitch, e.velocity, on - loop, off - loop }); // the pass before
        result.push_back ({ e.pitch, e.velocity, on, off });
    }

    std::stable_sort (result.begin(), result.end(), [] (const Event& a, const Event& b) { return a.on < b.on; });
    return result;
}

double getBlocksEnd (const juce::ValueTree& project, const TempoMap& tempo)
{
    double end = 0.0;
    for (const auto& block : project)
        if (block.hasType (ids::PATTERNCLIP))
            end = juce::jmax (end, tempo.toSeconds (juce::jmax (0.0, (double) block[ids::start]) + juce::jmax (0.0, (double) block[ids::length])));
    return end;
}
}
}
