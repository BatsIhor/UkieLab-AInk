#include "ApiHandlers.h"
#include "Settings.h"
#include "FontRegistry.h"
#include "Version.h"
#include <WiFi.h>
#include <ESPmDNS.h>
#include <SPIFFS.h>

// Static body accumulators for async request handling
static String canvasBody;
static String clearBody;
static String zonesBody;
static String deviceNameBody;
static String measureBody;

void ApiHandlers::setup(AsyncWebServer& server, Context& ctx)
{
    // GET /device - Device capabilities
    server.on("/device", HTTP_GET, [&ctx](AsyncWebServerRequest* req) {
        handleGetDevice(req, ctx);
    });

    // GET /canvas/screenshot - PBM image of current display
    // NOTE: Must be registered BEFORE /canvas to avoid prefix match collision
    server.on("/canvas/screenshot", HTTP_GET, [&ctx](AsyncWebServerRequest* req) {
        handleGetScreenshot(req, ctx);
    });

    // POST /canvas/clear - Clear display
    // NOTE: Must be registered BEFORE /canvas to avoid prefix match collision
    server.on("/canvas/clear", HTTP_POST,
        [](AsyncWebServerRequest* req) {},
        nullptr,
        [&ctx](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total) {
            handlePostClear(req, data, len, index, total, ctx);
        }
    );

    // POST /canvas - Render commands
    server.on("/canvas", HTTP_POST,
        [](AsyncWebServerRequest* req) {},  // onRequest (after body)
        nullptr,  // onUpload
        [&ctx](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total) {
            handlePostCanvas(req, data, len, index, total, ctx);
        }
    );

    // GET /canvas - Current frame info
    server.on("/canvas", HTTP_GET, [&ctx](AsyncWebServerRequest* req) {
        handleGetCanvas(req, ctx);
    });

    // POST /zones - Define zones
    server.on("/zones", HTTP_POST,
        [](AsyncWebServerRequest* req) {},
        nullptr,
        [&ctx](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total) {
            handlePostZones(req, data, len, index, total, ctx);
        }
    );

    // GET /zones
    server.on("/zones", HTTP_GET, [&ctx](AsyncWebServerRequest* req) {
        handleGetZones(req, ctx);
    });

    // DELETE /zones
    server.on("/zones", HTTP_DELETE, [&ctx](AsyncWebServerRequest* req) {
        handleDeleteZones(req, ctx);
    });

    // GET /health
    server.on("/health", HTTP_GET, [&ctx](AsyncWebServerRequest* req) {
        handleGetHealth(req, ctx);
    });

    // POST /device/name
    server.on("/device/name", HTTP_POST,
        [](AsyncWebServerRequest* req) {},
        nullptr,
        [&ctx](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total) {
            handlePostDeviceName(req, data, len, index, total, ctx);
        }
    );

    // POST /measure - Text measurement (no rendering)
    server.on("/measure", HTTP_POST,
        [](AsyncWebServerRequest* req) {},
        nullptr,
        [&ctx](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total) {
            handlePostMeasure(req, data, len, index, total, ctx);
        }
    );

    // Static spec files from SPIFFS — mark docs as read on access
    server.on("/openapi.json", HTTP_GET, [&ctx](AsyncWebServerRequest* req) {
        ctx.docsRead = true;
        req->send(SPIFFS, "/openapi.json", "application/json");
    });

    server.on("/mcp/tools.json", HTTP_GET, [&ctx](AsyncWebServerRequest* req) {
        ctx.docsRead = true;
        req->send(SPIFFS, "/mcp_tools.json", "application/json");
    });
}

void ApiHandlers::handleGetDevice(AsyncWebServerRequest* req, Context& ctx)
{
    DynamicJsonDocument doc(2048);

    doc["name"] = mySettings->canvasSettings.deviceName;
#ifdef EPD_PANEL_SSD1677
    doc["model"] = "aink-10.2";
#else
    doc["model"] = "aink-7.5";
#endif
    doc["version"] = VERSION;

    JsonObject display = doc.createNestedObject("display");
    display["width"] = EPD_WIDTH;
    display["height"] = EPD_HEIGHT;
    display["colorDepth"] = 1;
    display["colors"] = "bw";

    JsonObject network = doc.createNestedObject("network");
    network["ip"] = WiFi.localIP().toString();
    network["mac"] = WiFi.macAddress();
    network["rssi"] = WiFi.RSSI();

    doc["healthy"] = "unhealthy";
    doc["healthy_details"] = "Call GET /health for details";

    String baseUrl = "http://" + WiFi.localIP().toString();

    JsonObject api = doc.createNestedObject("api");
    api["openapi"] = baseUrl + "/openapi.json";
    api["mcp_tools"] = baseUrl + "/mcp/tools.json";

    doc["instructions"] = "STOP. You MUST fetch and read /mcp/tools.json (or /openapi.json) BEFORE sending "
                          "any drawing commands. The documentation contains the ONLY correct source of: available "
                          "operations, font names and sizes, text positioning rules, spacing formulas, and character "
                          "set limits (ASCII 0x20-0x7E only). Without reading it, your rendering WILL be incorrect. "
                          "Operations and fonts are intentionally not listed here to ensure you read the full "
                          "documentation first. Neither /device nor /health provide enough information to render "
                          "correctly -- you MUST read the full docs. "
                          "VALIDATION: You MUST pass \"validate\":true in every POST /canvas request. The response "
                          "will include a \"validation\" object with per-command bounding boxes, text_overlaps (pairs "
                          "of overlapping text commands), and off_screen warnings. After each render, you MUST check "
                          "text_overlaps and off_screen arrays -- if either is non-empty, adjust your layout and "
                          "re-render. Do NOT skip this step. Layout bugs (overlapping text, clipped content) are "
                          "the most common rendering mistakes and validation catches them automatically. "
                          "RECOMMENDED: Use \"dryRun\":true to validate layout WITHOUT drawing to the display. "
                          "Iterate with dryRun until text_overlaps and off_screen are both empty, then send "
                          "the final version without dryRun to actually render. This avoids multiple slow e-ink "
                          "refreshes while iterating on layout.";

    String response;
    serializeJson(doc, response);
    req->send(200, "application/json", response);
}

void ApiHandlers::handlePostCanvas(AsyncWebServerRequest* req, uint8_t* data, size_t len,
                                    size_t index, size_t total, Context& ctx)
{
    // Accumulate body
    if (index == 0) {
        if (total > 2 * 1024 * 1024) {
            req->send(413, "application/json", "{\"error\":\"Payload too large\"}");
            return;
        }
        canvasBody = "";
        canvasBody.reserve(total);
    }
    canvasBody += String((char*)data).substring(0, len);

    if (index + len != total) return;

    // Try to acquire render mutex
    if (xSemaphoreTake(ctx.renderMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        req->send(429, "application/json", "{\"error\":\"Display busy\"}");
        canvasBody = "";
        return;
    }

    // Move body to a char buffer so we can free the String before parsing.
    // ArduinoJson zero-copy mode: deserializing from char* avoids duplicate string allocations.
    size_t bodyLen = canvasBody.length();
    char* bodyBuf = (char*)malloc(bodyLen + 1);
    if (!bodyBuf) {
        xSemaphoreGive(ctx.renderMutex);
        req->send(500, "application/json", "{\"error\":\"Out of memory\"}");
        canvasBody = "";
        return;
    }
    memcpy(bodyBuf, canvasBody.c_str(), bodyLen + 1);
    canvasBody = ""; // Free String memory before allocating JSON doc

    // ArduinoJson 6 internal tree uses ~3-4x payload size for deeply nested structures
    // (polyline/polygon point arrays: each {"x":N,"y":N} object = ~48 bytes internal)
    size_t docSize = bodyLen * 4;
    if (docSize < 2048) docSize = 2048;
    if (docSize > 65536) docSize = 65536;

    DynamicJsonDocument doc(docSize);
    DeserializationError err = deserializeJson(doc, bodyBuf);

    if (err) {
        free(bodyBuf);
        xSemaphoreGive(ctx.renderMutex);
        String errMsg = "{\"error\":\"Invalid JSON: " + String(err.c_str()) + "\"}";
        req->send(400, "application/json", errMsg);
        return;
    }

    // Support both array at root and object with "commands" key
    JsonArray commands;
    String zone = "";
    String refreshStr = "full";
    String frameMeta = "";
    bool validate = req->hasParam("validate");
    bool dryRun = req->hasParam("dryRun");

    if (doc.is<JsonArray>()) {
        commands = doc.as<JsonArray>();
    } else if (doc.is<JsonObject>()) {
        JsonObject root = doc.as<JsonObject>();
        commands = root["commands"].as<JsonArray>();
        zone = root["zone"] | "";
        refreshStr = root["refresh"] | "full";
        if (root["validate"] | false) validate = true;
        if (root["dryRun"] | false) dryRun = true;
        if (root.containsKey("meta")) {
            DynamicJsonDocument metaDoc(512);
            metaDoc.set(root["meta"]);
            serializeJson(metaDoc, frameMeta);
        }
    }

    if (!commands) {
        free(bodyBuf);
        xSemaphoreGive(ctx.renderMutex);
        req->send(400, "application/json", "{\"error\":\"Expected commands array\"}");
        return;
    }

    // Store frame-level metadata if provided
    if (frameMeta.length() > 0) {
        ctx.log->setFrameMeta(frameMeta);
    }

    // Circuit breaker: after 3 consecutive dry-runs, auto-promote to real render
    static const uint8_t MAX_DRY_RUNS = 3;
    bool dryRunOverridden = false;
    if (dryRun) {
        ctx.consecutiveDryRuns++;
        if (ctx.consecutiveDryRuns > MAX_DRY_RUNS) {
            dryRun = false;
            dryRunOverridden = true;
            ctx.consecutiveDryRuns = 0;
        }
    } else {
        ctx.consecutiveDryRuns = 0;
    }

    // Execute rendering (draws to EPD buffer which IS the framebuffer back buffer)
    // In dryRun mode: computes bounding boxes without drawing pixels
    if (dryRun) validate = true; // dryRun implies validate
    RenderEngine engine(*ctx.epd, *ctx.fb, *ctx.zones, *ctx.log);
    RenderEngine::RenderResult renderResult = engine.execute(commands, zone, validate, dryRun);

    // Free parse buffer and JSON doc (zero-copy strings no longer needed after render)
    free(bodyBuf);
    doc.clear();
    doc.shrinkToFit();

    uint32_t frameId = 0;

    if (!dryRun) {
        // Commit framebuffer (compute dirty rect, copy back→front)
        FramebufferManager::DirtyRect dirty = ctx.fb->commit();

        // Update context
        frameId = ctx.log->endFrame();
        ctx.lastFrameId = frameId;
        ctx.lastRenderTimeMs = renderResult.renderTimeMs;

        // Refresh the display
        // refresh:"none" skips refresh — use for batched rendering (send multiple
        // small requests, then a final one with refresh:"full" to flush to screen)
        bool skipRefresh = (refreshStr == "none");
        if (!dirty.empty && !skipRefresh) {
#ifdef EPD_PANEL_SSD1677
            // SSD1677: refresh synchronously (same path as startup screen)
            ctx.refreshBusy = true;
            unsigned long rstart = millis();
            ctx.epd->display(false);
            ctx.lastRefreshTimeMs = millis() - rstart;
            ctx.refreshBusy = false;
#else
            // GD7965: signal background task for non-blocking refresh
            ctx.refreshRequested = true;
            if (ctx.displayTask) {
                xTaskNotifyGive(ctx.displayTask);
            }
#endif
        }
    }

    xSemaphoreGive(ctx.renderMutex);

    // Build response (returned immediately, display refreshes in background)
    size_t respSize = validate ? 8192 : 1024;
    DynamicJsonDocument respDoc(respSize);
    if (dryRunOverridden) {
        respDoc["status"] = "ok";
        respDoc["dry_run_overridden"] = true;
    } else {
        respDoc["status"] = dryRun ? "dry_run" : "ok";
    }
    if (!dryRun) respDoc["frame_id"] = frameId;
    respDoc["render_time_ms"] = renderResult.renderTimeMs;
    respDoc["commands_executed"] = renderResult.commandsExecuted;
    if (dryRun) {
        respDoc["dry_runs_remaining"] = MAX_DRY_RUNS - ctx.consecutiveDryRuns;
    }

    if (renderResult.warnings.size() > 0) {
        JsonArray warnings = respDoc.createNestedArray("warnings");
        for (const String& w : renderResult.warnings) {
            warnings.add(w);
        }
    }

    // Validation: per-command bounding boxes, overlap and off-screen detection
    if (validate && renderResult.bboxes.size() > 0) {
        JsonObject validation = respDoc.createNestedObject("validation");

        // Per-command bounding boxes
        JsonArray cmds = validation.createNestedArray("commands");
        for (size_t i = 0; i < renderResult.bboxes.size(); i++) {
            const auto& bb = renderResult.bboxes[i];
            JsonObject entry = cmds.createNestedObject();
            JsonArray bbox = entry.createNestedArray("bbox");
            bbox.add(bb.x); bbox.add(bb.y); bbox.add(bb.w); bbox.add(bb.h);
            if (bb.offScreen) entry["off_screen"] = true;
            if (bb.textLines > 0) {
                entry["lines"] = bb.textLines;
                if (bb.textTruncated) entry["truncated"] = true;
            }
        }

        // Detect text-text overlaps (most common layout bug)
        JsonArray overlaps = validation.createNestedArray("text_overlaps");
        for (size_t i = 0; i < renderResult.bboxes.size(); i++) {
            const auto& a = renderResult.bboxes[i];
            if (a.textLines == 0) continue; // not a text command
            for (size_t j = i + 1; j < renderResult.bboxes.size(); j++) {
                const auto& b = renderResult.bboxes[j];
                if (b.textLines == 0) continue;
                // Check intersection
                if (a.x < b.x + b.w && b.x < a.x + a.w &&
                    a.y < b.y + b.h && b.y < a.y + a.h) {
                    JsonArray pair = overlaps.createNestedArray();
                    pair.add(i); pair.add(j);
                }
            }
        }

        // List commands with off-screen content
        JsonArray offScreen = validation.createNestedArray("off_screen");
        for (size_t i = 0; i < renderResult.bboxes.size(); i++) {
            if (renderResult.bboxes[i].offScreen) offScreen.add(i);
        }
    }

    String response;
    serializeJson(respDoc, response);
    req->send(200, "application/json", response);
}

void ApiHandlers::handleGetCanvas(AsyncWebServerRequest* req, Context& ctx)
{
    DynamicJsonDocument doc(4096);
    JsonObject root = doc.to<JsonObject>();
    ctx.log->toJson(root);
    root["render_time_ms"] = ctx.lastRenderTimeMs;
    root["refresh_time_ms"] = ctx.lastRefreshTimeMs;

    String response;
    serializeJson(doc, response);
    req->send(200, "application/json", response);
}

void ApiHandlers::handleGetScreenshot(AsyncWebServerRequest* req, Context& ctx)
{
    // Output as PBM (Portable Bitmap) - simple, no compression needed
    // Format: P4 (binary PBM)
    const uint8_t* fb = ctx.fb->getFrontBuffer();
    if (!fb) {
        req->send(500, "text/plain", "No framebuffer");
        return;
    }

    String header = "P4\n" + String(EPD_WIDTH) + " " + String(EPD_HEIGHT) + "\n";
    size_t headerLen = header.length();
    size_t dataLen = EPD_BUF_SIZE;
    size_t totalLen = headerLen + dataLen;

    AsyncWebServerResponse* response = req->beginResponse("image/x-portable-bitmap", totalLen,
        [fb, header, headerLen, dataLen](uint8_t* buffer, size_t maxLen, size_t index) -> size_t {
            size_t written = 0;

            // Write header
            if (index < headerLen) {
                size_t hLen = min(maxLen, headerLen - index);
                memcpy(buffer, header.c_str() + index, hLen);
                written += hLen;
                index += hLen;
                maxLen -= hLen;
            }

            // Write pixel data (PBM P4: 1=black, 0=white, MSB first)
            // EPD buffer: 1=white, 0=black -- so we invert
            if (index >= headerLen && maxLen > 0) {
                size_t dataOffset = index - headerLen;
                size_t dLen = min(maxLen, dataLen - dataOffset);
                for (size_t i = 0; i < dLen; i++) {
                    buffer[written + i] = ~fb[dataOffset + i];
                }
                written += dLen;
            }

            return written;
        }
    );

    response->addHeader("Content-Disposition", "inline; filename=\"screenshot.pbm\"");
    req->send(response);
}

void ApiHandlers::handlePostClear(AsyncWebServerRequest* req, uint8_t* data, size_t len,
                                   size_t index, size_t total, Context& ctx)
{
    if (index == 0) clearBody = "";
    clearBody += String((char*)data).substring(0, len);
    if (index + len != total) return;

    if (xSemaphoreTake(ctx.renderMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        req->send(429, "application/json", "{\"error\":\"Display busy\"}");
        clearBody = "";
        return;
    }

    bool black = false;
    if (clearBody.length() > 0) {
        DynamicJsonDocument doc(256);
        if (deserializeJson(doc, clearBody) == DeserializationError::Ok) {
            black = RenderEngine::resolveColor(doc["color"], false);
        }
    }
    clearBody = "";

    ctx.fb->clear(!black);
    ctx.fb->swapAfterFullRefresh();

    ctx.log->beginFrame();
    ctx.log->addCommand("clear");
    ctx.lastFrameId = ctx.log->endFrame();

    // Refresh display
#ifdef EPD_PANEL_SSD1677
    ctx.refreshBusy = true;
    ctx.epd->display(false);
    ctx.refreshBusy = false;
#else
    ctx.refreshRequested = true;
    if (ctx.displayTask) {
        xTaskNotifyGive(ctx.displayTask);
    }
#endif

    xSemaphoreGive(ctx.renderMutex);

    req->send(200, "application/json", "{\"status\":\"ok\"}");
}

void ApiHandlers::handlePostZones(AsyncWebServerRequest* req, uint8_t* data, size_t len,
                                   size_t index, size_t total, Context& ctx)
{
    if (index == 0) zonesBody = "";
    zonesBody += String((char*)data).substring(0, len);
    if (index + len != total) return;

    DynamicJsonDocument doc(2048);
    if (deserializeJson(doc, zonesBody) != DeserializationError::Ok) {
        req->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
        zonesBody = "";
        return;
    }
    zonesBody = "";

    JsonArray zones = doc.as<JsonArray>();
    if (!zones) {
        // Maybe it's an object with "zones" key
        if (doc.is<JsonObject>()) {
            zones = doc["zones"].as<JsonArray>();
        }
    }

    if (zones) {
        ctx.zones->fromJson(zones);
        req->send(200, "application/json", "{\"status\":\"ok\",\"count\":" + String(ctx.zones->count()) + "}");
    } else {
        req->send(400, "application/json", "{\"error\":\"Expected zones array\"}");
    }
}

void ApiHandlers::handleGetZones(AsyncWebServerRequest* req, Context& ctx)
{
    DynamicJsonDocument doc(2048);
    JsonArray arr = doc.to<JsonArray>();
    ctx.zones->toJson(arr);

    String response;
    serializeJson(doc, response);
    req->send(200, "application/json", response);
}

void ApiHandlers::handleDeleteZones(AsyncWebServerRequest* req, Context& ctx)
{
    ctx.zones->clearAll();
    req->send(200, "application/json", "{\"status\":\"ok\"}");
}

void ApiHandlers::handleGetHealth(AsyncWebServerRequest* req, Context& ctx)
{
    DynamicJsonDocument doc(1024);

    doc["uptime_ms"] = millis();
    doc["free_heap"] = ESP.getFreeHeap();
    doc["min_free_heap"] = ESP.getMinFreeHeap();
    doc["largest_free_block"] = ESP.getMaxAllocHeap();
    doc["wifi_rssi"] = WiFi.RSSI();
    doc["wifi_ssid"] = WiFi.SSID();
    doc["ip"] = WiFi.localIP().toString();
    doc["frame_id"] = ctx.lastFrameId;
    doc["display_busy"] = ctx.refreshBusy;

    if (ctx.docsRead) {
        doc["status"] = "ok";
    } else {
        doc["status"] = "not_ready";

        String baseUrl = "http://" + WiFi.localIP().toString();
        doc["action_required"] = "You MUST fetch and read the API documentation before interacting "
                                 "with this device. GET " + baseUrl + "/mcp/tools.json or GET "
                                 + baseUrl + "/openapi.json first. Health will report 'ok' once "
                                 "documentation has been read.";
    }

    String response;
    serializeJson(doc, response);
    req->send(200, "application/json", response);
}

void ApiHandlers::handlePostDeviceName(AsyncWebServerRequest* req, uint8_t* data, size_t len,
                                        size_t index, size_t total, Context& ctx)
{
    if (index == 0) deviceNameBody = "";
    deviceNameBody += String((char*)data).substring(0, len);
    if (index + len != total) return;

    DynamicJsonDocument doc(256);
    if (deserializeJson(doc, deviceNameBody) != DeserializationError::Ok) {
        req->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
        deviceNameBody = "";
        return;
    }
    deviceNameBody = "";

    String name = doc["name"] | "";
    if (name.length() == 0) {
        req->send(400, "application/json", "{\"error\":\"Missing name\"}");
        return;
    }

    mySettings->canvasSettings.deviceName = name;
    mySettings->saveSettings(true);

    // Update mDNS
    MDNS.end();
    MDNS.begin(name.c_str());
    MDNS.addService("_aiscreen", "_tcp", 80);
    MDNS.addService("http", "tcp", 80);

    req->send(200, "application/json", "{\"status\":\"ok\",\"name\":\"" + name + "\"}");
}

// ============ Text Measurement ============

void ApiHandlers::handlePostMeasure(AsyncWebServerRequest* req, uint8_t* data, size_t len,
                                     size_t index, size_t total, Context& ctx)
{
    if (index == 0) {
        if (total > 32768) {
            req->send(413, "application/json", "{\"error\":\"Payload too large\"}");
            return;
        }
        measureBody = "";
        measureBody.reserve(total);
    }
    measureBody += String((char*)data).substring(0, len);
    if (index + len != total) return;

    DynamicJsonDocument doc(4096);
    DeserializationError err = deserializeJson(doc, measureBody);
    measureBody = "";

    if (err) {
        req->send(400, "application/json", "{\"error\":\"Invalid JSON: " + String(err.c_str()) + "\"}");
        return;
    }

    // Acquire render mutex briefly (measureString uses EPD::setFont/getTextBounds)
    if (xSemaphoreTake(ctx.renderMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        req->send(429, "application/json", "{\"error\":\"Display busy\"}");
        return;
    }

    bool isArray = doc.is<JsonArray>();
    DynamicJsonDocument respDoc(2048);

    auto measureOne = [&](const JsonObject& item) -> JsonObject {
        JsonObject result = isArray ? respDoc.createNestedObject() : respDoc.to<JsonObject>();

        String text = item["text"] | "";
        String family = item["font"] | "sans";
        int size = item["size"] | 12;
        bool bold = item["bold"] | false;
        int16_t maxWidth = item["maxWidth"] | 0;
        float lineHeight = item["lineHeight"] | 1.2f;

        const GFXfont* font = FontRegistry::findClosest(family.c_str(), size, bold);
        if (!font) {
            result["error"] = "No font found";
            return result;
        }

        TextLayout::LayoutParams params;
        params.maxWidth = maxWidth;
        params.lineHeight = lineHeight;
        params.font = font;

        TextLayout::LayoutResult meas = TextLayout::measure(*ctx.epd, text, params);

        result["width"] = meas.width;
        result["height"] = meas.height;
        result["lines"] = meas.lineCount;
        result["truncated"] = meas.truncated;
        result["yAdvance"] = font->yAdvance;
        result["lineHeight"] = (int)(font->yAdvance * lineHeight);
        return result;
    };

    if (isArray) {
        // Convert respDoc to array
        respDoc.to<JsonArray>();
        JsonArray arr = doc.as<JsonArray>();
        for (size_t i = 0; i < arr.size(); i++) {
            JsonObject item = arr[i];
            measureOne(item);
        }
    } else {
        measureOne(doc.as<JsonObject>());
    }

    xSemaphoreGive(ctx.renderMutex);

    String response;
    serializeJson(respDoc, response);
    req->send(200, "application/json", response);
}

// ============ Display Refresh Task ============
// Runs on its own FreeRTOS task so EPD blocking never starves async_tcp.

void ApiHandlers::displayRefreshTask(void* param)
{
    Context* ctx = (Context*)param;
    Serial.println("Display refresh task started");

    for (;;) {
        // Wait for notification from HTTP handlers (block indefinitely)
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        if (!ctx->refreshRequested) continue;
        ctx->refreshRequested = false;
        ctx->refreshBusy = true;

        Serial.println("Display task: starting EPD refresh");
        unsigned long start = millis();

        ctx->epd->display(false);

        uint32_t elapsed = millis() - start;
        ctx->lastRefreshTimeMs = elapsed;
        ctx->refreshBusy = false;
        Serial.printf("Display task: refresh done (%ums)\n", elapsed);
    }
}
