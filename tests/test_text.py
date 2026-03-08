"""Tests for text rendering on AI Canvas.

Text is verified by checking that non-white pixels exist in the expected
bounding region. GFXfont y-coordinate is the baseline — text descenders go
below y, and ascenders extend above by roughly the font size. For size=24
at y=200, expect pixels in roughly y=175..210.

Fonts differ between device (GFXfont bitmaps) and PIL (TrueType), so we
only check for pixel presence, not exact shape.
"""

import os
import pytest
from pbm_utils import region_has_pixels, count_pixels_in_region

DISPLAY_W, DISPLAY_H = 800, 480


def save_screenshot(img, output_dir, name):
    img.save(os.path.join(output_dir, f"{name}.png"))


class TestTextRenders:
    def test_text_renders(self, canvas, output_dir):
        """Basic text should produce black pixels near the specified position."""
        commands = [
            {"op": "clear", "color": "white"},
            {"op": "text", "x": 100, "y": 200, "text": "Hello World", "size": 24},
        ]
        canvas.render_and_wait(commands)
        shot = canvas.screenshot()
        save_screenshot(shot, output_dir, "text_renders")

        # GFXfont y=200 is baseline; text extends above and below
        assert region_has_pixels(shot, 80, 170, 400, 60, color=0), \
            "No black pixels found near text position"


class TestTextAlignment:
    def test_text_center(self, canvas, output_dir):
        """Center-aligned text should have pixels centered around x=400."""
        commands = [
            {"op": "clear", "color": "white"},
            {"op": "text", "x": 400, "y": 240, "text": "Centered", "size": 24, "align": "center"},
        ]
        canvas.render_and_wait(commands)
        shot = canvas.screenshot()
        save_screenshot(shot, output_dir, "text_center")

        # Pixels should be roughly centered around x=400
        left_count = count_pixels_in_region(shot, 250, 210, 150, 60, color=0)
        right_count = count_pixels_in_region(shot, 400, 210, 150, 60, color=0)

        assert left_count > 0, "No pixels on left side of center"
        assert right_count > 0, "No pixels on right side of center"

    def test_text_right(self, canvas, output_dir):
        """Right-aligned text should end near x=700."""
        commands = [
            {"op": "clear", "color": "white"},
            {"op": "text", "x": 700, "y": 240, "text": "Right", "size": 24, "align": "right"},
        ]
        canvas.render_and_wait(commands)
        shot = canvas.screenshot()
        save_screenshot(shot, output_dir, "text_right")

        # Pixels should exist to the LEFT of x=700, within ~200px
        assert region_has_pixels(shot, 500, 210, 200, 60, color=0), \
            "No pixels found left of right-align anchor"

        # Little or no pixels should exist far to the RIGHT of x=700
        far_right = count_pixels_in_region(shot, 720, 210, 80, 60, color=0)
        assert far_right == 0, f"Unexpected {far_right} pixels right of x=720"


class TestTextStyles:
    def test_text_bold(self, canvas, output_dir):
        """Bold text should render wider than regular text."""
        commands = [
            {"op": "clear", "color": "white"},
            {"op": "text", "x": 100, "y": 150, "text": "Bold Text", "size": 24, "bold": True},
            {"op": "text", "x": 100, "y": 300, "text": "Bold Text", "size": 24, "bold": False},
        ]
        canvas.render_and_wait(commands)
        shot = canvas.screenshot()
        save_screenshot(shot, output_dir, "text_bold")

        bold_pixels = count_pixels_in_region(shot, 80, 120, 400, 60, color=0)
        regular_pixels = count_pixels_in_region(shot, 80, 270, 400, 60, color=0)

        assert bold_pixels > 0, "Bold text not rendered"
        assert regular_pixels > 0, "Regular text not rendered"
        # Bold should use more pixels (thicker strokes)
        assert bold_pixels > regular_pixels, \
            f"Bold ({bold_pixels}px) should be wider than regular ({regular_pixels}px)"

    def test_text_sizes(self, canvas, output_dir):
        """Larger font size should produce more pixels than smaller font."""
        commands = [
            {"op": "clear", "color": "white"},
            {"op": "text", "x": 100, "y": 100, "text": "Small", "size": 9},
            {"op": "text", "x": 100, "y": 300, "text": "Large", "size": 24},
        ]
        canvas.render_and_wait(commands)
        shot = canvas.screenshot()
        save_screenshot(shot, output_dir, "text_sizes")

        # GFXfont y is baseline. Size 9 at y=100 renders ~y=100..125.
        # Size 24 at y=300 renders ~y=320..360.
        # Use generous regions that fully contain each text.
        small_pixels = count_pixels_in_region(shot, 80, 95, 300, 40, color=0)
        large_pixels = count_pixels_in_region(shot, 80, 310, 300, 60, color=0)

        assert small_pixels > 0, "Small text not rendered"
        assert large_pixels > 0, "Large text not rendered"
        assert large_pixels > small_pixels, \
            f"Size 24 ({large_pixels}px) should use more pixels than size 9 ({small_pixels}px)"


class TestTextWrap:
    def test_text_wrap(self, canvas, output_dir):
        """Long text with maxWidth should wrap to multiple lines."""
        long_text = "This is a long sentence that should wrap to multiple lines within the specified maximum width."
        commands = [
            {"op": "clear", "color": "white"},
            {"op": "text", "x": 100, "y": 100, "text": long_text, "size": 18, "maxWidth": 300},
        ]
        canvas.render_and_wait(commands)
        shot = canvas.screenshot()
        save_screenshot(shot, output_dir, "text_wrap")

        # Text should span multiple lines starting near y=100
        # First line area
        assert region_has_pixels(shot, 80, 80, 320, 60, color=0), "First line missing"
        # Wrapped lines further down
        assert region_has_pixels(shot, 80, 140, 320, 100, color=0), "Wrapped lines missing"
        # Should NOT extend beyond maxWidth (100 + 300 = 400, plus margin)
        overflow = count_pixels_in_region(shot, 430, 80, 370, 200, color=0)
        assert overflow == 0, f"Text overflows maxWidth: {overflow} pixels past x=430"


class TestTextColorInverted:
    def test_text_white_on_black(self, canvas, output_dir):
        """White text on black background should show white pixels."""
        commands = [
            {"op": "clear", "color": "black"},
            {"op": "text", "x": 400, "y": 240, "text": "White Text", "size": 24,
             "color": "white", "align": "center"},
        ]
        canvas.render_and_wait(commands)
        shot = canvas.screenshot()
        save_screenshot(shot, output_dir, "text_white_on_black")

        # Check for white pixels in the text area (on black background)
        assert region_has_pixels(shot, 200, 210, 400, 60, color=255), \
            "No white pixels found for white text on black"
