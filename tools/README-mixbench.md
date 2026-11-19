# Auto Mix test bench

`MicFXMixBench` mixes real multitrack recordings with Auto Mix and scores each mix against the
mix a person made of the same tracks. It is how Auto Mix changes are checked on real songs
(`src/analysis/MixBench.h` has the details).

The audio is not in the repository and never goes in it: the songs come from free libraries
whose licence allows non-commercial use only. Private testing only, no training, no shipping.

## Get the songs

1. Open the Cambridge-MT library (https://www.cambridge-mt.com/ms3/mtk/) in a browser and
   download, for each song in `tools/multitracks.txt`, the full multitrack zip and the mix
   (mp3 or wav). Put them all in one folder, `<cache>/zips/`.
2. `tools/fetch-multitracks.sh <cache>` lays them out as song folders (add `--flac` to save space). Zips of songs that aren't in the list are laid out too (style pop; edit the song's `song.txt`). Works with macOS's own bash.

A song folder can also be made by hand:

```
My song/
  tracks/*.wav      the raw tracks, all starting at 0
  mix.wav           the human mix (wav, flac, mp3...)
  song.txt          optional: style = rock, tempo = 96, role.<track file name> = voice
```

## Run

```
cmake --build --preset release --target MicFXMixBench
build/release/MicFXMixBench_artefacts/Release/MicFXMixBench <cache> [--style song|all|pop|...] [--out folder]
```

It writes `results.csv` (one row per song and style), `summary.md`, and for listening each
rendered mix next to a copy of the human one. Roles are guessed from track names, then the sound,
like the app does; override a wrong guess in `song.txt`.

## Scores

| Column | Meaning |
|---|---|
| `lufs`, `lra` | integrated loudness, loudness range |
| `crest` | sample peak over the RMS of the active part, dB |
| `width`, `corr` | side over mid energy (dB), left/right correlation |
| `balance` | left minus right, dB |
| `tone_distance_db`, `tone_tilt_db_oct` | 1/3-octave tonal balance after loudness matching: mean difference, and slope (+ = ours brighter) |
| `vob_*` | lead voice over everything else while it sings, LU (`vob_ours_exact` from our tracks' own renders) |
| `lu_<role>_*` | each role's loudness against the mix while it plays, LU |
| `role_error_lu` | mean difference between ours and the human per role |
| `fit_*_db` | how well the per-track gain fit explains that mix (lower is more trustworthy) |

The per-role numbers of a mix, the human one included, come from fitting the mix's band energy
over time as a sum of the raw tracks' energies with one gain per track, so no stems are needed.
A mix with heavy processing fits less well; check `fit_human_db` before trusting its role numbers.

## Learn

Auto Mix's targets (levels per role, EQ, compression, de-essing, reverb, pan, loudness) are
learned from the human mixes of the bench's songs in two steps, so the audio never has to
leave the computer it is on:

1. `tools/learn-mix.sh <songs folder>` builds the bench, scores today's Auto Mix on every song
   (`bench/results.csv`, `bench/summary.md`) and writes each song's measurements
   (`learn/<song>.mlf`, see `src/analysis/MixLearn.h`): band energy per 100 ms of every raw
   track and of the human mix, per channel. No audio. It packs them as `learn-out.zip`.
   (`MicFXMixBench <songs> --learn <folder> --learn-only` writes the measurements alone.)
2. `python3 tools/mixlearn/learn.py <learn folder> --out learned.json` (numpy, scipy) explains
   each human mix as a sum of its raw tracks, each with a level, an EQ curve (a gain per
   octave), a compressor (gain falling by a slope per dB over the track's median loudness),
   an extra slope on 5-8 kHz for voices (de-essing), a reverb tail with a decay time, and
   left/right gains (pan); then pools the per-track numbers by role and checks them song by
   song with that song left out of the pooling (`held_out` in the output). Tracks are grouped
   into one stem per role (the busses an engineer would treat), roles are re-read from the
   track names (`role_from_name`, the same rules as Auto Mix's own guess), and the mix is
   lined up with the tracks hop by hop, so a human mix that was edited (a cut intro, a moved
   section) still fits. `--fits fits.json` keeps every song's numbers for the next step.
3. `python3 tools/mixlearn/export.py fits.json --header src/analysis/LearnedTargets.h` turns
   them into what Auto Mix ships with, after checking each on songs left out of the learning:
   a shift on every music style's voice-over-the-band target, and a gentle EQ per instrument
   role (half of the engineers' median move, capped at 3 dB, as a low shelf, a bell and a
   high shelf). Learned settings that didn't hold up on held-out songs (background vocals'
   level under the lead, for one) aren't exported.
4. `python3 tools/mixlearn/score.py bench/results.csv fits.json [--before old/results.csv]`
   scores Auto Mix's levels with measures that can be trusted: ours exactly, from each track
   as it plays in our mix (`vob_ours_exact`, `lu_<role>_ours_exact`), the human mixes by the
   learning fit. The bench's own voice-over-backing and role columns fit the raw tracks to
   each mix without lining them up hop by hop, and on big or edited multitracks they can be
   off by 10 dB or more. `tools/learn-mix.sh` runs steps 2 and 4 itself when numpy and scipy
   are installed (`bench/score.md`).
