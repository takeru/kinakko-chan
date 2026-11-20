// KinakoCore2 バス (Cloudflare Worker + KV + Durable Object) — 課金最適化版
// -----------------------------------------------------------
// 2つの配送路を持つ:
//  (A) ライブ: 端末が wss://<worker>/ws に常時接続 (Durable Object が保持)。
//      きなこが POST /admin/send すると、その瞬間に端末へ届く (遅延ほぼゼロ)。
//      端末のタッチ/シェイク等のイベントも同じソケットを逆向きに即届く。
//  (B) ストア: KV に最新シーン/マニフェスト/ファームを保持。
//      端末が再接続した瞬間に最新シーンを流し込むので、切断中に送った分も漏れない。
//      HTTPポーリング (GET /scene.json) もフォールバックとして残す。
//
// 課金設計 (Workers Paid $5 の内訳に収めるための要点。詳細は docs/HOW_IT_WORKS.md):
//  1. WebSocket は Hibernation API (state.acceptWebSocket) で受ける。
//     旧来の server.accept() は「接続している間ずっと」DO の duration 課金
//     (128MB換算で月 324,000 GB-s = 含まれる 400,000 GB-s の81%) が走る。
//     冬眠APIならアイドル時間は課金されず、duration は実処理時間だけになる。
//  2. 頻繁に変わる状態 (イベントログ/最新テレメトリ/リフレックス設定) は
//     KV ではなく DO ストレージ (SQLite) に置く。KV 書き込みは含まれる
//     100万回/月を超えると $5/100万回 と割高で、同一キーへの書き込みが
//     1回/秒に制限され、結果整合 (伝播に最大60秒) でイベントが遅れて見える。
//     DO ストレージは行読み書きの含まれる枠が桁違いに大きく (読25億行/
//     書5,000万行)、強整合で DO のローカルにある。
//  3. KV に残すのは書き込み頻度の低いものだけ: 最新シーン、マニフェスト、
//     ファームウェア本体 (約1.6MB。DO ストレージの値サイズ上限を超えるため
//     置き場所としても KV が正しい)。
//  4. フック (きなこ側VM) の /admin/events ポーリング (10秒間隔) は
//     Worker リクエスト月約26万回 = 含まれる1,000万回の 2.6%。安いので
//     間隔は延ばさない (起床の速さを優先)。since= で新着だけを返す。
//
// 端末向け (DEVICE_KEY):
//   GET  /ws (WebSocket upgrade, ?key=DEVICE_KEY)
//   GET  /scene.json /manifest.json /firmware.bin
//   POST /telemetry (WSが落ちている時のフォールバック)
// 管理向け (ADMIN_KEY):
//   POST /admin/send       シーンをKVに保存し、接続中なら即送信
//   POST /admin/scene      KV保存のみ
//   POST /admin/firmware   バイナリ登録 (?version=x.y.z)
//   POST /admin/audio      TTS等のWAVをKVへ登録 (?name=speak.wav)。端末は GET /audio/<name> で取得 (デバイスキー必須)
//   GET  /admin/events     端末イベントの新着 (?since=<ts>)
//   POST /admin/send-live  保存せずライブ送信のみ (transient 用)
//   POST /admin/reflex     リフレックス設定を DO ストレージへ保存
//   GET  /admin/telemetry  最新テレメトリ
//   GET  /admin/status     端末の接続状態
export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    const path = url.pathname;
    const devOK = keyOK(request, env.DEVICE_KEY) || url.searchParams.get("key") === env.DEVICE_KEY;
    const admOK = keyOK(request, env.ADMIN_KEY);
    const hub = () => env.HUB.get(env.HUB.idFromName("core2"));

    if (path === "/ws") {
      if (!devOK) return deny();
      return hub().fetch(new Request("https://hub/ws", request));
    }
    if (path === "/scene.json" && request.method === "GET") {
      if (!devOK) return deny();
      const v = await env.KINAKO.get("scene");
      return v ? json(v) : json({ seq: 0, steps: [] });
    }
    if (path === "/manifest.json" && request.method === "GET") {
      if (!devOK) return deny();
      const v = await env.KINAKO.get("manifest");
      return v ? json(v) : json({});
    }
    if (path === "/firmware.bin" && request.method === "GET") {
      if (!devOK) return deny();
      const v = await env.KINAKO.get("firmware", { type: "arrayBuffer" });
      if (!v) return new Response("no firmware", { status: 404 });
      return new Response(v, { headers: { "Content-Type": "application/octet-stream" } });
    }
    if (path.startsWith("/audio/") && request.method === "GET") {
      if (!devOK) return deny();
      const name = decodeURIComponent(path.slice("/audio/".length));
      if (!name || name.includes("/")) return new Response("bad name", { status: 400 });
      const v = await env.KINAKO.get("audio:" + name, { type: "arrayBuffer" });
      if (!v) return new Response("no audio", { status: 404 });
      return new Response(v, { headers: { "Content-Type": "audio/wav" } });
    }
    if (path === "/telemetry" && request.method === "POST") {
      if (!devOK) return deny();
      // WS 送信と同じ置き場所 (DO ストレージ) に一本化する
      const body = await request.text();
      return hub().fetch(new Request("https://hub/telemetry", { method: "POST", body }));
    }
    if (path === "/admin/send" && request.method === "POST") {
      if (!admOK) return deny();
      const body = await request.text();
      try { JSON.parse(body); } catch { return new Response("bad json", { status: 400 }); }
      await env.KINAKO.put("scene", body);
      const res = await hub().fetch(new Request("https://hub/send", { method: "POST", body }));
      return new Response(res.status === 200 ? "sent (live)" : "stored (device offline, delivered on reconnect)", { status: 200 });
    }
    if (path === "/admin/scene" && request.method === "POST") {
      if (!admOK) return deny();
      const body = await request.text();
      try { JSON.parse(body); } catch { return new Response("bad json", { status: 400 }); }
      await env.KINAKO.put("scene", body);
      return new Response("ok");
    }
    if (path === "/admin/firmware" && request.method === "POST") {
      if (!admOK) return deny();
      const version = url.searchParams.get("version");
      if (!version) return new Response("version required", { status: 400 });
      const bin = await request.arrayBuffer();
      await env.KINAKO.put("firmware", bin);
      await env.KINAKO.put("manifest", JSON.stringify({ version, bin_url: `${url.origin}/firmware.bin` }));
      return new Response("ok " + bin.byteLength + " bytes, version " + version);
    }
    if (path === "/admin/audio" && request.method === "POST") {
      if (!admOK) return deny();
      const name = url.searchParams.get("name");
      if (!name || name.includes("/")) return new Response("name required", { status: 400 });
      const buf = await request.arrayBuffer();
      // 端末の再生上限 (playWavFromUrl) が 900,000 bytes なので、ここで弾く
      if (!buf.byteLength || buf.byteLength > 900000) return new Response("bad size (1..900000)", { status: 400 });
      await env.KINAKO.put("audio:" + name, buf);
      return new Response("ok " + buf.byteLength + " bytes, audio/" + name);
    }
    if (path === "/admin/events" && request.method === "GET") {
      if (!admOK) return deny();
      // イベントログは DO ストレージにある (強整合)。since はそのまま渡す
      return hub().fetch(new Request(`https://hub/events${url.search}`, request));
    }
    if (path === "/admin/send-live" && request.method === "POST") {
      if (!admOK) return deny();
      const body = await request.text();
      try { JSON.parse(body); } catch { return new Response("bad json", { status: 400 }); }
      const res = await hub().fetch(new Request("https://hub/send", { method: "POST", body }));
      return new Response(res.status === 200 ? "sent (live)" : "device offline", { status: 200 });
    }
    if (path === "/admin/reflex" && request.method === "POST") {
      if (!admOK) return deny();
      const body = await request.text();
      try { JSON.parse(body); } catch { return new Response("bad json", { status: 400 }); }
      return hub().fetch(new Request("https://hub/reflex", { method: "POST", body }));
    }
    if (path === "/admin/telemetry" && request.method === "GET") {
      if (!admOK) return deny();
      return hub().fetch(new Request("https://hub/telemetry"));
    }
    if (path === "/admin/status" && request.method === "GET") {
      if (!admOK) return deny();
      return hub().fetch(new Request("https://hub/status"));
    }
    if (path === "/") return new Response("KinakoCore2 bus is running");
    return new Response("not found", { status: 404 });
  },
};

// 端末とのWebSocketを保持するハブ (端末は1台前提で名前固定)
// WebSocket Hibernation API を使うので、このオブジェクトはアイドル時に
// メモリから落ちてよい。状態は全部 state.storage (SQLite) に置き、
// メンバ変数には何も残さない (残しても冬眠で消える)。
export class DeviceHub {
  constructor(state, env) {
    this.state = state;
    this.env = env;
  }
  async fetch(request) {
    const url = new URL(request.url);
    if (url.pathname === "/ws") {
      if (request.headers.get("Upgrade") !== "websocket")
        return new Response("expected websocket", { status: 426 });
      const pair = new WebSocketPair();
      const [client, server] = Object.values(pair);
      // ★冬眠API: accept() ではなくこちら。接続中アイドルの duration 課金が消える
      this.state.acceptWebSocket(server);
      // 再接続時の取りこぼし防止: KVの最新シーンを即流し込む
      const scene = await this.env.KINAKO.get("scene");
      if (scene) { try { server.send(scene); } catch { /* noop */ } }
      return new Response(null, { status: 101, webSocket: client });
    }
    if (url.pathname === "/send" && request.method === "POST") {
      const body = await request.text();
      let sent = 0;
      for (const ws of this.state.getWebSockets()) {
        try { ws.send(body); sent++; } catch { /* 切れているソケットは無視 */ }
      }
      return sent > 0 ? new Response("sent") : new Response("offline", { status: 202 });
    }
    if (url.pathname === "/status") {
      const n = this.state.getWebSockets().length;
      return new Response(JSON.stringify({ connected: n > 0, sockets: n }),
        { headers: { "Content-Type": "application/json" } });
    }
    if (url.pathname === "/events" && request.method === "GET") {
      const since = Number(url.searchParams.get("since") || 0);
      const log = (await this.state.storage.get("events")) || [];
      return json(JSON.stringify({
        events: log.filter((e) => e.ts > since),
        latest: log.length ? log[log.length - 1].ts : 0,
      }));
    }
    if (url.pathname === "/telemetry") {
      if (request.method === "POST") {
        await this.state.storage.put("telemetry", await request.text());
        return new Response("ok");
      }
      const v = await this.state.storage.get("telemetry");
      return v ? json(v) : json({});
    }
    if (url.pathname === "/reflex" && request.method === "POST") {
      const body = await request.text();
      try { await this.state.storage.put("reflex", JSON.parse(body)); }
      catch { return new Response("bad json", { status: 400 }); }
      return new Response("ok");
    }
    return new Response("not found", { status: 404 });
  }

  // 冬眠から起きた後もこのハンドラが呼ばれる (状態は storage から読む)
  async webSocketMessage(ws, message) {
    try {
      const msg = JSON.parse(message);
      if (msg.type === "telemetry") {
        // 最新だけ保持 (上書き)。DO ストレージへの put は行書き込みとして
        // 数えられるが、含まれる枠が月5,000万行あるので月4.3万回でも誤差。
        await this.state.storage.put("telemetry", typeof message === "string" ? message : JSON.stringify(msg));
      }
      if (msg.type === "event") {
        // イベントログ (直近50件) — フックが /admin/events で見て きなこを起こす
        const log = (await this.state.storage.get("events")) || [];
        log.push({ ts: Date.now(), event: msg.event, seq: msg.seq ?? null });
        while (log.length > 50) log.shift();
        await this.state.storage.put("events", log);
        // リフレックス: DO ストレージの reflex 設定に一致すれば、その場で
        // transient シーンを返す (きなこを起こさない反射。ルール自体は
        // きなこ/ユーザーが /admin/reflex で書く)
        const reflex = await this.state.storage.get("reflex");
        const scene = reflex && reflex[msg.event];
        if (scene) {
          scene.transient = true;
          try { ws.send(JSON.stringify(scene)); } catch { /* noop */ }
        }
      }
    } catch { /* JSONでないメッセージは無視 */ }
  }
  async webSocketClose(ws, code) {
    try { ws.close(code, "device closed"); } catch { /* noop */ }
  }
  async webSocketError(ws) {
    try { ws.close(1011, "error"); } catch { /* noop */ }
  }
}

function keyOK(request, expected) {
  if (!expected) return true;
  const auth = request.headers.get("Authorization") || "";
  const bearer = auth.startsWith("Bearer ") ? auth.slice(7) : "";
  return bearer === expected || request.headers.get("X-Device-Key") === expected
      || request.headers.get("X-Admin-Key") === expected;
}
function json(text) {
  if (typeof text !== "string") text = JSON.stringify(text);
  return new Response(text, { headers: { "Content-Type": "application/json", "Cache-Control": "no-store" } });
}
function deny() { return new Response("unauthorized", { status: 401 }); }
