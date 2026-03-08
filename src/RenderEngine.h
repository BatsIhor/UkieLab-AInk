#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <vector>
#include "EPD.h"
#include "FramebufferManager.h"
#include "FontRegistry.h"
#include "TextLayout.h"
#include "ImageDecoder.h"
#include "ZoneRegistry.h"
#include "CommandLog.h"

// Parses JSON command arrays and executes drawing operations on the framebuffer.
// Graceful error handling: never refuses to render; clamps coords, falls back fonts.
class RenderEngine {
public:
    struct CommandBBox {
        int16_t x, y, w, h;
        bool offScreen;
        int16_t textLines;      // 0 for non-text ops
        bool textTruncated;
    };

    struct RenderResult {
        int commandsExecuted;
        uint32_t renderTimeMs;
        std::vector<String> warnings;
        std::vector<CommandBBox> bboxes;
        bool trackBBoxes = false;
    };

    RenderEngine(EPD& epd, FramebufferManager& fb, ZoneRegistry& zones, CommandLog& log);

    // Execute a JSON array of drawing commands
    // validate: return per-command bounding boxes and overlap detection
    // dryRun: validate without drawing pixels or touching the display
    RenderResult execute(const JsonArray& commands, const String& zone = "", bool validate = false, bool dryRun = false);

    // Resolve color name/number to B/W (black=true, white=false for "is ink")
    static bool resolveColor(const JsonVariant& color, bool defaultBlack = true);

private:
    EPD& _epd;
    FramebufferManager& _fb;
    ZoneRegistry& _zones;
    CommandLog& _log;

    // Clip rect state
    struct ClipRect {
        int16_t x, y, w, h;
        bool active;
    };
    ClipRect _clip;

    // Zone offset
    int16_t _offsetX, _offsetY;
    int16_t _zoneW, _zoneH;

    // Dry-run mode: compute bboxes without drawing pixels
    bool _dryRun;

    // Execute individual operations
    void execClear(const JsonObject& cmd, RenderResult& result);
    void execPixel(const JsonObject& cmd, RenderResult& result);
    void execLine(const JsonObject& cmd, RenderResult& result);
    void execRect(const JsonObject& cmd, RenderResult& result);
    void execCircle(const JsonObject& cmd, RenderResult& result);
    void execEllipse(const JsonObject& cmd, RenderResult& result);
    void execArc(const JsonObject& cmd, RenderResult& result);
    void execPolygon(const JsonObject& cmd, RenderResult& result);
    void execPolyline(const JsonObject& cmd, RenderResult& result);
    void execText(const JsonObject& cmd, RenderResult& result);
    void execImage(const JsonObject& cmd, RenderResult& result);
    void execGradient(const JsonObject& cmd, RenderResult& result);
    void execFloodFill(const JsonObject& cmd, RenderResult& result);
    void execClip(const JsonObject& cmd, RenderResult& result);
    void execUnclip(const JsonObject& cmd, RenderResult& result);
    void execRawBitmap(const JsonObject& cmd, RenderResult& result);
    void execQrCode(const JsonObject& cmd, RenderResult& result);

    // Helper: set pixel with clipping and zone offset
    void setPixel(int16_t x, int16_t y, bool black);
    bool isClipped(int16_t x, int16_t y) const;

    // Helper: push bounding box for validation
    void pushBBox(RenderResult& result, int16_t x, int16_t y, int16_t w, int16_t h,
                  int16_t textLines = 0, bool textTruncated = false);
};
