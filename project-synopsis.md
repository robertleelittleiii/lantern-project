# Smart Walkway Lantern System - Full Project Synopsis

## Project Objective
Create **10 intelligent outdoor lanterns** installed along a walkway (mounted 5–6 ft high on posts). The system should produce a dynamic "light wave" or "follow-the-person" effect:

- As a person walks, the lanterns closest to them brighten significantly.
- Brightness gradients follow the person’s movement (brightest near them, gradually dimming farther away).
- 3 motion sensors spaced along the walkway detect direction and enable smooth handoff between zones.
- Lights return to a low ambient mode after the person passes.

## Hardware Details
- **Lanterns**: 10 small turquoise metal hanging lanterns (~4" diameter × 6" tall) with decorative cutouts on sides and bottom. Originally solar-powered with a single LED insert.
- **LEDs**: 8-LED WS2812B NeoPixel rings (one per lantern, **80 LEDs total**).
- **Controllers**:
  - One **ESP32 DevKit** running **WLED 0.16.0** (dedicated LED driver, handles effects and 10 segments). Already installed and working.
  - One **ESP32 DevKit** acting as the **"Brain"** for sensor reading, directional logic, and sending HTTP API commands to WLED.
- **Sensors**: 3× HC-SR501 PIR motion sensors.
- **Power**: Centralized 12V wall adapter + buck converter(s) feeding a 14/2 outdoor power bus.
- **Enclosure**: One weatherproof 3D-printed box (pb-tec parametric design) housing the central electronics, buck converter, Wagos, etc.

## Current Status (as of 2026-05-24)
- **All 10 lanterns fully wired and operational** — 80 LEDs across 10 × 8-LED WS2812B segments.
- Brain ESP32 firmware is deployed with the full feature set (see below) and has passed a 57-test integration suite.
- System runs standalone; no Home Assistant dependency.

### Implemented Features
- 3-sensor directional motion detection (front, middle, back) with smooth handoff logic
- Non-blocking wave animations via state machine (`WaveState` / `tickWave()`) — no `delay()` blocking
- Motion color override: user-selectable RGB applied on motion, with per-segment color save/restore when toggled off
- NVS settings persistence — all config survives power cycles via ESP32 `Preferences`
- Sensor zone tuning (`sensorMidSeg`) — adjustable middle sensor segment boundary
- Sensor simulator — inject pulse/hold signals via API or web UI without physical movement
- Web UI: effect/palette dropdowns (alphabetical, live from WLED), color picker, brightness/timing sliders, segment rescan
- REST API: `/status`, `/config` (GET+POST), `/preview`, `/simulate`, `/rescan`
- Integration test suite: `tests/test_api.py` (57 tests, pytest + requests)

## Technical Decisions
- WLED for high-quality LED effects and segment control.
- Custom ESP32 (Brain) for advanced logic: directional tracking, per-segment state, multi-sensor coordination.
- Communication between Brain and WLED via WLED JSON API (`/json/state`).
- Centralized power distribution (12V + buck converter).
- Standalone system — no Home Assistant dependency.
- ArduinoJson v6 (`StaticJsonDocument`) — upgrade to v7 would require API changes.

## Challenges Encountered
- Power stability: buck converters caused ESP32 brownouts; resolved with a 2A+ supply and decoupling caps.
- Reliable PIR sensor behavior: default H-mode keeps output HIGH; delay pot turned fully CCW for minimum hold time.
- Physical mounting: flat diffuser insert preferred for clean diffusion and weatherproofing.
- Smooth directional wave logic: required rising-edge detection, per-segment state arrays, and a non-blocking state machine.
- GPIO conflicts: GPIO 32/33 (RTC crystal), 14 (HSPI_CLK), 13 (UART) all caused issues; GPIO 27/25/26 are stable.

## Remaining / Future Work
- **Full sensor zone configuration** — front/back sensor segment mapping and per-sensor enable/disable
- **Two-person handling** — Front + Back simultaneous activation with converging waves
- **Transition effects** — gradual brightness fades rather than instant switch
- **OTA firmware updates** — push sketch updates over WiFi

---

This document contains everything needed for another AI to be fully up to speed. For live technical details, IP addresses, and API documentation see `CONTEXT.md`.


