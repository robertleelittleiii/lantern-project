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

## Current Status
- WLED 0.16.0 is installed and working on the dedicated ESP32 DevKit.
- Single lantern prototype is functional with WLED (8-LED ring works).
- User has tested multiple 3D-printed drop-in inserts for mounting the rings inside the lanterns (flat diffuser version currently preferred).
- Motion sensor integration via WLED alone has proven insufficient; user is adopting a **hybrid WLED + custom Brain ESP32** architecture.
- User is a systems developer comfortable with writing custom code.

## Technical Decisions
- WLED for high-quality LED effects and segment control.
- Custom ESP32 for advanced logic (directional tracking, gradients, multi-sensor coordination).
- Communication between Brain and WLED via HTTP API calls.
- Centralized power distribution.
- Goal is a mostly standalone system (avoiding Home Assistant for now).

## Challenges Encountered
- Power stability (buck converters causing ESP32 brownouts).
- Reliable PIR sensor behavior and proper timeouts.
- Physical mounting of rings inside small lanterns while maintaining weatherproofing.
- Creating smooth, directional "light wave" behavior.

## Next Steps (User's Current Focus)
- Build a prototype with **one lantern + one PIR + two ESP32s**.
- Brain ESP32 reads PIR and sends API commands to WLED ESP32.
- Iterate on 3D-printed inserts for clean, weatherproof ring mounting.

---

This document contains everything needed for another AI to be fully up to speed. Sections that may be expanded in future revisions include: current BOM, wiring diagrams, WLED segment configuration, and PIR zone mapping.


