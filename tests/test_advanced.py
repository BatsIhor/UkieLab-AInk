"""Tests for advanced rendering: gradient, clipping, flood fill, raw bitmap, pixel."""

import base64
import os
import pytest
from pbm_utils import region_has_pixels, count_pixels_in_region

DISPLAY_W, DISPLAY_H = 800, 480


def save_screenshot(img, output_dir, name):
    img.save(os.path.join(output_dir, f"{name}.png"))


def avg_darkness(img, x, y, w, h):
    """Return average darkness (0.0=white, 1.0=black) of a region."""
    px = img.convert("L").load()
    img_w, img_h = img.size
    total = 0
    count = 0
    for py in range(max(0, y), min(img_h, y + h)):
        for px_x in range(max(0, x), min(img_w, x + w)):
            total += (255 - px[px_x, py]) / 255.0
            count += 1
    return total / count if count > 0 else 0


class TestGradient:
    def test_gradient_horizontal(self, canvas, output_dir):
        """Horizontal gradient: left side should be darker than right side."""
        commands = [
            {"op": "clear", "color": "white"},
            {"op": "gradient", "x": 0, "y": 0, "w": 800, "h": 480,
             "direction": "horizontal", "from": "black", "to": "white"},
        ]
        canvas.render_and_wait(commands)
        shot = canvas.screenshot()
        save_screenshot(shot, output_dir, "gradient_horizontal")

        left_dark = avg_darkness(shot, 0, 0, 200, 480)
        right_dark = avg_darkness(shot, 600, 0, 200, 480)
        assert left_dark > right_dark, \
            f"Left ({left_dark:.2f}) should be darker than right ({right_dark:.2f})"

    def test_gradient_vertical(self, canvas, output_dir):
        """Vertical gradient: top should be darker than bottom."""
        commands = [
            {"op": "clear", "color": "white"},
            {"op": "gradient", "x": 0, "y": 0, "w": 800, "h": 480,
             "direction": "vertical", "from": "black", "to": "white"},
        ]
        canvas.render_and_wait(commands)
        shot = canvas.screenshot()
        save_screenshot(shot, output_dir, "gradient_vertical")

        top_dark = avg_darkness(shot, 0, 0, 800, 120)
        bottom_dark = avg_darkness(shot, 0, 360, 800, 120)
        assert top_dark > bottom_dark, \
            f"Top ({top_dark:.2f}) should be darker than bottom ({bottom_dark:.2f})"


class TestClipping:
    def test_clip(self, canvas, output_dir):
        """Clipped rect should only appear within clip region."""
        commands = [
            {"op": "clear", "color": "white"},
            {"op": "clip", "x": 200, "y": 200, "w": 200, "h": 200},
            {"op": "rect", "x": 0, "y": 0, "w": 800, "h": 480, "fill": True, "color": "black"},
        ]
        canvas.render_and_wait(commands)
        shot = canvas.screenshot()
        save_screenshot(shot, output_dir, "clip")

        # Inside clip region should be black
        assert region_has_pixels(shot, 220, 220, 160, 160, color=0), "Clip interior should be black"

        # Outside clip region should be white
        outside_tl = count_pixels_in_region(shot, 0, 0, 190, 190, color=0)
        outside_br = count_pixels_in_region(shot, 410, 410, 390, 70, color=0)
        assert outside_tl == 0, f"Top-left should be white, found {outside_tl} black pixels"
        assert outside_br == 0, f"Bottom-right should be white, found {outside_br} black pixels"

    def test_unclip(self, canvas, output_dir):
        """After unclip, drawing should extend to full screen."""
        commands = [
            {"op": "clear", "color": "white"},
            {"op": "clip", "x": 200, "y": 200, "w": 200, "h": 200},
            {"op": "rect", "x": 0, "y": 0, "w": 100, "h": 100, "fill": True, "color": "black"},
            {"op": "unclip"},
            {"op": "rect", "x": 0, "y": 0, "w": 100, "h": 100, "fill": True, "color": "black"},
        ]
        canvas.render_and_wait(commands)
        shot = canvas.screenshot()
        save_screenshot(shot, output_dir, "unclip")

        # After unclip, rect at (0,0) should be visible
        assert region_has_pixels(shot, 0, 0, 100, 100, color=0), \
            "Rect at (0,0) should be visible after unclip"


class TestFloodFill:
    def test_flood_fill(self, canvas, output_dir):
        """Flood fill inside a small rect outline should fill the interior.

        The device allocates a 4096-entry BFS queue (~16KB) via malloc.
        If heap is fragmented this allocation can fail. We test with a small
        region and accept that the operation may OOM on low-memory conditions.
        """
        commands = [
            {"op": "clear", "color": "white"},
            {"op": "rect", "x": 380, "y": 220, "w": 42, "h": 42, "fill": False, "color": "black"},
            {"op": "flood_fill", "x": 401, "y": 241, "color": "black"},
        ]
        result = canvas.render_and_wait(commands)
        shot = canvas.screenshot()
        save_screenshot(shot, output_dir, "flood_fill")

        warnings = result.get("warnings", [])
        oom = any("out of memory" in w for w in warnings)

        if oom:
            pytest.skip("flood_fill: device heap too fragmented to allocate BFS queue")

        # Interior should now be filled black
        interior_black = count_pixels_in_region(shot, 382, 222, 38, 38, color=0)
        interior_total = 38 * 38
        fill_ratio = interior_black / interior_total
        assert fill_ratio > 0.90, f"Interior should be mostly black, got {fill_ratio:.1%}"

        # Exterior should still be white
        exterior = count_pixels_in_region(shot, 0, 0, 370, 210, color=0)
        assert exterior == 0, f"Exterior should be white, found {exterior} black pixels"


class TestRawBitmap:
    def test_raw_bitmap(self, canvas, output_dir):
        """8x8 checkerboard raw bitmap should produce exact pixel pattern."""
        # Create an 8x8 checkerboard pattern
        # raw_bitmap: 0=black, 1=white, MSB first
        rows = []
        for y in range(8):
            byte_val = 0
            for x in range(8):
                bit = 7 - x
                # Checkerboard: (x+y) % 2 == 0 → white (1), else black (0)
                if (x + y) % 2 == 0:
                    byte_val |= (1 << bit)
            rows.append(byte_val)
        bitmap_data = base64.b64encode(bytes(rows)).decode()

        commands = [
            {"op": "clear", "color": "white"},
            {"op": "raw_bitmap", "x": 100, "y": 100, "w": 8, "h": 8, "data": bitmap_data},
        ]
        canvas.render_and_wait(commands)
        shot = canvas.screenshot()
        save_screenshot(shot, output_dir, "raw_bitmap")

        # Verify checkerboard pattern
        px = shot.convert("L").load()
        for y in range(8):
            for x in range(8):
                expected_white = (x + y) % 2 == 0
                actual_white = px[100 + x, 100 + y] >= 128
                assert actual_white == expected_white, \
                    f"Pixel ({x},{y}): expected {'white' if expected_white else 'black'}, got {'white' if actual_white else 'black'}"


class TestPixel:
    def test_pixel(self, canvas, output_dir):
        """Individual pixels should be set exactly."""
        commands = [
            {"op": "clear", "color": "white"},
            {"op": "pixel", "x": 100, "y": 100, "color": "black"},
            {"op": "pixel", "x": 200, "y": 200, "color": "black"},
        ]
        canvas.render_and_wait(commands)
        shot = canvas.screenshot()
        save_screenshot(shot, output_dir, "pixel")

        px = shot.convert("L").load()
        assert px[100, 100] < 128, "Pixel at (100,100) should be black"
        assert px[200, 200] < 128, "Pixel at (200,200) should be black"
        # Neighbors should be white
        assert px[101, 100] >= 128, "Neighbor should be white"
        assert px[100, 101] >= 128, "Neighbor should be white"
