# Smart Walkway Lantern System — Development Context

## Project Goal
10 intelligent outdoor lanterns along a walkway that produce a directional "follow-the-person" light wave effect using two ESP32 DevKits and WLED.

## Architecture
- **WLED ESP32** (192.168.1.130) — runs WLED 0.16.0, drives all 10 LED segments (8 LEDs each, WS2812B NeoPixel rings, 80 LEDs total)
- **Brain ESP32** (192.168.1.239) — reads 3× PIR sensors, runs motion/directional logic, sends HTTP commands to WLED JSON API, serves web UI

## Current State (as of 2026-08-23)

Firmware is built on GitHub Actions (`.github/workflows/firmware.yml`) and
published to the `firmware` release for OTA (`POST /update` or
`POST /update/url` / web UI **Install from GitHub**). Device name/tag,
WLED LAN discovery, remote `GET /log`, heap watchdog, and dual USB+network
logging are in `brain/brain.ino`.

Earlier snapshot:

### Hardware Working
- All **10 lanterns** wired and operational (80 LEDs, 10 × 8-LED segments)
- 3× HC-SR501 PIR sensors: Front → GPIO 27, Middle → GPIO 25, Back → GPIO 26
- All PIR inputs use INPUT_PULLDOWN to prevent false reads
- Centralized power supply (2A+) — no brownout issues
- Electronics in weatherproof 3D-printed enclosure

### Software Working
`brain/brain.ino` — full deployed sketch featuring:

**Core motion / wave logic**
- Auto-detects segment count from WLED `/json/state` at startup
- Per-segment brightness + effect control via WLED JSON API
- 3-sensor directional logic: front→forward wave, back→reverse wave, middle→outward-from-center wave
- Trailing-dim on timeout: segments dim in the order they were originally lit
- Non-blocking wave state machine (`WaveState` enum + `tickWave()`) — no `delay()` in wave paths, UI/API stays responsive during effects
- Configurable motion timeout (default 10 s) and wave step delay (default 500 ms)

**Settings persistence (NVS)**
- All tunable settings written to ESP32 flash via `Preferences` library on every config POST
- Settings loaded at startup — survive power cycles and OTA reboots

**Sensor zone configuration**
- `sensorMidSeg` — tunable setting defining which segment the middle sensor maps to
- Defaults to `numSegments / 2` when set to 0; adjustable from web UI slider
- Directional logic uses `sensorMidSeg` rather than a hard-coded midpoint

**Motion color override**
- `colBrightEnable` toggle — when enabled, applies a user-selected RGB color to every segment on motion activation
- `colBrightR/G/B` store the override color (default warm orange 255/150/0)
- On enable: snapshots each segment's current WLED primary color into `savedColR/G/B[]`
- On disable: restores per-segment original colors from the snapshot

**Sensor simulator**
- `simMode` flag replaces real PIR reads with injected values
- Per-sensor pulse (N-cycle countdown) and hold (persistent HIGH) modes
- `/simulate` endpoint accepts sensor, mode (`pulse`/`hold`/`clear`), and optional count
- Allows full motion-logic testing without physical movement

**Web UI** at `http://192.168.1.239/`
- Ambient/motion effect dropdowns populated from live WLED effects list (alphabetical)
- Ambient palette dropdown populated from live WLED palettes list (alphabetical)
- Motion color picker with enable/disable checkbox (picker disabled when override is off)
- Brightness sliders for ambient and motion modes
- Timing sliders: motion timeout and wave step delay (values update live on drag)
- Sensor Zones card: `sensorMidSeg` slider
- Preview buttons for ambient and motion modes
- Segment Simulator section: Trigger (pulse) and Hold buttons per sensor, plus Clear All
- Refresh Segments button to re-scan WLED segment count without reboot
- Live status bar: uptime, motion state, PIR readings, idle timer, segment count

**API endpoints**
- `GET /status` — JSON: uptime, motion, idle_s, pir_f/m/b, sim/sim_f/m/b, dir, segs, segs_bright[], ip
- `GET /config` — all tunable settings as JSON
- `POST /config` — partial or full update; persists to NVS; applies ambient immediately if not in motion
- `GET /preview?mode=bright|ambient` — preview a mode on all segments
- `POST /simulate` — inject sensor signals for testing (sensor, mode, count)
- `POST /rescan` — re-detect segment count from WLED without rebooting

**Testing**
- `tests/test_api.py` — pytest integration suite (57 tests) covering all endpoints, config roundtrips, NVS persistence, simulator API, new color override fields, and segment rescan
- `tests/monitor_sensors.py` — real-time sensor state monitor utility
- Run: `BRAIN_IP=192.168.1.239 pytest tests/test_api.py -v` (use a venv: `pip install pytest requests`)

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

## 3-Sensor Directional System

### Hardware
- 3× HC-SR501 PIR sensors: Front (GPIO 27), Middle (GPIO 25), Back (GPIO 26)
- All use INPUT_PULLDOWN to prevent false reads when disconnected
- 10 lanterns (80 LEDs, 10× 8-LED segments)

### Directional Logic
Uses rising-edge detection on each sensor and tracks `lastSensor` for direction:
- **Front fires (new)** → wave bright 0→N-1
- **Back fires (new)** → wave bright N-1→0
- **Middle fires (new)** → wave bright outward from `sensorMidSeg`
- **Front→Middle** → continue forward, light remaining back segments
- **Back→Middle** → continue backward, light remaining front segments
- **Middle→Front** → dim back segments (trailing behind person)
- **Middle→Back** → dim front segments (trailing behind person)
- **Timeout** → dim segments in the order they were originally lit

All wave steps are driven by the non-blocking `tickWave()` state machine — no blocking delays.

### Per-Segment State Tracking
- `segBright[]` — tracks bright/ambient state per segment
- `litOrder[]` / `litCount` — records order segments were lit for trailing dim
- `lightSeg()` skips already-lit segments, `dimSeg()` skips already-dim segments
- `savedColR/G/B[]` — per-segment color snapshot used by motion color override save/restore

## Future Work

### Full Sensor Zone Configuration
`sensorMidSeg` handles the middle sensor boundary. Remaining enhancements:
- **Front/back sensor segment pins** — explicit config for which segment each end sensor borders
- **Zone size** — how many segments each sensor covers
- **Per-sensor enable/disable** — testing with fewer than 3 sensors active

### Suggested Zone Mapping (10 lanterns)
- Front zone: segments 0–2 (3 lanterns near front sensor)
- Middle zone: segments 3–6 (4 lanterns near middle sensor)
- Back zone: segments 7–9 (3 lanterns near back sensor)

### Other Potential Improvements
- **Two-person handling** — Front + Back simultaneous activation, converging waves
- **Transition effects** — gradual brightness fade rather than instant switch
- **OTA firmware updates** — push new sketch over WiFi
- **Passwordless remote diagnostics** — configure passwordless sudo on OctoPi host for non-interactive log access

## Commit History
- `2bb511f` — Initial commit: Brain ESP32 prototype, project docs, config template
- `df020dc` — Move config.h into brain/ for Arduino include path
- `64413d6` — Fix PIR crashes: WLED simple API, GPIO 27, /status endpoint
- `f749979` — Restore JSON API POST after brownout fix
- `80fd9ee` — Add pir_pin to /status, reduce motion timeout to 10s
- `234b89c` — Web UI, segment auto-detect, effect switching, wave sequencing
- *(subsequent commits)* — 3-sensor directional logic, motion color override, NVS persistence, non-blocking wave state machine, sensor zone config (`sensorMidSeg`), sensor simulator, palette/effect alphabetization, integration test suite (57 tests)
