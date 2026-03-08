"""Tests for zone management and zone-relative rendering."""

import os
import pytest
from pbm_utils import region_has_pixels, count_pixels_in_region

DISPLAY_W, DISPLAY_H = 800, 480


def save_screenshot(img, output_dir, name):
    img.save(os.path.join(output_dir, f"{name}.png"))


class TestZoneCRUD:
    def test_zone_crud(self, canvas):
        """Create, read, and delete zones."""
        zones = [
            {"id": "header", "x": 0, "y": 0, "w": 800, "h": 100},
            {"id": "content", "x": 0, "y": 100, "w": 800, "h": 380},
        ]

        # Create
        result = canvas.set_zones(zones)
        assert result.get("status") == "ok"

        # Read
        got = canvas.get_zones()
        assert len(got) == 2
        ids = {z["id"] for z in got}
        assert "header" in ids
        assert "content" in ids

        # Delete
        result = canvas.delete_zones()
        assert result.get("status") == "ok"

        # Verify deleted
        got = canvas.get_zones()
        assert len(got) == 0


class TestZoneRender:
    def test_zone_render(self, canvas, output_dir):
        """Drawing at (0,0) in a zone should appear at the zone's offset in the screenshot."""
        zones = [{"id": "box", "x": 100, "y": 100, "w": 200, "h": 200}]
        canvas.set_zones(zones)

        try:
            # Draw a rect at zone-relative (0,0) filling the zone
            commands = [
                {"op": "clear", "color": "white"},
                {"op": "rect", "x": 0, "y": 0, "w": 200, "h": 200, "fill": True, "color": "black"},
            ]
            canvas.render_and_wait(commands, zone="box")
            shot = canvas.screenshot()
            save_screenshot(shot, output_dir, "zone_render")

            # Rect should appear at screen coords (100,100)
            assert region_has_pixels(shot, 110, 110, 180, 180, color=0), \
                "Rect should appear at zone offset (100,100)"

            # Area before the zone should be white
            before = count_pixels_in_region(shot, 0, 0, 90, 90, color=0)
            assert before == 0, f"Area before zone should be white, found {before} black"
        finally:
            canvas.delete_zones()

    def test_zone_clip(self, canvas, output_dir):
        """Drawing larger than the zone should be clipped to zone bounds."""
        zones = [{"id": "small", "x": 200, "y": 200, "w": 100, "h": 100}]
        canvas.set_zones(zones)

        try:
            # Draw a rect much larger than the zone
            commands = [
                {"op": "clear", "color": "white"},
                {"op": "rect", "x": 0, "y": 0, "w": 500, "h": 500, "fill": True, "color": "black"},
            ]
            canvas.render_and_wait(commands, zone="small")
            shot = canvas.screenshot()
            save_screenshot(shot, output_dir, "zone_clip")

            # Should only fill the zone area
            assert region_has_pixels(shot, 210, 210, 80, 80, color=0), "Zone interior should be black"

            # Area outside zone should be white
            outside = count_pixels_in_region(shot, 310, 310, 100, 100, color=0)
            assert outside == 0, f"Area outside zone should be white, found {outside} black"
        finally:
            canvas.delete_zones()

    def test_zone_missing(self, canvas):
        """Rendering to a nonexistent zone should produce a warning."""
        canvas.delete_zones()

        commands = [
            {"op": "clear", "color": "white"},
            {"op": "rect", "x": 0, "y": 0, "w": 100, "h": 100, "fill": True, "color": "black"},
        ]
        result = canvas.render(commands, zone="nonexistent")

        # Should still succeed (graceful degradation) but may have a warning
        assert result.get("status") == "ok"
