#!/usr/bin/env bash
# Lays out the multitracks of tools/multitracks.txt as song folders for MicFXMixBench:
#   <cache>/<song>/tracks/*.wav, <cache>/<song>/mix.<ext>, <cache>/<song>/song.txt
#
#   tools/fetch-multitracks.sh [cache folder] [--flac]
#
# Each song comes from <cache>/zips/: a zip of the multitrack and the mix (mp3 or wav), matched
# by artist and title (letters and digits only, any case: "AlJames_SchoolboyFacination_Full.zip"
# and "AlJames_SchoolboyFacination_Mix.mp3" both match "Al James - Schoolboy Facination").
# When a URL is listed and the file isn't there yet, it's downloaded with curl first.
# --flac converts the tracks to FLAC (needs `flac`) to roughly halve the space.
#
# Private testing only: the audio is free for non-commercial use. Never commit it.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
cache="${1:-/mnt/project-files/ai-mixing-research/multitracks}"
flac=0
[[ "${2:-}" == "--flac" || "${1:-}" == "--flac" ]] && flac=1
[[ "$cache" == "--flac" ]] && cache="/mnt/project-files/ai-mixing-research/multitracks"
mkdir -p "$cache/zips"

lower() { echo "$1" | tr '[:upper:]' '[:lower:]'; }  # bash 3.2 (macOS) has no ${var,,}
key() { echo "$1" | tr -cd '[:alnum:]' | tr '[:upper:]' '[:lower:]'; }
trim() { local s="$1"; s="${s#"${s%%[![:space:]]*}"}"; echo "${s%"${s##*[![:space:]]}"}"; }

missing=()
while IFS='|' read -r name style tempo zipurl mixurl; do
    name="$(trim "$name")"; [[ -z "$name" || "$name" == \#* ]] && continue
    style="$(trim "${style:-}")"; tempo="$(trim "${tempo:-}")"
    zipurl="$(trim "${zipurl:-}")"; mixurl="$(trim "${mixurl:-}")"
    k="$(key "$name")"
    song="$cache/$name"

    if [[ -d "$song/tracks" && -n "$(ls -A "$song/tracks" 2>/dev/null)" ]]; then
        echo "ok      $name"
        continue
    fi

    [[ -n "$zipurl" ]] && [[ ! -f "$cache/zips/$(basename "$zipurl")" ]] && curl -fL --retry 3 -o "$cache/zips/$(basename "$zipurl")" "$zipurl" || true
    [[ -n "$mixurl" ]] && [[ ! -f "$cache/zips/$(basename "$mixurl")" ]] && curl -fL --retry 3 -o "$cache/zips/$(basename "$mixurl")" "$mixurl" || true

    zip="" mix=""
    for f in "$cache"/zips/*; do
        [[ -f "$f" ]] || continue
        fk="$(key "$(basename "$f")")"
        [[ "$fk" == "$k"* ]] || continue
        case "$(lower "$f")" in
            *.zip) zip="$f" ;;
            *.mp3|*.wav|*.flac|*.aif|*.aiff|*.m4a) mix="$f" ;;
        esac
    done

    if [[ -z "$zip" ]]; then
        missing+=("$name")
        continue
    fi

    echo "unzip   $name"
    tmp="$(mktemp -d)"
    unzip -q -o "$zip" -d "$tmp"
    mkdir -p "$song/tracks"
    find "$tmp" -type f \( -iname '*.wav' -o -iname '*.aif' -o -iname '*.aiff' -o -iname '*.flac' \) -not -path '*/__MACOSX/*' -print0 |
        while IFS= read -r -d '' t; do
            if [[ $flac == 1 && "$(lower "$t")" == *.wav ]]; then
                flac -s --best -o "$song/tracks/$(basename "${t%.*}").flac" "$t"
            else
                mv "$t" "$song/tracks/"
            fi
        done
    rm -rf "$tmp"
    [[ -n "$mix" ]] && cp "$mix" "$song/mix.${mix##*.}"
    {
        echo "name = $name"
        [[ -n "$style" ]] && echo "style = $style"
        [[ -n "$tempo" ]] && echo "tempo = $tempo"
    } > "$song/song.txt"
    [[ -z "$mix" ]] && echo "        no mix found for $name: put it in $cache/zips/ and run again"
done < "$here/multitracks.txt"

# Zips that aren't in the list (songs you picked yourself): laid out too, in style pop; edit song.txt after.
for f in "$cache"/zips/*; do
    case "$(lower "$f")" in *.zip) ;; *) continue ;; esac
    base="$(basename "$f")"; base="${base%.*}"
    name="$(echo "$base" | sed -E 's/[_-]?(Full|Multitrack|Multitracks|Raw)$//I; s/_/ /g')"
    song="$cache/$name"
    [[ -d "$song/tracks" && -n "$(ls -A "$song/tracks" 2>/dev/null)" ]] && continue
    k="$(key "$name")"
    echo "unzip   $name (not in the list: style pop, edit $song/song.txt)"
    tmp="$(mktemp -d)"
    unzip -q -o "$f" -d "$tmp"
    mkdir -p "$song/tracks"
    find "$tmp" -type f \( -iname '*.wav' -o -iname '*.aif' -o -iname '*.aiff' -o -iname '*.flac' \) -not -path '*/__MACOSX/*' -exec mv {} "$song/tracks/" \;
    rm -rf "$tmp"
    for m in "$cache"/zips/*; do
        case "$(lower "$m")" in *.mp3|*.wav|*.flac|*.aif|*.aiff|*.m4a) ;; *) continue ;; esac
        [[ "$(key "$(basename "$m")")" == "$k"* ]] && cp "$m" "$song/mix.${m##*.}" && break
    done
    [[ -f "$song"/mix.* ]] 2>/dev/null || echo "        no mix found for $name"
    printf 'name = %s\nstyle = pop\n' "$name" > "$song/song.txt"
done

if (( ${#missing[@]} )); then
    echo
    echo "Not found in $cache/zips/ (download from https://www.cambridge-mt.com/ms3/mtk/ in a browser):"
    printf '  %s\n' "${missing[@]}"
fi
du -sh "$cache" 2>/dev/null | sed 's/^/Cache size: /'
