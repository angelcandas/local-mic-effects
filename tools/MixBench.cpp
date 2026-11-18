// MicFXMixBench: the Auto Mix test bench (see analysis/MixBench.h and tools/README-mixbench.md).
//
//   MicFXMixBench <songs folder | song folder> [--style pop|rock|...|song|all] [--out folder]
//                 [--amount 0..1] [--only name] [--no-phase] [--learn folder] [--no-audio]
//
// --learn also writes each song's measurements for learning targets (analysis/MixLearn.h)
// to that folder; --no-audio keeps no rendered mixes or copies (only the scores).
//
// Writes <out>/results.csv (one row per song and style), <out>/summary.md, and each
// rendered mix as <out>/<song>-<style>.wav next to a copy of the human mix
// (<out>/<song>-human.<ext>) for listening.

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "analysis/MixBench.h"
#include "analysis/MixLearn.h"
#include "PluginProcessor.h"

#include <iostream>

namespace bench = analysis::bench;

// The DSP sources hold the plugin processor; the bench has no editor.
juce::AudioProcessorEditor* PluginProcessor::createEditor() { return nullptr; }

namespace
{
void printUsage()
{
    std::cout << "Usage: MicFXMixBench <songs folder | song folder> [--style pop|rock|...|song|all]\n"
                 "                     [--out folder] [--amount 0..1] [--only name] [--no-phase]\n"
                 "                     [--listen both|on|off] [--learn folder] [--no-audio]\n\n"
                 "A song folder holds tracks/*.wav, the human mix as mix.* and optionally song.txt\n"
                 "(style = rock, tempo = 96, role.<track file name> = voice).\n"
                 "--style song (default) mixes each song in its own style (pop when it has none).\n"
                 "--listen both (default) mixes each song with and without listen-and-correct.\n"
                 "--learn writes each song's measurements for learning (no audio) to that folder.\n"
                 "--no-audio keeps only the scores, no rendered mixes.\n";
}
}

int main (int argc, char* argv[])
{
    std::cout << std::unitbuf;
    juce::ScopedJuceInitialiser_GUI juce;

    juce::StringArray args;
    for (int i = 1; i < argc; ++i)
        args.add (juce::String::fromUTF8 (argv[i]));

    if (args.isEmpty() || args.contains ("--help") || args.contains ("-h"))
    {
        printUsage();
        return args.isEmpty() ? 1 : 0;
    }

    const auto value = [&] (const char* flag, const juce::String& fallback)
    {
        const auto index = args.indexOf (flag);
        return index >= 0 && index + 1 < args.size() ? args[index + 1] : fallback;
    };

    const auto root = juce::File::getCurrentWorkingDirectory().getChildFile (args[0]);
    const auto out = juce::File::getCurrentWorkingDirectory().getChildFile (value ("--out", root.getChildFile ("bench-results").getFullPathName()));
    const auto styleArg = value ("--style", "song");
    const auto only = value ("--only", {});
    const auto learnArg = value ("--learn", {});
    const auto learnFolder = learnArg.isNotEmpty() ? juce::File::getCurrentWorkingDirectory().getChildFile (learnArg) : juce::File();
    const auto keepAudio = ! args.contains ("--no-audio");
    if (learnFolder != juce::File())
        learnFolder.createDirectory();

    bench::Options options;
    options.amount = juce::jlimit (0.0f, 1.0f, value ("--amount", "1").getFloatValue());
    options.alignments = ! args.contains ("--no-phase");
    options.log = [] (const juce::String& line) { std::cout << line << "\n"; };
    const auto listenArg = value ("--listen", "both");
    std::vector<bool> listenModes;
    if (listenArg != "on")
        listenModes.push_back (false);
    if (listenArg != "off")
        listenModes.push_back (true);

    juce::StringArray errors;
    auto songs = bench::findSongs (root, errors);
    for (const auto& error : errors)
        std::cout << "skipped " << error << "\n";
    if (only.isNotEmpty())
        songs.erase (std::remove_if (songs.begin(), songs.end(), [&] (const auto& s) { return ! s.name.containsIgnoreCase (only); }), songs.end());
    if (songs.empty())
    {
        std::cout << "No songs under " << root.getFullPathName() << "\n";
        return 1;
    }

    std::optional<analysis::MixStyle> fixedStyle;
    if (styleArg != "song" && styleArg != "all")
    {
        fixedStyle = bench::getStyleFromKey (styleArg);
        if (! fixedStyle.has_value())
        {
            std::cout << "Unknown style " << styleArg << "\n";
            return 1;
        }
    }

    out.createDirectory();
    std::vector<bench::Run> runs;

    // --learn-only: the measurements alone, roles from song.txt or the track names.
    if (learnFolder != juce::File() && args.contains ("--learn-only"))
    {
        for (const auto& song : songs)
        {
            std::map<juce::String, analysis::Role> roles;
            for (const auto& file : song.tracks)
            {
                const auto name = file.getFileNameWithoutExtension();
                if (const auto it = song.roleOverrides.find (name); it != song.roleOverrides.end())
                    roles[name] = it->second;
                else if (const auto byName = analysis::guessRoleFromName (name))
                    roles[name] = *byName;
            }
            std::cout << song.name << "\n";
            analysis::learn::writeSong (song, roles, learnFolder.getChildFile (juce::File::createLegalFileName (song.name) + ".mlf"), options.log);
        }
        return 0;
    }

    for (const auto& song : songs)
    {
        std::vector<analysis::MixStyle> styles;
        if (styleArg == "all")
            styles.assign (std::begin (analysis::allStyles), std::end (analysis::allStyles));
        else
            styles.push_back (fixedStyle.value_or (song.style.value_or (analysis::MixStyle::pop)));

        if (keepAudio && song.mix.existsAsFile())
        {
            const auto copy = out.getChildFile (juce::File::createLegalFileName (song.name) + "-human" + song.mix.getFileExtension());
            if (! copy.existsAsFile())
                song.mix.copyFileTo (copy);
        }

        for (const auto style : styles)
        for (const auto listen : listenModes)
        {
            std::cout << song.name << " (" << bench::getStyleKey (style) << (listen ? ", listen and correct" : "") << ")\n";
            options.listen = listen;
            auto run = bench::runSong (song, style, out, options);
            if (run.error.isNotEmpty())
                std::cout << "  failed: " << run.error << "\n";
            else
            {
                for (const auto& [id, role] : run.roles)
                    std::cout << "    " << run.trackNames[id] << ": " << analysis::getRoleName (role)
                              << " (in ours " << juce::String (run.oursFit.gainDb[id], 1) << " dB"
                              << (run.hasHuman ? ", in the human mix " + juce::String (run.humanFit.gainDb[id], 1) + " dB" : juce::String())
                              << ")\n";
                std::cout << "  " << run.numMoves << " moves, " << juce::String (run.elapsed, 0) << " s";
                if (run.hasHuman)
                    std::cout << ", tone distance " << juce::String (run.getToneDistanceDb(), 1) << " dB, role error "
                              << juce::String (run.getRoleErrorLu(), 1) << " LU (fit " << juce::String (run.humanFit.fitErrorDb, 1) << " dB)";
                std::cout << "\n";
                for (const auto& note : run.plan.notes)
                    if (note.startsWith ("Listened"))
                        std::cout << "  " << note << "\n";
            }
            if (! keepAudio)
                run.rendered.deleteFile();

            // The measurements for learning, once per song (they don't depend on the style).
            if (learnFolder != juce::File() && run.error.isEmpty())
            {
                const auto file = learnFolder.getChildFile (juce::File::createLegalFileName (song.name) + ".mlf");
                if (! file.existsAsFile())
                {
                    std::map<juce::String, analysis::Role> roles;
                    for (const auto& [id, role] : run.roles)
                        roles[run.trackNames[id]] = role;
                    analysis::learn::writeSong (song, roles, file, options.log);
                }
            }

            runs.push_back (std::move (run));

            // Written after every run so a long bench can be read while it goes.
            juce::StringArray lines { bench::getCsvHeader() };
            for (const auto& r : runs)
                lines.add (bench::toCsvRow (r));
            out.getChildFile ("results.csv").replaceWithText (lines.joinIntoString ("\n") + "\n");
            out.getChildFile ("summary.md").replaceWithText (bench::summarise (runs));
        }
    }

    std::cout << "\n" << bench::summarise (runs) << "\nResults in " << out.getFullPathName() << "\n";
    return 0;
}
