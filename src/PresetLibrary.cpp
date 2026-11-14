#include "PresetLibrary.h"
#include "dsp/FxModule.h"

#include <algorithm>
#include <set>

namespace
{
    const juce::Identifier PRESET { "PRESET" }, VALUE { "VALUE" }, INSERTS { "INSERTS" }, INSERT { "INSERT" },
                           kindId { "kind" }, nameId { "name" }, idId { "id" }, valueId { "value" }, typeId { "type" };

    const char* effectExtension = ".micfxfx";
    const char* chainExtension = ".micfxchain";

    juce::File getDefaultRoot()
    {
       #if JUCE_MAC
        return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("Application Support/MicFX/Library");
       #else
        return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("MicFX/Library");
       #endif
    }

    juce::ValueTree readTree (const juce::File& file)
    {
        if (! file.existsAsFile())
            return {};
        if (const auto xml = juce::XmlDocument::parse (file))
            return juce::ValueTree::fromXml (*xml);
        return {};
    }

    juce::String writeTree (const juce::File& file, const juce::ValueTree& tree)
    {
        if (! file.getParentDirectory().createDirectory())
            return "Couldn't make the folder " + file.getParentDirectory().getFullPathName();
        const auto xml = tree.createXml();
        if (xml == nullptr || ! xml->writeTo (file))
            return "Couldn't write " + file.getFullPathName();
        return {};
    }

    // The presets in a folder, by name (a name shown is the one inside the file).
    juce::Array<juce::File> filesIn (const juce::File& folder, const char* extension)
    {
        auto files = folder.findChildFiles (juce::File::findFiles, false, juce::String ("*") + extension);
        files.sort();
        return files;
    }

    juce::String nameOf (const juce::ValueTree& tree, const juce::File& file)
    {
        const auto name = tree[nameId].toString();
        return name.isNotEmpty() ? name : file.getFileNameWithoutExtension();
    }
}

PresetLibrary::PresetLibrary() : PresetLibrary (getDefaultRoot()) {}

PresetLibrary::PresetLibrary (juce::File folder) : root (std::move (folder)) {}

juce::String PresetLibrary::safeName (const juce::String& name)
{
    auto safe = name.trim().substring (0, 60);
    safe = safe.replaceCharacters ("/\\:*?\"<>|", "---------");
    safe = safe.removeCharacters ("\r\n\t").trim();
    while (safe.startsWith ("."))
        safe = safe.substring (1);
    return safe.trim();
}

juce::File PresetLibrary::effectFile (const juce::String& kind, const juce::String& name) const
{
    return root.getChildFile ("Effects").getChildFile (safeName (kind)).getChildFile (safeName (name) + effectExtension);
}

juce::File PresetLibrary::chainFile (const juce::String& name) const
{
    return root.getChildFile ("Chains").getChildFile (safeName (name) + chainExtension);
}

juce::File PresetLibrary::defaultFile (const juce::String& kind) const
{
    return root.getChildFile ("Defaults").getChildFile ("Effects").getChildFile (safeName (kind) + effectExtension);
}

//==============================================================================
juce::String PresetLibrary::writeValues (const juce::File& file, const juce::String& kind, const juce::String& name, const juce::var& values)
{
    const auto* object = values.getDynamicObject();
    if (object == nullptr)
        return "There are no settings to save.";

    juce::ValueTree preset (PRESET);
    preset.setProperty (kindId, kind, nullptr);
    preset.setProperty (nameId, name.trim().substring (0, 60), nullptr);

    for (const auto& [id, value] : object->getProperties())
    {
        const auto number = (double) value;
        if (! std::isfinite (number))
            continue;
        juce::ValueTree entry (VALUE);
        entry.setProperty (idId, id.toString(), nullptr);
        entry.setProperty (valueId, number, nullptr);
        preset.appendChild (entry, nullptr);
    }

    return writeTree (file, preset);
}

juce::var PresetLibrary::readValues (const juce::File& file)
{
    const auto preset = readTree (file);
    if (! preset.hasType (PRESET))
        return {};

    auto* values = new juce::DynamicObject();
    for (const auto& entry : preset)
        if (entry.hasType (VALUE) && entry[idId].toString().isNotEmpty())
            values->setProperty (juce::Identifier (entry[idId].toString()), (double) entry[valueId]);
    return juce::var (values);
}

juce::var PresetLibrary::listEffects() const
{
    auto* result = new juce::DynamicObject();

    for (const auto& folder : root.getChildFile ("Effects").findChildFiles (juce::File::findDirectories, false))
    {
        juce::Array<juce::var> presets;
        juce::String kind = folder.getFileName();

        for (const auto& file : filesIn (folder, effectExtension))
        {
            const auto tree = readTree (file);
            if (! tree.hasType (PRESET))
                continue;
            if (tree[kindId].toString().isNotEmpty())
                kind = tree[kindId].toString();

            auto* preset = new juce::DynamicObject();
            preset->setProperty (nameId, nameOf (tree, file));
            preset->setProperty ("values", readValues (file));
            presets.add (juce::var (preset));
        }

        std::sort (presets.begin(), presets.end(), [] (const juce::var& a, const juce::var& b)
                   { return a["name"].toString().compareNatural (b["name"].toString()) < 0; });
        if (! presets.isEmpty())
            result->setProperty (juce::Identifier (kind), presets);
    }

    return juce::var (result);
}

juce::String PresetLibrary::saveEffect (const juce::String& kind, const juce::String& name, const juce::var& values)
{
    if (safeName (kind).isEmpty())
        return "Which effect is this preset for?";
    if (safeName (name).isEmpty())
        return "Give the preset a name.";
    return writeValues (effectFile (kind, name), kind, name, values);
}

bool PresetLibrary::deleteEffect (const juce::String& kind, const juce::String& name)
{
    const auto file = effectFile (kind, name);
    return file.existsAsFile() && file.deleteFile();
}

juce::String PresetLibrary::renameEffect (const juce::String& kind, const juce::String& name, const juce::String& newName)
{
    const auto from = effectFile (kind, name);
    if (! from.existsAsFile())
        return "There's no preset called " + name + ".";
    if (safeName (newName).isEmpty())
        return "Give the preset a name.";

    const auto values = readValues (from);
    if (const auto error = writeValues (effectFile (kind, newName), kind, newName, values); error.isNotEmpty())
        return error;
    if (effectFile (kind, newName) != from)
        from.deleteFile();
    return {};
}

juce::var PresetLibrary::getDefault (const juce::String& kind) const
{
    return readValues (defaultFile (kind));
}

juce::String PresetLibrary::setDefault (const juce::String& kind, const juce::var& values)
{
    if (safeName (kind).isEmpty())
        return "Which effect is this default for?";
    return writeValues (defaultFile (kind), kind, "Default", values);
}

bool PresetLibrary::clearDefault (const juce::String& kind)
{
    const auto file = defaultFile (kind);
    return file.existsAsFile() && file.deleteFile();
}

//==============================================================================
bool PresetLibrary::isLiveChain (const juce::ValueTree& inserts)
{
    std::set<juce::String> seen;

    for (const auto& insert : inserts)
    {
        if (! insert.hasType (INSERT))
            continue;
        const auto type = insert[typeId].toString();
        bool builtIn = false;
        for (int t = 0; t < fx::numModuleTypes && ! builtIn; ++t)
            builtIn = type == fx::getModuleKey ((fx::ModuleType) t);

        if (! builtIn || ! seen.insert (type).second)
            return false;
    }

    return true;
}

juce::var PresetLibrary::listChains() const
{
    juce::Array<juce::var> result;

    for (const auto& file : filesIn (root.getChildFile ("Chains"), chainExtension))
    {
        const auto tree = readTree (file);
        if (! tree.hasType (INSERTS))
            continue;

        juce::Array<juce::var> effects;
        for (const auto& insert : tree)
            if (insert.hasType (INSERT))
                effects.add (insert[typeId].toString());

        auto* chain = new juce::DynamicObject();
        chain->setProperty (nameId, nameOf (tree, file));
        chain->setProperty ("effects", effects);
        chain->setProperty ("live", isLiveChain (tree));
        result.add (juce::var (chain));
    }

    std::sort (result.begin(), result.end(), [] (const juce::var& a, const juce::var& b)
               { return a["name"].toString().compareNatural (b["name"].toString()) < 0; });
    return result;
}

juce::String PresetLibrary::saveChain (const juce::String& name, const juce::ValueTree& inserts, const juce::ValueTree& macros)
{
    if (safeName (name).isEmpty())
        return "Give the chain a name.";

    juce::ValueTree chain (INSERTS);
    chain.setProperty (nameId, name.trim().substring (0, 60), nullptr);

    for (const auto& insert : inserts)
    {
        if (! insert.hasType (INSERT))
            continue;
        auto copy = insert.createCopy();
        copy.removeProperty (idId, nullptr); // ids belong to a project
        chain.appendChild (copy, nullptr);
    }
    if (macros.isValid())
        chain.appendChild (macros.createCopy(), nullptr);

    return writeTree (chainFile (name), chain);
}

juce::ValueTree PresetLibrary::loadChain (const juce::String& name) const
{
    auto tree = readTree (chainFile (name));
    return tree.hasType (INSERTS) ? tree : juce::ValueTree();
}

juce::ValueTree PresetLibrary::getDefaultChain() const
{
    auto tree = readTree (root.getChildFile ("Defaults").getChildFile (juce::String ("Audio track") + chainExtension));
    return tree.hasType (INSERTS) ? tree : juce::ValueTree();
}

juce::String PresetLibrary::setDefaultChain (const juce::ValueTree& inserts, const juce::ValueTree& macros)
{
    juce::ValueTree chain (INSERTS);
    chain.setProperty (nameId, "Audio track", nullptr);
    for (const auto& insert : inserts)
        if (insert.hasType (INSERT))
        {
            auto copy = insert.createCopy();
            copy.removeProperty (idId, nullptr);
            chain.appendChild (copy, nullptr);
        }
    if (macros.isValid())
        chain.appendChild (macros.createCopy(), nullptr);
    return writeTree (root.getChildFile ("Defaults").getChildFile (juce::String ("Audio track") + chainExtension), chain);
}

bool PresetLibrary::clearDefaultChain()
{
    const auto file = root.getChildFile ("Defaults").getChildFile (juce::String ("Audio track") + chainExtension);
    return file.existsAsFile() && file.deleteFile();
}

bool PresetLibrary::deleteChain (const juce::String& name)
{
    const auto file = chainFile (name);
    return file.existsAsFile() && file.deleteFile();
}

juce::String PresetLibrary::renameChain (const juce::String& name, const juce::String& newName)
{
    auto chain = loadChain (name);
    if (! chain.isValid())
        return "There's no chain called " + name + ".";
    if (safeName (newName).isEmpty())
        return "Give the chain a name.";

    chain.setProperty (nameId, newName.trim().substring (0, 60), nullptr);
    if (const auto error = writeTree (chainFile (newName), chain); error.isNotEmpty())
        return error;
    if (chainFile (newName) != chainFile (name))
        chainFile (name).deleteFile();
    return {};
}

//==============================================================================
int PresetLibrary::importOnce (const juce::String& json)
{
    const auto marker = root.getChildFile (".imported");
    if (marker.existsAsFile())
        return 0;

    int count = 0;
    const auto parsed = juce::JSON::parse (json);

    if (const auto* kinds = parsed.getDynamicObject())
        for (const auto& [kind, presets] : kinds->getProperties())
            if (const auto* list = presets.getArray())
                for (const auto& preset : *list)
                {
                    const auto name = preset["name"].toString();
                    // Saved later in the library wins over the old copy.
                    if (effectFile (kind.toString(), name).existsAsFile())
                        continue;
                    if (saveEffect (kind.toString(), name, preset["values"]).isEmpty())
                        ++count;
                }

    if (root.createDirectory())
        marker.replaceWithText ("The presets from the app's settings were copied here.\n");
    return count;
}
