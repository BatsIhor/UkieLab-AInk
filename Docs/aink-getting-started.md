# AInk: The AI-First E-Ink Display

> *A physical canvas your AI agent can discover, understand, and draw on — with no apps, no OS, and no human in the loop.*

---

## What Is AInk?

AInk is an open-source device that turns a 7.5" e-ink panel and a cheap ESP32 microcontroller into a **network-connected display peripheral for AI agents**.

Unlike a smart TV, tablet, or dashboard, AInk has no apps, no operating system, and no human-facing user interface. The HTTP API **is** the entire product. You — the human — are simply the viewer. Your AI agent is the operator.

When an agent discovers AInk on your local network, it can:

- Query the device's capabilities (resolution, fonts, drawing operations)
- Send a batch of JSON drawing commands to render anything on screen
- Update named regions of the display independently using fast partial refresh
- Read back what is currently displayed as either a command log or a screenshot

The interaction model is deliberately simple:

```
AI → display
```

No operating system. No application layer. No graphical framework. Just the agent and the canvas.

### Why E-Ink?

E-ink displays behave like digital paper. They:

- **Retain their image indefinitely** with zero power draw — unplug the device and it still shows the last thing rendered
- **Only consume power during a refresh** — ideal for ambient, always-visible information surfaces
- **Have no backlight glare** — comfortable to glance at in any lighting

This makes AInk ideal for placing in a room as a persistent, context-aware information board that updates when your agent has something to say, not when you think to check your phone.

### What Can It Show?

Anything an agent can express as 2D drawing commands:

- Morning briefings: weather + calendar + commute time, composed automatically
- Package arrival alerts pushed by a home automation agent
- A family schedule rendered fresh each morning
- Data dashboards — steps, energy use, server health
- Generative artwork created at a scheduled time each day
- QR codes, progress checklists, reminders

Since AInk is WiFi-connected, you can place multiple devices around your home or office. Your agent discovers all of them via mDNS and can choose which display gets which content.

---

## Hardware

You can build one for under $100.

| Component | Notes |
|-----------|-------|
| ESP32 development board | Standard 38-pin ESP32 DevKit |
| 7.5" B/W e-paper display | 800x480 px, SPI interface (Waveshare or equivalent) |
| USB power supply | Always-on; no battery or deep sleep mode |
| Optional: small enclosure | 3D-printed or off-the-shelf |

The ESP32 drives the e-paper panel over SPI. The device stays always-on with WiFi active — there is no deep sleep mode by design, so it is always reachable for agent commands.

---

## Setting It Up

### Step 1 — Install PlatformIO

AInk firmware is built with [PlatformIO](https://platformio.org/). Install it as a VS Code extension or via the CLI:

```bash
pip install platformio
```

### Step 2 — Clone and Flash

```bash
git clone https://github.com/BatsIhor/UkieLab-AInk.git
cd UkieLab-AInk

# Build and upload firmware
pio run -e aicanvas_esp32_75bw -t upload

# Build and upload the web UI and API specs to SPIFFS
pio run -e aicanvas_esp32_75bw -t buildfs
pio run -e aicanvas_esp32_75bw -t uploadfs
```

Both steps are required on first flash. The `buildfs`/`uploadfs` step uploads the web configuration UI, OpenAPI spec, and MCP tool definitions that the device serves from its filesystem.

### Step 3 — Connect to WiFi

1. Power on the device. It creates a WiFi access point named **UkieLab-AInk**.
2. Connect to that network from your phone or computer. A captive portal opens automatically (or navigate to `http://192.168.4.1`).
3. Enter your home WiFi credentials.
4. The device reboots, connects to your network, and shows its IP address directly on the e-ink screen.

That's it. The display is now live on your local network.

### Factory Reset

Triple-press the reset button within 10 seconds. This clears saved credentials and returns the device to setup mode. The display will show "Press RESET to begin setup" before going into AP mode.

### Serial Monitor (Optional)

To watch boot logs, WiFi connection status, HTTP requests, and render timing:

```bash
pio device monitor
```

Runs at 115200 baud.

### OTA Updates

Once on your network, firmware updates can be pulled from GitHub releases without a USB cable:

- Navigate to the device's IP in a browser
- Use the `/update` page to trigger an OTA update

---

## Using the API

AInk's API is designed to be consumed by AI agents, but you can also drive it directly with `curl` or any HTTP client.

### Discover the Device

**Via mDNS** (Bonjour/Zeroconf):
```bash
dns-sd -B _aiscreen._tcp      # macOS
avahi-browse _aiscreen._tcp   # Linux
```

The device advertises itself as `_aiscreen._tcp` with TXT records that include resolution, color depth, and API endpoint paths — so an agent can read basic capabilities before making a single HTTP request.

**By IP** (shown on the display after boot):
```
http://<device-ip>/
```

### Read Device Capabilities

Always start here. Returns everything the agent needs to know: display dimensions, available fonts, supported drawing operations, and links to the OpenAPI and MCP specs.

```bash
curl http://<device-ip>/device
```

```json
{
  "name": "aink",
  "display": {
    "width": 800,
    "height": 480,
    "depth": 1
  },
  "fonts": [
    { "name": "sans", "sizes": [9, 12, 18, 24, 32] },
    { "name": "serif", "sizes": [12, 18, 24] },
    { "name": "mono", "sizes": [9, 12, 18, 24] }
  ],
  "operations": [
    "clear", "pixel", "line", "rect", "circle", "ellipse",
    "arc", "polygon", "polyline", "text", "image",
    "gradient", "flood_fill", "clip", "unclip", "raw_bitmap", "qr"
  ],
  "endpoints": {
    "openapi": "/openapi.json",
    "mcp_tools": "/mcp/tools.json"
  }
}
```

### Render Something

Send a `POST` to `/canvas` with a JSON array of drawing commands. Commands execute in order on the framebuffer; the display refreshes once at the end.

```bash
curl -X POST http://<device-ip>/canvas \
  -H "Content-Type: application/json" \
  -d '[
    {"op": "clear", "color": "white"},
    {"op": "rect", "x": 0, "y": 0, "w": 800, "h": 60, "color": "black", "fill": true},
    {"op": "text", "x": 400, "y": 10, "text": "Hello from AInk", "font": "sans", "size": 24, "bold": true, "color": "white", "align": "center"},
    {"op": "text", "x": 400, "y": 200, "text": "Controlled by AI, viewed by you.", "font": "sans", "size": 18, "align": "center"},
    {"op": "rect", "x": 20, "y": 20, "w": 760, "h": 440, "color": "black", "fill": false}
  ]'
```

The response tells you what was rendered and flags any adjustments made (e.g. text truncated, coordinates clamped):

```json
{
  "status": "rendered",
  "commands_executed": 5,
  "render_time_ms": 120,
  "warnings": []
}
```

### All Drawing Operations

| Operation | What It Does |
|-----------|-------------|
| `clear` | Fill the screen or zone with a color |
| `pixel` | Set a single pixel |
| `line` | Draw a line between two points |
| `rect` | Rectangle, optionally filled, with optional rounded corners |
| `circle` | Circle from center + radius |
| `ellipse` | Ellipse with independent x/y radii |
| `arc` | Arc segment of a circle |
| `polygon` | Closed polygon from a vertex list |
| `polyline` | Open polyline (not closed) |
| `text` | Text with font, size, alignment, wrapping, and overflow control |
| `image` | Decode and draw a PNG (with Floyd-Steinberg, ordered, or threshold dithering) |
| `gradient` | Linear gradient fill within a rectangle |
| `flood_fill` | Flood-fill a contiguous area from a seed point |
| `clip` | Set a clipping region for subsequent operations |
| `unclip` | Remove the clipping region |
| `raw_bitmap` | Write a raw pixel buffer directly to the framebuffer |
| `qr` | Generate and render a QR code |

### Text Rendering

Text is the most-used operation. Key parameters:

```json
{
  "op": "text",
  "x": 400,
  "y": 100,
  "text": "Good morning. Rain starts at 9:30.",
  "font": "sans",
  "size": 18,
  "bold": true,
  "color": "black",
  "align": "center",
  "wrap": 700,
  "max_lines": 3,
  "overflow": "truncate"
}
```

- `align`: `"left"` (default), `"center"`, `"right"`
- `wrap`: pixel width at which word wrapping kicks in
- `overflow`: `"truncate"` (ellipsis), `"shrink"` (reduce font size to fit), `"clip"` (hard cut)
- `\n` in the `text` field inserts explicit line breaks

### Zones (Partial Updates)

Zones let you define named rectangular regions and update them independently using fast partial refresh (~0.5s) instead of a full screen refresh (~2–3s).

**Define zones:**
```bash
curl -X POST http://<device-ip>/zones \
  -H "Content-Type: application/json" \
  -d '{
    "zones": [
      {"id": "header", "x": 0,   "y": 0,   "w": 800, "h": 60},
      {"id": "main",   "x": 0,   "y": 60,  "w": 800, "h": 360},
      {"id": "footer", "x": 0,   "y": 420, "w": 800, "h": 60}
    ]
  }'
```

**Update just the footer:**
```bash
curl -X POST http://<device-ip>/canvas \
  -H "Content-Type: application/json" \
  -d '{
    "zone": "footer",
    "commands": [
      {"op": "clear", "color": "white"},
      {"op": "text", "x": 400, "y": 20, "text": "Updated: 2:34 PM", "font": "sans", "size": 14, "align": "center"}
    ]
  }'
```

### Other Useful Endpoints

| Method | Path | Purpose |
|--------|------|---------|
| `GET` | `/device` | Device capabilities, fonts, operations |
| `POST` | `/canvas` | Render drawing commands |
| `GET` | `/canvas` | Command log for the current frame |
| `GET` | `/canvas/screenshot` | PBM image of what's currently on screen |
| `POST` | `/canvas/clear` | Clear the display to white |
| `POST/GET/DELETE` | `/zones` | Manage named zones |
| `GET` | `/health` | Uptime, free memory, WiFi signal |
| `POST` | `/measure` | Measure text dimensions without rendering |
| `POST` | `/device/name` | Rename the device and update mDNS |
| `GET` | `/openapi.json` | Full OpenAPI 3.0 specification |
| `GET` | `/mcp/tools.json` | MCP tool definitions for agent frameworks |

---

## Using AInk with AI Agents

### Python Helper

Copy this pattern into your agent code to rate-limit renders and handle errors gracefully:

```python
import json, urllib.request, time

DISPLAY_IP = "192.168.1.x"  # replace with your device's IP
_last_render = 0

def render(commands):
    global _last_render
    # E-ink needs ~3s between refreshes
    wait = 3.0 - (time.time() - _last_render)
    if wait > 0:
        time.sleep(wait)
    data = json.dumps(commands).encode()
    req = urllib.request.Request(
        f"http://{DISPLAY_IP}/canvas", data=data,
        headers={"Content-Type": "application/json"}
    )
    try:
        with urllib.request.urlopen(req, timeout=10) as resp:
            _last_render = time.time()
            return json.loads(resp.read())
    except Exception as e:
        print(f"Display error: {e}")
        return None
```

### Via MCP (Model Context Protocol)

AInk serves a ready-to-use MCP tool definition at `/mcp/tools.json`. Any MCP-compatible agent framework can:

1. Discover the device via mDNS (`_aiscreen._tcp`)
2. Fetch `/mcp/tools.json`
3. Register `display_render` and `display_info` as available tools

No manual configuration required. The device self-describes.

### Via OpenAPI

For frameworks that use OpenAPI for tool generation (Claude tool use, GPT-4 function calling, etc.), fetch `/openapi.json` from the device. It is always in sync with the firmware.

---

## Graceful Error Handling

AInk never refuses to render. If something is wrong with a command, it renders the best approximation and reports what it did in the `warnings` array:

| Condition | Device Behavior | Warning Reported |
|-----------|----------------|-----------------|
| Coordinates out of bounds | Clamp to screen edges | `coords_clamped` |
| Text too long for area | Wrap, shrink, or truncate | `text_overflow` |
| Font not found | Fall back to `sans` | `font_fallback` |
| Font size not available | Use nearest size | `size_adjusted` |
| Unknown operation | Skip and continue | `unknown_op` |
| Image decode failure | Render placeholder `X` rect | `image_decode_failed` |

Only malformed JSON (400), oversized payload (413), and internal failures (500) return HTTP errors. Everything else renders with best-effort.

---

## Configuration

Settings are stored in SPIFFS (`data/settings.json`):

| Key | Default | Purpose |
|-----|---------|---------|
| `canvas.deviceName` | `aink` | mDNS hostname (also shown on startup screen) |
| `githubUser` | `BatsIhor` | OTA update source user |
| `githubRepo` | `UkieLab-AInk` | OTA update source repo |
| `connection.ssid` | *(set via portal)* | WiFi network |
| `connection.password` | *(set via portal)* | WiFi password |

---

## Example: Morning Briefing Layout

```python
render([
    # White background
    {"op": "clear", "color": "white"},

    # Header bar
    {"op": "rect", "x": 0, "y": 0, "w": 800, "h": 55, "color": "black", "fill": True},
    {"op": "text", "x": 20, "y": 10, "text": "MORNING BRIEFING", "font": "sans",
     "size": 18, "bold": True, "color": "white"},
    {"op": "text", "x": 780, "y": 14, "text": "Thursday, Mar 12", "font": "sans",
     "size": 12, "color": "white", "align": "right"},

    # Weather
    {"op": "text", "x": 30, "y": 75, "text": "Weather", "font": "sans", "size": 12, "color": "black"},
    {"op": "text", "x": 30, "y": 95, "text": "42F  Rain starting at 9:30", "font": "sans", "size": 18},

    # Commute
    {"op": "text", "x": 30, "y": 140, "text": "Commute", "font": "sans", "size": 12},
    {"op": "text", "x": 30, "y": 160, "text": "Leave by 8:15 to arrive on time.", "font": "sans", "size": 18},

    # Calendar
    {"op": "line", "x1": 20, "y1": 210, "x2": 780, "y2": 210, "color": "black"},
    {"op": "text", "x": 30, "y": 220, "text": "Today", "font": "sans", "size": 12},
    {"op": "text", "x": 30, "y": 240, "text": "9:00   Team standup", "font": "mono", "size": 14},
    {"op": "text", "x": 30, "y": 262, "text": "10:30  Design review", "font": "mono", "size": 14},
    {"op": "text", "x": 30, "y": 284, "text": "14:00  1:1 with manager", "font": "mono", "size": 14},

    # Footer
    {"op": "rect", "x": 0, "y": 455, "w": 800, "h": 25, "color": "black", "fill": True},
    {"op": "text", "x": 400, "y": 460, "text": "aink | No apps. No searching. Just information.",
     "font": "mono", "size": 9, "color": "white", "align": "center"},
])
```

---

## License

**UkieLab Non-Commercial License v1.0**

You are free to build, use, study, and modify AInk for personal, non-commercial purposes. You may not sell devices built with this firmware or use it in commercial products without written permission.

See [LICENSE](../LICENSE) for full terms. For commercial licensing, contact [ukielab.com](https://ukielab.com).

---

*Designed and built by **Ihor Bats** — [ukielab.com](https://ukielab.com)*
*Source: [github.com/BatsIhor/UkieLab-AInk](https://github.com/BatsIhor/UkieLab-AInk)*
