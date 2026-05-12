/*
 * Lantern Brain — Prototype (1 PIR, 1 lantern)
 * Hardware: ESP32 DevKit
 *
 * Wiring:
 *  - PIR HC-SR501  OUT --> GPIO 13
 *  - PIR HC-SR501  VCC --> 5V
 *  - PIR HC-SR501  GND --> GND
 *
 * WLED Presets (configure in WLED UI before use):
 *  - Preset 1 → Ambient / dim warm white  (default idle state)
 *  - Preset 2 → Bright / motion state
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "config.h"

// ── Settings ─────────────────────────────────────────────────────────────────
const int   PIR_PIN          = 13;
const unsigned long MOTION_TIMEOUT_MS = 45000;  // ms of no motion → return to ambient

// ── State ─────────────────────────────────────────────────────────────────────
bool  motionActive   = false;
unsigned long lastMotionTime = 0;

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

// Activate a WLED preset using the JSON API
void setPreset(int preset) {
  ensureWiFi();
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  char url[64];
  snprintf(url, sizeof(url), "http://%s/json/state", WLED_IP);

  // Build JSON payload: {"ps": <preset>}
  StaticJsonDocument<64> doc;
  doc["ps"] = preset;
  String body;
  serializeJson(doc, body);

  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(body);

  if (code > 0) {
    Serial.printf("Preset %d activated (HTTP %d)\n", preset, code);
  } else {
    Serial.printf("WLED POST failed: %s\n", http.errorToString(code).c_str());
  }
  http.end();
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

  delay(100);
}
