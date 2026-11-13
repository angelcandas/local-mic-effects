#include "MidiMap.h"

namespace
{
    const char* sourceName (MidiMap::Source s)
    {
        return s == MidiMap::Source::note ? "note" : s == MidiMap::Source::program ? "program" : "cc";
    }

    std::optional<Looper::Command> looperCommand (const juce::String& target)
    {
        if (target == "action:looper.main")  return Looper::Command::main;
        if (target == "action:looper.stop")  return Looper::Command::stop;
        if (target == "action:looper.undo")  return Looper::Command::undo;
        if (target == "action:looper.redo")  return Looper::Command::redo;
        if (target == "action:looper.clear") return Looper::Command::clear;
        return std::nullopt;
    }
}

MidiMap::MidiMap (juce::AudioProcessorValueTreeState& s, Looper& l, juce::File f)
    : state (s), looper (l), file (std::move (f))
{
    if (file.existsAsFile())
        setMappings (fromVar (juce::JSON::parse (file.loadFileAsString())));

    startTimerHz (60);
}

MidiMap::~MidiMap()
{
    stopTimer();
}

juce::File MidiMap::getDefaultFile()
{
    // Next to the app's settings file (see PluginEditor.cpp).
    juce::PropertiesFile::Options options;
    options.applicationName     = "MicFX";
    options.filenameSuffix      = ".settings";
    options.folderName          = "MicFX";
    options.osxLibrarySubFolder = "Application Support";
    return options.getDefaultFile().getSiblingFile ("MIDI mappings.json");
}

juce::String MidiMap::describe (const Mapping& m)
{
    const auto channel = juce::String (juce::CharPointer_UTF8 (" \xc2\xb7 ch ")) + juce::String (m.channel);
    switch (m.source)
    {
        case Source::note:    return "Note " + juce::MidiMessage::getMidiNoteName (m.number, true, true, 3) + channel;
        case Source::program: return "Program " + juce::String (m.number + 1) + channel;
        case Source::cc:      break;
    }
    return "CC " + juce::String (m.number) + channel;
}

bool MidiMap::isAudioThreadAction (const juce::String& target)
{
    return looperCommand (target).has_value();
}

//==============================================================================
std::vector<MidiMap::Mapping> MidiMap::getMappings() const
{
    return mappings;
}

void MidiMap::setMappings (std::vector<Mapping> newMappings)
{
    mappings.clear();
    for (auto& m : newMappings)
        if (m.target.isNotEmpty() && juce::isPositiveAndNotGreaterThan (m.channel, 16) && m.channel >= 1
            && juce::isPositiveAndBelow (m.number, 128))
            mappings.push_back (std::move (m));

    // A looper pedal must be able to start the first loop from the audio thread.
    for (const auto& m : mappings)
        if (isAudioThreadAction (m.target))
            looper.prepareMemory();

    pickedUp.clear();
    publish();
    save();
}

void MidiMap::removeTarget (const juce::String& target)
{
    auto copy = mappings;
    copy.erase (std::remove_if (copy.begin(), copy.end(), [&] (const Mapping& m) { return m.target == target; }), copy.end());
    setMappings (std::move (copy));
}

void MidiMap::learn (const juce::String& target)
{
    learnTarget = target;
}

void MidiMap::publish()
{
    const juce::SpinLock::ScopedLockType lock (tableLock);
    tableSize = 0;
    for (const auto& m : mappings)
        if (const auto command = looperCommand (m.target); command && tableSize < (int) table.size())
            table[(size_t) tableSize++] = { key (m.source, m.channel, m.number), *command };
}

void MidiMap::save() const
{
    if (file != juce::File())
    {
        file.getParentDirectory().createDirectory();
        file.replaceWithText (juce::JSON::toString (toVar()));
    }
}

juce::var MidiMap::toVar() const
{
    juce::Array<juce::var> list;
    for (const auto& m : mappings)
    {
        auto* entry = new juce::DynamicObject();
        entry->setProperty ("source", sourceName (m.source));
        entry->setProperty ("channel", m.channel);
        entry->setProperty ("number", m.number);
        entry->setProperty ("target", m.target);
        entry->setProperty ("name", describe (m));
        list.add (juce::var (entry));
    }
    return list;
}

std::vector<MidiMap::Mapping> MidiMap::fromVar (const juce::var& value)
{
    std::vector<Mapping> result;
    if (const auto* list = value.getArray())
        for (const auto& entry : *list)
        {
            Mapping m;
            const auto source = entry["source"].toString();
            m.source = source == "note" ? Source::note : source == "program" ? Source::program : Source::cc;
            m.channel = (int) entry.getProperty ("channel", 1);
            m.number = (int) entry.getProperty ("number", 0);
            m.target = entry["target"].toString();
            result.push_back (m);
        }
    return result;
}

//==============================================================================
void MidiMap::process (const juce::MidiBuffer& midi) noexcept
{
    for (const auto metadata : midi)
    {
        const auto message = metadata.getMessage();
        Event event {};

        if (message.isController())
            event = { Source::cc, message.getChannel(), message.getControllerNumber(), message.getControllerValue() };
        else if (message.isNoteOn())
            event = { Source::note, message.getChannel(), message.getNoteNumber(), message.getVelocity() };
        else if (message.isProgramChange())
            event = { Source::program, message.getChannel(), message.getProgramChangeNumber(), 0 };
        else
            continue;

        if (event.channel < 1 || event.channel > 16 || ! juce::isPositiveAndBelow (event.number, 128))
            continue;

        // A press: a key, a program change, or a CC (pedal) crossing the middle upwards.
        auto pressed = event.source != Source::cc;
        if (event.source == Source::cc)
        {
            auto& last = lastCc[(size_t) ((event.channel - 1) * 128 + event.number)];
            pressed = event.value >= 64 && last < 64;
            last = (juce::uint8) event.value;
        }

        if (pressed && learnTarget.isEmpty()) // while learning, a pedal only teaches
        {
            const juce::SpinLock::ScopedTryLockType lock (tableLock);
            if (lock.isLocked())
                for (int i = 0; i < tableSize; ++i)
                    if (table[(size_t) i].key == key (event.source, event.channel, event.number))
                        looper.handleOnAudioThread (table[(size_t) i].command);
        }

        const auto scope = fifo.write (1);
        if (scope.blockSize1 > 0)      events[(size_t) scope.startIndex1] = event;
        else if (scope.blockSize2 > 0) events[(size_t) scope.startIndex2] = event;
    }
}

void MidiMap::drain()
{
    const auto scope = fifo.read (fifo.getNumReady());
    std::vector<Event> pending;
    for (int i = 0; i < scope.blockSize1; ++i) pending.push_back (events[(size_t) (scope.startIndex1 + i)]);
    for (int i = 0; i < scope.blockSize2; ++i) pending.push_back (events[(size_t) (scope.startIndex2 + i)]);

    for (const auto& event : pending)
    {
        ++eventCount;
        Mapping source { event.source, event.channel, event.number, {} };
        lastEvent = describe (source);

        if (learnTarget.isNotEmpty())
        {
            // A pedal sends its release too; learn on what comes first, and let the rest pass.
            source.target = learnTarget;
            learnTarget = {};
            auto copy = mappings;
            copy.erase (std::remove_if (copy.begin(), copy.end(), [&] (const Mapping& m)
                                        { return m.sameSource (source) || m.target == source.target; }), copy.end());
            copy.push_back (source);
            setMappings (std::move (copy));
            if (event.source == Source::cc)
                lastCcMessage[(size_t) ((event.channel - 1) * 128 + event.number)] = (juce::uint8) event.value;
            if (onLearned != nullptr)
                onLearned (source);
            continue;
        }

        for (const auto& m : mappings)
            if (m.sameSource (source) && ! isAudioThreadAction (m.target))
                apply (m, event);

        if (event.source == Source::cc)
        {
            lastCcMessage[(size_t) ((event.channel - 1) * 128 + event.number)] = (juce::uint8) event.value;
            ccSeen[(size_t) ((event.channel - 1) * 128 + event.number)] = true;
        }
    }
}

void MidiMap::apply (const Mapping& m, const Event& event)
{
    const auto previous = event.source == Source::cc ? (int) lastCcMessage[(size_t) ((event.channel - 1) * 128 + event.number)] : 0;
    const auto pressed = event.source != Source::cc || (event.value >= 64 && previous < 64);

    if (m.target.startsWith ("action:"))
    {
        if (! pressed)
            return;

        const auto action = m.target.fromFirstOccurrenceOf (":", false, false);
        if (action == "preset.next" && onPresetStep != nullptr)          onPresetStep (1);
        else if (action == "preset.previous" && onPresetStep != nullptr) onPresetStep (-1);
        else if (action == "mute" && onMuteToggle != nullptr)            onMuteToggle();
        else if (action.startsWith ("scene.") && onScene != nullptr)     onScene (action);
        else if (onAction != nullptr)                                    onAction (action);
        return;
    }

    auto* parameter = state.getParameter (m.target.fromFirstOccurrenceOf ("param:", false, false));
    if (parameter == nullptr)
        return;

    const auto set = [parameter] (float normalised)
    {
        parameter->beginChangeGesture();
        parameter->setValueNotifyingHost (juce::jlimit (0.0f, 1.0f, normalised));
        parameter->endChangeGesture();
    };

    if (dynamic_cast<juce::AudioParameterBool*> (parameter) != nullptr)
    {
        if (pressed)
            set (parameter->getValue() > 0.5f ? 0.0f : 1.0f);
        return;
    }

    if (auto* choice = dynamic_cast<juce::AudioParameterChoice*> (parameter))
    {
        const auto count = choice->choices.size();
        if (count < 2)
            return;
        if (event.source == Source::cc)
            set (choice->convertTo0to1 ((float) juce::roundToInt ((float) event.value / 127.0f * (float) (count - 1))));
        else
            set (choice->convertTo0to1 ((float) ((choice->getIndex() + 1) % count)));
        return;
    }

    if (event.source != Source::cc)
    {
        set (parameter->getValue() < 0.5f ? 1.0f : 0.0f); // a key flips a knob between its ends
        return;
    }

    // A knob picks up: it moves the value only once it reaches it (or passes it), so the
    // sound doesn't jump to wherever the knob happened to be.
    const auto incoming = (float) event.value / 127.0f;
    const auto current = parameter->getValue();
    auto& following = pickedUp[m.target];

    // Moved on screen (or by a preset) since: pick up again.
    if (following && std::abs (current - lastSet[m.target]) > 0.01f)
        following = false;

    if (! following)
    {
        const auto seen = ccSeen[(size_t) ((event.channel - 1) * 128 + event.number)];
        const auto before = (float) previous / 127.0f;
        following = std::abs (incoming - current) < 0.03f || (seen && (before - current) * (incoming - current) <= 0.0f);
    }

    if (following)
    {
        set (incoming);
        lastSet[m.target] = parameter->getValue();
    }
}
