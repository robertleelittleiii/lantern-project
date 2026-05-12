/*
 * Lantern Brain — Prototype (1 PIR, 1 lantern)
 * Hardware: ESP32 DevKit
 *
 * Wiring:
 *  - PIR HC-SR501  OUT --> GPIO 27
 *  - PIR HC-SR501  VCC --> 5V
 *  - PIR HC-SR501  GND --> GND
 *
 * WLED Presets (configure in WLED UI before use):
 *  - Preset 1 → Ambient / dim warm white  (default idle state)
 *  - Preset 2 → Bright / motion state
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include "config.h"

// ── Settings ─────────────────────────────────────────────────────────────────
const int   PIR_PIN          = 27;
const unsigned long MOTION_TIMEOUT_MS = 45000;  // ms of no motion → return to ambient

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

// Activate a WLED preset using the simple HTTP API
void setPreset(int preset) {
  ensureWiFi();
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  char url[80];
  snprintf(url, sizeof(url), "http://%s/win?PL=%d", WLED_IP, preset);

  http.begin(url);
  http.setTimeout(5000);
  int code = http.GET();

  if (code > 0) {
    http.getString();  // response is small (~200 bytes XML)
    Serial.printf("Preset %d activated (HTTP %d)\n", preset, code);
  } else {
    Serial.printf("WLED request failed: %s\n", http.errorToString(code).c_str());
  }
  http.end();
  delay(10);  // yield to watchdog
}

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

  // Status endpoint for network monitoring
  server.on("/status", []() {
    char buf[256];
    unsigned long now = millis();
    unsigned long idle = motionActive ? (now - lastMotionTime) / 1000 : 0;
    snprintf(buf, sizeof(buf),
      "{\"uptime_s\":%lu,\"motion\":%s,\"idle_s\":%lu,\"ip\":\"%s\"}",
      now / 1000,
      motionActive ? "true" : "false",
      idle,
      WiFi.localIP().toString().c_str());
    server.send(200, "application/json", buf);
  });
  server.begin();

  // Start in ambient mode
  setPreset(1);
}

// ── Main loop ─────────────────────────────────────────────────────────────────
void loop() {
  bool pirHigh = digitalRead(PIR_PIN) == HIGH;

  if (pirHigh) {
    // Refresh timeout as long as motion is present
    lastMotionTime = millis();

    if (!motionActive) {
      Serial.println("Motion detected → bright");
      setPreset(2);
      motionActive = true;
    }
  } else if (motionActive && (millis() - lastMotionTime >= MOTION_TIMEOUT_MS)) {
    Serial.println("Motion ended → ambient");
    setPreset(1);
    motionActive = false;
  }

  server.handleClient();
  delay(100);
}
