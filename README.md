# MicFX: local mic effects

Real-time voice effects for your microphone. Everything runs locally on your machine with a few
milliseconds of latency. Built for macOS (Windows planned) in C++ with [JUCE 9](https://juce.com); the UI
is HTML/CSS/JS running in a native WebView.

Builds as a **standalone app**, plus **AU** and **VST3** plugins from the same code.

## What's inside

**25 effects**, processed in this order (each can be switched on/off):

| Category | Effects |
| --- | --- |
| Clean-up | High-pass · Noise Gate · EQ · **Parametric EQ** · Compressor · De-esser |
| Pitch | **Autotune** · Pitch Shift (with formant) · Harmonizer (2 voices) |
| Character | Exciter · Saturation · Radio · Bitcrusher · Robot · Filter · Auto-wah |
| Modulation | Tremolo · Vibrato · Chorus · Flanger · Phaser |
| Space | Doubler · Echo (ping-pong) · Reverb · Stereo Width |

```
mic → input gain → high-pass → gate → autotune/pitch/harmonizer → EQ → parametric EQ → compressor → de-esser
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
- Added latency is roughly half a pitch period (~1–5 ms). While Autotune or Pitch Shift is on, MicFX
  reports about 2.6 ms (the delay for a typical 200 Hz voice) to the host, so DAWs line the track up with
  the others. DAW mode delays the other tracks by the same amount, and recorded takes are placed to match.

The Pitch Shift and Harmonizer use the same engine, so they're tuned too when Autotune is on.
Chromatic works for any song. Pick the song's key and scale for a tighter, more musical tune.

### Parametric EQ

A precise seven-band EQ for mixing, next to the simple three-knob EQ:

- **Low cut** and **high cut** (24 dB/octave, each with its own on/off), a **low shelf**, three **bells**
  and a **high shelf**, ±18 dB.
- Drag the points on its curve to set frequency and gain; scroll on a bell to make it wider or narrower
  (Q 0.1–18). Every band also has knobs.
- It's the EQ the coming Auto Mix uses for its moves, so its response can be computed exactly
  (`fx::ParametricEq::getMagnitudeDb`).

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

### DAW mode (standalone app)

Switch **Live / DAW** at the top of the window. DAW mode is a multitrack recorder built around
tracks instead of one live chain:

- **Tracks** hold clips on a timeline with a bar ruler. Each track has its own **effect inserts**
  (any of the Live effects, in any order, or a preset loaded into the track), volume, pan,
  **M**ute, **S**olo, **●** arm and **In** (hear the mic through the track's effects while you play).
- Each effects chain (every track's, and the master's) has its own **In** and **Out** gain with
  meters, either side of its inserts. In both modes, effect cards move with your voice: the
  spectrum behind the EQ and filter curves, a dot riding the compressor and gate curves with the
  gain reduction (or gate open/closed), echo taps replaying what you sang, and in/out level bars.
- Every effect also has its own **In** and **Out** gain and a **Presets** menu (factory settings, plus
  your own saved with *Save current as…*, shared between Live and DAW).
- Every track feeds the **master** bus, which has its own inserts and volume. **Mixer** shows a
  channel strip per track plus the master.
- **Recording is always dry.** Arm one or more tracks and press **Rec** (or **R**). The take is the
  unprocessed mic; the track's inserts play on top, so you can change the effects afterwards.
  Optional click and count-in; the take is shifted by the audio round-trip latency.
- **Editing**: drag clips to move them (also to another track), drag the edges to trim, the top
  corners to fade, **Ctrl+E** splits at the playhead, **Ctrl+D** duplicates, **Delete** removes. Edits
  snap to the beat at the project tempo (hold **Alt** to drag freely). Drag on the ruler to mark a
  loop, **L** toggles it. **Ctrl+Z / Ctrl+Shift+Z** undo and redo every edit.
- **Projects** are folders (`My Song.micfx/`) with `project.xml`, the audio, autosave backups and
  exports, so they move and back up as one thing. If the app quits unexpectedly you're offered the
  newest autosave the next time.
- **Export** renders the mix, or one stem per track, to 16/24/32-bit WAV faster than real time,
  optionally normalised or limited to the loop range.

- **Tools**: Select (V), Cut (B: click a clip to split it there) and Gain (G: drag a clip up or down).
  Ctrl+X / C / V cut, copy and paste clips at the playhead.
- **Navigation**: the ↔ and ↕ sliders (or Ctrl+scroll and Alt+scroll) zoom time and track height.
  **Follow** keeps the playhead in view while playing; scrolling yourself switches it off so you can
  look and edit elsewhere while the song plays. Clicking a lane while playing only selects.
- Each track header has volume and pan; armed tracks show the take growing while you record.
- The **bottom panel** sits under the tracks (and the mixer), with two tabs. **Effects** shows the
  selected track's effects left to right, in chain order. **Track** shows the selected clip (gain,
  fades, split, loop, strip silence) and **Pitch & time** (transpose, quantize, tempo), whose
  sections fold away to their titles. Drag the bar above it to resize, or press **E** (or its
  **Hide** button) to fold it away.
- The **library** on the left lists audio files (this project, Recordings, Music, Downloads,
  Desktop and any folder you add with **+ Add folder…**), the effects, instruments (with MIDI
  tracks) and presets. Drag a file onto a track to put a clip where you drop it, or below the
  tracks for a new track; drag an effect or preset onto a track, the master, a mixer strip or the
  effects panel. Double-click (or Enter) adds to the selected track. **Shift+B** folds it to a thin
  strip, **Ctrl+F** searches it, and its right edge drags to resize.
- **MIDI tracks**: **+ MIDI** (Ctrl+Shift+T) adds one with a built-in synth (Keys, Soft Piano,
  Warm Pad, Strings, Synth Bass, Lead, Organ); drop an instrument from the library on a track, or
  below the tracks, to use another. VST3 instruments (and AU on macOS) are found by a scan of the
  usual plug-in folders (**Rescan plug-ins** in the library); open-source ones such as Surge XT,
  Dexed, Vital or OB-Xd work, and so do commercial ones like Kontakt, which opens in its own window
  from the ♫ button on the track. Their settings are saved in the project.
  Arm a MIDI track to play it from your MIDI keyboard (every MIDI input is opened) and record: the
  take becomes a MIDI clip (the sustain pedal is kept). Double-click a MIDI track's lane for an
  empty clip, and select a MIDI clip to edit its notes in the **piano roll** on the bottom panel's **Track** tab:
  click to add, drag to move, drag the end to resize, double-click to delete, box-select, ↑↓ to
  transpose, Q to quantize to the grid. Exports play the instruments too.
- **Menus**: the standalone app has File (new, open, save, import, export, audio settings), Edit,
  View and Transport menus, in the macOS menu bar at the top of the screen or under the title bar
  on Windows and Linux. They do the same as the buttons and shortcuts.
- **Keyboard shortcuts** follow what Ableton Live, Cubase and Logic Pro have in common (Space,
  M mute, S solo, R record, Q quantize, Ctrl+N/O/S, Ctrl+Z, Ctrl+D, Ctrl+E split, Ctrl+T new
  track, Tab or X for the mixer, numeric keypad 0 / * for stop / record). Ctrl is Cmd on a Mac.
  Press **?** (or the ? button) for the full list.
- **Full screen**: the title bar's maximise button, the ⛶ button at the top, **View → Full screen**,
  **F11**, or **Ctrl+Cmd+F** on a Mac.

### Auto Mix (DAW mode)

**Auto Mix…** at the top of the DAW screen (or **Edit → Auto Mix…**) mixes your voice against the
backing track for you, locally, with the effects you already have:

1. Pick a **style**: Pop, Ballad, Rock, or Coaching (the voice further up front so you hear yourself).
2. Press **Listen**. It renders every track offline (faster than real time) and measures loudness
   (EBU R128), tone (1/3-octave bands), when each track plays, your lowest notes, and where the
   backing covers your words.
3. It guesses which track is the **voice** and which is the **backing** from their names and sound.
   Change a role and the plan updates.
4. It shows every change it wants to make, with the reason, e.g. "Cut 3.5 dB at 315 Hz: muddy":
   - **Voice**: a Parametric EQ (low cut under your lowest note, mud/boxiness and harshness cuts or a
     presence lift, boom and air shelves), a Compressor if your level swings a lot between phrases,
     and a De-esser if the S's stand out.
   - **Backing**: a small EQ dip where it covers your voice the most.
   - **Balance**: your volume so the voice sits the style's amount over the backing.
   - **Master**: towards −14 LUFS without clipping.
5. **Apply** writes it as one undo step. **Hear before / Hear after** switches between the two while
   the song plays, and **Amount** makes it gentler. Everything it changed is an ordinary insert or
   fader you can tweak.

**Match a reference…** picks a song you like and matches the master to it: a gentle master EQ
towards its tonal balance, and the master volume towards its loudness (held below clipping).

### Split into stems (DAW mode)

Select a song's clip and press **Split into stems** in the clip panel. MicFX separates it into
**Vocals**, **Drums**, **Bass** and **Other**, each on its own track right under the song and lined
up with the clip (same start, trim, gain and fades), and mutes the song's clip. One undo takes it back.

- It runs on your computer with [Demucs v4](https://github.com/facebookresearch/demucs) (the hybrid
  transformer model by Meta AI Research) through [demucs.cpp](https://github.com/sevagh/demucs.cpp).
  Nothing is uploaded.
- The first split downloads the model weights once (about 84 MB) to
  `~/Library/Application Support/MicFX/Models/`, and checks them against their SHA-256.
- The song is cut into 30-second chunks that up to four CPU cores separate at once, crossfaded back
  together. On a Mac the matrix maths goes through Apple's Accelerate (the M-series matrix units). The
  panel shows the time left, and the finished message says how long separating took. Cancel any time.
- The stems are 24-bit 44.1 kHz WAVs (`Song - Vocals.wav`, ...) in the project's `Audio` folder, or
  next to your recordings when no project is open.

**On the Mac's GPU (optional, one-time).** Convert the network once and MicFX runs it with Core ML on
the GPU, with the CPU as the fallback:

```
# needs Python 3.12 (brew install python@3.12): PyTorch 2.7.0 has no build for 3.13 or newer
/opt/homebrew/bin/python3.12 -m venv ~/micfx-export && source ~/micfx-export/bin/activate
pip install torch==2.7.0 torchaudio==2.7.0 coremltools==9.0 einops "numpy<2.3" demucs==4.0.1
python scripts/export_htdemucs_coreml.py
```

This writes `htdemucs-core.mlpackage` to the Models folder (it needs the Demucs weights from
Meta's server, so run it on a machine with internet access). The panel says "on the GPU" when it is
used; if the model is missing or fails to load, the finished message says why the CPU was used.

### Song key

**Key** next to the time signature (or **Edit → Song key…**) finds the key the song is in, locally:

1. Press **Detect**. It listens to every track offline, works out which notes sound (whatever the
   octave) and compares them with how major and minor keys use their notes. Voice tracks are left
   out, since your melody may be the thing that's off key; the harmony comes from the instruments.
2. It shows the key ("A minor"), how clear the match is (and the runner-up when it isn't), the
   notes the music uses with the key's scale lit up, and what each track on its own sounds like.
   A song tuned away from A = 440 Hz (e.g. a 432 Hz recording) still lands on the right key, and
   the panel tells you how far off it is.
3. The key is saved with the project and shown on the button ("Key Am"). Pick another by hand if
   you disagree.
4. **Set autotune to key** points every Autotune in the session at it in one undo step. A scale
   that already fits keeps (a minor pentatonic for a minor song); otherwise it becomes Major or
   Minor. A new Autotune insert starts in the song's key.

In **Live** mode, the Autotune card has **Match song**: it listens to the loaded backing track and
sets the autotune's key and scale to it.

Press **?** in DAW mode for every keyboard shortcut (transport, ← → to move the playhead, ↑ ↓ to pick
a track, M / Shift+S / A / I for mute, solo, arm and monitor, and more).

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

**Options → Check for updates…** in the standalone app (also in the MicFX menu on macOS, or
**File** on Windows and Linux) fetches the latest version, lists what's
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

GitHub Actions (`.github/workflows/ci.yml`) runs them on every pull request: a Linux build of the
tests, the standalone app and the VST3, plus a syntax check of the UI scripts. Pushes to `main` (and
**Run workflow** in the Actions tab) also build and test on macOS, including the AU.

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
src/PluginProcessor.*   audio thread: input gain, the effect chain, output gain, limiter
src/Recorder.*          WAV recording of the output (and dry mic) on a background thread
src/Multitrack.*        backing track + takes: loading, resampling, timeline playback
src/daw/                DAW mode: Edit (the session as an undoable ValueTree), AudioPool (decoded
                        clip audio), DawEngine (tracks + inserts + master bus, lock-free graph swap,
                        dry recording, metronome), Project (.micfx folders, autosave, recovery),
                        Exporter (offline mixdown and stems to WAV)
src/analysis/           Auto Mix measurements: loudness (BS.1770 / EBU R128), 1/3-octave spectrum,
                        activity, how a backing track masks the voice, and offline per-track analysis
                        of a DAW session; MixAssistant turns them into EQ, compressor, de-esser and
                        volume moves (Auto Mix); Key finds the song's key from chroma and key
                        profiles
src/PluginEditor.*      WebView host: parameter relays, presets, meters, embedded files
src/stems/              Split into stems: chunked, multi-threaded separation, model download and
                        check (StemSplitter), and the Demucs adapter (DemucsSeparator)
src/DawBridge.*         the DAW screen's commands and events (session, status, export, stems)
src/StandaloneApp.cpp   standalone app window, with Options → Check for updates…
scripts/update.sh       update check / pull + rebuild, used by Check for updates…
src/dsp/                PitchDetector (YIN), PsolaShifter, PitchEngine (autotune/pitch/harmony),
                        Effects (EQ, de-esser, saturation, echo…), ParametricEq, Biquad,
                        FxModules (each effect + its values), FxChain (an ordered list of them,
                        with input/output gain), FxMeter (lock-free levels and spectra for the UI)
ui/modules.js           the effect catalog the UI is built from
ui/app.js, styles.css   the interface (Live mode)
ui/daw.js, daw.css      the DAW screen: tracks, clips, inserts, mixer, transport
ui/browser.js           the DAW screen's library sidebar (files, effects, instruments, presets)
ui/widgets.js           helpers shared by both screens
ui/fxpresets.js         per-effect presets (factory + your own) and the card's preset menu
ui/fxviews.js           effect pictures (EQ/filter curves, dynamics, echo taps…) for both screens,
                        animated from each effect's live readings
tests/DspTests.cpp      headless DSP test runner
libs/JUCE               JUCE 9.0.2 (git submodule)
libs/demucs.cpp         Demucs v4 inference in C++, MIT (git submodule; optional: without it the
libs/eigen              app builds but can't split stems), on Eigen, MPL 2.0 (git submodule)
```

### Adding an effect

1. Add its parameters in `src/Parameters.h/.cpp`.
2. Add the DSP (e.g. in `src/dsp/Effects.h`), then wrap it as a module in `src/dsp/FxModules.h`: declare
   its values with the same IDs and defaults as the parameters (`addValue` for knobs, which glide over ~15 ms
   so they never click; `addChoice` for lists and on/off options, which step), add a `ModuleType`, and add it to
   `createModule()` and `getModuleKey()`. Live mode runs every type in `ModuleType` order; the tests check
   the module values and the parameters stay in sync.
3. Add an entry to `ui/modules.js`. The card, sidebar item and parameter bindings are generated from it.
   For DAW inserts, map its id to the module key in `MODULE_KEYS` in `ui/modules.js`.

## Using the processed mic in other apps (Zoom, Discord, OBS)

Install a virtual audio device such as [BlackHole](https://github.com/ExistentialAudio/BlackHole) (`brew install blackhole-2ch`).
Set MicFX's output to BlackHole, then pick BlackHole as the microphone in the other app.
To hear yourself too, create a Multi-Output Device in *Audio MIDI Setup*.

## License

MicFX is released under the [GNU AGPL v3](LICENSE). It is built on [JUCE](https://juce.com), whose free
license is also AGPLv3. If you want to ship a closed-source build, you need a commercial JUCE license.

Stem splitting uses [demucs.cpp](https://github.com/sevagh/demucs.cpp) (MIT), [Eigen](https://eigen.tuxfamily.org)
(MPL 2.0) and the Demucs v4 weights (MIT, downloaded at first use); all three are compatible with the AGPL.
