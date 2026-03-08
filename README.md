# AInk — AI-First E-Ink Canvas

> *Read the full story: [Introducing the AI-First Display](https://ukielab.com/introducing-the-ai-first-display/)*

## A Device Designed to Be Used by AI

Traditional displays follow the same model — the human controls the screen. You open apps, navigate menus, search for information. Even "AI-powered" devices still require humans to operate the display, with AI constrained inside predefined containers.

**AInk is different.** It's a completely new category of device — an **AI-First Display** — designed from the ground up for AI agents to operate and communicate through.

On an AI-First Display:

- The **AI agent decides** what information is shown
- The **AI decides** when it should appear
- The **interface is generated dynamically**
- The **user simply glances** at the device

Instead of `AI → computer → OS → application → display`, the interaction becomes simply `AI → display`. No operating system, no graphical frameworks, no application layer — just direct communication between AI and the display.

### Self-Describing Hardware

When an AI agent discovers the device, it can immediately read its capabilities and start using it — no drivers, no cables, no manual configuration. The device explains itself:

```
Hello AI.
I am a display device.
Resolution: 800x480
Color: grayscale
Refresh time: 1.2 seconds
Send content using simple layout JSON.
```

### Why E-Ink

E-ink displays behave like digital paper — they consume very little power, retain their image indefinitely, and only update when content changes. This makes them ideal for ambient information surfaces that quietly show what matters. The display becomes a living information board rather than an interactive computer.

---

## What It Does

An ESP32-based persistent HTTP server that turns a 7.5" black-and-white e-paper display (800x480) into a physical canvas for AI agents. Agents discover the device on the local network via mDNS, read its capabilities, and send JSON drawing commands to render anything — dashboards, schedules, weather, artwork, notifications.

**No cloud. No app. No human UI.** The API is the entire product. The human is simply the viewer.

---

## Features

- **17 drawing operations** — lines, rectangles, circles, ellipses, arcs, polygons, text, PNG images, gradients, flood fill, clipping, QR codes, and more
- **Always-on HTTP server** — device stays awake with WiFi active, ready for commands
- **mDNS discovery** — agents find the device automatically via `_aiscreen._tcp`
- **MCP + OpenAPI specs** — machine-readable API docs served from the device itself
- **Named zones** — define regions for partial updates without redrawing the full screen
- **Double-buffered framebuffer** — dirty-rect detection, background EPD refresh
- **OTA updates** — firmware updates from GitHub releases, no USB required
- **Easy setup** — captive portal WiFi configuration, factory reset via triple-press

---

## Hardware

| Item | Notes |
|------|-------|
| ESP32 development board | Standard 38-pin ESP32 DevKit |
| 7.5" B/W e-paper display | 800x480 px, SPI interface |
| Power | USB (always-on, no battery/deep sleep) |

---

## Quick Start

### 1. Flash the firmware

You need [PlatformIO](https://platformio.org/) (VS Code extension or CLI).

```bash
# Clone the repository
git clone https://github.com/BatsIhor/UkieLab-Canvas.git
cd UkieLab-Canvas

# Build and upload firmware
pio run -e aicanvas_esp32_75bw -t upload

# Build and upload the web UI / filesystem
pio run -e aicanvas_esp32_75bw -t buildfs
pio run -e aicanvas_esp32_75bw -t uploadfs
```

### 2. First-time setup

1. Power on the device. It creates a WiFi access point named **UkieLab-AInk**.
2. Connect to **UkieLab-AInk** from your phone or computer. A captive portal opens (or go to `http://192.168.4.1`).
3. Enter your home WiFi SSID and password.
4. The device reboots, connects to WiFi, and displays its IP address on screen.

### 3. Send drawing commands

```bash
# Discover the device
dns-sd -B _aiscreen._tcp

# Draw something
curl -X POST http://<device-ip>/canvas \
  -H "Content-Type: application/json" \
  -d '[
    {"op":"clear"},
    {"op":"rect","x":10,"y":10,"w":780,"h":460,"fill":false},
    {"op":"text","x":400,"y":200,"text":"Hello from AI","font":"sans","size":24,"align":"center"}
  ]'
```

---

## API

| Method | Path | Purpose |
|--------|------|---------|
| GET | `/device` | Device capabilities and API doc links |
| POST | `/canvas` | Execute drawing commands |
| GET | `/canvas` | Current frame command log |
| GET | `/canvas/screenshot` | PBM image of current display |
| POST | `/canvas/clear` | Clear the display |
| POST/GET/DELETE | `/zones` | Manage named display zones |
| GET | `/health` | Uptime, memory, WiFi status |
| POST | `/measure` | Measure text dimensions (no render) |
| POST | `/device/name` | Rename device and update mDNS |
| GET | `/openapi.json` | OpenAPI 3.0 specification |
| GET | `/mcp/tools.json` | MCP tool definitions |

Full API documentation is served by the device itself. Agents should fetch `/mcp/tools.json` or `/openapi.json` before sending commands.

### Drawing Operations

`clear`, `pixel`, `line`, `rect`, `circle`, `ellipse`, `arc`, `polygon`, `polyline`, `text`, `image`, `gradient`, `flood_fill`, `clip`, `unclip`, `raw_bitmap`, `qr`

---

## Architecture

```
src/
  aicanvas_main.cpp        Main app — WiFi, mDNS, web server
  RenderEngine.cpp         JSON command parser, 16 drawing ops
  FramebufferManager.cpp   Double-buffered framebuffer, dirty rects
  TextLayout.cpp           Word wrap, alignment, overflow
  FontRegistry.cpp         Font lookup (family, size, bold)
  ImageDecoder.cpp         PNG decode with dithering
  ZoneRegistry.cpp         Named zones for partial updates
  CommandLog.cpp           Last-frame command log
  ApiHandlers.cpp          HTTP endpoint handlers
  utils/
    OTAUpdateManager.cpp   GitHub release OTA updates
lib/
  EPD_Custom/              Custom e-paper driver (B/W, full + partial refresh)
data/                      SPIFFS filesystem (HTML, CSS, JSON specs)
Settings.cpp               SPIFFS-persisted settings with atomic save
```

---

## Configuration

Settings are stored in SPIFFS (`data/settings.json`):

- `canvas.deviceName` — mDNS hostname (default: `aink`)
- `githubUser` / `githubRepo` — OTA update source (default: `BatsIhor/UkieLab-Canvas`)
- `connection.ssid` / `password` — WiFi credentials (set via captive portal)

### Factory Reset

Triple-press the reset button within 10 seconds to clear WiFi credentials and return to setup mode.

---

## Serial Monitor

```bash
pio device monitor
```

115200 baud. Shows boot, WiFi, mDNS, HTTP requests, render timing, and OTA progress.

---

## License

**UkieLab Non-Commercial License v1.0** — See [LICENSE](LICENSE) for full terms.

**You are free to:**
- View, study, and learn from the source code
- Build and use the device for personal, non-commercial purposes
- Modify the code for personal use
- Share the code non-commercially (with attribution)

**You may NOT (without written permission):**
- Sell devices built with this firmware
- Use the software in commercial products or services
- Commercially distribute the software or modifications

For commercial licensing inquiries, visit [ukielab.com](https://ukielab.com).

This project uses third-party open-source components under their own licenses. See [THIRD_PARTY_LICENSES](THIRD_PARTY_LICENSES) for details.

---

## Use Cases

AInk is built for **glanceable intelligence** — context-aware information surfaced by AI without any human interaction.

**Morning:** Your AI agent combines weather, traffic, and your calendar — *"Rain at 9:30. Leave by 8:15 to arrive on time."*

**Afternoon:** *"Package arriving in 12 minutes. Front door camera ready."*

**Evening:** *"Cold tomorrow. Kids should wear jacket and gloves."*

No apps. No searching. No menus. Just the right information at the right moment.

Since AInk devices are WiFi-enabled, they don't need to be in the same room as your AI. Put one in the office, one in the garage, one in the living room. Your agent discovers them all and displays relevant information on each — schedules, homework, reminders, or whatever context is available.

---

## Vision

AI-First Displays are just the beginning. In the future: AI home terminals, AI kitchen displays, AI office assistants, AI information panels. In all cases, the device is not a computer — it is a **communication surface for AI systems**. Something that AI can easily discover, understand, and use.

> *The interface of the AI is not another app, not a webpage, not a widget. It is the ability for AI to surface the right information at the right moment, presented in a way that requires no searching, no navigation, and no effort from the user.*

---

## Author

Designed and built by **Ihor Bats** — [ukielab.com](https://ukielab.com)
