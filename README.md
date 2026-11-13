# MicFX: local mic effects

Real-time voice effects for your microphone. Everything runs locally on your machine with a few
milliseconds of latency. Built for macOS (Windows planned) in C++ with [JUCE 9](https://juce.com); the UI
is HTML/CSS/JS running in a native WebView.

Builds as a **standalone app**, plus **AU** and **VST3** plugins from the same code.

## What's inside

**26 effects**, processed in this order (each can be switched on/off):

| Category | Effects |
| --- | --- |
| Clean-up | High-pass · **Denoise** · Noise Gate · EQ · **Parametric EQ** · Compressor · De-esser |
| Pitch | **Autotune** · Pitch Shift (with formant) · Harmonizer (up to 4 voices, follows the song's chords) |
| Character | Exciter · Saturation · Radio · Bitcrusher · Robot · Filter · Auto-wah |
| Modulation | Tremolo · Vibrato · Chorus · Flanger · Phaser |
| Space | Doubler · Echo (ping-pong) · Reverb · Stereo Width |

```
mic → input gain → high-pass → denoise → gate → autotune/pitch/harmonizer → EQ → parametric EQ → compressor → de-esser
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

### Harmonizer

Up to four extra voices, picked in one of three ways (**Mode**):

- **Intervals**: a fixed number of semitones from your voice (+4 = a major third).
- **Scale**: the interval as steps of the Autotune key and scale, so a "third" is major or minor as
  the key needs.
- **Chords**: notes of the chord that is playing now. **Style** puts the voices on the next chord
  notes above you, below you, alternating, or the chord's bass note under you with the others above.

Where the chords come from (**Chords from**, Live mode):

- **Backing track**: when you load a backing track, MicFX works out its chords (the same chroma
  analysis as key detection, plus a best path that doesn't let drum hits split a chord). The voices
  then change exactly when the song does.
- **Instrument input**: a guitar or keyboard on input 2 of the audio interface, followed as you play
  (about 0.1 to 0.3 s to settle on a new chord).
- **MIDI keys**: the chord you hold on a MIDI keyboard (one key alone plays the key's triad on it), kept
  after you let go. Held keys also win over the backing track or input 2 while they're down.

In DAW mode a harmonizer insert listens to the tracks without pitch inserts (read slightly ahead) and
to the MIDI being played; held MIDI keys win while they're down.

**Humanize** gives each voice its own small delay and pitch drift so they sound like separate singers,
**Spread** places them across the stereo field, and **Formant** makes the voices sound bigger or smaller
than you.

### Denoise

A spectral noise reducer, in the spirit of iZotope's Voice De-noise. Unlike the gate, which only
mutes the gaps between words, it removes steady noise (hiss, hum, fans, air conditioning) from every
frequency band separately, including while you sing or speak. It's in Live mode (after the high-pass)
and in Add effect on any DAW track.

- The signal is split into ~250 frequency bins (short-time FFT, 75% overlap).
- **Adaptive** (on by default): the noise in each bin is tracked statistically. Bins that sit well above
  their recent minimum are treated as voice and leave the estimate alone (MCRA), so it follows a changing
  room without learning your voice, or a long held note, as noise. With nothing learnt yet, the first half
  second sets a starting profile from the quietest moments of every band.
- **Learn noise**: switch it on during a few seconds of room noise only (no voice), then off. That profile
  is kept; with Adaptive on it's the starting point, with Adaptive off it's used as is. Switching the
  effect off and on, or changing a DAW track's other inserts, keeps it.
- **Reduction** is how far noise is turned down at most (8-12 dB is natural, 25+ dB is aggressive).
  **Threshold** above 0 dB treats more of the signal as noise, below 0 dB less.
- The card shows the noise profile per band and, from the top, how much each band is turned down.
- Added latency is one FFT frame: 10.7 ms at 48 kHz (11.6 ms at 44.1 kHz), only while it's on. MicFX
  reports it to the host, and DAW mode lines the other tracks up with it.

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

### Looper (Live mode)

**Loop** in the top bar opens the looper above the transport. It stacks up to four layers of your
processed voice (recorded after the effects, before the safety limiter) and plays them under what
you sing.

1. Press **Record loop** (or **L**) and sing. Press it again to close the loop: it plays back at once.
2. Press **Overdub** (L) to sing another layer on top, and **End overdub** (L) to stop. An overdub
   starts wherever the loop is, and every pass you leave it on adds to that layer.
3. **↶ / ↷** (U / Shift+U) undo and redo the top layer; undo while overdubbing drops that layer.
   **■** (K) stops the loop and **▶** plays it from the top. **Clear** needs a second click.

- **Free** loops are exactly as long as you recorded (a quarter second to 60 s). **Bars** rounds the
  first layer to whole bars at the tempo: type it or press **Tap** on the beat a few times. Closing a
  little early keeps recording to the bar line; a little late trims the extra and plays on in time.
- Each layer has a waveform, **M** (mute) and a level slider; **Loop** sets the volume of all of them.
  Double-click a slider for 100%.
- **Save WAV** writes each layer (one pass, 24-bit stereo) to the recordings folder. **Send to DAW**
  (in the app) also puts each layer on its own DAW track, "Loop 1" and on, repeated from bar 1 as many
  times as the **×** menu says, with each layer's level and mute. In Bars mode the project takes the
  loop's tempo and time signature. One undo in the DAW takes it all back.
- The loop is part of what **Record** captures, so a take holds your voice and your loops.
- Tempo, Free/Bars and loop volume are remembered; the loop itself isn't saved when you quit.

### Practice any song (Live mode)

**Practice** in the top bar swaps the effect rack for a practice lane. **Open song…** takes any song
(WAV, AIFF, FLAC, Ogg, MP3): MicFX splits it into stems (the same Demucs model as the DAW's Split into
stems), keeps the voice as a quiet **guide** and finds its melody, note by note, grouped into phrases at
the breaths. The rest of the song becomes the backing. Both go on the Live tracks, so the transport at
the bottom plays them. **Voice only…** takes a recording of a voice on its own (an a cappella or a guide
vocal) without splitting.

- The target notes scroll past the playhead; your pitch (the mic before any effect) draws over them,
  green within 25 cents, amber within 60. Sing in any octave: a low voice can sing a high song.
- Each note is scored as it ends (pitch, coming in on time, holding steady) and turns green, amber or
  red; each phrase gets a score in the strip under the lane, and the pass gets one at the top right.
  Hover a phrase for whether you leaned sharp or flat and came in early or late.
- **Guide voice** sets how loud the original singer plays under you (all the way down mutes it).
  Click a phrase (in the strip or the lane) to go back and sing it again; **From the top** restarts.
- A song is prepared once: the stems and the melody are kept in a cache named after the file's contents
  (Application Support/MicFX/Practice), so opening it again is instant.
- Timing is measured against what you hear: the audio interface's latency is taken off.
- When the song stops after some singing, the **report** opens (or press **Report**): the pass score,
  pitch, timing and steadiness, whether you leaned sharp or flat, a heat map of every phrase over the
  song, and up to three weak phrases worth another go.
- **Loop it** plays a weak phrase over and over with a short lead-in, scoring each time round. Pick a
  **speed** (down to 50%) and a **key** (up to 5 semitones either way): the backing and the guide are
  rendered with the warp engine (the guide keeps its character) and the notes move with them.
  **Next weak phrase** moves on, **Back to the song** returns.

### Scenes (Live mode)

The bar above the effects keeps **scenes**: the sound of each part of a song (verse, chorus,
bridge), up to eight. A scene holds every setting except the input gain.

- **+ Scene** saves the current sound. Click a scene (or **[** / **]**) to go there: knobs glide over
  the **Glide** time, effects that come on switch on at the start, effects that go off switch off at
  the end, and choices change at once.
- When you change the sound, the active scene shows a dot: **Save to …** puts the changes into it.
  Double-click a scene to rename it; right-click to save into it or delete it.
- Scenes are saved with the plug-in state (the DAW session, or the app's last state). With MIDI learn,
  a pedal or pad can step through scenes or jump to one.

### Setlists (standalone app)

**Setlist** in the top bar opens the show's songs, in order. Each song keeps its sound (every
setting but the input gain), its scenes, the looper's tempo, a key, a backing track, lyrics and notes.

- Set up a song (sound, scenes, tempo, backing track), then **+ Add the current sound as a song**.
  **Save the sound to this song** updates it later. Name, key and mode, BPM, backing track, lyrics
  (type them or load a .txt / .lrc file) and notes are edited below the list.
- Click a song to load it: its sound changes at once, its scenes replace the scene bar, the looper
  takes its tempo, the autotune takes its key (and major or minor, when the scale is major or minor),
  and its backing track replaces the tracks in the player.
- **Page Down / Page Up** (what most page-turner pedals send) go to the next and previous song, with
  no wrap at the ends. With MIDI learn, a pedal can do the same.
- **Stage view** shows the song's name, key, tempo and notes and its lyrics in large type. Lyrics in
  .lrc form (`[01:02.50] words`) follow the backing track line by line.
- Setlists are saved as `.micfxset` files; the last few are under **Recent**.

### MIDI learn (Live mode)

Pedals, knobs, pads and keys on any MIDI controller can drive the Live screen. In the app every
MIDI input is open; in a DAW, send MIDI to the MicFX track (VST3; Logic doesn't pass MIDI to audio
effects).

1. Press **MIDI** in the top bar. Everything you can map is outlined.
2. Click a knob, switch, effect, choice or button, then move a knob, press a pedal or key, or send a
   program change. It's mapped (a source or control that had a mapping gets the new one instead).
3. Map more, or press **Done** (or Esc). Right-click a mapped control, or **×** in the list, to unmap it.

- A **knob or fader** (CC) moves a value. It picks up: it only takes over once it reaches the value
  on screen, so the sound never jumps. On a choice, its position picks the option.
- A **pedal, pad, key or program change** flips a switch, steps through a choice, or flips a knob
  between its two ends. A pedal acts when pressed, not when released.
- **Buttons**: the looper's (Loop, stop, undo, redo, clear), previous/next preset, the scenes (each
  one, and previous/next), the setlist's previous/next song, Mute and Record.
  Looper pedals act in the audio block they arrive in, so a footswitch closes a loop on time.
- Mappings belong to the app (`MIDI mappings.json` next to its settings), not to the presets, and
  work while the window is closed.

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
- Every track feeds the **master** bus, which has its own inserts, volume and balance. **Mixer**
  shows a channel strip per track, the return tracks, and the master pinned at the right edge.
- **Return tracks** hold shared effects. A new project starts with a **Reverb** (A) and a
  **Chorus** (B) return, fully wet. Each track has a **send** knob per return (post-fader, or
  **Pre** in the Effects panel), and an output picker that can play the whole track into a return
  instead of the master (a group bus). **+ Return…** adds more (Echo, Doubler, Flanger, Phaser or
  empty); returns have their own inserts, pan, mute and solo.
- **Mastering effects** (DAW only, under *Mastering* in Add effect and the library): **Mastering
  EQ** (low cut, shelves, one wide bell, air), **Multiband Compressor** (three bands with their
  own thresholds and live gain reduction), **Stereo Imager** (width with mono bass, correlation
  readout), **Limiter** (look-ahead brickwall: drive, ceiling, release) and a **Loudness Meter**
  (momentary, short-term and integrated LUFS against a target). Put them on the master.
- **Recording is always dry.** Arm one or more tracks and press **Rec** (or **R**). The take is the
  unprocessed mic; the track's inserts play on top, so you can change the effects afterwards.
  Optional click and count-in; the take is shifted by the audio round-trip latency.
- **Inputs and stereo tracks.** The app opens every input of your audio interface (up to 16).
  Each audio track picks what it records and monitors: one input (mono), a neighbouring pair, or
  any two inputs as left and right (Input at the top of the mixer strip, or the Input column in
  the Effects panel). Tracks on different inputs record at the same time, each into its own WAV
  (stereo for a stereo track). Every strip, the returns and the master have left/right meters;
  sends and returns are stereo. Live mode uses input 1.
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

**On the Mac's GPU (automatic).** On a Mac, the first split also downloads the network converted to
Core ML (about 70 MB, checked against its SHA-256) next to the CPU model, and runs it on the GPU.
The panel says "on the GPU"; if the download or the model fails, the CPU does the split and the
finished message says why. Nothing to install.

The converted model is a release asset (`stem-model-v1`) made by the **Core ML stem model**
workflow (Actions tab, Run workflow): it runs `scripts/export_htdemucs_coreml.py` on a GitHub Mac
and prints the zip's SHA-256, which goes into `GpuModel::sha256` in `src/stems/StemSplitter.h`.
To convert by hand instead, use Python 3.12 (PyTorch 2.7.0 has no build for newer):
`/opt/homebrew/bin/python3.12 -m venv ~/micfx-export && source ~/micfx-export/bin/activate`, then
`pip install torch==2.7.0 torchaudio==2.7.0 coremltools==9.0 einops "numpy<2.3" demucs==4.0.1` and
`python scripts/export_htdemucs_coreml.py`.

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

### Instruments

MIDI tracks play the MicFX instruments, each with a window of its own (presets, knobs, a scope and a
keyboard you can click): open it with the instrument's button on the track or in the piano roll.

- **Grand**: a modelled grand piano: hammer hardness and tone, two-string beating and two-stage
  decay, dampers and the sustain pedal, soundboard and room. Presets: Concert Grand, Bright Pop,
  Felt Piano, Upright, Honky-Tonk, Ballad Hall.
- **Nova**: an analog-style polysynth: two oscillators with unison, sub and noise, a resonant
  filter with its own envelope, LFO, glide and mono/legato, chorus, echo and room. Presets for pads,
  strings, basses, leads, brass and plucks.

- **Tines**: an electric piano: tine or reed, bark that grows with how hard you play, a bell
  partial, drive, stereo tremolo, chorus and room.
- **Drawbar**: a tonewheel organ: nine drawbars (pull down for more), single-trigger percussion,
  key click, scanner vibrato and chorus, an overdriven amp and a rotary speaker (the mod wheel
  switches it to fast).

- **Pluck**: plucked strings from a waveguide: nylon and steel guitar, harp and koto bodies, pick
  hardness and position, palm mute, chorus and room.
- **Mallets**: struck bars and bells by modal synthesis: marimba, vibraphone (with its motor's
  tremolo), glockenspiel, kalimba and tubular bells.
- **Beat**: a synthesized drum kit on the General MIDI drum notes (36 kick, 38 snare, 42/46 hats,
  toms, crash, ride, clap, cowbell…), with tuning and decay per drum, drive and room. Its keyboard
  shows which drum is on each key.

Projects that used the first built-in sounds (Keys, Organ, Soft Piano, Warm Pad, Strings, Synth
Bass, Lead) open on the matching instrument and preset.

Each one is also a VST3 (and AU on macOS) plugin for other DAWs, "MicFX Grand", "MicFX Nova", "MicFX Tines", "MicFX Drawbar", "MicFX Pluck", "MicFX Mallets" and "MicFX Beat":
`cmake --build build/debug --target MicFXInstruments`, then copy the `.vst3` (or `.component`)
from `build/debug/MicFX<Name>_artefacts/` into your plugin folder.

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
src/Looper.*            the Live looper: four layers, overdub, undo/redo, bar-locked length
src/MidiMap.*           MIDI learn: CC / note / program change -> parameters and actions
src/PitchTap.h          the singer's pitch before the effects, every 5 ms, for the Practice view
src/Practice.*          Practice view: song preparation (stems, melody, cache), live scoring
src/Scenes.*            Live scenes: snapshots of the sound, recalled with a glide
src/Setlist.*           the setlist: songs with sound, scenes, tempo, key, backing track, lyrics
src/daw/                DAW mode: Edit (the session as an undoable ValueTree), AudioPool (decoded
                        clip audio), DawEngine (tracks + inserts + master bus, lock-free graph swap,
                        dry recording, metronome), Project (.micfx folders, autosave, recovery),
                        Exporter (offline mixdown and stems to WAV)
src/analysis/           Auto Mix measurements: loudness (BS.1770 / EBU R128), 1/3-octave spectrum,
                        activity, how a backing track masks the voice, and offline per-track analysis
                        of a DAW session; MixAssistant turns them into EQ, compressor, de-esser and
                        volume moves (Auto Mix); Key finds the song's key from chroma and key
                        profiles; Coach turns a voice into target notes and phrases and scores
                        singing against them
src/PluginEditor.*      WebView host: parameter relays, presets, meters, embedded files
src/stems/              Split into stems: chunked, multi-threaded separation, model download and
                        check (StemSplitter), and the Demucs adapter (DemucsSeparator)
src/instruments/        the MicFX instruments (Grand, Nova, Tines, Drawbar, Pluck, Mallets, Beat): a Spec per instrument (parameters,
                        window layout, presets, sound engine), InstrumentProcessor (the built-in
                        instrument and the VST3/AU plugin), InstrumentEditor (its window)
src/DawBridge.*         the DAW screen's commands and events (session, status, export, stems)
src/StandaloneApp.cpp   standalone app window, with Options → Check for updates…
scripts/update.sh       update check / pull + rebuild, used by Check for updates…
src/dsp/                PitchDetector (YIN), PsolaShifter, PitchEngine (autotune/pitch/harmony),
                        Effects (EQ, de-esser, saturation, echo…), ParametricEq, Biquad,
                        FxModules (each effect + its values), FxChain (an ordered list of them,
                        with input/output gain), FxMeter (lock-free levels and spectra for the UI)
ui/modules.js           the effect catalog the UI is built from
ui/app.js, styles.css   the interface (Live mode)
ui/looper.js            the Live looper panel
ui/practice.js, .css    the Practice view (Live mode)
ui/midilearn.js         MIDI learn mode on the Live screen
ui/scenes.js            the Live scene bar
ui/setlist.js           the setlist drawer and stage view (standalone)
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
