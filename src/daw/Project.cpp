#include "Project.h"

#include <algorithm>

namespace daw
{
namespace
{
    void forEachClip (juce::ValueTree tree, const std::function<void (juce::ValueTree&)>& callback)
    {
        for (auto child : tree)
        {
            if (child.hasType (ids::CLIP))
            {
                if (! Edit::isMidiClip (child)) // MIDI clips hold notes, not a file
                    callback (child);
            }
            else
                forEachClip (child, callback);
        }
    }

    // XML keeps every property as text: "0.0", "1", "Vocal". Put the types back as the Edit
    // makes them (ids and versions as ints, switches as bools, the rest numbers where they
    // read as numbers), so a loaded project is the same as one built in memory. Without
    // this, the DAW screen got volumes as text and couldn't draw a loaded project.
    void restoreTypes (juce::ValueTree tree)
    {
        static const juce::Identifier textIds[] { ids::name, ids::file, ids::colour, ids::type, ids::kind, ids::plugin,
                                                  ids::format, ids::vendor, ids::description, ids::state };
        static const juce::Identifier boolIds[] { ids::mute, ids::solo, ids::arm, ids::monitor, ids::monitorAuto, ids::loopOn, ids::keyMinor, ids::drum };
        static const juce::Identifier intIds[] { ids::id, ids::version, ids::key };

        for (int i = 0; i < tree.getNumProperties(); ++i)
        {
            const auto name = tree.getPropertyName (i);
            const auto value = tree[name];

            if (! value.isString() || std::find (std::begin (textIds), std::end (textIds), name) != std::end (textIds))
                continue;

            const auto text = value.toString().trim();

            if (std::find (std::begin (boolIds), std::end (boolIds), name) != std::end (boolIds))
                tree.setProperty (name, text == "1" || text.equalsIgnoreCase ("true"), nullptr);
            else if (std::find (std::begin (intIds), std::end (intIds), name) != std::end (intIds))
                tree.setProperty (name, text.getIntValue(), nullptr);
            else if (text.isNotEmpty() && text.containsOnly ("0123456789.-+eE") && text.containsAnyOf ("0123456789"))
                tree.setProperty (name, text.getDoubleValue(), nullptr);
        }

        for (auto child : tree)
            restoreTypes (child);
    }

    juce::String createFolder (const juce::File& folder)
    {
        return folder.createDirectory() ? juce::String() : "Couldn't create " + folder.getFullPathName() + ".";
    }

    // Backups are named by the time they were written, in ms, so they sort exactly
    // (file times can be whole seconds).
    juce::int64 backupTime (const juce::File& backup)
    {
        return backup.getFileNameWithoutExtension().fromLastOccurrenceOf ("autosave-", false, false).getLargeIntValue();
    }

    juce::Array<juce::File> findBackups (const juce::File& backupsFolder)
    {
        auto backups = backupsFolder.findChildFiles (juce::File::findFiles, false, "autosave-*.xml");
        std::sort (backups.begin(), backups.end(), [] (const auto& a, const auto& b) { return backupTime (a) > backupTime (b); });
        return backups;
    }

    juce::File withExtension (const juce::File& folder)
    {
        return folder.getFileName().endsWithIgnoreCase (Project::extension)
                   ? folder
                   : folder.getSiblingFile (folder.getFileName() + Project::extension);
    }
}

Project::Project (Edit& e) : edit (e)
{
    edit.getState().addListener (this);
}

Project::~Project()
{
    stopTimer();
    close();
    edit.getState().removeListener (this);
}

bool Project::isProjectFolder (const juce::File& candidate)
{
    return candidate.isDirectory() && candidate.getChildFile ("project.xml").existsAsFile();
}

void Project::setFolder (const juce::File& newFolder)
{
    if (isOpen() && folder != newFolder)
        getLockFile().deleteFile();

    folder = newFolder;

    for (const auto& sub : { getAudioFolder(), getBackupsFolder(), getExportsFolder() })
        sub.createDirectory();

    // Present while the project is open; left behind if the app doesn't close cleanly.
    // Holds the time of the last save, so recovery only offers backups made after it.
    lastSaveTime = juce::Time::currentTimeMillis();
    getLockFile().replaceWithText (juce::String (lastSaveTime));
}

//==============================================================================
juce::String Project::createNew (const juce::File& requested)
{
    const auto target = withExtension (requested);

    if (target.exists() && ! (target.isDirectory() && target.getNumberOfChildFiles (juce::File::findFilesAndDirectories) == 0))
        return target.getFileName() + " already exists.";

    if (const auto error = createFolder (target); error.isNotEmpty())
        return error;

    close();

    Edit fresh;
    edit.replaceState (fresh.getState());
    setFolder (target);
    recoverable = juce::File();
    return save();
}

juce::File Project::findProjectFolder (const juce::File& picked)
{
    for (auto candidate = picked; candidate != juce::File(); candidate = candidate.getParentDirectory())
    {
        if (isProjectFolder (candidate))
            return candidate;

        if (candidate.getParentDirectory() == candidate) // the root
            break;
    }

    return {};
}

juce::String Project::open (const juce::File& requested)
{
    if (! isProjectFolder (requested))
        return requested.getFileName() + " isn't a MicFX project (no project.xml).";

    const auto lock = requested.getChildFile ("Backups").getChildFile (".open");
    const auto hadLock = lock.existsAsFile();
    const auto lastSave = lock.loadFileAsString().getLargeIntValue();

    if (const auto error = load (requested.getChildFile ("project.xml"), requested); error.isNotEmpty())
        return error;

    close();
    setFolder (requested);
    dirty = false;

    // The last session didn't close: offer the newest backup if it's newer than the save.
    recoverable = juce::File();

    if (hadLock)
    {
        const auto backups = findBackups (getBackupsFolder());

        if (! backups.isEmpty() && backupTime (backups.getFirst()) > lastSave)
            recoverable = backups.getFirst();
    }

    return {};
}

juce::String Project::load (const juce::File& xmlFile, const juce::File& projectFolder)
{
    const auto xml = juce::parseXML (xmlFile);

    if (xml == nullptr)
        return "Couldn't read " + xmlFile.getFullPathName() + ".";

    auto tree = juce::ValueTree::fromXml (*xml);

    if (! tree.hasType (ids::PROJECT))
        return xmlFile.getFileName() + " isn't a MicFX project.";

    restoreTypes (tree);

    if ((int) tree.getProperty (ids::version, 0) > Edit::formatVersion)
        return "This project was saved by a newer version of MicFX. Update MicFX to open it.";

    // Relative clip paths are relative to the project folder.
    forEachClip (tree, [&] (juce::ValueTree& clip)
    {
        const auto path = clip[ids::file].toString();
        if (! juce::File::isAbsolutePath (path))
            clip.setProperty (ids::file, projectFolder.getChildFile (path).getFullPathName(), nullptr);
    });

    tree.setProperty (ids::version, Edit::formatVersion, nullptr);
    edit.replaceState (tree);
    dirty = false;
    return {};
}

juce::String Project::writeTree (const juce::File& destination) const
{
    auto tree = edit.getState().createCopy();

    // Files inside the project folder are stored relative, so the folder can move.
    forEachClip (tree, [this] (juce::ValueTree& clip)
    {
        const juce::File file (clip[ids::file].toString());
        if (file.isAChildOf (folder))
            clip.setProperty (ids::file, file.getRelativePathFrom (folder).replaceCharacter ('\\', '/'), nullptr);
    });

    const auto xml = tree.createXml();
    const auto temp = destination.getSiblingFile (destination.getFileName() + ".tmp");

    // Write next to it, then swap, so a crash mid-write never leaves a broken project.
    if (xml == nullptr || ! xml->writeTo (temp) || ! temp.moveFileTo (destination))
    {
        temp.deleteFile();
        return "Couldn't write " + destination.getFullPathName() + ".";
    }

    return {};
}

juce::String Project::save()
{
    if (! isOpen())
        return "No project is open.";

    if (onBeforeWrite)
        onBeforeWrite();

    if (const auto error = writeTree (getProjectFile()); error.isNotEmpty())
        return error;

    dirty = false;
    recoverable = juce::File();
    lastSaveTime = juce::jmax (juce::Time::currentTimeMillis(), lastSaveTime + 1);
    if (const auto backups = findBackups (getBackupsFolder()); ! backups.isEmpty())
        lastSaveTime = juce::jmax (lastSaveTime, backupTime (backups.getFirst()) + 1);
    getLockFile().replaceWithText (juce::String (lastSaveTime));
    return {};
}

juce::String Project::saveAs (const juce::File& requested)
{
    const auto target = withExtension (requested);

    if (target == folder)
        return save();

    if (target.exists() && ! (target.isDirectory() && target.getNumberOfChildFiles (juce::File::findFilesAndDirectories) == 0))
        return target.getFileName() + " already exists.";

    if (const auto error = createFolder (target.getChildFile ("Audio")); error.isNotEmpty())
        return error;

    // Copy every file the clips use into the new Audio folder (once each).
    std::map<juce::String, juce::File> copies;
    juce::String error;

    forEachClip (edit.getState(), [&] (juce::ValueTree& clip)
    {
        const juce::File source (clip[ids::file].toString());
        auto& copy = copies[source.getFullPathName()];

        if (copy == juce::File() && source.existsAsFile())
        {
            copy = target.getChildFile ("Audio").getNonexistentChildFile (source.getFileNameWithoutExtension(), source.getFileExtension(), false);

            if (! source.copyFileTo (copy))
                error = "Couldn't copy " + source.getFileName() + ".";
        }

        if (copy != juce::File())
            clip.setProperty (ids::file, copy.getFullPathName(), nullptr); // not an edit: no undo
    });

    if (error.isNotEmpty())
        return error;

    setFolder (target);
    return save();
}

void Project::close()
{
    // The autosave timer keeps running: it only writes while a project is open, and
    // stopping it here would end autosave for every project opened after this one.
    if (isOpen())
        getLockFile().deleteFile();

    folder = juce::File();
    recoverable = juce::File();
}

//==============================================================================
juce::File Project::importAudio (const juce::File& source)
{
    if (! isOpen() || ! source.existsAsFile())
        return {};

    if (source.isAChildOf (getAudioFolder()))
        return source;

    const auto copy = getAudioFolder().getNonexistentChildFile (source.getFileNameWithoutExtension(), source.getFileExtension(), false);
    return source.copyFileTo (copy) ? copy : juce::File();
}

juce::StringArray Project::getMissingFiles() const
{
    juce::StringArray missing;

    forEachClip (edit.getState(), [&] (juce::ValueTree& clip)
    {
        const juce::File file (clip[ids::file].toString());
        if (! file.existsAsFile())
            missing.addIfNotAlreadyThere (file.getFullPathName());
    });

    return missing;
}

//==============================================================================
void Project::setAutosaveInterval (int seconds)
{
    if (seconds > 0)
        startTimer (seconds * 1000);
    else
        stopTimer();
}

void Project::timerCallback()
{
    if (isOpen() && dirty)
        writeBackup();
}

juce::String Project::writeBackup()
{
    if (! isOpen())
        return "No project is open.";

    if (onBeforeWrite)
        onBeforeWrite();

    // Strictly increasing times, even for two backups in the same millisecond.
    auto time = juce::jmax (juce::Time::currentTimeMillis(), lastSaveTime + 1);
    if (const auto existing = findBackups (getBackupsFolder()); ! existing.isEmpty())
        time = juce::jmax (time, backupTime (existing.getFirst()) + 1);

    if (const auto error = writeTree (getBackupsFolder().getChildFile ("autosave-" + juce::String (time) + ".xml")); error.isNotEmpty())
        return error;

    // Keep the newest few.
    auto backups = findBackups (getBackupsFolder());

    for (int i = numBackups; i < backups.size(); ++i)
        backups.getReference (i).deleteFile();

    return {};
}

juce::String Project::restoreBackup (const juce::File& backup)
{
    if (! isOpen())
        return "No project is open.";

    if (const auto error = load (backup, folder); error.isNotEmpty())
        return error;

    recoverable = juce::File();
    dirty = true; // restored, not yet saved over project.xml
    return {};
}
}
