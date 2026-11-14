#include "AutomationRecorder.h"
#include "DawEngine.h"

namespace daw
{
AutomationRecorder::AutomationRecorder (Edit& e, DawEngine* d) : edit (e), engine (d) {}

juce::String AutomationRecorder::modeKey (Mode m)
{
    return m == Mode::touch ? "touch" : m == Mode::latch ? "latch" : "off";
}

AutomationRecorder::Mode AutomationRecorder::modeFromKey (const juce::String& key)
{
    return key == "touch" ? Mode::touch : key == "latch" ? Mode::latch : Mode::off;
}

void AutomationRecorder::setMode (Mode m)
{
    if (m == mode)
        return;
    finishAll (lastTime);
    mode = m;
}

bool AutomationRecorder::moved (int trackId, const juce::String& target, float knob, double time)
{
    const auto track = edit.getTrack (trackId);
    if (mode == Mode::off || ! track.isValid())
        return false;

    lastTime = time;
    const auto key = std::make_pair (trackId, target);
    auto it = passes.find (key);
    const auto isVolume = target == "volume";

    if (it == passes.end())
    {
        Pass pass;
        pass.start = pass.last = time;
        pass.knobBefore = isVolume ? (float) track.getProperty (ids::volume, 0.0f) : Edit::getLaneBase (track, target);
        pass.rideAtStart = isVolume ? Edit::getAutomationValueAt (Edit::getLane (track, "volume"), time) : 0.0f;
        it = passes.emplace (key, std::move (pass)).first;
        if (engine != nullptr && ! isVolume)
            engine->holdLane (trackId, target, true);
    }

    auto& pass = it->second;
    // The fader's move is added to the ride that was there.
    const auto value = isVolume ? pass.rideAtStart + (knob - pass.knobBefore) : knob;
    if (time + 1.0e-6 < pass.last)
    {
        // The playhead jumped back (a loop): this pass ends, a new one starts.
        finish (trackId, target, pass, pass.last);
        passes.erase (it);
        return moved (trackId, target, knob, time);
    }

    pass.points.push_back ({ time, value });
    pass.last = time;
    pass.lastValue = value;
    pass.lastMove = juce::Time::getMillisecondCounterHiRes();
    return true;
}

void AutomationRecorder::update (double time, bool playing)
{
    lastTime = time;
    if (passes.empty())
        return;

    if (! playing)
    {
        finishAll (time);
        return;
    }

    const auto now = juce::Time::getMillisecondCounterHiRes();
    for (auto it = passes.begin(); it != passes.end();)
    {
        auto& pass = it->second;
        if (time + 1.0e-6 < pass.last)
        {
            // Looped back: what was written up to the loop's end is kept.
            finish (it->first.first, it->first.second, pass, pass.last);
            it = passes.erase (it);
            continue;
        }
        if (mode == Mode::latch)
        {
            pass.last = time; // the last value runs on
            ++it;
        }
        else if (now - pass.lastMove > releaseSeconds * 1000.0)
        {
            finish (it->first.first, it->first.second, pass, pass.last);
            it = passes.erase (it);
        }
        else
            ++it;
    }
}

void AutomationRecorder::finishAll (double time)
{
    for (auto& [key, pass] : passes)
        finish (key.first, key.second, pass, mode == Mode::latch ? juce::jmax (pass.last, time) : pass.last);
    passes.clear();
}

juce::StringArray AutomationRecorder::getWritingTargets (int trackId) const
{
    juce::StringArray targets;
    for (const auto& [key, pass] : passes)
        if (key.first == trackId)
            targets.add (key.second);
    return targets;
}

void AutomationRecorder::finish (int trackId, const juce::String& target, Pass& pass, double end)
{
    if (engine != nullptr && target != "volume")
        engine->holdLane (trackId, target, false);
    if (pass.points.empty())
        return;

    auto points = pass.points;
    if (end > points.back().time)
        points.push_back ({ end, pass.lastValue });

    const auto track = edit.getTrack (trackId);
    if (! track.isValid())
        return;
    points = Edit::thinPoints (points, Edit::getThinTolerance (track, target, points));

    auto& undo = edit.getUndoManager();
    undo.beginNewTransaction ("Record automation");
    const auto base = target == "volume" ? 0.0f : pass.knobBefore;
    edit.writeLaneRange (trackId, target, points, pass.start, end, base);
    restoreKnob (trackId, target, pass.knobBefore);
    undo.beginNewTransaction();
}

void AutomationRecorder::restoreKnob (int trackId, const juce::String& target, float value)
{
    if (target == "volume")
        edit.setTrackVolume (trackId, value);
    else if (target == "pan")
        edit.setTrackPan (trackId, value);
    else if (target.startsWith ("send:"))
        edit.setSend (trackId, target.fromFirstOccurrenceOf (":", false, false).getIntValue(), value);
    else if (target.startsWith ("macro:"))
        edit.setMacro (trackId, target.fromFirstOccurrenceOf (":", false, false).getIntValue(), value);
    else if (target.startsWith ("insert:"))
    {
        const auto rest = target.fromFirstOccurrenceOf (":", false, false);
        edit.setInsertValue (rest.upToFirstOccurrenceOf (":", false, false).getIntValue(), rest.fromFirstOccurrenceOf (":", false, false), value);
    }
}
}
