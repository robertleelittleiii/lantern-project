# Smart Walkway Lantern System

10 WS2812B NeoPixel lanterns along a walkway with a directional "follow-the-person"
light wave. Two ESP32 DevKits: one runs **WLED**, the other is the **Brain**
(sensors, logic, web UI).

See `project-synopsis.md` for hardware and `CONTEXT.md` for the technical notes.

## Architecture

- **WLED ESP32** — LED driver (effects, palettes, 10 segments)
- **Brain ESP32** — 3× PIR sensors, directional waves, web UI, REST API

Brain factory Wi‑Fi / WLED IP live in `brain/config.h` (gitignored). At runtime
the Brain stores Wi‑Fi, WLED host, and a device **name/tag** in NVS.

## Firmware updates (no USB after the first flash)

GitHub Actions compiles `brain.ino.bin` on every push to `master` and publishes
it as the **Latest Brain firmware** release:

https://github.com/robertleelittleiii/lantern-project/releases/latest

On a Brain that already has OTA firmware:

- Web UI → **Firmware → Install from GitHub**
- or:

```bash
curl -L -o /tmp/brain.ino.bin \
  https://github.com/robertleelittleiii/lantern-project/releases/latest/download/brain.ino.bin
curl -f -F "update=@/tmp/brain.ino.bin" http://<brain-ip>/update
```

`POST /update/url` with `{"url":"https://..."}` pulls a `.bin` over HTTPS
(GitHub redirect-friendly). `GET /update` reports OTA hostname, tag, and URL.

USB / Arduino IDE Network Port (`lanternbrain` or `lanternbrain-<name>`) still work.


### Build, flash, and publish (with binary)
1. `arduino-cli compile --fqbn esp32:esp32:esp32 --output-dir brain/build brain/`
2. Flash: OTA `curl -f -F "update=@brain/build/brain.ino.bin" http://<brain-ip>/update` (or USB `arduino-cli upload ...`).
3. Run tests: `BRAIN_IP=<ip> [WLED_IP=<ip>] tests/.venv/bin/pytest tests/test_api.py -v`.
4. Publish the binary in the repo so Brains can pull it: copy `brain/build/brain.ino.bin` to
   `firmware/brain.ino.bin`, write `firmware/VERSION.txt` (commit SHA + date), commit, and push the branch.
   A branch binary is reachable at `https://raw.githubusercontent.com/robertleelittleiii/lantern-project/<branch>/firmware/brain.ino.bin`
   (use it with `POST /update/url`). "Install from GitHub" uses releases/latest, which is built only by pushes to master.

## Setup (first flash)

1. Copy `brain/config.h.example` to `brain/config.h` and fill in Wi‑Fi (WLED IP is a fallback; the UI can discover hosts).
2. Upload:
   ```bash
   arduino-cli compile --fqbn esp32:esp32:esp32 brain/
   arduino-cli upload  --fqbn esp32:esp32:esp32 --port /dev/cu.SLAB_USBtoUART brain/
   ```
3. Open `http://lanternbrain.local/` (or the device IP). Set a **Name** if you
   run more than one Brain (`lanternbrain-walkway.local`, …).

## WLED presets
Ambient and Motion can each use a WLED preset (by numeric ID; the UI shows "2 · Halloween"). With a preset the Brain
loads it with one `{"ps":id}` call and then changes **brightness only**, so per-lantern colors survive. See PRESET_SUPPORT.md.
Endpoints: `GET /presets` (id+name), `GET /presets/sync` (per-lantern colors), `/preview?mode=ambient|motion|off`.

## Project structure

```
brain/brain.ino            # Brain sketch
brain/config.h.example     # Wi‑Fi / WLED factory defaults
.github/workflows/firmware.yml
tests/test_api.py          # live-device pytest
```

## Wiring (Brain ESP32)

| Component                 | GPIO |
|---------------------------|------|
| PIR Front  HC-SR501 OUT   | 27   |
| PIR Middle HC-SR501 OUT   | 25   |
| PIR Back   HC-SR501 OUT   | 26   |
| All PIRs   VCC            | 5V   |
| All PIRs   GND            | GND  |

PIR inputs are `INPUT_PULLDOWN`.

## Tests

```bash
python3 -m venv .venv && source .venv/bin/activate
pip install -r tests/requirements.txt
BRAIN_IP=192.168.10.9 pytest tests/test_api.py -v
```

The suite talks to a live Brain and restores config afterwards.
