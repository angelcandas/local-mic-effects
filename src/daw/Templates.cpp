#include "Templates.h"

#include "Project.h"

namespace daw::templates
{
const std::vector<Template>& getBuiltIn()
{
    static const std::vector<Template> list {
        { "vocal", "Vocal recording",
          "A lead vocal ready to record, a double and a harmony track, a little reverb, click and count-in.",
          100.0, 4, 0, true, 1,
          { { "Lead Vocal", "", "Natural", true, -18.0f, "#ff6b8a" },
            { "Double", "", "Natural", false, -18.0f, "#ffb36b" },
            { "Harmony", "", "Natural", false, -16.0f, "#b38cff" },
            { "Backing track", "", "", false, -100.0f, "#7ee0a1" } } },

        { "podcast", "Podcast",
          "Host and guest tracks with the podcast voice chain, plus a music track. No click.",
          120.0, 4, 0, false, 0,
          { { "Host", "", "Podcast", true, -100.0f, "#5fd0ff" },
            { "Guest", "", "Podcast", false, -100.0f, "#ffd166" },
            { "Music", "", "", false, -100.0f, "#7ee0a1" } } },

        { "songwriter", "Singer-songwriter",
          "A vocal, a guitar and a piano to sketch a song on, with the click on.",
          90.0, 4, 0, true, 1,
          { { "Vocal", "", "Natural", true, -16.0f, "#ff6b8a" },
            { "Guitar", "", "", false, -22.0f, "#ffb36b" },
            { "Piano", "builtin:piano", "", false, -22.0f, "#5fd0ff" } } },

        { "band", "Band tracking",
          "Tracks for drums, bass, two guitars, keys and a vocal. Arm the ones you record.",
          110.0, 4, 0, true, 1,
          { { "Drums", "", "", false, -100.0f, "#ffb36b" },
            { "Bass", "", "", false, -100.0f, "#b38cff" },
            { "Guitar 1", "", "", false, -100.0f, "#7ee0a1" },
            { "Guitar 2", "", "", false, -100.0f, "#5fd0ff" },
            { "Keys", "builtin:keys", "", false, -100.0f, "#ffd166" },
            { "Vocal", "", "Natural", true, -18.0f, "#ff6b8a" } } },

        { "beat", "Beat making",
          "Drum kit, bass, keys and a lead on MIDI tracks, with a four-bar loop at 90 BPM.",
          90.0, 4, 4, false, 0,
          { { "Drums", "builtin:kit", "", false, -100.0f, "#ffb36b" },
            { "Bass", "builtin:bass", "", false, -100.0f, "#b38cff" },
            { "Keys", "builtin:keys", "", false, -24.0f, "#5fd0ff" },
            { "Lead", "builtin:lead", "", false, -20.0f, "#7ee0a1" } } },
    };
    return list;
}

const Template* findBuiltIn (const juce::String& key)
{
    for (const auto& t : getBuiltIn())
        if (key == t.key)
            return &t;
    return nullptr;
}

//==============================================================================
juce::File getUserFolder()
{
   #if JUCE_MAC
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("Application Support/MicFX/Templates");
   #else
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("MicFX/Templates");
   #endif
}

namespace
{
    constexpr const char* extension = ".micfxtemplate";

    juce::File fileFor (const juce::String& name)
    {
        return getUserFolder().getChildFile (cleanName (name) + extension);
    }

    void stripAudio (juce::ValueTree tree)
    {
        for (int i = tree.getNumChildren(); --i >= 0;)
        {
            auto child = tree.getChild (i);
            if (child.hasType (ids::CLIP) && ! Edit::isMidiClip (child))
                tree.removeChild (i, nullptr);
            else
                stripAudio (child);
        }
        tree.removeProperty (ids::frozenFile, nullptr);
        tree.removeProperty (ids::frozenStart, nullptr);
    }
}

juce::String cleanName (const juce::String& name)
{
    return juce::File::createLegalFileName (name.trim()).replaceCharacter ('.', '-').trim();
}

juce::Array<UserTemplate> listUser()
{
    juce::Array<UserTemplate> list;
    for (const auto& file : getUserFolder().findChildFiles (juce::File::findFiles, false, juce::String ("*") + extension))
        list.add ({ file.getFileNameWithoutExtension(), file });

    std::sort (list.begin(), list.end(), [] (const auto& a, const auto& b) { return a.name.compareNatural (b.name) < 0; });
    return list;
}

juce::String saveUser (const juce::ValueTree& editState, const juce::String& name)
{
    if (cleanName (name).isEmpty())
        return "Give the template a name.";

    auto tree = editState.createCopy();
    stripAudio (tree);
    tree.removeProperty (ids::viewState, nullptr);

    const auto file = fileFor (name);
    const auto xml = tree.createXml();
    const auto temp = file.getSiblingFile (file.getFileName() + ".tmp");

    if (! getUserFolder().createDirectory() || xml == nullptr || ! xml->writeTo (temp) || ! temp.moveFileTo (file))
    {
        temp.deleteFile();
        return "Couldn't save the template to " + file.getFullPathName() + ".";
    }
    return {};
}

juce::String removeUser (const juce::String& name)
{
    const auto file = fileFor (name);
    return file.existsAsFile() && ! file.deleteFile() ? "Couldn't delete " + file.getFileName() + "." : juce::String();
}

juce::ValueTree readUser (const juce::String& name)
{
    return Project::readTree (fileFor (name));
}
}
