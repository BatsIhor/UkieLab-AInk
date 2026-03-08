"""Tests for API endpoint behavior (non-rendering)."""

import os
import pytest


class TestHealth:
    def test_health(self, canvas):
        """GET /health should return expected fields."""
        h = canvas.health()
        assert h.get("status") == "ok"
        assert "uptime_ms" in h
        assert "free_heap" in h
        assert "wifi_rssi" in h
        assert isinstance(h["uptime_ms"], (int, float))


class TestDevice:
    def test_device(self, canvas):
        """GET /device should return display info, fonts, and operations."""
        d = canvas.device_info()
        assert d["display"]["width"] == 800
        assert d["display"]["height"] == 480
        assert isinstance(d.get("fonts"), list)
        assert len(d["fonts"]) > 0
        assert isinstance(d.get("operations"), list)
        assert "clear" in d["operations"]
        assert "text" in d["operations"]
        assert "rect" in d["operations"]


class TestCanvasGet:
    def test_canvas_get(self, canvas):
        """GET /canvas after rendering should return frame info."""
        canvas.render_and_wait([
            {"op": "clear", "color": "white"},
            {"op": "rect", "x": 0, "y": 0, "w": 100, "h": 100, "fill": True},
        ])
        info = canvas.get_canvas()
        assert "frame_id" in info
        assert info.get("command_count", len(info.get("commands", []))) > 0

    def test_frame_id_increments(self, canvas):
        """Each render should increment the frame_id."""
        canvas.render_and_wait([{"op": "clear", "color": "white"}])
        info1 = canvas.get_canvas()
        fid1 = info1["frame_id"]

        canvas.render_and_wait([{"op": "clear", "color": "black"}])
        info2 = canvas.get_canvas()
        fid2 = info2["frame_id"]

        assert fid2 > fid1, f"frame_id should increment: {fid1} → {fid2}"


class TestEdgeCases:
    def test_unknown_op(self, canvas):
        """Unknown operation should produce a warning but not fail."""
        result = canvas.render([{"op": "bogus_operation"}])
        assert result.get("status") == "ok"
        assert result.get("commands_executed", 0) == 0
        warnings = result.get("warnings", [])
        assert len(warnings) > 0, "Expected a warning for unknown op"

    def test_empty_commands(self, canvas):
        """Empty command array should succeed with 0 commands executed."""
        result = canvas.render([])
        assert result.get("status") == "ok"
        assert result.get("commands_executed", 0) == 0


class TestScreenshotFormat:
    def test_screenshot_format(self, canvas):
        """GET /canvas/screenshot should return valid PBM P4 data."""
        raw = canvas.screenshot_raw()

        # Should start with P4 magic
        assert raw[:2] == b"P4", f"Expected PBM P4 magic, got {raw[:10]!r}"

        # Parse header to find dimensions
        lines = raw.split(b"\n", 3)
        assert lines[0] == b"P4"
        dims = lines[1].split()
        width, height = int(dims[0]), int(dims[1])
        assert width == 800, f"Expected width 800, got {width}"
        assert height == 480, f"Expected height 480, got {height}"

        # Data should be at least (800/8)*480 = 48000 bytes
        header_len = len(lines[0]) + 1 + len(lines[1]) + 1
        pixel_data_len = len(raw) - header_len
        expected_min = (800 + 7) // 8 * 480  # 48000
        assert pixel_data_len >= expected_min, \
            f"Expected at least {expected_min} bytes of pixel data, got {pixel_data_len}"

    def test_screenshot_parses(self, canvas):
        """Screenshot should parse into a valid PIL image."""
        img = canvas.screenshot()
        assert img.size == (800, 480)
        assert img.mode == "1"
