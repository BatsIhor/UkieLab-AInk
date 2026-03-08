"""PBM P4 binary parser and image comparison utilities."""

import io
from dataclasses import dataclass
from PIL import Image, ImageChops


def parse_pbm(data: bytes) -> Image.Image:
    """Parse binary PBM P4 data into a PIL 1-bit Image.

    PBM P4 format:
      P4\\n
      <width> <height>\\n
      <binary data: 1 bit per pixel, MSB first, rows padded to byte boundary>

    In PBM: 1=black, 0=white. PIL mode "1": 0=black, 255=white.
    We return a mode "1" image where 0=black, 255=white.
    """
    stream = io.BytesIO(data)

    magic = stream.readline().strip()
    if magic != b"P4":
        raise ValueError(f"Expected PBM P4 magic, got {magic!r}")

    # Skip comments
    line = stream.readline()
    while line.startswith(b"#"):
        line = stream.readline()

    parts = line.strip().split()
    width, height = int(parts[0]), int(parts[1])

    row_bytes = (width + 7) // 8
    pixel_data = stream.read(row_bytes * height)

    if len(pixel_data) < row_bytes * height:
        raise ValueError(
            f"Expected {row_bytes * height} bytes of pixel data, got {len(pixel_data)}"
        )

    # Build image from packed bits
    img = Image.new("1", (width, height), 255)
    pixels = img.load()

    for row in range(height):
        offset = row * row_bytes
        for col in range(width):
            byte_idx = offset + col // 8
            bit_idx = 7 - (col % 8)
            bit = (pixel_data[byte_idx] >> bit_idx) & 1
            # PBM: 1=black, 0=white; PIL mode "1": 0=black, 255=white
            if bit:
                pixels[col, row] = 0  # black

    return img


@dataclass
class CompareResult:
    match_ratio: float
    total_pixels: int
    matching_pixels: int
    diff_image: Image.Image | None

    @property
    def passed(self) -> bool:
        return self.match_ratio >= 0.97

    def __repr__(self):
        pct = self.match_ratio * 100
        return f"CompareResult({pct:.1f}% match, {self.total_pixels - self.matching_pixels} diff pixels)"


def compare_images(
    actual: Image.Image, expected: Image.Image, tolerance_px: int = 2
) -> CompareResult:
    """Compare two 1-bit images with edge tolerance.

    For each differing pixel, check if there's a matching pixel within
    tolerance_px distance in the expected image. This accounts for
    rasterization differences at shape edges.
    """
    if actual.size != expected.size:
        raise ValueError(f"Size mismatch: {actual.size} vs {expected.size}")

    w, h = actual.size
    total = w * h

    # Convert to mode "L" for easier pixel access
    act = actual.convert("L")
    exp = expected.convert("L")
    act_px = act.load()
    exp_px = exp.load()

    matching = 0
    diff_img = Image.new("RGB", (w, h), (255, 255, 255))
    diff_px = diff_img.load()

    for y in range(h):
        for x in range(w):
            a_val = 0 if act_px[x, y] < 128 else 255
            e_val = 0 if exp_px[x, y] < 128 else 255

            if a_val == e_val:
                matching += 1
                diff_px[x, y] = (200, 200, 200) if a_val == 0 else (255, 255, 255)
            else:
                # Check within tolerance radius
                found = False
                if tolerance_px > 0:
                    for dy in range(-tolerance_px, tolerance_px + 1):
                        for dx in range(-tolerance_px, tolerance_px + 1):
                            nx, ny = x + dx, y + dy
                            if 0 <= nx < w and 0 <= ny < h:
                                n_val = 0 if exp_px[nx, ny] < 128 else 255
                                if n_val == a_val:
                                    found = True
                                    break
                        if found:
                            break

                if found:
                    matching += 1
                    diff_px[x, y] = (255, 255, 200)  # yellow: tolerated
                else:
                    # Red: actual has black where expected is white
                    # Blue: actual has white where expected is black
                    if a_val == 0:
                        diff_px[x, y] = (255, 0, 0)
                    else:
                        diff_px[x, y] = (0, 0, 255)

    return CompareResult(
        match_ratio=matching / total if total > 0 else 1.0,
        total_pixels=total,
        matching_pixels=matching,
        diff_image=diff_img,
    )


def region_has_pixels(
    img: Image.Image, x: int, y: int, w: int, h: int, color: int = 0
) -> bool:
    """Check if a region contains pixels of the specified color.

    color=0 means black, color=255 means white.
    """
    px = img.convert("L").load()
    img_w, img_h = img.size

    for py in range(max(0, y), min(img_h, y + h)):
        for px_x in range(max(0, x), min(img_w, x + w)):
            val = 0 if px[px_x, py] < 128 else 255
            if val == color:
                return True
    return False


def count_pixels_in_region(
    img: Image.Image, x: int, y: int, w: int, h: int, color: int = 0
) -> int:
    """Count pixels of specified color in a region."""
    px = img.convert("L").load()
    img_w, img_h = img.size
    count = 0

    for py in range(max(0, y), min(img_h, y + h)):
        for px_x in range(max(0, x), min(img_w, x + w)):
            val = 0 if px[px_x, py] < 128 else 255
            if val == color:
                count += 1
    return count


def save_diff(actual: Image.Image, expected: Image.Image, path: str):
    """Save a side-by-side comparison image."""
    w, h = actual.size
    combined = Image.new("RGB", (w * 3, h), (255, 255, 255))

    # Left: actual
    combined.paste(actual.convert("RGB"), (0, 0))
    # Middle: expected
    combined.paste(expected.convert("RGB"), (w, 0))
    # Right: diff
    result = compare_images(actual, expected)
    if result.diff_image:
        combined.paste(result.diff_image, (w * 2, 0))

    combined.save(path)
