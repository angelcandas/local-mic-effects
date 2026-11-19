#!/usr/bin/env bash
# Measures real multitracks for learning Auto Mix's targets, and scores today's Auto Mix on
# them (tools/README-mixbench.md, "Learn"). Run from anywhere:
#
#   tools/learn-mix.sh <songs folder> [output folder]
#
# <songs folder> holds one folder per song (tracks/*.wav, mix.*, song.txt), as laid out by
# tools/fetch-multitracks.sh. The output (default ~/Desktop/learn-out) holds no audio:
#   learn/<song>.mlf     each song's measurements (band energy per 100 ms of every track and the mix)
#   bench/results.csv    Auto Mix's scores against the human mixes, and bench/summary.md
#   bench/score.md       the levels compared with measures you can trust (when numpy and scipy are there)
# and learn-out.zip next to it, the file to upload.
#
# Private testing only: the audio stays on this computer.
set -euo pipefail

if [[ $# -lt 1 || ! -d "$1" ]]; then
    echo "Usage: tools/learn-mix.sh <songs folder> [output folder]"
    exit 1
fi

repo="$(cd "$(dirname "$0")/.." && pwd)"
songs="$(cd "$1" && pwd)"
# Given the folder that holds multitracks/ (and maybe old bench results), use multitracks/.
[[ -d "$songs/multitracks" ]] && songs="$songs/multitracks"
out="${2:-$HOME/Desktop/learn-out}"
mkdir -p "$out"
out="$(cd "$out" && pwd)"

cd "$repo"
echo "== Updating libraries"
git submodule update --init > /dev/null

echo "== Building the bench (a few minutes the first time)"
generator=()
command -v ninja > /dev/null && generator=(-G Ninja)
arch_flags=()
[[ "$(uname)" == "Darwin" ]] && arch_flags=(-DCMAKE_OSX_ARCHITECTURES="$(uname -m)")
cmake -S . -B build/learn ${generator[@]+"${generator[@]}"} -DCMAKE_BUILD_TYPE=Release ${arch_flags[@]+"${arch_flags[@]}"} > build-learn-configure.log 2>&1 \
    || { tail -30 build-learn-configure.log; exit 1; }
cmake --build build/learn --target MicFXMixBench --parallel > build-learn.log 2>&1 \
    || { grep -m 20 "error" build-learn.log || tail -30 build-learn.log; exit 1; }
rm -f build-learn-configure.log build-learn.log
bench="build/learn/MicFXMixBench_artefacts/Release/MicFXMixBench"

echo "== Listening to the songs in $songs"
"$bench" "$songs" --listen on --no-audio --out "$out/bench" --learn "$out/learn" | tee "$out/bench-log.txt"

echo "== Scoring against the human mixes (needs python3 with numpy and scipy)"
if python3 -c "import numpy, scipy" > /dev/null 2>&1; then
    python3 tools/mixlearn/learn.py "$out/learn" --out "$out/learned.json" --fits "$out/fits.json" > "$out/learn-log.txt" 2>&1 \
        && python3 tools/mixlearn/score.py "$out/bench/results.csv" "$out/fits.json" --out "$out/bench/score.md" \
        || echo "  scoring failed, see $out/learn-log.txt (the zip is scored in the project either way)"
else
    echo "  skipped: 'pip3 install numpy scipy' to score here (the zip is scored in the project either way)"
fi

echo "== Packing"
rm -f "$out/../learn-out.zip"
(cd "$out/.." && zip -qr learn-out.zip "$(basename "$out")" -x '*.wav' '*.mp3' '*.flac')
echo
echo "Done. Upload this file to the project: $(cd "$out/.." && pwd)/learn-out.zip"
