// Remembered audio devices (interfaces, headphones) for the standalone app: an ordered favourites
// list, the settings each device was last used with, and the rule that picks which device to use
// when devices are plugged in or pulled out. Pure logic, so it is unit-tested without hardware.
#pragma once

#include <juce_core/juce_core.h>
#include <functional>
#include <vector>

namespace devfav
{
    struct Entry
    {
        juce::String type;             // "CoreAudio", "ALSA", "Windows Audio"...
        juce::String output, input;    // device names; either may be empty
        double sampleRate = 0.0;
        int bufferSize = 0;
        int bitDepth = 0;              // what the device ran at; the OS owns it, we only remember it

        bool sameDevice (const Entry& other) const
        {
            return type == other.type && output == other.output && input == other.input;
        }

        juce::String label() const
        {
            if (output.isNotEmpty() && input.isNotEmpty() && output != input)
                return output + " / " + input;
            return output.isNotEmpty() ? output : input;
        }
    };

    struct Favourites
    {
        std::vector<Entry> list; // best first

        int indexOf (const Entry& e) const
        {
            for (size_t i = 0; i < list.size(); ++i)
                if (list[i].sameDevice (e))
                    return (int) i;
            return -1;
        }

        // Remember what a device is running with. A device seen for the first time goes to the end
        // of the list; one already there keeps its place and only gets its settings refreshed.
        void remember (const Entry& e)
        {
            if (e.output.isEmpty() && e.input.isEmpty())
                return;

            const auto i = indexOf (e);
            if (i < 0)
                list.push_back (e);
            else
                list[(size_t) i] = e;
        }

        void move (int from, int to)
        {
            if (from < 0 || from >= (int) list.size() || to < 0 || to >= (int) list.size() || from == to)
                return;
            auto e = list[(size_t) from];
            list.erase (list.begin() + from);
            list.insert (list.begin() + to, e);
        }

        void remove (int index)
        {
            if (index >= 0 && index < (int) list.size())
                list.erase (list.begin() + index);
        }

        // Which favourite should be in use now? -1 means leave things as they are.
        //  current      the device in use, or null if none opened
        //  isAvailable  is this favourite plugged in right now
        //  isNew        did it just appear (not there at the last check); at start-up everything is new
        // The best available favourite wins when the current device is gone, is not a favourite, or
        // ranks lower than a favourite that just appeared. A lower-ranked device the user chose by hand
        // stays put while nothing relevant changes.
        int choose (const Entry* current,
                    const std::function<bool (const Entry&)>& isAvailable,
                    const std::function<bool (const Entry&)>& isNew) const
        {
            int best = -1;
            for (size_t i = 0; i < list.size(); ++i)
                if (isAvailable (list[i])) { best = (int) i; break; }

            if (best < 0)
                return -1;

            if (current == nullptr || ! isAvailable (*current))
                return best;

            const auto currentIndex = indexOf (*current);
            if (currentIndex == best)
                return -1;
            if (currentIndex < 0)
                return best;                                   // in use but not a favourite
            return (currentIndex > best && isNew (list[(size_t) best])) ? best : -1;
        }

        //==============================================================================
        juce::String toString() const
        {
            juce::XmlElement root ("DEVICES");
            for (const auto& e : list)
            {
                auto* x = root.createNewChildElement ("DEVICE");
                x->setAttribute ("type", e.type);
                x->setAttribute ("output", e.output);
                x->setAttribute ("input", e.input);
                x->setAttribute ("rate", e.sampleRate);
                x->setAttribute ("buffer", e.bufferSize);
                x->setAttribute ("bits", e.bitDepth);
            }
            return root.toString (juce::XmlElement::TextFormat().singleLine());
        }

        static Favourites fromString (const juce::String& text)
        {
            Favourites result;
            if (auto xml = juce::parseXML (text))
                for (auto* x : xml->getChildWithTagNameIterator ("DEVICE"))
                {
                    Entry e;
                    e.type = x->getStringAttribute ("type");
                    e.output = x->getStringAttribute ("output");
                    e.input = x->getStringAttribute ("input");
                    e.sampleRate = x->getDoubleAttribute ("rate");
                    e.bufferSize = x->getIntAttribute ("buffer");
                    e.bitDepth = x->getIntAttribute ("bits");
                    if (result.indexOf (e) < 0 && (e.output.isNotEmpty() || e.input.isNotEmpty()))
                        result.list.push_back (e);
                }
            return result;
        }
    };
}
