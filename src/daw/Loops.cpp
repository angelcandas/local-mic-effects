#include "Loops.h"

#include "analysis/Beats.h"

#include <cmath>
#include <memory>

namespace daw::loops
{
double tempoFromName (const juce::String& name)
{
    // Split into words on anything that isn't a digit, a letter or a point; "120bpm" and
    // "120 BPM" mark a tempo for sure, a lone number between separators is a guess.
    juce::StringArray words;
    juce::String word;
    for (auto c : name + " ")
    {
        if (juce::CharacterFunctions::isLetterOrDigit (c) || c == '.')
            word += c;
        else if (word.isNotEmpty())
        {
            words.add (word);
            word.clear();
        }
    }

    const auto inRange = [] (double bpm) { return bpm >= 50.0 && bpm <= 250.0; };
    double guess = 0.0;
    for (int i = 0; i < words.size(); ++i)
    {
        const auto w = words[i].toLowerCase();
        if (w.endsWith ("bpm") && w.length() > 3 && w.dropLastCharacters (3).containsOnly ("0123456789."))
            if (const auto bpm = w.dropLastCharacters (3).getDoubleValue(); inRange (bpm))
                return bpm;
        if (w.containsOnly ("0123456789.") && w.containsAnyOf ("0123456789"))
        {
            const auto bpm = w.getDoubleValue();
            if (i + 1 < words.size() && words[i + 1].equalsIgnoreCase ("bpm") && inRange (bpm))
                return bpm;
            if (guess == 0.0 && inRange (bpm) && w.length() >= 2 && ! w.startsWith ("0"))
                guess = bpm;
        }
    }
    return guess;
}

int wholeBars (double seconds, double bpm, int beatsPerBar)
{
    if (seconds <= 0.0 || bpm <= 0.0 || beatsPerBar <= 0)
        return 0;

    const auto bars = seconds * bpm / 60.0 / beatsPerBar;
    const auto whole = (int) std::lround (bars);
    if (whole < 1 || whole > 32)
        return 0;
    return std::abs (bars - whole) <= 0.03 * whole ? whole : 0;
}

std::optional<Loop> findLoop (const juce::String& name, const juce::StringPairArray& metadata, double seconds,
                              const juce::AudioBuffer<float>* audio, double sampleRate, int beatsPerBar)
{
    if (seconds <= 0.0)
        return std::nullopt;

    const auto fit = [&] (double bpm, const char* from) -> std::optional<Loop>
    {
        if (const auto bars = wholeBars (seconds, bpm, beatsPerBar); bars > 0)
            return Loop { bars * beatsPerBar * 60.0 / seconds, bpm, bars, from };
        return std::nullopt;
    };

    // The acid chunk: one-shots are never loops; a tempo, or a beat count, is a strong hint.
    const auto oneShot = metadata.getValue (juce::WavAudioFormat::acidOneShot, "0").getIntValue() != 0;
    if (oneShot && metadata.containsKey (juce::WavAudioFormat::acidizerFlag))
        return std::nullopt;

    if (const auto bpm = tempoFromName (name); bpm > 0.0)
        if (auto loop = fit (bpm, "name"))
            return loop;

    if (const auto bpm = metadata.getValue (juce::WavAudioFormat::acidTempo, "0").getDoubleValue(); bpm > 0.0)
        if (auto loop = fit (bpm, "acid"))
            return loop;

    if (const auto beats = metadata.getValue (juce::WavAudioFormat::acidBeats, "0").getIntValue(); beats > 0)
        if (auto loop = fit (beats * 60.0 / seconds, "acid"))
            return loop;

    if (audio != nullptr && sampleRate > 0.0 && seconds <= 130.0)
    {
        const auto tempo = analysis::rhythm::detectTempo (*audio, sampleRate, beatsPerBar);
        if (tempo.bpm > 0.0 && tempo.confidence >= 0.2)
            for (const auto factor : { 1.0, 2.0, 0.5 })
                if (const auto bpm = tempo.bpm * factor; bpm >= 60.0 && bpm <= 200.0)
                    if (auto loop = fit (bpm, "audio"); loop && juce::isPowerOfTwo (loop->bars)) // 1, 2, 4... bars: a guess needs a loop's shape
                        return loop;
    }
    return std::nullopt;
}

std::optional<Loop> findLoop (const juce::File& file, int beatsPerBar)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    const std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
    if (reader == nullptr || reader->sampleRate <= 0.0 || reader->lengthInSamples <= 0)
        return std::nullopt;

    const auto seconds = (double) reader->lengthInSamples / reader->sampleRate;
    std::unique_ptr<juce::AudioBuffer<float>> audio;
    if (seconds <= 130.0)
    {
        const auto channels = (int) juce::jlimit (1u, 2u, reader->numChannels);
        audio = std::make_unique<juce::AudioBuffer<float>> (channels, (int) reader->lengthInSamples);
        reader->read (audio.get(), 0, (int) reader->lengthInSamples, 0, true, channels > 1);
    }
    return findLoop (file.getFileNameWithoutExtension(), reader->metadataValues, seconds, audio.get(), reader->sampleRate, beatsPerBar);
}
}
