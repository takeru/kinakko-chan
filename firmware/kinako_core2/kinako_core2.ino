/*
 * KinakoCore2 ランタイム v1 (M5Stack Core2)
 * ----------------------------------------------
 * 一度だけUSBで焼く常駐ファーム。以後の更新は全部ネット越し:
 *  - L2 シーン: feed_url の scene.json を10秒間隔でポーリングし、
 *    新しい seq が来たらJSONスクリプト(DSL)を即実行する。再コンパイル不要。
 *  - L1 OTA   : 同じフォルダの manifest.json を10分間隔で確認し、
 *    version が新しければ bin_url から HTTPS OTA で自動書換。
 *  - 上り     : IMU/電池/タッチ等のテレメトリを60秒間隔で GitHub API へ
 *    (feed/telemetry.json)。トークン未設定ならローカルHTTP /telemetry のみ。
 *  - ローカル : 同一LANから POST /scene でも直接シーンを実行できる。
 *
 * 設定 (WiFi/フィードURL/トークン) は初回起動時の設定AP
 * 「KinakoCore2-Setup」から入力し、端末のNVSにだけ保存する。
 * 起動直後にボタンAを押していても設定APに入る。
 *
 * 必要ライブラリ: M5Unified, M5GFX (M5Unifiedの依存), ArduinoJson v7,
 *                WebSockets (Markus Sattler / links2004)
 * ボード: M5Stack Core2 (esp32:esp32:m5stack_core2 または m5stack:esp32:m5stack_core2),
 *        OTA用パーティション (既定の 2 x 6.5MB app でよい)
 */

#include <M5Unified.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <Adafruit_NeoPixel.h>
#include <ArduinoJson.h>
#include <WebSocketsClient.h>

#define FW_VERSION "1.5.8"
static const uint32_t FEED_POLL_MS = 10000;
static const uint32_t OTA_POLL_MS = 60000;  // 通常は WS の即時トリガが主経路。これは予備
static const uint32_t TELEM_POLL_MS = 60000;

// ---------------- 設定 (NVS) ----------------
Preferences prefs;
String cfgSsid[3], cfgPass[3];  // WiFi プロファイル (最大3件, v1.5.3)
String cfgFeed, cfgToken, cfgRepo, cfgTelem, cfgKey;
bool apMode = false;

// ---------------- 顔の状態 ----------------
String emotion = "normal";
String bubbleText = "";
String bigTitle = "", bigBody = "";
uint32_t bigUntil = 0;
bool faceDirty = true;
bool canvasMode = false;            // 自由描画 (draw/image) 中は顔で上書きしない
bool canvasOk = false;              // オフスクリーン確保済みか
M5Canvas canvas(&M5.Display);       // 自由描画用オフスクリーン (320x240, PSRAM)
M5Canvas faceCanvas(&M5.Display);   // 顔描画用オフスクリーン (ダブルバッファでちらつき防止)
bool faceCanvasOk = false;
uint32_t blinkAt = 0;
uint32_t lastScrollTick = 0;
bool blinking = false;
bool talkingNow = false, wasTalking = false, talkOpen = false;
uint32_t talkFlipAt = 0;
// ---- 音声入力 (STT, v1.5.2): プッシュトゥトーク ----
// 画面を押している間だけ録音し、離したら Worker の Whisper へ送る。
// M5.Mic.record() は固定長録音しかできないので 0.5秒チャンクをつなぐ。
const int REC_RATE = 16000;
const int REC_MAX_SECONDS = 15;
const size_t REC_CHUNK_SAMPLES = REC_RATE / 2;  // 0.5秒
const size_t REC_MAX_SAMPLES = (size_t)REC_RATE * REC_MAX_SECONDS;
uint8_t* recBuf = nullptr;  // WAVヘッダ44byte + PCM16モノラル (PSRAM)
bool recording = false;
bool recActive = false;      // チャンク録音中
bool touchConsumed = false;  // この押下で録音を開始済み
uint32_t recStopAt = 0;      // 離した時刻 (0 = まだ押している)
uint32_t chunkStartedAt = 0;
uint32_t recStartedAt = 0;
uint32_t lastRecUi = 0;
bool recLedOn = false;
size_t recSamples = 0;       // 確定したサンプル数
bool pendingTouch = false;
uint32_t touchDownAt = 0;
bool pressOnEar = false;  // 押した場所が耳かどうか (v1.5.4)

// ---- M5GO Bottom2 の LED バー (SK6812 x10, GPIO25, v1.5.5) ----
// 台座の両サイドに並ぶ拡散バー。録音中のフィードバックに使う。
Adafruit_NeoPixel ledBar(10, 25, NEO_GRB + NEO_KHZ800);
void ledBarAll(uint8_t r, uint8_t g, uint8_t b) {
  for (int i = 0; i < 10; i++) ledBar.setPixelColor(i, ledBar.Color(r, g, b));
  ledBar.show();
}
void ledBarOff() { ledBarAll(0, 0, 0); }

// ---------------- シーン実行 ----------------
JsonDocument residentDoc;           // 常駐シーン (イベント定義を保持)
long residentSeq = -1;
JsonDocument activeDoc;             // 実行中ステップ列
JsonArray activeSteps;
size_t stepIdx = 0;
uint32_t stepWaitUntil = 0;
bool running = false;

uint8_t* wavBuf = nullptr;          // PSRAM上のWAVバッファ (再生中は保持)
size_t wavLen = 0;

// ---------------- テレメトリ ----------------
uint32_t touchCount = 0, shakeCount = 0, btnCount = 0;
String lastEvent = "boot";
uint32_t lastShakeAt = 0, lastTiltAt = 0;

WebServer web(80);
DNSServer dns;

// ---------------- WebSocket (ライブ配送) ----------------
WebSocketsClient wsClient;
bool wsConnected = false;
bool otaRequested = false;  // WS で {"type":"ota"} を受けたら次のループで即チェックする

void wsEvent(WStype_t type, uint8_t* payload, size_t length) {
  if (type == WStype_CONNECTED) {
    wsConnected = true;
    lastEvent = "ws_connected";
    wsClient.sendTXT("{\"type\":\"hello\",\"version\":\"" FW_VERSION "\"}");
  } else if (type == WStype_DISCONNECTED) {
    wsConnected = false;
  } else if (type == WStype_TEXT) {
    JsonDocument doc;
    if (!deserializeJson(doc, payload, length)) {
      // OTA 即時チェックの指示 (シーンではない)
      if (String(doc["type"] | "") == "ota") { otaRequested = true; return; }
      // transient なシーンは常駐シーン (イベント定義) を上書きせず一回だけ再生する
      bool transient = doc["transient"] | false;
      runScene(doc, !transient);
    }
  }
}

void startWs() {
  if (!cfgFeed.length()) return;
  int p = cfgFeed.indexOf("://");
  if (p < 0) return;
  int slash = cfgFeed.indexOf('/', p + 3);
  String host = (slash < 0) ? cfgFeed.substring(p + 3) : cfgFeed.substring(p + 3, slash);
  String path = "/ws?key=" + cfgKey;
  wsClient.beginSSL(host, 443, path);
  wsClient.onEvent(wsEvent);
  wsClient.setReconnectInterval(5000);
  wsClient.enableHeartbeat(15000, 3000, 2);
}

void sendEventToServer(const char* key) {
  if (!wsConnected) return;
  String msg = "{\"type\":\"event\",\"event\":\"" + String(key) +
               "\",\"seq\":" + String(residentSeq) + "}";
  wsClient.sendTXT(msg);
}

// ---------------- 小物 ----------------
String httpGet(const String& url) {
  WiFiClientSecure client;
  client.setInsecure();  // 個人宅の専用端末。証明書検証より到達性を優先 (docs参照)
  HTTPClient http;
  if (!http.begin(client, url)) return "";
  if (cfgToken.length()) http.addHeader("Authorization", "Bearer " + cfgToken);
  if (cfgKey.length()) http.addHeader("X-Device-Key", cfgKey);
  http.setTimeout(8000);
  int code = http.GET();
  String body = (code == 200) ? http.getString() : "";
  http.end();
  return body;
}

String b64encode(const String& in) {
  static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  String out;
  int val = 0, valb = -6;
  for (size_t i = 0; i < in.length(); i++) {
    val = (val << 8) + (uint8_t)in[i];
    valb += 8;
    while (valb >= 0) { out += T[(val >> valb) & 0x3F]; valb -= 6; }
  }
  if (valb > -6) out += T[((val << 8) >> (valb + 8)) & 0x3F];
  while (out.length() % 4) out += '=';
  return out;
}

// ---------------- 顔描画 ----------------
void drawFace() {
  uint32_t now = millis();
  if (canvasMode && now >= bigUntil) { faceDirty = false; return; }  // 自由描画モード中は顔で上書きしない (通知表示中は除く)
  // 顔はオフスクリーンに描いてから一括転送する (ダブルバッファ)。
  // 直接描くと fillScreen の黒で画面が点滅して見える (まばたき毎のちらつき対策, v1.3.2)
  if (!faceCanvasOk) faceCanvasOk = (faceCanvas.createSprite(320, 240) != nullptr);
  LovyanGFX& d = faceCanvasOk ? static_cast<LovyanGFX&>(faceCanvas) : static_cast<LovyanGFX&>(M5.Display);
  if (now < bigUntil) {  // 通知の全画面表示
    d.fillScreen(d.color565(24, 60, 120));
    d.setFont(&fonts::lgfxJapanGothic_24);
    d.setTextColor(TFT_WHITE);
    d.setTextDatum(textdatum_t::middle_center);
    d.drawString(bigTitle, 160, 90);
    d.setFont(&fonts::lgfxJapanGothic_16);
    d.drawString(bigBody, 160, 140);
    d.setTextDatum(textdatum_t::top_left);
    if (faceCanvasOk) faceCanvas.pushSprite(0, 0);
    faceDirty = false;
    return;
  }
  d.fillScreen(TFT_BLACK);
  // ---- きなっこちゃんの顔 (ぬいぐるみの猫, v1.5.1) ----
  // ユーザーのぬいぐるみ写真から再構成した常時顔: ベージュのクマ
  // 着ぐるみフード (丸い耳) + ブルーグレーの猫顔 + 白い口元。
  // 大きな黒目、小さなピンクの鼻、ωの口が特徴。
  uint16_t hoodC  = d.color565(203, 168, 120);  // フード (タン)
  uint16_t hoodS  = d.color565(172, 138, 92);   // フードの影
  uint16_t earInC = d.color565(228, 205, 170);  // 耳の内側
  uint16_t faceC  = d.color565(150, 148, 158);  // 猫の毛 (ブルーグレー)
  uint16_t faceS  = d.color565(126, 124, 136);  // 顔の影
  uint16_t muzzleC = d.color565(246, 244, 241); // 口元 (白)
  uint16_t noseC  = d.color565(233, 131, 120);  // 鼻 (サーモンピンク)
  uint16_t darkC  = d.color565(38, 36, 40);     // 目と口
  uint16_t heartC = d.color565(232, 106, 146);  // ハート目

  // フード: 影 → 本体 → クマの丸い耳
  d.fillEllipse(160, 136, 120, 106, hoodS);
  d.fillEllipse(160, 130, 116, 102, hoodC);
  for (int ex : {74, 246}) {
    d.fillCircle(ex, 50, 23, hoodC);
    d.fillCircle(ex, 50, 11, earInC);
  }
  // 顔: 影つきの楕円 + 白い口元
  d.fillEllipse(160, 126, 74, 62, faceS);
  d.fillEllipse(160, 122, 72, 60, faceC);
  d.fillEllipse(160, 146, 42, 24, muzzleC);

  // 目 (大きくて少し垂れた黒目)
  int eyeLX = 128, eyeRX = 192, eyeY = 112;
  int rx = 14, ry = 18;
  if (emotion == "surprise") { rx = 16; ry = 21; eyeY -= 2; }
  else if (emotion == "sad") { ry = 11; eyeY += 3; }
  if (blinking && emotion != "sleep" && emotion != "happy" &&
      emotion != "love" && emotion != "dizzy") ry = 2;
  if (emotion == "love") {
    for (int cx : {eyeLX, eyeRX}) {
      d.fillCircle(cx - 5, eyeY - 4, 6, heartC);
      d.fillCircle(cx + 5, eyeY - 4, 6, heartC);
      d.fillTriangle(cx - 10, eyeY - 1, cx + 10, eyeY - 1, cx, eyeY + 12, heartC);
    }
  } else if (emotion == "dizzy") {
    for (int cx : {eyeLX, eyeRX}) {
      d.drawLine(cx - 7, eyeY - 7, cx + 7, eyeY + 7, darkC);
      d.drawLine(cx - 7, eyeY + 7, cx + 7, eyeY - 7, darkC);
      d.drawLine(cx - 6, eyeY - 7, cx + 8, eyeY + 7, darkC);
      d.drawLine(cx - 6, eyeY + 7, cx + 8, eyeY - 7, darkC);
    }
  } else if (emotion == "happy") {
    // 逆U字の目
    d.drawArc(eyeLX, eyeY + 4, 11, 14, 180, 360, darkC);
    d.drawArc(eyeRX, eyeY + 4, 11, 14, 180, 360, darkC);
  } else if (emotion == "sleep") {
    d.fillEllipse(eyeLX, eyeY, 11, 2, darkC);
    d.fillEllipse(eyeRX, eyeY, 11, 2, darkC);
  } else {
    d.fillEllipse(eyeLX, eyeY, rx, ry, darkC);
    d.fillEllipse(eyeRX, eyeY, (emotion == "think") ? 6 : rx, ry, darkC);
    if (ry > 6) {  // 目のハイライト (小さな点)
      d.fillCircle(eyeLX - 5, eyeY - 7, 2, TFT_WHITE);
      if (emotion != "think") d.fillCircle(eyeRX - 5, eyeY - 7, 2, TFT_WHITE);
    }
  }

  // 鼻 (小さな逆三角)
  d.fillTriangle(154, 130, 166, 130, 160, 138, noseC);

  // 口 (猫のω口。再生中は口パクが優先, v1.4.2)
  int mx = 160, my = 142;
  if (talkingNow && emotion != "sleep") {
    if (talkOpen) d.fillEllipse(mx, my + 2, 5, 7, darkC);  // 開いた口
    else {  // 閉じている時はω口
      d.drawArc(mx - 8, my - 2, 5, 7, 20, 160, darkC);
      d.drawArc(mx + 8, my - 2, 5, 7, 20, 160, darkC);
    }
  } else if (emotion == "happy" || emotion == "love") {
    d.drawArc(mx, my - 4, 10, 13, 20, 160, darkC);
  } else if (emotion == "sad") {
    d.drawArc(mx, my + 12, 8, 11, 200, 340, darkC);
  } else if (emotion == "surprise") {
    d.fillEllipse(mx, my + 4, 5, 7, darkC);
  } else if (emotion == "sleep") {
    d.fillEllipse(mx, my + 2, 3, 3, darkC);
  } else {
    // ω口: 鼻の下から左右に小さな弧
    d.drawLine(mx, 138, mx, 141, darkC);
    d.drawArc(mx - 8, my - 2, 5, 7, 20, 160, darkC);
    d.drawArc(mx + 8, my - 2, 5, 7, 20, 160, darkC);
  }

  // 吹き出し (セリフ)。長い文は横スクロールで全文読める (v1.5.8)
  if (bubbleText.length()) {
    d.fillRoundRect(14, 8, 292, 44, 8, d.color565(40, 40, 40));
    d.setFont(&fonts::lgfxJapanGothic_16);
    d.setTextColor(TFT_WHITE);
    int tw = d.textWidth(bubbleText);
    if (tw <= 284) {
      d.setTextDatum(textdatum_t::middle_center);
      d.drawString(bubbleText, 160, 30);
    } else {
      static String lastBubble = "";
      static uint32_t scrollStart = 0;
      if (bubbleText != lastBubble) { lastBubble = bubbleText; scrollStart = millis(); }
      int over = tw - 284;
      uint32_t scrollMs = (uint32_t)over * 1000 / 45;  // 45px/秒
      uint32_t cycle = 900 + scrollMs + 900;           // 前後に静止時間
      uint32_t t = (millis() - scrollStart) % cycle;
      int off = 0;
      if (t > 900) off = (t - 900 >= scrollMs) ? over : (int)((t - 900) * 45 / 1000);
      d.setClipRect(16, 10, 288, 40);
      d.setTextDatum(textdatum_t::middle_left);
      d.drawString(bubbleText, 18 - off, 30);
      d.clearClipRect();
    }
    d.setTextDatum(textdatum_t::top_left);
  }
  // 状態行 (フードに埋もれないよう暗いピルを敷く)
  d.fillRoundRect(2, 222, 228, 15, 4, d.color565(28, 24, 20));
  d.setFont(&fonts::Font0);
  d.setTextColor(d.color565(170, 160, 145));
  String st = "KinakoCore2 v" + String(FW_VERSION) + "  seq:" + String(residentSeq) +
              "  batt:" + String(M5.Power.getBatteryLevel()) + "%";
  d.drawString(st, 6, 226);
  // 録音中インジケーター (吹き出し右上の赤い点滅バッジ)
  if (recording) {
    bool on = (millis() / 400) % 2 == 0;
    d.fillCircle(300, 14, 8, TFT_WHITE);
    d.fillCircle(300, 14, on ? 6 : 4, d.color565(229, 57, 53));
  }
  if (faceCanvasOk) faceCanvas.pushSprite(0, 0);
  faceDirty = false;
}

// ---------------- 自由描画 (draw / image) ----------------
// URL がフィードと同じオリジン (scheme://host[:port]) か。
// 同じならデバイスキーを付けてよい (Worker の保護付き配布物用)
bool sameOrigin(const String& url) {
  auto origin = [](const String& u) {
    int p = u.indexOf("://");
    if (p < 0) return String("");
    int s = u.indexOf('/', p + 3);
    return (s < 0) ? u : u.substring(0, s);
  };
  return cfgFeed.length() && origin(url) == origin(cfgFeed);
}

uint16_t parseColor(JsonVariant v, uint16_t def = TFT_WHITE) {
  if (v.is<const char*>()) {
    String c = v.as<const char*>();
    if (c.startsWith("#") && c.length() >= 7) {
      long rgb = strtol(c.c_str() + 1, nullptr, 16);
      return M5.Display.color565((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255);
    }
    return def;
  }
  if (v.is<int>()) return (uint16_t)(v.as<int>() & 0xFFFF);
  return def;
}

void ensureCanvas() {
  if (!canvasOk) {
    canvasOk = (canvas.createSprite(320, 240) != nullptr);
    if (canvasOk) canvas.fillScreen(TFT_BLACK);
  }
}

// {"do":"draw","ops":[{op:..., ...}, ...]} をオフスクリーンに描いて一括表示する
void execDraw(JsonArray ops) {
  if (ops.isNull()) return;
  ensureCanvas();
  if (!canvasOk) return;
  canvasMode = true;
  for (JsonObject o : ops) {
    String op = o["op"] | "";
    uint16_t col = parseColor(o["color"]);
    int x = o["x"] | 0, y = o["y"] | 0;
    if (op == "clear") canvas.fillScreen(parseColor(o["color"], TFT_BLACK));
    else if (op == "fill_rect") canvas.fillRect(x, y, o["w"] | 0, o["h"] | 0, col);
    else if (op == "rect") canvas.drawRect(x, y, o["w"] | 0, o["h"] | 0, col);
    else if (op == "fill_round_rect") canvas.fillRoundRect(x, y, o["w"] | 0, o["h"] | 0, o["r"] | 4, col);
    else if (op == "round_rect") canvas.drawRoundRect(x, y, o["w"] | 0, o["h"] | 0, o["r"] | 4, col);
    else if (op == "fill_circle") canvas.fillCircle(x, y, o["r"] | 10, col);
    else if (op == "circle") canvas.drawCircle(x, y, o["r"] | 10, col);
    else if (op == "fill_ellipse") canvas.fillEllipse(x, y, o["rx"] | 10, o["ry"] | 10, col);
    else if (op == "ellipse") canvas.drawEllipse(x, y, o["rx"] | 10, o["ry"] | 10, col);
    else if (op == "line") canvas.drawLine(x, y, o["x2"] | x, o["y2"] | y, col);
    else if (op == "fill_triangle") canvas.fillTriangle(x, y, o["x2"] | x, o["y2"] | y, o["x3"] | x, o["y3"] | y, col);
    else if (op == "triangle") canvas.drawTriangle(x, y, o["x2"] | x, o["y2"] | y, o["x3"] | x, o["y3"] | y, col);
    else if (op == "arc") canvas.drawArc(x, y, o["r0"] | 10, o["r1"] | 12, o["a0"] | 0, o["a1"] | 360, col);
    else if (op == "text") {
      int size = o["size"] | 16;
      canvas.setFont(size >= 20 ? &fonts::lgfxJapanGothic_24 : &fonts::lgfxJapanGothic_16);
      canvas.setTextColor(col);
      bool center = String(o["align"] | "") == "center";
      canvas.setTextDatum(center ? textdatum_t::middle_center : textdatum_t::top_left);
      canvas.drawString((const char*)(o["text"] | ""), x, y);
      canvas.setTextDatum(textdatum_t::top_left);
    }
  }
  canvas.pushSprite(0, 0);
}

// {"do":"image","url":"https://... (PNG/JPG)","x":0,"y":0}
void execImage(JsonObject s) {
  String url = s["url"] | "";
  if (!url.length()) return;
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  if (!http.begin(client, url)) return;
  if (cfgToken.length()) http.addHeader("Authorization", "Bearer " + cfgToken);
  if (cfgKey.length() && sameOrigin(url)) http.addHeader("X-Device-Key", cfgKey);
  if (http.GET() != 200) { http.end(); return; }
  int len = http.getSize();
  if (len <= 0 || len > 1500000) { http.end(); return; }
  uint8_t* buf = (uint8_t*)ps_malloc(len);
  if (!buf) { http.end(); return; }
  WiFiClient* stream = http.getStreamPtr();
  size_t got = 0;
  uint32_t start = millis();
  while (got < (size_t)len && millis() - start < 10000) {
    int avail = stream->available();
    if (avail > 0) got += stream->readBytes(buf + got, min(avail, len - (int)got));
    else delay(5);
  }
  http.end();
  if (got != (size_t)len) { free(buf); return; }
  ensureCanvas();
  if (!canvasOk) { free(buf); return; }
  canvasMode = true;
  int x = s["x"] | 0, y = s["y"] | 0;
  if (len > 3 && buf[0] == 0x89 && buf[1] == 'P') canvas.drawPng(buf, len, x, y);
  else if (len > 2 && buf[0] == 0xFF && buf[1] == 0xD8) canvas.drawJpg(buf, len, x, y);
  free(buf);
  canvas.pushSprite(0, 0);
}

// ---------------- 音 ----------------
bool playWavFromUrl(const String& url) {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  if (!http.begin(client, url)) return false;
  if (cfgToken.length()) http.addHeader("Authorization", "Bearer " + cfgToken);
  if (cfgKey.length() && sameOrigin(url)) http.addHeader("X-Device-Key", cfgKey);
  int code = http.GET();
  if (code != 200) { http.end(); return false; }
  int len = http.getSize();
  if (len <= 0 || len > 900000) { http.end(); return false; }
  if (!wavBuf) wavBuf = (uint8_t*)ps_malloc(900000);
  if (!wavBuf) { http.end(); return false; }
  WiFiClient* stream = http.getStreamPtr();
  size_t got = 0;
  uint32_t start = millis();
  while (got < (size_t)len && millis() - start < 10000) {
    int avail = stream->available();
    if (avail > 0) got += stream->readBytes(wavBuf + got, min(avail, len - (int)got));
    else delay(5);
  }
  http.end();
  if (got != (size_t)len) return false;
  wavLen = got;
  M5.Speaker.playWav(wavBuf, wavLen);
  return true;
}

// ---------------- 音声入力 (STT, v1.5.0) ----------------
// プッシュトゥトーク: 押している間チャンク録音し、離したら WAV にして
// Worker の POST /stt へ送る。文字起こし結果は吹き出しに表示され、同じ文が
// on_speech イベントとして Worker 経由できなこにも届く。
String feedOrigin() {
  int p = cfgFeed.indexOf("://");
  if (p < 0) return String("");
  int s = cfgFeed.indexOf('/', p + 3);
  return (s < 0) ? cfgFeed : cfgFeed.substring(0, s);
}

void writeWavHeader(uint8_t* p, uint32_t dataLen) {
  memcpy(p, "RIFF", 4); *(uint32_t*)(p + 4) = 36 + dataLen; memcpy(p + 8, "WAVE", 4);
  memcpy(p + 12, "fmt ", 4); *(uint32_t*)(p + 16) = 16; *(uint16_t*)(p + 20) = 1;
  *(uint16_t*)(p + 22) = 1; *(uint32_t*)(p + 24) = REC_RATE;
  *(uint32_t*)(p + 28) = REC_RATE * 2; *(uint16_t*)(p + 32) = 2; *(uint16_t*)(p + 34) = 16;
  memcpy(p + 36, "data", 4); *(uint32_t*)(p + 40) = dataLen;
}

void startRecording() {
  if (recording || M5.Speaker.isPlaying()) return;
  if (!recBuf) recBuf = (uint8_t*)ps_malloc(44 + REC_MAX_SAMPLES * 2);
  if (!recBuf) return;
  M5.Speaker.end();  // Core2 はマイクとスピーカーで I2S を共有する
  M5.Mic.begin();
  recSamples = 0;
  recStopAt = 0;
  if (M5.Mic.record((int16_t*)(recBuf + 44), REC_CHUNK_SAMPLES, REC_RATE)) {
    recording = true;
    recActive = true;
    recStartedAt = chunkStartedAt = millis();
    lastRecUi = 0;
    bubbleText = "ききとりちゅう…";
    recLedOn = true;
    ledBarAll(90, 0, 0);
    faceDirty = true;
  } else {
    M5.Mic.end();
    M5.Speaker.begin();
  }
}

void finishRecording() {
  recording = false;
  recActive = false;
  recStopAt = 0;
  ledBarOff();
  M5.Mic.end();
  M5.Speaker.begin();
  String text = "";
  if (recSamples >= 1600) {  // 0.1秒未満は送らない
    // v1.5.7: 先頭のタッチノイズ (約0.35秒) を捨て、残りをピーク基準で
    // 増幅してから送る (声が小さいと Whisper が幻覚を出しやすいので)
    {
      int16_t* smp = (int16_t*)(recBuf + 44);
      const int skip = 5600;  // 0.35秒
      if (recSamples > skip + 1600) {
        int peak = 0;
        for (int i = skip; i < recSamples; i++) {
          int a = smp[i]; if (a < 0) a = -a; if (a > peak) peak = a;
        }
        float gain = (peak > 0) ? 26000.0f / peak : 1.0f;
        if (gain > 6.0f) gain = 6.0f;
        if (gain < 1.0f) gain = 1.0f;
        int outLen = recSamples - skip;
        for (int i = 0; i < outLen; i++) {
          int v = (int)(smp[skip + i] * gain);
          if (v > 32767) v = 32767; else if (v < -32767) v = -32767;
          smp[i] = (int16_t)v;
        }
        recSamples = outLen;
      }
    }
    writeWavHeader(recBuf, recSamples * 2);
    bubbleText = "おくっているよ…";
    faceDirty = true;
    drawFace();  // アップロード中 (数秒ブロック) も状態が見えるように先に描く
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.setTimeout(25000);
    if (http.begin(client, feedOrigin() + "/stt")) {
      if (cfgKey.length()) http.addHeader("X-Device-Key", cfgKey);
      http.addHeader("Content-Type", "audio/wav");
      int code = http.sendRequest("POST", recBuf, 44 + recSamples * 2);
      if (code == 200) {
        StaticJsonDocument<1024> doc;
        if (deserializeJson(doc, http.getString()) == DeserializationError::Ok)
          text = (const char*)(doc["text"] | "");
      }
      http.end();
    }
  }
  if (text.length()) {
    bubbleText = (text.length() > 120) ? text.substring(0, 120) : text;
    lastEvent = "on_speech";
    ledBarAll(0, 130, 50);  // 聞き取れたら緑でお知らせ
    delay(400);
    ledBarOff();
  } else {
    bubbleText = "ききとれなかった…";
    for (int i = 0; i < 2; i++) { ledBarAll(120, 0, 0); delay(150); ledBarOff(); delay(150); }
  }
  faceDirty = true;
}

// 録音の進行管理: チャンクをつなぎ、離したら最終チャンクを離した
// 時刻で切り取って送信する。録音中は LED 点滅 + 経過秒表示も更新。
void serviceRecording(uint32_t now) {
  if (!recording) return;
  if (now - lastRecUi >= 250) {
    lastRecUi = now;
    recLedOn = !recLedOn;
    // LED バー: 経過秒数だけ赤く埋まっていく (先端が点滅)
    {
      int lit = (int)((now - recStartedAt) / 1000) + 1;
      if (lit > 10) lit = 10;
      for (int i = 0; i < 10; i++) {
        if (i < lit - 1) ledBar.setPixelColor(i, ledBar.Color(90, 0, 0));
        else if (i == lit - 1) ledBar.setPixelColor(i, recLedOn ? ledBar.Color(220, 30, 30) : ledBar.Color(60, 0, 0));
        else ledBar.setPixelColor(i, 0);
      }
      ledBar.show();
    }
    bubbleText = "ききとりちゅう " + String((now - recStartedAt) / 1000) + "びょう";
    faceDirty = true;
  }
  if (recActive && !M5.Mic.isRecording()) {
    recActive = false;
    if (recStopAt) {
      uint32_t ms = (recStopAt > chunkStartedAt) ? (recStopAt - chunkStartedAt) : 0;
      size_t valid = (size_t)((uint64_t)ms * REC_RATE / 1000);
      if (valid > REC_CHUNK_SAMPLES) valid = REC_CHUNK_SAMPLES;
      recSamples += valid;
      finishRecording();
      return;
    }
    recSamples += REC_CHUNK_SAMPLES;
    if (recSamples + REC_CHUNK_SAMPLES > REC_MAX_SAMPLES) { finishRecording(); return; }
    if (M5.Mic.record((int16_t*)(recBuf + 44 + recSamples * 2), REC_CHUNK_SAMPLES, REC_RATE)) {
      recActive = true;
      chunkStartedAt = now;
    } else {
      finishRecording();
    }
  } else if (!recActive && recStopAt) {
    finishRecording();
  }
}

void beep(int freq, int ms) { M5.Speaker.tone(freq, ms); }

// ---------------- 音量 (v1.5.6) ----------------
int cfgVolume = 180;
uint32_t cDownAt = 0, bcDownAt = 0, ledBarUntil = 0;

void applyVolume(int v, bool announce) {
  cfgVolume = constrain(v, 0, 255);
  M5.Speaker.setVolume(cfgVolume);
  prefs.begin("kinako", false);
  prefs.putInt("volume", cfgVolume);
  prefs.end();
  if (announce) {
    bubbleText = (cfgVolume == 0) ? "ミュート" : "おんりょう " + String(cfgVolume);
    faceDirty = true;
    if (!recording) {  // LED バーで量を少しの間だけ表示
      int lit = (cfgVolume * 10 + 127) / 255;
      for (int i = 0; i < 10; i++)
        ledBar.setPixelColor(i, i < lit ? ledBar.Color(50, 38, 16) : 0);
      ledBar.show();
      ledBarUntil = millis() + 700;
    }
  }
}

// ---------------- DSL実行 ----------------
void startSteps(JsonArray steps) {
  String serialized;
  serializeJson(steps, serialized);
  activeDoc.clear();
  deserializeJson(activeDoc, serialized);
  activeSteps = activeDoc.as<JsonArray>();
  stepIdx = 0;
  stepWaitUntil = 0;
  running = true;
}

void execStep(JsonObject s) {
  String act = s["do"] | "";
  if (act == "face") {
    canvasMode = false;
    emotion = (const char*)(s["emotion"] | "normal");
    faceDirty = true;
  } else if (act == "draw") {
    execDraw(s["ops"].as<JsonArray>());
  } else if (act == "image") {
    execImage(s);
  } else if (act == "say") {
    bubbleText = (const char*)(s["text"] | "");
    faceDirty = true;
    String url = s["audio_url"] | "";
    if (url.length()) playWavFromUrl(url);
  } else if (act == "notify") {
    bigTitle = (const char*)(s["title"] | "");
    bigBody = (const char*)(s["body"] | "");
    int dur = s["duration_ms"] | 4000;
    bigUntil = millis() + dur;
    int n = s["beep"] | 1;
    for (int i = 0; i < n; i++) { beep(1568, 120); delay(180); }
    faceDirty = true;
  } else if (act == "beep") {
    beep(s["freq"] | 880, s["ms"] | 120);
  } else if (act == "tone_seq") {
    for (JsonObject t : s["tones"].as<JsonArray>()) {
      beep(t["freq"] | 880, t["ms"] | 120);
      delay((t["ms"] | 120) + 40);
    }
  } else if (act == "led_text") {
    bigTitle = (const char*)(s["text"] | "");
    bigBody = "";
    bigUntil = millis() + (int)(s["duration_ms"] | 3000);
    faceDirty = true;
  } else if (act == "brightness") {
    M5.Display.setBrightness(s["value"] | 160);
  } else if (act == "volume") {
    applyVolume(s["value"] | 180, false);
  } else if (act == "clear_text") {
    canvasMode = false;
    bubbleText = "";
    bigUntil = 0;
    faceDirty = true;
  }
  // "wait" は stepWaitUntil 側で処理
}

void runScene(JsonDocument& doc, bool makeResident) {
  if (makeResident) {
    residentDoc = doc;
    residentSeq = doc["seq"] | residentSeq;
  }
  JsonArray steps = doc["steps"].as<JsonArray>();
  if (!steps.isNull()) startSteps(steps);
}

void fireEvent(const char* key) {
  lastEvent = key;
  sendEventToServer(key);
  JsonArray ev = residentDoc[key].as<JsonArray>();
  if (!ev.isNull()) startSteps(ev);
}

void stepMachine() {
  if (!running) return;
  if (stepWaitUntil && millis() < stepWaitUntil) return;
  stepWaitUntil = 0;
  if (stepIdx >= activeSteps.size()) { running = false; return; }
  JsonObject s = activeSteps[stepIdx++];
  String act = s["do"] | "";
  if (act == "wait") {
    stepWaitUntil = millis() + (int)(s["ms"] | 500);
    return;
  }
  execStep(s);
}

// ---------------- フィード / OTA / テレメトリ ----------------
void pollFeed() {
  if (!cfgFeed.length()) return;
  String body = httpGet(cfgFeed);
  if (!body.length()) return;
  JsonDocument doc;
  if (deserializeJson(doc, body)) return;
  long seq = doc["seq"] | -1;
  if (seq > residentSeq) runScene(doc, true);
}

String manifestUrl() {
  String u = cfgFeed;
  int slash = u.lastIndexOf('/');
  if (slash < 0) return "";
  return u.substring(0, slash + 1) + "manifest.json";
}

bool versionNewer(const String& a, const String& b) {  // a > b ?
  int ai[3] = {0, 0, 0}, bi[3] = {0, 0, 0};
  sscanf(a.c_str(), "%d.%d.%d", &ai[0], &ai[1], &ai[2]);
  sscanf(b.c_str(), "%d.%d.%d", &bi[0], &bi[1], &bi[2]);
  for (int i = 0; i < 3; i++) if (ai[i] != bi[i]) return ai[i] > bi[i];
  return false;
}

void pollOta() {
  String mu = manifestUrl();
  if (!mu.length()) return;
  String body = httpGet(mu);
  if (!body.length()) return;
  JsonDocument doc;
  if (deserializeJson(doc, body)) return;
  String ver = doc["version"] | "";
  String bin = doc["bin_url"] | "";
  if (ver.length() && bin.length() && versionNewer(ver, FW_VERSION)) {
    WiFiClientSecure client;
    client.setInsecure();
    httpUpdate.rebootOnUpdate(true);
    // httpUpdate はカスタムヘッダを付けられない。Worker の /firmware.bin は
    // デバイスキー必須なので、キーがある場合はクエリパラメータで渡す
    // (v1.2.2 まではキー無しで 401 になり OTA が常に失敗していた)
    String u = bin;
    if (cfgKey.length()) u += (u.indexOf('?') >= 0 ? "&key=" : "?key=") + cfgKey;
    httpUpdate.update(client, u);  // 成功時はここで再起動する
  }
}

String telemetryJson() {
  float ax = 0, ay = 0, az = 0, gx = 0, gy = 0, gz = 0;
  M5.Imu.update();
  M5.Imu.getAccel(&ax, &ay, &az);
  M5.Imu.getGyro(&gx, &gy, &gz);
  JsonDocument doc;
  doc["version"] = FW_VERSION;  // 遠隔からファーム版数を確定できるように (v1.3.1〜)
  doc["seq"] = residentSeq;
  doc["emotion"] = emotion;
  doc["battery"] = M5.Power.getBatteryLevel();
  doc["voltage_mv"] = M5.Power.getBatteryVoltage();
  doc["charging"] = M5.Power.isCharging();
  doc["rssi"] = WiFi.RSSI();
  doc["heap"] = ESP.getFreeHeap();
  doc["uptime_s"] = millis() / 1000;
  // 配列で入れる。以前は serialized() で CSV を生挿入しており
  // "accel":0.12,-0.98,0.05 のような不正な JSON になっていたため、
  // Worker 側の JSON.parse で弾かれてテレメトリが届かなかった (v1.2.0〜1.2.3 のバグ)
  JsonArray acc = doc["accel"].to<JsonArray>();
  acc.add(ax); acc.add(ay); acc.add(az);
  JsonArray gyr = doc["gyro"].to<JsonArray>();
  gyr.add(gx); gyr.add(gy); gyr.add(gz);
  doc["touch_count"] = touchCount;
  doc["shake_count"] = shakeCount;
  doc["button_count"] = btnCount;
  doc["last_event"] = lastEvent;
  String out;
  serializeJson(doc, out);
  return out;
}

void pushTelemetry() {
  String tj = telemetryJson();
  if (wsConnected) {
    wsClient.sendTXT("{\"type\":\"telemetry\",\"data\":" + tj + "}");
    return;
  }
  if (cfgTelem.length()) {  // 汎用エンドポイント (Cloudflare Worker 等) へPOST
    WiFiClientSecure c; c.setInsecure();
    HTTPClient h;
    if (h.begin(c, cfgTelem)) {
      h.addHeader("Content-Type", "application/json");
      if (cfgKey.length()) h.addHeader("X-Device-Key", cfgKey);
      h.POST(tj);
      h.end();
    }
    return;
  }
  if (!cfgToken.length() || !cfgRepo.length()) return;
  String path = "feed/telemetry.json";
  String api = "https://api.github.com/repos/" + cfgRepo + "/contents/" + path;
  // 既存ファイルの sha を取得
  String sha = "";
  {
    WiFiClientSecure c; c.setInsecure();
    HTTPClient h;
    if (h.begin(c, api)) {
      h.addHeader("Authorization", "Bearer " + cfgToken);
      h.addHeader("Accept", "application/vnd.github+json");
      if (h.GET() == 200) {
        JsonDocument d;
        if (!deserializeJson(d, h.getString())) sha = (const char*)(d["sha"] | "");
      }
      h.end();
    }
  }
  JsonDocument body;
  body["message"] = "telemetry from KinakoCore2";
  body["content"] = b64encode(telemetryJson());
  if (sha.length()) body["sha"] = sha;
  String payload;
  serializeJson(body, payload);
  WiFiClientSecure c; c.setInsecure();
  HTTPClient h;
  if (h.begin(c, api)) {
    h.addHeader("Authorization", "Bearer " + cfgToken);
    h.addHeader("Accept", "application/vnd.github+json");
    h.addHeader("Content-Type", "application/json");
    h.PUT(payload);
    h.end();
  }
}

// ---------------- Web (設定ポータル / ローカルAPI) ----------------
void setupWeb() {
  web.on("/", []() {
    if (apMode) {
      String html = "<html><head><meta charset='utf-8'><title>KinakoCore2 Setup</title></head><body>"
        "<h2>KinakoCore2 セットアップ</h2><form method='POST' action='/save'>"
        "WiFi 1 SSID <input name='ssid1' value='" + cfgSsid[0] + "'><br>"
        "WiFi 1 Password <input name='pass1' type='password'><br>"
        "WiFi 2 SSID <input name='ssid2' value='" + cfgSsid[1] + "'><br>"
        "WiFi 2 Password <input name='pass2' type='password'><br>"
        "WiFi 3 SSID <input name='ssid3' value='" + cfgSsid[2] + "'><br>"
        "WiFi 3 Password <input name='pass3' type='password'><br>"
        "音量 (0-255, 0=ミュート) <input name='volume' size='5' value='" + String(cfgVolume) + "'><br>"
        "Feed URL (scene.json の raw URL) <input name='feed' size='60' value='" + cfgFeed + "'><br>"
        "GitHub Repo (owner/name, テレメトリ用) <input name='repo' size='40' value='" + cfgRepo + "'><br>"
        "Telemetry URL (任意, 例 Cloudflare Worker) <input name='telem' size='60' value='" + cfgTelem + "'><br>"
        "Device Key (任意) <input name='key' type='password' size='40'><br>"
        "GitHub Token (任意, GitHubバス併用時) <input name='token' type='password' size='40'><br>"
        "<input type='submit' value='保存して再起動'></form></body></html>";
      web.send(200, "text/html", html);
    } else {
      web.send(200, "text/plain", "KinakoCore2 v" FW_VERSION);
    }
  });
  web.on("/save", HTTP_POST, []() {
    prefs.begin("kinako", false);
    {
      char k[8];
      for (int i = 0; i < 3; i++) {
        snprintf(k, sizeof k, "ssid%d", i + 1);
        prefs.putString(k, web.arg(k));
        snprintf(k, sizeof k, "pass%d", i + 1);
        if (web.arg(k).length()) prefs.putString(k, web.arg(k));
      }
    }
    if (web.arg("volume").length()) {
      int v = web.arg("volume").toInt();
      prefs.putInt("volume", constrain(v, 0, 255));
    }
    prefs.putString("feed", web.arg("feed"));
    prefs.putString("repo", web.arg("repo"));
    prefs.putString("telem", web.arg("telem"));
    if (web.arg("key").length()) prefs.putString("key", web.arg("key"));
    if (web.arg("token").length()) prefs.putString("token", web.arg("token"));
    prefs.end();
    web.send(200, "text/plain", "saved. rebooting...");
    delay(500);
    ESP.restart();
  });
  web.on("/telemetry", []() { web.send(200, "application/json", telemetryJson()); });
  web.on("/scene", HTTP_POST, []() {
    JsonDocument doc;
    if (!deserializeJson(doc, web.arg("plain"))) {
      runScene(doc, false);
      web.send(200, "text/plain", "ok");
    } else web.send(400, "text/plain", "bad json");
  });
  web.onNotFound([]() {
    if (apMode) { web.sendHeader("Location", "/"); web.send(302); }
    else web.send(404, "text/plain", "not found");
  });
  web.begin();
}

// ---------------- WiFi 接続 (複数プロファイル, v1.5.3) ----------------
// 見えている既知APの中で一番電波の強いものにつなぐ。家とテザリングを
// 行き来しても自動で切り替わる。SSID を隠しているAP用に、見えない
// プロファイルも後で順に試す。
bool tryConnect(int i, uint32_t timeoutMs) {
  WiFi.begin(cfgSsid[i].c_str(), cfgPass[i].c_str());
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) delay(250);
  if (WiFi.status() == WL_CONNECTED) return true;
  WiFi.disconnect();
  return false;
}

bool connectWifi() {
  WiFi.mode(WIFI_STA);
  int n = WiFi.scanNetworks();
  bool tried[3] = {false, false, false};
  for (int k = 0; k < 3; k++) {
    int best = -1, bestRssi = -999;
    for (int i = 0; i < 3; i++) {
      if (!cfgSsid[i].length() || tried[i]) continue;
      for (int j = 0; j < n; j++) {
        if (WiFi.SSID(j) == cfgSsid[i] && (int)WiFi.RSSI(j) > bestRssi) {
          bestRssi = WiFi.RSSI(j);
          best = i;
        }
      }
    }
    if (best < 0) break;
    tried[best] = true;
    if (tryConnect(best, 12000)) { WiFi.scanDelete(); return true; }
  }
  WiFi.scanDelete();
  for (int i = 0; i < 3; i++) {
    if (!cfgSsid[i].length() || tried[i]) continue;
    if (tryConnect(i, 10000)) return true;
  }
  return false;
}

// ---------------- setup / loop ----------------
void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);
  M5.Display.setBrightness(160);
  M5.Speaker.setVolume(180);
  ledBar.begin();
  ledBarAll(40, 26, 8);  // 起動確認のほんのり点灯
  delay(300);
  ledBarOff();
  M5.update();
  bool forcePortal = M5.BtnA.isPressed();

  prefs.begin("kinako", true);
  {
    char k[8];
    for (int i = 0; i < 3; i++) {
      snprintf(k, sizeof k, "ssid%d", i + 1); cfgSsid[i] = prefs.getString(k, "");
      snprintf(k, sizeof k, "pass%d", i + 1); cfgPass[i] = prefs.getString(k, "");
    }
    if (!cfgSsid[0].length()) {  // 旧形式 (ssid/pass) からの移行
      cfgSsid[0] = prefs.getString("ssid", "");
      cfgPass[0] = prefs.getString("pass", "");
    }
  }
  cfgFeed = prefs.getString("feed", "");
  cfgToken = prefs.getString("token", "");
  cfgRepo = prefs.getString("repo", "");
  cfgTelem = prefs.getString("telem", "");
  cfgKey = prefs.getString("key", "");
  prefs.end();
  cfgVolume = prefs.getInt("volume", 180);
  M5.Speaker.setVolume(cfgVolume);
  bool portalFlag = false;
  {
    prefs.begin("kinako", false);
    portalFlag = prefs.getBool("portal", false);
    if (portalFlag) prefs.putBool("portal", false);
    prefs.end();
  }

  emotion = "think";
  drawFace();

  if (forcePortal || portalFlag || !(cfgSsid[0].length() || cfgSsid[1].length() || cfgSsid[2].length())) {
    apMode = true;
    WiFi.mode(WIFI_AP);
    WiFi.softAP("KinakoCore2-Setup");
    dns.start(53, "*", WiFi.softAPIP());
    setupWeb();
    bubbleText = "WiFi 'KinakoCore2-Setup' につないで設定してね";
    emotion = "normal";
    faceDirty = true;
    return;
  }

  if (!connectWifi()) {  // つながらなければ設定APへ
    apMode = true;
    WiFi.mode(WIFI_AP);
    WiFi.softAP("KinakoCore2-Setup");
    dns.start(53, "*", WiFi.softAPIP());
    setupWeb();
    bubbleText = "WiFiにつながらなかった。設定し直してね";
    faceDirty = true;
    return;
  }
  setupWeb();
  startWs();
  emotion = "happy";
  bubbleText = "つながったよ！";
  faceDirty = true;
  pollFeed();
}

uint32_t lastFeed = 0, lastOta = 0, lastTelem = 0, lastBlink = 0, lastImu = 0, wifiDownSince = 0;

void loop() {
  M5.update();
  web.handleClient();
  if (apMode) { dns.processNextRequest(); if (faceDirty) drawFace(); delay(10); return; }

  wsClient.loop();
  uint32_t now = millis();

  // WiFi が20秒以上切れていたら再スキャンして一番強い既知APへ (v1.5.3)
  if (WiFi.status() != WL_CONNECTED) {
    if (!wifiDownSince) wifiDownSince = now;
    else if (now - wifiDownSince > 20000) {
      wifiDownSince = 0;
      bubbleText = "WiFiをさがしているよ…";
      faceDirty = true;
      drawFace();
      connectWifi();
    }
  } else wifiDownSince = 0;

  // まばたき
  if (now - lastBlink > 3400) {
    lastBlink = now;
    blinking = true;
    faceDirty = true;
    blinkAt = now + 140;
  }
  if (blinking && now > blinkAt) { blinking = false; faceDirty = true; }

  // 口パク (音声の再生中だけ口を開閉させる, v1.4.2)
  talkingNow = M5.Speaker.isPlaying();
  if (talkingNow) {
    if (now > talkFlipAt) { talkOpen = !talkOpen; talkFlipAt = now + 130; faceDirty = true; }
  } else if (wasTalking) { talkOpen = false; faceDirty = true; }
  wasTalking = talkingNow;

  // 長い吹き出しのスクロール用に 100ms ごとに描き直す (v1.5.8)
  if (bubbleText.length() && faceCanvasOk && now - lastScrollTick >= 100) {
    faceCanvas.setFont(&fonts::lgfxJapanGothic_16);
    if (faceCanvas.textWidth(bubbleText) > 284) { lastScrollTick = now; faceDirty = true; }
  }

  // ボタン
  if (M5.BtnA.wasPressed()) { btnCount++; fireEvent("on_button_a"); }
  if (M5.BtnB.wasPressed()) { btnCount++; fireEvent("on_button_b"); }
  // ボタンC: 短く押して離すと音量サイクル (240→180→120→60→0ミュート, v1.5.6)
  if (M5.BtnC.wasPressed()) { btnCount++; cDownAt = now; }
  if (M5.BtnC.wasReleased()) {
    if (cDownAt && now - cDownAt < 800 && !M5.BtnB.isPressed()) {
      static const int levels[] = {240, 180, 120, 60, 0};
      int idx = 1;
      for (int i = 0; i < 5; i++) if (levels[i] == cfgVolume) idx = i;
      applyVolume(levels[(idx + 1) % 5], true);
      if (cfgVolume > 0) beep(cfgVolume >= 180 ? 1568 : 1047, 60);
    }
    cDownAt = 0;
  }
  // ボタンB+C 同時3秒長押し: 設定ポータルで再起動 (v1.5.6)
  // (起動時Aボタン長押しは静電容量の初期較正で外しやすいので、その代替)
  if (M5.BtnB.isPressed() && M5.BtnC.isPressed()) {
    if (!bcDownAt) bcDownAt = now;
    else if (now - bcDownAt >= 3000) {
      prefs.begin("kinako", false);
      prefs.putBool("portal", true);
      prefs.end();
      bubbleText = "せっていモードで さいきどうするね";
      faceDirty = true;
      drawFace();
      delay(600);
      ESP.restart();
    }
  } else bcDownAt = 0;

  // タッチ (短タップ = on_touch / 耳を押している間 = プッシュトゥトーク, v1.5.4)
  // 録音は顔の耳 (画面上のクマ耳) を押したときだけ始める。顔のタップや
  // 長押しは なでなで (on_touch) として扱う。
  {
    auto t = M5.Touch.getDetail(0);
    if (t.wasPressed()) {
      touchDownAt = now;
      pendingTouch = true;
      touchConsumed = false;
      int dx1 = t.x - 74, dy1 = t.y - 50, dx2 = t.x - 246, dy2 = t.y - 50;
      pressOnEar = (dx1 * dx1 + dy1 * dy1 <= 30 * 30) || (dx2 * dx2 + dy2 * dy2 <= 30 * 30);
    }
    if (pendingTouch && !touchConsumed && pressOnEar && t.isPressed() && now - touchDownAt >= 300 && !recording) {
      touchConsumed = true;
      startRecording();
    }
    if (t.wasReleased()) {
      if (recording && !recStopAt) recStopAt = now;  // 離したら送信へ
      if (pendingTouch && !touchConsumed) {
        touchCount++;
        if (bigUntil && now < bigUntil) { bigUntil = 0; faceDirty = true; lastEvent = "notify_dismissed"; }
        else fireEvent("on_touch");
      }
      pendingTouch = false;
    }
  }

  // IMU: シェイク / 傾け
  if (now - lastImu > 100) {
    lastImu = now;
    M5.Imu.update();
    float ax, ay, az;
    M5.Imu.getAccel(&ax, &ay, &az);
    float mag = sqrtf(ax * ax + ay * ay + az * az);
    if (mag > 2.3f && now - lastShakeAt > 3000) {
      lastShakeAt = now;
      shakeCount++;
      fireEvent("on_shake");
    } else if ((ax > 0.75f || ax < -0.75f) && now - lastTiltAt > 5000) {
      lastTiltAt = now;
      fireEvent("on_tilt");
    }
  }

  stepMachine();

  if (!wsConnected && now - lastFeed > FEED_POLL_MS) { lastFeed = now; pollFeed(); }
  if (otaRequested || now - lastOta > OTA_POLL_MS) { otaRequested = false; lastOta = now; pollOta(); }
  if (now - lastTelem > TELEM_POLL_MS) { lastTelem = now; pushTelemetry(); }

  // 録音の進行管理 (チャンク接続・送信・フィードバック, v1.5.2)
  serviceRecording(now);

  if (bigUntil && now > bigUntil) { bigUntil = 0; if (canvasMode && canvasOk) canvas.pushSprite(0, 0); else faceDirty = true; }
  if (ledBarUntil && now > ledBarUntil && !recording) { ledBarUntil = 0; ledBarOff(); }
  if (faceDirty) drawFace();
  delay(10);
}
