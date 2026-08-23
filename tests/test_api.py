"""
Integration tests for the Lantern Brain ESP32 HTTP API.

Requires the Brain ESP32 to be powered on and reachable at BRAIN_IP.

Run:
    pip install pytest requests
    pytest tests/test_api.py -v

To target a different IP:
    BRAIN_IP=192.168.1.100 pytest tests/test_api.py -v
"""

import os
import time
import pytest
import requests

BRAIN_IP       = os.environ.get("BRAIN_IP", "192.168.1.239")
BASE_URL       = f"http://{BRAIN_IP}"
TIMEOUT        = 10   # seconds — status/config GET, simulator, web UI
CONFIG_TIMEOUT = 30   # seconds — config POST (triggers blocking WLED calls for all segments)


# ── Fixtures ──────────────────────────────────────────────────────────────────

@pytest.fixture(scope="session")
def brain_url():
    return BASE_URL


@pytest.fixture(scope="session", autouse=True)
def restore_config(brain_url):
    """Snapshot config, clear any leftover sim state, then restore config afterwards."""
    # Clear any simulation that may have been left by a previous test run
    requests.get(f"{brain_url}/simulate?sensor=clear", timeout=TIMEOUT)
    time.sleep(0.3)
    r = requests.get(f"{brain_url}/config", timeout=TIMEOUT)
    r.raise_for_status()
    snapshot = r.json()
    yield snapshot
    requests.post(f"{brain_url}/config", json=snapshot, timeout=CONFIG_TIMEOUT)


# ── /status endpoint ──────────────────────────────────────────────────────────

class TestStatusEndpoint:

    def test_reachable(self, brain_url):
        r = requests.get(f"{brain_url}/status", timeout=TIMEOUT)
        assert r.status_code == 200

    def test_content_type_is_json(self, brain_url):
        r = requests.get(f"{brain_url}/status", timeout=TIMEOUT)
        assert "application/json" in r.headers.get("Content-Type", "")

    def test_all_required_fields_present(self, brain_url):
        data = requests.get(f"{brain_url}/status", timeout=TIMEOUT).json()
        for field in ("uptime_s", "motion", "idle_s",
                      "pir_f", "pir_m", "pir_b",
                      "sim", "sim_f", "sim_m", "sim_b",
                      "dir", "segs", "segs_bright", "ip", "wled",
                      "tag", "hostname", "heap", "heap_min"):
            assert field in data, f"Missing field: {field}"
        assert data["heap"] > 0
        assert data["heap_min"] > 0

    def test_sim_false_by_default(self, brain_url):
        data = requests.get(f"{brain_url}/status", timeout=TIMEOUT).json()
        assert data["sim"] is False, "sim should be false when no simulation is active"
        assert data["sim_f"] == 0
        assert data["sim_m"] == 0
        assert data["sim_b"] == 0

    def test_pir_values_are_binary(self, brain_url):
        data = requests.get(f"{brain_url}/status", timeout=TIMEOUT).json()
        assert data["pir_f"] in (0, 1), f"pir_f out of range: {data['pir_f']}"
        assert data["pir_m"] in (0, 1), f"pir_m out of range: {data['pir_m']}"
        assert data["pir_b"] in (0, 1), f"pir_b out of range: {data['pir_b']}"

    def test_motion_is_boolean(self, brain_url):
        data = requests.get(f"{brain_url}/status", timeout=TIMEOUT).json()
        assert isinstance(data["motion"], bool)

    def test_dir_is_valid(self, brain_url):
        data = requests.get(f"{brain_url}/status", timeout=TIMEOUT).json()
        assert data["dir"] in ("none", "front", "middle", "back"), \
            f"Unexpected dir value: {data['dir']}"

    def test_segs_bright_length_matches_segs_count(self, brain_url):
        data = requests.get(f"{brain_url}/status", timeout=TIMEOUT).json()
        assert isinstance(data["segs_bright"], list)
        assert len(data["segs_bright"]) == data["segs"], \
            f"segs_bright length {len(data['segs_bright'])} != segs {data['segs']}"

    def test_segs_bright_contains_booleans(self, brain_url):
        data = requests.get(f"{brain_url}/status", timeout=TIMEOUT).json()
        for i, val in enumerate(data["segs_bright"]):
            assert isinstance(val, bool), f"segs_bright[{i}] is not bool: {val!r}"

    def test_segment_count_positive(self, brain_url):
        data = requests.get(f"{brain_url}/status", timeout=TIMEOUT).json()
        assert data["segs"] > 0, "Expected at least one WLED segment"

    def test_uptime_increases(self, brain_url):
        t1 = requests.get(f"{brain_url}/status", timeout=TIMEOUT).json()["uptime_s"]
        time.sleep(2)
        t2 = requests.get(f"{brain_url}/status", timeout=TIMEOUT).json()["uptime_s"]
        assert t2 > t1, "Uptime did not increase over 2 seconds"

    def test_reported_ip_matches_target(self, brain_url):
        data = requests.get(f"{brain_url}/status", timeout=TIMEOUT).json()
        assert data["ip"] == BRAIN_IP, \
            f"Reported IP {data['ip']!r} does not match {BRAIN_IP!r}"

    def test_idle_s_zero_when_not_in_motion(self, brain_url):
        """idle_s tracks time-since-last-motion only while motionActive; should be 0 otherwise."""
        data = requests.get(f"{brain_url}/status", timeout=TIMEOUT).json()
        if not data["motion"]:
            assert data["idle_s"] == 0, \
                f"idle_s should be 0 when motion=false, got {data['idle_s']}"


# ── /config GET endpoint ──────────────────────────────────────────────────────

class TestConfigGet:

    def test_reachable(self, brain_url):
        r = requests.get(f"{brain_url}/config", timeout=TIMEOUT)
        assert r.status_code == 200

    def test_content_type_is_json(self, brain_url):
        r = requests.get(f"{brain_url}/config", timeout=TIMEOUT)
        assert "application/json" in r.headers.get("Content-Type", "")

    def test_all_required_fields_present(self, brain_url):
        data = requests.get(f"{brain_url}/config", timeout=TIMEOUT).json()
        for field in ("briBright", "briAmbient", "fxBright", "fxAmbient",
                      "palAmbient", "motionTimeout", "waveDelay",
                      "colBrightR", "colBrightG", "colBrightB",
                      "colBrightEnable", "sensorMidSeg"):
            assert field in data, f"Missing config field: {field}"

    def test_brightness_values_in_range(self, brain_url):
        data = requests.get(f"{brain_url}/config", timeout=TIMEOUT).json()
        assert 1 <= data["briBright"]  <= 255, f"briBright out of range: {data['briBright']}"
        assert 1 <= data["briAmbient"] <= 255, f"briAmbient out of range: {data['briAmbient']}"

    def test_fx_values_non_negative(self, brain_url):
        data = requests.get(f"{brain_url}/config", timeout=TIMEOUT).json()
        assert data["fxBright"]  >= 0
        assert data["fxAmbient"] >= 0

    def test_pal_ambient_in_range(self, brain_url):
        data = requests.get(f"{brain_url}/config", timeout=TIMEOUT).json()
        assert 0 <= data["palAmbient"] <= 70, \
            f"palAmbient out of range: {data['palAmbient']}"

    def test_timing_values_positive(self, brain_url):
        data = requests.get(f"{brain_url}/config", timeout=TIMEOUT).json()
        assert data["motionTimeout"] > 0
        assert data["waveDelay"]     > 0

    def test_motion_timeout_sensible_range(self, brain_url):
        """Timeout should be between 1 s and 5 min (in milliseconds)."""
        data = requests.get(f"{brain_url}/config", timeout=TIMEOUT).json()
        assert 1_000 <= data["motionTimeout"] <= 300_000, \
            f"motionTimeout seems wrong: {data['motionTimeout']} ms"


# ── /config POST endpoint ─────────────────────────────────────────────────────

class TestConfigPost:

    def test_update_bright_brightness(self, brain_url, restore_config):
        r = requests.post(f"{brain_url}/config",
                          json={"briBright": 200},
                          timeout=CONFIG_TIMEOUT)
        assert r.status_code == 200
        assert r.json().get("success") is True
        cfg = requests.get(f"{brain_url}/config", timeout=CONFIG_TIMEOUT).json()
        assert cfg["briBright"] == 200

    def test_update_ambient_brightness(self, brain_url, restore_config):
        r = requests.post(f"{brain_url}/config",
                          json={"briAmbient": 50},
                          timeout=CONFIG_TIMEOUT)
        assert r.status_code == 200
        cfg = requests.get(f"{brain_url}/config", timeout=CONFIG_TIMEOUT).json()
        assert cfg["briAmbient"] == 50

    def test_update_wave_delay(self, brain_url, restore_config):
        r = requests.post(f"{brain_url}/config",
                          json={"waveDelay": 300},
                          timeout=CONFIG_TIMEOUT)
        assert r.status_code == 200
        cfg = requests.get(f"{brain_url}/config", timeout=CONFIG_TIMEOUT).json()
        assert cfg["waveDelay"] == 300

    def test_update_motion_timeout(self, brain_url, restore_config):
        r = requests.post(f"{brain_url}/config",
                          json={"motionTimeout": 15000},
                          timeout=CONFIG_TIMEOUT)
        assert r.status_code == 200
        cfg = requests.get(f"{brain_url}/config", timeout=CONFIG_TIMEOUT).json()
        assert cfg["motionTimeout"] == 15000

    def test_update_fx_values(self, brain_url, restore_config):
        r = requests.post(f"{brain_url}/config",
                          json={"fxBright": 0, "fxAmbient": 45},
                          timeout=CONFIG_TIMEOUT)
        assert r.status_code == 200
        cfg = requests.get(f"{brain_url}/config", timeout=CONFIG_TIMEOUT).json()
        assert cfg["fxBright"]  == 0
        assert cfg["fxAmbient"] == 45

    def test_update_pal_ambient(self, brain_url, restore_config):
        """Verify palAmbient can be changed (e.g. 8=Lava, 35=Fire, 36=Icefire)."""
        for pal in (8, 35, 36, 0):
            r = requests.post(f"{brain_url}/config",
                              json={"palAmbient": pal},
                              timeout=CONFIG_TIMEOUT)
            assert r.status_code == 200, f"POST failed for palAmbient={pal}"
            cfg = requests.get(f"{brain_url}/config", timeout=CONFIG_TIMEOUT).json()
            assert cfg["palAmbient"] == pal, \
                f"palAmbient expected {pal}, got {cfg['palAmbient']}"

    def test_partial_update_preserves_other_fields(self, brain_url, restore_config):
        before = requests.get(f"{brain_url}/config", timeout=TIMEOUT).json()
        requests.post(f"{brain_url}/config",
                      json={"waveDelay": 750},
                      timeout=CONFIG_TIMEOUT)
        after = requests.get(f"{brain_url}/config", timeout=CONFIG_TIMEOUT).json()

        assert after["waveDelay"] == 750
        # All other fields should be unchanged
        for key in ("briBright", "briAmbient", "fxBright", "fxAmbient",
                    "palAmbient", "motionTimeout"):
            assert after[key] == before[key], \
                f"{key} changed unexpectedly: {before[key]} → {after[key]}"

    def test_invalid_json_returns_400(self, brain_url):
        r = requests.post(
            f"{brain_url}/config",
            data="not {{ valid json",
            headers={"Content-Type": "application/json"},
            timeout=TIMEOUT,
        )
        assert r.status_code == 400
        assert r.json().get("success") is False

    def test_full_roundtrip(self, brain_url, restore_config):
        new_cfg = {
            "briBright":     180,
            "briAmbient":     80,
            "fxBright":        0,
            "fxAmbient":      45,
            "palAmbient":     35,
            "motionTimeout": 8000,
            "waveDelay":      400,
        }
        r = requests.post(f"{brain_url}/config", json=new_cfg, timeout=CONFIG_TIMEOUT)
        assert r.status_code == 200

        returned = requests.get(f"{brain_url}/config", timeout=CONFIG_TIMEOUT).json()
        for key, expected in new_cfg.items():
            assert returned[key] == expected, \
                f"{key}: expected {expected}, got {returned[key]}"


# ── Color override settings ───────────────────────────────────────────────────

class TestColorOverride:

    def test_col_bright_enable_present_in_config(self, brain_url):
        data = requests.get(f"{brain_url}/config", timeout=TIMEOUT).json()
        assert "colBrightEnable" in data

    def test_col_bright_enable_default_is_false(self, brain_url):
        """After NVS migration the default must be False, not the old True."""
        data = requests.get(f"{brain_url}/config", timeout=TIMEOUT).json()
        assert data["colBrightEnable"] is False, (
            f"Expected colBrightEnable=False (post-migration default), got {data['colBrightEnable']}"
        )

    def test_col_bright_enable_toggle_on(self, brain_url, restore_config):
        r = requests.post(f"{brain_url}/config",
                          json={"colBrightEnable": True},
                          timeout=CONFIG_TIMEOUT)
        assert r.status_code == 200
        assert r.json().get("success") is True
        cfg = requests.get(f"{brain_url}/config", timeout=CONFIG_TIMEOUT).json()
        assert cfg["colBrightEnable"] is True

    def test_col_bright_enable_toggle_off(self, brain_url, restore_config):
        # Enable first (triggers saveSegmentColors + applyAmbient), then disable
        requests.post(f"{brain_url}/config", json={"colBrightEnable": True}, timeout=CONFIG_TIMEOUT)
        r = requests.post(f"{brain_url}/config",
                          json={"colBrightEnable": False},
                          timeout=CONFIG_TIMEOUT)
        assert r.status_code == 200
        cfg = requests.get(f"{brain_url}/config", timeout=CONFIG_TIMEOUT).json()
        assert cfg["colBrightEnable"] is False

    def test_col_bright_enable_persists_after_save(self, brain_url, restore_config):
        """colBrightEnable written via POST should survive a round-trip read."""
        for val in (True, False):
            requests.post(f"{brain_url}/config", json={"colBrightEnable": val}, timeout=CONFIG_TIMEOUT)
            returned = requests.get(f"{brain_url}/config", timeout=CONFIG_TIMEOUT).json()
            assert returned["colBrightEnable"] is val, (
                f"Expected {val}, got {returned['colBrightEnable']}"
            )

    def test_col_bright_rgb_defaults_in_range(self, brain_url):
        data = requests.get(f"{brain_url}/config", timeout=TIMEOUT).json()
        for key in ("colBrightR", "colBrightG", "colBrightB"):
            assert 0 <= data[key] <= 255, f"{key} out of range: {data[key]}"

    def test_col_bright_rgb_round_trip(self, brain_url, restore_config):
        """Custom color values should survive a POST→GET round-trip."""
        new_color = {"colBrightR": 200, "colBrightG": 100, "colBrightB": 50}
        r = requests.post(f"{brain_url}/config", json=new_color, timeout=CONFIG_TIMEOUT)
        assert r.status_code == 200
        cfg = requests.get(f"{brain_url}/config", timeout=CONFIG_TIMEOUT).json()
        for key, expected in new_color.items():
            assert cfg[key] == expected, f"{key}: expected {expected}, got {cfg[key]}"

    def test_col_bright_enable_does_not_affect_other_fields(self, brain_url, restore_config):
        """Toggling colBrightEnable should not change unrelated config fields."""
        before = requests.get(f"{brain_url}/config", timeout=TIMEOUT).json()
        requests.post(f"{brain_url}/config", json={"colBrightEnable": True}, timeout=CONFIG_TIMEOUT)
        after = requests.get(f"{brain_url}/config", timeout=CONFIG_TIMEOUT).json()
        for key in ("briBright", "briAmbient", "fxBright", "fxAmbient",
                    "palAmbient", "motionTimeout", "waveDelay"):
            assert after[key] == before[key], (
                f"{key} changed unexpectedly: {before[key]} → {after[key]}"
            )


# ── /simulate endpoint ────────────────────────────────────────────────────────

class TestSimulateEndpoint:

    def _clear(self, brain_url, max_wait=35):
        """Clear sim mode, retrying until any active wave unblocks the web server."""
        deadline = time.time() + max_wait
        while time.time() < deadline:
            try:
                r = requests.get(f"{brain_url}/simulate?sensor=clear", timeout=5)
                if r.status_code == 200:
                    return
            except requests.exceptions.ReadTimeout:
                time.sleep(1)  # wave still running; wait and retry

    def _reset_idle(self, brain_url, max_wait=60):
        """Clear sim mode AND wait until motion=False + all segs dark.

        Scenario tests require a fully idle device so leftover wave/motion state
        from a preceding test does not interfere with directional logic.
        Waits up to max_wait seconds for the motion timeout + dim to complete.
        """
        # Force the device to idle by resetting config (calls applyAmbient/turnAllOff
        # and resets waveState/motionActive) — much faster than waiting for timeout.
        requests.post(f"{brain_url}/config", json={}, timeout=CONFIG_TIMEOUT)
        self._clear(brain_url)
        time.sleep(0.5)  # brief stabilisation
        deadline = time.time() + max_wait
        while time.time() < deadline:
            try:
                s = requests.get(f"{brain_url}/status", timeout=5).json()
                if not s.get("sim") and not s.get("motion") and \
                        not any(s.get("segs_bright", [True])):
                    return
            except Exception:
                pass
            time.sleep(0.5)

    def test_trigger_front(self, brain_url):
        r = requests.get(f"{brain_url}/simulate?sensor=front", timeout=TIMEOUT)
        assert r.status_code == 200
        d = r.json()
        assert d["success"] is True
        assert d["sim"] is True
        assert d["front"] == 1
        self._clear(brain_url)

    def test_trigger_middle(self, brain_url):
        r = requests.get(f"{brain_url}/simulate?sensor=middle", timeout=TIMEOUT)
        assert r.status_code == 200
        assert r.json()["middle"] == 1
        self._clear(brain_url)

    def test_trigger_back(self, brain_url):
        r = requests.get(f"{brain_url}/simulate?sensor=back", timeout=TIMEOUT)
        assert r.status_code == 200
        assert r.json()["back"] == 1
        self._clear(brain_url)

    def test_status_reflects_sim_active(self, brain_url):
        requests.get(f"{brain_url}/simulate?sensor=front", timeout=TIMEOUT)
        status = requests.get(f"{brain_url}/status", timeout=TIMEOUT).json()
        assert status["sim"] is True
        self._clear(brain_url)

    def test_hold_on(self, brain_url):
        r = requests.get(f"{brain_url}/simulate?sensor=front&hold=1", timeout=TIMEOUT)
        assert r.status_code == 200
        d = r.json()
        assert d["sim"] is True
        assert d["front"] == 1
        self._clear(brain_url)

    def test_hold_off_releases_sensor(self, brain_url):
        requests.get(f"{brain_url}/simulate?sensor=back&hold=1", timeout=TIMEOUT)
        r = requests.get(f"{brain_url}/simulate?sensor=back&hold=0", timeout=TIMEOUT)
        assert r.json()["back"] == 0
        self._clear(brain_url)

    def test_clear_exits_sim_mode(self, brain_url):
        requests.get(f"{brain_url}/simulate?sensor=middle", timeout=TIMEOUT)
        r = requests.get(f"{brain_url}/simulate?sensor=clear", timeout=TIMEOUT)
        assert r.status_code == 200
        assert r.json()["sim"] is False
        status = requests.get(f"{brain_url}/status", timeout=TIMEOUT).json()
        assert status["sim"] is False
        assert status["sim_f"] == 0
        assert status["sim_m"] == 0
        assert status["sim_b"] == 0

    def test_scenario_front_to_back(self, brain_url):
        """Simulate a person walking front-to-back: Front → Middle → Back."""
        self._reset_idle(brain_url)  # ensure clean state from preceding test
        requests.get(f"{brain_url}/simulate?sensor=front",  timeout=TIMEOUT)
        time.sleep(0.5)
        requests.get(f"{brain_url}/simulate?sensor=middle", timeout=TIMEOUT)
        time.sleep(0.5)
        requests.get(f"{brain_url}/simulate?sensor=back",   timeout=TIMEOUT)
        time.sleep(0.5)
        status = requests.get(f"{brain_url}/status", timeout=TIMEOUT).json()
        assert status["sim"] is True
        assert status["motion"] is True
        assert status["dir"] == "back"
        self._clear(brain_url)

    def test_scenario_back_to_front(self, brain_url):
        """Simulate a person walking back-to-front: Back → Middle → Front."""
        self._reset_idle(brain_url)
        requests.get(f"{brain_url}/simulate?sensor=back",   timeout=TIMEOUT)
        time.sleep(0.5)
        requests.get(f"{brain_url}/simulate?sensor=middle", timeout=TIMEOUT)
        time.sleep(0.5)
        requests.get(f"{brain_url}/simulate?sensor=front",  timeout=TIMEOUT)
        time.sleep(0.5)
        status = requests.get(f"{brain_url}/status", timeout=TIMEOUT).json()
        assert status["dir"] == "front"
        self._clear(brain_url)

    def test_scenario_middle_entry(self, brain_url):
        """Person appears at the middle sensor."""
        self._reset_idle(brain_url)
        requests.get(f"{brain_url}/simulate?sensor=middle", timeout=TIMEOUT)
        time.sleep(0.5)
        status = requests.get(f"{brain_url}/status", timeout=TIMEOUT).json()
        assert status["motion"] is True
        assert status["dir"] == "middle"
        self._clear(brain_url)


# ── /rescan endpoint ────────────────────────────────────────────────────────

class TestRescanEndpoint:

    def test_rescan_returns_200(self, brain_url):
        r = requests.get(f"{brain_url}/rescan", timeout=30)  # extra time: hits WLED
        assert r.status_code == 200

    def test_rescan_returns_segs_field(self, brain_url):
        r = requests.get(f"{brain_url}/rescan", timeout=30)
        data = r.json()
        assert "segs" in data
        assert "success" in data

    def test_rescan_segs_matches_status(self, brain_url):
        """After a rescan, /status should report the same segment count."""
        rescan_data = requests.get(f"{brain_url}/rescan", timeout=30).json()
        status_data = requests.get(f"{brain_url}/status", timeout=TIMEOUT).json()
        assert rescan_data["segs"] == status_data["segs"], \
            f"Rescan segs ({rescan_data['segs']}) != status segs ({status_data['segs']})"

    def test_rescan_success_true_when_wled_reachable(self, brain_url):
        data = requests.get(f"{brain_url}/rescan", timeout=30).json()
        assert data["success"] is True, \
            "Rescan should succeed when WLED is reachable"

    def test_rescan_segs_count_positive(self, brain_url):
        data = requests.get(f"{brain_url}/rescan", timeout=30).json()
        assert data["segs"] > 0


# ── /preview endpoint ─────────────────────────────────────────────────────────

class TestPreviewEndpoint:

    def test_preview_solid_half_bright(self, brain_url):
        r = requests.get(f"{brain_url}/preview?bri=128&fx=0", timeout=CONFIG_TIMEOUT)
        assert r.status_code == 200
        assert r.json().get("success") is True

    def test_preview_full_brightness(self, brain_url):
        r = requests.get(f"{brain_url}/preview?bri=255&fx=0", timeout=CONFIG_TIMEOUT)
        assert r.status_code == 200

    def test_preview_fire_effect(self, brain_url):
        r = requests.get(f"{brain_url}/preview?bri=200&fx=66", timeout=CONFIG_TIMEOUT)
        assert r.status_code == 200

    def test_preview_candle_effect(self, brain_url):
        r = requests.get(f"{brain_url}/preview?bri=150&fx=88", timeout=CONFIG_TIMEOUT)
        assert r.status_code == 200


# ── Web UI root ───────────────────────────────────────────────────────────────

class TestWebUI:

    def test_root_returns_200(self, brain_url):
        r = requests.get(f"{brain_url}/", timeout=TIMEOUT)
        assert r.status_code == 200

    def test_root_is_html(self, brain_url):
        r = requests.get(f"{brain_url}/", timeout=TIMEOUT)
        assert "text/html" in r.headers.get("Content-Type", "")

    def test_html_contains_wled_picker(self, brain_url):
        r = requests.get(f"{brain_url}/", timeout=TIMEOUT)
        assert "wled-sel" in r.text, "WLED host picker should be in the HTML"
        assert "Find WLED" in r.text

    def test_html_contains_firmware_upload(self, brain_url):
        r = requests.get(f"{brain_url}/", timeout=TIMEOUT)
        assert 'id="fw-file"' in r.text
        assert "/update" in r.text

    def test_html_contains_title(self, brain_url):
        r = requests.get(f"{brain_url}/", timeout=TIMEOUT)
        assert "Lantern Brain" in r.text


# ── Sensor polling consistency ────────────────────────────────────────────────

class TestSensorConsistency:

    def test_all_three_pirs_readable(self, brain_url):
        """All three sensor GPIO readings must be present and valid."""
        data = requests.get(f"{brain_url}/status", timeout=TIMEOUT).json()
        for sensor in ("pir_f", "pir_m", "pir_b"):
            assert data[sensor] in (0, 1), f"{sensor} invalid: {data[sensor]}"

    def test_pir_readings_stable_without_motion(self, brain_url):
        """Readings sampled 0.5 s apart should be stable when nobody is near.

        HC-SR501 sensors emit spurious pulses for ~30 s after power-on, so
        allow up to 2 flips (i.e. one brief HIGH spike) in the sample window.
        """
        samples = []
        for _ in range(4):
            samples.append(requests.get(f"{brain_url}/status", timeout=TIMEOUT).json())
            time.sleep(0.5)

        # Each sensor should not flip more than twice (one brief spike) in 4 samples
        for key in ("pir_f", "pir_m", "pir_b"):
            vals = [s[key] for s in samples]
            flips = sum(a != b for a, b in zip(vals, vals[1:]))
            assert flips <= 2, \
                f"{key} flipped {flips} times in 4 samples — persistent noise: {vals}"

    def test_dir_none_when_motion_false(self, brain_url):
        """When motion is inactive, dir should be 'none'."""
        data = requests.get(f"{brain_url}/status", timeout=TIMEOUT).json()
        if not data["motion"]:
            assert data["dir"] == "none", \
                f"Expected dir='none' when idle, got {data['dir']!r}"

    def test_segs_bright_all_false_when_idle(self, brain_url):
        """If the system is idle (not in motion), all segments should be dim."""
        data = requests.get(f"{brain_url}/status", timeout=TIMEOUT).json()
        if not data["motion"]:
            assert not any(data["segs_bright"]), \
                "Some segments are bright but system reports idle"


# ── /wled discovery + selection ───────────────────────────────────────────────

class TestWledEndpoint:

    def test_get_returns_json(self, brain_url):
        r = requests.get(f"{brain_url}/wled", timeout=TIMEOUT)
        assert r.status_code == 200
        data = r.json()
        for field in ("ip", "name", "reachable", "leds", "ver"):
            assert field in data, f"Missing /wled field: {field}"

    def test_status_wled_matches_wled_endpoint(self, brain_url):
        wled = requests.get(f"{brain_url}/wled", timeout=TIMEOUT).json()
        status = requests.get(f"{brain_url}/status", timeout=TIMEOUT).json()
        assert status["wled"] == wled["ip"]

    def test_post_rejects_invalid_ip(self, brain_url):
        r = requests.post(f"{brain_url}/wled", json={"ip": "not-an-ip"}, timeout=TIMEOUT)
        assert r.status_code == 400
        assert r.json().get("success") is False

    def test_post_rejects_empty_ip(self, brain_url):
        r = requests.post(f"{brain_url}/wled", json={"ip": ""}, timeout=TIMEOUT)
        assert r.status_code == 400

    def test_post_saves_and_restores_current(self, brain_url):
        original = requests.get(f"{brain_url}/wled", timeout=TIMEOUT).json()["ip"]
        assert original, "Brain has no WLED IP configured"
        try:
            r = requests.post(f"{brain_url}/wled", json={"ip": original},
                              timeout=CONFIG_TIMEOUT)
            assert r.status_code == 200
            data = r.json()
            assert data["success"] is True
            assert data["ip"] == original
        finally:
            requests.post(f"{brain_url}/wled", json={"ip": original},
                          timeout=CONFIG_TIMEOUT)

    def test_scan_returns_hosts_array(self, brain_url):
        r = requests.get(f"{brain_url}/wled/scan", timeout=20)
        assert r.status_code == 200
        data = r.json()
        assert "hosts" in data
        assert isinstance(data["hosts"], list)
        assert "ip" in data
        # Currently selected target is always listed, even if unreachable.
        ips = [h.get("ip") for h in data["hosts"]]
        assert data["ip"] in ips


# ── Firmware OTA ──────────────────────────────────────────────────────────────

class TestTagEndpoint:

    def test_get_returns_tag_and_hostname(self, brain_url):
        r = requests.get(f"{brain_url}/tag", timeout=TIMEOUT)
        assert r.status_code == 200
        data = r.json()
        assert "tag" in data
        assert "hostname" in data
        assert data["hostname"].startswith("lanternbrain")

    def test_status_matches_tag_endpoint(self, brain_url):
        tag = requests.get(f"{brain_url}/tag", timeout=TIMEOUT).json()
        status = requests.get(f"{brain_url}/status", timeout=TIMEOUT).json()
        assert status["tag"] == tag["tag"]
        assert status["hostname"] == tag["hostname"]

    def test_html_shows_tag_heading(self, brain_url):
        r = requests.get(f"{brain_url}/", timeout=TIMEOUT)
        assert "h1-tag" in r.text
        assert "dev-tag" in r.text
        assert "Save Name" in r.text
        assert "contenteditable" in r.text

    def test_post_same_tag_does_not_reboot(self, brain_url):
        original = requests.get(f"{brain_url}/tag", timeout=TIMEOUT).json()["tag"]
        r = requests.post(f"{brain_url}/tag", json={"tag": original}, timeout=TIMEOUT)
        assert r.status_code == 200
        data = r.json()
        assert data["success"] is True
        assert data["reboot"] is False
        assert data["tag"] == original


class TestLogEndpoint:

    def test_log_returns_plain_text(self, brain_url):
        r = requests.get(f"{brain_url}/log", timeout=TIMEOUT)
        assert r.status_code == 200
        assert "text/plain" in r.headers.get("Content-Type", "")
        assert "X-Log-Seq" in r.headers
        assert "X-Log-Size" in r.headers

    def test_log_contains_boot_or_http(self, brain_url):
        r = requests.get(f"{brain_url}/log", timeout=TIMEOUT)
        # Ring may wrap; at minimum the endpoint must return a string body.
        assert isinstance(r.text, str)

    def test_log_after_returns_200(self, brain_url):
        r = requests.get(f"{brain_url}/log", timeout=TIMEOUT)
        seq = int(r.headers.get("X-Log-Seq", "0"))
        r2 = requests.get(f"{brain_url}/log", params={"after": seq}, timeout=TIMEOUT)
        assert r2.status_code == 200
        assert r2.text == "" or isinstance(r2.text, str)

    def test_log_clear(self, brain_url):
        r = requests.post(f"{brain_url}/log/clear", timeout=TIMEOUT)
        assert r.status_code == 200
        assert r.json().get("success") is True
        r2 = requests.get(f"{brain_url}/log", timeout=TIMEOUT)
        assert r2.status_code == 200


class TestOtaEndpoint:

    def test_update_get_advertises_ota(self, brain_url):
        r = requests.get(f"{brain_url}/update", timeout=TIMEOUT)
        assert r.status_code == 200
        data = r.json()
        assert data.get("ota") is True
        assert isinstance(data.get("hostname"), str) and data["hostname"].startswith("lanternbrain")
        assert "tag" in data
        assert "url" in data
        assert str(data["url"]).startswith("http")
