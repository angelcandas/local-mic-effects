#pragma once

#include "Analysis.h"
#include "MixAssistant.h"

#include <map>
#include <optional>
#include <vector>

// The Auto Mix test bench: mixes real multitrack recordings with Auto Mix and scores
// the result against the mix a person made of the same tracks.
//
// A song is a folder:
//   tracks/*.wav (or .aif, .flac...)  the raw multitrack (any audio file in the song
//                                     folder itself counts when there's no tracks/)
//   mix.*                             the human mix (wav, flac, mp3...)
//   song.txt (optional)               lines "key = value": style = rock, tempo = 96,
//                                     and "role.<file name without extension> = voice"
//                                     to override a role guess.
//
// Scores (the audio-feature set of the learned mixers' papers, plus ours): integrated
// loudness and loudness range, crest factor, stereo width, left/right balance, tonal
// distance on 1/3-octave bands after loudness matching, voice over backing in LU, and
// each role's level against the whole mix. The per-role numbers of a mix come from
// fitting the mix's band energies over time as a sum of the raw tracks' energies with
// one gain per track (estimateContributions), so the human mix needs no stems.
//
// Everything here runs on the message thread (it builds sessions and renders them).
// Private testing only: the multitracks are not ours to ship, train on or commit.
namespace analysis::bench
{
struct Song
{
    juce::String name;
    juce::File folder;
    juce::Array<juce::File> tracks;
    juce::File mix;                         // may not exist (then nothing is compared)
    std::optional<MixStyle> style;          // from song.txt
    double tempo = 120.0;
    std::map<juce::String, Role> roleOverrides; // by file name without extension
};

// Reads a song folder. Empty (with `error` set) when it holds no tracks.
std::optional<Song> loadSong (const juce::File& folder, juce::String& error);

// Every song under `root` (a song folder itself, or a folder of them), sorted by name.
std::vector<Song> findSongs (const juce::File& root, juce::StringArray& errors);

// MixStyle from its key or name ("pop", "hiphop", "Hip-hop"...).
std::optional<MixStyle> getStyleFromKey (const juce::String&);
juce::String getStyleKey (MixStyle);

//==============================================================================
struct Features
{
    float lufs = -200.0f;         // integrated
    float lra = 0.0f;             // loudness range, LU (short-term 3 s, 10th to 95th percentile)
    float crestDb = 0.0f;         // sample peak over the RMS of the active part
    float widthDb = -200.0f;      // side over mid energy, dB (-200: mono)
    float correlation = 1.0f;     // left/right, average over the active hops
    float balanceDb = 0.0f;       // left minus right RMS, dB
    Bands tone {};                // average spectrum over the active hops, loudness matched (mean 0 dB over 40 Hz-16 kHz)
};

Features measureFeatures (const Profile&, float balanceDb = 0.0f);

// Left minus right RMS of a stereo file, dB (0 for mono or unreadable).
float measureBalanceDb (const juce::File&);

// Distance between two tonal balances: mean |difference| over 40 Hz-16 kHz (dB) and the
// slope of `a - b` against octaves (dB per octave, + is `a` brighter).
float getToneDistanceDb (const Bands& a, const Bands& b);
float getToneTiltDbPerOctave (const Bands& a, const Bands& b);

//==============================================================================
// How much of each raw track is in a mix: mix band power over time ≈ Σ gain × track band
// power, gains >= 0, fitted on the relative error of every (hop, band) where the mix
// plays. lagHops: the mix is that many hops later than the tracks (found first, ±10 s).
struct Contributions
{
    std::map<int, float> gainDb;   // by track id (-200: not heard in the mix)
    float fitErrorDb = 0.0f;       // RMS of the model's error per (hop, band), dB
    int lagHops = 0;
};

Contributions estimateContributions (const std::vector<std::pair<int, Profile>>& tracks, const Profile& mix);

struct RoleLevels
{
    std::map<Role, float> roleLu;   // each role's loudness over its active hops minus the mix's over the same hops
    float voiceOverBackingLu = 0.0f; // the lead voice over everything else while it sings (0 when no voice)
    bool hasVoice = false;
};

RoleLevels measureRoleLevels (const std::vector<std::pair<int, Profile>>& tracks, const std::map<int, Role>& roles,
                              const Contributions&, const Profile& mix);

//==============================================================================
struct Options
{
    float amount = 1.0f;
    double sampleRate = 44100.0;
    bool alignments = true;           // measure phase between tracks like the app does
    std::function<void (const juce::String&)> log; // progress lines
};

struct Run
{
    juce::String song, style;
    juce::String error;              // set when the song couldn't be mixed
    int numTracks = 0;
    double seconds = 0.0;            // song length
    double elapsed = 0.0;            // wall time of the run
    int numMoves = 0;
    std::map<int, Role> roles;
    std::map<int, juce::String> trackNames;
    juce::File rendered;             // our mix (wav)
    bool hasHuman = false;

    Features ours, human;
    Contributions oursFit, humanFit;
    RoleLevels oursLevels, humanLevels;
    RoleLevels oursExactLevels;      // ours from the tracks' own renders (no fit), to check the fit
    MixPlan plan;

    float getToneDistanceDb() const { return bench::getToneDistanceDb (ours.tone, human.tone); }
    float getToneTiltDbPerOctave() const { return bench::getToneTiltDbPerOctave (ours.tone, human.tone); }
    float getVoiceOverBackingErrorLu() const; // ours - human (0 when either has no voice)
    float getRoleErrorLu() const;             // mean |ours - human| over the roles both have
    std::optional<Role> getWorstRole (float& errorLu) const;
};

// Builds the session from the song, plans and applies Auto Mix in `style`, renders the
// mix to `outFolder`/<song>-<style>.wav and scores it against the human mix.
Run runSong (const Song&, MixStyle, const juce::File& outFolder, const Options& = {});

juce::String getCsvHeader();
juce::String toCsvRow (const Run&);

// A Markdown summary: one row per run, then the average per style and the worst misses.
juce::String summarise (const std::vector<Run>&);
}
