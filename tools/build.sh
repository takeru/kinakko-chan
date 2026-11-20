#!/bin/sh
# KinakoCore2 ファームをビルドして bin/ に .bin を出す
#   bin/kinako_core2.bin        アプリ単体 (OTA配信用)
#   bin/kinako_core2_merged.bin ブートローダ等込み (USBで 0x0 に焼く用)
# 必要: arduino-cli、ボード定義 (esp32:esp32 か m5stack:esp32)、
#       ライブラリ M5Unified / ArduinoJson v7 / WebSockets (links2004)
set -e
cd "$(dirname "$0")/.."
# PATH の arduino-cli を優先し、無ければきなこVMの配置を使う (ACLI で上書き可)
ACLI=${ACLI:-$(command -v arduino-cli || echo ~/workspace/tools/arduino/arduino-cli)}
# VMのプロキシ認証はセッションごとに変わるので、毎回いまの値を設定し直す (古い値だと Proxy Authentication Required になる)
if [ -n "$HTTPS_PROXY" ]; then "$ACLI" config set network.proxy "$HTTPS_PROXY" >/dev/null 2>&1 || true; fi
# M5Stack 版コアがあればそれを、無ければ Espressif 公式コアの Core2 定義を使う (FQBN で上書き可)
# (esp32:esp32 の既定パーティションは OTA 対応の 2 x 6.5MB app)
if [ -z "$FQBN" ]; then
  if "$ACLI" core list | grep -q '^m5stack:esp32 '; then FQBN="m5stack:esp32:m5stack_core2"
  else FQBN="esp32:esp32:m5stack_core2"; fi
fi
echo "FQBN: $FQBN"
mkdir -p bin build-out
"$ACLI" compile --fqbn "$FQBN" --output-dir build-out firmware/kinako_core2
cp build-out/kinako_core2.ino.bin bin/kinako_core2.bin
# merged bin は esp32 コア 3.x 系が出力する (古いコアでは出ない)
if [ -f build-out/kinako_core2.ino.merged.bin ]; then cp build-out/kinako_core2.ino.merged.bin bin/kinako_core2_merged.bin
else echo "注意: merged bin が出力されなかった (USB書き込みは arduino-cli upload か Arduino IDE で)"; fi
ls -la bin/
