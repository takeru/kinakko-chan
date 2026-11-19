# kinakko-chan （きなっこちゃん）

M5Stack Core2 を、AIエージェント「きなこ」の小さな体にするプロジェクト。
一度だけUSBで常駐ファームを焼けば、あとは全部ネットワーク越しに更新できる。

- 開発日記: [docs/index.html](docs/index.html)（GitHub Pages でも公開）
- 設計: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)
- 動作の仕組みと課金設計（図解）: [docs/HOW_IT_WORKS.md](docs/HOW_IT_WORKS.md)
- 初回手順: [SETUP.md](SETUP.md)
- 顔シミュレータ: [docs/simulator.html](docs/simulator.html)（実機と同じシーンDSLをブラウザ再生）

## 仕組み（3層更新＋WebSocket常時接続）

| 層 | 中身 | 更新方法 | 反映 |
|---|---|---|---|
| L1 ランタイム | WiFi・顔・DSL解釈・OTA・テレメトリの常駐ファーム | HTTPS OTA | 数分 |
| L2 シーン | 顔・セリフ・通知・反応を書いたJSONスクリプト | Worker経由で送信 | **即時**（WebSocket） |
| L3 音声 | セリフ用WAV | URL参照 | 即時 |

端末は Cloudflare Worker (Durable Object) に wss で常時接続し、
シーンの受信とセンサー/イベントの送信を同じソケットで行う。
配信バスは Cloudflare Workers + KV が主、GitHub フィードが予備。

## OSS部分と設定部分の分け方

このリポジトリにあるのは **OSS部分だけ**。動かすのに必要な設定・秘密情報は
リポジトリに入れず、各ファイルの example をコピーしてローカルで埋める。

| OSSに含まれる | 設定（Git管理外, .gitignore 済み） |
|---|---|
| `firmware/` ファーム本体 | `cloudflare/keys.env`（デバイスキー/管理キー/Worker URL） |
| `cloudflare/src/` Worker本体 | `cloudflare/wrangler.toml`（自分のKV ID） |
| `cloudflare/*.example.*` 雛形 | `feed/scene.json` など実行時の個人データ |
| `tools/` 送信CLI、`scenes/` 例 | `bin/` ビルド生成物 |

## ライセンス

MIT License （LICENSE 参照）
