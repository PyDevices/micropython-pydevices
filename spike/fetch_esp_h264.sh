#!/usr/bin/env bash
# Fetch espressif/esp_h264 1.4.1 from the component registry into spike/components.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
V=1.4.1
mkdir -p "$HERE/components"
if [ ! -f "$HERE/components/esp_h264/idf_component.yml" ]; then
  tmp=$(mktemp -d)
  curl -sSL -o "$tmp/esp_h264.zip" "https://components-file.espressif.com/components/espressif/esp_h264/$V/espressif__esp_h264-v$V.zip"
  mkdir -p "$HERE/components/esp_h264"
  unzip -q "$tmp/esp_h264.zip" -d "$HERE/components/esp_h264"
  rm -rf "$tmp"
fi
grep -q "version: $V" "$HERE/components/esp_h264/idf_component.yml" && echo "esp_h264 $V in place"
