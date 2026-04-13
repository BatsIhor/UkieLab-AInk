#!/usr/bin/env python3
"""
Meme Display for AI Canvas E-Ink
Fetches a random meme from Reddit and displays it on the e-ink screen.
Runs every 15 minutes by default.

Usage:
    python3 tools/meme_display.py                    # one-shot
    python3 tools/meme_display.py --loop              # every 15 min
    python3 tools/meme_display.py --loop --interval 5 # every 5 min
    python3 tools/meme_display.py --ip 192.168.87.42  # custom IP
"""

import urllib.request
import json
import time
import io
import argparse
from PIL import Image

DISPLAY_IP = "192.168.87.61"
MEME_API = "https://meme-api.com/gimme"
HEADERS = {"User-Agent": "AInk-Meme-Bot/1.0"}
MAX_RETRIES = 10
DISPLAY_W = 800
DISPLAY_H = 480


def send_commands(commands, refresh="full"):
    """Send JSON drawing commands to the display."""
    payload = json.dumps({"commands": commands, "refresh": refresh}).encode()
    req = urllib.request.Request(
        f"http://{DISPLAY_IP}/canvas",
        data=payload,
        headers={"Content-Type": "application/json"},
    )
    with urllib.request.urlopen(req, timeout=30) as resp:
        return json.loads(resp.read())


def send_image(png_bytes, x=0, y=0, w=0, h=0, dither="ordered", refresh="full"):
    """Send a PNG image directly via binary upload endpoint (no base64 overhead)."""
    params = f"?x={x}&y={y}&dither={dither}&refresh={refresh}"
    if w > 0:
        params += f"&w={w}"
    if h > 0:
        params += f"&h={h}"
    req = urllib.request.Request(
        f"http://{DISPLAY_IP}/canvas/image{params}",
        data=png_bytes,
        headers={"Content-Type": "image/png"},
    )
    with urllib.request.urlopen(req, timeout=60) as resp:
        return json.loads(resp.read())


def send_bitmap(raw_bytes, refresh="full"):
    """Send raw 1-bit framebuffer data directly (zero overhead on ESP32).

    raw_bytes must be exactly DISPLAY_W * DISPLAY_H / 8 bytes, packed MSB-first.
    This bypasses all decoding on the device — just memcpy into framebuffer.
    """
    params = f"?refresh={refresh}"
    req = urllib.request.Request(
        f"http://{DISPLAY_IP}/canvas/bitmap{params}",
        data=raw_bytes,
        headers={"Content-Type": "application/octet-stream"},
    )
    with urllib.request.urlopen(req, timeout=30) as resp:
        return json.loads(resp.read())


def fetch_meme():
    """Fetch a random SFW meme from the API."""
    for attempt in range(MAX_RETRIES):
        req = urllib.request.Request(MEME_API, headers=HEADERS)
        resp = urllib.request.urlopen(req, timeout=10)
        meme = json.loads(resp.read())
        if not meme.get("nsfw", False):
            return meme
        print(f"  Skipping NSFW meme, retry {attempt + 1}...")
    raise RuntimeError("Could not find a SFW meme after retries")


def image_to_framebuffer(img_bw, fb_w=DISPLAY_W, fb_h=DISPLAY_H, paste_x=0, paste_y=0):
    """Composite a 1-bit PIL image onto a white framebuffer and return raw packed bytes.

    Returns exactly fb_w * fb_h / 8 bytes, MSB-first (matching EPD format).
    """
    # Create full-screen white canvas and paste the image at the given position
    canvas = Image.new("1", (fb_w, fb_h), 1)  # white
    canvas.paste(img_bw, (paste_x, paste_y))

    # Pack to raw bytes — PIL mode '1' tobytes() gives packed MSB-first bytes
    raw = canvas.tobytes()
    expected = fb_w * fb_h // 8
    if len(raw) != expected:
        # PIL sometimes pads rows — manually pack if needed
        pixels = list(canvas.getdata())
        packed = bytearray(expected)
        for i, p in enumerate(pixels):
            if p:
                packed[i // 8] |= 0x80 >> (i % 8)
        return bytes(packed)
    return raw


def resize_for_display(image_data, max_w, max_h):
    """Resize image to fit display area, dither to 1-bit.

    Dithering is done on the Python side (Floyd-Steinberg).
    Returns the 1-bit PIL image and its dimensions.
    """
    img = Image.open(io.BytesIO(image_data)).convert("L")

    ratio = min(max_w / img.width, max_h / img.height, 1.0)
    if ratio < 1:
        img = img.resize(
            (int(img.width * ratio), int(img.height * ratio)), Image.LANCZOS
        )

    # Floyd-Steinberg dithering to 1-bit — done here, not on ESP32
    img_bw = img.convert("1")
    return img_bw, img_bw.width, img_bw.height


def sanitize_title(title, max_len=70):
    """Keep only ASCII 0x20-0x7E."""
    clean = "".join(c if 0x20 <= ord(c) <= 0x7E else "?" for c in title)
    if len(clean) > max_len:
        clean = clean[: max_len - 3] + "..."
    return clean


def show_meme():
    """Full pipeline: fetch, resize, display.

    Uses two-step rendering:
    1. JSON commands for header/footer text (no refresh)
    2. Raw bitmap for the full framebuffer with meme image (with refresh)

    The bitmap approach sends all 48000 bytes directly — zero decoding on
    the ESP32, no PNG overhead, works reliably at any image size.
    """
    print("Fetching random meme...")
    meme = fetch_meme()
    title = sanitize_title(meme["title"])
    sub = f'r/{meme["subreddit"]}'
    print(f"  {title} ({sub})")

    print("Downloading image...")
    img_req = urllib.request.Request(meme["url"], headers=HEADERS)
    img_data = urllib.request.urlopen(img_req, timeout=20).read()
    print(f"  {len(img_data)} bytes")

    # Leave room for header (44px) and footer (24px)
    content_y = 50
    content_h = DISPLAY_H - content_y - 30
    max_img_w = DISPLAY_W - 40
    max_img_h = content_h - 10

    print("Resizing for e-ink...")
    img_bw, img_w, img_h = resize_for_display(img_data, max_img_w, max_img_h)
    print(f"  {img_w}x{img_h}")

    # Center image in content area
    img_x = (DISPLAY_W - img_w) // 2
    img_y = content_y + (content_h - img_h) // 2

    # Step 1: Draw header and footer via JSON commands (no refresh)
    print("Rendering header/footer...")
    send_commands(
        [
            {"op": "clear", "color": "white"},
            {"op": "rect", "x": 0, "y": 0, "w": 800, "h": 44, "color": "black", "fill": True},
            {
                "op": "text", "x": 20, "y": 5, "text": title,
                "font": "sans", "size": 12, "bold": True, "color": "white",
                "maxWidth": 760, "overflow": "truncate",
            },
            {"op": "rect", "x": 0, "y": 456, "w": 800, "h": 24, "color": "black", "fill": True},
            {
                "op": "text", "x": 10, "y": 457, "text": sub,
                "font": "mono", "size": 9, "bold": True, "color": "white",
            },
            {
                "op": "text", "x": 790, "y": 457, "text": "aink meme machine",
                "font": "mono", "size": 9, "bold": True, "color": "white", "align": "right",
            },
        ],
        refresh="none",
    )

    time.sleep(0.5)

    # Step 2: Compose meme image into framebuffer and send as raw bitmap.
    # We read back the current framebuffer state (with header/footer already
    # rendered) via screenshot, composite the image, and push the full buffer.
    # This ensures header/footer text + image are all in one consistent frame.
    print("Compositing image into framebuffer...")
    raw_fb = image_to_framebuffer(img_bw, paste_x=img_x, paste_y=img_y)

    # But we need the header/footer too — they're already in the device's back buffer
    # from step 1. So instead, send the image via the PNG endpoint for small images,
    # or use bitmap for the meme portion overlaid on the existing buffer.
    # Simplest: just send the image as raw bitmap after the text commands.
    # The text is already in the back buffer. We just need to blit the meme pixels
    # into the right region. Use the PNG endpoint for small images.

    # Actually — let's just read back the framebuffer, composite, and send full bitmap.
    # Fetch current screenshot as PBM
    print("Reading back current framebuffer...")
    try:
        screenshot_req = urllib.request.Request(
            f"http://{DISPLAY_IP}/canvas/screenshot",
            headers=HEADERS,
        )
        screenshot_data = urllib.request.urlopen(screenshot_req, timeout=10).read()
        # PBM P4 format: "P4\n800 480\n" followed by raw packed bits
        # Parse header to find start of pixel data
        header_end = screenshot_data.index(b'\n', screenshot_data.index(b'\n') + 1) + 1
        fb_raw = screenshot_data[header_end:]
        # PBM: 1 = black, 0 = white. EPD: 1 = white, 0 = black. Invert.
        canvas = Image.new("1", (DISPLAY_W, DISPLAY_H))
        fb_bytes = bytearray(DISPLAY_W * DISPLAY_H // 8)
        for i in range(min(len(fb_raw), len(fb_bytes))):
            fb_bytes[i] = fb_raw[i] ^ 0xFF  # invert PBM to EPD convention
        canvas = Image.frombytes("1", (DISPLAY_W, DISPLAY_H), bytes(fb_bytes))
    except Exception as e:
        print(f"  Screenshot failed ({e}), using white canvas")
        canvas = Image.new("1", (DISPLAY_W, DISPLAY_H), 1)

    # Paste meme image onto the canvas
    canvas.paste(img_bw, (img_x, img_y))

    # Pack to raw EPD format and send
    raw_fb = canvas.tobytes()
    expected = DISPLAY_W * DISPLAY_H // 8
    if len(raw_fb) != expected:
        pixels = list(canvas.getdata())
        packed = bytearray(expected)
        for i, p in enumerate(pixels):
            if p:
                packed[i // 8] |= 0x80 >> (i % 8)
        raw_fb = bytes(packed)

    print(f"Sending full framebuffer ({len(raw_fb)} bytes)...")
    result = send_bitmap(raw_fb, refresh="full")
    print(f"  Done!")
    return True


def main():
    parser = argparse.ArgumentParser(description="Meme Display for AI Canvas")
    parser.add_argument("--loop", action="store_true", help="Run continuously")
    parser.add_argument(
        "--interval", type=int, default=15, help="Minutes between memes (default: 15)"
    )
    parser.add_argument("--ip", type=str, default=None, help="Display IP address")
    args = parser.parse_args()

    if args.ip:
        global DISPLAY_IP
        DISPLAY_IP = args.ip

    if args.loop:
        print(f"Meme machine started! New meme every {args.interval} minutes.")
        print(f"Display: {DISPLAY_IP}")
        print("-" * 50)
        while True:
            try:
                show_meme()
            except Exception as e:
                print(f"Error: {e}")
            print(f"\nNext meme in {args.interval} minutes...\n")
            time.sleep(args.interval * 60)
    else:
        try:
            show_meme()
        except Exception as e:
            print(f"Error: {e}")


if __name__ == "__main__":
    main()
