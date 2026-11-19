#!/bin/sh
# KinakoCore2 ファームをビルドして bin/ に .bin を出す
set -e
cd "$(dirname "$0")/.."
ACLI=~/workspace/tools/arduino/arduino-cli
# VMのプロキシ認証はセッションごとに変わるので、毎回いまの値を設定し直す (古い値だと Proxy Authentication Required になる)
"$ACLI" config set network.proxy "$HTTPS_PROXY" >/dev/null 2>&1 || true
FQBN="m5stack:esp32:m5stack_core2"  # OTA対応スキームに差し替え可
mkdir -p bin build-out
"$ACLI" compile --fqbn "$FQBN" --output-dir build-out firmware/kinako_core2
cp build-out/kinako_core2.ino.bin bin/kinako_core2.bin
ls -la bin/
