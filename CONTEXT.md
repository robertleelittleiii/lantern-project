# Smart Walkway Lantern System — Development Context

## Project Goal
10 intelligent outdoor lanterns along a walkway that produce a directional "follow-the-person" light wave effect using two ESP32 DevKits and WLED.

## Architecture
- **WLED ESP32** (192.168.1.130) — runs WLED 0.16.0, drives all LED segments (8 LEDs each, WS2812B NeoPixel rings)
- **Brain ESP32** (192.168.1.239) — reads PIR sensors, runs motion logic, sends HTTP commands to WLED JSON API, serves web UI

## Current State (as of 2026-05-12)

### Hardware Working
- Brain ESP32 on GPIO 27 for PIR (HC-SR501)
- 4 lanterns (32 LEDs) connected, WLED segments configured as 4 × 8 LEDs
- Upgraded power supply resolved brownout crashes
- PIR sensor wired: OUT→GPIO 27, VCC→5V, GND→GND

### Software Working
- `brain/brain.ino` — full sketch with:
  - Auto-detects segment count from WLED `/json/state` at startup
  - Per-segment brightness + effect control via WLED JSON API
  - Wave on: segments light up 0→1→2→3 in sequence with configurable delay
  - Wave off: segments return to ambient in same order (trailing behind person)
  - Ambient mode: Fire 2012 effect (fx 66) at full brightness
  - Motion mode: Solid (fx 0) at full brightness
  - 10-second motion timeout (configurable)
  - 500ms wave delay between segments (configurable)
- **Web UI** at `http://192.168.1.239/` with:
  - Ambient/motion effect dropdowns (populated from WLED effects list)
  - Brightness sliders for each mode
  - Motion timeout and wave delay sliders
  - Preview buttons to test modes live
  - Live status bar (uptime, motion state, PIR pin, idle timer)
- **API endpoints**:
  - `GET /status` — JSON with uptime, motion state, PIR pin reading, idle seconds
  - `GET /config` — current tunable settings
  - `POST /config` — update settings (applies ambient immediately if not in motion)
  - `GET /preview?bri=N&fx=N` — preview a mode on all segments
- `.gitignore` properly excludes `brain/config.h` (WiFi/WLED credentials)

### Key Files
- `brain/brain.ino` — main sketch
- `brain/config.h` — credentials (gitignored)
- `brain/config.h.example` — credentials template
- `project-synopsis.md` — original hardware/design document
- `README.md` — setup instructions and wiring

## Lessons Learned

### GPIO Pin Selection (ESP32 DevKit)
- **GPIO 32/33** — connected to RTC crystal, causes crashes
- **GPIO 14** — HSPI_CLK, interferes with SPI during WiFi operations, causes flash errors
- **GPIO 13** — reported UART conflict on this board
- **GPIO 27** — works reliably, no conflicts ✓
- Safe pins for future sensors: 25, 26, 4, 5, 16, 17, 18, 19, 21, 22, 23

### Power
- ESP32 + WiFi + PIR on USB power is marginal — causes brownout crashes
- Upgraded power supply (2A+) resolved all stability issues
- For final install: centralized 12V + buck converter, add 100µF decoupling caps

### WLED API
- JSON API POST to `/json/state` with `http.getString()` to consume response — works reliably
- Response body MUST be consumed before `http.end()` to avoid memory issues
- `http.setTimeout(5000)` prevents hangs
- `delay(10)` after HTTP calls yields to watchdog
- ArduinoJson v6 (`StaticJsonDocument`) — if upgrading to v7, API changes needed

### PIR Sensor (HC-SR501)
- Default H mode (repeatable trigger) keeps output HIGH almost continuously
- Turn delay potentiometer fully counterclockwise for minimum hold time (~3s)
- Consider L mode (single trigger) for cleaner LOW periods
- 10-second software timeout works well for walkway use case

### WLED Presets
- Presets are referenced by numeric ID, not name — easy to mislabel in WLED UI
- Preset brightness must be configured explicitly — defaults may not match expectations
- Segment colors must be set per-segment (new segments default to pure white [255,255,255])
- Warm white reference: [255, 248, 247]

## 3-Sensor Directional System (Implemented)

### Hardware
- 3× HC-SR501 PIR sensors: Front (GPIO 27), Middle (GPIO 25), Back (GPIO 26)
- All use INPUT_PULLDOWN to prevent false reads when disconnected
- 4 lanterns (32 LEDs, 4× 8-LED segments) currently; expanding to 10

### Directional Logic (Current Implementation)
Uses rising-edge detection on each sensor and tracks `lastSensor` for direction:
- **Front fires (new)** → wave bright 0→N-1
- **Back fires (new)** → wave bright N-1→0
- **Middle fires (new)** → wave bright outward from center
- **Front→Middle** → continue forward, light remaining back segments
- **Back→Middle** → continue backward, light remaining front segments
- **Middle→Front** → dim back segments (trailing behind person)
- **Middle→Back** → dim front segments (trailing behind person)
- **Timeout** → dim segments in the order they were originally lit

### Per-Segment State Tracking
- `segBright[]` — tracks bright/ambient state per segment
- `litOrder[]` / `litCount` — records order segments were lit for trailing dim
- `lightSeg()` skips already-lit segments, `dimSeg()` skips already-dim segments

### Status Endpoint
- `/status` now returns all 3 PIR readings (`pir_f`, `pir_m`, `pir_b`), direction (`dir`), and segment count
- Web UI status bar shows: `F:0 M:0 B:0 | MOTION front | 5s`

## Future Work

### Sensor Zone Configuration (Web UI)
Currently the sensor-to-segment mapping is implicit (front=seg 0 side, back=seg N side, middle=center). For the real walkway install, the web UI needs:
- **Sensor position config** — which segment each sensor is nearest to
- **Zone size** — how many segments each sensor covers
- **Segment ordering** — whether physical order matches segment numbering
- **Per-sensor enable/disable** — for testing with fewer than 3 sensors
- Store via `/config` API and persist across reboots (ESP32 Preferences/NVS)

### Suggested Zone Mapping (10 lanterns)
- Front zone: segments 0–2 (3 lanterns near front sensor)
- Middle zone: segments 3–6 (4 lanterns near middle sensor)
- Back zone: segments 7–9 (3 lanterns near back sensor)

### Other Planned Improvements
- **Settings persistence** via ESP32 Preferences (NVS) — currently settings reset on reboot
- **Non-blocking waves** — current wave functions use `delay()`, blocking sensor reads during wave sequences
- **Two-person handling** — Front + Back simultaneous activation, converging waves
- **Transition effects** — gradual brightness fade rather than instant switch

## Commit History
- `2bb511f` — Initial commit: Brain ESP32 prototype, project docs, config template
- `df020dc` — Move config.h into brain/ for Arduino include path
- `64413d6` — Fix PIR crashes: WLED simple API, GPIO 27, /status endpoint
- `f749979` — Restore JSON API POST after brownout fix
- `80fd9ee` — Add pir_pin to /status, reduce motion timeout to 10s
- `234b89c` — Web UI, segment auto-detect, effect switching, wave sequencing
