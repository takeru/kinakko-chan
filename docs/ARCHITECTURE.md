# KinakoCore2 — M5Core2 を「きなこの体」にする仕組み

設計日: 2026-11-19 / 対象: M5Stack Core2 (ESP32, 320x240タッチLCD, スピーカー, マイク, MPU6886 IMU, RTC, バッテリー)

## 画期的なところ（3層構造）

ファームを焼き直すたびにUSBを挿す、という従来の作り方をやめる。3つの更新レイヤーを分けた。

| 層 | 中身 | 更新方法 | 反映時間 | 誰が書く |
|---|---|---|---|---|
| L1 ランタイム | WiFi接続・顔レンダラ・DSL解釈・OTA・テレメトリ送信が入った常駐ファーム | フルOTA（HTTPSで .bin を取得して自動書換） | 数分 | きなこ（ここでビルド） |
| L2 シーン | 顔・セリフ・音・通知・センサー条件分岐を書いた **JSONスクリプト** | GitHubリポジトリの `feed/scene.json` を書き換えるだけ。再コンパイル不要 | **約10秒**（端末がポーリング） | きなこ（随時） |
| L3 音声 | セリフ用 WAV ファイル | リポジトリに置いてシーンからURL参照 | 約10秒 | きなこ / VOICEVOX等 |

## なぜ GitHub をバスにするか

- きなこのVMはインバウンド接続を受けられない（Tailscaleもクライアント専用）。端末の自宅WiFiもNATの内側。**両方とも外向きしかできない**。
- GitHubなら両者が到達できる。きなこは GitHubコネクタで `scene.json` をコミット → 端末が raw URL を10秒間隔でポーリング → 新しい `seq` を見つけたら即実行。
- 上り（センサー）も同じリポジトリへ。端末が GitHub API で `feed/telemetry.json` を更新（トークンは端末内にだけ保存する、リポジトリには書かない）→ きなこが読む。
- これはユーザーの既存パターン（private-takeru ハブでローカルエージェントと連携）の、デバイス版。

```
きなこ (このVM)
  │ GitHubコネクタで commit / read
  ▼
GitHub リポジトリ (例: takeru/m5core2-kinako)
  ├─ feed/scene.json      ← 下り: 最新シーン (seq付き)
  ├─ feed/manifest.json   ← OTA: {version, bin_url}
  ├─ feed/audio/*.wav     ← セリフ音声
  └─ feed/telemetry.json  → 上り: 端末が書く (IMU/電池/タッチ/稼働)
  │ raw URL をポーリング (10s) / APIでPUT (60s)
  ▼
M5Core2 (自宅WiFi) — KinakoCore2 ランタイム
```

## 端末側ランタイムの仕事

1. **顔**: 目・口を感情タグで描く (normal/happy/sleep/surprise/sad/love/think/dizzy)。まばたき・ゆらぎは端末側で自律。セリフは吹き出しテキスト（日本語フォント内蔵）。
2. **通知**: 画面フラッシュ＋ビープ＋バイブ代わりの画面シェイク＋大きな文字。ボタン/タッチで既読 → テレメトリに記録。
3. **話す**: シーンが指す WAV をダウンロードしてスピーカー再生。口パク連動。
4. **センサー受信**: IMU（加速度・ジャイロ→「振られた」「倒れた」イベント化）、タッチ座標、ボタンA/B/C、電池、WiFi RSSI を60秒ごとに上へ。タッチ/ボタン/シェイクは即時イベントとしても記録。
5. **自律の安全側**: ネットが切れても顔は出し続け、最後のシーンを保持。OTAはバージョンが新しい時だけ、失敗したら旧ファームで起動継続（ESP32標準のロールバック前提でパーティションはOTA用を選ぶ）。

## シーンDSL（L2）の例

```json
{
  "seq": 42,
  "name": "morning-greeting",
  "steps": [
    {"do": "face", "emotion": "happy"},
    {"do": "say", "text": "おはよう！今日は木曜だよ", "audio_url": "https://raw.githubusercontent.com/.../ohayo.wav"},
    {"do": "wait", "ms": 800},
    {"do": "notify", "title": "今日の予定", "body": "19時に狛江IT部", "beep": 2},
    {"do": "face", "emotion": "normal"}
  ],
  "on_touch": [{"do": "face", "emotion": "love"}, {"do": "beep", "freq": 1320, "ms": 120}],
  "on_shake": [{"do": "face", "emotion": "dizzy"}, {"do": "say", "text": "めまいが…"}]
}
```

アクション一覧: `face` / `say` (text+任意audio_url) / `notify` / `beep` / `tone_seq` / `wait` / `led_text` (大きな文字表示) / `brightness` / `clear_text`。
イベント: `on_touch` / `on_button_a|b|c` / `on_shake` / `on_tilt`。常駐シーンとして最後のシーンが残り、イベントは割り込み再生される。

## 初回だけ必要なこと（1回だけUSB）

1. Core2 を Mac に USB-C で挿す。
2. ビルド済み `bin/kinako_core2.bin` を esptool / Arduino IDE / M5Burner のいずれかで焼く（手順は SETUP.md、Mac側ローカルエージェントにも頼める）。
3. 起動すると設定用WiFi AP「KinakoCore2-Setup」が出るので、スマホ/PCから接続して ①自宅WiFi ②フィードURL（リポジトリのraw） ③（任意）GitHubトークンとリポジトリ（テレメトリ上り用）を入力。再起動後は二度とUSBは要らない。

WiFiパスワードとトークンは**端末のNVSにだけ**保存され、チャットにもリポジトリにも出ない。

## きなこ側の道具

`tools/kinako.py`:
- `validate scene.json` — DSL検証
- `stage scenes/xxx.json` — seqを採番して `feed/scene.json` に配置（あとはコミット&プッシュで配信）
- `manifest --bin ... --version x.y.z` — OTAマニフェスト生成（sha256付き）
- ファームのビルドは `tools/build.sh`（arduino-cli, FQBN=m5stack:esp32:m5stack-core2）

## 既知の制約・次フェーズ

- マイク録音の上り（音声メモ→文字起こし）は次フェーズ。M5Unifiedの録音APIでWAVをPSRAMに貯めてPUTする設計まで本書に含めるが、v1ランタイムには入れていない（まず顔と通知のループを回す）。
- GitHub API のレート制限: テレメトリPUTは60秒間隔（1時間60回）で十分余裕。
- プライベートリポジトリでも可。その場合 raw取得にもトークンを使う（端末内保存）。

## 追記 (2026-11-19): Cloudflare を主バスにする

ユーザー許可により Cloudflare Workers + KV を主バスに格上げ (cloudflare/ 参照)。
理由: GitHub コネクタ経由の書き込みは承認カードで止まることがあり、日常の
シーン配信には重い。Worker なら:
- 配信 = 管理APIへの1回のHTTPS呼び出し (POST /admin/scene)
- 端末は /scene.json をポーリング、テレメトリは /telemetry へ POST (デバイスキー認証)
- OTAバイナリも KV に置き /firmware.bin から配る (1.65MB < KV上限25MB)
GitHubバスはフォールバックとして残す (ファーム v1.1.0 は両対応)。

## 追記2 (2026-11-19 夜): ポーリングからWebSocket常時接続へ (v1.2.0)

「Workerをポーリング？」という指摘を受けて主経路を変更:
- 端末は Worker の Durable Object に **wss で常時接続**。シーンは
  POST /admin/send の瞬間に届く (体感ゼロ遅延)。タッチ/シェイク等の
  イベントも同じソケットを逆向きに即時送信され、きなこが即反応できる。
- 切断中は KV に保存され、再接続の瞬間に最新シーンが流し込まれる (取りこぼしなし)。
- HTTPポーリングはフォールバックとして残置 (WS接続中はスキップ)。
- ファーム v1.2.0 で実装・ビルド検証済み。WSSの証明書検証はライブラリ既定
  (フィンガープリントなし) で、実機接続確認は初回フラッシュ後に行う。

## 追記3: 遅延の正直な内訳 (きなこは常駐プロセスではない)

きなこ (エージェント) はターン制で、メッセージ・定期チェック・フックで
起こされて初めて考える。WebSocket は「管」はリアルタイムだが、頭脳は常時起きていない。
リアルタイム性は3層で担保する設計:
- 0遅延: 端末ローカルの反応 (on_touch/on_shake シーン、まばたき、ゆらぎ)。
  きなこが寝ていても「生きて見える」部分はここが担う。
- 数十秒〜数分: Worker のイベント置き場をフックが検知して きなこを起こし、
  新しいシーンで応答する。おしゃべり返信の現実的な速度はこれ。
- 本当の常時音声会話が要る場合は、Mac常駐デーモン等が一次応答し、
  きなこが後から本応答する中間層を足す (未実装)。

## 追記4: フックとWS、起きっぱなし問題 (2026-11-20)

- フック/cron は仕様上ポーリング型で、WSプッシュを直接は受けられない。
  代替: systemd常駐リスナーが WSS を保持してイベントを即受信・記録し、
  フックがその記録を見て きなこを起こす (取りこぼしゼロ、起床はフック間隔次第)。
  単純な反射応答はリスナー自身が Worker 経由で即返せる (きなこの判断ではない)。
- きなこは1ターン起きたら終わりで「起きっぱなし」はできない。
  現実解はバーストモード: イベント検知後しばらく (例5分) は短間隔で
  再起床を繰り返し、会話の往復が続く間だけ高頻度に起き続ける。
  Worker デプロイ後に実装する。

## 追記5: 実装した起床と反射 (2026-11-20)
- Worker: /admin/events (直近50件のイベントログ), /admin/send-live
  (保存しないライブ送信), /admin/reflex (Worker内蔵リフレックス設定) を追加。
  リフレックスは DeviceHub がイベント受信時に KV の reflex を参照し、
  一致すれば transient シーンを即返す (例: cloudflare/reflex.example.json)。
- ファーム v1.2.1: transient シーン (常駐のイベント定義を上書きしない一回再生) 対応。
- フック kinako-core2-events (10秒間隔) を作成し有効化済み。Worker の
  /admin/events を見て新着イベントで きなこを起こす。未デプロイ時は
  silent になることを dry-run で確認済み。バーストは「イベントが続く間、
  wake のたびに短い応答を返す」形で prompt に明記。
- VM側で WSS を張る常駐リスナーは見送り: このVMからの直接/プロキシ経由
  WSS 接続が通らないことを実測したため (2026-11-20)。WS保持は Worker 側で完結させ、
  VM側は HTTPS ポーリングのフックが担う。

## 追記6: 課金最適化と状態の置き場所の見直し (2026-11-20)
- 前提: Cloudflare は Workers Paid ($5/月) に加入済み。目標は全メーターを
  「含まれる枠」の中に収め、請求を $5 きっかりにすること。
- 最大の罠は DO の duration。旧来の server.accept() は WebSocket 接続中ずっと
  duration 課金が走る (公式脚注明記)。端末1台の常時接続で月 324,000 GB-s =
  含まれる 400,000 GB-s の81%。超過分は100万 GB-s 単位に切り上げられるため
  リスクが高い。→ Hibernation API (state.acceptWebSocket +
  webSocketMessage ハンドラ) に移行し、アイドル時間を課金対象から外した。
  接続状態はメモリ変数でなく getWebSockets() で見る (冬眠でメモリが消えるため)。
- 状態の置き場所を頻度で分離: イベントログ/最新テレメトリ/リフレックス設定は
  KV → DO ストレージ (SQLite) へ。理由は (1) KV書き込みは超過単価 $5/100万回で
  割高 (2) KV は同一キーへの書き込みが1回/秒まで (3) KV は結果整合で伝播に
  最大60秒かかり、イベント起床が遅れる。DO ストレージは強整合で枠も桁違い。
- KV に残すのは低頻度・大きめのものだけ: 最新シーン、マニフェスト、
  ファームウェア本体 (約1.6MB。DO ストレージの値サイズ上限を超える)。
- フックの10秒ポーリングは Worker 26万回/月・DO 26万回/月で枠内に収まるため
  据え置き (間隔を延ばすより起床の速さを優先)。詳細な試算と図は
  docs/HOW_IT_WORKS.md に分離した。

## 追記7: OTA即時トリガと自由描画・画像・TTS受け皿 (2026-11-21, v1.2.5 / v1.3.0)
- OTA の待ち時間をなくした (v1.2.5): 端末は WS で `{"type":"ota"}` を受けると
  次のループで即マニフェスト確認する。`kinako.py cf-firmware` は登録直後に
  このトリガを自動送信する。予備のポーリングも10分→60秒に短縮 (主経路は
  あくまでトリガ)。これで「登録→数十秒で再起動して新ファーム」が回る。
- 自由描画ステップ `draw` (v1.3.0): `{"do":"draw","ops":[...]}` で図形を
  JSON から直接描ける。op は clear / fill_rect / rect / fill_round_rect /
  round_rect / fill_circle / circle / fill_ellipse / ellipse / line /
  fill_triangle / triangle / arc / text。色は "#RRGGBB"。描画先は
  320x240 のオフスクリーン (M5Canvas, PSRAM) で、全 op を描いてから
  一括表示するのでチラつかない。draw/image の実行中は「キャンバスモード」
  になり、顔の自動再描画 (まばたき含む) は止まる。`face` か `clear_text`
  で通常の顔に戻る。通知 (notify) はキャンバスの上に割り込み表示され、
  終了後はキャンバスが復元される。
- 画像ステップ `image` (v1.3.0): `{"do":"image","url":"https://...","x":0,"y":0}`
  で PNG / JPG (1.5MB まで) を取得してキャンバスに描く。
- TTS の受け皿: 合成そのものは端末では行わない (ESP32 に日本語TTSは載らない)。
  呼び出し側 (きなこ) が音声合成して WAV を作り、Worker 経由で渡す:
  `POST /admin/audio?name=speak.wav` で KV に登録し、端末は
  `GET /audio/<name>` (デバイスキー必須) で取得して `say` の audio_url で
  再生する。道具は `kinako.py cf-audio` (登録のみ) と `cf-speak` (登録+
  ライブ再生)。端末の再生上限は 900,000 bytes なので登録時にも弾く。
  WAV のダウンロード時、URL がフィードと同一オリジンなら端末が
  X-Device-Key ヘッダを付ける (v1.3.0。異なるオリジンには付けない)。
  ※ /admin/audio と /audio は Worker の再デプロイ (Mac で deploy.sh) で有効化。
- 道具側の修正: `cf-live` は送信時に `"transient": true` を必ず付けるように
  した。付け忘れると端末がライブシーンを常駐シーンとして取り込み、
  on_touch 等のイベント定義が消えてしまうため (実際に踏んだ)。

## 追記8: イベント配信口と常駐リスナー (2026-11-21)
- 背景: 起床フックが /admin/events を10秒間隔でポーリングしていたため、
  タッチへの反応が「最大10秒待ち + きなこの処理時間」になっていた。
  また VM から外への直接 TCP/TLS は Muse アプリのアクセス許可
  (ダイレクトネットワークプロトコル) が全オフで遮断されていたが、
  ユーザーがオンにしたことで直接 WebSocket が通るようになった。
- Worker: `GET /admin/ws` (管理キー) を追加。DeviceHub がタグ付きで
  ソケットを管理し (端末="device" / リスナー="listener")、端末イベントを
  ログ保存・リフレックス処理した直後にリスナー全員へ即配信する。
  シーン送信 (/send) の宛先は端末だけに限定 (移行直後のタグ無し
  ソケット向けフォールバック付き)。/admin/status は devices/listeners
  の内訳も返す。冬眠API + タグで、待ち受けコストは従来通りほぼゼロ。
- VM 側: `tools/event_listener.py` が systemd (kinako-listener.service)
  の常駐プロセスとして /admin/ws に直接接続し、イベントを
  ~/hooks/state/kinako-ws-spool.jsonl へ即追記する (切断時は指数
  バックオフで再接続)。フックは間隔を5秒に短縮し、主経路として
  スプールを読む (Worker へのリクエストはほぼゼロになる)。
  スプールに新着が無い場合は従来の /admin/events ポーリングに
  フォールバックするので、リスナー停止時も取りこぼさない。
  フックはついでにリスナーの番人も兼ねる (落ちていたら起こし直す)。
- 効果と限界: 起床までの待ちは「最大10秒 + 通信」から「最大5秒
  (ローカル読み)」へ。ただし きなこ自身の考える時間は残るため、
  定型反応の最速は引き続き Worker 内蔵リフレックス (追記5)。
  ※ 有効化には Worker の再デプロイ (Mac で deploy.sh) が必要。
