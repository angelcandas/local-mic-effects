# MicFX changelog

What changed in MicFX, newest first, in plain language. The app shows it under
**Help → Changelog**, and after an update it opens **What's new** with the entries you
haven't seen yet. To add an entry for your change, see "Adding entries" in
`docs/changelog.md`.

## 2026-11-26

- **Practice labels on the lyrics line, and a practice track from a MIDI file.** The coach's cues for the line being sung (Grit, Piano, Prepare position...) show above the words of the karaoke line. In the DAW, File > New practice track from MIDI... turns the melody of a .mid file into the guide lane to sing along to while you record. (#236)
- **Labels, a melody editor and a count-in for Practice.** Right-click a note in Practice to write a cue on it (Grit, Prepare position, Slower, Piano, Forte...) from a shared label library you can edit. Edit notes lets you fix the melody when backing singers confuse it: move, resize, split, merge or delete notes, or take a whole phrase out. A count-in before the song, and the Library now shows each song's best and last pass. (#235)
- **Practice guide in the DAW.** Open in DAW from Practice now brings the song's melody along as a Guide lane under the ruler: the notes to sing scroll past as you record with a visual reference, not only the sound. Right-click it to nudge or remove it. (#234)
- **Review your takes.** In Practice, Review take shows what you sang over the song's notes: your pitch coloured flat, in tune or sharp along the whole song, a score and what was off for each phrase, and a Where to focus list to hear or loop the weakest phrases. (#233)

## 2026-11-25

- **Arrange with sections, and mix versions.** Drag a song section on the ruler to move it and its music on every track, or right-click it to duplicate or delete it: automation, tempo changes and chords go along. The new Versions button keeps your mix as a version so you can try another and switch between them to compare. (#231)
- **MIDI clock sync.** Keep drum machines, synths and other apps in time with MicFX, or MicFX in time with them. Settings → MIDI clock: send clock from the DAW song or Live mode's tempo and looper, or follow a clock coming in (it sets Live mode's echo and looper tempo, and plays and stops the DAW song). (#230)
- **Voice repair.** Three new DAW effects for recorded voices: Breath Tamer turns down the breaths between phrases, Mouth De-click rebuilds lip smacks and tongue clicks, and Room Echo Reduction takes out the ring of the room it was recorded in. The Podcast and Streamer chains now take out clicks and ease breaths. (#228)
- **Clip launcher.** A Launch tab with scenes and slots: launch loops and one-shots in time, record what you play into the song, and let the AI build and launch scenes. (#229)
- **Sidechain.** A compressor, noise gate or dynamic EQ can listen to another track: music that ducks under your voice, a bass that makes room for the kick, guitars that dip only where the voice sits. Pick the track under Listens to on the effect. (#227)
- **Vocoder.** Your voice plays a built-in synth: hold keys on a MIDI keyboard, or it follows the song's chords (else a drone on the key). Saw, pulse or ensemble sound, 16 to 32 bands, formant and a clear-words control. In Live and as a DAW insert. (#224)
- **MIDI effects and velocity lane.** Put an arpeggiator, a chord maker or a scale lock before a MIDI track's instrument: arpeggios on the beat, the song key's chords from single notes, wrong notes kept in key. The piano roll gets a Velocity lane to draw how hard each note plays. (#225)
- **Feedback Guard.** A Live effect that catches the mic ringing through the speakers and notches that note before it howls, while your singing goes through untouched. Ring out before the show to fix the notches for the room. (#223)

## 2026-11-24

- **Align to lead.** Line a double, a harmony or a re-sung line up with the lead vocal, syllable by syllable, and optionally tune its notes to the lead's: the clip editor's Align section, or Align to lead… on a track. (#222)
- **Chord track.** A lane under the ruler with the song's chords: detect them from the music or type them. Harmonies follow it, the piano roll shows each chord's notes, and Practice shows the chord playing now. (#221)

## 2026-11-19

- **Auto Mix balances the band.** Drums, bass, guitar, piano and synth tracks you haven't set a fader on are brought to the balance engineers used in real mixes, with the band as loud as before. (#218)
- **Auto Mix: split lead vocals sit right.** When the lead is spread over several tracks (verses on one, choruses on another), each part now sits at the right level over the band while it sings. (#217)

## 2026-11-18

- **Auto Mix learns from real mixes.** The voice sits where engineers put it over the band, instruments get a gentle tone nudge toward finished mixes, and track names like BassAmpMic or ElecGtr_Vox get the right role. (#216)
- **Lyrics in Practice.** Paste a song's lyrics or import an .lrc file: each line is timed to the singing, lights up word by word as you sing, follows speed changes, and any line can be nudged and is saved with the song. (#215)
- **Live key and speed in Practice.** Changing the song's speed or key in Practice is heard within a moment, right where the song is, while the rest of the song is prepared behind it; settings you've used before load instantly. (#211)
- **Auto Mix starts fresh.** Closing Auto Mix forgets the last run, so it opens with the default style and options every time. Changes you tried and didn't keep yet are still shown so you can keep or discard them. (#212)
- **Practice upgrades.** Exit Practice with a button or Esc, reopen saved songs from a Library without splitting again, and change the speed (50-150%) and key (up to 12 semitones plus cents) of the whole song independently. (#208)
- **Computer audio in Live.** Mix sound from your computer (a backing track from a browser, a video call) with your voice, hear it and record it. (#194)
- **Pick your mic input.** In Live, choose which interface input is the microphone. (#195)
- **MIDI instruments in Live.** Play a keyboard or a drum kit straight from your MIDI controller. (#197)
- **MIDI files.** Import and export MIDI files in the DAW. (#196)
- **MIDI controllers.** Record, play back and edit CC, pitch bend and pressure, with a controller lane in the piano roll and controllers kept in MIDI files. (#199)
- **Starter content.** Two synthesized drum kits, the Kit comes loaded, and starter patterns in the library. (#200)
- **Freeze track.** Render a track to audio in place to save CPU, and go back with one click. (#201)
- **Project templates.** Start from a vocal, podcast, singer-songwriter, band or beat template, and save your own. (#202)
- **Clip volume envelope.** Draw volume points inside a clip with the Envelope tool. (#203)
- **Key commands.** A dialog to search, rebind and reset keyboard shortcuts; they are kept between launches. (#204)
- **Sampler.** A new sampler instrument (keys, ADSR, filter, loop, slices), plus Slice to MIDI from an audio clip. (#205)
- **MP3 export.** Export to MP3. (#192, #193)
- **Auto Mix dialogs.** Auto Mix asks "who is who" when it opens and confirms mode switches in a dialog. (#198)
- **Changelog and What's new.** This list, in the app. (#206)

## 2026-11-17

- **Auto Mix by section.** The voice follows each song section and the band lifts in the choruses. (#187)
- **Cue mix.** Hear your voice in the headphones with no wait for the effects. (#191)
- **Monitoring delay.** The audio status shows how much delay your effects add to what you hear. (#189)
- **Recording fixes.** Armed tracks monitor in Auto, Stop ends the take, and the playhead stays put while recording. (#188)

## 2026-11-16

- **Auto Mix audition list.** Tick moves to hear them live, five sliders, Front/Normal/Back priority, and the plan is saved with the project. (#181)
- **Auto Mix try, keep or discard.** It can leave the master alone, and it now listens to its own result and corrects the levels. (#185, #186)
- **Auto Mix test bench.** Mix real multitracks and score them against the human mix (for development). (#183)
- **AI control.** A local server lets AI programs on your computer drive MicFX, with named actions and composition actions that write a part fitting your song. (#179, #180, #182)
- **Backing vocals as normal tracks.** Optionally one track per voice. (#176)
- **Mastering templates.** Section moves, and automation lanes on the master and returns. (#178)
- **Clip editor in its own window.** Play from the Notes ruler, box select with any tool, pitch sliders. Take lanes list the newest first. (#177, #184)

## 2026-11-14

- **Automation.** Lanes under each track for volume, pan, sends, effect values and macros; record touch/latch passes; automation follows clips; any plug-in control can be automated. (#172, #174, #175)
- **Macros and racks.** Eight macro knobs per chain and parallel effect chains in the DAW and in Live. (#168, #169)
- **Vocal effects.** Opto compressor, compressor styles, gate/expander, smart de-esser, plate reverb, echo tempo sync and duck, saturation types, doubler modes. (#150, #154)
- **16 factory vocal sounds.** With returns, macros, rack versions of the pop and rap leads, and a button to fit a chain to your voice. Auto Mix uses the new effects. (#163, #165, #170, #171, #173)
- **Preset library on disk.** Save your own Live sounds and DAW chains, with defaults for new effects and tracks. (#159, #162)
- **Effect drag and drop.** Drop into a chain slot or onto a card, drag cards to other tracks or the library, switch slots in the mixer strips. (#164, #166)
- **Live presets.** Presets under each effect in the library, and search. Live uses the same device cards as the DAW. (#153, #167)
- **Warp.** Tempo definition per clip, stretch by the edge, make warp permanent, a Hitpoints tool and a Tempo tool in the clip editor, and an arrangement Warp tool. (#152, #160, #161)
- **Slice a loop to the drum rack.** The clip menu cuts a loop at its hits onto a new Kit's pads. (#158)

## 2026-11-13

- **Practice.** Sing along to any song with live scoring, a pass report, a heat map, and loop weak phrases slower or in another key. Loop a song section. (#97, #100, #137)
- **Live looper.** Four layers, overdub, undo/redo, bar-locked length and Send to DAW. (#93)
- **MIDI in Live.** MIDI input and MIDI learn for parameters, presets, mute, record and looper. (#95)
- **Scenes and setlists.** Scenes with glides and setlists with a stage view. (#96)
- **Sound check.** Sets the chain for your voice, mic and room. (#109)
- **Auto level.** Keeps the voice steady, or a few LU over the backing. (#111)
- **Capture.** Keep the last minutes of the voice and armed inputs and save them as a take. (#116)
- **Harmonizer.** Up to four voices that follow the song's chords. (#94)
- **Live safety.** Undo in Live, instant mode switch, reorder the chain, sing-along kept. (#147, #149)
- **Pattern looper.** Patterns, steps, pattern blocks, a step rack, a Draw tool, velocity bars, loop recording, drop library samples, rack tools, block editing, and converting blocks to and from MIDI. (#98, #102, #105, #113, #115, #133, #155)
- **Kit.** A sampler drum kit with sixteen pads, its own window and VST3/AU build. (#103)
- **Audio to MIDI.** Turn sung lines and drum loops into notes from the clip menu. (#114, #117)
- **Mix Check.** A report of findings with Fix buttons. (#119, #123)
- **Auto Mix.** Adds Smooth where a track rings, places tracks left and right, lines up mics that recorded the same sound, and a backing track (karaoke) role. (#118, #120, #121, #141)
- **Backing vocals.** Build harmony parts and doubles from the clip menu. (#124, #125)
- **Song map.** Beats, bars, sections and tempo from the performance, shown on the ruler with Follow the performance. (#126, #130)
- **More effects.** Smooth resonance suppressor, speaker cabinets and IR reverb, and a built-in guitar and bass amp. (#112, #127, #128)
- **Effect plug-ins in the DAW.** Use VST3/AU effects as track inserts, with their parameters on the card. (#129, #132)
- **More stems.** 6-stem and Best quality splitting, and Rebalance. (#122)
- **Export formats.** WAV, AIFF, FLAC and OGG with quality, sample rate, file name and folder. (#140)
- **Loops in tempo.** Dropped and imported loops play at the project tempo from the bar. (#136)
- **Clip editor.** Laid out like a sample editor with a Warp tool and time compare; live waveform stretch and hearing notes while editing; warp several clips at once; hitpoints stored per clip. (#106, #108, #134, #144, #148)
- **Cleaner library and mixer.** Returns and Master scroll with the tracks. Library favourites, recent, search everything, effect presets and file preview. (#85, #131)
- **Setup and help.** Welcome card, Settings window and audio status strip, Help menu, F1 manual, hint line, Assist menu. (#146, #151)
- **Editing.** Range tool, snapping, track drag, crossfades, punch and bounce; Locate missing files, DAW MIDI learn, undo history, markers. (#156, #157)
- **Words and keys.** Consistent names, Space always plays, presets on Ctrl+arrows, M mutes in both modes. (#139)
- **Effect cards.** One card for the DAW with title bar, menu, keys, A/B and rename; effect chain edits are seamless. (#135, #145)
- **Faster warp.** Edits render only the pieces they change. (#99)
- **Stems on the GPU, safely.** The model is checked against the CPU result first and its checksum is pinned. (#104, #107, #110)
- **Fixes.** Crash on Detect key; recording over a track moves clips into Take 1. (#142, #143)

## 2026-11-12

- **Auto Mix grows up.** Track roles, more styles, normalize, more effects, stems split, volume rides, faster listening, modes (fit into the backing, clean & polish, Speech/podcast, Master), MIDI tracks, shared reverb through sends, echo throws, and compressor settings by role (attack, release, makeup). Closing the panel ends the run. (#48, #50, #55, #57, #59, #60, #62, #63, #65, #66, #80)
- **Elastic audio.** Stretch algorithms per clip, formant-preserving transpose, a tempo track with tempo changes and ramps, musical mode, hitpoints, quantize and linked tracks. (#56, #61, #64, #67)
- **Transient shaper and dynamic EQ.** And Auto Mix uses them. (#69)
- **Denoise.** Spectral noise reduction in Live and the DAW. (#73)
- **Stem splitting on the GPU.** Core ML on macOS, with time left shown. (#42, #49, #54)
- **Returns and mastering.** Return tracks with sends, master balance and mastering effects. (#53)
- **Take lanes.** Comping, loop recording and Auto Comp. (#78)
- **Interface inputs.** Per-track inputs, stereo tracks and stereo meters. (#74)
- **Audio device favourites.** An ordered list, fallback on unplug, restores rate and buffer. (#71)
- **Instruments.** Grand piano, Nova synth, Tines electric piano, Drawbar organ, Pluck strings, Mallets and Beat drum kit, each in its own window and as VST3/AU plug-ins. (#83, #86, #90)
- **Group tracks.** Foldable header, one fader, effects and sends. (#87)
- **Clip editor and note pitch editor.** Edit pitch and warping, and per-note pitch edits on sung notes. (#89, #91, #92)
- **DAW layout.** Toolbar in zones, named undo, running jobs, track header sections, right-click menus, tooltips, a hint line, and the mixer as a bottom tab with multi-select. (#77, #81, #82, #84)
- **Effect cards.** Drag cards to reorder the chain, right-click menu on tracks. (#47)
- **AI tools views.** Plugin-style views for Auto Mix and the Stem splitter. (#79)
- **Smoother DAW.** Track rows update in place and waveform redraws are debounced while zooming. (#43, #76)
- **Floating piano.** A keyboard piano window. (#46)

## 2026-11-11

- **Auto Mix.** Seven-band parametric EQ, an analysis engine (loudness, spectrum, activity, masking), a mix assistant and a panel, plus reference matching. (#15, #16, #17, #18, #35)
- **DAW timeline.** Markers, time signature, snap grid, tap tempo, click volume, go-to clock. (#19)
- **Clips.** Mute, rename, normalise, strip silence, anti-click edges, loop clip, zoom to fit; time warp and transpose; quantize audio; tempo from a leading track; real waveform detail when zooming. (#23, #27, #29, #38)
- **Tracks and mixer.** Duplicate track, copy effects, exclusive solo, clip lights, recent projects, mic meter. (#25)
- **Live mode for the stage.** Panic mute, hold for dry, mic check, preset search and favourites, stage keys, echo tap, reset an effect. (#26)
- **Library sidebar.** Audio files, effects, instruments and presets. (#30)
- **Effects.** Live effect animations, input/output gain and presets on every effect, smoother knobs. (#22, #32, #36)
- **Song key.** Detect it, autotune to the key, and Live "Match song". (#34)
- **MIDI.** MIDI tracks, instrument plug-ins and a piano roll; play MIDI from the computer keyboard. (#33, #44)
- **Stems.** Split a song into vocals, drums, bass and other. (#31)
- **Shortcuts and toolbar.** Shortcuts from Ableton, Cubase and Logic Pro; compact icon toolbar; Track tab and Effects tab. (#39, #40, #41)
- **Top bar.** Live/DAW title, project name, confirm mode switch. (#45)
- **Latency reporting.** Autotune and pitch-shift delay is reported to hosts and the DAW. (#20)
- **Compact clip panel.** One vertical card. (#28)
- **Fixes.** DAW projects after Open, Add track and Import. (#24)

## 2026-11-10

- **DAW mode.** Tracks with inserts and a mixer and master bus, recording with monitoring, metronome and count-in, project save/open/autosave with crash recovery, offline mixdown and stem export, and clip editing (move, trim, split, fades, snap, loop). (#5, #6, #7, #8, #9, #10, #11, #12)
- **Effect pictures.** EQ and filter curves, dynamics, echo, reverb and LFOs. (#13)
- **Effects panel and menu bar.** Effects under the tracks, full screen and an app menu bar. (#14)
- **Update check.** "Check for updates…" in the standalone app. (#4)

## 2026-11-06

- **Recording.** Save the processed output (and optionally the dry mic) to WAV. (#2)
- **Multi-track player.** Load a backing track, sing along and record takes on their own rows. (#3)

## 2026-09-23

- **MicFX.** Real-time local mic effects. (#1)
