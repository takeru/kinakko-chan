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
 * 必要ライブラリ: M5Unified, M5GFX (M5Unifiedの依存), ArduinoJson v7
 * ボード: M5Stack Core2 (m5stack:esp32:m5stack-core2), OTA用パーティション
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

#define FW_VERSION "1.2.1"
static const uint32_t FEED_POLL_MS = 10000;
static const uint32_t OTA_POLL_MS = 600000;
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
uint32_t blinkAt = 0;
bool blinking = false;

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
  auto& d = M5.Display;
  uint32_t now = millis();
  if (now < bigUntil) {  // 通知の全画面表示
    d.fillScreen(d.color888(24, 60, 120));
    d.setFont(&fonts::lgfxJapanGothic_24);
    d.setTextColor(TFT_WHITE);
    d.setTextDatum(textdatum_t::middle_center);
    d.drawString(bigTitle, 160, 90);
    d.setFont(&fonts::lgfxJapanGothic_16);
    d.drawString(bigBody, 160, 140);
    d.setTextDatum(textdatum_t::top_left);
    faceDirty = false;
    return;
  }
  d.fillScreen(TFT_BLACK);
  uint16_t white = TFT_WHITE;
  int eyeY = 105, eyeLX = 105, eyeRX = 215;
  int ry = 26, rx = 15;
  if (emotion == "sleep") ry = 3;
  else if (emotion == "surprise") { ry = 34; rx = 19; }
  else if (emotion == "sad") ry = 16;
  else if (emotion == "happy") ry = 10;
  if (blinking && emotion != "sleep") ry = 3;

  if (emotion == "love") {
    for (int cx : {eyeLX, eyeRX}) {
      d.fillCircle(cx - 8, eyeY - 8, 9, white);
      d.fillCircle(cx + 8, eyeY - 8, 9, white);
      d.fillTriangle(cx - 16, eyeY - 4, cx + 16, eyeY - 4, cx, eyeY + 18, white);
    }
  } else if (emotion == "dizzy") {
    for (int cx : {eyeLX, eyeRX}) {
      d.drawLine(cx - 12, eyeY - 12, cx + 12, eyeY + 12, white);
      d.drawLine(cx - 12, eyeY + 12, cx + 12, eyeY - 12, white);
      d.drawLine(cx - 11, eyeY - 12, cx + 13, eyeY + 12, white);
      d.drawLine(cx - 11, eyeY + 12, cx + 13, eyeY - 12, white);
    }
  } else if (emotion == "happy") {
    // 逆U字の目
    d.drawArc(eyeLX, eyeY + 6, 13, 16, 180, 360, white);
    d.drawArc(eyeRX, eyeY + 6, 13, 16, 180, 360, white);
  } else {
    d.fillEllipse(eyeLX, eyeY, rx, ry, white);
    d.fillEllipse(eyeRX, eyeY, (emotion == "think") ? 3 : rx, ry, white);
  }

  // 口
  int mx = 160, my = 168;
  if (emotion == "happy" || emotion == "love") {
    d.drawArc(mx, my - 8, 22, 26, 20, 160, white);
  } else if (emotion == "sad") {
    d.drawArc(mx, my + 14, 20, 24, 200, 340, white);
  } else if (emotion == "surprise") {
    d.fillEllipse(mx, my, 12, 16, white);
  } else if (emotion == "sleep") {
    d.fillEllipse(mx, my, 6, 8, white);
  } else {
    d.fillRoundRect(mx - 24, my - 3, 48, 6, 3, white);
  }

  // 吹き出し (セリフ)
  if (bubbleText.length()) {
    d.fillRoundRect(14, 8, 292, 44, 8, d.color888(40, 40, 40));
    d.setFont(&fonts::lgfxJapanGothic_16);
    d.setTextColor(TFT_WHITE);
    d.setTextDatum(textdatum_t::middle_center);
    d.drawString(bubbleText, 160, 30);
    d.setTextDatum(textdatum_t::top_left);
  }
  // 状態行
  d.setFont(&fonts::Font0);
  d.setTextColor(d.color888(90, 90, 90));
  String st = "KinakoCore2 v" + String(FW_VERSION) + "  seq:" + String(residentSeq) +
              "  batt:" + String(M5.Power.getBatteryLevel()) + "%";
  d.drawString(st, 6, 226);
  faceDirty = false;
}

// ---------------- 音 ----------------
bool playWavFromUrl(const String& url) {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  if (!http.begin(client, url)) return false;
  if (cfgToken.length()) http.addHeader("Authorization", "Bearer " + cfgToken);
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
    emotion = (const char*)(s["emotion"] | "normal");
    faceDirty = true;
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
    httpUpdate.update(client, bin);  // 成功時はここで再起動する
  }
}

String telemetryJson() {
  float ax = 0, ay = 0, az = 0, gx = 0, gy = 0, gz = 0;
  M5.Imu.update();
  M5.Imu.getAccel(&ax, &ay, &az);
  M5.Imu.getGyro(&gx, &gy, &gz);
  JsonDocument doc;
  doc["seq"] = residentSeq;
  doc["emotion"] = emotion;
  doc["battery"] = M5.Power.getBatteryLevel();
  doc["voltage_mv"] = M5.Power.getBatteryVoltage();
  doc["charging"] = M5.Power.isCharging();
  doc["rssi"] = WiFi.RSSI();
  doc["heap"] = ESP.getFreeHeap();
  doc["uptime_s"] = millis() / 1000;
  doc["accel"] = serialized(String(ax, 2) + "," + String(ay, 2) + "," + String(az, 2));
  doc["gyro"] = serialized(String(gx, 1) + "," + String(gy, 1) + "," + String(gz, 1));
  doc["touch_count"] = touchCount;
  doc["shake_count"] = shakeCount;
  doc["button_count"] = btnCount;
  doc["last_event"] = lastEvent;
  String out;
  serializeJson(doc, out);
  return out;
}

void pushTelemetry() {
  if (wsConnected) {
    wsClient.sendTXT("{\"type\":\"telemetry\",\"data\":" + telemetryJson() + "}");
    return;
  }
  if (cfgTelem.length()) {  // 汎用エンドポイント (Cloudflare Worker 等) へPOST
    WiFiClientSecure c; c.setInsecure();
    HTTPClient h;
    if (h.begin(c, cfgTelem)) {
      h.addHeader("Content-Type", "application/json");
      if (cfgKey.length()) h.addHeader("X-Device-Key", cfgKey);
      h.POST(telemetryJson());
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
  if (now - lastOta > OTA_POLL_MS) { lastOta = now; pollOta(); }
  if (now - lastTelem > TELEM_POLL_MS) { lastTelem = now; pushTelemetry(); }

  if (bigUntil && now > bigUntil) { bigUntil = 0; faceDirty = true; }
  if (faceDirty) drawFace();
  delay(10);
}
