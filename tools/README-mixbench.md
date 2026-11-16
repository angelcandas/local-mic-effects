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
2. `tools/fetch-multitracks.sh <cache>` lays them out as song folders (add `--flac` to save space).

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
