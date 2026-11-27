#pragma once

#include <juce_audio_formats/juce_audio_formats.h>

// Which audio files the app can open, and a fallback for the ones JUCE can't decode itself.
//
// JUCE reads WAV, AIFF, FLAC and Ogg Vorbis everywhere, MP3 with its own decoder, and on macOS
// (CoreAudio) and Windows (Media Foundation) also AAC / M4A / MP4 and the audio of video files.
// On Linux there is no AAC decoder in JUCE, so a file it can't open (an .mp4, .m4a, .opus, .mkv
// ...) is converted to a WAV with ffmpeg when ffmpeg is installed. That is also how the audio of
// a video gets in on every platform where JUCE only reads the audio-only containers.
namespace micfx::audioimport
{
    // Extensions we offer in file pickers, on top of what the JUCE formats report.
    inline juce::StringArray extraExtensions()
    {
        return { ".wav", ".aif", ".aiff", ".flac", ".ogg", ".oga", ".opus", ".mp3", ".m4a", ".mp4", ".m4v", ".aac",
                 ".mov", ".caf", ".wma", ".webm", ".mkv", ".3gp" };
    }

    // "*.wav;*.mp3;..." for a juce::FileChooser.
    inline juce::String wildcard()
    {
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        auto list = extraExtensions();
        for (auto* format : formats)
            for (const auto& extension : format->getFileExtensions())
                list.addIfNotAlreadyThere (extension.toLowerCase());

        juce::StringArray patterns;
        for (const auto& extension : list)
            patterns.add ("*" + extension);
        return patterns.joinIntoString (";");
    }

    inline bool jucePlaysIt (const juce::File& file)
    {
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        const std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
        return reader != nullptr && reader->lengthInSamples > 0;
    }

    inline juce::File findFfmpeg()
    {
        static const auto found = []
        {
            for (const char* path : { "/opt/homebrew/bin/ffmpeg", "/usr/local/bin/ffmpeg", "/usr/bin/ffmpeg", "/snap/bin/ffmpeg" })
                if (juce::File (path).existsAsFile())
                    return juce::File (path);

            juce::ChildProcess which;
           #if JUCE_WINDOWS
            if (which.start ("where ffmpeg") && which.waitForProcessToFinish (3000))
           #else
            if (which.start ("which ffmpeg") && which.waitForProcessToFinish (3000))
           #endif
            {
                const juce::File file (which.readAllProcessOutput().upToFirstOccurrenceOf ("\n", false, false).trim());
                if (file.existsAsFile())
                    return file;
            }
            return juce::File();
        }();
        return found;
    }

    // The file itself when JUCE can read it. Otherwise a WAV decoded with ffmpeg (kept in a cache
    // folder, named after the file's path and date so the same file converts once), or `error`
    // says why not. Slow only for the files JUCE can't read: a few seconds for a long song.
    inline juce::File makeReadable (const juce::File& file, juce::String& error)
    {
        error = {};
        if (jucePlaysIt (file))
            return file;

        const auto ffmpeg = findFfmpeg();
        if (ffmpeg == juce::File())
        {
            error = "Couldn't open " + file.getFileName() + ". This computer can't decode "
                  + (file.getFileExtension().isEmpty() ? juce::String ("that format") : file.getFileExtension().toLowerCase())
                  + " files; try a WAV, AIFF, FLAC, Ogg or MP3 file, or install ffmpeg.";
            return {};
        }

        const auto folder = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("MicFX imports");
        folder.createDirectory();
        const auto key = juce::String::toHexString (juce::String (file.getFullPathName() + "|" + juce::String (file.getLastModificationTime().toMilliseconds())
                                                                  + "|" + juce::String (file.getSize())).hashCode64());
        const auto out = folder.getChildFile (key).getChildFile (juce::File::createLegalFileName (file.getFileNameWithoutExtension()) + ".wav");
        out.getParentDirectory().createDirectory();
        if (out.existsAsFile() && jucePlaysIt (out))
            return out;

        juce::StringArray args { ffmpeg.getFullPathName(), "-nostdin", "-y", "-v", "error", "-i", file.getFullPathName(),
                                 "-vn", "-map", "0:a:0", "-c:a", "pcm_s24le", out.getFullPathName() };
        juce::ChildProcess process;
        if (! process.start (args, juce::ChildProcess::wantStdErr) || ! process.waitForProcessToFinish (180000)
            || process.getExitCode() != 0 || ! jucePlaysIt (out))
        {
            process.kill();
            out.deleteFile();
            error = "Couldn't read any audio from " + file.getFileName() + ".";
            return {};
        }
        return out;
    }
}
