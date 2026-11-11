#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "Instruments.h"

// Finds and loads instrument plugins (VST3 everywhere, AU on macOS) for MIDI tracks.
//
// Scanning runs on a background thread and looks in each format's usual folders plus
// any folders added by hand. What it finds is kept in Plugins.xml in the app's data
// folder, so later launches don't scan again. A plugin that crashes the scan is noted
// (dead man's pedal) and skipped next time.
//
// The host installs itself as the instruments' plugin loader (instruments::setPluginLoader)
// while it exists. Any plugin with a VST3 or AU build works the same way, including big
// sample players such as Kontakt: they're loaded with their saved state and their own
// editor window.
namespace daw
{
class PluginHost final : private juce::Thread,
                         private juce::AsyncUpdater
{
public:
    // Keeps its list in the app's data folder ("MicFX" in the user's app data).
    PluginHost();
    // Keeps its list in `dataFolder` (the tests use a temporary one).
    explicit PluginHost (const juce::File& dataFolder);
    ~PluginHost() override;

    //==============================================================================
    // Every instrument: the built-in synths first, then plugins by name. Any thread.
    juce::Array<Edit::InstrumentInfo> getInstruments() const;
    std::optional<Edit::InstrumentInfo> findInstrument (const juce::String& id) const;
    static Edit::InstrumentInfo describe (const juce::PluginDescription&);

    //==============================================================================
    // Message thread. Scans in the background; onScanFinished runs on the message thread.
    // A full rescan forgets the list first (otherwise known, unchanged files are skipped).
    void startScan (bool fullRescan = false);
    void cancelScan();
    bool isScanning() const                     { return isThreadRunning(); }
    float getScanProgress() const               { return progress.load(); }
    juce::String getScanningName() const;
    juce::StringArray getFailedFiles() const;   // files the last scan couldn't read
    std::function<void()> onScanFinished;

    // Extra folders to scan, besides each format's usual ones.
    juce::StringArray getExtraFolders() const;
    void setExtraFolders (const juce::StringArray&);
    // The folders a scan looks in for one format (the usual ones plus the extra ones).
    juce::FileSearchPath getSearchPath (juce::AudioPluginFormat&) const;

    juce::AudioPluginFormatManager& getFormats() noexcept { return formats; }
    juce::KnownPluginList& getKnownPlugins() noexcept     { return known; }

    //==============================================================================
    // Message thread. Loads an INSTRUMENT's plugin: found by its id among the scanned
    // plugins, or else from its saved description (a project from another computer).
    std::unique_ptr<juce::AudioPluginInstance> load (const juce::ValueTree& instrument, double sampleRate, int blockSize,
                                                     juce::String& error);

private:
    void run() override;
    void handleAsyncUpdate() override;
    void saveList();

    juce::File dataFolder;
    juce::File listFile() const        { return dataFolder.getChildFile ("Plugins.xml"); }
    juce::File foldersFile() const     { return dataFolder.getChildFile ("Plugin folders.txt"); }
    juce::File deadMansPedal() const   { return dataFolder.getChildFile ("Plugin scan.tmp"); }

    juce::AudioPluginFormatManager formats;
    juce::KnownPluginList known;

    mutable juce::CriticalSection lock; // scanningName, failed, extraFolders
    juce::String scanningName;
    juce::StringArray failed, extraFolders;
    std::atomic<float> progress { 0.0f };
    PluginHost* previousHost = nullptr; // the loader before this one, put back when this goes

    JUCE_DECLARE_NON_COPYABLE (PluginHost)
};
}
