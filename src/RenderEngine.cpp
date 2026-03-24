#include "RenderEngine.h"
#include <mbedtls/base64.h>
#include <math.h>
#include <qrcode.h>

RenderEngine::RenderEngine(EPD& epd, FramebufferManager& fb, ZoneRegistry& zones, CommandLog& log)
    : _epd(epd), _fb(fb), _zones(zones), _log(log),
      _offsetX(0), _offsetY(0), _zoneW(EPD_WIDTH), _zoneH(EPD_HEIGHT),
      _dryRun(false)
{
    _clip.active = false;
    _clip.x = 0; _clip.y = 0;
    _clip.w = EPD_WIDTH; _clip.h = EPD_HEIGHT;
}

bool RenderEngine::resolveColor(const JsonVariant& color, bool defaultBlack)
{
    if (color.isNull()) return defaultBlack;

    if (color.is<const char*>()) {
        String name = color.as<String>();
        name.toLowerCase();
        if (name == "white") return false;
        if (name == "black") return true;
        // gray0-gray15: threshold at gray8
        if (name.startsWith("gray")) {
            int level = name.substring(4).toInt();
            return level >= 8; // gray8+ = black
        }
        return defaultBlack;
    }

    if (color.is<int>()) {
        int val = color.as<int>();
        return val >= 8; // 0-7 = white, 8-15 = black
    }

    return defaultBlack;
}

void RenderEngine::setPixel(int16_t x, int16_t y, bool black)
{
    if (_dryRun) return;

    int16_t ax = x + _offsetX;
    int16_t ay = y + _offsetY;

    // Zone bounds check
    if (_zoneW < EPD_WIDTH || _zoneH < EPD_HEIGHT) {
        if (ax < _offsetX || ax >= _offsetX + _zoneW ||
            ay < _offsetY || ay >= _offsetY + _zoneH) return;
    }

    if (isClipped(ax, ay)) return;

    _fb.setPixel(ax, ay, !black); // framebuffer: true=white
}

bool RenderEngine::isClipped(int16_t x, int16_t y) const
{
    if (!_clip.active) return false;
    return (x < _clip.x || x >= _clip.x + _clip.w ||
            y < _clip.y || y >= _clip.y + _clip.h);
}

void RenderEngine::pushBBox(RenderResult& result, int16_t x, int16_t y, int16_t w, int16_t h,
                             int16_t textLines, bool textTruncated)
{
    if (!result.trackBBoxes) return;
    CommandBBox bb;
    bb.x = x; bb.y = y; bb.w = w; bb.h = h;
    bb.offScreen = (x < 0 || y < 0 || (x + w) > EPD_WIDTH || (y + h) > EPD_HEIGHT);
    bb.textLines = textLines;
    bb.textTruncated = textTruncated;
    result.bboxes.push_back(bb);
}

RenderEngine::RenderResult RenderEngine::execute(const JsonArray& commands, const String& zone, bool validate, bool dryRun)
{
    RenderResult result;
    result.commandsExecuted = 0;
    result.renderTimeMs = 0;
    _dryRun = dryRun;
    if (dryRun) validate = true; // dryRun implies validate
    result.trackBBoxes = validate;

    unsigned long startTime = millis();

    // Apply zone offset if specified
    _offsetX = 0; _offsetY = 0;
    _zoneW = EPD_WIDTH; _zoneH = EPD_HEIGHT;
    if (zone.length() > 0) {
        const ZoneRegistry::Zone* z = _zones.findZone(zone);
        if (z) {
            _offsetX = z->x;
            _offsetY = z->y;
            _zoneW = z->w;
            _zoneH = z->h;
        } else {
            result.warnings.push_back("Zone '" + zone + "' not found, using full screen");
        }
    }

    _clip.active = false;
    _log.beginFrame();

    for (size_t i = 0; i < commands.size(); i++) {
        JsonObject cmd = commands[i];
        if (!cmd.containsKey("op")) {
            result.warnings.push_back("Command " + String(i) + ": missing 'op'");
            continue;
        }

        String op = cmd["op"].as<String>();
        op.toLowerCase();

        // Extract per-command metadata
        String meta = "";
        if (cmd.containsKey("meta")) {
            DynamicJsonDocument metaDoc(256);
            metaDoc.set(cmd["meta"]);
            serializeJson(metaDoc, meta);
        }
        _log.addCommand(op, cmd["x"] | 0, cmd["y"] | 0, meta);

        if (op == "clear") execClear(cmd, result);
        else if (op == "pixel") execPixel(cmd, result);
        else if (op == "line") execLine(cmd, result);
        else if (op == "rect") execRect(cmd, result);
        else if (op == "circle") execCircle(cmd, result);
        else if (op == "ellipse") execEllipse(cmd, result);
        else if (op == "arc") execArc(cmd, result);
        else if (op == "polygon") execPolygon(cmd, result);
        else if (op == "polyline") execPolyline(cmd, result);
        else if (op == "text") execText(cmd, result);
        else if (op == "image") execImage(cmd, result);
        else if (op == "gradient") execGradient(cmd, result);
        else if (op == "flood_fill") execFloodFill(cmd, result);
        else if (op == "clip") execClip(cmd, result);
        else if (op == "unclip") execUnclip(cmd, result);
        else if (op == "raw_bitmap") execRawBitmap(cmd, result);
        else if (op == "qr") execQrCode(cmd, result);
        else {
            result.warnings.push_back("Unknown op: " + op);
            continue;
        }

        result.commandsExecuted++;
    }

    result.renderTimeMs = millis() - startTime;
    return result;
}

void RenderEngine::execClear(const JsonObject& cmd, RenderResult& result)
{
    bool black = resolveColor(cmd["color"], false); // default white
    if (!_dryRun) _fb.clear(!black);
    pushBBox(result, _offsetX, _offsetY, _zoneW, _zoneH);
}

void RenderEngine::execPixel(const JsonObject& cmd, RenderResult& result)
{
    int16_t x = cmd["x"] | 0;
    int16_t y = cmd["y"] | 0;
    bool black = resolveColor(cmd["color"]);
    setPixel(x, y, black);
    pushBBox(result, x + _offsetX, y + _offsetY, 1, 1);
}

void RenderEngine::execLine(const JsonObject& cmd, RenderResult& result)
{
    // Support naming conventions: (x0,y0,x1,y1), (x,y,x1,y1), and (x1,y1,x2,y2)
    // When x2/y2 present: x1,y1 is start, x2,y2 is end
    // Use containsKey to correctly handle zero coordinates
    int16_t x0, y0, x1, y1;
    if (cmd.containsKey("x2") || cmd.containsKey("y2")) {
        // Agent sent (x1,y1,x2,y2) — "point 1" to "point 2"
        x0 = cmd.containsKey("x1") ? cmd["x1"].as<int16_t>() : 0;
        y0 = cmd.containsKey("y1") ? cmd["y1"].as<int16_t>() : 0;
        x1 = cmd.containsKey("x2") ? cmd["x2"].as<int16_t>() : 0;
        y1 = cmd.containsKey("y2") ? cmd["y2"].as<int16_t>() : 0;
    } else {
        // Standard (x0,y0,x1,y1) or (x,y,x1,y1)
        x0 = cmd.containsKey("x0") ? cmd["x0"].as<int16_t>()
           : cmd.containsKey("x")  ? cmd["x"].as<int16_t>() : 0;
        y0 = cmd.containsKey("y0") ? cmd["y0"].as<int16_t>()
           : cmd.containsKey("y")  ? cmd["y"].as<int16_t>() : 0;
        x1 = cmd.containsKey("x1") ? cmd["x1"].as<int16_t>() : 0;
        y1 = cmd.containsKey("y1") ? cmd["y1"].as<int16_t>() : 0;
    }
    bool black = resolveColor(cmd["color"]);
    int16_t thickness = cmd["thickness"] | 1;

    // Bresenham with thickness
    int16_t dx = abs(x1 - x0);
    int16_t dy = -abs(y1 - y0);
    int16_t sx = x0 < x1 ? 1 : -1;
    int16_t sy = y0 < y1 ? 1 : -1;
    int16_t err = dx + dy;
    int16_t cx = x0, cy = y0;

    while (true) {
        if (thickness <= 1) {
            setPixel(cx, cy, black);
        } else {
            int16_t half = thickness / 2;
            for (int16_t ty = -half; ty <= half; ty++) {
                for (int16_t tx = -half; tx <= half; tx++) {
                    setPixel(cx + tx, cy + ty, black);
                }
            }
        }
        if (cx == x1 && cy == y1) break;
        int16_t e2 = 2 * err;
        if (e2 >= dy) { err += dy; cx += sx; }
        if (e2 <= dx) { err += dx; cy += sy; }
    }

    int16_t half = (thickness - 1) / 2;
    int16_t bx = min(x0, x1) - half + _offsetX;
    int16_t by = min(y0, y1) - half + _offsetY;
    int16_t bw = abs(x1 - x0) + thickness;
    int16_t bh = abs(y1 - y0) + thickness;
    pushBBox(result, bx, by, bw, bh);
}

void RenderEngine::execRect(const JsonObject& cmd, RenderResult& result)
{
    int16_t x = cmd["x"] | 0;
    int16_t y = cmd["y"] | 0;
    int16_t w = cmd["w"] | 0;
    int16_t h = cmd["h"] | 0;
    int16_t r = cmd["radius"] | 0;
    bool fill = cmd["fill"] | true;
    bool black = resolveColor(cmd["color"]);

    if (fill) {
        if (r > 0) {
            // Filled rounded rect via EPD primitives on our back buffer
            // We'll draw directly to framebuffer
            for (int16_t row = y; row < y + h; row++) {
                for (int16_t col = x; col < x + w; col++) {
                    // Simple rounded rect: skip corners outside radius
                    bool inside = true;
                    if (r > 0) {
                        int16_t dx2 = 0, dy2 = 0;
                        if (col < x + r && row < y + r) { dx2 = x + r - col; dy2 = y + r - row; }
                        else if (col >= x + w - r && row < y + r) { dx2 = col - (x + w - r - 1); dy2 = y + r - row; }
                        else if (col < x + r && row >= y + h - r) { dx2 = x + r - col; dy2 = row - (y + h - r - 1); }
                        else if (col >= x + w - r && row >= y + h - r) { dx2 = col - (x + w - r - 1); dy2 = row - (y + h - r - 1); }
                        if (dx2 > 0 && dy2 > 0) {
                            inside = (dx2 * dx2 + dy2 * dy2) <= (r * r);
                        }
                    }
                    if (inside) setPixel(col, row, black);
                }
            }
        } else {
            for (int16_t row = y; row < y + h; row++) {
                for (int16_t col = x; col < x + w; col++) {
                    setPixel(col, row, black);
                }
            }
        }
    }

    // Stroke
    if (cmd.containsKey("stroke")) {
        bool strokeBlack = resolveColor(cmd["stroke"]);
        int16_t strokeW = cmd["strokeWidth"] | 1;
        for (int16_t t = 0; t < strokeW; t++) {
            // Top
            for (int16_t col = x + t; col < x + w - t; col++) {
                setPixel(col, y + t, strokeBlack);
                setPixel(col, y + h - 1 - t, strokeBlack);
            }
            // Sides
            for (int16_t row = y + t; row < y + h - t; row++) {
                setPixel(x + t, row, strokeBlack);
                setPixel(x + w - 1 - t, row, strokeBlack);
            }
        }
    } else if (!fill) {
        // Outline only
        for (int16_t col = x; col < x + w; col++) {
            setPixel(col, y, black);
            setPixel(col, y + h - 1, black);
        }
        for (int16_t row = y; row < y + h; row++) {
            setPixel(x, row, black);
            setPixel(x + w - 1, row, black);
        }
    }

    pushBBox(result, x + _offsetX, y + _offsetY, w, h);
}

void RenderEngine::execCircle(const JsonObject& cmd, RenderResult& result)
{
    int16_t cx = cmd["x"] | 0;
    int16_t cy = cmd["y"] | 0;
    int16_t r = cmd["r"] | (cmd["radius"] | 0);
    bool fill = cmd["fill"] | true;
    bool black = resolveColor(cmd["color"]);

    if (fill) {
        for (int16_t dy = -r; dy <= r; dy++) {
            for (int16_t dx = -r; dx <= r; dx++) {
                if (dx * dx + dy * dy <= r * r) {
                    setPixel(cx + dx, cy + dy, black);
                }
            }
        }
    } else {
        // Midpoint circle
        int16_t x = 0, y = r;
        int16_t d = 1 - r;
        while (x <= y) {
            setPixel(cx + x, cy + y, black); setPixel(cx - x, cy + y, black);
            setPixel(cx + x, cy - y, black); setPixel(cx - x, cy - y, black);
            setPixel(cx + y, cy + x, black); setPixel(cx - y, cy + x, black);
            setPixel(cx + y, cy - x, black); setPixel(cx - y, cy - x, black);
            if (d < 0) { d += 2 * x + 3; }
            else { d += 2 * (x - y) + 5; y--; }
            x++;
        }
    }

    pushBBox(result, cx - r + _offsetX, cy - r + _offsetY, 2 * r + 1, 2 * r + 1);
}

void RenderEngine::execEllipse(const JsonObject& cmd, RenderResult& result)
{
    int16_t cx = cmd["x"] | 0;
    int16_t cy = cmd["y"] | 0;
    int16_t rx = cmd["rx"] | 0;
    int16_t ry = cmd["ry"] | 0;
    bool fill = cmd["fill"] | true;
    bool black = resolveColor(cmd["color"]);

    if (fill) {
        for (int16_t dy = -ry; dy <= ry; dy++) {
            for (int16_t dx = -rx; dx <= rx; dx++) {
                float ex = (float)dx / rx;
                float ey = (float)dy / ry;
                if (ex * ex + ey * ey <= 1.0f) {
                    setPixel(cx + dx, cy + dy, black);
                }
            }
        }
    } else {
        // Midpoint ellipse outline
        for (int deg = 0; deg < 360; deg++) {
            float rad = deg * M_PI / 180.0f;
            int16_t px = cx + (int16_t)(rx * cosf(rad));
            int16_t py = cy + (int16_t)(ry * sinf(rad));
            setPixel(px, py, black);
        }
    }

    pushBBox(result, cx - rx + _offsetX, cy - ry + _offsetY, 2 * rx + 1, 2 * ry + 1);
}

void RenderEngine::execArc(const JsonObject& cmd, RenderResult& result)
{
    int16_t cx = cmd["x"] | 0;
    int16_t cy = cmd["y"] | 0;
    int16_t r = cmd["r"] | (cmd["radius"] | 0);
    float startAngle = cmd["startAngle"] | 0.0f;
    float endAngle = cmd["endAngle"] | 360.0f;
    bool black = resolveColor(cmd["color"]);

    for (float deg = startAngle; deg <= endAngle; deg += 1.0f) {
        float rad = deg * M_PI / 180.0f;
        int16_t px = cx + (int16_t)(r * cosf(rad));
        int16_t py = cy + (int16_t)(r * sinf(rad));
        setPixel(px, py, black);
    }

    pushBBox(result, cx - r + _offsetX, cy - r + _offsetY, 2 * r + 1, 2 * r + 1);
}

void RenderEngine::execPolygon(const JsonObject& cmd, RenderResult& result)
{
    JsonArray points = cmd["points"];
    if (!points || points.size() < 3) {
        result.warnings.push_back("polygon: need >= 3 points");
        return;
    }

    bool fill = cmd["fill"] | true;
    bool black = resolveColor(cmd["color"]);

    int nPoints = min((int)points.size(), 64);
    int16_t px[64], py[64];
    int16_t minX = 32767, maxX = -32768;
    int16_t minY = 32767, maxY = -32768;

    for (int i = 0; i < nPoints; i++) {
        px[i] = points[i]["x"] | 0;
        py[i] = points[i]["y"] | 0;
        if (px[i] < minX) minX = px[i];
        if (px[i] > maxX) maxX = px[i];
        if (py[i] < minY) minY = py[i];
        if (py[i] > maxY) maxY = py[i];
    }

    if (fill) {
        // Scanline fill
        for (int16_t scanY = minY; scanY <= maxY; scanY++) {
            int16_t nodeX[64];
            int nodes = 0;

            for (int i = 0, j = nPoints - 1; i < nPoints; j = i++) {
                if ((py[i] <= scanY && py[j] > scanY) || (py[j] <= scanY && py[i] > scanY)) {
                    nodeX[nodes++] = px[i] + (int32_t)(scanY - py[i]) * (px[j] - px[i]) / (py[j] - py[i]);
                }
            }

            // Sort
            for (int i = 0; i < nodes - 1; i++) {
                for (int j = i + 1; j < nodes; j++) {
                    if (nodeX[i] > nodeX[j]) { int16_t t = nodeX[i]; nodeX[i] = nodeX[j]; nodeX[j] = t; }
                }
            }

            for (int i = 0; i < nodes - 1; i += 2) {
                for (int16_t x = nodeX[i]; x <= nodeX[i + 1]; x++) {
                    setPixel(x, scanY, black);
                }
            }
        }
    }

    // Draw outline
    for (int i = 0; i < nPoints; i++) {
        int j = (i + 1) % nPoints;
        // Bresenham line
        int16_t x0 = px[i], y0 = py[i], x1 = px[j], y1 = py[j];
        int16_t dx = abs(x1 - x0), dy = -abs(y1 - y0);
        int16_t sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
        int16_t err = dx + dy;
        while (true) {
            setPixel(x0, y0, black);
            if (x0 == x1 && y0 == y1) break;
            int16_t e2 = 2 * err;
            if (e2 >= dy) { err += dy; x0 += sx; }
            if (e2 <= dx) { err += dx; y0 += sy; }
        }
    }

    pushBBox(result, minX + _offsetX, minY + _offsetY, maxX - minX + 1, maxY - minY + 1);
}

void RenderEngine::execPolyline(const JsonObject& cmd, RenderResult& result)
{
    JsonArray points = cmd["points"];
    if (!points || points.size() < 2) {
        result.warnings.push_back("polyline: need >= 2 points");
        return;
    }

    bool black = resolveColor(cmd["color"]);
    int nPoints = min((int)points.size(), 64);
    int16_t plMinX = 32767, plMaxX = -32768;
    int16_t plMinY = 32767, plMaxY = -32768;

    for (int i = 0; i < nPoints - 1; i++) {
        int16_t x0 = points[i]["x"] | 0;
        int16_t y0 = points[i]["y"] | 0;
        int16_t x1 = points[i + 1]["x"] | 0;
        int16_t y1 = points[i + 1]["y"] | 0;

        if (x0 < plMinX) plMinX = x0; if (x0 > plMaxX) plMaxX = x0;
        if (y0 < plMinY) plMinY = y0; if (y0 > plMaxY) plMaxY = y0;
        if (i == nPoints - 2) {
            if (x1 < plMinX) plMinX = x1; if (x1 > plMaxX) plMaxX = x1;
            if (y1 < plMinY) plMinY = y1; if (y1 > plMaxY) plMaxY = y1;
        }

        int16_t dx = abs(x1 - x0), dy = -abs(y1 - y0);
        int16_t sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
        int16_t err = dx + dy;
        while (true) {
            setPixel(x0, y0, black);
            if (x0 == x1 && y0 == y1) break;
            int16_t e2 = 2 * err;
            if (e2 >= dy) { err += dy; x0 += sx; }
            if (e2 <= dx) { err += dx; y0 += sy; }
        }
    }

    pushBBox(result, plMinX + _offsetX, plMinY + _offsetY, plMaxX - plMinX + 1, plMaxY - plMinY + 1);
}

void RenderEngine::execText(const JsonObject& cmd, RenderResult& result)
{
    int16_t x = cmd["x"] | 0;
    int16_t y = cmd["y"] | 0;
    String text = cmd["text"] | "";
    if (text.length() == 0) return;

    String family = cmd["font"] | "sans";
    int size = cmd["size"] | 12;
    bool bold = cmd["bold"] | false;

    const GFXfont* font = FontRegistry::findClosest(family.c_str(), size, bold);
    if (!font) {
        result.warnings.push_back("No font found for " + family + " " + String(size));
        font = FontRegistry::findClosest("sans", 12, false);
        if (!font) return;
    }

    bool black = resolveColor(cmd["color"]);
    uint16_t epdColor = black ? GxEPD_BLACK : GxEPD_WHITE;

    // Check for layout params
    int16_t maxWidth = cmd["maxWidth"] | 0;
    int16_t maxHeight = cmd["maxHeight"] | 0;
    String alignStr = cmd["align"] | "left";
    String vAlignStr = cmd["valign"] | "top";
    String overflowStr = cmd["overflow"] | "clip";
    float lineHeight = cmd["lineHeight"] | 1.2f;

    TextLayout::HAlign hAlign = TextLayout::ALIGN_LEFT;
    if (alignStr == "center") hAlign = TextLayout::ALIGN_CENTER;
    else if (alignStr == "right") hAlign = TextLayout::ALIGN_RIGHT;

    TextLayout::VAlign vAlign = TextLayout::VALIGN_TOP;
    if (vAlignStr == "middle") vAlign = TextLayout::VALIGN_MIDDLE;
    else if (vAlignStr == "bottom") vAlign = TextLayout::VALIGN_BOTTOM;

    TextLayout::Overflow overflow = TextLayout::OVERFLOW_CLIP;
    if (overflowStr == "truncate") overflow = TextLayout::OVERFLOW_TRUNCATE;
    else if (overflowStr == "shrink") overflow = TextLayout::OVERFLOW_SHRINK;

    // EPD buffer IS the framebuffer back buffer, so text renders directly
    TextLayout::LayoutParams params;
    params.x = x + _offsetX;
    params.y = y + _offsetY;
    params.maxWidth = maxWidth;
    params.maxHeight = maxHeight;
    params.hAlign = hAlign;
    params.vAlign = vAlign;
    params.overflow = overflow;
    params.lineHeight = lineHeight;
    params.font = font;

    TextLayout::LayoutResult lr = _dryRun
        ? TextLayout::measure(_epd, text, params)
        : TextLayout::render(_epd, text, params, epdColor);

    if (result.trackBBoxes) {
        int16_t bboxX = params.x;
        int16_t bboxW = lr.width;
        if (maxWidth > 0) {
            bboxW = maxWidth;
        } else if (hAlign == TextLayout::ALIGN_CENTER) {
            bboxX = params.x - lr.width / 2;
        } else if (hAlign == TextLayout::ALIGN_RIGHT) {
            bboxX = params.x - lr.width;
        }
        pushBBox(result, bboxX, params.y, bboxW, lr.height, lr.lineCount, lr.truncated);
    }
}

void RenderEngine::execImage(const JsonObject& cmd, RenderResult& result)
{
    int16_t x = cmd["x"] | 0;
    int16_t y = cmd["y"] | 0;
    int16_t w = cmd["w"] | 0;
    int16_t h = cmd["h"] | 0;
    String data = cmd["data"] | "";
    String ditherStr = cmd["dither"] | "ordered";

    if (data.length() == 0) {
        result.warnings.push_back("image: no data");
        return;
    }

    DitherMode dither = DITHER_ORDERED;
    if (ditherStr == "threshold") dither = DITHER_THRESHOLD;
    else if (ditherStr == "floyd_steinberg") dither = DITHER_FLOYD_STEINBERG;

    if (!_dryRun) {
        ImageDecoder::DecodeParams params;
        params.base64Data = data.c_str();
        params.base64Len = data.length();
        params.destX = x + _offsetX;
        params.destY = y + _offsetY;
        params.destW = w;
        params.destH = h;
        params.dither = dither;
        params.framebuffer = _fb.getBackBuffer();
        params.fbWidth = EPD_WIDTH;
        params.fbHeight = EPD_HEIGHT;

        ImageDecoder::DecodeResult dr = ImageDecoder::decode(params);
        if (!dr.success) {
            result.warnings.push_back("image decode: " + dr.error);
        }
    }
    pushBBox(result, x + _offsetX, y + _offsetY, w, h);
}

void RenderEngine::execGradient(const JsonObject& cmd, RenderResult& result)
{
    int16_t x = cmd["x"] | 0;
    int16_t y = cmd["y"] | 0;
    int16_t w = cmd["w"] | EPD_WIDTH;
    int16_t h = cmd["h"] | EPD_HEIGHT;
    String direction = cmd["direction"] | "horizontal";
    bool fromBlack = resolveColor(cmd["from"], true);
    bool toBlack = resolveColor(cmd["to"], false);

    for (int16_t row = 0; row < h; row++) {
        for (int16_t col = 0; col < w; col++) {
            float t;
            if (direction == "vertical") {
                t = (float)row / (h > 1 ? h - 1 : 1);
            } else {
                t = (float)col / (w > 1 ? w - 1 : 1);
            }

            // Linear interpolation: from=black(255), to=white(0)
            float fromVal = fromBlack ? 0.0f : 255.0f;
            float toVal = toBlack ? 0.0f : 255.0f;
            float gray = fromVal + t * (toVal - fromVal);

            uint8_t threshold = ImageDecoder::_bayerMatrix[(y + row) & 7][(x + col) & 7] * 4;
            bool black = gray < threshold;

            setPixel(x + col, y + row, black);
        }
    }

    pushBBox(result, x + _offsetX, y + _offsetY, w, h);
}

void RenderEngine::execFloodFill(const JsonObject& cmd, RenderResult& result)
{
    int16_t startX = cmd["x"] | 0;
    int16_t startY = cmd["y"] | 0;
    bool black = resolveColor(cmd["color"]);

    if (_dryRun) {
        // Can't compute accurate bbox without actually flood-filling
        pushBBox(result, startX + _offsetX, startY + _offsetY, 1, 1);
        return;
    }

    bool targetWhite = _fb.getPixel(startX + _offsetX, startY + _offsetY); // true=white

    // Target must be different from fill color
    if ((black && !targetWhite) || (!black && targetWhite)) return;

    // BFS with capped queue
    static const int MAX_QUEUE = 4096;
    struct Point { int16_t x, y; };
    Point* queue = (Point*)malloc(MAX_QUEUE * sizeof(Point));
    if (!queue) {
        result.warnings.push_back("flood_fill: out of memory");
        return;
    }

    int head = 0, tail = 0;
    queue[tail++] = {startX, startY};
    setPixel(startX, startY, black);

    while (head != tail && tail < MAX_QUEUE) {
        Point p = queue[head++];
        if (head >= MAX_QUEUE) head = 0;

        const int16_t dx[] = {1, -1, 0, 0};
        const int16_t dy[] = {0, 0, 1, -1};

        for (int d = 0; d < 4; d++) {
            int16_t nx = p.x + dx[d];
            int16_t ny = p.y + dy[d];
            int16_t ax = nx + _offsetX;
            int16_t ay = ny + _offsetY;

            if (ax < 0 || ax >= EPD_WIDTH || ay < 0 || ay >= EPD_HEIGHT) continue;
            if (isClipped(ax, ay)) continue;

            bool pixelWhite = _fb.getPixel(ax, ay);
            if (pixelWhite == targetWhite) {
                setPixel(nx, ny, black);
                if (tail < MAX_QUEUE) {
                    queue[tail++] = {nx, ny};
                }
            }
        }
    }

    if (tail >= MAX_QUEUE) {
        result.warnings.push_back("flood_fill: queue limit reached, fill may be incomplete");
    }

    free(queue);
    pushBBox(result, startX + _offsetX, startY + _offsetY, 1, 1);
}

void RenderEngine::execClip(const JsonObject& cmd, RenderResult& result)
{
    _clip.x = (cmd["x"] | 0) + _offsetX;
    _clip.y = (cmd["y"] | 0) + _offsetY;
    _clip.w = cmd["w"] | EPD_WIDTH;
    _clip.h = cmd["h"] | EPD_HEIGHT;
    _clip.active = true;
    pushBBox(result, _clip.x, _clip.y, _clip.w, _clip.h);
}

void RenderEngine::execUnclip(const JsonObject& cmd, RenderResult& result)
{
    _clip.active = false;
    pushBBox(result, 0, 0, 0, 0);
}

void RenderEngine::execRawBitmap(const JsonObject& cmd, RenderResult& result)
{
    int16_t x = cmd["x"] | 0;
    int16_t y = cmd["y"] | 0;
    int16_t w = cmd["w"] | 0;
    int16_t h = cmd["h"] | 0;
    String data = cmd["data"] | "";

    if (data.length() == 0 || w <= 0 || h <= 0) {
        result.warnings.push_back("raw_bitmap: missing data/dimensions");
        return;
    }

    // Decode base64
    size_t binLen = 0;
    int ret = mbedtls_base64_decode(nullptr, 0, &binLen,
                                     (const uint8_t*)data.c_str(), data.length());
    if (ret != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL || binLen == 0) {
        result.warnings.push_back("raw_bitmap: invalid base64");
        return;
    }

    size_t expectedLen = ((w + 7) / 8) * h;
    uint8_t* binData = (uint8_t*)malloc(binLen);
    if (!binData) {
        result.warnings.push_back("raw_bitmap: out of memory");
        return;
    }

    size_t actualLen = 0;
    mbedtls_base64_decode(binData, binLen, &actualLen,
                           (const uint8_t*)data.c_str(), data.length());

    int bytesPerRow = (w + 7) / 8;
    for (int16_t row = 0; row < h && row * bytesPerRow < (int16_t)actualLen; row++) {
        for (int16_t col = 0; col < w; col++) {
            int byteIdx = row * bytesPerRow + (col / 8);
            int bitIdx = 7 - (col % 8);
            if (byteIdx < (int)actualLen) {
                bool black = !((binData[byteIdx] >> bitIdx) & 1); // 1=white, 0=black
                setPixel(x + col, y + row, black);
            }
        }
    }

    free(binData);
    pushBBox(result, x + _offsetX, y + _offsetY, w, h);
}

void RenderEngine::execQrCode(const JsonObject& cmd, RenderResult& result)
{
    int16_t x = cmd["x"] | 0;
    int16_t y = cmd["y"] | 0;
    int16_t size = cmd["size"] | 100;
    String data = cmd["data"] | "";
    bool black = resolveColor(cmd["color"]);
    String ecStr = cmd["ec"] | "M";

    if (data.length() == 0) {
        result.warnings.push_back("qr: no data");
        return;
    }

    uint8_t ecLevel = ECC_MEDIUM;
    if (ecStr == "L") ecLevel = ECC_LOW;
    else if (ecStr == "Q") ecLevel = ECC_QUARTILE;
    else if (ecStr == "H") ecLevel = ECC_HIGH;

    // Binary mode byte capacity per version with ECC_MEDIUM:
    // v1=14, v2=26, v3=42, v4=62, v5=84, v6=106, v7=122, v8=152, v9=180, v10=213
    static const int byteCapacity[] = {0, 14, 26, 42, 62, 84, 106, 122, 152, 180, 213};
    int dataLen = data.length();
    int minVersion = 1;
    for (int v = 1; v <= 10; v++) {
        if (byteCapacity[v] >= dataLen) {
            minVersion = v;
            break;
        }
        if (v == 10) minVersion = 10;
    }

    // Try progressively larger versions until data fits
    QRCode qrcode;
    uint8_t* qrcodeData = nullptr;
    bool success = false;

    for (int v = minVersion; v <= 10; v++) {
        size_t bufSize = qrcode_getBufferSize(v);
        qrcodeData = (uint8_t*)malloc(bufSize);
        if (!qrcodeData) {
            result.warnings.push_back("qr: out of memory");
            return;
        }

        int err = qrcode_initText(&qrcode, qrcodeData, v, ecLevel, data.c_str());
        if (err == 0) {
            success = true;
            break;
        }
        free(qrcodeData);
        qrcodeData = nullptr;
    }

    if (!success) {
        result.warnings.push_back("qr: data too long for QR version 1-10");
        return;
    }

    // Calculate module pixel size
    int modules = qrcode.size;
    int moduleSize = size / (modules + 4); // +4 for 2-module quiet zone on each side
    if (moduleSize < 1) moduleSize = 1;

    int actualSize = moduleSize * (modules + 4);
    int offsetX = (size - actualSize) / 2;
    int offsetY = (size - actualSize) / 2;

    // Draw quiet zone (background)
    bool bg = !black;
    for (int16_t row = 0; row < size; row++) {
        for (int16_t col = 0; col < size; col++) {
            setPixel(x + col, y + row, bg);
        }
    }

    // Draw QR modules
    int qzOffset = 2 * moduleSize; // quiet zone offset
    for (int my = 0; my < modules; my++) {
        for (int mx = 0; mx < modules; mx++) {
            if (qrcode_getModule(&qrcode, mx, my)) {
                int px = x + offsetX + qzOffset + mx * moduleSize;
                int py = y + offsetY + qzOffset + my * moduleSize;
                for (int dy = 0; dy < moduleSize; dy++) {
                    for (int dx = 0; dx < moduleSize; dx++) {
                        setPixel(px + dx, py + dy, black);
                    }
                }
            }
        }
    }

    free(qrcodeData);
    pushBBox(result, x + _offsetX, y + _offsetY, size, size);
}
