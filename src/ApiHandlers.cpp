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
static String waveformTestBody;

// Binary image upload accumulator (raw bytes, not String)
static uint8_t* imageUploadBuf = nullptr;
static size_t imageUploadLen = 0;

// Image decode task parameters (shared between HTTP handler and decode task)
struct ImageDecodeTaskParams {
    ImageDecoder::BinaryDecodeParams decodeParams;
    ImageDecoder::DecodeResult result;
    FramebufferManager* fb;
    bool hadFrontBuffer;
    SemaphoreHandle_t doneSemaphore;
};

static const char* TEMP_IMG_PATH = "/tmp_img.png";

// Always decode directly from RAM. The upload buffer is the detached front buffer
// (~48KB) and the PNG decoder (~45KB) is heap-allocated. With ~47KB largest free
// block remaining after detach, both fit simultaneously for any upload size.
// SPIFFS fallback is kept but should never trigger under normal conditions.
static const size_t DIRECT_DECODE_THRESHOLD = FramebufferManager::BUFFER_SIZE + 1;

static void imageDecodeTask(void* param)
{
    ImageDecodeTaskParams* p = (ImageDecodeTaskParams*)param;
    size_t pngLen = p->decodeParams.pngLen;

    if (pngLen < DIRECT_DECODE_THRESHOLD) {
        // Small image: decode directly from RAM (upload buffer + PNG decoder both fit)
        Serial.printf("Image decode: direct from RAM (%u bytes)\n", pngLen);
        p->result = ImageDecoder::decodeBinary(p->decodeParams);
        free(const_cast<uint8_t*>(p->decodeParams.pngData));
        imageUploadBuf = nullptr;
    } else {
        // Large image: write to SPIFFS, free buffer, decode from file
        Serial.printf("Image decode: via SPIFFS (%u bytes)\n", pngLen);

        File f = SPIFFS.open(TEMP_IMG_PATH, "w");
        if (!f) {
            p->result = {false, 0, 0, "Failed to open temp file for write"};
            free(const_cast<uint8_t*>(p->decodeParams.pngData));
            imageUploadBuf = nullptr;
            xSemaphoreGive(p->doneSemaphore);
            vTaskDelete(nullptr);
            return;
        }
        size_t written = f.write(p->decodeParams.pngData, pngLen);
        f.close();
        Serial.printf("Image decode: wrote %u/%u bytes to SPIFFS\n", written, pngLen);

        // Free upload buffer — returns ~48KB to heap for PNG decoder
        free(const_cast<uint8_t*>(p->decodeParams.pngData));
        imageUploadBuf = nullptr;

        Serial.printf("Image decode: heap after free: %u, block: %u\n",
                       ESP.getFreeHeap(), ESP.getMaxAllocHeap());

        if (written != pngLen) {
            p->result = {false, 0, 0, "SPIFFS write incomplete"};
            SPIFFS.remove(TEMP_IMG_PATH);
            xSemaphoreGive(p->doneSemaphore);
            vTaskDelete(nullptr);
            return;
        }

        p->result = ImageDecoder::decodeFile(
            TEMP_IMG_PATH,
            p->decodeParams.destX, p->decodeParams.destY,
            p->decodeParams.destW, p->decodeParams.destH,
            p->decodeParams.dither,
            p->decodeParams.framebuffer,
            p->decodeParams.fbWidth, p->decodeParams.fbHeight
        );

        SPIFFS.remove(TEMP_IMG_PATH);
    }

    xSemaphoreGive(p->doneSemaphore);
    vTaskDelete(nullptr);
}

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

    // POST /canvas/bitmap - Raw 1-bit framebuffer upload (no decoding needed)
    // NOTE: Must be registered BEFORE /canvas to avoid prefix match collision
    server.on("/canvas/bitmap", HTTP_POST,
        [](AsyncWebServerRequest* req) {},
        nullptr,
        [&ctx](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total) {
            handlePostCanvasBitmap(req, data, len, index, total, ctx);
        }
    );

    // POST /canvas/waveform_test - Render a solid test pattern with tunable
    // controller-level waveform knobs (0x06 booster, 0x50 CDI, 0x82 VCOM_DC,
    // 0xe0 cascade, 0xe5 force-temp). Used to diagnose washed-out blacks on
    // weak-OTP panel variants.
    // NOTE: Must be registered BEFORE /canvas to avoid prefix match collision
    server.on("/canvas/waveform_test", HTTP_POST,
        [](AsyncWebServerRequest* req) {},
        nullptr,
        [&ctx](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total) {
            handlePostWaveformTest(req, data, len, index, total, ctx);
        }
    );

    // POST /canvas/image - Binary image upload (PNG body, no JSON/base64 overhead)
    // NOTE: Must be registered BEFORE /canvas to avoid prefix match collision
    server.on("/canvas/image", HTTP_POST,
        [](AsyncWebServerRequest* req) {},
        nullptr,
        [&ctx](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total) {
            handlePostCanvasImage(req, data, len, index, total, ctx);
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
    if (docSize > 131072) docSize = 131072;  // 128KB max — ESP32 has ~290KB headroom

    DynamicJsonDocument doc(docSize);
    DeserializationError err = deserializeJson(doc, bodyBuf);

    if (err) {
        free(bodyBuf);
        xSemaphoreGive(ctx.renderMutex);
        String errMsg;
        if (err == DeserializationError::NoMemory) {
            errMsg = "{\"error\":\"JSON too large for parser (" + String(bodyLen) +
                     " bytes payload, " + String(docSize) + " bytes parser limit). "
                     "Split into smaller batches using refresh:none, then send final batch with refresh:full.\"}";
        } else {
            errMsg = "{\"error\":\"Invalid JSON: " + String(err.c_str()) + "\"}";
        }
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
    bool mutexHandedOff = false;  // true when displayRefreshTask will release the mutex

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
            // GD7965: signal background task for non-blocking refresh.
            // The display task will release renderMutex after the SPI
            // transfer completes — do NOT release it here.
            ctx.refreshRequested = true;
            if (ctx.displayTask) {
                xTaskNotifyGive(ctx.displayTask);
                mutexHandedOff = true;
            }
#endif
        }
    }

    // Release the mutex only if we didn't hand it off to the display task.
    // When mutexHandedOff is true, displayRefreshTask releases it after
    // the EPD SPI transfer finishes, preventing buffer corruption.
    if (!mutexHandedOff) {
        xSemaphoreGive(ctx.renderMutex);
    }

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
    bool mutexHandedOff = false;
#ifdef EPD_PANEL_SSD1677
    ctx.refreshBusy = true;
    ctx.epd->display(false);
    ctx.refreshBusy = false;
#else
    ctx.refreshRequested = true;
    if (ctx.displayTask) {
        xTaskNotifyGive(ctx.displayTask);
        mutexHandedOff = true;
    }
#endif

    if (!mutexHandedOff) {
        xSemaphoreGive(ctx.renderMutex);
    }

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

// ============ Binary Image Upload ============
// POST /canvas/image — receives raw PNG bytes (no JSON, no base64)
// Query params: x, y, w, h, dither (threshold|ordered|floyd_steinberg), refresh (full|none)
// Memory: only the PNG binary + PNGdec decode buffer needed (~1x image size)
// For 800x480 display, a typical PNG is 20-100KB which fits comfortably in heap.

// Track whether we detached the front buffer for this upload
static bool imageUploadDetachedFB = false;

void ApiHandlers::handlePostCanvasImage(AsyncWebServerRequest* req, uint8_t* data, size_t len,
                                         size_t index, size_t total, Context& ctx)
{
    // First chunk: try to reuse front buffer memory as upload buffer
    if (index == 0) {
        // Max upload: front buffer size (48KB) fits most 1-bit PNGs of 800x480
        size_t maxUpload = FramebufferManager::BUFFER_SIZE;
        if (total > maxUpload) {
            String msg = "{\"error\":\"Image too large (" + String(total) +
                         " bytes). Max " + String(maxUpload) +
                         " bytes. Send a 1-bit PNG for smaller size.\"}";
            req->send(413, "application/json", msg);
            return;
        }
        // Detach front buffer — gives us its 48KB allocation without
        // fragmenting the heap (no free+malloc cycle).
        free(imageUploadBuf);
        imageUploadDetachedFB = ctx.fb->isDoubleBuffered();
        if (imageUploadDetachedFB) {
            imageUploadBuf = ctx.fb->detachFrontBuffer();
        } else {
            imageUploadBuf = (uint8_t*)malloc(total);
        }
        imageUploadLen = 0;
        if (!imageUploadBuf) {
            req->send(500, "application/json", "{\"error\":\"Out of memory for image upload\"}");
            return;
        }
    }

    // Accumulate binary data
    if (imageUploadBuf && index + len <= total) {
        memcpy(imageUploadBuf + index, data, len);
        imageUploadLen = index + len;
    }

    // Not all data received yet
    if (index + len != total) return;

    // All data received — decode and render
    if (!imageUploadBuf) {
        req->send(500, "application/json", "{\"error\":\"Upload buffer lost\"}");
        return;
    }

    // Try to acquire render mutex
    if (xSemaphoreTake(ctx.renderMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        free(imageUploadBuf);
        imageUploadBuf = nullptr;
        req->send(429, "application/json", "{\"error\":\"Display busy\"}");
        return;
    }

    // Parse query parameters
    int16_t destX = req->hasParam("x") ? req->getParam("x")->value().toInt() : 0;
    int16_t destY = req->hasParam("y") ? req->getParam("y")->value().toInt() : 0;
    int16_t destW = req->hasParam("w") ? req->getParam("w")->value().toInt() : 0;
    int16_t destH = req->hasParam("h") ? req->getParam("h")->value().toInt() : 0;
    String ditherStr = req->hasParam("dither") ? req->getParam("dither")->value() : "ordered";
    String refreshStr = req->hasParam("refresh") ? req->getParam("refresh")->value() : "full";

    DitherMode dither = DITHER_ORDERED;
    if (ditherStr == "threshold") dither = DITHER_THRESHOLD;
    else if (ditherStr == "floyd_steinberg") dither = DITHER_FLOYD_STEINBERG;

    // PNGdec needs ~4KB stack which exceeds async_tcp's stack.
    // Offload decode to a one-shot FreeRTOS task with 8KB stack.
    ImageDecodeTaskParams taskParams;
    taskParams.decodeParams.pngData = imageUploadBuf;
    taskParams.decodeParams.pngLen = imageUploadLen;
    taskParams.decodeParams.destX = destX;
    taskParams.decodeParams.destY = destY;
    taskParams.decodeParams.destW = destW;
    taskParams.decodeParams.destH = destH;
    taskParams.decodeParams.dither = dither;
    taskParams.decodeParams.framebuffer = ctx.fb->getBackBuffer();
    taskParams.decodeParams.fbWidth = EPD_WIDTH;
    taskParams.decodeParams.fbHeight = EPD_HEIGHT;
    taskParams.fb = ctx.fb;
    taskParams.hadFrontBuffer = false; // front buffer already detached in accumulation phase
    taskParams.doneSemaphore = xSemaphoreCreateBinary();

    if (!taskParams.doneSemaphore) {
        free(imageUploadBuf);
        imageUploadBuf = nullptr;
        xSemaphoreGive(ctx.renderMutex);
        req->send(500, "application/json", "{\"error\":\"Failed to create decode semaphore\"}");
        return;
    }

    BaseType_t created = xTaskCreatePinnedToCore(
        imageDecodeTask, "img_decode", 8192,
        &taskParams, 1, nullptr, 0  // core 0 (away from async_tcp on core 1)
    );

    if (created != pdPASS) {
        vSemaphoreDelete(taskParams.doneSemaphore);
        free(imageUploadBuf);
        imageUploadBuf = nullptr;
        if (imageUploadDetachedFB) {
            ctx.fb->reacquireFrontBuffer();
            imageUploadDetachedFB = false;
        }
        xSemaphoreGive(ctx.renderMutex);
        req->send(500, "application/json", "{\"error\":\"Failed to create decode task\"}");
        return;
    }

    // Wait for decode to complete (up to 30 seconds)
    xSemaphoreTake(taskParams.doneSemaphore, pdMS_TO_TICKS(30000));
    vSemaphoreDelete(taskParams.doneSemaphore);

    // Reacquire front buffer now that decode is done and upload buffer is freed
    if (imageUploadDetachedFB) {
        ctx.fb->reacquireFrontBuffer();
        imageUploadDetachedFB = false;
    }

    ImageDecoder::DecodeResult dr = taskParams.result;

    if (!dr.success) {
        xSemaphoreGive(ctx.renderMutex);
        req->send(400, "application/json", "{\"error\":\"" + dr.error + "\"}");
        return;
    }

    // Commit framebuffer
    FramebufferManager::DirtyRect dirty = ctx.fb->commit();

    // Log the frame
    ctx.log->beginFrame();
    ctx.log->addCommand("image");
    uint32_t frameId = ctx.log->endFrame();
    ctx.lastFrameId = frameId;

    // Refresh display
    bool skipRefresh = (refreshStr == "none");
    bool mutexHandedOff = false;
    if (!dirty.empty && !skipRefresh) {
#ifdef EPD_PANEL_SSD1677
        ctx.refreshBusy = true;
        unsigned long rstart = millis();
        ctx.epd->display(false);
        ctx.lastRefreshTimeMs = millis() - rstart;
        ctx.refreshBusy = false;
#else
        ctx.refreshRequested = true;
        if (ctx.displayTask) {
            xTaskNotifyGive(ctx.displayTask);
            mutexHandedOff = true;
        }
#endif
    }

    if (!mutexHandedOff) {
        xSemaphoreGive(ctx.renderMutex);
    }

    // Response
    DynamicJsonDocument respDoc(256);
    respDoc["status"] = "ok";
    respDoc["frame_id"] = frameId;
    respDoc["image_width"] = dr.width;
    respDoc["image_height"] = dr.height;
    String response;
    serializeJson(respDoc, response);
    req->send(200, "application/json", response);
}

// ============ Raw Bitmap Upload ============
// Accepts raw 1-bit framebuffer data (EPD_WIDTH * EPD_HEIGHT / 8 bytes).
// No PNG decoder needed — just memcpy into back buffer. Zero extra heap.

void ApiHandlers::handlePostCanvasBitmap(AsyncWebServerRequest* req, uint8_t* data, size_t len,
                                          size_t index, size_t total, Context& ctx)
{
    const size_t expectedSize = FramebufferManager::BUFFER_SIZE;

    // Validate total size on first chunk
    if (index == 0 && total != expectedSize) {
        String msg = "{\"error\":\"Expected " + String(expectedSize) +
                     " bytes (raw 1-bit " + String(EPD_WIDTH) + "x" + String(EPD_HEIGHT) +
                     "), got " + String(total) + "\"}";
        req->send(400, "application/json", msg);
        return;
    }

    // Copy each chunk directly into the back buffer — zero extra allocation
    uint8_t* backBuf = ctx.fb->getBackBuffer();
    if (backBuf && index + len <= expectedSize) {
        memcpy(backBuf + index, data, len);
    }

    // Not all data received yet
    if (index + len != total) return;

    // All data received — commit and refresh
    if (xSemaphoreTake(ctx.renderMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        req->send(429, "application/json", "{\"error\":\"Display busy\"}");
        return;
    }

    String refreshStr = req->hasParam("refresh") ? req->getParam("refresh")->value() : "full";

    FramebufferManager::DirtyRect dirty = ctx.fb->commit();

    ctx.log->beginFrame();
    ctx.log->addCommand("bitmap");
    uint32_t frameId = ctx.log->endFrame();
    ctx.lastFrameId = frameId;

    bool skipRefresh = (refreshStr == "none");
    bool mutexHandedOff = false;
    if (!dirty.empty && !skipRefresh) {
#ifdef EPD_PANEL_SSD1677
        ctx.refreshBusy = true;
        unsigned long rstart = millis();
        ctx.epd->display(false);
        ctx.lastRefreshTimeMs = millis() - rstart;
        ctx.refreshBusy = false;
#else
        ctx.refreshRequested = true;
        if (ctx.displayTask) {
            xTaskNotifyGive(ctx.displayTask);
            mutexHandedOff = true;
        }
#endif
    }

    if (!mutexHandedOff) {
        xSemaphoreGive(ctx.renderMutex);
    }

    DynamicJsonDocument respDoc(128);
    respDoc["status"] = "ok";
    respDoc["frame_id"] = frameId;
    String response;
    serializeJson(respDoc, response);
    req->send(200, "application/json", response);
}

// ============ Waveform Test Handler ============
// Renders a solid test pattern using a runtime-tunable waveform. Lets us
// iterate on controller-level knobs (booster 0x06, CDI 0x50, VCOM_DC 0x82,
// cascade 0xe0, force-temp 0xe5) without reflashing, to fix washed-out
// blacks on weak-OTP panel variants.
//
// JSON body (all fields optional; defaults preserve current behavior):
//   pattern: "black" | "white" | "half_h" | "half_v" | "vbars" | "quarters" | "checker"
//   panel: int (0x00 byte, default 31 = 0x1f)
//   booster_enabled: bool (default false)
//   booster: [b0,b1,b2,b3]   (default datasheet [0x17,0x17,0x28,0x17]; TRMNL GEN2 [0x27,0x27,0x18,0x17])
//   cdi: [b0,b1]             (default [0x29,0x07])
//   vcom_dc_enabled: bool, vcom_dc: int (default 0x26)
//   force_temp_enabled: bool, force_temp: int (default 0x5a = 90C)
//   cascade_enabled: bool, cascade: int (default 0x02)
static void fillWaveformPattern(uint8_t* buf, size_t size, const String& pattern)
{
    const int stride = EPD_WIDTH / 8;
    if (pattern == "black") {
        memset(buf, 0x00, size);
    } else if (pattern == "white") {
        memset(buf, 0xFF, size);
    } else if (pattern == "half_h") {
        // left half black, right half white
        for (int y = 0; y < EPD_HEIGHT; y++) {
            uint8_t* row = buf + y * stride;
            memset(row, 0x00, stride / 2);
            memset(row + stride / 2, 0xFF, stride - stride / 2);
        }
    } else if (pattern == "half_v") {
        // top half black, bottom half white
        memset(buf, 0x00, (size_t)(EPD_HEIGHT / 2) * stride);
        memset(buf + (size_t)(EPD_HEIGHT / 2) * stride, 0xFF,
               size - (size_t)(EPD_HEIGHT / 2) * stride);
    } else if (pattern == "vbars") {
        // 4 vertical bars: black, white, black, white
        for (int y = 0; y < EPD_HEIGHT; y++) {
            uint8_t* row = buf + y * stride;
            int q = stride / 4;
            memset(row,         0x00, q);
            memset(row + q,     0xFF, q);
            memset(row + 2 * q, 0x00, q);
            memset(row + 3 * q, 0xFF, stride - 3 * q);
        }
    } else if (pattern == "quarters") {
        // BW/WB quadrants
        int halfRows = EPD_HEIGHT / 2;
        int halfCols = stride / 2;
        for (int y = 0; y < EPD_HEIGHT; y++) {
            uint8_t* row = buf + y * stride;
            bool top = (y < halfRows);
            memset(row,            top ? 0x00 : 0xFF, halfCols);
            memset(row + halfCols, top ? 0xFF : 0x00, stride - halfCols);
        }
    } else if (pattern == "checker") {
        // 50px checkerboard — lets you see per-pixel black vs. white adjacency
        const int block = 50;
        for (int y = 0; y < EPD_HEIGHT; y++) {
            uint8_t* row = buf + y * stride;
            int yBlock = y / block;
            for (int xByte = 0; xByte < stride; xByte++) {
                // each byte = 8 px; block boundaries don't align with bytes
                // so compute each pixel
                uint8_t b = 0;
                for (int bit = 0; bit < 8; bit++) {
                    int x = xByte * 8 + bit;
                    int xBlock = x / block;
                    bool black = ((yBlock + xBlock) & 1) == 0;
                    if (!black) b |= (0x80 >> bit);
                }
                row[xByte] = b;
            }
        }
    } else {
        // Unknown pattern -> white
        memset(buf, 0xFF, size);
    }
}

void ApiHandlers::handlePostWaveformTest(AsyncWebServerRequest* req, uint8_t* data, size_t len,
                                          size_t index, size_t total, Context& ctx)
{
    if (index == 0) waveformTestBody = "";
    waveformTestBody += String((char*)data).substring(0, len);
    if (index + len != total) return;

    DynamicJsonDocument doc(1024);
    auto err = deserializeJson(doc, waveformTestBody);
    waveformTestBody = "";
    if (err) {
        req->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
        return;
    }

    if (xSemaphoreTake(ctx.renderMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        req->send(429, "application/json", "{\"error\":\"Display busy\"}");
        return;
    }

    // Build tuning: start from current, then apply any overrides in the body.
    WaveformTuning t = ctx.epd->getWaveformTuning();
    if (doc.containsKey("panel"))            t.panel_setting     = (uint8_t)doc["panel"].as<int>();
    if (doc.containsKey("booster_enabled"))  t.booster_enabled   = doc["booster_enabled"].as<bool>();
    if (doc["booster"].is<JsonArray>() && doc["booster"].size() == 4) {
        for (int i = 0; i < 4; i++) t.booster[i] = (uint8_t)doc["booster"][i].as<int>();
    }
    if (doc["cdi"].is<JsonArray>() && doc["cdi"].size() == 2) {
        t.cdi[0] = (uint8_t)doc["cdi"][0].as<int>();
        t.cdi[1] = (uint8_t)doc["cdi"][1].as<int>();
    }
    if (doc.containsKey("vcom_dc_enabled"))    t.vcom_dc_enabled    = doc["vcom_dc_enabled"].as<bool>();
    if (doc.containsKey("vcom_dc"))            t.vcom_dc            = (uint8_t)doc["vcom_dc"].as<int>();
    if (doc.containsKey("force_temp_enabled")) t.force_temp_enabled = doc["force_temp_enabled"].as<bool>();
    if (doc.containsKey("force_temp"))         t.force_temp         = (uint8_t)doc["force_temp"].as<int>();
    if (doc.containsKey("cascade_enabled"))    t.cascade_enabled    = doc["cascade_enabled"].as<bool>();
    if (doc.containsKey("cascade"))            t.cascade            = (uint8_t)doc["cascade"].as<int>();
    ctx.epd->setWaveformTuning(t);

    String pattern = doc["pattern"].isNull() ? String("black") : String(doc["pattern"].as<const char*>());

    // Fill back buffer with the requested pattern
    uint8_t* backBuf = ctx.fb->getBackBuffer();
    if (backBuf) {
        fillWaveformPattern(backBuf, FramebufferManager::BUFFER_SIZE, pattern);
    }
    ctx.fb->commit();

    ctx.log->beginFrame();
    ctx.log->addCommand("waveform_test");
    ctx.lastFrameId = ctx.log->endFrame();

    bool mutexHandedOff = false;
#ifdef EPD_PANEL_SSD1677
    ctx.refreshBusy = true;
    ctx.epd->display(false);
    ctx.refreshBusy = false;
#else
    ctx.refreshRequested = true;
    if (ctx.displayTask) {
        xTaskNotifyGive(ctx.displayTask);
        mutexHandedOff = true;
    }
#endif
    if (!mutexHandedOff) xSemaphoreGive(ctx.renderMutex);

    // Echo applied tuning
    DynamicJsonDocument resp(512);
    resp["status"]  = "ok";
    resp["pattern"] = pattern;
    resp["panel"]   = t.panel_setting;
    resp["booster_enabled"] = t.booster_enabled;
    JsonArray ba = resp.createNestedArray("booster");
    for (int i = 0; i < 4; i++) ba.add(t.booster[i]);
    JsonArray ca = resp.createNestedArray("cdi");
    ca.add(t.cdi[0]); ca.add(t.cdi[1]);
    resp["vcom_dc_enabled"]    = t.vcom_dc_enabled;
    resp["vcom_dc"]            = t.vcom_dc;
    resp["force_temp_enabled"] = t.force_temp_enabled;
    resp["force_temp"]         = t.force_temp;
    resp["cascade_enabled"]    = t.cascade_enabled;
    resp["cascade"]            = t.cascade;
    String out; serializeJson(resp, out);
    req->send(200, "application/json", out);
}

// ============ Display Refresh Task ============
// Runs on its own FreeRTOS task so EPD blocking never starves async_tcp.
// The render mutex is held by the HTTP handler that triggered the refresh
// and is released HERE after the EPD SPI transfer completes. This prevents
// a new request from modifying the shared _black buffer mid-transfer.

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

        // Release the render mutex that the HTTP handler held through the
        // SPI transfer. This is the critical fix: without this, the mutex
        // was released before the buffer was fully sent to the EPD, allowing
        // concurrent modification → washed-out / corrupted display.
        xSemaphoreGive(ctx->renderMutex);

        Serial.printf("Display task: refresh done (%ums)\n", elapsed);
    }
}

