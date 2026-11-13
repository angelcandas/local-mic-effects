#include "LiveHistory.h"

LiveHistory::LiveHistory (juce::AudioProcessorValueTreeState& s)
    : state (s)
{
    for (auto* p : state.processor.getParameters())
        if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (p))
            params.add (ranged);

    committed = last = take();
    startTimer (50);
}

LiveHistory::~LiveHistory()
{
    stopTimer();
}

LiveHistory::Snapshot LiveHistory::take() const
{
    Snapshot values;
    values.reserve ((size_t) params.size());
    for (auto* p : params)
        values.push_back (p->getValue());
    return values;
}

void LiveHistory::apply (const Snapshot& values)
{
    for (int i = 0; i < params.size() && i < (int) values.size(); ++i)
    {
        auto* p = params.getUnchecked (i);
        if (p->getValue() != values[(size_t) i])
        {
            p->beginChangeGesture();
            p->setValueNotifyingHost (values[(size_t) i]);
            p->endChangeGesture();
        }
    }
}

void LiveHistory::label (const juce::String& name)
{
    flush(); // whatever changed before belongs to the step before
    pendingLabel = name;
    labelMs = juce::Time::getMillisecondCounter();
}

juce::String LiveHistory::describe (const Snapshot& before, const Snapshot& after) const
{
    juce::RangedAudioParameter* changed = nullptr;
    int count = 0;
    for (size_t i = 0; i < before.size() && i < after.size(); ++i)
        if (before[i] != after[i])
        {
            changed = params[(int) i];
            ++count;
        }

    if (count == 1 && changed != nullptr)
        return changed->getName (64);
    return "Change " + juce::String (count) + " settings";
}

void LiveHistory::commit (const Snapshot& now)
{
    if (now == committed)
        return;

    const auto preset = getPreset ? getPreset() : 0;
    Step step { committed, now, pendingLabel.isNotEmpty() ? pendingLabel : describe (committed, now), committedPreset, preset };
    committedPreset = preset;
    undoSteps.push_back (std::move (step));
    if ((int) undoSteps.size() > maxSteps)
        undoSteps.erase (undoSteps.begin());
    redoSteps.clear();
    committed = now;
    pendingLabel.clear();
    if (onChange)
        onChange();
}

void LiveHistory::flush()
{
    if (held)
        return;
    const auto now = take();
    last = now;
    commit (now);
}

void LiveHistory::timerCallback()
{
    if (held)
        return;

    const auto nowMs = juce::Time::getMillisecondCounter();
    const auto now = take();

    if (now != last)
    {
        last = now;
        lastChangeMs = nowMs;
        return;
    }

    if (now != committed && nowMs - lastChangeMs >= (juce::uint32) restMs)
        commit (now);
    else if (pendingLabel.isNotEmpty() && now == committed && nowMs - labelMs > 1000)
        pendingLabel.clear(); // the labelled action changed nothing
}

void LiveHistory::hold (const juce::String& name)
{
    if (held)
        return;
    label (name);
    held = true;
}

void LiveHistory::release()
{
    if (! held)
        return;
    held = false;
    const auto name = pendingLabel;
    flush();
    if (pendingLabel == name)
        pendingLabel.clear(); // nothing changed in the end
}

bool LiveHistory::undo()
{
    release();
    flush();
    if (undoSteps.empty())
        return false;

    auto step = std::move (undoSteps.back());
    undoSteps.pop_back();
    apply (step.before);
    if (setPreset) setPreset (committedPreset = step.presetBefore);
    committed = last = take();
    redoSteps.push_back (std::move (step));
    if (onChange)
        onChange();
    return true;
}

bool LiveHistory::redo()
{
    release();
    flush();
    if (redoSteps.empty())
        return false;

    auto step = std::move (redoSteps.back());
    redoSteps.pop_back();
    apply (step.after);
    if (setPreset) setPreset (committedPreset = step.presetAfter);
    committed = last = take();
    undoSteps.push_back (std::move (step));
    if (onChange)
        onChange();
    return true;
}

juce::String LiveHistory::getUndoName() const { return undoSteps.empty() ? juce::String() : undoSteps.back().name; }
juce::String LiveHistory::getRedoName() const { return redoSteps.empty() ? juce::String() : redoSteps.back().name; }

void LiveHistory::clear()
{
    held = false;
    undoSteps.clear();
    redoSteps.clear();
    committed = last = take();
    committedPreset = getPreset ? getPreset() : 0;
    pendingLabel.clear();
    if (onChange)
        onChange();
}
