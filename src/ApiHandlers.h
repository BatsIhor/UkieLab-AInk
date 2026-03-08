#pragma once

#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include "RenderEngine.h"
#include "TextLayout.h"
#include "FramebufferManager.h"
#include "ZoneRegistry.h"
#include "CommandLog.h"
#include "EPD.h"

// Sets up all HTTP API endpoints for the AI Canvas.
class ApiHandlers {
public:
    struct Context {
        EPD* epd;
        FramebufferManager* fb;
        ZoneRegistry* zones;
        CommandLog* log;
        SemaphoreHandle_t renderMutex;
        TaskHandle_t displayTask;       // FreeRTOS task for EPD refresh
        volatile bool refreshRequested; // Signal to display task
        volatile bool refreshBusy;      // Display task is refreshing
        volatile bool docsRead;         // True after /mcp/tools.json or /openapi.json fetched
        uint32_t lastFrameId;
        uint32_t lastRenderTimeMs;
        uint32_t lastRefreshTimeMs;
        uint8_t consecutiveDryRuns;     // Circuit breaker: auto-render after 3
    };

    static void setup(AsyncWebServer& server, Context& ctx);

    // Display refresh task — runs on its own FreeRTOS task, never blocks async_tcp
    static void displayRefreshTask(void* param);

private:
    static void handleGetDevice(AsyncWebServerRequest* req, Context& ctx);
    static void handlePostCanvas(AsyncWebServerRequest* req, uint8_t* data, size_t len,
                                  size_t index, size_t total, Context& ctx);
    static void handleGetCanvas(AsyncWebServerRequest* req, Context& ctx);
    static void handleGetScreenshot(AsyncWebServerRequest* req, Context& ctx);
    static void handlePostClear(AsyncWebServerRequest* req, uint8_t* data, size_t len,
                                 size_t index, size_t total, Context& ctx);
    static void handlePostZones(AsyncWebServerRequest* req, uint8_t* data, size_t len,
                                 size_t index, size_t total, Context& ctx);
    static void handleGetZones(AsyncWebServerRequest* req, Context& ctx);
    static void handleDeleteZones(AsyncWebServerRequest* req, Context& ctx);
    static void handleGetHealth(AsyncWebServerRequest* req, Context& ctx);
    static void handlePostDeviceName(AsyncWebServerRequest* req, uint8_t* data, size_t len,
                                      size_t index, size_t total, Context& ctx);
    static void handlePostMeasure(AsyncWebServerRequest* req, uint8_t* data, size_t len,
                                   size_t index, size_t total, Context& ctx);
};
