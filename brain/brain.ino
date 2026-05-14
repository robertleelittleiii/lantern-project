/*
 * Lantern Brain — Sequential Segment Test (1 PIR, 2 lanterns)
 * Hardware: ESP32 DevKit
 *
 * Wiring:
 *  - PIR HC-SR501  OUT --> GPIO 27
 *  - PIR HC-SR501  VCC --> 5V
 *  - PIR HC-SR501  GND --> GND
 *
 * WLED Segments (configure in WLED UI):
 *  - Segment 0: LEDs 0–7   (lantern 1)
 *  - Segment 1: LEDs 8–15  (lantern 2)
 *
 * Web UI: http://<brain-ip>/  — configure effects and timing
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <WebServer.h>
#include "config.h"

// ── Fixed Settings ───────────────────────────────────────────────────────────
const int PIR_PIN      = 27;
int       numSegments  = 0;  // auto-detected from WLED at startup

// ── Tunable Settings (changeable via web UI) ─────────────────────────────────
int           briBright    = 255;
int           briAmbient   = 255;
int           fxBright     = 0;      // Solid
int           fxAmbient    = 66;     // Fire 2012
unsigned long motionTimeout = 10000; // ms
int           waveDelay    = 500;    // ms between segment changes

// ── State ─────────────────────────────────────────────────────────────────────
bool  motionActive   = false;
unsigned long lastMotionTime = 0;
WebServer server(80);

// ── Helpers ───────────────────────────────────────────────────────────────────
void ensureWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  Serial.print("Reconnecting to WiFi");
  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
    delay(500);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nReconnected: " + WiFi.localIP().toString());
  } else {
    Serial.println("\nWiFi reconnect failed — will retry next cycle");
  }
}

// Set brightness and effect on a single WLED segment
void setSegment(int segId, int bri, int fx) {
  ensureWiFi();
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  char url[64];
  snprintf(url, sizeof(url), "http://%s/json/state", WLED_IP);

  StaticJsonDocument<128> doc;
  JsonArray seg = doc.createNestedArray("seg");
  JsonObject s = seg.createNestedObject();
  s["id"] = segId;
  s["bri"] = bri;
  s["fx"] = fx;
  String body;
  serializeJson(doc, body);

  http.begin(url);
  http.setTimeout(5000);
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(body);

  if (code > 0) {
    http.getString();
    Serial.printf("Seg %d → bri %d fx %d (HTTP %d)\n", segId, bri, fx, code);
  } else {
    Serial.printf("WLED POST failed: %s\n", http.errorToString(code).c_str());
  }
  http.end();
  delay(10);
}

void waveOn() {
  for (int i = 0; i < numSegments; i++) {
    setSegment(i, briBright, fxBright);
    if (i < numSegments - 1) delay(waveDelay);
  }
}

void waveOff() {
  for (int i = 0; i < numSegments; i++) {
    setSegment(i, briAmbient, fxAmbient);
    if (i < numSegments - 1) delay(waveDelay);
  }
}

void applyAmbient() {
  for (int i = 0; i < numSegments; i++) {
    setSegment(i, briAmbient, fxAmbient);
  }
}

// Query WLED for segment count
int detectSegments() {
  ensureWiFi();
  if (WiFi.status() != WL_CONNECTED) return 0;

  HTTPClient http;
  char url[64];
  snprintf(url, sizeof(url), "http://%s/json/state", WLED_IP);

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
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Lantern Brain</title>
<style>
body{font-family:sans-serif;max-width:480px;margin:20px auto;padding:0 15px;background:#1a1a2e;color:#eee}
h1{color:#ff9f43;font-size:1.4em}h2{color:#aaa;font-size:1em;margin-top:18px}
.card{background:#16213e;border-radius:8px;padding:12px 16px;margin:8px 0}
label{display:block;margin:8px 0 4px;font-size:.85em;color:#ccc}
select,input[type=range]{width:100%;margin:2px 0}
.val{float:right;color:#ff9f43}
button{width:100%;padding:12px;margin:8px 0;border:none;border-radius:6px;font-size:1em;cursor:pointer}
#apply{background:#ff9f43;color:#1a1a2e;font-weight:bold}
#preview-a{background:#2d4059;color:#eee}#preview-m{background:#2d4059;color:#eee}
.status{font-size:.8em;color:#888;text-align:center;margin-top:10px}
#msg{text-align:center;padding:8px;color:#4ecca3;display:none}
</style></head><body>
<h1>🏮 Lantern Brain</h1>
<div id="msg"></div>

<h2>Ambient Mode</h2>
<div class="card">
  <label>Effect <span class="val" id="va-fx"></span></label>
  <select id="a-fx"></select>
  <label>Brightness <span class="val" id="va-bri"></span></label>
  <input type="range" id="a-bri" min="1" max="255">
</div>
<button id="preview-a" onclick="preview('a')">Preview Ambient</button>

<h2>Motion Mode</h2>
<div class="card">
  <label>Effect <span class="val" id="vm-fx"></span></label>
  <select id="m-fx"></select>
  <label>Brightness <span class="val" id="vm-bri"></span></label>
  <input type="range" id="m-bri" min="1" max="255">
</div>
<button id="preview-m" onclick="preview('m')">Preview Motion</button>

<h2>Timing</h2>
<div class="card">
  <label>Motion Timeout <span class="val" id="vt"></span></label>
  <input type="range" id="timeout" min="1" max="60" step="1">
  <label>Wave Delay <span class="val" id="vw"></span></label>
  <input type="range" id="wave" min="100" max="2000" step="50">
</div>

<button id="apply" onclick="apply()">Apply &amp; Save</button>
<div class="status" id="status"></div>

<script>
const WLED='http://__WLED_IP__';
let fx={};

async function init(){
  try{
    const r=await fetch(WLED+'/json/effects');
    const list=await r.json();
    list.forEach((n,i)=>{if(n)fx[i]=n});
  }catch(e){fx={0:'Solid',66:'Fire 2012',88:'Candle',45:'Fire Flicker'};}
  populateSelect('a-fx');populateSelect('m-fx');
  const r2=await fetch('/config');
  const c=await r2.json();
  document.getElementById('a-fx').value=c.fxAmbient;
  document.getElementById('a-bri').value=c.briAmbient;
  document.getElementById('m-fx').value=c.fxBright;
  document.getElementById('m-bri').value=c.briBright;
  document.getElementById('timeout').value=c.motionTimeout/1000;
  document.getElementById('wave').value=c.waveDelay;
  updateLabels();
  setInterval(pollStatus,3000);
}
function populateSelect(id){
  const sel=document.getElementById(id);
  sel.innerHTML='';
  Object.keys(fx).sort((a,b)=>a-b).forEach(k=>{
    const o=document.createElement('option');o.value=k;o.text=k+': '+fx[k];sel.add(o);
  });
}
function updateLabels(){
  document.getElementById('va-fx').textContent=fx[document.getElementById('a-fx').value]||'';
  document.getElementById('va-bri').textContent=document.getElementById('a-bri').value;
  document.getElementById('vm-fx').textContent=fx[document.getElementById('m-fx').value]||'';
  document.getElementById('vm-bri').textContent=document.getElementById('m-bri').value;
  document.getElementById('vt').textContent=document.getElementById('timeout').value+'s';
  document.getElementById('vw').textContent=document.getElementById('wave').value+'ms';
}
document.querySelectorAll('input,select').forEach(el=>el.addEventListener('input',updateLabels));

async function apply(){
  const body=JSON.stringify({
    briBright:+document.getElementById('m-bri').value,
    briAmbient:+document.getElementById('a-bri').value,
    fxBright:+document.getElementById('m-fx').value,
    fxAmbient:+document.getElementById('a-fx').value,
    motionTimeout:document.getElementById('timeout').value*1000,
    waveDelay:+document.getElementById('wave').value
  });
  const r=await fetch('/config',{method:'POST',headers:{'Content-Type':'application/json'},body});
  const d=await r.json();
  const m=document.getElementById('msg');
  m.textContent=d.success?'Settings applied!':'Error';
  m.style.display='block';setTimeout(()=>m.style.display='none',2000);
}
async function preview(mode){
  const bri=+document.getElementById(mode+'-bri').value;
  const fxv=+document.getElementById(mode+'-fx').value;
  await fetch('/preview?bri='+bri+'&fx='+fxv);
}
async function pollStatus(){
  try{
    const r=await fetch('/status');const d=await r.json();
    document.getElementById('status').textContent=
      'Uptime: '+d.uptime_s+'s | Motion: '+(d.motion?'YES':'no')+' | PIR: '+d.pir_pin+' | Idle: '+d.idle_s+'s';
  }catch(e){}
}
init();
</script></body></html>
)rawliteral";

// ── Setup ─────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(PIR_PIN, INPUT);

  Serial.print("Connecting to WiFi");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nConnected: " + WiFi.localIP().toString());

  // ── Web UI endpoint ──
  server.on("/", []() {
    String html = FPSTR(HTML_PAGE);
    html.replace("__WLED_IP__", WLED_IP);
    server.send(200, "text/html", html);
  });

  // ── Config GET ──
  server.on("/config", HTTP_GET, []() {
    char buf[256];
    snprintf(buf, sizeof(buf),
      "{\"briBright\":%d,\"briAmbient\":%d,\"fxBright\":%d,\"fxAmbient\":%d,"
      "\"motionTimeout\":%lu,\"waveDelay\":%d}",
      briBright, briAmbient, fxBright, fxAmbient, motionTimeout, waveDelay);
    server.send(200, "application/json", buf);
  });

  // ── Config POST ──
  server.on("/config", HTTP_POST, []() {
    StaticJsonDocument<256> doc;
    DeserializationError err = deserializeJson(doc, server.arg("plain"));
    if (err) {
      server.send(400, "application/json", "{\"success\":false}");
      return;
    }
    if (doc.containsKey("briBright"))    briBright     = doc["briBright"];
    if (doc.containsKey("briAmbient"))   briAmbient    = doc["briAmbient"];
    if (doc.containsKey("fxBright"))     fxBright      = doc["fxBright"];
    if (doc.containsKey("fxAmbient"))    fxAmbient     = doc["fxAmbient"];
    if (doc.containsKey("motionTimeout")) motionTimeout = doc["motionTimeout"];
    if (doc.containsKey("waveDelay"))    waveDelay     = doc["waveDelay"];

    Serial.printf("Config updated: bri=%d/%d fx=%d/%d timeout=%lu wave=%d\n",
      briBright, briAmbient, fxBright, fxAmbient, motionTimeout, waveDelay);

    // Apply ambient immediately if not in motion
    if (!motionActive) applyAmbient();

    server.send(200, "application/json", "{\"success\":true}");
  });

  // ── Preview endpoint ──
  server.on("/preview", []() {
    int bri = server.arg("bri").toInt();
    int fx  = server.arg("fx").toInt();
    for (int i = 0; i < numSegments; i++) {
      setSegment(i, bri, fx);
    }
    server.send(200, "application/json", "{\"success\":true}");
  });

  // ── Status endpoint ──
  server.on("/status", []() {
    char buf[256];
    unsigned long now = millis();
    unsigned long idle = motionActive ? (now - lastMotionTime) / 1000 : 0;
    int pirRaw = digitalRead(PIR_PIN);
    snprintf(buf, sizeof(buf),
      "{\"uptime_s\":%lu,\"motion\":%s,\"idle_s\":%lu,\"pir_pin\":%d,\"ip\":\"%s\"}",
      now / 1000,
      motionActive ? "true" : "false",
      idle,
      pirRaw,
      WiFi.localIP().toString().c_str());
    server.send(200, "application/json", buf);
  });

  server.begin();

  // Auto-detect segments from WLED
  numSegments = detectSegments();
  Serial.printf("Detected %d WLED segments\n", numSegments);
  if (numSegments == 0) {
    Serial.println("WARNING: No segments detected — check WLED");
    numSegments = 1;  // fallback
  }

  // Start all segments in ambient mode
  Serial.println("Setting all segments to ambient");
  applyAmbient();
}

// ── Main loop ─────────────────────────────────────────────────────────────────
void loop() {
  bool pirHigh = digitalRead(PIR_PIN) == HIGH;

  if (pirHigh) {
    lastMotionTime = millis();

    if (!motionActive) {
      Serial.println("Motion detected → wave on");
      waveOn();
      motionActive = true;
    }
  } else if (motionActive && (millis() - lastMotionTime >= motionTimeout)) {
    Serial.println("Motion ended → wave off");
    waveOff();
    motionActive = false;
  }

  server.handleClient();
  delay(100);
}
