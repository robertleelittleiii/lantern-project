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

## Future Work — 3-Sensor Directional System

### Hardware Needed
- 2 additional HC-SR501 PIR sensors
- GPIO assignments: Front=27, Middle=25, Back=26 (suggested)
- 10 total lanterns (6 more rings to add)

### Directional Logic Design
Three sensors: **Front**, **Middle**, **Back** — placed along the walkway.

**Single person scenarios:**
- Front sensor → wave lights front→middle
- Back sensor → wave lights back→middle
- Middle sensor → wave lights outward to both ends
- Front then Middle → continue wave front→middle→back
- Back then Middle → continue wave back→middle→front
- Middle then Front → person heading to front, back segments return to ambient
- Middle then Back → person heading to back, front segments return to ambient

**Two people (opposite directions):**
- Front + Back sensors fire → two waves converge toward middle
- Both waves reach middle → all segments bright
- People pass each other → segments trail to ambient behind each person

### Architecture Changes Required
The current single-boolean `motionActive` state won't work. Needs:

1. **Per-segment state** — each segment independently tracks bright/ambient
2. **Direction tracking** — sensor activation order determines wave direction
3. **Independent wave timers** — two waves can run simultaneously
4. **Segment ownership** — when waves overlap, most recent trigger wins
5. **Sensor-to-segment zone mapping** — which segments each sensor "covers"

### Suggested Zone Mapping (10 lanterns)
- Front zone: segments 0–2 (3 lanterns near front sensor)
- Middle zone: segments 3–6 (4 lanterns near middle sensor)
- Back zone: segments 7–9 (3 lanterns near back sensor)

### Implementation Approach
- Replace `motionActive` boolean with per-sensor state structs
- Track `lastTriggerTime` per sensor
- Determine direction from sensor pair activation order and timing
- State machine: IDLE → WAVE_FORWARD → WAVE_BACKWARD → WAVE_BOTH → TRAILING
- Wave functions take start/end segment and direction as parameters
- Settings persistence via ESP32 Preferences (NVS) — currently settings reset on reboot

## Commit History
- `2bb511f` — Initial commit: Brain ESP32 prototype, project docs, config template
- `df020dc` — Move config.h into brain/ for Arduino include path
- `64413d6` — Fix PIR crashes: WLED simple API, GPIO 27, /status endpoint
- `f749979` — Restore JSON API POST after brownout fix
- `80fd9ee` — Add pir_pin to /status, reduce motion timeout to 10s
- `234b89c` — Web UI, segment auto-detect, effect switching, wave sequencing
