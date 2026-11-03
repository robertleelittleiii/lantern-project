# WLED Preset Support for Lantern Brain — Design Sketch

Status: implemented on branch `preset-support` (Nov 1, 2026). Notes vs. the sketch:
- Brightness writes use one call for all segments (`setAllSegBri`) where possible; per-lantern wave uses `setSegBri`.
- `/preview?mode=ambient|motion|off` runs the real `applyAmbient()` / `startMotionLook()` + `lightSeg()` path;
  optional bri/fx/pal/ps args preview unsaved values. No `mode` = legacy raw paint.
- `GET /presets/sync` snapshots per-lantern colors (UI "Sync" button; Preview buttons show them while on).
- Names shown as "ID · Name"; missing IDs show "Preset N (missing)". `/config` adds psAmbientName/psMotionName.

## Problem
`brain/brain.ino` paints every segment directly through `setSegment()` (bri + fx + pal + col):
- `applyAmbient()` (boot, schedule start, config save) writes fxAmbient/palAmbient to all segments.
- `lightSeg()` writes fxBright plus a color (override or hardcoded warm white 255,248,247).
- `dimSeg()` / `WAVE_APPLY_AMBIENT` write fxAmbient/palAmbient again.
- `/preview` writes the on-screen bri/fx/pal to all segments.

WLED presets (1 "Master", 2 "Halloween" with per-lantern orange/purple) are never called, so any
preset gets overwritten. Per-lantern colors are the whole point of Halloween, so they are lost.

## 1. Mapping ambient/motion to preset IDs
New NVS settings (0 = legacy behavior, so nothing changes until a preset is chosen):
- `psAmbient` (int) — preset loaded for idle/ambient.
- `psMotion` (int) — preset loaded when motion starts. 0 = same look as ambient, brightness only.
- `briAmbient` / `briBright` stay as they are and become the only per-segment values the Brain writes.

## 2. Code changes
- New `applyPreset(int id)`: POST `{"ps":id}` to `/json/state`, then re-read segment count.
- New `setSegBri(int seg, int bri)`: POST `{"seg":[{"id":seg,"bri":bri}]}` — brightness only, keeps the
  preset's fx/pal/col.
- `applyAmbient()`: if `psAmbient > 0`, `applyPreset(psAmbient)` then `setSegBri(i, briAmbient)` for all
  segments; otherwise current code.
- Motion start (first rising edge): if `psMotion > 0 && psMotion != psAmbient`, `applyPreset(psMotion)` and
  set all segments to `briAmbient` so the wave still has something to reveal.
- `lightSeg()`: in preset mode, `setSegBri(seg, briBright)` only. No fx, no color, no warm white.
  The color override (`colBrightEnable`) still works if you turn it on, since it's an explicit choice.
- `dimSeg()`: in preset mode, `setSegBri(seg, briAmbient)`.
- `WAVE_APPLY_AMBIENT`: in preset mode, call `applyAmbient()` once (reload the ambient preset) instead of
  painting each segment.
- `/preview?mode=ambient|bright`: in preset mode, load the matching preset plus brightness, which is the same
  path real motion uses, so the preview matches reality.

## 3. UI and API
- `GET /presets` (Brain) proxies WLED `/presets.json` and returns `[{id,name}]`.
- Ambient card: a "Preset" dropdown with "None (use effect/palette below)" plus each WLED preset. Choosing one
  greys out the effect and palette dropdowns.
- Motion card: the same dropdown, plus a "Same as ambient (brightness only)" option.
- `GET/POST /config` gains `psAmbient` and `psMotion` and persists them to NVS (bump `SETTINGS_VER`).
- Refresh button re-reads the preset list, like Refresh Segments.
- Tests: config roundtrip, `/presets` list, and checking that preview/motion leave segment colors untouched.

## 4. Boot behavior
- Today: WLED boots into Master (ps 1, bri 128), then the Brain's `applyAmbient()` paints over it.
- New: the Brain's boot `applyAmbient()` loads `psAmbient`, so Halloween survives a reboot no matter what
  WLED's own boot preset is. Optionally also set WLED's boot preset to match, which avoids a white
  flash during the few seconds before the Brain is up.

## 5. Trade-offs
- In preset mode the Brain no longer sets per-step effect/palette. Motion "look" changes only by brightness, or
  by switching to a separate motion preset (a whole-strip swap, not a per-lantern wave of a new effect).
- A different motion preset causes a visible snap when it loads, and the wave then ramps brightness.
  Using the same preset for both is the smoothest option.
- `applyPreset` is one HTTP call instead of ten, which is faster and lighter on the ESP32.
- Effects still run: the effect saved in the preset (Fire 2012 in Master) keeps animating, and the Brain only
  scales brightness.
- Presets depend on WLED's segment layout. If someone changes segment counts in WLED, the presets need re-saving.
- Legacy mode (preset = 0) is unchanged, so nothing breaks until a preset is selected.

## Motion preset swap respects the sensor wave
When `psMotion` differs from `psAmbient`, `startMotionLook()` loads the motion preset and then
sets every segment's brightness to 0 (previously `briAmbient`, which lit all lanterns at once).
The existing directional wave (`lightSeg`) then reveals lanterns one by one: front sensor
front→back, back sensor back→front, middle sensor outward. Test:
`TestPresets::test_motion_preset_swap_reveals_by_wave`.

## No flash before the chase
Posting the whole motion preset at motion start (even with every segment `"on":false`) made all
lanterns crossfade to the preset's solid full-bright look during WLED's 700 ms transition, so the
path flashed bright before the chase. Now `startMotionLook()` only caches each segment of the
motion preset from WLED `/presets.json` (`buildDarkPreset()`, cleared by `refreshPresetNames()`),
and `lightSeg()` switches just the lantern the wave reaches to its preset look
(`{"seg":[{"on":true,"bri":briBright,...preset seg...}]}`). Unreached lanterns keep the ambient
look. Falls back to the old ps + blank path if the fetch fails. The first trip after boot or a
settings change waits ~0.9 s for the fetch.
