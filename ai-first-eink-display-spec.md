# AI-FIRST E-INK DISPLAY

Device Specification & Developer Guide

*A Physical Display Peripheral for AI Agents*

ESP32 + 10.2" E-Ink | Network-Discoverable | Zero-Configuration

Version 1.0 — February 2026

DRAFT

# 1. Introduction

## 1.1 Purpose

This document specifies the design, architecture, and API for an AI-First E-Ink Display — a new category of physical device purpose-built for AI agents. Unlike traditional displays that have a human-facing UI with an API bolted on, this device has no human user interface at all. The API is the entire product. The human is simply the viewer of content that AI agents compose and render.

The device consists of an ESP32 microcontroller connected to a 10.2-inch e-ink screen, discoverable on the local network via mDNS. AI agents discover it, query its capabilities, and send drawing commands to display any information a user has requested — dashboards, schedules, weather, artwork, notifications, or anything else an agent can render.

## 1.2 What Problem This Solves

AI agents today are powerful but have no physical presence. They can generate information, make decisions, and take actions, but their outputs exist only on screens the user is already looking at. This creates a fundamental limitation: AI-generated information competes for attention with every other app, tab, and notification on the user’s existing devices.

An always-on e-ink display solves this by giving agents a dedicated, persistent, ambient surface. Information stays visible without power draw, without competing for screen real estate, and without requiring the user to check a specific app. The display becomes the agent’s physical canvas in the user’s environment.

## 1.3 Design Philosophy: Agent-First

The agent-first philosophy is the single most important concept in this specification. Every design decision flows from one question: what would make this device maximally useful and easy for an AI agent to operate? This inverts the traditional approach to device design in several fundamental ways.

> **Key Principle**
>
> The device has no human UI. The API is the product. The human is the viewer, not the operator.

### 1.3.1 The Device Must Be Self-Describing

An AI agent encountering this device for the first time must be able to query it and immediately understand everything it can do. Screen dimensions, color depth, available fonts, supported drawing operations, refresh capabilities, and current state — all must be programmatically discoverable. The device is its own documentation. There should be zero need for an agent to consult external references or be pre-programmed with device knowledge.

### 1.3.2 Single-Request Rendering

AI agents work best when they can compose a complete intent in a single request. E-ink reinforces this constraint — each refresh takes seconds, making rapid sequential draw calls impractical. Therefore, the API is designed around composing an entire frame as a batch of drawing commands submitted atomically. The agent describes everything it wants on screen in one POST request, and the device renders and refreshes once.

### 1.3.3 Forgiving and Graceful

Agents will make mistakes. Text will sometimes be too long for the specified area. Coordinates may exceed screen bounds. A requested font might not be available. Rather than returning errors and forcing the agent to retry, the device should handle these situations gracefully: wrap or truncate overflowing text, clamp out-of-bounds coordinates, fall back to a similar available font. The device always tries to render something reasonable and reports what it actually did in the response, including any adjustments it made.

### 1.3.4 Queryable State

The agent must be able to ask the device what is currently displayed. This enables intelligent partial updates, error recovery, and multi-agent coordination. The device maintains a command log for the current frame and can export the current framebuffer as an image that multimodal agents can visually inspect.

### 1.3.5 Machine-Readable Everything

Every aspect of the device is designed for machine consumption. The device serves an OpenAPI specification describing its full API, and an MCP tool definition that compatible agent frameworks can consume directly. JSON is the universal data format. Human-readable documentation is a courtesy, not a requirement — the structured data is authoritative.

# 2. Hardware Specification

## 2.1 Microcontroller

The recommended microcontroller is the ESP32-S3 with PSRAM. The S3 variant provides sufficient processing power for JSON parsing, image decoding, and framebuffer manipulation, while PSRAM is essential for holding the full framebuffer in memory.

|               |                                                   |
|---------------|---------------------------------------------------|
| **Component** | **Specification**                                 |
| MCU           | ESP32-S3 (dual-core Xtensa LX7, 240 MHz)          |
| RAM           | 512 KB SRAM + 8 MB PSRAM (required)               |
| Flash         | 16 MB (recommended) for firmware + fonts + assets |
| Connectivity  | Wi-Fi 802.11 b/g/n (2.4 GHz)                      |
| Interface     | SPI (for e-ink display communication)             |

Recommended boards include the ESP32-S3-DevKitC-1 (N16R8 variant with 16MB flash and 8MB PSRAM), Waveshare ESP32-S3-DEV-KIT-N8R8, or equivalent modules with sufficient PSRAM.

## 2.2 Display

|               |                                                              |
|---------------|--------------------------------------------------------------|
| **Parameter** | **Specification**                                            |
| Size          | 10.2 inches diagonal                                         |
| Resolution    | 960 x 680 pixels (recommended)                               |
| Color Depth   | 4-bit grayscale (16 levels), black/white mode supported      |
| Refresh Time  | Full: ~2.5s | Partial: ~0.5s | Fast: ~0.2s (with ghosting) |
| Interface     | SPI with additional control pins (DC, RST, BUSY)             |
| Power         | Zero power to maintain image; power only during refresh      |

Recommended panels include Waveshare 10.2" and Good Display GDEP102TC2 or equivalent. The 4-bit grayscale mode is the recommended default as it provides sufficient dynamic range for charts, images, and text without the 15+ second refresh times of tri-color panels.

## 2.3 Memory Budget

The primary memory constraint is the framebuffer. At 960 x 680 resolution with 4-bit (16-level grayscale) color depth, the framebuffer requires approximately 326 KB (960 x 680 / 2 bytes). A double-buffered implementation for diff-based partial refresh requires approximately 652 KB. Both fit comfortably within the 8 MB PSRAM. Remaining PSRAM is available for JSON parsing buffers, image decoding workspace, font caching, and the HTTP server stack.

## 2.4 Power

E-ink displays consume power only during refresh, making this device ideal for battery operation. In a USB-powered desk configuration, the ESP32 can remain in light sleep between requests, waking on network activity. For battery configurations, the ESP32 can deep-sleep and wake on a timer or external trigger, though this sacrifices network discoverability during sleep. The recommended configuration for agent use is USB-C power with the ESP32 in light sleep, maintaining Wi-Fi connectivity for instant responsiveness.

# 3. Network Discovery & Connectivity

## 3.1 mDNS Service Advertisement

The device advertises itself on the local network using mDNS (multicast DNS), also known as Bonjour or Zeroconf. This enables zero-configuration discovery: any agent on the same network can find the device without knowing its IP address.

Service Type

The device advertises as service type \_aiscreen.\_tcp. This is a custom service type specific to AI-first display devices. The device also advertises the standard \_http.\_tcp service for generic HTTP discovery.

TXT Records

The mDNS advertisement includes TXT records that allow agents to identify and filter devices before connecting:

> Service: \_aiscreen.\_tcp.local.
>
> Instance: eink-office-01.\_aiscreen.\_tcp.local.
>
> Host: eink-office-01.local.
>
> Port: 80
>
> TXT Records:
>
> model = ai-display-10.2
>
> version = 1.0
>
> width = 960
>
> height = 680
>
> depth = 4
>
> api = /openapi.json

Agents discover devices by querying for \_aiscreen.\_tcp.local and can immediately determine display resolution and API endpoint from TXT records alone, before making any HTTP requests.

## 3.2 HTTP Server

The device runs an asynchronous HTTP server on port 80. All API endpoints accept and return JSON (Content-Type: application/json) unless otherwise specified. The server supports concurrent connections to handle simultaneous queries from multiple agents, though rendering is serialized (only one frame can be rendered at a time).

## 3.3 Network Configuration

On first boot with no saved credentials, the device starts a Wi-Fi Access Point (AP) for configuration. The user connects to this AP and provides Wi-Fi credentials via a simple captive portal. Credentials are stored in non-volatile storage. Subsequent boots connect automatically to the saved network. A physical reset button clears saved credentials and returns to AP mode.

# 4. API Specification

This section defines the complete HTTP API. All endpoints use JSON for request and response bodies. The device serves a machine-readable OpenAPI 3.0 specification at /openapi.json that agents can ingest for automated tool generation.

## 4.1 Device Information

GET /device — Returns complete device capabilities. This is the first endpoint an agent should call after discovery. The response contains everything needed to construct valid rendering commands.

Response Schema

> {
>
> "name": "eink-office-01",
>
> "type": "ai-display",
>
> "firmware_version": "1.0.0",
>
> "api_version": "1.0",
>
> "display": {
>
> "width": 960,
>
> "height": 680,
>
> "color_depth": 4,
>
> "colors": \["black", "white", "gray1", "gray2", ... "gray15"\],
>
> "refresh_modes": {
>
> "full": { "duration_ms": 2500, "ghosting": "none" },
>
> "partial": { "duration_ms": 500, "ghosting": "minimal" },
>
> "fast": { "duration_ms": 200, "ghosting": "moderate" }
>
> }
>
> },
>
> "fonts": \[
>
> {
>
> "name": "sans",
>
> "sizes": \[12, 16, 20, 24, 32, 48, 64, 96\],
>
> "styles": \["regular", "bold", "italic"\]
>
> },
>
> { "name": "serif", "sizes": \[12, 16, 20, 24, 32, 48, 64\] },
>
> { "name": "mono", "sizes": \[12, 16, 20, 24, 32\] }
>
> \],
>
> "icons": \["weather_sun", "weather_cloud", "weather_rain", ...\],
>
> "operations": \[
>
> "clear", "pixel", "line", "rect", "circle", "ellipse",
>
> "arc", "polygon", "polyline", "text", "image",
>
> "gradient", "flood_fill", "clip", "unclip", "raw_bitmap"
>
> \],
>
> "max_payload_bytes": 2097152,
>
> "endpoints": {
>
> "openapi": "/openapi.json",
>
> "mcp_tools": "/mcp/tools.json"
>
> }
>
> }

## 4.2 Canvas Render

POST /canvas — The primary rendering endpoint. Accepts a batch of drawing commands that are executed in order on the framebuffer, then refreshes the display. This is the most important endpoint in the API.

Request Schema

> {
>
> "commands": \[
>
> { "op": "clear", "color": "white" },
>
> { "op": "rect", "x": 0, "y": 0, "w": 960, "h": 80,
>
> "fill": "black" },
>
> { "op": "text", "x": 480, "y": 50,
>
> "text": "Dashboard Title",
>
> "font": "sans", "size": 32, "color": "white",
>
> "align": "center", "baseline": "middle" },
>
> { "op": "line", "x1": 20, "y1": 90, "x2": 940, "y2": 90,
>
> "color": "gray8", "width": 2 },
>
> { "op": "image", "x": 700, "y": 110, "w": 200, "h": 200,
>
> "data": "\<base64 encoded png\>",
>
> "dither": "floyd_steinberg" }
>
> \],
>
> "refresh": "full",
>
> "zone": null
>
> }

Response Schema

> {
>
> "status": "rendered",
>
> "frame_id": "f_a3c8e1b2",
>
> "render_time_ms": 145,
>
> "refresh_time_ms": 2400,
>
> "commands_executed": 5,
>
> "warnings": \[
>
> {
>
> "command_index": 4,
>
> "type": "image_resized",
>
> "message": "Image resized from 512x512 to 200x200"
>
> }
>
> \]
>
> }
>
> **Graceful Error Handling**
>
> The device never fails silently but also never refuses to render. If a command has issues, it renders the best approximation and reports what it did in the warnings array. Agents can inspect warnings to improve future requests.

## 4.3 Drawing Operations Reference

The following table lists all supported drawing operations. Every operation the display driver supports is exposed through the API, giving agents complete control over every pixel on the screen.

|               |                                                                                                   |                                                       |
|---------------|---------------------------------------------------------------------------------------------------|-------------------------------------------------------|
| **Operation** | **Parameters**                                                                                    | **Description**                                       |
| clear         | color                                                                                             | Fill entire screen or zone with color                 |
| pixel         | x, y, color                                                                                       | Set a single pixel                                    |
| line          | x1, y1, x2, y2, color, width, dash                                                                | Draw a line between two points                        |
| rect          | x, y, w, h, fill, stroke, stroke_width, radius                                                    | Rectangle with optional fill, stroke, rounded corners |
| circle        | cx, cy, r, fill, stroke, stroke_width                                                             | Circle from center point and radius                   |
| ellipse       | cx, cy, rx, ry, fill, stroke                                                                      | Ellipse with independent x/y radii                    |
| arc           | cx, cy, r, start_angle, end_angle, stroke                                                         | Arc segment of a circle                               |
| polygon       | points\[\], fill, stroke                                                                          | Closed polygon from vertex list                       |
| polyline      | points\[\], color, width                                                                          | Open polyline (not closed)                            |
| text          | x, y, text, font, size, color, align, baseline, wrap, line_height, max_width, max_lines, overflow | Render text with full layout control                  |
| image         | x, y, w, h, data (base64), format, dither                                                         | Decode and draw an image with optional dithering      |
| gradient      | x, y, w, h, from_color, to_color, direction                                                       | Linear gradient fill within a rectangle               |
| flood_fill    | x, y, color                                                                                       | Fill contiguous area from seed point                  |
| clip          | x, y, w, h                                                                                        | Set clipping region for subsequent ops                |
| unclip        | (none)                                                                                            | Remove clipping region                                |
| raw_bitmap    | x, y, w, h, data (raw pixel buffer)                                                               | Direct framebuffer write for pre-rendered content     |

## 4.4 Text Rendering Details

Text is the most common operation agents will use, so it receives special attention. The text operation supports full layout control to handle the wide variety of content agents produce.

Alignment & Baseline

The align parameter controls horizontal positioning relative to the x coordinate: left (default), center, or right. The baseline parameter controls vertical positioning relative to the y coordinate: top, middle, or bottom (default is top).

Text Wrapping & Overflow

When wrap is specified (as a pixel width), text automatically wraps at word boundaries to fit the given width. The max_lines parameter limits the number of rendered lines. The overflow parameter controls what happens when text exceeds the available space: truncate (add ellipsis), shrink (reduce font size to fit), or clip (hard cut). If text overflows without any overflow parameter, the device defaults to truncate and reports a warning.

Newlines

The text content supports literal \n characters for explicit line breaks, used in combination with line_height (as a multiplier, e.g., 1.5) to control spacing between lines.

## 4.5 Zones (Partial Update Regions)

Zones are one of the most powerful features for agent use. They allow the agent to define named rectangular regions of the screen that can be updated independently using fast partial refresh, without touching the rest of the display. This is critical for e-ink, where a full refresh takes 2.5 seconds but a partial refresh takes 0.5 seconds or less.

Define Zones

POST /zones — Define or redefine the zone layout.

> {
>
> "zones": \[
>
> { "id": "header", "x": 0, "y": 0, "w": 960, "h": 80 },
>
> { "id": "main", "x": 0, "y": 80, "w": 960, "h": 500 },
>
> { "id": "sidebar", "x": 700, "y": 80, "w": 260, "h": 500 },
>
> { "id": "footer", "x": 0, "y": 580, "w": 960, "h": 100 }
>
> \]
>
> }

Update a Zone

To update a single zone, include the zone parameter in the POST /canvas request. Coordinates in commands are relative to the zone’s origin, not the full screen. The refresh is limited to the zone’s bounding box.

> {
>
> "zone": "footer",
>
> "commands": \[
>
> { "op": "clear", "color": "white" },
>
> { "op": "text", "x": 480, "y": 50,
>
> "text": "Last updated: 2:34 PM",
>
> "font": "sans", "size": 16,
>
> "align": "center", "baseline": "middle" }
>
> \],
>
> "refresh": "partial"
>
> }

Query Zones

GET /zones — Returns the current zone configuration.

Delete Zones

DELETE /zones — Clears all zone definitions. Subsequent renders use the full screen.

## 4.6 State Query Endpoints

GET /canvas — Returns the command list for the current frame, along with metadata such as frame ID and timestamp. This allows agents to understand what is currently displayed and make intelligent decisions about partial updates.

GET /canvas/screenshot — Returns the current framebuffer as a PNG image. This is invaluable for multimodal agents that can visually inspect the display to verify rendering, debug layout issues, or decide what to update.

POST /canvas/clear — Clears the display to white (or a specified color) and performs a full refresh. A convenience endpoint equivalent to posting a single clear command.

## 4.7 System Endpoints

GET /health — Simple health check. Returns 200 with uptime, free memory, Wi-Fi signal strength, and display status.

GET /openapi.json — Full OpenAPI 3.0 specification for all endpoints. Agents and agent frameworks can ingest this to auto-generate client code or tool definitions.

GET /mcp/tools.json — MCP (Model Context Protocol) tool definitions ready for use by MCP-compatible agent frameworks. See section 6 for details.

POST /device/name — Rename the device. Updates mDNS hostname. Accepts {"name": "new-name"}.

# 5. Firmware Architecture

## 5.1 Technology Stack

The firmware is built on ESP-IDF (Espressif IoT Development Framework) for maximum control over hardware resources, with Arduino compatibility layer available for rapid prototyping. The key software components form a layered architecture:

> ┌──────────────────────────────────────────┐
>
> │ mDNS Service Advertisement │
>
> │ \_aiscreen.\_tcp.local │
>
> ├──────────────────────────────────────────┤
>
> │ HTTP Server (async, ESP-IDF httpd) │
>
> │ ├── /device (capabilities) │
>
> │ ├── /canvas (render / query) │
>
> │ ├── /zones (region management) │
>
> │ ├── /openapi.json (API spec) │
>
> │ └── /mcp/tools.json (MCP tool def) │
>
> ├──────────────────────────────────────────┤
>
> │ Rendering Engine │
>
> │ ├── JSON command parser │
>
> │ ├── 2D drawing primitives │
>
> │ ├── Text layout engine │
>
> │ ├── PNG decoder + dithering │
>
> │ └── Framebuffer manager │
>
> ├──────────────────────────────────────────┤
>
> │ Display Driver (GxEPD2 / custom) │
>
> │ ├── Full refresh │
>
> │ ├── Partial refresh │
>
> │ └── Fast refresh │
>
> ├──────────────────────────────────────────┤
>
> │ ESP32-S3 Hardware + PSRAM │
>
> └──────────────────────────────────────────┘

## 5.2 Rendering Engine

The rendering engine is the core of the firmware. It receives a JSON array of drawing commands, executes them sequentially on an in-memory framebuffer, and then flushes the framebuffer to the display driver.

Drawing primitives are implemented directly on the framebuffer using established algorithms: Bresenham’s line algorithm, midpoint circle algorithm, scanline polygon fill, and so on. The Adafruit GFX library provides a solid foundation, though custom implementations may be needed for grayscale gradient support and advanced text layout.

For image handling, the engine supports PNG decoding with automatic dithering (Floyd-Steinberg, ordered, or threshold) to convert full-color images to the display’s grayscale palette. Images are decoded in streaming fashion to minimize peak memory usage.

## 5.3 Font System

Fonts are stored in flash memory as pre-rendered bitmap fonts at specific sizes. The firmware ships with three font families (sans, serif, mono) at the sizes listed in the /device endpoint. Custom fonts can be added by flashing new firmware or, in future versions, uploading font files via the API.

The text layout engine handles word wrapping, alignment, and multi-line rendering. It measures text before rendering to support the shrink overflow mode, where the font size is reduced until text fits the specified bounds.

## 5.4 Framebuffer Management

The system maintains a double-buffered framebuffer in PSRAM. The "front" buffer represents what is currently displayed on the e-ink panel. The "back" buffer is where new rendering commands draw. When a render is committed, the engine computes the difference between front and back buffers to determine the minimal region that needs to be sent to the display. This enables efficient partial refresh — only changed pixels are updated.

The command log for the current frame is stored alongside the framebuffer, enabling the GET /canvas endpoint to return the command history. A configurable maximum log size prevents memory exhaustion from complex frames.

# 6. Agent Integration

## 6.1 MCP Tool Definition

The device serves a ready-to-use MCP (Model Context Protocol) tool definition at /mcp/tools.json. MCP-compatible agent frameworks can discover the device via mDNS, fetch this endpoint, and immediately present the display as an available tool to the AI model. No manual configuration required.

The MCP tool definition includes two primary tools:

display_render Tool

The primary tool for rendering content. Accepts the full command array, zone targeting, and refresh mode. This is the low-level, full-control tool.

> {
>
> "name": "display_render",
>
> "description": "Render graphics and text on the physical
>
> e-ink display. Accepts drawing commands including shapes,
>
> text, and images. Commands execute in order on a
>
> 960x680 grayscale canvas.",
>
> "input_schema": {
>
> "type": "object",
>
> "properties": {
>
> "commands": {
>
> "type": "array",
>
> "description": "Drawing commands to execute",
>
> "items": { "\$ref": "#/definitions/DrawCommand" }
>
> },
>
> "refresh": {
>
> "type": "string",
>
> "enum": \["full", "partial", "fast"\],
>
> "default": "full"
>
> },
>
> "zone": {
>
> "type": "string",
>
> "description": "Optional zone ID for partial update"
>
> }
>
> },
>
> "required": \["commands"\]
>
> }
>
> }

display_info Tool

A read-only tool that returns current device state, capabilities, and what is currently displayed. Agents use this to understand the canvas before rendering.

## 6.2 OpenAPI Specification

For agent frameworks that use function-calling via OpenAPI specifications (such as those built on GPT-4 function calling, Claude tool use, or similar), the device serves a complete OpenAPI 3.0 spec at /openapi.json. This spec is auto-generated from the firmware’s endpoint definitions and is always in sync with the actual API.

## 6.3 Discovery Workflow

The typical agent integration workflow requires zero manual configuration:

1.  Agent framework starts and performs mDNS query for \_aiscreen.\_tcp.local services.

2.  Device(s) respond with name, IP, port, and basic display info in TXT records.

3.  Agent framework fetches /mcp/tools.json (or /openapi.json) from discovered device.

4.  Agent framework registers display_render and display_info as available tools.

5.  When a user asks the agent to display information, the model invokes display_render with appropriate drawing commands.

6.  The device renders the content and responds with status and any warnings.

For multi-device environments (e.g., displays in different rooms), the mDNS instance names differentiate devices, and the agent can reason about which display to target based on device names or user context.

# 7. Usage Examples

The following examples demonstrate the versatility of the API across common use cases. Each example shows the complete JSON payload an agent would send to POST /canvas.

## 7.1 Weather Dashboard

An agent creates a structured weather display with current conditions and a multi-day forecast. This demonstrates the use of shapes for layout, text at various sizes for hierarchy, and lines for visual separation.

> {
>
> "commands": \[
>
> { "op": "clear", "color": "white" },
>
> // Header bar
>
> { "op": "rect", "x": 0, "y": 0, "w": 960, "h": 70,
>
> "fill": "black" },
>
> { "op": "text", "x": 20, "y": 35, "text": "Weather",
>
> "font": "sans", "size": 28, "color": "white",
>
> "baseline": "middle" },
>
> { "op": "text", "x": 940, "y": 35,
>
> "text": "San Francisco, CA",
>
> "font": "sans", "size": 18, "color": "gray12",
>
> "align": "right", "baseline": "middle" },
>
> // Current temperature (large)
>
> { "op": "text", "x": 120, "y": 120,
>
> "text": "68°F",
>
> "font": "sans", "size": 96, "color": "black" },
>
> { "op": "text", "x": 120, "y": 230,
>
> "text": "Partly Cloudy",
>
> "font": "sans", "size": 24, "color": "gray6" },
>
> { "op": "text", "x": 120, "y": 270,
>
> "text": "H: 72° L: 58° Humidity: 65%",
>
> "font": "sans", "size": 16, "color": "gray8" },
>
> // Divider
>
> { "op": "line", "x1": 20, "y1": 320,
>
> "x2": 940, "y2": 320,
>
> "color": "gray12", "width": 1 },
>
> // 5-day forecast row
>
> { "op": "text", "x": 96, "y": 360, "text": "Mon",
>
> "font": "sans", "size": 16, "align": "center" },
>
> { "op": "text", "x": 96, "y": 390, "text": "72/58",
>
> "font": "sans", "size": 20, "align": "center",
>
> "color": "gray4" },
>
> { "op": "text", "x": 288, "y": 360, "text": "Tue",
>
> "font": "sans", "size": 16, "align": "center" },
>
> { "op": "text", "x": 288, "y": 390, "text": "70/56",
>
> "font": "sans", "size": 20, "align": "center",
>
> "color": "gray4" },
>
> { "op": "text", "x": 480, "y": 360, "text": "Wed",
>
> "font": "sans", "size": 16, "align": "center" },
>
> { "op": "text", "x": 480, "y": 390, "text": "65/54",
>
> "font": "sans", "size": 20, "align": "center",
>
> "color": "gray4" },
>
> { "op": "text", "x": 672, "y": 360, "text": "Thu",
>
> "font": "sans", "size": 16, "align": "center" },
>
> { "op": "text", "x": 672, "y": 390, "text": "68/55",
>
> "font": "sans", "size": 20, "align": "center",
>
> "color": "gray4" },
>
> { "op": "text", "x": 864, "y": 360, "text": "Fri",
>
> "font": "sans", "size": 16, "align": "center" },
>
> { "op": "text", "x": 864, "y": 390, "text": "71/57",
>
> "font": "sans", "size": 20, "align": "center",
>
> "color": "gray4" },
>
> // Footer
>
> { "op": "text", "x": 480, "y": 650,
>
> "text": "Updated Feb 18, 2026 at 10:30 AM",
>
> "font": "sans", "size": 12, "color": "gray10",
>
> "align": "center" }
>
> \],
>
> "refresh": "full"
>
> }

## 7.2 Daily Schedule

An agent renders the user’s calendar for the day, using rectangles as time blocks, grayscale shading to differentiate events, and zones to enable quick timestamp updates in the footer.

> {
>
> "commands": \[
>
> { "op": "clear", "color": "white" },
>
> // Title
>
> { "op": "text", "x": 20, "y": 30,
>
> "text": "Tuesday, February 18",
>
> "font": "sans", "size": 32, "color": "black",
>
> "bold": true },
>
> // Time block: 9 AM standup
>
> { "op": "rect", "x": 20, "y": 80, "w": 920, "h": 70,
>
> "fill": "gray14", "radius": 6 },
>
> { "op": "text", "x": 40, "y": 100,
>
> "text": "9:00 AM", "font": "mono", "size": 16,
>
> "color": "gray6" },
>
> { "op": "text", "x": 160, "y": 95,
>
> "text": "Team Standup",
>
> "font": "sans", "size": 20 },
>
> { "op": "text", "x": 160, "y": 125,
>
> "text": "Room 3B • 30 min",
>
> "font": "sans", "size": 14, "color": "gray6" },
>
> // Time block: 10:30 AM design review
>
> { "op": "rect", "x": 20, "y": 165, "w": 920, "h": 90,
>
> "fill": "gray13", "radius": 6 },
>
> { "op": "rect", "x": 20, "y": 165, "w": 6, "h": 90,
>
> "fill": "black", "radius": 3 },
>
> { "op": "text", "x": 40, "y": 185,
>
> "text": "10:30 AM", "font": "mono", "size": 16,
>
> "color": "gray6" },
>
> { "op": "text", "x": 160, "y": 180,
>
> "text": "Design Review - Q2 Roadmap",
>
> "font": "sans", "size": 20, "bold": true },
>
> { "op": "text", "x": 160, "y": 215,
>
> "text": "Conf Room A • 60 min • 6 attendees",
>
> "font": "sans", "size": 14, "color": "gray6" },
>
> // ... additional events ...
>
> // Footer with next-up summary
>
> { "op": "line", "x1": 20, "y1": 620,
>
> "x2": 940, "y2": 620, "color": "gray12" },
>
> { "op": "text", "x": 20, "y": 645,
>
> "text": "Next: Design Review in 45 min",
>
> "font": "sans", "size": 18, "color": "black" }
>
> \],
>
> "refresh": "full"
>
> }

## 7.3 Quick Notification (Partial Refresh)

An agent pushes a brief notification to a pre-defined zone using partial refresh for near-instant display. This demonstrates zone-based partial updates.

> {
>
> "zone": "footer",
>
> "commands": \[
>
> { "op": "clear", "color": "white" },
>
> { "op": "rect", "x": 10, "y": 10, "w": 940, "h": 80,
>
> "fill": "gray14", "stroke": "black",
>
> "stroke_width": 2, "radius": 8 },
>
> { "op": "text", "x": 40, "y": 35,
>
> "text": "New Message",
>
> "font": "sans", "size": 16, "bold": true },
>
> { "op": "text", "x": 40, "y": 60,
>
> "text": "Alice: Can we move the 3pm to 4pm?",
>
> "font": "sans", "size": 14, "color": "gray4" }
>
> \],
>
> "refresh": "fast"
>
> }

## 7.4 Data Visualization

An agent renders a simple bar chart using only primitive drawing operations. This demonstrates that even without a charting library, the drawing API is sufficient for data visualization — agents can compose any visual representation from basic shapes and text.

> {
>
> "commands": \[
>
> { "op": "clear", "color": "white" },
>
> { "op": "text", "x": 480, "y": 30,
>
> "text": "Weekly Steps", "font": "sans",
>
> "size": 24, "align": "center" },
>
> // Y-axis
>
> { "op": "line", "x1": 80, "y1": 80,
>
> "x2": 80, "y2": 550, "color": "gray8" },
>
> // X-axis
>
> { "op": "line", "x1": 80, "y1": 550,
>
> "x2": 900, "y2": 550, "color": "gray8" },
>
> // Bars (Mon-Sun)
>
> { "op": "rect", "x": 120, "y": 250,
>
> "w": 80, "h": 300, "fill": "gray4" },
>
> { "op": "text", "x": 160, "y": 570, "text": "Mon",
>
> "font": "sans", "size": 14, "align": "center" },
>
> { "op": "text", "x": 160, "y": 240,
>
> "text": "8.2k", "font": "sans", "size": 12,
>
> "align": "center", "color": "gray6" },
>
> { "op": "rect", "x": 230, "y": 180,
>
> "w": 80, "h": 370, "fill": "gray6" },
>
> { "op": "text", "x": 270, "y": 570, "text": "Tue",
>
> "font": "sans", "size": 14, "align": "center" },
>
> { "op": "text", "x": 270, "y": 170,
>
> "text": "10.1k", "font": "sans", "size": 12,
>
> "align": "center", "color": "gray6" },
>
> // ... remaining days ...
>
> // Y-axis labels
>
> { "op": "text", "x": 70, "y": 550,
>
> "text": "0", "font": "mono", "size": 12,
>
> "align": "right", "color": "gray8" },
>
> { "op": "text", "x": 70, "y": 320,
>
> "text": "5k", "font": "mono", "size": 12,
>
> "align": "right", "color": "gray8" },
>
> { "op": "text", "x": 70, "y": 80,
>
> "text": "10k", "font": "mono", "size": 12,
>
> "align": "right", "color": "gray8" }
>
> \],
>
> "refresh": "full"
>
> }

## 7.5 Generative Art

An agent generates artwork by composing many drawing primitives. This demonstrates the full creative potential of the low-level API — the agent is limited only by the pixel grid, not by predefined templates.

> {
>
> "commands": \[
>
> { "op": "clear", "color": "white" },
>
> { "op": "circle", "cx": 480, "cy": 340, "r": 250,
>
> "stroke": "black", "stroke_width": 3 },
>
> { "op": "circle", "cx": 480, "cy": 340, "r": 200,
>
> "stroke": "gray6", "stroke_width": 2 },
>
> { "op": "circle", "cx": 480, "cy": 340, "r": 150,
>
> "stroke": "gray10", "stroke_width": 1 },
>
> { "op": "polygon",
>
> "points": \[
>
> \[480, 90\], \[580, 240\], \[730, 240\],
>
> \[610, 340\], \[650, 500\], \[480, 410\],
>
> \[310, 500\], \[350, 340\], \[230, 240\],
>
> \[380, 240\]
>
> \],
>
> "stroke": "black", "stroke_width": 2 },
>
> { "op": "text", "x": 480, "y": 630,
>
> "text": "Generated at sunrise • Feb 18",
>
> "font": "serif", "size": 16, "color": "gray8",
>
> "align": "center" }
>
> \],
>
> "refresh": "full"
>
> }

## 7.6 Pre-Rendered Image Upload

When the drawing API is insufficient for complex visuals, the agent can render a full image externally and upload it. This approach is ideal for photos, complex charts generated by plotting libraries, or AI-generated images that have been converted to grayscale.

> {
>
> "commands": \[
>
> { "op": "image", "x": 0, "y": 0, "w": 960, "h": 680,
>
> "data": "\<base64 encoded 960x680 PNG\>",
>
> "format": "png",
>
> "dither": "floyd_steinberg" }
>
> \],
>
> "refresh": "full"
>
> }

The dithering algorithm converts the full-color image to the display’s 16-level grayscale palette. Supported algorithms are floyd_steinberg (best quality), ordered (faster, patterned), and threshold (fastest, binary).

# 8. Error Handling & Graceful Degradation

Consistent with the agent-first philosophy, the device prioritizes rendering something useful over returning errors. The following table defines how common error conditions are handled:

|                            |                                                |                      |
|----------------------------|------------------------------------------------|----------------------|
| **Condition**              | **Device Behavior**                            | **Warning Reported** |
| Coordinates out of bounds  | Clamp to screen/zone edges                     | coords_clamped       |
| Text exceeds max_width     | Wrap, shrink, or truncate per overflow setting | text_overflow        |
| Font not found             | Fall back to default sans font                 | font_fallback        |
| Font size not available    | Use nearest available size                     | size_adjusted        |
| Image too large for memory | Downsample before decoding                     | image_downsampled    |
| Image decode failure       | Render placeholder rectangle with X            | image_decode_failed  |
| Invalid color name         | Default to black                               | color_fallback       |
| Unknown operation          | Skip and continue                              | unknown_op           |
| Zone ID not found          | Render to full screen instead                  | zone_not_found       |
| Payload exceeds size limit | Return HTTP 413 error                          | (error, not warning) |

The only conditions that produce HTTP errors (4xx/5xx) rather than graceful fallbacks are malformed JSON (400), payload too large (413), and internal rendering failures (500). All other conditions render with best-effort and report warnings. The agent can inspect the warnings array in the response and adapt its subsequent requests accordingly.

# 9. Future Considerations

## 9.1 Touch Input

Future hardware revisions could include a capacitive touch overlay on the e-ink display. This would enable bidirectional agent-human interaction: the agent renders buttons or interactive regions, the human touches them, and touch events are reported back to the agent via a webhook or polling endpoint. The zone system already provides a natural foundation for mapping touch areas to semantic actions.

## 9.2 Multi-Display Coordination

In environments with multiple AI displays, agents could coordinate content across screens. A living room display might show a family calendar while an office display shows a work dashboard, with a kitchen display showing recipes. The mDNS discovery system supports this by exposing device names and locations, allowing agents to reason about which display to target for different content.

## 9.3 OTA Firmware Updates

The firmware should support over-the-air (OTA) updates to add new drawing operations, fonts, or protocol features without physical access to the device. The /device endpoint would report available updates, and a POST /system/update endpoint would trigger the update process.

## 9.4 Custom Font Upload

A future API extension could allow agents to upload custom fonts as TrueType or bitmap font files, expanding typography options beyond the built-in set. Uploaded fonts would be stored in flash and reported in the /device fonts array.

## 9.5 Webhook Subscriptions

Rather than polling the device, agents could subscribe to webhook notifications for events such as display refresh completion, touch input, button presses, or device status changes. This would enable more responsive agent-device interaction.

## 9.6 Color E-Ink

As color e-ink technology matures, the API is designed to accommodate color displays with minimal changes. The color system already uses named colors, and the /device endpoint reports available colors. A color display would simply advertise a richer color palette, and agents would use those color names in their commands.

# 10. Appendix

## 10.1 Color Reference

The 4-bit grayscale display supports 16 color levels. Colors are referenced by name in the API:

|          |             |          |               |
|----------|-------------|----------|---------------|
| **Name** | **Value**   | **Name** | **Value**     |
| black    | 0 (darkest) | gray8    | 8             |
| gray1    | 1           | gray9    | 9             |
| gray2    | 2           | gray10   | 10            |
| gray3    | 3           | gray11   | 11            |
| gray4    | 4           | gray12   | 12            |
| gray5    | 5           | gray13   | 13            |
| gray6    | 6           | gray14   | 14            |
| gray7    | 7           | white    | 15 (lightest) |

Colors can also be specified as integer values (0–15) for agents that prefer numeric addressing.

## 10.2 Coordinate System

The origin (0, 0) is the top-left corner of the display. The x-axis increases to the right, and the y-axis increases downward. All coordinates are in pixels. For zone-relative rendering, the origin is the top-left corner of the zone.

## 10.3 HTTP Status Codes

|          |                   |                                                             |
|----------|-------------------|-------------------------------------------------------------|
| **Code** | **Status**        | **Meaning**                                                 |
| 200      | OK                | Request successful, content rendered (check warnings array) |
| 400      | Bad Request       | Malformed JSON or missing required fields                   |
| 413      | Payload Too Large | Request body exceeds max_payload_bytes                      |
| 429      | Too Many Requests | Display is still refreshing from a previous request         |
| 500      | Internal Error    | Unexpected firmware error during rendering                  |

*End of Specification*
