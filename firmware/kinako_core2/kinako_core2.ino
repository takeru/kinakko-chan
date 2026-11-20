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
#include <ArduinoJson.h>
#include <WebSocketsClient.h>

#define FW_VERSION "1.4.2"
static const uint32_t FEED_POLL_MS = 10000;
static const uint32_t OTA_POLL_MS = 60000;  // 通常は WS の即時トリガが主経路。これは予備
static const uint32_t TELEM_POLL_MS = 60000;

// ---------------- 設定 (NVS) ----------------
Preferences prefs;
String cfgSsid, cfgPass, cfgFeed, cfgToken, cfgRepo, cfgTelem, cfgKey;
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
bool blinking = false;
bool talkingNow = false, wasTalking = false, talkOpen = false;
uint32_t talkFlipAt = 0;

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
  // ---- きなっこちゃんの顔 (もふもふフード, v1.4.0) ----
  // ユーザーが選んだぬいぐるみの顔を図形で再構成した常時顔。
  // 感情とまばたきは目と口の形で表す (色は写真から採った近似値)。
  uint16_t hoodC  = d.color565(240, 233, 216);  // フード (アイボリー)
  uint16_t hoodS  = d.color565(221, 209, 187);  // フードの影
  uint16_t faceC  = d.color565(247, 217, 190);  // 顔 (ピーチ)
  uint16_t faceS  = d.color565(239, 200, 163);  // 顔の影
  uint16_t cheekC = d.color565(244, 169, 184);  // ほっぺ
  uint16_t darkC  = d.color565(43, 35, 32);     // 目と口
  uint16_t heartC = d.color565(232, 106, 146);  // ハート目

  // フード: 影 → もふもふの縁 (円周上の小円) → 本体
  d.fillEllipse(160, 136, 120, 106, hoodS);
  for (int a = 0; a < 360; a += 18) {
    float r = a * 0.0174533f;
    int fx = 160 + (int)(116 * cosf(r));
    int fy = 130 + (int)(102 * sinf(r));
    d.fillCircle(fx, fy, 13, hoodC);
  }
  d.fillEllipse(160, 130, 116, 102, hoodC);
  // 顔: 影つきの楕円
  d.fillEllipse(160, 123, 64, 54, faceS);
  d.fillEllipse(160, 119, 62, 52, faceC);
  // ほっぺ
  d.fillCircle(100, 139, 12, cheekC);
  d.fillCircle(220, 139, 12, cheekC);

  // 目
  int eyeLX = 134, eyeRX = 186, eyeY = 110;
  int rx = 7, ry = 10;
  if (emotion == "surprise") { rx = 9; ry = 13; }
  else if (emotion == "sad") { ry = 6; eyeY += 2; }
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
    d.drawArc(eyeLX, eyeY + 4, 8, 11, 180, 360, darkC);
    d.drawArc(eyeRX, eyeY + 4, 8, 11, 180, 360, darkC);
  } else if (emotion == "sleep") {
    d.fillEllipse(eyeLX, eyeY, 8, 2, darkC);
    d.fillEllipse(eyeRX, eyeY, 8, 2, darkC);
  } else {
    d.fillEllipse(eyeLX, eyeY, rx, ry, darkC);
    d.fillEllipse(eyeRX, eyeY, (emotion == "think") ? 3 : rx, ry, darkC);
    if (ry > 3) {  // 目のハイライト (つぶらな瞳)
      d.fillCircle(eyeLX - 2, eyeY - 3, 2, TFT_WHITE);
      if (emotion != "think") d.fillCircle(eyeRX - 2, eyeY - 3, 2, TFT_WHITE);
    }
  }

  // 口 (小さめ。再生中は口パクが優先, v1.4.2)
  int mx = 160, my = 140;
  if (talkingNow && emotion != "sleep") {
    if (talkOpen) d.fillEllipse(mx, my + 1, 6, 8, darkC);  // 開いた口
    else d.fillEllipse(mx, my, 6, 2, darkC);               // 閉じた口
  } else if (emotion == "happy" || emotion == "love") {
    d.drawArc(mx, my - 6, 11, 14, 20, 160, darkC);
  } else if (emotion == "sad") {
    d.drawArc(mx, my + 10, 9, 12, 200, 340, darkC);
  } else if (emotion == "surprise") {
    d.fillEllipse(mx, my, 6, 8, darkC);
  } else if (emotion == "sleep") {
    d.fillEllipse(mx, my + 2, 3, 4, darkC);
  } else {
    d.fillEllipse(mx, my, 5, 4, darkC);
  }

  // 吹き出し (セリフ)
  if (bubbleText.length()) {
    d.fillRoundRect(14, 8, 292, 44, 8, d.color565(40, 40, 40));
    d.setFont(&fonts::lgfxJapanGothic_16);
    d.setTextColor(TFT_WHITE);
    d.setTextDatum(textdatum_t::middle_center);
    d.drawString(bubbleText, 160, 30);
    d.setTextDatum(textdatum_t::top_left);
  }
  // 状態行 (フードに埋もれないよう暗いピルを敷く)
  d.fillRoundRect(2, 222, 228, 15, 4, d.color565(28, 24, 20));
  d.setFont(&fonts::Font0);
  d.setTextColor(d.color565(170, 160, 145));
  String st = "KinakoCore2 v" + String(FW_VERSION) + "  seq:" + String(residentSeq) +
              "  batt:" + String(M5.Power.getBatteryLevel()) + "%";
  d.drawString(st, 6, 226);
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

void beep(int freq, int ms) { M5.Speaker.tone(freq, ms); }

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
        "WiFi SSID <input name='ssid' value='" + cfgSsid + "'><br>"
        "WiFi Password <input name='pass' type='password'><br>"
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
    prefs.putString("ssid", web.arg("ssid"));
    if (web.arg("pass").length()) prefs.putString("pass", web.arg("pass"));
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

// ---------------- setup / loop ----------------
void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);
  M5.Display.setBrightness(160);
  M5.Speaker.setVolume(180);
  M5.update();
  bool forcePortal = M5.BtnA.isPressed();

  prefs.begin("kinako", true);
  cfgSsid = prefs.getString("ssid", "");
  cfgPass = prefs.getString("pass", "");
  cfgFeed = prefs.getString("feed", "");
  cfgToken = prefs.getString("token", "");
  cfgRepo = prefs.getString("repo", "");
  cfgTelem = prefs.getString("telem", "");
  cfgKey = prefs.getString("key", "");
  prefs.end();

  emotion = "think";
  drawFace();

  if (forcePortal || !cfgSsid.length()) {
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

  WiFi.mode(WIFI_STA);
  WiFi.begin(cfgSsid.c_str(), cfgPass.c_str());
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) delay(250);
  if (WiFi.status() != WL_CONNECTED) {  // つながらなければ設定APへ
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

uint32_t lastFeed = 0, lastOta = 0, lastTelem = 0, lastBlink = 0, lastImu = 0;

void loop() {
  M5.update();
  web.handleClient();
  if (apMode) { dns.processNextRequest(); if (faceDirty) drawFace(); delay(10); return; }

  wsClient.loop();
  uint32_t now = millis();

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

  // ボタン
  if (M5.BtnA.wasPressed()) { btnCount++; fireEvent("on_button_a"); }
  if (M5.BtnB.wasPressed()) { btnCount++; fireEvent("on_button_b"); }
  if (M5.BtnC.wasPressed()) { btnCount++; fireEvent("on_button_c"); }

  // タッチ
  if (M5.Touch.getCount() > 0) {
    auto t = M5.Touch.getDetail(0);
    if (t.wasPressed()) {
      touchCount++;
      if (bigUntil && now < bigUntil) { bigUntil = 0; faceDirty = true; lastEvent = "notify_dismissed"; }
      else fireEvent("on_touch");
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

  if (bigUntil && now > bigUntil) { bigUntil = 0; if (canvasMode && canvasOk) canvas.pushSprite(0, 0); else faceDirty = true; }
  if (faceDirty) drawFace();
  delay(10);
}
