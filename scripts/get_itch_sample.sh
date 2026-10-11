#!/usr/bin/env bash
# Downloads one full trading day of NASDAQ TotalView-ITCH 5.0 (Dec 30, 2019; 3.5 GB
# compressed, 8.25 GB raw, 268.7M messages). NASDAQ publishes these sample days free at
# https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/
set -euo pipefail
dest="${1:-data}"
file="12302019.NASDAQ_ITCH50.gz"
mkdir -p "$dest"
curl -L -C - -A "Mozilla/5.0" -o "$dest/$file" "https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/$file"
size=$(wc -c < "$dest/$file" | tr -d ' ')
if [ "$size" != "3524013057" ]; then
  echo "unexpected size $size (expected 3524013057); re-run to resume" >&2
  exit 1
fi
echo "ok: $dest/$file"
echo "replay: ./build/bench_itch $dest/$file --validate"
