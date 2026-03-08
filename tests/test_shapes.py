"""Tests for basic shape rendering on AI Canvas."""

import os
import pytest
from PIL import Image, ImageDraw
from pbm_utils import compare_images, save_diff, region_has_pixels, count_pixels_in_region

DISPLAY_W, DISPLAY_H = 800, 480


def make_expected(draw_fn) -> Image.Image:
    """Create a PIL reference image by calling draw_fn(ImageDraw)."""
    img = Image.new("1", (DISPLAY_W, DISPLAY_H), 255)
    draw = ImageDraw.Draw(img)
    draw_fn(draw)
    return img


def save_screenshot(img, output_dir, name):
    path = os.path.join(output_dir, f"{name}.png")
    img.save(path)
    return path


class TestClear:
    def test_clear_white(self, canvas, output_dir):
        canvas.render_and_wait([{"op": "clear", "color": "white"}])
        shot = canvas.screenshot()
        save_screenshot(shot, output_dir, "clear_white")

        # All pixels should be white
        black_count = count_pixels_in_region(shot, 0, 0, DISPLAY_W, DISPLAY_H, color=0)
        assert black_count == 0, f"Expected all white, found {black_count} black pixels"

    def test_clear_black(self, canvas, output_dir):
        canvas.render_and_wait([{"op": "clear", "color": "black"}])
        shot = canvas.screenshot()
        save_screenshot(shot, output_dir, "clear_black")

        white_count = count_pixels_in_region(shot, 0, 0, DISPLAY_W, DISPLAY_H, color=255)
        assert white_count == 0, f"Expected all black, found {white_count} white pixels"


class TestRect:
    def test_filled_rect(self, canvas, output_dir):
        commands = [
            {"op": "clear", "color": "white"},
            {"op": "rect", "x": 100, "y": 100, "w": 200, "h": 100, "fill": True, "color": "black"},
        ]
        canvas.render_and_wait(commands)
        shot = canvas.screenshot()
        save_screenshot(shot, output_dir, "filled_rect")

        expected = make_expected(lambda d: d.rectangle([100, 100, 299, 199], fill=0))
        result = compare_images(shot, expected)
        save_diff(shot, expected, os.path.join(output_dir, "filled_rect_diff.png"))
        assert result.passed, f"Filled rect: {result}"

    def test_outline_rect(self, canvas, output_dir):
        commands = [
            {"op": "clear", "color": "white"},
            {"op": "rect", "x": 100, "y": 100, "w": 200, "h": 100, "fill": False, "color": "black"},
        ]
        canvas.render_and_wait(commands)
        shot = canvas.screenshot()
        save_screenshot(shot, output_dir, "outline_rect")

        # Border pixels should be black
        assert region_has_pixels(shot, 100, 100, 200, 1, color=0), "Top border missing"
        assert region_has_pixels(shot, 100, 199, 200, 1, color=0), "Bottom border missing"
        assert region_has_pixels(shot, 100, 100, 1, 100, color=0), "Left border missing"
        assert region_has_pixels(shot, 299, 100, 1, 100, color=0), "Right border missing"

        # Interior should be white (sample center area)
        interior_black = count_pixels_in_region(shot, 110, 110, 180, 80, color=0)
        assert interior_black == 0, f"Interior should be white, found {interior_black} black pixels"

    def test_rounded_rect(self, canvas, output_dir):
        commands = [
            {"op": "clear", "color": "white"},
            {"op": "rect", "x": 100, "y": 100, "w": 600, "h": 300, "radius": 20, "fill": True, "color": "black"},
        ]
        canvas.render_and_wait(commands)
        shot = canvas.screenshot()
        save_screenshot(shot, output_dir, "rounded_rect")

        # Center should be black
        assert region_has_pixels(shot, 300, 200, 100, 100, color=0), "Center should be black"

        # Corners should be white (the rounded-off area)
        # Check a pixel right at the corner that should be white due to rounding
        px = shot.convert("L").load()
        corner_val = 0 if px[100, 100] < 128 else 255
        assert corner_val == 255, "Corner (100,100) should be white due to rounding"


class TestCircle:
    def test_circle_filled(self, canvas, output_dir):
        cx, cy, r = 400, 240, 80
        commands = [
            {"op": "clear", "color": "white"},
            {"op": "circle", "x": cx, "y": cy, "r": r, "fill": True, "color": "black"},
        ]
        canvas.render_and_wait(commands)
        shot = canvas.screenshot()
        save_screenshot(shot, output_dir, "circle_filled")

        expected = make_expected(
            lambda d: d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=0)
        )
        result = compare_images(shot, expected)
        save_diff(shot, expected, os.path.join(output_dir, "circle_filled_diff.png"))
        assert result.passed, f"Filled circle: {result}"

    def test_circle_outline(self, canvas, output_dir):
        cx, cy, r = 400, 240, 80
        commands = [
            {"op": "clear", "color": "white"},
            {"op": "circle", "x": cx, "y": cy, "r": r, "fill": False, "color": "black"},
        ]
        canvas.render_and_wait(commands)
        shot = canvas.screenshot()
        save_screenshot(shot, output_dir, "circle_outline")

        # Center should be white (it's just an outline)
        center_black = count_pixels_in_region(shot, cx - 10, cy - 10, 20, 20, color=0)
        assert center_black == 0, f"Circle center should be white, found {center_black} black pixels"

        # Edge should have black pixels
        assert region_has_pixels(shot, cx + r - 5, cy - 5, 10, 10, color=0), "Right edge missing"


class TestLine:
    def test_line_diagonal(self, canvas, output_dir):
        commands = [
            {"op": "clear", "color": "white"},
            {"op": "line", "x0": 0, "y0": 0, "x1": 799, "y1": 479, "color": "black"},
        ]
        canvas.render_and_wait(commands)
        shot = canvas.screenshot()
        save_screenshot(shot, output_dir, "line_diagonal")

        expected = make_expected(lambda d: d.line([0, 0, 799, 479], fill=0, width=1))
        result = compare_images(shot, expected)
        save_diff(shot, expected, os.path.join(output_dir, "line_diagonal_diff.png"))
        assert result.passed, f"Diagonal line: {result}"

    def test_line_thick(self, canvas, output_dir):
        commands = [
            {"op": "clear", "color": "white"},
            {"op": "line", "x0": 100, "y0": 100, "x1": 700, "y1": 100, "color": "black", "thickness": 5},
        ]
        canvas.render_and_wait(commands)
        shot = canvas.screenshot()
        save_screenshot(shot, output_dir, "line_thick")

        # Verify thick horizontal line exists
        assert region_has_pixels(shot, 100, 98, 600, 9, color=0), "Thick line area should have black pixels"

        # Area above and below should be white
        above_black = count_pixels_in_region(shot, 100, 80, 600, 15, color=0)
        below_black = count_pixels_in_region(shot, 100, 108, 600, 15, color=0)
        assert above_black == 0, f"Area above line should be white, found {above_black}"
        assert below_black == 0, f"Area below line should be white, found {below_black}"


class TestEllipse:
    def test_ellipse(self, canvas, output_dir):
        cx, cy, rx, ry = 400, 240, 150, 80
        commands = [
            {"op": "clear", "color": "white"},
            {"op": "ellipse", "x": cx, "y": cy, "rx": rx, "ry": ry, "fill": True, "color": "black"},
        ]
        canvas.render_and_wait(commands)
        shot = canvas.screenshot()
        save_screenshot(shot, output_dir, "ellipse")

        expected = make_expected(
            lambda d: d.ellipse([cx - rx, cy - ry, cx + rx, cy + ry], fill=0)
        )
        result = compare_images(shot, expected)
        save_diff(shot, expected, os.path.join(output_dir, "ellipse_diff.png"))
        assert result.passed, f"Ellipse: {result}"


class TestPolygon:
    def test_polygon_triangle(self, canvas, output_dir):
        points = [{"x": 400, "y": 100}, {"x": 550, "y": 350}, {"x": 250, "y": 350}]
        commands = [
            {"op": "clear", "color": "white"},
            {"op": "polygon", "points": points, "fill": True, "color": "black"},
        ]
        canvas.render_and_wait(commands)
        shot = canvas.screenshot()
        save_screenshot(shot, output_dir, "polygon_triangle")

        pil_points = [(p["x"], p["y"]) for p in points]
        expected = make_expected(lambda d: d.polygon(pil_points, fill=0))
        result = compare_images(shot, expected)
        save_diff(shot, expected, os.path.join(output_dir, "polygon_triangle_diff.png"))
        assert result.passed, f"Triangle: {result}"


class TestPolyline:
    def test_polyline(self, canvas, output_dir):
        points = [
            {"x": 100, "y": 240},
            {"x": 250, "y": 100},
            {"x": 400, "y": 380},
            {"x": 550, "y": 100},
            {"x": 700, "y": 240},
        ]
        commands = [
            {"op": "clear", "color": "white"},
            {"op": "polyline", "points": points, "color": "black"},
        ]
        canvas.render_and_wait(commands)
        shot = canvas.screenshot()
        save_screenshot(shot, output_dir, "polyline")

        # Verify connected segments by checking that pixels exist near each point
        for p in points:
            assert region_has_pixels(shot, p["x"] - 5, p["y"] - 5, 10, 10, color=0), \
                f"No pixels near point ({p['x']}, {p['y']})"


class TestMultipleShapes:
    def test_multiple_shapes(self, canvas, output_dir):
        commands = [
            {"op": "clear", "color": "white"},
            {"op": "rect", "x": 50, "y": 50, "w": 200, "h": 150, "fill": True, "color": "black"},
            {"op": "circle", "x": 500, "y": 150, "r": 80, "fill": True, "color": "black"},
            {"op": "line", "x0": 100, "y0": 350, "x1": 700, "y1": 350, "color": "black", "thickness": 3},
        ]
        canvas.render_and_wait(commands)
        shot = canvas.screenshot()
        save_screenshot(shot, output_dir, "multiple_shapes")

        # Verify each shape region has black pixels
        assert region_has_pixels(shot, 50, 50, 200, 150, color=0), "Rect missing"
        assert region_has_pixels(shot, 420, 70, 160, 160, color=0), "Circle missing"
        assert region_has_pixels(shot, 100, 348, 600, 5, color=0), "Line missing"
