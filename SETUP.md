# SETUP — 初回だけの手順（USBはこの1回だけ）

## 0-A. 配信バス: Cloudflare（主バス・推奨）
Mac で1回だけ:
```
cd cloudflare && sh deploy.sh
```
wrangler のログイン（ブラウザ承認）と KV 作成、Worker デプロイ、シークレット
(DEVICE_KEY / ADMIN_KEY、keys.env から) 登録まで自動で行う。
完了したら Worker の URL をきなこに伝える（きなこが初回シーンとファームを流し込む）。
端末の設定ポータルには Feed URL = `<Worker URL>/scene.json`、
Telemetry URL = `<Worker URL>/telemetry`、Device Key = keys.env の DEVICE_KEY を入れる。

## 0-B. 配信バス: GitHub（フォールバック）
GitHubで空のリポジトリを作る（このプロジェクトでは作成済み: takeru/kinakko-chan）。
このフォルダの `feed/` をそのリポジトリに置く。きなこ側は GitHubコネクタで
`feed/scene.json` を更新して配信する。

※ Muse の GitHub App はリポジトリ作成ができない(403)ため、作成だけはユーザー操作。

## 1. ファームを焼く（どれか1つ）
ビルド済みバイナリ: `bin/kinako_core2.bin`（無ければ `tools/build.sh` で生成）

- **esptool（Mac/Linux, 推奨）** — merged bin を 0x0 に焼くだけ
  ```
  pip install esptool
  python -m esptool --chip esp32 --port /dev/tty.usbserial-XXXX -b 921600 write_flash 0x0 bin/kinako_core2_merged.bin
  ```
  （アプリ単体 `bin/kinako_core2.bin` は OTA配信用。USBで焼くなら merged を使う）
  Arduino IDE でビルドした場合は「スケッチ > コンパイル済みバイナリを出力」でフル構成も可。
- **Arduino IDE**: ボード M5Stack Core2 を選び `firmware/kinako_core2/kinako_core2.ino` を開いて書き込み。
  ライブラリ: M5Unified, ArduinoJson v7。
- Mac側のローカルエージェント（Claude Code等）に、private-takeru ハブ経由で
  「Core2が挿さったらこのbinを焼いて」と頼むこともできる。

## 2. 初回設定（端末の画面に出る）
起動すると設定用WiFi **KinakoCore2-Setup** が立つ（または起動直後にボタンA長押しでも入る）。
スマホ/PCで接続すると設定ページが開くので入力:
- 自宅WiFiの SSID / パスワード
- Feed URL: `https://raw.githubusercontent.com/<owner>/<repo>/main/feed/scene.json`
  （Private repo の場合は次項のトークンも必須）
- GitHub Repo: `<owner>/<repo>`（テレメトリ上り用、任意）
- GitHub Token: そのリポジトリだけに Contents Read/Write できる Fine-grained PAT（任意だが推奨）
  トークンは端末のNVSにだけ保存され、どこにも送信されない（GitHub API呼び出しにのみ使用）。

保存すると再起動し、約10秒以内に `feed/scene.json` の顔が出る。

## 3. 以後の運用
- 顔・セリフ・通知を変えたい → きなこに言うだけ。`tools/kinako.py` でシーンを作り、リポジトリへpush → 約10秒で反映。
- ファーム自体を変えたい → きなこがビルドして `feed/manifest.json` の version を上げる → 端末が10分以内にOTAで自動更新。
- センサーの様子 → `feed/telemetry.json` をきなこが読んで報告できる。

## トラブル時
- 画面が真っ黒なまま → USBで電源確認。Core2は電池切れだと起動しないことがあるので充電してから。
- WiFiにつながらない → 起動直後にボタンAを押して設定APに入り直す。
- OTAで文鎮化しない設計: ESP32のOTAは書換成功後にだけ新ファームへ切り替わり、失敗時は旧ファームのまま。
