#include "Setlist.h"

namespace
{
    const juce::Identifier SONG { "SONG" }, SCENES { "SCENES" }, name { "name" }, sound { "sound" }, key { "key" },
                           isMinor { "minor" }, tempo { "tempo" }, backing { "backing" }, lyrics { "lyrics" },
                           notes { "notes" }, current { "current" },
                           filePath { "file" }, unsaved { "dirty" };
}

Setlist::Setlist (juce::AudioProcessor& p, Scenes& s, Looper& l, Multitrack& m)
    : processor (p), scenes (s), looper (l), multitrack (m)
{
}

int Setlist::getCurrent() const
{
    const auto index = (int) tree.getProperty (current, -1);
    return juce::isPositiveAndBelow (index, size()) ? index : -1;
}

juce::String Setlist::getName() const
{
    return file != juce::File() ? file.getFileNameWithoutExtension() : juce::String ("New setlist");
}

juce::String Setlist::captureSound() const
{
    juce::StringArray pairs;
    for (auto* p : processor.getParameters())
        if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (p); ranged != nullptr && ranged->getParameterID() != "inputGain")
            pairs.add (ranged->getParameterID() + "=" + juce::String (ranged->convertFrom0to1 (ranged->getValue()), 6));
    return pairs.joinIntoString (";");
}

juce::File Setlist::getBackingFile() const
{
    for (const auto& track : multitrack.getTracks())
        if (! track->isTake)
            return track->file;
    return {};
}

juce::ValueTree Setlist::captureSong (const juce::String& songName) const
{
    juce::ValueTree song (SONG);
    song.setProperty (name, songName, nullptr);
    song.setProperty (sound, captureSound(), nullptr);
    song.setProperty (tempo, looper.isTempoLocked() ? looper.getBpm() : 0.0, nullptr);
    song.setProperty (backing, getBackingFile().getFullPathName(), nullptr);
    song.appendChild (scenes.copyScenes(), nullptr);
    return song;
}

int Setlist::addSong (const juce::String& songName)
{
    const auto backingFile = getBackingFile();
    const auto fallback = backingFile != juce::File() ? backingFile.getFileNameWithoutExtension() : "Song " + juce::String (size() + 1);
    tree.appendChild (captureSong (songName.trim().isNotEmpty() ? songName.trim().substring (0, 60) : fallback), nullptr);
    tree.setProperty (current, size() - 1, nullptr);
    dirty = true;
    return size() - 1;
}

void Setlist::updateSong (int index)
{
    auto song = tree.getChild (index);
    if (! song.isValid())
        return;

    song.setProperty (sound, captureSound(), nullptr);
    if (looper.isTempoLocked())
        song.setProperty (tempo, looper.getBpm(), nullptr);
    if (const auto backingFile = getBackingFile(); backingFile != juce::File())
        song.setProperty (backing, backingFile.getFullPathName(), nullptr);
    song.removeChild (song.getChildWithName (SCENES), nullptr);
    song.appendChild (scenes.copyScenes(), nullptr);
    dirty = true;
}

void Setlist::removeSong (int index)
{
    if (! juce::isPositiveAndBelow (index, size()))
        return;

    const auto now = getCurrent();
    tree.removeChild (index, nullptr);
    tree.setProperty (current, now == index ? -1 : now > index ? now - 1 : now, nullptr);
    dirty = true;
}

void Setlist::moveSong (int index, int newIndex)
{
    if (! juce::isPositiveAndBelow (index, size()))
        return;

    newIndex = juce::jlimit (0, size() - 1, newIndex);
    const auto loaded = getCurrent() >= 0 ? tree.getChild (getCurrent()) : juce::ValueTree();
    tree.moveChild (index, newIndex, nullptr);
    tree.setProperty (current, loaded.isValid() ? tree.indexOf (loaded) : -1, nullptr);
    dirty = true;
}

void Setlist::setSongInfo (int index, const juce::var& fields)
{
    auto song = tree.getChild (index);
    if (! song.isValid())
        return;

    if (fields.hasProperty ("name") && fields["name"].toString().trim().isNotEmpty())
        song.setProperty (name, fields["name"].toString().trim().substring (0, 60), nullptr);
    if (fields.hasProperty ("key"))    song.setProperty (key, juce::jlimit (-1, 11, (int) fields["key"]), nullptr);
    if (fields.hasProperty ("minor"))  song.setProperty (isMinor, (bool) fields["minor"], nullptr);
    if (fields.hasProperty ("tempo"))  song.setProperty (tempo, juce::jlimit (0.0, 300.0, (double) fields["tempo"]), nullptr);
    if (fields.hasProperty ("backing")) song.setProperty (backing, fields["backing"].toString(), nullptr);
    if (fields.hasProperty ("lyrics")) song.setProperty (lyrics, fields["lyrics"].toString().substring (0, 100000), nullptr);
    if (fields.hasProperty ("notes"))  song.setProperty (notes, fields["notes"].toString().substring (0, 20000), nullptr);
    dirty = true;
}

bool Setlist::loadSong (int index, juce::String& warning)
{
    const auto song = tree.getChild (index);
    if (! song.isValid())
        return false;

    // The song's scenes first: a scene glide still running finishes here, before the song's
    // sound is put in place (it would otherwise land on top of it).
    scenes.replaceScenes (song.getChildWithName (SCENES));

    // The last song's loop stops (its layers stay, for Play or Clear).
    if (const auto loopState = looper.getState(); loopState == Looper::State::playing || loopState == Looper::State::overdubbing)
        looper.send (Looper::Command::stop);

    // The sound, at once (a song change isn't a glide).
    std::map<juce::String, float> values;
    for (const auto& pair : juce::StringArray::fromTokens (song[sound].toString(), ";", {}))
        values[pair.upToFirstOccurrenceOf ("=", false, false)] = pair.fromFirstOccurrenceOf ("=", false, false).getFloatValue();

    const auto set = [] (juce::RangedAudioParameter& parameter, float normalised)
    {
        if (std::abs (parameter.getValue() - normalised) < 1.0e-6f)
            return;
        parameter.beginChangeGesture();
        parameter.setValueNotifyingHost (normalised);
        parameter.endChangeGesture();
    };

    for (auto* p : processor.getParameters())
        if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (p); ranged != nullptr && ranged->getParameterID() != "inputGain")
            if (const auto it = values.find (ranged->getParameterID()); it != values.end())
                set (*ranged, ranged->convertTo0to1 (it->second));

    // Its key pulls the autotune (a major or minor scale follows the song's mode).
    if (const auto songKey = (int) song.getProperty (key, -1); songKey >= 0)
        for (auto* p : processor.getParameters())
        {
            auto* choice = dynamic_cast<juce::AudioParameterChoice*> (p);
            if (choice == nullptr)
                continue;
            if (choice->getParameterID() == "tuneKey")
                set (*choice, choice->convertTo0to1 ((float) songKey));
            if (choice->getParameterID() == "tuneScale" && (choice->getIndex() == 1 || choice->getIndex() == 2))
                set (*choice, choice->convertTo0to1 ((bool) song.getProperty (isMinor, false) ? 2.0f : 1.0f));
        }

    if (const auto bpm = (double) song.getProperty (tempo, 0.0); bpm > 0.0)
    {
        looper.setTempo (bpm, looper.getBeatsPerBar());
        looper.setTempoLocked (true);
    }

    // The backing track: the last song's tracks (and takes) make way for this one's.
    multitrack.pause();
    multitrack.seek (0);
    for (const auto& track : multitrack.getTracks())
        multitrack.removeTrack (track->id);

    const juce::File backingFile (song[backing].toString());
    if (song[backing].toString().isNotEmpty())
    {
        if (juce::File::isAbsolutePath (song[backing].toString()) && backingFile.existsAsFile())
            multitrack.loadBackingTrack (backingFile);
        else
            warning = "The backing track isn't there any more: " + song[backing].toString();
    }

    tree.setProperty (current, index, nullptr);
    return true;
}

bool Setlist::step (int delta, juce::String& warning)
{
    if (size() == 0)
        return false;

    const auto now = getCurrent();
    const auto next = now < 0 ? (delta > 0 ? 0 : size() - 1) : now + delta;
    return juce::isPositiveAndBelow (next, size()) && loadSong (next, warning); // no wrap: the show has an end
}

void Setlist::clear()
{
    tree = juce::ValueTree ("SETLIST");
    file = juce::File();
    dirty = false;
}

juce::String Setlist::save (const juce::File& destination)
{
    auto copy = tree.createCopy();
    copy.removeProperty (current, nullptr);
    const auto xml = copy.createXml();
    if (xml == nullptr || ! xml->writeTo (destination))
        return "Couldn't save " + destination.getFullPathName();

    file = destination;
    dirty = false;
    return {};
}

juce::String Setlist::open (const juce::File& source)
{
    const auto xml = juce::XmlDocument::parse (source);
    if (xml == nullptr || ! xml->hasTagName ("SETLIST"))
        return source.getFileName() + " isn't a MicFX setlist.";

    tree = juce::ValueTree::fromXml (*xml);
    file = source;
    dirty = false;
    return {};
}

juce::ValueTree Setlist::getSnapshot() const
{
    auto copy = tree.createCopy();
    copy.setProperty (filePath, file.getFullPathName(), nullptr);
    copy.setProperty (unsaved, dirty, nullptr);
    return copy;
}

void Setlist::restore (const juce::ValueTree& snapshot)
{
    if (! snapshot.hasType ("SETLIST"))
        return;

    tree = snapshot.createCopy();
    const auto path = tree[filePath].toString();
    file = juce::File::isAbsolutePath (path) ? juce::File (path) : juce::File();
    dirty = (bool) tree[unsaved];
    tree.removeProperty (filePath, nullptr);
    tree.removeProperty (unsaved, nullptr);
}

juce::var Setlist::describe() const
{
    juce::Array<juce::var> songs;
    for (const auto& song : tree)
    {
        auto* entry = new juce::DynamicObject();
        entry->setProperty ("name", song[name].toString());
        entry->setProperty ("key", (int) song.getProperty (key, -1));
        entry->setProperty ("minor", (bool) song.getProperty (isMinor, false));
        entry->setProperty ("tempo", (double) song.getProperty (tempo, 0.0));
        entry->setProperty ("backing", song[backing].toString());
        entry->setProperty ("lyrics", song[lyrics].toString());
        entry->setProperty ("notes", song[notes].toString());

        juce::Array<juce::var> sceneNames;
        for (const auto& scene : song.getChildWithName (SCENES))
            sceneNames.add (scene["name"].toString());
        entry->setProperty ("scenes", sceneNames);
        songs.add (juce::var (entry));
    }

    auto* result = new juce::DynamicObject();
    result->setProperty ("name", getName());
    result->setProperty ("file", file.getFullPathName());
    result->setProperty ("dirty", dirty);
    result->setProperty ("current", getCurrent());
    result->setProperty ("songs", songs);
    return juce::var (result);
}
