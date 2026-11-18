
#!/usr/bin/env bash
set -euo pipefail

DEST="../ai-mixing-research/multitracks/zips"
mkdir -p "$DEST"
cd "$DEST"

download() {
  local filename="$1"
  local url="$2"

  echo "Downloading: $filename"

  curl -fL \
    --retry 5 \
    --retry-delay 3 \
    --connect-timeout 30 \
    -C - \
    -o "$filename" \
    "$url"
}

download "Actions_OneMinuteSmile.zip" \
  "http://mtkdata.cambridgemusictechnology.co.uk/MTK005/Actions_OneMinuteSmile_Full.zip" &

download "YoungGriffo_Facade.zip" \
  "http://mtkdata.cambridgemusictechnology.co.uk/MTK002/MR1110_YoungGriffo_Full.zip" &

download "RyanCali_BackDown.zip" \
  "http://multitracks.cambridge-mt.com/RyanCali_BackDown_Full.zip" &

download "TomMcKenzie_Directions.zip" \
  "http://mtkdata.cambridgemusictechnology.co.uk/MTK006/MR1304_TomMcKenzie_Full.zip" &

wait

echo ""
echo "Download processes finished. Checking archives..."

for file in *.zip; do
  if unzip -tq "$file" >/dev/null 2>&1; then
    echo "OK: $file"
  else
    echo "INVALID OR INCOMPLETE: $file"
  fi
done

echo ""
du -h ./*.zip
