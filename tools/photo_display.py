#!/usr/bin/env python3
"""
Photo Display for AI Canvas E-Ink
Uploads a local image file as a full-screen photo (no text/header/footer).

Dithering is done client-side with Floyd-Steinberg, then pushed as a raw
1-bit framebuffer via /canvas/bitmap — zero decoding on the device.

Usage:
    python3 tools/photo_display.py path/to/photo.jpg
    python3 tools/photo_display.py photo.png --ip 192.168.87.42
    python3 tools/photo_display.py photo.jpg --fit fill
"""

import argparse
import io
import json
import sys
import urllib.request
from pathlib import Path

from PIL import Image, ImageOps

DISPLAY_IP = "192.168.87.63"
HEADERS = {"User-Agent": "AInk-Photo/1.0"}
DISPLAY_W = 800
DISPLAY_H = 480


def get_device_info():
    """Fetch display dimensions from the device."""
    global DISPLAY_W, DISPLAY_H
    try:
        req = urllib.request.Request(f"http://{DISPLAY_IP}/device", headers=HEADERS)
        with urllib.request.urlopen(req, timeout=10) as resp:
            info = json.loads(resp.read())
            DISPLAY_W = info["display"]["width"]
            DISPLAY_H = info["display"]["height"]
            print(f"Device: {info.get('model', 'Unknown')} ({DISPLAY_W}x{DISPLAY_H})")
    except Exception as e:
        print(f"Could not fetch device info ({e}). Using default {DISPLAY_W}x{DISPLAY_H}.")


def send_bitmap(raw_bytes, refresh="full"):
    """POST raw 1-bit framebuffer to /canvas/bitmap."""
    req = urllib.request.Request(
        f"http://{DISPLAY_IP}/canvas/bitmap?refresh={refresh}",
        data=raw_bytes,
        headers={"Content-Type": "application/octet-stream"},
    )
    with urllib.request.urlopen(req, timeout=30) as resp:
        return json.loads(resp.read())


def prepare_framebuffer(path, fit="fit", background="white"):
    """Load image, resize to display, dither to 1-bit, return packed bytes.

    fit:
        "fit"     — preserve aspect, letterbox with `background` (default)
        "fill"    — preserve aspect, crop to fill the display
        "stretch" — ignore aspect, stretch to display dimensions
    """
    img = Image.open(path)

    # Honor EXIF orientation (phone photos)
    img = ImageOps.exif_transpose(img)

    # Work in 8-bit grayscale before dithering
    img = img.convert("L")

    if fit == "stretch":
        img = img.resize((DISPLAY_W, DISPLAY_H), Image.LANCZOS)
        canvas = img
    elif fit == "fill":
        canvas = ImageOps.fit(img, (DISPLAY_W, DISPLAY_H), method=Image.LANCZOS)
    else:  # "fit"
        img.thumbnail((DISPLAY_W, DISPLAY_H), Image.LANCZOS)
        bg = 255 if background == "white" else 0
        canvas = Image.new("L", (DISPLAY_W, DISPLAY_H), bg)
        offset = ((DISPLAY_W - img.width) // 2, (DISPLAY_H - img.height) // 2)
        canvas.paste(img, offset)

    # Floyd-Steinberg dither to 1-bit
    bw = canvas.convert("1")

    # PIL mode '1' packs MSB-first — same as EPD expects. 1 = white, 0 = black.
    raw = bw.tobytes()
    expected = DISPLAY_W * DISPLAY_H // 8
    if len(raw) == expected:
        return raw

    # Rare: PIL padded row bytes. Repack manually.
    packed = bytearray(expected)
    for i, p in enumerate(bw.getdata()):
        if p:
            packed[i // 8] |= 0x80 >> (i % 8)
    return bytes(packed)


def main():
    parser = argparse.ArgumentParser(description="Upload a local photo to AI Canvas")
    parser.add_argument("image", type=Path, help="Path to image file (JPG, PNG, etc.)")
    parser.add_argument("--ip", type=str, default=None, help="Display IP address")
    parser.add_argument(
        "--fit",
        choices=["fit", "fill", "stretch"],
        default="fit",
        help="fit: letterbox (default). fill: crop to cover. stretch: ignore aspect.",
    )
    parser.add_argument(
        "--background",
        choices=["white", "black"],
        default="white",
        help="Letterbox color when --fit fit (default: white)",
    )
    args = parser.parse_args()

    if not args.image.is_file():
        print(f"File not found: {args.image}", file=sys.stderr)
        sys.exit(1)

    if args.ip:
        global DISPLAY_IP
        DISPLAY_IP = args.ip

    get_device_info()

    print(f"Preparing {args.image.name} ({args.fit})...")
    raw = prepare_framebuffer(args.image, fit=args.fit, background=args.background)

    print(f"Sending {len(raw)} bytes to {DISPLAY_IP}...")
    result = send_bitmap(raw)
    print(f"Done: {result}")


if __name__ == "__main__":
    main()
