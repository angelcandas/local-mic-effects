# MicFX: local mic effects

Real-time voice effects for your microphone. Everything runs locally on your machine with a few
milliseconds of latency. Built for macOS (Windows planned) in C++ with [JUCE 9](https://juce.com); the UI
is HTML/CSS/JS running in a native WebView.

Builds as a **standalone app**, plus **AU** and **VST3** plugins from the same code.

## What's inside

**24 effects**, processed in this order (each can be switched on/off):

| Category | Effects |
| --- | --- |
| Clean-up | High-pass · Noise Gate · EQ · Compressor · De-esser |
| Pitch | **Autotune** · Pitch Shift (with formant) · Harmonizer (2 voices) |
| Character | Exciter · Saturation · Radio · Bitcrusher · Robot · Filter · Auto-wah |
| Modulation | Tremolo · Vibrato · Chorus · Flanger · Phaser |
| Space | Doubler · Echo (ping-pong) · Reverb · Stereo Width |

```
mic → input gain → high-pass → gate → autotune/pitch/harmonizer → EQ → compressor → de-esser
    → exciter → saturation → radio → bitcrusher → robot → filter → auto-wah → tremolo → vibrato
    → chorus → flanger → phaser → doubler → echo → reverb → width → output gain → limiter (−1 dB)
```

**24 voice presets** in five groups: Clean (Podcast, Radio Host…), Tuned (Pop Tune, Hard Tune, Choir…),
Characters (Deep Voice, Chipmunk, Demon, Robot, Alien, Ghost, Wah Funk), Lo-fi (Telephone,
Walkie-Talkie, Megaphone, 8-Bit, Underwater, Tape) and Space (Stadium, Cathedral, Astronaut).
They also appear as programs in DAWs.

### Autotune

- A YIN pitch detector tracks your voice every 5 ms.
- The nearest note in the chosen **key and scale** becomes the target. Scales: chromatic, major,
  minor, harmonic minor, pentatonics and blues.
- A TD-PSOLA shifter moves your voice to that note while keeping your vocal character (formants).
- **Speed** at 0 ms gives the robotic "hard tune" sound. 20–60 ms sounds natural.
- **Amount** below 100% only partially corrects the pitch.
- Added latency is roughly half a pitch period (~3–6 ms).

The Pitch Shift and Harmonizer use the same engine, so they're tuned too when Autotune is on.
Chromatic works for any song. Pick the song's key and scale for a tighter, more musical tune.

### Sing along and record

The panel at the bottom of the window is a small multi-track player:

1. **+ Backing track** loads a song (WAV, AIFF, FLAC, Ogg or MP3; on macOS also M4A). It appears
   as the first row with its waveform.
2. Press **Record** (or **R**). The music plays, and you hear it mixed with your processed voice.
3. Press **Stop**. Your take lands on its own row below the music, at the point where you started
   singing. Record again for more takes; earlier takes play back while you record.

- **Play/pause** (or **Space**) and **back to start** control the transport. Click a row to move the playhead.
- Each row has **M** (mute), a volume slider (double-click for 0 dB) and **×** (remove from the
  session; the file stays on disk).
- Takes contain only your voice, never the backing track. In the app they're shifted earlier by the
  audio interface's round-trip latency so they line up with what you heard.
- Takes are 24-bit WAV files in `~/Music/MicFX Recordings` by default. **Change…** picks another
  folder. **Save dry mic** also writes your unprocessed voice to a second, sample-aligned file
  (`… (dry).wav`), so you can re-process it later.
- Disk access and file decoding run on background threads, so they don't add load to the audio thread.
- The session (which tracks are loaded) isn't saved yet when you quit.

## Requirements

- Xcode Command Line Tools (`xcode-select --install`)
- CMake ≥ 3.22 and Ninja: `brew install cmake ninja`

## Build & run

```bash
git clone --recurse-submodules https://github.com/angelcandas/local-mic-effects.git
cd local-mic-effects
cmake --preset debug
cmake --build --preset app              # standalone only; use --preset debug for AU/VST3 + tests
open build/debug/MicFX_artefacts/Debug/Standalone/MicFX.app
```

On first launch (and after rebuilds, which change the app's signature):

1. Allow microphone access.
2. JUCE mutes the input to prevent feedback. Click **Settings…** in the yellow bar and untick
   **Mute audio input**. **Use headphones**, or you'll get feedback.
3. In **Options → Audio/MIDI Settings**, pick your mic and set the buffer size. 128 samples at 48 kHz ≈ 2.7 ms per buffer.

## Updating

**Options → Check for updates…** in the standalone app fetches the latest version, lists what's
new, and on **Update and rebuild** pulls it, updates JUCE and rebuilds the app. Then
**Relaunch MicFX** restarts it. The new build has a new signature, so macOS asks for mic access again.

The same steps work from a terminal:

```bash
scripts/update.sh check     # what's new (exit 0 = up to date, 10 = update available)
scripts/update.sh install   # pull + submodule update + cmake --preset debug + cmake --build --preset app
```

It updates from the current branch's upstream (`origin/main` for a normal clone), and won't
touch a checkout with uncommitted changes or local commits.

## Working on the UI

Debug builds load `ui/` straight from the source folder. To see a change:

1. Edit HTML, CSS or JS.
2. Right-click the MicFX window → **Reload**. No rebuild is needed, so macOS won't ask for mic permission again.

- **Inspect Element** in the same menu opens Safari's Web Inspector.
- Uncaught JavaScript errors are also printed to the app's log (VS Code debug console or terminal) as `[MicFX UI] …`.
- Release builds embed the UI into the binary.
- To tweak the layout in a normal browser, serve the folder with `npx serve ui` (`ui/juce` links to JUCE's JS library).
  Without the C++ backend the controls show placeholder values.

## Tests

A headless test runner checks pitch detection, pitch-shift and autotune accuracy (in cents). It
also runs every preset through the full processor, checking for NaNs, clipping and CPU load.

```bash
cmake --build --preset debug --target MicFXTests
./build/debug/MicFXTests_artefacts/Debug/MicFXTests
```

## VS Code

Recommended extensions (auto-suggested): CMake Tools, clangd, CodeLLDB.

| Action | How |
| --- | --- |
| Build | `⇧⌘B` (standalone, debug) |
| Run | Command Palette → *Tasks: Run Task* → **Run MicFX** |
| Debug | `F5` → **Debug MicFX** (macOS asks for mic access for VS Code) |
| Attach | Run the app first, then **Attach to MicFX** |

IntelliSense comes from `build/debug/compile_commands.json`, so run a configure once before it works.

## Project layout

```
src/Parameters.*        every parameter: IDs, ranges, defaults
src/Presets.*           factory voice presets
src/PluginProcessor.*   audio thread: the effects chain
src/Recorder.*          WAV recording of the output (and dry mic) on a background thread
src/Multitrack.*        backing track + takes: loading, resampling, timeline playback
src/PluginEditor.*      WebView host: parameter relays, presets, meters, embedded files
src/StandaloneApp.cpp   standalone app window, with Options → Check for updates…
scripts/update.sh       update check / pull + rebuild, used by Check for updates…
src/dsp/                PitchDetector (YIN), PsolaShifter, PitchEngine (autotune/pitch/harmony),
                        Effects (EQ, de-esser, saturation, echo…), Biquad
ui/modules.js           the effect catalog the UI is built from
ui/app.js, styles.css   the interface
tests/DspTests.cpp      headless DSP test runner
libs/JUCE               JUCE 9.0.2 (git submodule)
```

### Adding an effect

1. Add its parameters in `src/Parameters.h/.cpp`.
2. Add the DSP (e.g. in `src/dsp/Effects.h`). Update it in `updateDspParameters()` and `run()` it in
   `processBlock()`.
3. Add an entry to `ui/modules.js`. The card, sidebar item and parameter bindings are generated from it.

## Using the processed mic in other apps (Zoom, Discord, OBS)

Install a virtual audio device such as [BlackHole](https://github.com/ExistentialAudio/BlackHole) (`brew install blackhole-2ch`).
Set MicFX's output to BlackHole, then pick BlackHole as the microphone in the other app.
To hear yourself too, create a Multi-Output Device in *Audio MIDI Setup*.

## License

MicFX is released under the [GNU AGPL v3](LICENSE). It is built on [JUCE](https://juce.com), whose free
license is also AGPLv3. If you want to ship a closed-source build, you need a commercial JUCE license.
