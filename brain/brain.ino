/*
 * Lantern Brain — 3-Sensor Directional Wave System
 * Hardware: ESP32 DevKit
 *
 * Wiring:
 *  - PIR Front   OUT --> GPIO 27
 *  - PIR Middle  OUT --> GPIO 25
 *  - PIR Back    OUT --> GPIO 26
 *  - All PIR     VCC --> 5V, GND --> GND
 *
 * Web UI: http://<brain-ip>/  — configure effects, WLED host, and timing
 * OTA:    Arduino IDE Network Port, POST /update, or POST /update/url (GitHub)
 * Log:    USB Serial Monitor @ 115200  AND  GET /log (same stream)
 *         GET /log?after=SEQ  GET /log?clear=1  POST /log/clear
 */

#ifndef FW_URL_DEFAULT
#define FW_URL_DEFAULT "https://github.com/robertleelittleiii/lantern-project/releases/latest/download/brain.ino.bin"
#endif
#ifndef FW_VERSION_URL
#define FW_VERSION_URL "https://github.com/robertleelittleiii/lantern-project/releases/latest/download/version.json"
#endif

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include <Update.h>
#include <HTTPUpdate.h>
#include <WiFiClientSecure.h>
#include <time.h>
#include <math.h>
#include <ctype.h>
#include "config.h"
#include "version.h"

// ── Fixed Settings ───────────────────────────────────────────────────────────
const int PIR_FRONT  = 27;
const int PIR_MIDDLE = 25;
const int PIR_BACK   = 26;
#define MAX_SEG 16
#define SETTINGS_VER 4   // increment when default values change (v4: psAmbient/psMotion)
int       numSegments = 0;  // auto-detected from WLED at startup

// ── Tunable Settings (changeable via web UI) ─────────────────────────────────
int           briBright    = 255;
int           briAmbient   = 255;
int           fxBright     = 0;      // Solid
int           fxAmbient    = 66;     // Fire 2012
int           palAmbient   = 35;     // Fire palette (35=Fire 8=Lava 36=Icefire 0=Default)
int           colBrightR   = 255;    // Motion color R (default warm orange)
int           colBrightG   = 150;    // Motion color G
int           colBrightB   = 0;      // Motion color B
bool          colBrightEnable = false; // Send color override on motion activation
unsigned long motionTimeout = 10000; // ms
int           waveDelay    = 500;    // ms between segment changes
int           sensorMidSeg = 0;      // middle sensor segment (0 = auto: numSegments/2)
int           psAmbient    = 0;      // WLED preset for ambient (0 = legacy fx/pal painting)
int           psMotion     = 0;      // WLED preset for motion (0 = same look as ambient, brightness only)

// ── Schedule Settings ─────────────────────────────────────────────────────
int           scheduleMode  = 0;     // 0=always on, 1=sunset-to-sunrise, 2=custom hours
float         schedLat      = 0.0f;  // latitude (decimal degrees, e.g. 40.71)
float         schedLon      = 0.0f;  // longitude (decimal degrees, e.g. -74.00)
int           tzOffsetMin   = 0;     // UTC offset in minutes (e.g. -300 for EST)
int           schedOnHour   = 18;    // custom window start hour
int           schedOnMin    = 0;     // custom window start minute
int           schedOffHour  = 6;     // custom window end hour
int           schedOffMin   = 0;     // custom window end minute

// ── State ─────────────────────────────────────────────────────
enum SensorId { S_NONE, S_FRONT, S_MIDDLE, S_BACK };
bool  motionActive    = false;
unsigned long lastAnyMotionTime = 0;
SensorId lastSensor   = S_NONE;       // last sensor that triggered
bool segBright[MAX_SEG];              // per-segment bright/ambient state
int  litOrder[MAX_SEG];               // order segments were lit (for trailing dim)
int  litCount         = 0;
bool prevFront = false, prevMiddle = false, prevBack = false;
int  savedColR[MAX_SEG];             // per-segment colors saved before override
int  savedColG[MAX_SEG];
int  savedColB[MAX_SEG];
bool colsSaved     = false;          // true once a snapshot has been taken

// ── Schedule state ─────────────────────────────────────────────────────────
bool          ntpSynced      = false; // true after first successful NTP sync
unsigned long lastNtpSync    = 0;    // millis() of last sync (for daily resync)
bool          schedWasActive = true; // previous schedule state (for edge detection)

// ── Simulator state ───────────────────────────────────────────────────────────
bool simMode        = false;  // blocks real sensor reads; injected values used instead
int  simFrontPulse  = 0, simMiddlePulse = 0, simBackPulse = 0; // loop-cycle countdowns
bool simFrontHold   = false, simMiddleHold = false, simBackHold = false; // persistent HIGH

// ── Dim state machine (bright waves use the concurrent pool below) ──────────────
enum WaveState { WAVE_IDLE, WAVE_DIM_ORDER, WAVE_APPLY_AMBIENT };
WaveState    waveState    = WAVE_IDLE;
unsigned long waveNextMs  = 0;
int  waveOrdIdx = 0;   // DIM_ORDER: cursor into litOrder[]
int  waveCurr   = 0;   // APPLY_AMBIENT: ambient sweep index
bool dimDone    = false;  // signals loop() to reset motionActive

// ── Concurrent bright-wave pool (up to MAX_WAVE_FRONTS people simultaneously) ──────
#define MAX_WAVE_FRONTS 4
struct WaveFront {
  bool  active;
  bool  fromMid;        // true = outward-from-center, false = linear
  int   curr, dest;     // linear: current position / destination
  int   stepDir;        // linear: +1 or -1
  int   midC, midOff;   // fromMid: center segment, expansion offset
  unsigned long nextMs; // earliest time to advance this front
};
WaveFront wavePool[MAX_WAVE_FRONTS];

Preferences  prefs;
WebServer server(80);

// ── Debug log: USB Serial (115200) AND remote GET /log (same bytes) ─────────
#define LOG_RING 6144
static char     logRing[LOG_RING];
static size_t   logHead = 0;       // next write index
static size_t   logFilled = 0;     // bytes currently stored
static uint32_t logSeq = 0;        // total bytes ever written
static bool     logAtLineStart = true;

void logToRing(uint8_t c) {
  if (logAtLineStart && c != '\n' && c != '\r') {
    char ts[16];
    int n = snprintf(ts, sizeof(ts), "%lu ", (unsigned long)millis());
    for (int i = 0; i < n; i++) {
      logRing[logHead] = ts[i];
      logHead = (logHead + 1) % LOG_RING;
      if (logFilled < LOG_RING) logFilled++;
      logSeq++;
    }
    logAtLineStart = false;
  }
  if (c == '\n') logAtLineStart = true;
  logRing[logHead] = (char)c;
  logHead = (logHead + 1) % LOG_RING;
  if (logFilled < LOG_RING) logFilled++;
  logSeq++;
}

// Always write UART (USB-serial chip) and the RAM ring used by GET /log.
void logEmit(const uint8_t* buf, size_t n) {
  if (n == 0) return;
  Serial.write(buf, n);                 // USB Serial Monitor when a host is attached
  for (size_t i = 0; i < n; i++) logToRing(buf[i]);
}

class LogTee : public Print {
public:
  size_t write(uint8_t c) override {
    logEmit(&c, 1);
    return 1;
  }
  size_t write(const uint8_t* buf, size_t n) override {
    logEmit(buf, n);
    return n;
  }
} Log;

void logCopy(String& out, uint32_t after) {
  uint32_t oldest = logSeq - logFilled;  // seq just before oldest stored byte
  uint32_t from = after;
  if (from < oldest) from = oldest;
  if (from >= logSeq) return;
  size_t skip = (size_t)(from - oldest);
  size_t n = logFilled - skip;
  size_t start = (logHead + LOG_RING - logFilled + skip) % LOG_RING;
  out.reserve(out.length() + n);
  for (size_t i = 0; i < n; i++) out += logRing[(start + i) % LOG_RING];
}

void logClear() {
  logHead = 0;
  logFilled = 0;
  logAtLineStart = true;
}

void handleLogGet() {
  uint32_t after = 0;
  if (server.hasArg("after")) after = (uint32_t)strtoul(server.arg("after").c_str(), nullptr, 10);
  bool doClear = server.hasArg("clear") && server.arg("clear") != "0";
  String body;
  logCopy(body, after);
  server.sendHeader("X-Log-Seq", String(logSeq));
  server.sendHeader("X-Log-Size", String(logFilled));
  server.sendHeader("Access-Control-Expose-Headers", "X-Log-Seq, X-Log-Size");
  server.send(200, "text/plain; charset=utf-8", body);
  if (doClear) logClear();
}

void handleLogClear() {
  logClear();
  server.send(200, "application/json", "{\"success\":true}");
}

void attachLogRoutes() {
  server.on("/log", HTTP_GET, handleLogGet);
  server.on("/log", HTTP_DELETE, handleLogClear);
  server.on("/log/clear", HTTP_POST, handleLogClear);
}

bool   otaInProgress = false;
uint32_t otaBytes = 0, otaTotal = 0;
const char* otaPhase = "idle";
char otaUrlBuf[256];
char otaErrBuf[96];

// ESP32 has no GC. Bound buffers + this watchdog keep long uptimes from dying on heap.
#define HEAP_RESTART_MIN 16384
void maintainMemory() {
  uint32_t heap = ESP.getFreeHeap();
  static unsigned long lastHeapLog = 0;
  if (lastHeapLog == 0 || millis() - lastHeapLog > 3600000UL) {
    lastHeapLog = millis();
    Log.printf("heap %lu free, min %lu, maxalloc %lu\n",
               (unsigned long)heap,
               (unsigned long)ESP.getMinFreeHeap(),
               (unsigned long)ESP.getMaxAllocHeap());
  }
  if (!otaInProgress && heap < HEAP_RESTART_MIN) {
    Log.printf("heap low (%lu) — restarting\n", (unsigned long)heap);
    delay(300);
    ESP.restart();
  }
}

void firmwareOtaTask(void*) {
  WiFi.setSleep(false);
  Log.println("URL OTA start");
  otaPhase = "download";
  WiFiClientSecure client;
  client.setInsecure();
  HTTPUpdate updater(180000);
  updater.rebootOnUpdate(false);
  updater.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  updater.onProgress([](int cur, int tot) {
    otaBytes = (uint32_t)cur;
    otaTotal = (uint32_t)tot;
    otaPhase = "flash";
  });
  t_httpUpdate_return ret = updater.update(client, String(otaUrlBuf));
  if (ret == HTTP_UPDATE_OK) {
    otaPhase = "reboot";
    if (otaTotal) otaBytes = otaTotal;
    Log.println("URL OTA OK — rebooting");
    delay(800);
    ESP.restart();
  } else {
    strncpy(otaErrBuf, updater.getLastErrorString().c_str(), sizeof(otaErrBuf) - 1);
    otaErrBuf[sizeof(otaErrBuf) - 1] = 0;
    otaPhase = "error";
    otaInProgress = false;
    Log.printf("URL OTA failed: %s\n", otaErrBuf);
  }
  vTaskDelete(NULL);
}

void attachHttpLogMiddleware() {
  server.addMiddleware([](WebServer& s, Middleware::Callback next) {
    String uri = s.uri();
    if (uri == "/status" || uri == "/schedule" || uri == "/log" ||
        uri == "/update/check" || uri == "/update/progress" || uri == "/geo") return next();
    uint32_t t0 = millis();
    bool ok = next();
    Log.printf("HTTP %s %s -> %d %lums\n",
               http_method_str(s.method()), uri.c_str(),
               s.responseCode(), (unsigned long)(millis() - t0));
    return ok;
  });
}

// ── Runtime WiFi credentials (NVS-overridable; fall back to config.h) ─────────
String wifiSSID;
String wifiPass;

// ── Runtime WLED target (NVS-overridable; fall back to config.h WLED_IP) ──────
String wledIP;
String wledName;
String deviceTag;       // display name, e.g. "Walkway"

#define MAX_WLED_HOSTS 12
struct WledHost {
  char ip[16];
  char name[32];
  char ver[16];
  char source[8];
  int  leds;
};
WledHost wledFound[MAX_WLED_HOSTS];
int      wledFoundCount = 0;

// ── Helpers ───────────────────────────────────────────────────────────────────
void ensureWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  Log.print("Reconnecting to WiFi");
  WiFi.disconnect();
  WiFi.begin(wifiSSID, wifiPass);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
    delay(500);
    Log.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Log.println("\nReconnected: " + WiFi.localIP().toString());
  } else {
    Log.println("\nWiFi reconnect failed — will retry next cycle");
  }
}

// Set brightness, effect, optional palette, and optional color on a single WLED segment.
// Pass pal=-1 to leave palette unchanged. Pass colR=-1 to leave color unchanged.
void setSegment(int segId, int bri, int fx, int pal = -1,
                int colR = -1, int colG = -1, int colB = -1) {
  ensureWiFi();
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  char url[64];
  snprintf(url, sizeof(url), "http://%s/json/state", wledIP.c_str());

  StaticJsonDocument<384> doc;
  JsonArray seg = doc.createNestedArray("seg");
  JsonObject s = seg.createNestedObject();
  s["id"] = segId;
  s["bri"] = bri;
  s["fx"] = fx;
  if (pal >= 0) s["pal"] = pal;
  if (colR >= 0) {
    JsonArray colArr = s.createNestedArray("col");
    JsonArray primary = colArr.createNestedArray();
    primary.add(colR); primary.add(colG); primary.add(colB);
  }
  char body[320];
  size_t n = serializeJson(doc, body, sizeof(body));

  http.begin(url);
  http.setTimeout(5000);
  http.addHeader("Content-Type", "application/json");
  int code = http.POST((uint8_t*)body, n);

  if (code > 0) {
    Log.printf("Seg %d → bri %d fx %d pal %d (HTTP %d)\n", segId, bri, fx, pal, code);
  } else {
    Log.printf("WLED POST failed: %s\n", http.errorToString(code).c_str());
  }
  http.end();
  delay(10);
}

String psAmbientName = "", psMotionName = "";  // display names (IDs are what's stored)
String presetLabel(int id, const String& name);
// ── WLED preset helpers ─────────────────────────────────────────────────────
// POST a raw JSON body to WLED /json/state. Response is consumed before end().
bool wledPostState(const char* body, size_t n) {
  ensureWiFi();
  if (WiFi.status() != WL_CONNECTED) return false;
  HTTPClient http;
  char url[64];
  snprintf(url, sizeof(url), "http://%s/json/state", wledIP.c_str());
  http.begin(url);
  http.setTimeout(5000);
  http.addHeader("Content-Type", "application/json");
  int code = http.POST((uint8_t*)body, n);
  if (code > 0) http.getString();  // drain
  else Log.printf("WLED POST failed: %s\n", http.errorToString(code).c_str());
  http.end();
  delay(10);
  return code == 200;
}

// Load a WLED preset with ONE call. WLED applies presets in its own loop, so give it a moment.
void applyPreset(int id) {
  if (id <= 0) return;
  char body[32];
  size_t n = snprintf(body, sizeof(body), "{\"ps\":%d}", id);
  bool ok = wledPostState(body, n);
  const String& nm = (id == psAmbient) ? psAmbientName : (id == psMotion ? psMotionName : String(""));
  Log.printf("Preset %s loaded (%s)\n", presetLabel(id, nm).c_str(), ok ? "ok" : "fail");
  delay(150);
}

// Brightness-only write (optionally with a color override); keeps the preset's fx/pal/col.
void setSegBri(int seg, int bri, int r = -1, int g = -1, int b = -1) {
  char body[96];
  size_t n;
  if (r >= 0) n = snprintf(body, sizeof(body), "{\"seg\":[{\"id\":%d,\"bri\":%d,\"col\":[[%d,%d,%d]]}]}", seg, bri, r, g, b);
  else        n = snprintf(body, sizeof(body), "{\"seg\":[{\"id\":%d,\"bri\":%d}]}", seg, bri);
  wledPostState(body, n);
  Log.printf("Seg %d → bri %d (preset mode)\n", seg, bri);
}

// One call that sets the brightness of every segment.
void setAllSegBri(int bri) {
  char body[16 + MAX_SEG * 24];
  size_t n = snprintf(body, sizeof(body), "{\"seg\":[");
  for (int i = 0; i < numSegments && i < MAX_SEG; i++)
    n += snprintf(body + n, sizeof(body) - n, "%s{\"id\":%d,\"bri\":%d}", i ? "," : "", i, bri);
  n += snprintf(body + n, sizeof(body) - n, "]}");
  wledPostState(body, n);
}


String presetLabel(int id, const String& name) {
  if (id <= 0) return String("none");
  if (name.length() == 0) return String("Preset ") + id + " (missing)";
  return String(id) + " \u00B7 " + name;
}

// Fetch WLED /presets.json (streamed through a name-only filter) into out["presets"]=[{id,name}].
// Also refreshes the display names of psAmbient/psMotion. Returns HTTP-ish status.
int fetchPresetList(JsonDocument& out) {
  ensureWiFi();
  if (WiFi.status() != WL_CONNECTED) return 502;
  HTTPClient http;
  char url[64];
  snprintf(url, sizeof(url), "http://%s/presets.json", wledIP.c_str());
  http.begin(url);
  http.setTimeout(5000);
  int code = http.GET();
  if (code != 200) {
    if (code > 0) http.getString();
    http.end();
    return 502;
  }
  StaticJsonDocument<64> filter;
  filter["*"]["n"] = true;               // keep only names; presets.json can be large
  DynamicJsonDocument doc(3072);
  DeserializationError err = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  if (err) { Log.printf("presets.json parse: %s\n", err.c_str()); return 500; }
  JsonArray arr = out.createNestedArray("presets");
  String na = "", nm2 = "";
  for (JsonPair kv : doc.as<JsonObject>()) {
    int id = atoi(kv.key().c_str());
    if (id <= 0) continue;               // "0" is WLED's placeholder
    String nm = kv.value()["n"] | "";
    nm.trim();
    if (nm.length() == 0) nm = String("Preset ") + id;
    JsonObject o = arr.createNestedObject();
    o["id"] = id;
    o["name"] = nm;
    if (id == psAmbient) na = nm;
    if (id == psMotion)  nm2 = nm;
  }
  psAmbientName = na; psMotionName = nm2;
  return 200;
}

void refreshPresetNames() {
  if (psAmbient <= 0 && psMotion <= 0) return;
  DynamicJsonDocument tmp(3072);
  fetchPresetList(tmp);
  Log.printf("Presets: ambient=%s motion=%s\n", presetLabel(psAmbient, psAmbientName).c_str(),
             presetLabel(psMotion, psMotionName).c_str());
}

bool motionPresetMode() { return psAmbient > 0 || psMotion > 0; }
bool motionSwapsPreset() { return psMotion > 0 && psMotion != psAmbient; }

// Called once when a fresh motion episode starts: snap to the motion preset if it differs.
void startMotionLook() {
  if (!motionSwapsPreset()) return;
  applyPreset(psMotion);
  // Preset loads every segment lit; blank them so the sensor wave (lightSeg) reveals
  // lanterns one by one in its direction instead of all at once.
  setAllSegBri(0);
}

// ── Directional wave helpers ─────────────────────────────────────────────────

// Light a single segment and record the order
void lightSeg(int seg) {
  if (seg < 0 || seg >= numSegments || segBright[seg]) return;
  if (litCount >= MAX_SEG) return;  // bounds guard — prevents litOrder[] overflow
  if (motionPresetMode()) {
    // Preset mode: brightness only, preset colors/effect survive (override only if explicitly on)
    if (colBrightEnable) setSegBri(seg, briBright, colBrightR, colBrightG, colBrightB);
    else                 setSegBri(seg, briBright);
  } else if (colBrightEnable)
    setSegment(seg, briBright, fxBright, -1, colBrightR, colBrightG, colBrightB);
  else
    // No override: send neutral warm white so any stale override color in WLED
    // is cleared. WLED's effect/palette still controls final appearance.
    setSegment(seg, briBright, fxBright, -1, 255, 248, 247);
  segBright[seg] = true;
  litOrder[litCount++] = seg;
}

// Dim a single segment
void dimSeg(int seg) {
  if (seg < 0 || seg >= numSegments || !segBright[seg]) return;
  if (motionPresetMode()) setSegBri(seg, briAmbient);
  else setSegment(seg, briAmbient, fxAmbient, palAmbient);
  segBright[seg] = false;
}

// ── Wave pool helpers ────────────────────────────────────────────────────────
bool allWavesDone() {
  for (int i = 0; i < MAX_WAVE_FRONTS; i++)
    if (wavePool[i].active) return false;
  return true;
}
void clearWavePool() {
  for (int i = 0; i < MAX_WAVE_FRONTS; i++) wavePool[i].active = false;
}
// Returns a free pool slot; if full, evicts the front closest to its destination.
int allocWaveFront() {
  for (int i = 0; i < MAX_WAVE_FRONTS; i++)
    if (!wavePool[i].active) return i;
  int best = 0, bestDist = numSegments + 1;
  for (int i = 0; i < MAX_WAVE_FRONTS; i++) {
    if (!wavePool[i].fromMid) {
      int d = abs(wavePool[i].dest - wavePool[i].curr);
      if (d < bestDist) { best = i; bestDist = d; }
    }
  }
  wavePool[best].active = false;
  return best;
}
void startPoolWaveBright(int from, int to) {
  int i = allocWaveFront();
  wavePool[i] = {true, false, from, to, (from<=to)?1:-1, 0, 0, 0};
}
void startPoolWaveBrightFromMid(int mid) {
  int i = allocWaveFront();
  wavePool[i] = {true, true, 0, 0, 0, mid, 0, 0};
}
void startDimInOrder() {
  waveOrdIdx = 0;
  waveState  = WAVE_DIM_ORDER;
  waveNextMs = 0;
}

// Advance the FIRST ready bright-wave pool front (one HTTP call per tick).
void tickWaves() {
  for (int i = 0; i < MAX_WAVE_FRONTS; i++) {
    WaveFront& w = wavePool[i];
    if (!w.active) continue;
    if ((long)(millis() - w.nextMs) < 0) continue;

    if (w.fromMid) {
      int left  = w.midC - 1 - w.midOff;
      int right = w.midC + w.midOff;
      if (left  >= 0)           lightSeg(left);
      if (right < numSegments)  lightSeg(right);
      w.midOff++;
      bool more = (w.midC - 1 - w.midOff >= 0) || (w.midC + w.midOff < numSegments);
      if (!more) w.active = false;
      else       w.nextMs = millis() + waveDelay;
    } else {
      lightSeg(w.curr);
      if (w.curr == w.dest) {
        w.active = false;
      } else {
        w.curr  += w.stepDir;
        w.nextMs = millis() + waveDelay;
      }
    }
    return;  // one HTTP call per loop() tick
  }
}

// Advance the dim state machine one step (WAVE_DIM_ORDER → WAVE_APPLY_AMBIENT).
void tickDim() {
  if (waveState == WAVE_IDLE) return;
  if ((long)(millis() - waveNextMs) < 0) return;

  switch (waveState) {
    case WAVE_DIM_ORDER:
      while (waveOrdIdx < litCount && !segBright[litOrder[waveOrdIdx]]) waveOrdIdx++;
      if (waveOrdIdx >= litCount) {
        litCount   = 0;
        waveCurr   = 0;
        waveState  = WAVE_APPLY_AMBIENT;
        waveNextMs = 0;
      } else {
        dimSeg(litOrder[waveOrdIdx]);
        waveOrdIdx++;
        waveNextMs = millis() + waveDelay;
      }
      break;

    case WAVE_APPLY_AMBIENT:
      if (psAmbient > 0) {
        // Preset mode: return to the ambient preset/level in one go
        if (motionSwapsPreset()) applyPreset(psAmbient);
        setAllSegBri(briAmbient);
        for (int i = 0; i < numSegments; i++) segBright[i] = false;
        litCount = 0; waveState = WAVE_IDLE; dimDone = true;
        break;
      }
      setSegment(waveCurr, briAmbient, fxAmbient, palAmbient);
      segBright[waveCurr] = false;
      waveCurr++;
      if (waveCurr >= numSegments) {
        litCount  = 0;
        waveState = WAVE_IDLE;
        dimDone   = true;  // signals loop() to reset motionActive + lastSensor
      } else {
        waveNextMs = millis();  // process next segment immediately
      }
      break;

    default:
      waveState = WAVE_IDLE;
  }
}

void applyAmbient() {
  clearWavePool();
  if (psAmbient > 0) {
    applyPreset(psAmbient);
    setAllSegBri(briAmbient);  // preset brightness is explicit: always apply briAmbient
    for (int i = 0; i < numSegments; i++) segBright[i] = false;
    litCount = 0;
    return;
  }
  for (int i = 0; i < numSegments; i++) {
    setSegment(i, briAmbient, fxAmbient, palAmbient);
    segBright[i] = false;
  }
  litCount = 0;
}

// Snapshot the primary color of every WLED segment before enabling color override.
void saveSegmentColors() {
  ensureWiFi();
  if (WiFi.status() != WL_CONNECTED) return;
  HTTPClient http;
  char url[64];
  snprintf(url, sizeof(url), "http://%s/json/state", wledIP.c_str());
  http.begin(url);
  http.setTimeout(5000);
  int code = http.GET();
  if (code == 200) {
    String body = http.getString();
    StaticJsonDocument<4096> doc;
    if (!deserializeJson(doc, body)) {
      int idx = 0;
      for (JsonObject s : doc["seg"].as<JsonArray>()) {
        if (idx >= numSegments || idx >= MAX_SEG) break;
        JsonArray col = s["col"].as<JsonArray>();
        if (col.size() > 0) {
          JsonArray p = col[0].as<JsonArray>();
          savedColR[idx] = p[0] | 0;
          savedColG[idx] = p[1] | 0;
          savedColB[idx] = p[2] | 0;
        } else {
          savedColR[idx] = 255; savedColG[idx] = 255; savedColB[idx] = 255;
        }
        idx++;
      }
      colsSaved = true;
      Log.printf("Saved colors for %d segments\n", idx);
    }
  }
  http.end();
}

// Restore the snapshotted colors to every segment (ambient effect/palette stays).
void restoreSegmentColors() {
  if (!colsSaved) { applyAmbient(); return; }
  for (int i = 0; i < numSegments; i++) {
    setSegment(i, briAmbient, fxAmbient, palAmbient, savedColR[i], savedColG[i], savedColB[i]);
    segBright[i] = false;
  }
  litCount = 0;
  Log.println("Segment colors restored");
}

// ── NTP sync ────────────────────────────────────────────────────────
void syncNTP() {
  configTime((long)tzOffsetMin * 60L, 0, "pool.ntp.org", "time.nist.gov");
  struct tm ti;
  unsigned long start = millis();
  while (!getLocalTime(&ti) && millis() - start < 5000) delay(200);
  if (getLocalTime(&ti)) {
    ntpSynced   = true;
    lastNtpSync = millis();
    Log.printf("NTP sync OK: %02d:%02d:%02d\n", ti.tm_hour, ti.tm_min, ti.tm_sec);
  } else {
    Log.println("NTP sync failed — will retry");
  }
}

// ── Sunrise/sunset ────────────────────────────────────────────────────────
// NOAA simplified algorithm.  Returns local decimal hours (e.g. 6.5 = 06:30).
// Returns -1.0 on polar day/night or invalid coordinates.
float calcSunEvent(float lat, float lon, int dayOfYear, int tzOffMin, bool isSunrise) {
  const float D2R = (float)M_PI / 180.0f;
  const float R2D = 180.0f / (float)M_PI;
  const float ZEN = 90.833f;  // official zenith (accounts for refraction)

  float lngHour = lon / 15.0f;
  float t = isSunrise ? dayOfYear + (6.0f  - lngHour) / 24.0f
                      : dayOfYear + (18.0f - lngHour) / 24.0f;

  float M = (0.9856f * t) - 3.289f;
  float L = M + (1.916f * sinf(M * D2R)) + (0.020f * sinf(2.0f * M * D2R)) + 282.634f;
  while (L <   0) L += 360.0f;
  while (L >= 360) L -= 360.0f;

  float RA = R2D * atanf(0.91764f * tanf(L * D2R));
  while (RA <   0) RA += 360.0f;
  while (RA >= 360) RA -= 360.0f;
  RA = (RA + (floorf(L / 90.0f) * 90.0f - floorf(RA / 90.0f) * 90.0f)) / 15.0f;

  float sinDec = 0.39782f * sinf(L * D2R);
  float cosDec = cosf(asinf(sinDec));
  float cosH   = (cosf(ZEN * D2R) - sinDec * sinf(lat * D2R)) / (cosDec * cosf(lat * D2R));
  if (cosH >  1.0f) return -1.0f;
  if (cosH < -1.0f) return -1.0f;

  float H  = isSunrise ? (360.0f - R2D * acosf(cosH)) / 15.0f
                       :           (R2D * acosf(cosH))  / 15.0f;
  float T  = H + RA - (0.06571f * t) - 6.622f;
  float UT = T - lngHour;
  while (UT <  0)  UT += 24.0f;
  while (UT >= 24) UT -= 24.0f;
  float local = UT + tzOffMin / 60.0f;
  while (local <  0)  local += 24.0f;
  while (local >= 24) local -= 24.0f;
  return local;
}

// ── Schedule gate ────────────────────────────────────────────────────────
bool isActiveSchedule() {
  if (scheduleMode == 0) return true;
  struct tm ti;
  if (!getLocalTime(&ti)) return true;  // NTP not yet synced → stay active (safe default)
  float nowHr = ti.tm_hour + ti.tm_min / 60.0f;

  if (scheduleMode == 1) {              // sunset-to-sunrise
    if (schedLat == 0.0f && schedLon == 0.0f) return true;  // location not set
    int   doy  = ti.tm_yday + 1;       // 1–366
    float rise = calcSunEvent(schedLat, schedLon, doy, tzOffsetMin, true);
    float set  = calcSunEvent(schedLat, schedLon, doy, tzOffsetMin, false);
    if (rise < 0 || set < 0) return true;  // polar day/night edge case
    return (nowHr >= set) || (nowHr < rise);  // active dusk-to-dawn
  }

  // Mode 2: custom hours
  float onHr  = schedOnHour  + schedOnMin  / 60.0f;
  float offHr = schedOffHour + schedOffMin / 60.0f;
  if (onHr <= offHr) return nowHr >= onHr && nowHr < offHr;
  return nowHr >= onHr || nowHr < offHr;      // window crosses midnight
}

// Turn off all WLED segments (called when schedule becomes inactive)
void turnAllOff() {
  waveState    = WAVE_IDLE;
  dimDone      = false;
  motionActive = false;
  lastSensor   = S_NONE;
  litCount     = 0;
  clearWavePool();
  for (int i = 0; i < numSegments; i++) {
    setSegment(i, 0, 0);   // bri=0 → off
    segBright[i] = false;
  }
  Log.println("Schedule: lights off");
}

// ── NVS settings persistence ───────────────────────────────────────────────────────────
void loadSettings() {
  prefs.begin("brain", true);  // read-only
  int storedVer   = prefs.getInt("settVer",      0);
  briBright       = prefs.getInt("briBright",    255);
  briAmbient      = prefs.getInt("briAmbient",   255);
  fxBright        = prefs.getInt("fxBright",     0);
  fxAmbient       = prefs.getInt("fxAmbient",    66);
  palAmbient      = prefs.getInt("palAmbient",   35);
  colBrightR      = prefs.getInt("colBrightR",   255);
  colBrightG      = prefs.getInt("colBrightG",   150);
  colBrightB      = prefs.getInt("colBrightB",   0);
  colBrightEnable = prefs.getBool("colBrightEn", false);
  motionTimeout   = prefs.getULong("mTimeout",   10000);
  waveDelay       = prefs.getInt("waveDelay",    500);
  sensorMidSeg    = prefs.getInt("midSeg",       0);
  psAmbient       = prefs.getInt("psAmbient",    0);
  psMotion        = prefs.getInt("psMotion",     0);
  scheduleMode    = prefs.getInt("schedMode",    0);
  schedLat        = prefs.getFloat("schedLat",   0.0f);
  schedLon        = prefs.getFloat("schedLon",   0.0f);
  tzOffsetMin     = prefs.getInt("tzOffMin",     0);
  schedOnHour     = prefs.getInt("schedOnH",     18);
  schedOnMin      = prefs.getInt("schedOnM",     0);
  schedOffHour    = prefs.getInt("schedOffH",    6);
  schedOffMin     = prefs.getInt("schedOffM",    0);
  prefs.end();

  // Version migrations — run once, then save to advance stored version
  if (storedVer < SETTINGS_VER) {
    if (storedVer < 2) {
      colBrightEnable = false;  // v2: default changed from true → false
      Log.println("NVS migration v2: colBrightEnable reset to false");
    }
    // v3: schedule fields — already defaulted by getX() calls above
    if (storedVer < 3) Log.println("NVS migration v3: schedule fields initialised");
    if (storedVer < 4) Log.println("NVS migration v4: psAmbient/psMotion = 0 (legacy)");
    saveSettings();  // advance stored version so this block runs only once
  }
  Log.println("Settings loaded from NVS");
}

// ── WiFi credential persistence ─────────────────────────────────────────────
void loadWifiCreds() {
  prefs.begin("brain", true);
  wifiSSID = prefs.getString("wifiSSID", WIFI_SSID);    // NVS → config.h fallback
  wifiPass = prefs.getString("wifiPass",  WIFI_PASSWORD);
  prefs.end();
  Log.printf("WiFi creds: SSID=%s (from %s)\n", wifiSSID.c_str(),
                wifiSSID == WIFI_SSID ? "config.h" : "NVS");
}
void saveWifiCreds(const String& ssid, const String& pass) {
  prefs.begin("brain", false);
  prefs.putString("wifiSSID", ssid);
  prefs.putString("wifiPass",  pass);
  prefs.end();
  wifiSSID = ssid; wifiPass = pass;
  Log.printf("WiFi creds saved: SSID=%s\n", ssid.c_str());
}

// ── Device tag (display name + unique mDNS/OTA hostname) ────────────────────
String sanitizeTag(const String& in) {
  String out;
  for (size_t i = 0; i < in.length() && out.length() < 32; i++) {
    char c = in[i];
    if (c >= 32 && c < 127 && c != '"' && c != '\\' && c != '<' && c != '>' && c != '&')
      out += c;
  }
  out.trim();
  return out;
}

String mdnsHostname() {
  String slug;
  for (size_t i = 0; i < deviceTag.length(); i++) {
    char c = deviceTag[i];
    if (isalnum((unsigned char)c)) slug += (char)tolower((unsigned char)c);
    else if ((c == ' ' || c == '-' || c == '_') && slug.length() && slug[slug.length() - 1] != '-')
      slug += '-';
  }
  while (slug.endsWith("-")) slug.remove(slug.length() - 1);
  if (slug.length() == 0) return String("lanternbrain");
  String host = String("lanternbrain-") + slug;
  if (host.length() > 32) host = host.substring(0, 32);
  while (host.endsWith("-")) host.remove(host.length() - 1);
  return host;
}

void loadDeviceTag() {
  prefs.begin("brain", true);
  deviceTag = sanitizeTag(prefs.getString("devTag", ""));
  prefs.end();
  Log.printf("Device tag: '%s'  mDNS: %s.local\n",
                deviceTag.c_str(), mdnsHostname().c_str());
}
void saveDeviceTag() {
  prefs.begin("brain", false);
  prefs.putString("devTag", deviceTag);
  prefs.end();
  Log.printf("Device tag saved: '%s'\n", deviceTag.c_str());
}

// ── WLED target persistence + LAN discovery ─────────────────────────────────
void loadWledTarget() {
  prefs.begin("brain", true);
  wledIP   = prefs.getString("wledIP",   WLED_IP);
  wledName = prefs.getString("wledName", "");
  prefs.end();
  if (wledIP.length() == 0) wledIP = WLED_IP;
  Log.printf("WLED target: %s (%s)\n", wledIP.c_str(),
                wledName.length() ? wledName.c_str() : "unnamed");
}
void saveWledTarget() {
  prefs.begin("brain", false);
  prefs.putString("wledIP",   wledIP);
  prefs.putString("wledName", wledName);
  prefs.end();
  Log.printf("WLED target saved: %s (%s)\n", wledIP.c_str(), wledName.c_str());
}

bool isValidIPv4(const String& s) {
  IPAddress ip;
  return ip.fromString(s);
}

void wledFoundClear() { wledFoundCount = 0; }

bool wledFoundHas(const String& ip) {
  for (int i = 0; i < wledFoundCount; i++)
    if (ip == wledFound[i].ip) return true;
  return false;
}

void wledFoundAdd(const String& ip, const String& name, int leds,
                  const String& ver, const char* source) {
  if (ip.length() == 0 || wledFoundCount >= MAX_WLED_HOSTS) return;
  if (wledFoundHas(ip)) return;
  WledHost& h = wledFound[wledFoundCount++];
  strncpy(h.ip, ip.c_str(), sizeof(h.ip) - 1);       h.ip[sizeof(h.ip) - 1] = 0;
  strncpy(h.name, name.c_str(), sizeof(h.name) - 1); h.name[sizeof(h.name) - 1] = 0;
  strncpy(h.ver, ver.c_str(), sizeof(h.ver) - 1);    h.ver[sizeof(h.ver) - 1] = 0;
  strncpy(h.source, source, sizeof(h.source) - 1);   h.source[sizeof(h.source) - 1] = 0;
  h.leds = leds;
}

// Confirm a host speaks WLED JSON API. Fills name/leds/ver on success.
bool probeWled(const String& ip, String& name, int& leds, String& ver) {
  name = ""; ver = ""; leds = 0;
  if (!isValidIPv4(ip)) return false;
  HTTPClient http;
  char url[80];
  snprintf(url, sizeof(url), "http://%s/json/info", ip.c_str());
  http.setConnectTimeout(400);
  http.setTimeout(800);
  if (!http.begin(url)) return false;
  int code = http.GET();
  bool ok = false;
  if (code == 200) {
    String body = http.getString();
    StaticJsonDocument<192> filter;
    filter["name"] = true;
    filter["ver"] = true;
    filter["leds"]["count"] = true;
    StaticJsonDocument<256> doc;
    if (!deserializeJson(doc, body, DeserializationOption::Filter(filter))) {
      const char* n = doc["name"] | "WLED";
      const char* v = doc["ver"]  | "";
      // Require a version string — distinguishes WLED from other HTTP devices.
      if (strlen(v) > 0 || doc["leds"]["count"].is<int>()) {
        name = n;
        ver  = v;
        leds = doc["leds"]["count"] | 0;
        ok = true;
      }
    }
  }
  http.end();
  return ok;
}

void probeAndAdd(const String& ip, const char* source) {
  if (ip.length() == 0 || wledFoundHas(ip)) return;
  if (ip == WiFi.localIP().toString()) return;  // skip ourselves
  String name, ver;
  int leds = 0;
  if (probeWled(ip, name, leds, ver))
    wledFoundAdd(ip, name, leds, ver, source);
}

void scanWledMdns() {
  IPAddress a = MDNS.queryHost("wled", 1500);
  if (a) probeAndAdd(a.toString(), "mdns");

  int n = MDNS.queryService("wled", "tcp");
  for (int i = 0; i < n; i++) {
    IPAddress ip = MDNS.address(i);
    if (ip) probeAndAdd(ip.toString(), "mdns");
  }
  n = MDNS.queryService("http", "tcp");
  for (int i = 0; i < n; i++) {
    IPAddress ip = MDNS.address(i);
    if (ip) probeAndAdd(ip.toString(), "mdns");
  }
}

// Walk the local /24 looking for port 80 + WLED /json/info. ~15–25 s.
void scanWledLan() {
  IPAddress local = WiFi.localIP();
  WiFiClient client;
  for (int i = 1; i < 255; i++) {
    IPAddress ip(local[0], local[1], local[2], i);
    if (ip == local) continue;
    ArduinoOTA.handle();
    yield();
    if (!client.connect(ip, 80, 50)) continue;
    client.stop();
    probeAndAdd(ip.toString(), "lan");
  }
}

void refreshSegmentsFromWled() {
  int n = detectSegments();
  if (n > 0 && n <= MAX_SEG) {
    numSegments = n;
    if (sensorMidSeg <= 0 || sensorMidSeg >= numSegments)
      sensorMidSeg = numSegments / 2;
    waveState = WAVE_IDLE; dimDone = false; clearWavePool();
    if (isActiveSchedule()) applyAmbient(); else turnAllOff();
  }
}

// ── AP fallback mode (served when WiFi connection fails) ──────────────────────
const char AP_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Lantern Brain — WiFi Setup</title>
<style>
body{font-family:sans-serif;max-width:360px;margin:40px auto;padding:0 20px;background:#1a1a2e;color:#eee}
h1{color:#ff9f43;font-size:1.2em}
label{display:block;margin:10px 0 4px;font-size:.9em;color:#ccc}
input[type=text],input[type=password]{width:100%;padding:8px;background:#16213e;border:1px solid #333;color:#eee;border-radius:4px;box-sizing:border-box}
select{width:100%;padding:8px;background:#16213e;border:1px solid #333;color:#eee;border-radius:4px;box-sizing:border-box}
.row{display:flex;gap:6px;margin-bottom:6px}.row select{flex:1}
.row .bsc{padding:8px 12px;background:#2d4059;color:#eee;border:none;border-radius:4px;cursor:pointer;white-space:nowrap;font-size:.9em}
.chk{font-size:.8em;color:#666;display:flex;align-items:center;gap:4px;margin-top:4px;cursor:pointer}
button.sv{width:100%;padding:12px;margin-top:16px;background:#ff9f43;color:#1a1a2e;border:none;border-radius:6px;font-size:1em;cursor:pointer}
.note{font-size:.8em;color:#888;margin-top:12px}
</style></head><body>
<h1>&#x1F3EE; Lantern Brain — WiFi Setup</h1>
<p style="color:#ccc;font-size:.9em">Could not connect to the saved network. Enter new WiFi credentials.</p>
<label>Available Networks <span id="ss" style="font-size:.8em;color:#888"></span></label>
<div class="row">
  <select id="sel" onchange="pick()"><option value="">&#8212; tap &#128246; to scan &#8212;</option></select>
  <button class="bsc" id="bsc" onclick="scan()">&#128246; Scan</button>
</div>
<label>SSID</label>
<input type="text" id="s" value="__SSID__">
<label>Password</label>
<input type="password" id="p" placeholder="(enter password)">
<label class="chk"><input type="checkbox" id="sp" onchange="document.getElementById('p').type=this.checked?'text':'password'"> Show password</label>
<button class="sv" onclick="save()">Save &amp; Reboot</button>
<div class="note" id="msg"></div>
<script>
async function scan(){
  const btn=document.getElementById('bsc');
  const sts=document.getElementById('ss');
  const sel=document.getElementById('sel');
  btn.disabled=true;btn.textContent='Scanning\u2026';sts.textContent='scanning\u2026';
  try{
    const r=await fetch('/wifi/scan');
    const nets=await r.json();
    nets.sort((a,b)=>b.rssi-a.rssi);
    sel.innerHTML='<option value="">\u2014 select a network \u2014</option>';
    nets.forEach(n=>{
      const bars=n.rssi>=-55?'\u2588\u2588\u2588\u2588':n.rssi>=-65?'\u2588\u2588\u2588\u2591':n.rssi>=-75?'\u2588\u2588\u2591\u2591':'\u2588\u2591\u2591\u2591';
      const o=document.createElement('option');
      o.value=n.ssid;o.text=n.ssid+' '+bars+' '+n.rssi+'dBm';sel.add(o);
    });
    sts.textContent='('+nets.length+' found)';
  }catch(e){sts.textContent='scan failed';}
  btn.disabled=false;btn.textContent='\ud83d\udcf6 Scan';
}
function pick(){const v=document.getElementById('sel').value;if(v)document.getElementById('s').value=v;}
function save(){
  const s=document.getElementById('s').value.trim();
  const p=document.getElementById('p').value;
  if(!s){document.getElementById('msg').textContent='SSID cannot be empty.';return;}
  document.getElementById('msg').textContent='Saving\u2026';
  fetch('/wifi',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({ssid:s,pass:p})})
  .then(()=>document.getElementById('msg').textContent='Saved! Rebooting\u2026')
  .catch(()=>document.getElementById('msg').textContent='Error \u2014 try again.');
}
</script></body></html>
)rawliteral";

void startAPMode() {
  Log.println("WiFi failed — starting LanternBrain-Setup AP");
  WiFi.mode(WIFI_AP_STA);  // AP+STA mode so WiFi scan works
  WiFi.softAP("LanternBrain-Setup");
  Log.print("AP IP: "); Log.println(WiFi.softAPIP());

  attachHttpLogMiddleware();
  attachLogRoutes();
  server.on("/", []() {
    String html = FPSTR(AP_HTML);
    html.replace("__SSID__", wifiSSID);
    server.send(200, "text/html; charset=utf-8", html);
  });
  server.on("/wifi/scan", HTTP_GET, []() {
    int n = WiFi.scanNetworks();
    StaticJsonDocument<2048> sdoc;
    JsonArray sarr = sdoc.to<JsonArray>();
    for (int i = 0; i < n; i++) {
      if (WiFi.SSID(i).length() == 0) continue;
      bool dup = false;
      for (JsonObject obj : sarr) {
        if (obj["ssid"].as<String>() == WiFi.SSID(i)) {
          if (WiFi.RSSI(i) > obj["rssi"].as<int>()) obj["rssi"] = WiFi.RSSI(i);
          dup = true; break;
        }
      }
      if (!dup) {
        JsonObject net = sarr.createNestedObject();
        net["ssid"] = WiFi.SSID(i); net["rssi"] = WiFi.RSSI(i);
      }
    }
    WiFi.scanDelete();
    String sj; serializeJson(sdoc, sj);
    server.send(200, "application/json", sj);
  });
  server.on("/wifi", HTTP_POST, []() {
    StaticJsonDocument<256> doc;
    if (!deserializeJson(doc, server.arg("plain"))) {
      String s = doc["ssid"] | "";
      String p = doc["pass"] | "";
      if (s.length() > 0) {
        saveWifiCreds(s, p);
        server.send(200, "application/json", "{\"success\":true}");
        delay(500); ESP.restart(); return;
      }
    }
    server.send(400, "application/json", "{\"success\":false}");
  });
  server.begin();
  Log.println("AP web server started. Connect to LanternBrain-Setup and open 192.168.4.1");
  while (true) { maintainMemory(); server.handleClient(); delay(10); }  // never returns
}

void saveSettings() {
  prefs.begin("brain", false);  // read-write
  prefs.putInt("settVer",      SETTINGS_VER);
  prefs.putInt("briBright",    briBright);
  prefs.putInt("briAmbient",   briAmbient);
  prefs.putInt("fxBright",     fxBright);
  prefs.putInt("fxAmbient",    fxAmbient);
  prefs.putInt("palAmbient",   palAmbient);
  prefs.putInt("colBrightR",   colBrightR);
  prefs.putInt("colBrightG",   colBrightG);
  prefs.putInt("colBrightB",   colBrightB);
  prefs.putBool("colBrightEn", colBrightEnable);
  prefs.putULong("mTimeout",   motionTimeout);
  prefs.putInt("waveDelay",    waveDelay);
  prefs.putInt("midSeg",       sensorMidSeg);
  prefs.putInt("psAmbient",    psAmbient);
  prefs.putInt("psMotion",     psMotion);
  prefs.putInt("schedMode",    scheduleMode);
  prefs.putFloat("schedLat",   schedLat);
  prefs.putFloat("schedLon",   schedLon);
  prefs.putInt("tzOffMin",     tzOffsetMin);
  prefs.putInt("schedOnH",     schedOnHour);
  prefs.putInt("schedOnM",     schedOnMin);
  prefs.putInt("schedOffH",    schedOffHour);
  prefs.putInt("schedOffM",    schedOffMin);
  prefs.end();
  Log.println("Settings saved to NVS");
}

// Query WLED for segment count
int detectSegments() {
  ensureWiFi();
  if (WiFi.status() != WL_CONNECTED) return 0;

  HTTPClient http;
  char url[64];
  snprintf(url, sizeof(url), "http://%s/json/state", wledIP.c_str());

  http.begin(url);
  http.setTimeout(5000);
  int code = http.GET();
  int count = 0;

  if (code == 200) {
    String body = http.getString();
    StaticJsonDocument<2048> doc;
    DeserializationError err = deserializeJson(doc, body);
    if (!err) {
      count = doc["seg"].as<JsonArray>().size();
    }
  }
  http.end();
  return count;
}

// ── Web UI HTML ──────────────────────────────────────────────────────────────
const char HTML_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>__DEVICE_TAG__</title>
<style>
body{font-family:sans-serif;max-width:480px;margin:20px auto;padding:0 15px;background:#1a1a2e;color:#eee}
h1{color:#ff9f43;font-size:1.4em}h2{color:#aaa;font-size:1em;margin-top:18px}
#h1-tag{cursor:text;border-bottom:1px dotted #ff9f43;outline:none}
#h1-tag:focus{background:#16213e;padding:0 4px;border-radius:4px}
.card{background:#16213e;border-radius:8px;padding:12px 16px;margin:8px 0}
label{display:block;margin:8px 0 4px;font-size:.85em;color:#ccc}
select,input[type=range]{width:100%;margin:2px 0}
.val{float:right;color:#ff9f43}
button{width:100%;padding:12px;margin:8px 0;border:none;border-radius:6px;font-size:1em;cursor:pointer}
#apply{background:#ff9f43;color:#1a1a2e;font-weight:bold}
#preview-a{background:#555;color:#eee}#preview-m{background:#555;color:#eee}
select:disabled{opacity:.4}
#rescan,#btn-wled-scan,#btn-wled-lan{background:#2d4059;color:#eee;margin-top:4px}
#btn-wled-scan,#btn-wled-lan{width:auto;padding:8px 12px;font-size:.9em;margin:0;white-space:nowrap}
#btn-fw{background:#ff9f43;color:#1a1a2e;font-weight:bold}
input[type=file]{width:100%;margin:8px 0;color:#ccc;font-size:.85em}
input[type=color]{width:100%;height:38px;border:none;border-radius:4px;cursor:pointer;padding:2px;box-sizing:border-box}
.status{font-size:.8em;color:#888;text-align:center;margin-top:10px}
#msg{text-align:center;padding:8px;color:#4ecca3;display:none}
.seg-row{display:flex;gap:6px;justify-content:center;margin:10px 0}
.seg{width:36px;height:36px;border-radius:6px;display:flex;align-items:center;justify-content:center;font-size:.7em;font-weight:bold;transition:background .3s}
.seg-off{background:#8b0000;color:#faa}
.seg-on{background:#228b22;color:#afa}
.seg-label{text-align:center;font-size:.75em;color:#666;margin-top:2px}
.sim-grid{display:grid;grid-template-columns:1fr 1fr 1fr;gap:8px;margin:8px 0}
.sim-col{text-align:center}
.sim-lbl{font-size:.8em;color:#aaa;margin-bottom:4px}
.btn-trig,.btn-hold{background:#2d4059;color:#eee;padding:8px 0;margin:2px 0;font-size:.88em;width:100%}
.btn-hold.on{background:#c0392b;color:#fff}
#btn-clr{background:#1a1a2e;color:#666;font-size:.85em;padding:8px;border:1px solid #333}
#sim-badge{background:#c0392b;color:#fff;font-size:.65em;padding:2px 8px;border-radius:10px;vertical-align:middle;margin-left:6px;display:none}
.num-input,.time-input{width:100%;padding:5px 8px;background:#0d1b2e;border:1px solid #333;color:#eee;border-radius:4px;font-size:.9em;box-sizing:border-box;margin:2px 0}
.sched-info{font-size:.8em;color:#888;text-align:center;margin-top:8px;min-height:1.2em}
</style></head><body>
<h1>🏮 <span id="h1-tag" contenteditable="true" spellcheck="false" title="Click to edit name">__DEVICE_TAG__</span></h1>
<div class="status" id="h1-host" style="margin-top:-6px;margin-bottom:8px">__MDNS_HOST__.local</div>
<div id="msg"></div>

<h2>Name</h2>
<div class="card">
  <label>Name / tag <span style="color:#555;font-size:.8em">(click the title above, or type here)</span></label>
  <input type="text" id="dev-tag" class="num-input" maxlength="32" placeholder="e.g. Walkway, Porch, Driveway" oninput="previewTag()">
  <div style="font-size:.75em;color:#555;margin-top:4px">Network name: <strong id="dev-host" style="color:#ff9f43">__MDNS_HOST__.local</strong></div>
  <button id="btn-tag" onclick="saveTag()" style="background:#ff9f43;color:#1a1a2e;font-weight:bold;margin-top:8px">Save Name</button>
  <div style="font-size:.75em;color:#555;margin-top:6px">Saved on this page and as <code>lanternbrain-<em>name</em>.local</code>. A name change reboots so the new address appears on the LAN.</div>
</div>

<h2>Ambient Mode</h2>
<div class="card">
  <label>Preset</label>
  <select id="a-ps" onchange="psChanged()"><option value="0">None (use effect/palette below)</option></select>
  <label>Effect <span class="val" id="va-fx"></span></label>
  <select id="a-fx"></select>
  <label>Brightness <span class="val" id="va-bri"></span></label>
  <input type="range" id="a-bri" min="1" max="255">
  <label>Palette</label>
  <select id="a-pal"></select>
</div>
<button id="preview-a" onclick="preview('a')">Preview Ambient</button>

<h2>Motion Mode</h2>
<div class="card">
  <label>Preset</label>
  <select id="m-ps" onchange="psChanged()"><option value="0">Same as ambient (brightness only)</option></select>
  <label>Effect <span class="val" id="vm-fx"></span></label>
  <select id="m-fx"></select>
  <label>Brightness <span class="val" id="vm-bri"></span></label>
  <input type="range" id="m-bri" min="1" max="255">
  <label>Color override <input type="checkbox" id="m-col-en" style="width:auto;float:right"></label>
  <input type="color" id="m-col" value="#ff9600">
</div>
<button id="preview-m" onclick="preview('m')">Preview Motion</button>
<button id="btn-sync" onclick="syncPreset()" style="background:#2d4059;color:#eee">&#8645; Sync colors from WLED preset</button>

<h2>Timing</h2>
<div class="card">
  <label>Motion Timeout <span class="val" id="vt"></span></label>
  <input type="range" id="timeout" min="1" max="60" step="1">
  <label>Wave Delay <span class="val" id="vw"></span></label>
  <input type="range" id="wave" min="100" max="10000" step="100">
</div>

<h2>Sensor Zones</h2>
<div class="card">
  <label>Middle sensor at segment <span class="val" id="vz"></span></label>
  <input type="range" id="mid-seg" min="1" max="15" step="1">
  <div style="font-size:.75em;color:#555;margin-top:4px" id="zone-hint"></div>
</div>

<h2>Network</h2>
<div class="card">
  <div style="font-size:.85em;color:#ccc;margin-bottom:2px">Connected to: <strong id="net-ssid" style="color:#ff9f43"></strong></div>
  <div style="display:flex;align-items:center;gap:8px;margin-bottom:8px">
    <span style="font-size:.85em;color:#ccc;flex:1">Password: <span id="net-pw-cur" style="color:#888;letter-spacing:.1em">&bull;&bull;&bull;&bull;&bull;&bull;&bull;</span></span>
    <label style="font-size:.75em;color:#666;white-space:nowrap;margin:0;display:flex;align-items:center;gap:3px">
      <input type="checkbox" id="net-pw-show" onchange="toggleCurPw()" style="width:auto"> Show
    </label>
  </div>
  <label>Available Networks <span id="scan-status" style="font-size:.8em;color:#888"></span></label>
  <div style="display:flex;gap:6px;margin-bottom:6px">
    <select id="net-sel" onchange="selectNetwork()" style="flex:1;padding:5px 8px;background:#0d1b2e;border:1px solid #333;color:#eee;border-radius:4px;font-size:.9em;box-sizing:border-box">
      <option value="">&#8212; tap &#128246; to scan &#8212;</option>
    </select>
    <button onclick="scanNetworks()" id="btn-scan" style="background:#2d4059;color:#eee;padding:8px 12px;font-size:.9em;white-space:nowrap;width:auto;border-radius:4px;margin:0">&#128246; Scan</button>
  </div>
  <label>SSID <span style="color:#555;font-size:.8em">(auto-filled from scan, or type here)</span></label>
  <input type="text" id="net-s" class="num-input" placeholder="Network name">
  <label style="margin-top:6px">New Password</label>
  <input type="password" id="net-p" class="num-input" placeholder="(leave blank to keep current)">
  <label style="font-size:.75em;color:#666;margin-top:2px;display:flex;align-items:center;gap:4px;cursor:pointer">
    <input type="checkbox" id="net-np-show" onchange="toggleNewPw()" style="width:auto"> Show new password
  </label>
  <button id="btn-net" onclick="changeNetwork()" style="background:#c0392b;color:#fff;margin-top:8px">Change Network &amp; Reboot</button>
  <div style="font-size:.75em;color:#555;margin-top:6px">After reboot reconnect to the new network and open <strong>lanternbrain.local</strong></div>
</div>

<h2>WLED Controller</h2>
<div class="card">
  <div style="font-size:.85em;color:#ccc;margin-bottom:6px">Using: <strong id="wled-cur" style="color:#ff9f43">—</strong>
    <span id="wled-reach" style="font-size:.8em;color:#888"></span>
  </div>
  <label>Discovered hosts <span id="wled-scan-sts" style="font-size:.8em;color:#888"></span></label>
  <div style="display:flex;gap:6px;margin-bottom:6px;flex-wrap:wrap">
    <select id="wled-sel" onchange="pickWled()" style="flex:1;min-width:140px;padding:5px 8px;background:#0d1b2e;border:1px solid #333;color:#eee;border-radius:4px;font-size:.9em;box-sizing:border-box">
      <option value="">&#8212; tap Find WLED &#8212;</option>
    </select>
    <button onclick="scanWled(false)" id="btn-wled-scan">Find WLED</button>
    <button onclick="scanWled(true)" id="btn-wled-lan">Scan LAN</button>
  </div>
  <label>IP address <span style="color:#555;font-size:.8em">(from list, or type any IPv4)</span></label>
  <input type="text" id="wled-ip" class="num-input" placeholder="192.168.x.x" inputmode="decimal">
  <button id="btn-wled-use" onclick="useWled()" style="background:#ff9f43;color:#1a1a2e;font-weight:bold;margin-top:8px">Use this WLED</button>
  <div style="font-size:.75em;color:#555;margin-top:6px">Find WLED uses mDNS (~3s). Scan LAN walks the subnet (~20s) and finds hosts that mDNS misses.</div>
</div>

<h2>Schedule</h2>
<div class="card">
  <label>Mode</label>
  <select id="sched-mode" onchange="updateSchedUI()">
    <option value="0">Always On</option>
    <option value="1">Sunset to Sunrise</option>
    <option value="2">Custom Hours</option>
  </select>
  <div id="sched-loc">
    <label>Latitude <span style="color:#555;font-size:.8em">(decimal, e.g. 40.71)</span></label>
    <input type="number" id="sched-lat" step="0.001" min="-90" max="90" class="num-input">
    <label>Longitude <span style="color:#555;font-size:.8em">(decimal, e.g. -74.00)</span></label>
    <input type="number" id="sched-lon" step="0.001" min="-180" max="180" class="num-input">
    <button id="btn-geo" onclick="grabLocation()" style="background:#2d4059;color:#eee;font-size:.85em;padding:8px;margin:4px 0">&#128205; Use My Location</button>
    <div style="font-size:.75em;color:#555;margin-top:4px">Browsers block GPS on http:// pages. This button will use network location instead, or type coordinates from a maps app.</div>
  </div>
  <div id="sched-custom">
    <label>Lights On at</label>
    <input type="time" id="sched-on" class="time-input">
    <label>Lights Off at</label>
    <input type="time" id="sched-off" class="time-input">
  </div>
  <div id="sched-tz-row">
    <label>Time Zone <span style="color:#555;font-size:.8em">(update manually for DST transitions)</span></label>
    <select id="sched-tz"></select>
  </div>
  <div class="sched-info" id="sched-info">—</div>
</div>

<button id="apply" onclick="apply()">Apply &amp; Save</button>

<h2>Segments <span id="seg-count" style="color:#ff9f43;font-size:.85em"></span></h2>
<div class="seg-row" id="seg-row"></div>
<button id="rescan" onclick="rescan()">&#8635; Refresh Segments</button>
<div class="status" id="status"></div>

<h2>Sensor Simulator <span id="sim-badge">SIM</span></h2>
<div class="card">
  <div style="font-size:.75em;color:#666;text-align:center;margin-bottom:8px">Inject sensor signals — real sensors ignored while active</div>
  <div class="sim-grid">
    <div class="sim-col">
      <div class="sim-lbl">&#9650; Front</div>
      <button class="btn-trig" onclick="trigSim('front')">&#9889; Trigger</button>
      <button class="btn-hold" id="hold-front" onclick="holdSim('front')">Hold</button>
    </div>
    <div class="sim-col">
      <div class="sim-lbl">&#9679; Middle</div>
      <button class="btn-trig" onclick="trigSim('middle')">&#9889; Trigger</button>
      <button class="btn-hold" id="hold-middle" onclick="holdSim('middle')">Hold</button>
    </div>
    <div class="sim-col">
      <div class="sim-lbl">&#9660; Back</div>
      <button class="btn-trig" onclick="trigSim('back')">&#9889; Trigger</button>
      <button class="btn-hold" id="hold-back" onclick="holdSim('back')">Hold</button>
    </div>
  </div>
  <button id="btn-clr" onclick="clearSim()">&#10005; Exit Simulation</button>
</div>

<h2>Firmware</h2>
<div class="card">
  <div style="font-size:.85em;color:#ccc">This Brain: <strong id="fw-cur" style="color:#ff9f43">…</strong></div>
  <div style="font-size:.85em;color:#ccc;margin-top:4px">GitHub: <strong id="fw-latest" style="color:#888">checking…</strong></div>
  <div id="fw-avail" style="display:none;font-size:.85em;color:#4ecca3;margin-top:6px">A newer build is available.</div>
  <button id="btn-gh" onclick="installGithub()" style="background:#ff9f43;color:#1a1a2e;font-weight:bold;margin-top:8px">Install from GitHub</button>
  <input type="file" id="fw-file" accept=".bin">
  <button id="btn-fw" onclick="uploadFw()">Upload .bin &amp; Reboot</button>
  <div id="fw-bar-wrap" style="display:none;margin-top:10px;background:#0d1b2e;border-radius:4px;overflow:hidden;height:14px">
    <div id="fw-bar" style="height:14px;width:0;background:#ff9f43;transition:width .3s"></div>
  </div>
  <div style="font-size:.8em;color:#888;text-align:center;min-height:1.2em" id="fw-sts"></div>
</div>

<h2>Log</h2>
<div class="card">
  <pre id="log-box" style="max-height:220px;overflow:auto;background:#0d1b2e;padding:8px;border-radius:4px;font-size:.7em;white-space:pre-wrap;word-break:break-all;margin:0 0 8px"></pre>
  <div style="display:flex;gap:6px;flex-wrap:wrap">
    <button onclick="refreshLog()" id="btn-log" style="background:#2d4059;color:#eee;width:auto;padding:8px 12px;margin:0">Refresh</button>
    <button onclick="clearLog()" style="background:#2d4059;color:#eee;width:auto;padding:8px 12px;margin:0">Clear</button>
    <label style="font-size:.8em;color:#ccc;display:flex;align-items:center;gap:4px;margin:0">
      <input type="checkbox" id="log-auto" onchange="toggleLogAuto()" style="width:auto"> Auto
    </label>
  </div>
  <div style="font-size:.75em;color:#555;margin-top:6px">Same stream as <strong>USB Serial Monitor @ 115200</strong>. CLI: <code>curl http://__MDNS_HOST__.local/log</code></div>
</div>

<script>
let WLED='';
let fx={},pals={},holds={front:false,middle:false,back:false};
let curPw='';  // current WiFi password for show/hide
let logSeq=0,logTimer=0;
const TZLIST=[
  {l:'UTC-12:00 (Baker Island)',v:-720},
  {l:'UTC-11:00 (American Samoa, Pago Pago)',v:-660},
  {l:'UTC-10:00 (Hawaii)',v:-600},
  {l:'UTC-9:30 (Marquesas Islands)',v:-570},
  {l:'UTC-9:00 (Alaska Standard / AKST)',v:-540},
  {l:'UTC-8:00 (PST \u00b7 Alaska Summer / AKDT)',v:-480},
  {l:'UTC-7:00 (PDT \u00b7 MST \u00b7 Arizona)',v:-420},
  {l:'UTC-6:00 (MDT \u00b7 CST \u00b7 Mexico City)',v:-360},
  {l:'UTC-5:00 (CDT \u00b7 EST \u00b7 Lima \u00b7 Bogota)',v:-300},
  {l:'UTC-4:00 (EDT \u00b7 AST \u00b7 Caracas \u00b7 Santiago)',v:-240},
  {l:'UTC-3:30 (Newfoundland / NDT)',v:-210},
  {l:'UTC-3:00 (BRT \u00b7 ART \u00b7 Sao Paulo)',v:-180},
  {l:'UTC-2:00 (Fernando de Noronha)',v:-120},
  {l:'UTC-1:00 (Azores \u00b7 Cape Verde)',v:-60},
  {l:'UTC+0:00 (GMT \u00b7 WET \u00b7 Reykjavik \u00b7 Accra)',v:0},
  {l:'UTC+1:00 (CET \u00b7 BST \u00b7 Lagos \u00b7 Paris)',v:60},
  {l:'UTC+2:00 (EET \u00b7 CEST \u00b7 Cairo \u00b7 Johannesburg)',v:120},
  {l:'UTC+3:00 (MSK \u00b7 Riyadh \u00b7 Nairobi \u00b7 Istanbul)',v:180},
  {l:'UTC+3:30 (Iran Standard / IRST)',v:210},
  {l:'UTC+4:00 (Gulf \u00b7 Dubai \u00b7 Baku)',v:240},
  {l:'UTC+4:30 (Afghanistan / AFT)',v:270},
  {l:'UTC+5:00 (PKT \u00b7 Karachi \u00b7 Tashkent)',v:300},
  {l:'UTC+5:30 (IST \u00b7 India \u00b7 Sri Lanka)',v:330},
  {l:'UTC+5:45 (NPT \u00b7 Nepal)',v:345},
  {l:'UTC+6:00 (BST \u00b7 Dhaka \u00b7 Almaty)',v:360},
  {l:'UTC+6:30 (MMT \u00b7 Myanmar \u00b7 Yangon)',v:390},
  {l:'UTC+7:00 (ICT \u00b7 Bangkok \u00b7 Jakarta \u00b7 Hanoi)',v:420},
  {l:'UTC+8:00 (CST \u00b7 SGT \u00b7 HKT \u00b7 Perth \u00b7 KL)',v:480},
  {l:'UTC+8:45 (ACWST \u00b7 Eucla)',v:525},
  {l:'UTC+9:00 (JST \u00b7 KST \u00b7 Tokyo \u00b7 Seoul)',v:540},
  {l:'UTC+9:30 (ACST \u00b7 Darwin \u00b7 Adelaide Std)',v:570},
  {l:'UTC+10:00 (AEST \u00b7 Vladivostok \u00b7 Brisbane)',v:600},
  {l:'UTC+10:30 (LHST \u00b7 Lord Howe Island)',v:630},
  {l:'UTC+11:00 (AEDT \u00b7 Sydney Summer \u00b7 Solomon Is.)',v:660},
  {l:'UTC+12:00 (NZST \u00b7 Fiji \u00b7 Kamchatka)',v:720},
  {l:'UTC+12:45 (CHAST \u00b7 Chatham Islands)',v:765},
  {l:'UTC+13:00 (NZDT \u00b7 Tonga \u00b7 Samoa Summer)',v:780},
  {l:'UTC+14:00 (LINT \u00b7 Kiribati)',v:840},
];
async function trigSim(s){await fetch('/simulate?sensor='+s);}
async function holdSim(s){
  holds[s]=!holds[s];
  document.getElementById('hold-'+s).classList.toggle('on',holds[s]);
  await fetch('/simulate?sensor='+s+'&hold='+(holds[s]?1:0));
}
async function clearSim(){
  holds={front:false,middle:false,back:false};
  ['front','middle','back'].forEach(s=>document.getElementById('hold-'+s).classList.remove('on'));
  document.getElementById('sim-badge').style.display='none';
  await fetch('/simulate?sensor=clear');
}
function applyLatLon(lat,lon,note){
  document.getElementById('sched-lat').value=Number(lat).toFixed(5);
  document.getElementById('sched-lon').value=Number(lon).toFixed(5);
  if(note) document.getElementById('sched-info').textContent=note;
}
async function geoFromIp(){
  const r=await fetch('/geo');
  const d=await r.json();
  if(!d.ok) throw new Error(d.error||'ip lookup failed');
  const place=[d.city,d.region,d.country].filter(Boolean).join(', ');
  applyLatLon(d.lat,d.lon,'Approx. '+place+' (from network IP). Click Apply & Save.');
}
async function grabLocation(){
  const btn=document.getElementById('btn-geo');
  const info=document.getElementById('sched-info');
  btn.disabled=true;btn.textContent='\ud83d\udccd Locating\u2026';
  const finish=()=>{btn.disabled=false;btn.textContent='\ud83d\udccd Use My Location';};
  const secure=window.isSecureContext===true;
  if(secure && navigator.geolocation){
    navigator.geolocation.getCurrentPosition(
      function(p){
        applyLatLon(p.coords.latitude,p.coords.longitude,'GPS location set. Click Apply & Save.');
        finish();
      },
      async function(e){
        try{await geoFromIp();}
        catch(err){
          const m={1:'Location permission denied.',2:'Location unavailable.',3:'Location request timed out.'};
          info.textContent=(m[e.code]||'Location error.')+' Enter lat/lon manually.';
        }
        finish();
      },
      {timeout:8000,maximumAge:300000}
    );
    return;
  }
  info.textContent='GPS needs HTTPS \u2014 using network location\u2026';
  try{await geoFromIp();}
  catch(e){info.textContent='Could not guess location. Paste latitude and longitude from a maps app, then Apply & Save.';}
  finish();
}
function updateSchedUI(){
  const m=+document.getElementById('sched-mode').value;
  document.getElementById('sched-loc').style.display    = m===1?'':'none';
  document.getElementById('sched-custom').style.display = m===2?'':'none';
  document.getElementById('sched-tz-row').style.display = m>0?'':'none';
}
function hm2t(h,m){return String(h).padStart(2,'0')+':'+String(m).padStart(2,'0');}
function t2hm(v){const p=(v||'00:00').split(':');return{h:+p[0],m:+p[1]};}
async function pollSchedule(){
  try{
    const r=await fetch('/schedule');const d=await r.json();
    let s=d.ntpSync?'Time: '+d.localTime:'NTP syncing...';
    if(d.mode===1&&d.ntpSync) s+=' | \u2600\ufe0f '+d.sunrise+' \ud83c\udf19 '+d.sunset;
    else if(d.mode===2) s+=' | Window: '+document.getElementById('sched-on').value+'\u2013'+document.getElementById('sched-off').value;
    s+=' | '+(d.active?'\u2705 Active':'\u26d4 Inactive');
    document.getElementById('sched-info').textContent=s;
  }catch(e){}
}
function rgbToHex(r,g,b){return '#'+[r,g,b].map(v=>v.toString(16).padStart(2,'0')).join('');}
function setWledUrl(ip){
  WLED=ip?'http://'+ip:'';
  const el=document.getElementById('wled-cur');
  if(el) el.textContent=ip||'—';
  const inp=document.getElementById('wled-ip');
  if(inp && ip) inp.value=ip;
}
function wledLabel(h){
  const bits=[h.name||'WLED', h.ip];
  if(h.leds) bits.push(h.leds+' LEDs');
  if(h.ver) bits.push('v'+h.ver);
  return bits.join(' · ');
}
function fillWledSelect(hosts, selectedIp){
  const sel=document.getElementById('wled-sel');
  sel.innerHTML='<option value="">\u2014 select a WLED host \u2014</option>';
  (hosts||[]).forEach(h=>{
    if(!h.ip) return;
    const o=document.createElement('option');
    o.value=h.ip;
    o.text=wledLabel(h);
    if(h.ip===selectedIp) o.selected=true;
    sel.add(o);
  });
}
async function loadEffects(){
  fx={};pals={};
  try{
    const r=await fetch(WLED+'/json/effects');
    const list=await r.json();
    list.forEach((n,i)=>{if(n)fx[i]=n;});
  }catch(e){fx={0:'Solid',66:'Fire 2012',88:'Candle',45:'Fire Flicker'};}
  try{
    const rp=await fetch(WLED+'/json/palettes');
    const list=await rp.json();
    list.forEach((n,i)=>{if(n)pals[i]=n;});
  }catch(e){pals={0:'Default',7:'Cloud',8:'Lava',11:'Rainbow',13:'Sunset',35:'Fire',36:'Icefire',37:'Cyane'};}
  populateSelect('a-fx');populateSelect('m-fx');populatePalSelect('a-pal');
}
async function loadWledCurrent(){
  try{
    const r=await fetch('/wled');const d=await r.json();
    setWledUrl(d.ip||'');
    document.getElementById('wled-reach').textContent=d.reachable?'(reachable'+(d.name?': '+d.name:'')+')':'(not reachable)';
    document.getElementById('wled-reach').style.color=d.reachable?'#4ecca3':'#c0392b';
    fillWledSelect(d.ip?[{ip:d.ip,name:d.name,leds:d.leds,ver:d.ver}]:[], d.ip);
  }catch(e){
    document.getElementById('wled-reach').textContent='(status failed)';
  }
}
async function scanWled(full){
  const btn=document.getElementById(full?'btn-wled-lan':'btn-wled-scan');
  const sts=document.getElementById('wled-scan-sts');
  const orig=btn.textContent;
  btn.disabled=true;btn.textContent=full?'Scanning LAN\u2026':'Finding\u2026';
  sts.textContent=full?'walking subnet, ~20s':'mDNS\u2026';
  try{
    const r=await fetch('/wled/scan'+(full?'?full=1':''),{signal:AbortSignal.timeout?AbortSignal.timeout(full?45000:12000):undefined});
    const d=await r.json();
    fillWledSelect(d.hosts||[], d.ip);
    if(d.ip) setWledUrl(d.ip);
    const n=(d.hosts||[]).length;
    sts.textContent='('+n+' found)';
  }catch(e){sts.textContent='scan failed';}
  btn.disabled=false;btn.textContent=orig;
}
function pickWled(){
  const v=document.getElementById('wled-sel').value;
  if(v) document.getElementById('wled-ip').value=v;
}
async function useWled(){
  const ip=document.getElementById('wled-ip').value.trim();
  if(!ip){alert('Enter or select a WLED IP.');return;}
  const btn=document.getElementById('btn-wled-use');
  btn.disabled=true;btn.textContent='Connecting\u2026';
  try{
    const r=await fetch('/wled',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({ip:ip})});
    const d=await r.json();
    if(!d.success){alert(d.error||'Could not save WLED IP');}
    else{
      setWledUrl(d.ip);
      document.getElementById('wled-reach').textContent=d.reachable?'(reachable'+(d.name?': '+d.name:'')+')':'(saved, not reachable)';
      document.getElementById('wled-reach').style.color=d.reachable?'#4ecca3':'#c0392b';
      await loadEffects();
      updateLabels();
      await rescan();
    }
  }catch(e){alert('Failed to set WLED');}
  btn.disabled=false;btn.textContent='Use this WLED';
}
async function refreshLog(){
  try{
    const r=await fetch('/log'+(logSeq?('?after='+logSeq):''));
    const seq=r.headers.get('X-Log-Seq');
    if(seq) logSeq=+seq;
    const t=await r.text();
    const box=document.getElementById('log-box');
    if(t){
      box.textContent=(box.textContent+t).slice(-12000);
      box.scrollTop=box.scrollHeight;
    }
  }catch(e){}
}
async function clearLog(){
  try{await fetch('/log/clear',{method:'POST'});}catch(e){}
  document.getElementById('log-box').textContent='';
}
function toggleLogAuto(){
  if(logTimer){clearInterval(logTimer);logTimer=0;}
  if(document.getElementById('log-auto').checked){
    refreshLog();
    logTimer=setInterval(refreshLog,2000);
  }
}
function shortSha(s){s=(s||'').trim();return s.length>7?s.slice(0,7):s;}
async function checkFw(){
  const curEl=document.getElementById('fw-cur');
  const latEl=document.getElementById('fw-latest');
  const av=document.getElementById('fw-avail');
  const btn=document.getElementById('btn-gh');
  try{
    const u=await fetch('/update');const c=await u.json();
    const cur=c.sha_short||shortSha(c.sha)||'dev';
    curEl.textContent=cur+(c.built?' · '+c.built:'');
  }catch(e){curEl.textContent='unknown';}
  try{
    const r=await fetch('/update/check');const d=await r.json();
    if(!d.checked){
      latEl.textContent=d.error?('check failed (HTTP '+d.error+')'):'could not reach GitHub';
      latEl.style.color='#c0392b';
      return;
    }
    const lat=d.latest_short||shortSha(d.latest)||'—';
    if(d.update){
      latEl.textContent=lat+(d.latest_built?' · '+d.latest_built:'');
      latEl.style.color='#ff9f43';
      av.style.display='block';
      btn.textContent='Install update from GitHub';
    }else{
      latEl.textContent=lat+' · up to date';
      latEl.style.color='#4ecca3';
      av.style.display='none';
      btn.textContent='Reinstall from GitHub';
    }
  }catch(e){
    latEl.textContent='check failed';
    latEl.style.color='#c0392b';
  }
}
function showFwBar(pct,msg){
  const wrap=document.getElementById('fw-bar-wrap');
  wrap.style.display='block';
  const p=Math.max(0,Math.min(100,+pct||0));
  document.getElementById('fw-bar').style.width=p+'%';
  if(msg) document.getElementById('fw-sts').textContent=msg;
}
function waitForBrain(){
  const sts=document.getElementById('fw-sts');
  showFwBar(100,'Rebooting\u2026 waiting for the Brain to come back');
  let down=false;
  let n=0;
  const t=setInterval(async()=>{
    n++;
    try{
      const r=await fetch('/update?t='+Date.now(),{cache:'no-store',signal:AbortSignal.timeout?AbortSignal.timeout(2500):undefined});
      if(r.ok && (down || n>=4)){
        clearInterval(t);
        sts.textContent='Back online \u2014 reloading';
        location.reload();
      }
    }catch(e){
      down=true;
      showFwBar(100,'Waiting for Brain\u2026 '+(n*2)+'s');
    }
    if(n>90){
      clearInterval(t);
      sts.textContent='Still down after 3 min \u2014 reload this page when it returns.';
    }
  },2000);
}
async function pollOtaProgress(){
  for(let i=0;i<360;i++){
    await new Promise(r=>setTimeout(r,500));
    try{
      const r=await fetch('/update/progress',{cache:'no-store',signal:AbortSignal.timeout?AbortSignal.timeout(2000):undefined});
      const d=await r.json();
      const tot=+d.total||0, n=+d.bytes||0;
      const pct=tot>0?Math.round(n*100/tot):(d.running?2:0);
      const kb=tot?(' '+Math.round(n/1024)+'/'+Math.round(tot/1024)+' KB'):'';
      showFwBar(pct,(d.phase||'updating')+' \u2014 '+pct+'%'+kb);
      if(d.error){document.getElementById('fw-sts').textContent='Update failed: '+d.error;return;}
      if(d.phase==='reboot' || (tot>0 && n>=tot && !d.running)) break;
    }catch(e){
      waitForBrain();
      return;
    }
  }
  waitForBrain();
}
async function installGithub(){
  if(!confirm('Download the latest GitHub firmware and reboot this Brain?'))return;
  const sts=document.getElementById('fw-sts');
  const btn=document.getElementById('btn-gh');
  btn.disabled=true;document.getElementById('btn-fw').disabled=true;
  showFwBar(1,'Starting GitHub download\u2026');
  try{
    const r=await fetch('/update/url',{method:'POST',headers:{'Content-Type':'application/json'},body:'{}'});
    const d=await r.json();
    if(!d.started){sts.textContent='GitHub update failed to start';return;}
    await pollOtaProgress();
  }catch(e){
    waitForBrain();
  }
}
function uploadFw(){
  const f=document.getElementById('fw-file').files[0];
  const sts=document.getElementById('fw-sts');
  if(!f){alert('Choose a .bin file first.\\nArduino IDE: Sketch \u2192 Export Compiled Binary');return;}
  if(!confirm('Upload '+f.name+' ('+Math.round(f.size/1024)+' KB) and reboot the Brain?'))return;
  const fd=new FormData();fd.append('update',f);
  const xhr=new XMLHttpRequest();
  xhr.open('POST','/update');
  xhr.timeout=180000;
  xhr.upload.onprogress=function(e){
    if(e.lengthComputable){
      const pct=Math.round(e.loaded*100/e.total);
      showFwBar(pct,'Uploading \u2014 '+pct+'% ('+Math.round(e.loaded/1024)+'/'+Math.round(e.total/1024)+' KB)');
    }
  };
  xhr.onload=function(){
    if(xhr.status>=200&&xhr.status<300){
      showFwBar(100,'Uploaded \u2014 rebooting');
      waitForBrain();
    }else{
      sts.textContent='Upload failed (HTTP '+xhr.status+')';
    }
  };
  xhr.onerror=function(){waitForBrain();};
  xhr.ontimeout=function(){sts.textContent='Upload timed out';};
  document.getElementById('btn-fw').disabled=true;
  document.getElementById('btn-gh').disabled=true;
  showFwBar(0,'Uploading\u2026');
  xhr.send(fd);
}
async function init(){
  await loadWledCurrent();
  await loadEffects();
  const r2=await fetch('/config');
  const c=await r2.json();
  await loadPresets(c.psAmbient||0,c.psMotion||0);
  document.getElementById('a-fx').value=c.fxAmbient;
  document.getElementById('a-bri').value=c.briAmbient;
  document.getElementById('a-pal').value=c.palAmbient||35;
  document.getElementById('m-fx').value=c.fxBright;
  document.getElementById('m-bri').value=c.briBright;
  document.getElementById('m-col').value=rgbToHex(c.colBrightR??255,c.colBrightG??150,c.colBrightB??0);
  const en=c.colBrightEnable!==false;
  document.getElementById('m-col-en').checked=en;
  document.getElementById('m-col').disabled=!en;
  document.getElementById('timeout').value=c.motionTimeout/1000;
  document.getElementById('wave').value=c.waveDelay;
  document.getElementById('mid-seg').value=c.sensorMidSeg||5;
  document.getElementById('sched-mode').value=c.scheduleMode||0;
  document.getElementById('sched-lat').value=c.schedLat||0;
  document.getElementById('sched-lon').value=c.schedLon||0;
  const tzSel=document.getElementById('sched-tz');
  tzSel.innerHTML='<option value="">\u2014 select time zone \u2014</option>';
  TZLIST.forEach(z=>{const o=document.createElement('option');o.value=z.v;o.text=z.l;tzSel.add(o);});
  tzSel.value=c.tzOffsetMin??0;
  document.getElementById('sched-on').value=hm2t(c.schedOnHour??18,c.schedOnMin??0);
  document.getElementById('sched-off').value=hm2t(c.schedOffHour??6,c.schedOffMin??0);
  try{
    const wr=await fetch('/wifi');const w=await wr.json();
    document.getElementById('net-ssid').textContent=w.ssid||'';
    curPw=w.pass||'';
    document.getElementById('net-pw-cur').textContent=curPw?'\u2022\u2022\u2022\u2022\u2022\u2022\u2022':'(not set)';
    document.getElementById('net-s').placeholder='e.g. '+w.ssid;
  }catch(e){}
  try{
    const tr=await fetch('/tag');const t=await tr.json();
    document.getElementById('dev-tag').value=t.tag||'';
    document.getElementById('h1-tag').textContent=t.tag||'Lantern Brain';
    document.getElementById('h1-host').textContent=(t.hostname||'lanternbrain')+'.local';
    document.getElementById('dev-host').textContent=(t.hostname||'lanternbrain')+'.local';
    document.title=t.tag||'Lantern Brain';
  }catch(e){}
  const ht=document.getElementById('h1-tag');
  ht.addEventListener('input',()=>{
    document.getElementById('dev-tag').value=ht.textContent.replace(/\s+/g,' ');
    previewTag();
  });
  ht.addEventListener('keydown',e=>{
    if(e.key==='Enter'){e.preventDefault();ht.blur();}
  });
  ht.addEventListener('blur',()=>saveTag());
  updateSchedUI();
  updateLabels();
  checkFw();
  setInterval(pollStatus,1000);
  setInterval(pollSchedule,5000);
  pollSchedule();
}
function populatePalSelect(id){
  const sel=document.getElementById(id);
  sel.innerHTML='';
  Object.keys(pals).sort((a,b)=>pals[a].localeCompare(pals[b])).forEach(k=>{
    const o=document.createElement('option');o.value=k;o.text=pals[k]+' ('+k+')';sel.add(o);
  });
}
function populateSelect(id){
  const sel=document.getElementById(id);
  sel.innerHTML='';
  Object.keys(fx).sort((a,b)=>fx[a].localeCompare(fx[b])).forEach(k=>{
    const o=document.createElement('option');o.value=k;o.text=fx[k]+' ('+k+')';sel.add(o);
  });
}
function updateLabels(){
  document.getElementById('va-fx').textContent=fx[document.getElementById('a-fx').value]||'';
  document.getElementById('va-bri').textContent=document.getElementById('a-bri').value;
  document.getElementById('vm-fx').textContent=fx[document.getElementById('m-fx').value]||'';
  document.getElementById('vm-bri').textContent=document.getElementById('m-bri').value;
  document.getElementById('vt').textContent=document.getElementById('timeout').value+'s';
  const wv=document.getElementById('wave').value;document.getElementById('vw').textContent=wv>=1000?(wv/1000)+'s':wv+'ms';
  const ms=+document.getElementById('mid-seg').value;
  document.getElementById('vz').textContent=ms;
  document.getElementById('zone-hint').textContent='Front: 0–'+(ms-1)+' · Mid: '+ms+' · Back: '+(ms+1)+'–?';
}
document.querySelectorAll('input,select').forEach(el=>el.addEventListener('input',updateLabels));
document.getElementById('m-col-en').addEventListener('change',function(){
  document.getElementById('m-col').disabled=!this.checked;
});

async function scanNetworks(){
  const btn=document.getElementById('btn-scan');
  const sts=document.getElementById('scan-status');
  const sel=document.getElementById('net-sel');
  btn.disabled=true;btn.textContent='Scanning\u2026';
  sts.textContent='scanning\u2026';
  try{
    const r=await fetch('/wifi/scan',{signal:AbortSignal.timeout?AbortSignal.timeout(10000):undefined});
    const nets=await r.json();
    nets.sort((a,b)=>b.rssi-a.rssi);
    sel.innerHTML='<option value="">&#8212; select a network &#8212;</option>';
    nets.forEach(n=>{
      const bars=n.rssi>=-55?'\u2588\u2588\u2588\u2588':n.rssi>=-65?'\u2588\u2588\u2588\u2591':n.rssi>=-75?'\u2588\u2588\u2591\u2591':'\u2588\u2591\u2591\u2591';
      const o=document.createElement('option');
      o.value=n.ssid;
      o.text=n.ssid+'  '+bars+'  '+n.rssi+'dBm';
      sel.add(o);
    });
    sts.textContent='('+nets.length+' found)';
  }catch(e){
    sts.textContent='scan failed';
  }
  btn.disabled=false;btn.textContent='\u{1F4F6} Scan';
}
function selectNetwork(){
  const v=document.getElementById('net-sel').value;
  if(v) document.getElementById('net-s').value=v;
}
function toggleCurPw(){
  const show=document.getElementById('net-pw-show').checked;
  document.getElementById('net-pw-cur').textContent=show?curPw:'\u2022\u2022\u2022\u2022\u2022\u2022\u2022';
}
function toggleNewPw(){
  document.getElementById('net-p').type=document.getElementById('net-np-show').checked?'text':'password';
}
function slugHost(tag){
  let s=(tag||'').toLowerCase().replace(/[^a-z0-9]+/g,'-').replace(/^-+|-+$/g,'');
  if(!s) return 'lanternbrain';
  let h='lanternbrain-'+s;
  if(h.length>32) h=h.slice(0,32).replace(/-+$/,'');
  return h||'lanternbrain';
}
function previewTag(){
  const tag=document.getElementById('dev-tag').value.trim();
  const h=document.getElementById('h1-tag');
  if(document.activeElement!==h) h.textContent=tag||'Lantern Brain';
  const host=slugHost(tag)+'.local';
  document.getElementById('h1-host').textContent=host;
  document.getElementById('dev-host').textContent=host;
  document.title=tag||'Lantern Brain';
}
function tagFieldFocused(){
  const a=document.activeElement;
  return a&&(a.id==='dev-tag'||a.id==='h1-tag');
}
async function saveTag(){
  const fromTitle=document.activeElement&&document.activeElement.id==='h1-tag';
  const tag=(fromTitle?document.getElementById('h1-tag').textContent:document.getElementById('dev-tag').value).replace(/\s+/g,' ').trim();
  document.getElementById('dev-tag').value=tag;
  document.getElementById('h1-tag').textContent=tag||'Lantern Brain';
  const btn=document.getElementById('btn-tag');
  btn.disabled=true;btn.textContent='Saving\u2026';
  try{
    const r=await fetch('/tag',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({tag:tag})});
    const d=await r.json();
    if(!d.success){alert(d.error||'Could not save name');}
    else{
      document.getElementById('h1-tag').textContent=d.tag||'Lantern Brain';
      document.getElementById('dev-tag').value=d.tag||'';
      document.getElementById('h1-host').textContent=d.hostname+'.local';
      document.getElementById('dev-host').textContent=d.hostname+'.local';
      document.title=d.tag||'Lantern Brain';
      const m=document.getElementById('msg');
      if(d.reboot){
        m.textContent='Name saved \u2014 rebooting as '+d.hostname+'.local';
        m.style.display='block';
        btn.textContent='Rebooting\u2026';
        return;
      }
      m.textContent='Name saved';
      m.style.display='block';setTimeout(()=>m.style.display='none',2000);
    }
  }catch(e){alert('Failed to save name');}
  btn.disabled=false;btn.textContent='Save Name';
}
async function changeNetwork(){
  const s=document.getElementById('net-s').value.trim();
  const rawP=document.getElementById('net-p').value;
  // Blank password = keep the current saved password (e.g. same network, new SSID)
  const p=rawP.length>0?rawP:curPw;
  if(!s){alert('SSID cannot be empty.');return;}
  const pwNote=rawP.length===0&&curPw?' (using current password)':'';
  if(!confirm('Change to "'+s+'"'+pwNote+' and reboot?\nYou will need to reconnect to the new network.'))return;
  document.getElementById('btn-net').textContent='Rebooting\u2026';
  document.getElementById('btn-net').disabled=true;
  try{await fetch('/wifi',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({ssid:s,pass:p})});}
  catch(e){}
}
async function rescan(){
  await previewOff(true);
  const btn=document.getElementById('rescan');
  btn.disabled=true;btn.textContent='Scanning...';
  try{
    const r=await fetch('/rescan');const d=await r.json();
    document.getElementById('seg-count').textContent='('+d.segs+' segments)';
    await pollStatus();
  }catch(e){}
  btn.disabled=false;btn.textContent='\u21BB Refresh Segments';
}
async function apply(){
  const b=document.getElementById('apply');b.disabled=true;b.textContent='Saving\u2026';
  let ok=false;try{ok=await apply0();}catch(e){}
  b.textContent=ok?'\u2713 Saved':'Save failed';b.style.background=ok?'#2ecc71':'#e74c3c';
  setTimeout(()=>{b.disabled=false;b.textContent='Apply & Save';b.style.background='';},ok?2000:3000);
}
async function apply0(){
  const mc=document.getElementById('m-col').value;
  const onTm=t2hm(document.getElementById('sched-on').value);
  const offTm=t2hm(document.getElementById('sched-off').value);
  const body=JSON.stringify({
    briBright:+document.getElementById('m-bri').value,
    briAmbient:+document.getElementById('a-bri').value,
    fxBright:+document.getElementById('m-fx').value,
    psAmbient:+document.getElementById('a-ps').value,
    psMotion:+document.getElementById('m-ps').value,
    fxAmbient:+document.getElementById('a-fx').value,
    palAmbient:+document.getElementById('a-pal').value,
    colBrightR:parseInt(mc.slice(1,3),16),
    colBrightG:parseInt(mc.slice(3,5),16),
    colBrightB:parseInt(mc.slice(5,7),16),
    colBrightEnable:document.getElementById('m-col-en').checked,
    motionTimeout:document.getElementById('timeout').value*1000,
    waveDelay:+document.getElementById('wave').value,
    sensorMidSeg:+document.getElementById('mid-seg').value,
    scheduleMode:+document.getElementById('sched-mode').value,
    schedLat:+document.getElementById('sched-lat').value,
    schedLon:+document.getElementById('sched-lon').value,
    tzOffsetMin:+document.getElementById('sched-tz').value,
    schedOnHour:onTm.h,schedOnMin:onTm.m,
    schedOffHour:offTm.h,schedOffMin:offTm.m
  });
  const r=await fetch('/config',{method:'POST',headers:{'Content-Type':'application/json'},body});
  const d=await r.json();
  const m=document.getElementById('msg');
  m.textContent=d.success?'Settings applied!':'Error';
  m.style.display='block';setTimeout(()=>m.style.display='none',2000);
  if(d.success){
    const tag=document.getElementById('dev-tag').value.trim();
    try{
      const tr=await fetch('/tag',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({tag:tag})});
      const td=await tr.json();
      if(td&&td.reboot){
        m.textContent='Settings saved \u2014 rebooting as '+td.hostname+'.local';
        return true;
      }
    }catch(e){}
    await rescan();
  }
  return !!d.success;
}
let pvOn='';let syncCols=[];
async function loadPresets(pa,pm){
  try{
    const r=await fetch('/presets');const d=await r.json();
    ['a-ps','m-ps'].forEach(id=>{
      const sel=document.getElementById(id);
      while(sel.options.length>1)sel.remove(1);
      (d.presets||[]).forEach(p=>{const o=document.createElement('option');o.value=p.id;o.textContent=p.id+' \u00B7 '+String(p.name).trim();sel.appendChild(o);});
    });
  }catch(e){}
  setPs('a-ps',pa);setPs('m-ps',pm);psChanged();
}
function setPs(id,v){const s=document.getElementById(id);if(![...s.options].some(o=>+o.value===+v)){const o=document.createElement('option');o.value=v;o.textContent='Preset '+v+' (missing)';s.appendChild(o);}s.value=v;}
function psChanged(){
  const pa=+document.getElementById('a-ps').value,pm=+document.getElementById('m-ps').value;
  document.getElementById('a-fx').disabled=pa>0;document.getElementById('a-pal').disabled=pa>0;
  document.getElementById('m-fx').disabled=(pa>0||pm>0);
}
function pvColor(){
  if(syncCols.length){const c=syncCols.map(x=>'rgb('+x.join(',')+')');return c.length>1?'linear-gradient(90deg,'+c.join(',')+')':c[0];}
  return '#ff9f43';
}
function paintPv(){
  ['a','m'].forEach(k=>{const b=document.getElementById('preview-'+k);
    b.style.background=(pvOn===k)?pvColor():'#555';b.style.color=(pvOn===k)?'#1a1a2e':'#eee';
    b.textContent=(pvOn===k?'\u25CF ':'')+'Preview '+(k==='a'?'Ambient':'Motion')+(pvOn===k?' (on)':'');});
}
// Refresh calls this: previews off and return to the ambient preset (rescan re-applies ambient).
async function previewOff(viaRescan){
  const was=pvOn;pvOn='';paintPv();
  if(was&&!viaRescan)await fetch('/preview?mode=off');
}
async function preview(mode){
  if(pvOn===mode){await previewOff();return;}
  const bri=+document.getElementById(mode+'-bri').value;
  const fxv=+document.getElementById(mode+'-fx').value;
  const ps=+document.getElementById(mode+'-ps').value;
  let q='/preview?mode='+(mode==='a'?'ambient':'motion')+'&bri='+bri+'&fx='+fxv+'&ps='+ps;
  if(mode==='a')q+='&pal='+document.getElementById('a-pal').value;
  await fetch(q);
  pvOn=mode;paintPv();
}
async function syncPreset(){
  const b=document.getElementById('btn-sync');b.disabled=true;
  try{const r=await fetch('/presets/sync');const d=await r.json();
    if(d.success){syncCols=d.colors||[];b.textContent='\u2713 Synced '+syncCols.length+' lantern colors';}
    else b.textContent='Sync failed';
  }catch(e){b.textContent='Sync failed';}
  b.disabled=false;paintPv();
  setTimeout(()=>b.innerHTML='&#8645; Sync colors from WLED preset',3000);
}
function renderSegs(segs){
  const row=document.getElementById('seg-row');
  if(!segs)return;
  if(row.children.length!==segs.length){
    row.innerHTML='';
    segs.forEach((_,i)=>{const d=document.createElement('div');d.className='seg';d.textContent=i;row.appendChild(d);});
  }
  segs.forEach((on,i)=>{row.children[i].className='seg '+(on?'seg-on':'seg-off');});
}
async function pollStatus(){
  try{
    const r=await fetch('/status');const d=await r.json();
    const sim=d.sim===true;
    document.getElementById('sim-badge').style.display=sim?'inline':'none';
    if(!sim){holds={front:false,middle:false,back:false};['front','middle','back'].forEach(s=>document.getElementById('hold-'+s).classList.remove('on'));}
    const pir=sim?'SIM F:'+d.sim_f+' M:'+d.sim_m+' B:'+d.sim_b:'F:'+d.pir_f+' M:'+d.pir_m+' B:'+d.pir_b;
    document.getElementById('status').textContent=
      (d.tag?d.tag+' | ':'')+'Up:'+d.uptime_s+'s | '+pir+' | '+(d.motion?'MOTION '+d.dir:'idle')+' | '+d.idle_s+'s'+(d.wled?' | WLED '+d.wled:'');
    if(!tagFieldFocused()){
      if(d.tag) document.getElementById('h1-tag').textContent=d.tag;
      if(d.hostname){
        document.getElementById('h1-host').textContent=d.hostname+'.local';
        const dh=document.getElementById('dev-host');
        if(dh) dh.textContent=d.hostname+'.local';
      }
    }
    document.getElementById('seg-count').textContent='('+d.segs+' segments)';
    renderSegs(d.segs_bright);
  }catch(e){}
}
init();
</script></body></html>
)rawliteral";

// ── Setup ─────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(500);
  Log.printf("Lantern Brain boot — USB serial 115200 + GET /log  sha %s\n", FW_GIT_SHA_SHORT);

  pinMode(PIR_FRONT, INPUT_PULLDOWN);
  pinMode(PIR_MIDDLE, INPUT_PULLDOWN);
  pinMode(PIR_BACK, INPUT_PULLDOWN);
  memset(segBright, false, sizeof(segBright));

  loadSettings();
  loadWifiCreds();
  loadWledTarget();
  loadDeviceTag();

  Log.print("Connecting to WiFi");
  WiFi.begin(wifiSSID, wifiPass);
  {
    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
      delay(500);
      Log.print(".");
    }
  }
  if (WiFi.status() != WL_CONNECTED) startAPMode();  // never returns
  Log.println("\nConnected: " + WiFi.localIP().toString());
  // Persist working creds so a GitHub .bin (built from config.h.example) still joins.
  saveWifiCreds(wifiSSID, wifiPass);
  {
    String host = mdnsHostname();
    if (MDNS.begin(host.c_str())) Log.println("mDNS: " + host + ".local");
    ArduinoOTA.setHostname(host.c_str());
  }
  ArduinoOTA.onStart([]() {
    otaInProgress = true;
    WiFi.setSleep(false);
    Log.println("OTA start");
  });
  ArduinoOTA.onEnd([]() {
    otaInProgress = false;
    Log.println("OTA end");
  });
  ArduinoOTA.onError([](ota_error_t e) {
    otaInProgress = false;
    Log.printf("OTA error %u\n", e);
  });
  ArduinoOTA.begin();
  Log.printf("OTA: %s (Arduino IDE Network Port) or POST /update\n",
                mdnsHostname().c_str());

  syncNTP();  // sync time before registering endpoints

  // ── Web UI endpoint ──
  server.on("/", []() {
    String html = FPSTR(HTML_PAGE);
    html.replace("__DEVICE_TAG__", deviceTag.length() ? deviceTag : "Lantern Brain");
    html.replace("__MDNS_HOST__", mdnsHostname());
    server.send(200, "text/html; charset=utf-8", html);
  });

  // ── WiFi credential endpoint ──
  server.on("/wifi", HTTP_GET, []() {
    // Return current SSID and password (home LAN device — user-requested)
    StaticJsonDocument<256> wdoc;
    wdoc["ssid"] = wifiSSID;
    wdoc["pass"] = wifiPass;
    String wjson; serializeJson(wdoc, wjson);
    server.send(200, "application/json", wjson);
  });
  // ── WiFi scan endpoint ──
  server.on("/wifi/scan", HTTP_GET, []() {
    Log.println("WiFi scan starting (~3s)...");
    int n = WiFi.scanNetworks();   // blocking, ~2-4 s
    Log.printf("Scan found %d networks\n", n);
    StaticJsonDocument<2048> sdoc;
    JsonArray sarr = sdoc.to<JsonArray>();
    for (int i = 0; i < n; i++) {
      if (WiFi.SSID(i).length() == 0) continue;  // skip hidden
      // Deduplicate: keep entry with strongest signal
      bool dup = false;
      for (JsonObject obj : sarr) {
        if (obj["ssid"].as<String>() == WiFi.SSID(i)) {
          if (WiFi.RSSI(i) > obj["rssi"].as<int>()) obj["rssi"] = WiFi.RSSI(i);
          dup = true; break;
        }
      }
      if (!dup) {
        JsonObject net = sarr.createNestedObject();
        net["ssid"] = WiFi.SSID(i);
        net["rssi"] = WiFi.RSSI(i);
      }
    }
    WiFi.scanDelete();
    String sjson; serializeJson(sdoc, sjson);
    server.send(200, "application/json", sjson);
  });
  server.on("/wifi", HTTP_POST, []() {
    StaticJsonDocument<256> doc;
    DeserializationError err = deserializeJson(doc, server.arg("plain"));
    if (err || !doc.containsKey("ssid")) {
      server.send(400, "application/json", "{\"success\":false}"); return;
    }
    String newSSID = doc["ssid"] | "";
    String newPass = doc["pass"] | "";
    if (newSSID.length() == 0) {
      server.send(400, "application/json", "{\"success\":false,\"error\":\"empty ssid\"}"); return;
    }
    saveWifiCreds(newSSID, newPass);
    server.send(200, "application/json", "{\"success\":true}");
    delay(500); ESP.restart();
  });

  // ── WLED target ──
  server.on("/wled", HTTP_GET, []() {
    String name, ver;
    int leds = 0;
    bool ok = probeWled(wledIP, name, leds, ver);
    if (ok && name.length()) wledName = name;
    StaticJsonDocument<384> doc;
    doc["ip"] = wledIP;
    doc["name"] = wledName;
    doc["reachable"] = ok;
    doc["leds"] = leds;
    doc["ver"] = ver;
    String json; serializeJson(doc, json);
    server.send(200, "application/json", json);
  });
  server.on("/wled", HTTP_POST, []() {
    StaticJsonDocument<256> doc;
    if (deserializeJson(doc, server.arg("plain"))) {
      server.send(400, "application/json", "{\"success\":false,\"error\":\"bad json\"}");
      return;
    }
    String ip = doc["ip"] | "";
    ip.trim();
    if (!isValidIPv4(ip)) {
      server.send(400, "application/json", "{\"success\":false,\"error\":\"invalid ip\"}");
      return;
    }
    String postedName = doc["name"] | "";
    String name, ver;
    int leds = 0;
    bool ok = probeWled(ip, name, leds, ver);
    wledIP = ip;
    if (name.length()) wledName = name;
    else if (postedName.length()) wledName = postedName;
    saveWledTarget();
    if (ok) refreshSegmentsFromWled();
    char buf[192];
    snprintf(buf, sizeof(buf),
      "{\"success\":true,\"ip\":\"%s\",\"name\":\"%s\",\"reachable\":%s,\"segs\":%d}",
      wledIP.c_str(), wledName.c_str(), ok ? "true" : "false", numSegments);
    server.send(200, "application/json", buf);
  });
  server.on("/wled/scan", HTTP_GET, []() {
    wledFoundClear();
    probeAndAdd(wledIP, "saved");
    probeAndAdd(String(WLED_IP), "default");
    if (wledIP.length() && !wledFoundHas(wledIP))
      wledFoundAdd(wledIP, wledName.length() ? wledName : String("current"),
                   0, "", "saved");
    scanWledMdns();
    bool full = server.hasArg("full") && server.arg("full") != "0";
    if (full) scanWledLan();

    StaticJsonDocument<2048> doc;
    doc["ip"] = wledIP;
    doc["name"] = wledName;
    JsonArray hosts = doc.createNestedArray("hosts");
    for (int i = 0; i < wledFoundCount; i++) {
      JsonObject o = hosts.createNestedObject();
      o["ip"]     = wledFound[i].ip;
      o["name"]   = wledFound[i].name;
      o["ver"]    = wledFound[i].ver;
      o["leds"]   = wledFound[i].leds;
      o["source"] = wledFound[i].source;
    }
    String json; serializeJson(doc, json);
    server.send(200, "application/json", json);
  });

  // ── Device tag ──
  server.on("/tag", HTTP_GET, []() {
    StaticJsonDocument<192> doc;
    doc["tag"] = deviceTag;
    doc["hostname"] = mdnsHostname();
    String json; serializeJson(doc, json);
    server.send(200, "application/json", json);
  });
  server.on("/tag", HTTP_POST, []() {
    StaticJsonDocument<192> doc;
    if (deserializeJson(doc, server.arg("plain"))) {
      server.send(400, "application/json", "{\"success\":false,\"error\":\"bad json\"}");
      return;
    }
    String oldHost = mdnsHostname();
    String newTag  = sanitizeTag(doc["tag"] | "");
    deviceTag = newTag;
    saveDeviceTag();
    String host = mdnsHostname();
    bool reboot = (host != oldHost);
    StaticJsonDocument<256> out;
    out["success"] = true;
    out["tag"] = deviceTag;
    out["hostname"] = host;
    out["reboot"] = reboot;
    String json; serializeJson(out, json);
    server.send(200, "application/json", json);
    if (reboot) {
      delay(400);
      ESP.restart();
    }
  });

  // ── Firmware OTA (browser upload) ──
  server.on("/update/progress", HTTP_GET, []() {
    uint32_t tot = otaTotal, n = otaBytes;
    int pct = (tot > 0) ? (int)((n * 100ULL) / tot) : 0;
    StaticJsonDocument<256> doc;
    doc["running"] = otaInProgress;
    doc["phase"] = otaPhase;
    doc["bytes"] = n;
    doc["total"] = tot;
    doc["pct"] = pct;
    doc["error"] = otaErrBuf;
    String json; serializeJson(doc, json);
    server.send(200, "application/json", json);
  });
  server.on("/update", HTTP_GET, []() {
    StaticJsonDocument<512> doc;
    doc["ota"] = true;
    doc["hostname"] = mdnsHostname();
    doc["tag"] = deviceTag;
    doc["url"] = FW_URL_DEFAULT;
    doc["built"] = String(__DATE__) + " " + __TIME__;
    doc["sha"] = FW_GIT_SHA;
    doc["sha_short"] = FW_GIT_SHA_SHORT;
    String json; serializeJson(doc, json);
    server.send(200, "application/json", json);
  });
  server.on("/update/check", HTTP_GET, []() {
    StaticJsonDocument<512> out;
    out["current"] = FW_GIT_SHA;
    out["current_short"] = FW_GIT_SHA_SHORT;
    out["built"] = String(__DATE__) + " " + __TIME__;
    out["url"] = FW_URL_DEFAULT;
    out["latest"] = "";
    out["latest_short"] = "";
    out["update"] = false;
    out["checked"] = false;

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.setTimeout(8000);
    http.setUserAgent("LanternBrain");
    if (http.begin(client, FW_VERSION_URL)) {
      int code = http.GET();
      if (code == 200) {
        String body = http.getString();
        StaticJsonDocument<384> ver;
        if (!deserializeJson(ver, body)) {
          String latest = ver["sha"] | "";
          String latestShort = ver["sha_short"] | latest.substring(0, 7);
          out["latest"] = latest;
          out["latest_short"] = latestShort;
          out["latest_built"] = ver["built"] | "";
          out["checked"] = true;
          String cur = String(FW_GIT_SHA);
          bool unknown = (cur == "dev" || cur.length() < 7);
          bool match = false;
          if (!unknown && latest.length() >= 7) {
            match = true;
            for (int i = 0; i < 7; i++) {
              char a = cur[i], b = latest[i];
              if (a >= 'A' && a <= 'Z') a += 32;
              if (b >= 'A' && b <= 'Z') b += 32;
              if (a != b) { match = false; break; }
            }
          }
          out["update"] = latest.length() > 0 && (unknown || !match);
        }
      } else {
        out["error"] = code;
      }
      http.end();
    }
    String json; serializeJson(out, json);
    server.send(200, "application/json", json);
  });
  server.on("/update/url", HTTP_POST, []() {
    if (otaInProgress) {
      server.send(409, "application/json", "{\"success\":false,\"error\":\"ota running\"}");
      return;
    }
    StaticJsonDocument<512> doc;
    String url = FW_URL_DEFAULT;
    if (!deserializeJson(doc, server.arg("plain"))) {
      String u = doc["url"] | "";
      u.trim();
      if (u.startsWith("http://") || u.startsWith("https://")) url = u;
    }
    if (url.length() >= (int)sizeof(otaUrlBuf)) {
      server.send(400, "application/json", "{\"success\":false,\"error\":\"url too long\"}");
      return;
    }
    strncpy(otaUrlBuf, url.c_str(), sizeof(otaUrlBuf) - 1);
    otaUrlBuf[sizeof(otaUrlBuf) - 1] = 0;
    otaBytes = 0;
    otaTotal = 0;
    otaErrBuf[0] = 0;
    otaPhase = "download";
    otaInProgress = true;
    Log.printf("OTA from URL: %s\n", otaUrlBuf);
    server.send(200, "application/json", "{\"success\":true,\"started\":true}");
    xTaskCreate(firmwareOtaTask, "fwota", 16384, NULL, 1, NULL);
  });
  server.on("/update", HTTP_POST, []() {
    if (Update.hasError()) {
      otaInProgress = false;
      server.send(500, "application/json", "{\"success\":false}");
    } else {
      server.send(200, "application/json", "{\"success\":true,\"reboot\":true}");
      delay(400);
      ESP.restart();
    }
  }, []() {
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
      otaInProgress = true;
      otaPhase = "upload";
      otaBytes = 0;
      otaTotal = upload.totalSize;
      otaErrBuf[0] = 0;
      WiFi.setSleep(false);
      Log.printf("HTTP update: %s\n", upload.filename.c_str());
      if (!Update.begin(UPDATE_SIZE_UNKNOWN)) Update.printError(Log);
    } else if (upload.status == UPLOAD_FILE_WRITE) {
      otaBytes += upload.currentSize;
      otaTotal = upload.totalSize;
      otaPhase = "flash";
      if (Update.write(upload.buf, upload.currentSize) != upload.currentSize)
        Update.printError(Log);
    } else if (upload.status == UPLOAD_FILE_END) {
      if (Update.end(true))
        Log.printf("HTTP update success: %u bytes\n", upload.totalSize);
      else
        Update.printError(Log);
    } else if (upload.status == UPLOAD_FILE_ABORTED) {
      Update.end();
      otaInProgress = false;
      Log.println("HTTP update aborted");
    }
  });

  server.on("/geo", HTTP_GET, []() {
    HTTPClient http;
    http.setTimeout(6000);
    http.setUserAgent("LanternBrain");
    if (!http.begin("http://ip-api.com/json/?fields=status,message,lat,lon,city,regionName,country")) {
      server.send(502, "application/json", "{\"ok\":false,\"error\":\"begin failed\"}");
      return;
    }
    int code = http.GET();
    String body = (code > 0) ? http.getString() : "";
    http.end();
    StaticJsonDocument<256> out;
    out["ok"] = false;
    if (code == 200) {
      StaticJsonDocument<384> geo;
      if (!deserializeJson(geo, body) && String(geo["status"] | "") == "success") {
        out["ok"] = true;
        out["lat"] = geo["lat"];
        out["lon"] = geo["lon"];
        out["city"] = geo["city"] | "";
        out["region"] = geo["regionName"] | "";
        out["country"] = geo["country"] | "";
      } else {
        out["error"] = geo["message"] | "lookup failed";
      }
    } else {
      out["error"] = code;
    }
    String json; serializeJson(out, json);
    server.send(200, "application/json", json);
  });

  // ── Schedule endpoint ──
  server.on("/schedule", []() {
    struct tm ti;
    bool hasTm = getLocalTime(&ti);
    char timeBuf[6] = "--:--";  char riseBuf[6] = "--:--";  char setBuf[6] = "--:--";
    if (hasTm) {
      snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d", ti.tm_hour, ti.tm_min);
      if (scheduleMode == 1 && !(schedLat == 0.0f && schedLon == 0.0f)) {
        int   doy  = ti.tm_yday + 1;
        float rise = calcSunEvent(schedLat, schedLon, doy, tzOffsetMin, true);
        float set  = calcSunEvent(schedLat, schedLon, doy, tzOffsetMin, false);
        if (rise >= 0) snprintf(riseBuf, sizeof(riseBuf), "%02d:%02d",
                                (int)rise, (int)((rise-(int)rise)*60+0.5f));
        if (set  >= 0) snprintf(setBuf,  sizeof(setBuf),  "%02d:%02d",
                                (int)set,  (int)((set -(int)set )*60+0.5f));
      }
    }
    char buf[200];
    snprintf(buf, sizeof(buf),
      "{\"mode\":%d,\"active\":%s,\"ntpSync\":%s,"
      "\"localTime\":\"%s\",\"sunrise\":\"%s\",\"sunset\":\"%s\"}",
      scheduleMode,
      isActiveSchedule() ? "true" : "false",
      ntpSynced ? "true" : "false",
      timeBuf, riseBuf, setBuf);
    server.send(200, "application/json", buf);
  });

  // ── Config GET ──
  server.on("/config", HTTP_GET, []() {
    char buf[960];
    snprintf(buf, sizeof(buf),
      "{\"psAmbient\":%d,\"psMotion\":%d,\"psAmbientName\":\"%s\",\"psMotionName\":\"%s\",\"briBright\":%d,\"briAmbient\":%d,\"fxBright\":%d,\"fxAmbient\":%d,"
      "\"palAmbient\":%d,"
      "\"colBrightR\":%d,\"colBrightG\":%d,\"colBrightB\":%d,"
      "\"colBrightEnable\":%s,"
      "\"motionTimeout\":%lu,\"waveDelay\":%d,\"sensorMidSeg\":%d,"
      "\"scheduleMode\":%d,\"schedLat\":%.6f,\"schedLon\":%.6f,"
      "\"tzOffsetMin\":%d,"
      "\"schedOnHour\":%d,\"schedOnMin\":%d,"
      "\"schedOffHour\":%d,\"schedOffMin\":%d}",
      psAmbient, psMotion,
      presetLabel(psAmbient, psAmbientName).c_str(), presetLabel(psMotion, psMotionName).c_str(),
      briBright, briAmbient, fxBright, fxAmbient, palAmbient,
      colBrightR, colBrightG, colBrightB,
      colBrightEnable ? "true" : "false",
      motionTimeout, waveDelay, sensorMidSeg,
      scheduleMode, schedLat, schedLon,
      tzOffsetMin,
      schedOnHour, schedOnMin,
      schedOffHour, schedOffMin);
    server.send(200, "application/json", buf);
  });

  // ── Config POST ──
  server.on("/config", HTTP_POST, []() {
    StaticJsonDocument<1024> doc;
    DeserializationError err = deserializeJson(doc, server.arg("plain"));
    if (err) {
      server.send(400, "application/json", "{\"success\":false}");
      return;
    }
    bool prevColBrightEnable = colBrightEnable;
    int  prevTzOffsetMin     = tzOffsetMin;
    if (doc.containsKey("psAmbient"))    psAmbient     = max(0, (int)doc["psAmbient"]);
    if (doc.containsKey("psMotion"))     psMotion      = max(0, (int)doc["psMotion"]);
    if (doc.containsKey("briBright"))    briBright     = doc["briBright"];
    if (doc.containsKey("briAmbient"))   briAmbient    = doc["briAmbient"];
    if (doc.containsKey("fxBright"))     fxBright      = doc["fxBright"];
    if (doc.containsKey("fxAmbient"))    fxAmbient     = doc["fxAmbient"];
    if (doc.containsKey("palAmbient"))   palAmbient    = doc["palAmbient"];
    if (doc.containsKey("colBrightR"))      colBrightR      = doc["colBrightR"];
    if (doc.containsKey("colBrightG"))      colBrightG      = doc["colBrightG"];
    if (doc.containsKey("colBrightB"))      colBrightB      = doc["colBrightB"];
    if (doc.containsKey("colBrightEnable")) colBrightEnable = doc["colBrightEnable"];
    if (doc.containsKey("motionTimeout")) motionTimeout = doc["motionTimeout"];
    if (doc.containsKey("waveDelay"))    waveDelay     = doc["waveDelay"];
    if (doc.containsKey("sensorMidSeg")) sensorMidSeg  = constrain(doc["sensorMidSeg"], 1, numSegments - 1);
    if (doc.containsKey("scheduleMode")) scheduleMode  = doc["scheduleMode"];
    if (doc.containsKey("schedLat"))     schedLat      = doc["schedLat"];
    if (doc.containsKey("schedLon"))     schedLon      = doc["schedLon"];
    if (doc.containsKey("tzOffsetMin"))  tzOffsetMin   = doc["tzOffsetMin"];
    if (doc.containsKey("schedOnHour"))  schedOnHour   = doc["schedOnHour"];
    if (doc.containsKey("schedOnMin"))   schedOnMin    = doc["schedOnMin"];
    if (doc.containsKey("schedOffHour")) schedOffHour  = doc["schedOffHour"];
    if (doc.containsKey("schedOffMin"))  schedOffMin   = doc["schedOffMin"];

    saveSettings();  // persist all settings to NVS flash
    if (doc.containsKey("psAmbient") || doc.containsKey("psMotion")) refreshPresetNames();
    if (tzOffsetMin != prevTzOffsetMin) syncNTP();  // re-sync if timezone changed

    // Interrupt any active wave, then apply ambient (or off if schedule inactive)
    waveState = WAVE_IDLE; dimDone = false; clearWavePool();
    if (!motionActive) {
      if (!isActiveSchedule()) {
        turnAllOff();
      } else if (colBrightEnable && !prevColBrightEnable) {
        saveSegmentColors();
        applyAmbient();
      } else if (!colBrightEnable && prevColBrightEnable) {
        restoreSegmentColors();
      } else {
        applyAmbient();
      }
    }

    server.send(200, "application/json", "{\"success\":true}");
  });

  // ── Preview endpoint ──
  // mode=ambient|motion|off uses the same code path as the real modes. Optional bri/fx/pal/ps
  // args preview unsaved on-screen values (restored afterwards). No mode = legacy raw paint.
  server.on("/preview", []() {
    String mode = server.arg("mode");
    if (mode.length() == 0) {
      int bri = server.arg("bri").toInt();
      int fx  = server.arg("fx").toInt();
      int pal = server.hasArg("pal") ? server.arg("pal").toInt() : -1;
      for (int i = 0; i < numSegments; i++) setSegment(i, bri, fx, pal);
      server.send(200, "application/json", "{\"success\":true}");
      return;
    }
    int sv[6] = {briAmbient, fxAmbient, palAmbient, psAmbient, briBright, psMotion};
    int svFxB = fxBright;
    bool amb = (mode == "ambient" || mode == "a");
    bool mot = (mode == "motion" || mode == "bright" || mode == "m");
    if (amb) {
      if (server.hasArg("bri")) briAmbient = server.arg("bri").toInt();
      if (server.hasArg("fx"))  fxAmbient  = server.arg("fx").toInt();
      if (server.hasArg("pal")) palAmbient = server.arg("pal").toInt();
      if (server.hasArg("ps"))  psAmbient  = server.arg("ps").toInt();
    } else if (mot) {
      if (server.hasArg("bri")) briBright = server.arg("bri").toInt();
      if (server.hasArg("fx"))  fxBright  = server.arg("fx").toInt();
      if (server.hasArg("ps"))  psMotion  = server.arg("ps").toInt();
    }
    waveState = WAVE_IDLE; dimDone = false; clearWavePool();
    motionActive = false; lastSensor = S_NONE;
    if (mot) {
      applyAmbient();          // start from the real ambient state
      startMotionLook();       // snap to motion preset if different
      for (int i = 0; i < numSegments; i++) lightSeg(i);
    } else if (isActiveSchedule()) {
      applyAmbient();          // ambient or off
    } else {
      turnAllOff();
    }
    briAmbient = sv[0]; fxAmbient = sv[1]; palAmbient = sv[2]; psAmbient = sv[3];
    briBright = sv[4]; psMotion = sv[5]; fxBright = svFxB;
    if (mot) { for (int i = 0; i < numSegments; i++) segBright[i] = false; litCount = 0; }
    server.send(200, "application/json", "{\"success\":true}");
  });

  // ── WLED presets list: proxies /presets.json, returns [{id,name}] ──
  server.on("/presets", HTTP_GET, []() {
    DynamicJsonDocument out(3072);
    int code = fetchPresetList(out);
    if (code != 200) {
      server.send(code, "application/json", "{\"success\":false,\"presets\":[]}");
      return;
    }
    out["success"] = true;
    String body;
    serializeJson(out, body);
    server.send(200, "application/json", body);
  });

  // ── Sync: pull per-lantern colors from WLED's current (preset) state into the Brain ──
  server.on("/presets/sync", []() {
    colsSaved = false;
    saveSegmentColors();
    String body = "{\"success\":";
    body += colsSaved ? "true" : "false";
    body += ",\"colors\":[";
    for (int i = 0; i < numSegments && colsSaved; i++) {
      char c[24];
      snprintf(c, sizeof(c), "%s[%d,%d,%d]", i ? "," : "", savedColR[i], savedColG[i], savedColB[i]);
      body += c;
    }
    body += "]}";
    server.send(200, "application/json", body);
  });

  // ── Simulate endpoint ──
  server.on("/simulate", []() {
    String sensor = server.arg("sensor");

    if (sensor == "clear") {
      simMode = false;
      simFrontHold = simMiddleHold = simBackHold = false;
      simFrontPulse = simMiddlePulse = simBackPulse = 0;
      // Reset edge-detection to real sensor states to avoid false rises
      prevFront  = digitalRead(PIR_FRONT)  == HIGH;
      prevMiddle = digitalRead(PIR_MIDDLE) == HIGH;
      prevBack   = digitalRead(PIR_BACK)   == HIGH;
      server.send(200, "application/json",
                  "{\"success\":true,\"sim\":false,\"front\":0,\"middle\":0,\"back\":0}");
      Log.println("Simulate: cleared");
      return;
    }

    // First activation — reset edge detection so trigger fires cleanly
    if (!simMode) {
      simMode = true;
      prevFront = prevMiddle = prevBack = false;
    }

    if (server.hasArg("hold")) {
      bool h = server.arg("hold") == "1";
      if (sensor == "front")  { simFrontHold  = h; if (!h) simFrontPulse  = 0; }
      if (sensor == "middle") { simMiddleHold = h; if (!h) simMiddlePulse = 0; }
      if (sensor == "back")   { simBackHold   = h; if (!h) simBackPulse   = 0; }
    } else {
      // Pulse: 4 loop cycles (~400 ms HIGH), guarantees a clean rising edge
      if (sensor == "front")  { prevFront  = false; simFrontPulse  = 4; }
      if (sensor == "middle") { prevMiddle = false; simMiddlePulse = 4; }
      if (sensor == "back")   { prevBack   = false; simBackPulse   = 4; }
    }

    int f = simFrontHold  || simFrontPulse  > 0 ? 1 : 0;
    int m = simMiddleHold || simMiddlePulse > 0 ? 1 : 0;
    int b = simBackHold   || simBackPulse   > 0 ? 1 : 0;
    char buf[96];
    snprintf(buf, sizeof(buf),
             "{\"success\":true,\"sim\":true,\"front\":%d,\"middle\":%d,\"back\":%d}",
             f, m, b);
    server.send(200, "application/json", buf);
    Log.printf("Simulate: %s → F:%d M:%d B:%d\n", sensor.c_str(), f, m, b);
  });

  // ── Rescan endpoint ──
  server.on("/rescan", []() {
    int n = detectSegments();
    bool ok = (n > 0 && n <= MAX_SEG);
    if (ok) {
      numSegments = n;
      if (sensorMidSeg <= 0 || sensorMidSeg >= numSegments)
        sensorMidSeg = numSegments / 2;  // re-auto if out of range
      Log.printf("Rescan: %d segments, mid=%d\n", numSegments, sensorMidSeg);
      waveState = WAVE_IDLE; dimDone = false; clearWavePool();
      if (isActiveSchedule()) applyAmbient(); else turnAllOff();
    } else {
      Log.printf("Rescan failed (got %d) — keeping %d\n", n, numSegments);
    }
    char buf[64];
    snprintf(buf, sizeof(buf), "{\"segs\":%d,\"success\":%s}",
             numSegments, ok ? "true" : "false");
    server.send(200, "application/json", buf);
  });

  // ── Status endpoint ──
  server.on("/status", []() {
    char buf[960];
    unsigned long now = millis();
    unsigned long idle = motionActive ? (now - lastAnyMotionTime) / 1000 : 0;
    const char* dirStr = "none";
    if (lastSensor == S_FRONT) dirStr = "front";
    else if (lastSensor == S_MIDDLE) dirStr = "middle";
    else if (lastSensor == S_BACK) dirStr = "back";

    // Effective sensor states (real or simulated)
    int effF = simMode ? (simFrontHold  || simFrontPulse  > 0 ? 1 : 0)
                       : (digitalRead(PIR_FRONT)  == HIGH ? 1 : 0);
    int effM = simMode ? (simMiddleHold || simMiddlePulse > 0 ? 1 : 0)
                       : (digitalRead(PIR_MIDDLE) == HIGH ? 1 : 0);
    int effB = simMode ? (simBackHold   || simBackPulse   > 0 ? 1 : 0)
                       : (digitalRead(PIR_BACK)   == HIGH ? 1 : 0);

    // Build segs_bright array: [true,false,true,...]
    char segArr[128] = "[";
    for (int i = 0; i < numSegments; i++) {
      if (i > 0) strcat(segArr, ",");
      strcat(segArr, segBright[i] ? "true" : "false");
    }
    strcat(segArr, "]");

    snprintf(buf, sizeof(buf),
      "{\"uptime_s\":%lu,\"motion\":%s,\"idle_s\":%lu,"
      "\"pir_f\":%d,\"pir_m\":%d,\"pir_b\":%d,"
      "\"sim\":%s,\"sim_f\":%d,\"sim_m\":%d,\"sim_b\":%d,"
      "\"dir\":\"%s\",\"segs\":%d,\"segs_bright\":%s,\"ip\":\"%s\","
      "\"wled\":\"%s\",\"tag\":\"%s\",\"hostname\":\"%s\","
      "\"heap\":%lu,\"heap_min\":%lu}",
      now / 1000,
      motionActive ? "true" : "false",
      idle,
      effF, effM, effB,
      simMode ? "true" : "false",
      simFrontHold  || simFrontPulse  > 0 ? 1 : 0,
      simMiddleHold || simMiddlePulse > 0 ? 1 : 0,
      simBackHold   || simBackPulse   > 0 ? 1 : 0,
      dirStr, numSegments, segArr,
      WiFi.localIP().toString().c_str(),
      wledIP.c_str(),
      deviceTag.c_str(),
      mdnsHostname().c_str(),
      (unsigned long)ESP.getFreeHeap(),
      (unsigned long)ESP.getMinFreeHeap());
    server.send(200, "application/json", buf);
  });

  attachHttpLogMiddleware();
  attachLogRoutes();
  server.begin();

  // Auto-detect segments from WLED
  numSegments = detectSegments();
  Log.printf("Detected %d WLED segments\n", numSegments);
  if (numSegments == 0) {
    Log.println("WARNING: No segments detected — check WLED");
    numSegments = 1;
  } else if (wledIP.indexOf("XXX") < 0) {
    saveWledTarget();
  }
  // Apply default middle segment if not stored or out of range
  if (sensorMidSeg <= 0 || sensorMidSeg >= numSegments)
    sensorMidSeg = numSegments / 2;
  Log.printf("Sensor mid segment: %d\n", sensorMidSeg);

  // Start segments according to schedule
  schedWasActive = isActiveSchedule();
  if (schedWasActive) {
    Log.println("Schedule active — setting ambient");
    refreshPresetNames();
    applyAmbient();
    if (psAmbient > 0) saveSegmentColors();  // remember the preset's per-lantern colors
  } else {
    Log.println("Schedule inactive — lights off");
    turnAllOff();
  }
}

// ── Main loop ───────────────────────────────────────────────────────────
void loop() {
  maintainMemory();

  // 0a. Daily NTP resync
  if (ntpSynced && (millis() - lastNtpSync > 86400000UL)) syncNTP();

  // 0b. Schedule gate — skip motion processing outside active window
  bool schedActive = isActiveSchedule();
  if (!schedActive &&  schedWasActive) turnAllOff();
  if ( schedActive && !schedWasActive) applyAmbient();
  schedWasActive = schedActive;
  if (!schedActive) {
    server.handleClient();
    ArduinoOTA.handle();
    delay(50);
    return;
  }

  if (otaInProgress) {
    server.handleClient();
    ArduinoOTA.handle();
    delay(10);
    return;
  }

  // 1. Advance bright-wave pool + dim state machine (one HTTP call per tick total)
  tickWaves();
  tickDim();

  // 2. Decrement simulation pulse counters
  if (simFrontPulse  > 0) simFrontPulse--;
  if (simMiddlePulse > 0) simMiddlePulse--;
  if (simBackPulse   > 0) simBackPulse--;

  // 3. Read sensors (real or simulated)
  bool front  = simMode ? (simFrontHold  || simFrontPulse  > 0)
                        : (digitalRead(PIR_FRONT)  == HIGH);
  bool middle = simMode ? (simMiddleHold || simMiddlePulse > 0)
                        : (digitalRead(PIR_MIDDLE) == HIGH);
  bool back   = simMode ? (simBackHold   || simBackPulse   > 0)
                        : (digitalRead(PIR_BACK)   == HIGH);
  bool anyMotion = front || middle || back;
  if (anyMotion) lastAnyMotionTime = millis();

  // 4. Detect rising edges
  bool frontRise  = front  && !prevFront;
  bool middleRise = middle && !prevMiddle;
  bool backRise   = back   && !prevBack;
  prevFront = front; prevMiddle = middle; prevBack = back;

  int mid = sensorMidSeg;  // configured middle sensor position

  // 5. Reset motion state after trailing dim+ambient sweep completes
  if (dimDone) {
    dimDone = false;
    motionActive = false;
    lastSensor = S_NONE;
  }

  // 6. Concurrent wave dispatch — every rising edge adds a new independent pool wave.
  bool dimInProgress = (waveState == WAVE_DIM_ORDER || waveState == WAVE_APPLY_AMBIENT);

  if (frontRise || middleRise || backRise) {
    // Fresh start: system was idle or mid-dim — wipe state and start clean
    if (!motionActive || dimInProgress) {
      litCount  = 0;
      memset(segBright, false, sizeof(segBright));
      waveState = WAVE_IDLE;
      dimDone   = false;
      clearWavePool();
      if (!motionActive) startMotionLook();  // preset swap once per motion episode
    }
    // Each triggered sensor starts its own concurrent pool wave
    if (frontRise) {
      startPoolWaveBright(0, numSegments - 1);
      motionActive = true; lastSensor = S_FRONT;
      Log.println("Front → pool wave 0→N");
    }
    if (middleRise) {
      startPoolWaveBrightFromMid(mid);
      motionActive = true; lastSensor = S_MIDDLE;
      Log.println("Middle → pool wave outward");
    }
    if (backRise) {
      startPoolWaveBright(numSegments - 1, 0);
      motionActive = true; lastSensor = S_BACK;
      Log.println("Back → pool wave N→0");
    }
  }

  // 7. Timeout trailing dim — start only when sensors quiet AND all pool waves done
  if (motionActive && !anyMotion && allWavesDone() && waveState == WAVE_IDLE &&
      (millis() - lastAnyMotionTime >= motionTimeout)) {
    Log.println("Timeout → trailing dim");
    startDimInOrder();
  }

  server.handleClient();
  ArduinoOTA.handle();
  delay(50);  // shorter: waves no longer block the loop
}
