#include "Scenes.h"

namespace
{
    const juce::Identifier SCENES { "SCENES" }, SCENE { "SCENE" }, name { "name" }, values { "values" },
                           active { "active" }, glide { "glide" };

    // "id=value;id=value" -> { id: value }
    std::map<juce::String, float> parse (const juce::String& text)
    {
        std::map<juce::String, float> result;
        for (const auto& pair : juce::StringArray::fromTokens (text, ";", {}))
            if (pair.contains ("="))
                result[pair.upToFirstOccurrenceOf ("=", false, false)] = pair.fromFirstOccurrenceOf ("=", false, false).getFloatValue();
        return result;
    }
}

Scenes::Scenes (juce::AudioProcessor& p, juce::AudioProcessorValueTreeState& s)
    : processor (p), state (s)
{
}

Scenes::~Scenes()
{
    stopTimer();
}

bool Scenes::isSceneParameter (const juce::RangedAudioParameter& parameter)
{
    return parameter.getParameterID() != "inputGain"; // the mic's level belongs to the room, like in presets
}

juce::ValueTree Scenes::getTree() const
{
    auto root = state.state;
    return root.getOrCreateChildWithName (SCENES, nullptr);
}

juce::String Scenes::capture() const
{
    juce::StringArray pairs;
    for (auto* p : processor.getParameters())
        if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (p); ranged != nullptr && isSceneParameter (*ranged))
            pairs.add (ranged->getParameterID() + "=" + juce::String (ranged->convertFrom0to1 (ranged->getValue()), 6));
    return pairs.joinIntoString (";");
}

int Scenes::size() const                     { return getTree().getNumChildren(); }
juce::String Scenes::getName (int index) const { return getTree().getChild (index)[name].toString(); }
double Scenes::getGlideSeconds() const       { return (double) getTree().getProperty (glide, 0.5); }

int Scenes::getActive() const
{
    const auto index = (int) getTree().getProperty (active, -1);
    return juce::isPositiveAndBelow (index, size()) ? index : -1;
}

void Scenes::setGlideSeconds (double seconds)
{
    getTree().setProperty (glide, juce::jlimit (0.0, 10.0, seconds), nullptr);
}

bool Scenes::isModified() const
{
    const auto index = getActive();
    if (index < 0)
        return false;

    const auto saved = parse (getTree().getChild (index)[values].toString());
    for (auto* p : processor.getParameters())
        if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (p); ranged != nullptr && isSceneParameter (*ranged))
            if (const auto it = saved.find (ranged->getParameterID()); it != saved.end()
                && std::abs (ranged->convertTo0to1 (it->second) - ranged->getValue()) > 0.002f)
                return true;
    return false;
}

int Scenes::add (const juce::String& sceneName)
{
    auto tree = getTree();
    if (tree.getNumChildren() >= maxScenes)
        return -1;

    juce::ValueTree scene (SCENE);
    scene.setProperty (name, sceneName.trim().isNotEmpty() ? sceneName.trim().substring (0, 40)
                                                           : "Scene " + juce::String (tree.getNumChildren() + 1), nullptr);
    scene.setProperty (values, capture(), nullptr);
    tree.appendChild (scene, nullptr);
    tree.setProperty (active, tree.getNumChildren() - 1, nullptr);
    return tree.getNumChildren() - 1;
}

void Scenes::update (int index)
{
    auto scene = getTree().getChild (index);
    if (! scene.isValid())
        return;

    scene.setProperty (values, capture(), nullptr);
    getTree().setProperty (active, index, nullptr);
}

void Scenes::rename (int index, const juce::String& sceneName)
{
    if (auto scene = getTree().getChild (index); scene.isValid() && sceneName.trim().isNotEmpty())
        scene.setProperty (name, sceneName.trim().substring (0, 40), nullptr);
}

void Scenes::remove (int index)
{
    auto tree = getTree();
    if (! juce::isPositiveAndBelow (index, tree.getNumChildren()))
        return;

    const auto current = getActive();
    tree.removeChild (index, nullptr);
    tree.setProperty (active, current == index ? -1 : current > index ? current - 1 : current, nullptr);
}

void Scenes::step (int delta)
{
    const auto count = size();
    if (count == 0)
        return;

    const auto current = getActive();
    recall (current < 0 ? (delta > 0 ? 0 : count - 1) : ((current + delta) % count + count) % count);
}

void Scenes::recall (int index)
{
    const auto scene = getTree().getChild (index);
    if (! scene.isValid())
        return;

    // A glide already running finishes where it was going first.
    if (! moves.empty())
        advance (glideLength);

    getTree().setProperty (active, index, nullptr);
    const auto saved = parse (scene[values].toString());

    for (auto* p : processor.getParameters())
    {
        auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (p);
        if (ranged == nullptr || ! isSceneParameter (*ranged))
            continue;

        const auto it = saved.find (ranged->getParameterID());
        if (it == saved.end())
            continue; // a parameter newer than the scene keeps its value

        const auto from = ranged->getValue();
        const auto to = ranged->convertTo0to1 (it->second);
        if (std::abs (to - from) < 1.0e-5f)
            continue;

        const auto isSwitch = dynamic_cast<juce::AudioParameterBool*> (ranged) != nullptr;
        const auto isChoice = dynamic_cast<juce::AudioParameterChoice*> (ranged) != nullptr;

        // Choices and effects coming on change now; effects going off wait for the end.
        if (isChoice || (isSwitch && to > 0.5f))
        {
            ranged->beginChangeGesture();
            ranged->setValueNotifyingHost (to);
            ranged->endChangeGesture();
            continue;
        }

        ranged->beginChangeGesture();
        moves.push_back ({ ranged, from, to, isSwitch ? Move::When::end : Move::When::glide });
    }

    glideTime = 0.0;
    glideLength = getGlideSeconds();
    lastTick = juce::Time::getMillisecondCounterHiRes();

    if (glideLength <= 0.0)
        advance (0.0);
    else if (! moves.empty())
        startTimerHz (60);
}

void Scenes::timerCallback()
{
    const auto now = juce::Time::getMillisecondCounterHiRes();
    advance ((now - lastTick) / 1000.0);
    lastTick = now;
}

void Scenes::advance (double seconds)
{
    if (moves.empty())
    {
        stopTimer();
        return;
    }

    glideTime += seconds;
    const auto done = glideTime >= glideLength;
    const auto t = done ? 1.0f : (float) (glideTime / glideLength);
    const auto eased = t * t * (3.0f - 2.0f * t); // smooth start and end

    for (const auto& move : moves)
    {
        if (move.when == Move::When::glide)
            move.parameter->setValueNotifyingHost (move.from + (move.to - move.from) * eased);
        else if (done)
            move.parameter->setValueNotifyingHost (move.to);
    }

    if (done)
    {
        for (const auto& move : moves)
            move.parameter->endChangeGesture();
        moves.clear();
        stopTimer();
    }
}

juce::var Scenes::describe() const
{
    juce::Array<juce::var> list;
    for (int i = 0; i < size(); ++i)
    {
        auto* entry = new juce::DynamicObject();
        entry->setProperty ("name", getName (i));
        list.add (juce::var (entry));
    }

    auto* result = new juce::DynamicObject();
    result->setProperty ("scenes", list);
    result->setProperty ("active", getActive());
    result->setProperty ("modified", isModified());
    result->setProperty ("glide", getGlideSeconds());
    result->setProperty ("gliding", isGliding());
    result->setProperty ("max", maxScenes);
    return juce::var (result);
}

juce::ValueTree Scenes::copyScenes() const
{
    return getTree().createCopy();
}

void Scenes::replaceScenes (const juce::ValueTree& scenes)
{
    if (! moves.empty())
        advance (glideLength);

    auto tree = getTree();
    tree.removeAllChildren (nullptr);
    tree.removeAllProperties (nullptr);
    if (scenes.hasType (SCENES))
        tree.copyPropertiesAndChildrenFrom (scenes, nullptr);
}
