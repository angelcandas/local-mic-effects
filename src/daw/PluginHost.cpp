#include "PluginHost.h"

namespace daw
{
namespace
{
    juce::File getDefaultDataFolder()
    {
       #if JUCE_MAC
        return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("Application Support/MicFX");
       #else
        return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("MicFX");
       #endif
    }

    PluginHost* currentHost = nullptr; // the one installed as the instruments' loader
}

PluginHost::PluginHost() : PluginHost (getDefaultDataFolder()) {}

PluginHost::PluginHost (const juce::File& folder) : juce::Thread ("MicFX plugin scan"), dataFolder (folder)
{
    juce::addDefaultFormatsToManager (formats);

    if (auto xml = juce::XmlDocument::parse (listFile()))
        known.recreateFromXml (*xml);

    extraFolders = juce::StringArray::fromLines (foldersFile().loadFileAsString());
    extraFolders.removeEmptyStrings();

    previousHost = currentHost;
    currentHost = this;
    instruments::setPluginLoader ([] (const juce::ValueTree& instrument, double rate, int block, juce::String& error)
                                  -> std::unique_ptr<juce::AudioProcessor>
    {
        if (currentHost == nullptr)
        {
            error = "Instrument plugins aren't available.";
            return {};
        }

        return currentHost->load (instrument, rate, block, error);
    });
}

PluginHost::~PluginHost()
{
    cancelPendingUpdate();
    stopThread (10000);

    if (currentHost == this)
        currentHost = previousHost;
}

//==============================================================================
Edit::InstrumentInfo PluginHost::describe (const juce::PluginDescription& description)
{
    Edit::InstrumentInfo info;
    info.id = description.createIdentifierString();
    info.name = description.name;
    info.format = description.pluginFormatName;
    info.vendor = description.manufacturerName;

    if (auto xml = description.createXml())
        info.description = xml->toString (juce::XmlElement::TextFormat().singleLine().withoutHeader());

    return info;
}

namespace
{
    juce::Array<juce::PluginDescription> sortedByName (juce::Array<juce::PluginDescription> types)
    {
        std::sort (types.begin(), types.end(), [] (const auto& a, const auto& b)
        {
            return a.name.compareNatural (b.name, false) != 0 ? a.name.compareNatural (b.name, false) < 0
                                                              : a.pluginFormatName < b.pluginFormatName;
        });
        return types;
    }

    // An effect: anything that isn't an instrument. (A VST3 scan doesn't know the channel
    // counts without loading the plugin, so a MIDI-only plugin is listed too; as an insert
    // it would just pass its input on.)
    bool isEffect (const juce::PluginDescription& type) { return ! type.isInstrument; }
}

juce::Array<Edit::InstrumentInfo> PluginHost::getInstruments() const
{
    auto list = instruments::getBuiltins();

    for (const auto& type : sortedByName (known.getTypes()))
        if (type.isInstrument)
            list.add (describe (type));

    return list;
}

juce::Array<Edit::InstrumentInfo> PluginHost::getEffects() const
{
    juce::Array<Edit::InstrumentInfo> list;

    for (const auto& type : sortedByName (known.getTypes()))
        if (isEffect (type))
            list.add (describe (type));

    return list;
}

std::optional<Edit::InstrumentInfo> PluginHost::findEffect (const juce::String& id) const
{
    for (const auto& type : known.getTypes())
        if (isEffect (type) && type.createIdentifierString() == id)
            return describe (type);

    return std::nullopt;
}

std::optional<Edit::InstrumentInfo> PluginHost::findInstrument (const juce::String& id) const
{
    if (auto builtin = instruments::findBuiltin (id))
        return builtin;

    for (const auto& type : known.getTypes())
        if (type.createIdentifierString() == id)
            return describe (type);

    return std::nullopt;
}

//==============================================================================
juce::StringArray PluginHost::getExtraFolders() const
{
    const juce::ScopedLock sl (lock);
    return extraFolders;
}

void PluginHost::setExtraFolders (const juce::StringArray& folders)
{
    {
        const juce::ScopedLock sl (lock);
        extraFolders = folders;
        extraFolders.removeEmptyStrings();
        extraFolders.removeDuplicates (false);
    }

    dataFolder.createDirectory();
    foldersFile().replaceWithText (getExtraFolders().joinIntoString ("\n"));
}

juce::FileSearchPath PluginHost::getSearchPath (juce::AudioPluginFormat& format) const
{
    auto path = format.getDefaultLocationsToSearch();

    for (const auto& folder : getExtraFolders())
        if (juce::File::isAbsolutePath (folder) && juce::File (folder).isDirectory())
            path.addIfNotAlreadyThere (juce::File (folder));

    return path;
}

juce::String PluginHost::getScanningName() const
{
    const juce::ScopedLock sl (lock);
    return scanningName;
}

juce::StringArray PluginHost::getFailedFiles() const
{
    const juce::ScopedLock sl (lock);
    return failed;
}

void PluginHost::startScan (bool fullRescan)
{
    if (isThreadRunning())
        return;

    if (fullRescan)
        known.clear();

    {
        const juce::ScopedLock sl (lock);
        failed.clear();
        scanningName.clear();
    }

    progress = 0.0f;
    dataFolder.createDirectory();
    startThread (juce::Thread::Priority::background);
}

void PluginHost::cancelScan()
{
    signalThreadShouldExit();
}

void PluginHost::run()
{
    const auto numFormats = juce::jmax (1, formats.getNumFormats());

    for (int f = 0; f < formats.getNumFormats() && ! threadShouldExit(); ++f)
    {
        auto& format = *formats.getFormat (f);
        if (! format.canScanForPlugins())
            continue;

        juce::PluginDirectoryScanner scanner (known, format, getSearchPath (format), true, deadMansPedal(), true);
        juce::String name;

        for (;;)
        {
            if (threadShouldExit())
                break;

            {
                const juce::ScopedLock sl (lock);
                scanningName = scanner.getNextPluginFileThatWillBeScanned();
            }

            const auto more = scanner.scanNextFile (true, name);
            progress = ((float) f + juce::jlimit (0.0f, 1.0f, scanner.getProgress())) / (float) numFormats;

            if (! more)
                break;
        }

        const juce::ScopedLock sl (lock);
        failed.addArray (scanner.getFailedFiles());
    }

    progress = 1.0f;
    triggerAsyncUpdate();
}

void PluginHost::handleAsyncUpdate()
{
    saveList();

    if (onScanFinished)
        onScanFinished();
}

void PluginHost::saveList()
{
    dataFolder.createDirectory();

    if (auto xml = known.createXml())
        xml->writeTo (listFile());
}

//==============================================================================
std::unique_ptr<juce::AudioPluginInstance> PluginHost::load (const juce::ValueTree& instrument, double sampleRate,
                                                             int blockSize, juce::String& error)
{
    const auto id = instrument[ids::plugin].toString();
    std::optional<juce::PluginDescription> description;

    for (const auto& type : known.getTypes())
        if (type.createIdentifierString() == id)
            description = type;

    if (! description.has_value())
    {
        // Not scanned here: try where it was when the project was saved.
        juce::PluginDescription saved;
        if (auto xml = juce::parseXML (instrument[ids::description].toString()); xml != nullptr && saved.loadFromXml (*xml))
            description = saved;
    }

    if (! description.has_value())
    {
        error = instrument[ids::name].toString() + " isn't installed (or hasn't been scanned yet).";
        return {};
    }

    juce::String message;
    auto plugin = formats.createPluginInstance (*description, sampleRate, blockSize, message);

    if (plugin == nullptr)
    {
        error = "Couldn't load " + description->name + (message.isNotEmpty() ? ": " + message : juce::String ("."));
        return {};
    }

    // Stereo out on the main bus when the plugin allows it (sample players often
    // default to many outputs); everything else as the plugin likes.
    auto layout = plugin->getBusesLayout();
    if (! layout.outputBuses.isEmpty() && layout.outputBuses.getReference (0) != juce::AudioChannelSet::stereo())
    {
        auto stereo = layout;
        stereo.outputBuses.getReference (0) = juce::AudioChannelSet::stereo();
        if (plugin->checkBusesLayoutSupported (stereo))
            plugin->setBusesLayout (stereo);
    }

    // An effect: stereo in as well where it can, else mono in and out (the insert sums
    // the two sides into a mono plugin and plays its output on both).
    layout = plugin->getBusesLayout();
    if (! layout.inputBuses.isEmpty() && ! layout.outputBuses.isEmpty()
        && (layout.getMainInputChannelSet() != juce::AudioChannelSet::stereo()
            || layout.getMainOutputChannelSet() != juce::AudioChannelSet::stereo()))
    {
        for (const auto& set : { juce::AudioChannelSet::stereo(), juce::AudioChannelSet::mono() })
        {
            auto tried = layout;
            tried.inputBuses.getReference (0) = set;
            tried.outputBuses.getReference (0) = set;
            if (plugin->checkBusesLayoutSupported (tried) && plugin->setBusesLayout (tried))
                break;
        }
    }

    return plugin;
}
}
