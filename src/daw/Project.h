#pragma once

#include <juce_events/juce_events.h>

#include "Edit.h"

// A DAW project on disk: a folder holding the session and its audio, so it can be moved,
// zipped or backed up as one thing.
//
//   My Song.micfx/
//     project.xml   the Edit's tree; clip files inside the folder are stored relative
//     Audio/        recorded takes and imported files
//     Backups/      the last few autosaves
//     Exports/      the default place for mixdowns
//
// In memory, clip paths are always absolute, so the engine never needs to know where
// the project lives. Message thread only.
namespace daw
{
class Project : private juce::ValueTree::Listener,
                private juce::Timer
{
public:
    static constexpr const char* extension = ".micfx";
    static constexpr int numBackups = 5;

    explicit Project (Edit&);
    ~Project() override;

    // Creates a new, empty project folder (adding ".micfx" if missing) and makes it the
    // current one. The Edit is cleared. Returns an error message, or empty.
    juce::String createNew (const juce::File& folder);

    // Loads a project folder into the Edit. Returns an error message, or empty.
    juce::String open (const juce::File& folder);

    // The project folder that holds `picked`: the folder itself, its project.xml, or any
    // file or folder inside it (what a file chooser hands back varies by platform).
    // Returns an empty File if `picked` isn't part of a project.
    static juce::File findProjectFolder (const juce::File& picked);

    // Writes project.xml. Returns an error message, or empty.
    juce::String save();

    // Saves into another folder, copying the audio the project uses, and switches to it.
    juce::String saveAs (const juce::File& folder);

    // Closes the project cleanly (no crash recovery offered next time).
    void close();

    bool isOpen() const noexcept                 { return folder != juce::File(); }
    bool hasUnsavedChanges() const noexcept      { return dirty; }
    juce::File getFolder() const                 { return folder; }
    juce::String getName() const                 { return folder.getFileNameWithoutExtension(); }
    juce::File getAudioFolder() const            { return folder.getChildFile ("Audio"); }
    juce::File getExportsFolder() const          { return folder.getChildFile ("Exports"); }
    juce::File getBackupsFolder() const          { return folder.getChildFile ("Backups"); }

    // Copies an audio file into Audio/ (keeping its name, made unique) and returns the
    // copy, or an empty File on failure. Files already inside Audio/ are returned as is.
    juce::File importAudio (const juce::File&);

    // True while autosave is running (it keeps running across open, new and close).
    bool isAutosaving() const                    { return isTimerRunning(); }

    // Clip files that don't exist (moved or deleted outside the app).
    juce::StringArray getMissingFiles() const;

    //==============================================================================
    // Autosave: while a project is open and has unsaved changes, a backup is written to
    // Backups/ every `seconds`, keeping the newest numBackups.
    void setAutosaveInterval (int seconds);
    juce::String writeBackup();

    // A backup newer than project.xml left by a session that didn't close cleanly (the
    // app crashed or was killed), or an empty File. Check right after open().
    juce::File getRecoverableBackup() const      { return recoverable; }
    juce::String restoreBackup (const juce::File& backup);

    static bool isProjectFolder (const juce::File&);

private:
    juce::File getProjectFile() const   { return folder.getChildFile ("project.xml"); }
    juce::File getLockFile() const      { return getBackupsFolder().getChildFile (".open"); }

    juce::String writeTree (const juce::File& destination) const;
    juce::String load (const juce::File& xmlFile, const juce::File& projectFolder);
    void setFolder (const juce::File&);

    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override { dirty = true; }
    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override            { dirty = true; }
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override     { dirty = true; }
    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override             { dirty = true; }
    void timerCallback() override;

    Edit& edit;
    juce::File folder;
    juce::File recoverable;
    bool dirty = false;
    juce::int64 lastSaveTime = 0; // ms; backups are always stamped after it

    JUCE_DECLARE_NON_COPYABLE (Project)
};
}
