# Smart Walkway Lantern System

10 WS2812B NeoPixel lanterns installed along a walkway that produce a dynamic
"follow-the-person" light wave effect using two ESP32 DevKits and WLED.

See `project-synopsis.md` for full hardware details and design decisions.

## Architecture

- **WLED ESP32** — runs WLED 0.16.0, drives all 10 lantern segments (8 LEDs each)
- **Brain ESP32** — reads 3× HC-SR501 PIR sensors, sends HTTP commands to WLED JSON API

## Project Structure

```
brain/
  brain.ino          # Brain ESP32 sketch (upload this one)
brain/config.h         # WiFi + WLED credentials (gitignored — copy from config.h.example)
  brain/config.h.example # Credentials template
project-synopsis.md  # Full project design document
```

## Setup

1. Copy `brain/config.h.example` to `brain/config.h` and fill in your credentials
2. In the Arduino IDE:
   - Install board support: **esp32 by Espressif Systems** via Boards Manager
   - Install library: **ArduinoJson by Benoit Blanchon** via Library Manager
   - Select board: **ESP32 Dev Module**
3. Configure two presets in the WLED UI:
   - Preset 1 → Ambient / dim warm white (idle state)
   - Preset 2 → Bright (motion detected)
4. Open `brain/brain.ino` and upload to the Brain ESP32

## Wiring (Brain ESP32)

| Component         | GPIO |
|-------------------|------|
| PIR HC-SR501 OUT  | 27   |
| PIR HC-SR501 VCC  | 5V   |
| PIR HC-SR501 GND  | GND  |
