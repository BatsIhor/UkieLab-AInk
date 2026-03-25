#ifdef AI_CANVAS

#include <Arduino.h>
#include <WiFi.h>
#include <SPIFFS.h>
#include <ESPAsyncWebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Update.h>
#include <esp_wifi.h>
#include <esp_sleep.h>
#include <Preferences.h>
#include <time.h>

#include "Settings.h"
#include "EPD.h"
#include "src/FontRegistry.h"
#include "src/FramebufferManager.h"
#include "src/RenderEngine.h"
#include "src/ZoneRegistry.h"
#include "src/CommandLog.h"
#include "src/ApiHandlers.h"

#include "Version.h"
#include "src/OTAUpdateManager.h"
#include <qrcode.h>

// Multi-reset detector for factory reset (triple press)
#define ESP_MRD_USE_SPIFFS true
#define ESP_MRD_USE_EEPROM false
#define MULTIRESETDETECTOR_DEBUG true
#define MRD_TIMES 3
#define MRD_TIMEOUT 10
#define MRD_ADDRESS 0
#include <ESP_MultiResetDetector.h>
MultiResetDetector* mrd = nullptr;

// Hardware pins
static const uint8_t EPD_BUSY = 4;
static const uint8_t EPD_CS   = 5;
static const uint8_t EPD_RST  = 16;
static const uint8_t EPD_DC   = 17;
static const uint8_t EPD_SCK  = 18;
static const uint8_t EPD_MISO = 19;
static const uint8_t EPD_MOSI = 23;

// Global objects
EPD* epd = nullptr;
FramebufferManager framebuffer;
ZoneRegistry zones;
CommandLog commandLog;
AsyncWebServer server(80);
DNSServer dnsServer;
OTAUpdateManager otaManager;
SemaphoreHandle_t renderMutex = nullptr;
ApiHandlers::Context apiCtx;

volatile bool otaTaskRunning = false;
bool wifiConnected = false;
bool shouldRedirectToWifi = false;
unsigned long setupModeStartTime = 0;
const unsigned long SETUP_INACTIVITY_TIMEOUT = 5 * 60 * 1000;

// NVS namespace for auto-update settings (survives OTA firmware updates)
static const char* NVS_NAMESPACE  = "aink_cfg";
static const char* NVS_KEY_AUTO   = "auto_upd";
static const char* NVS_KEY_LAST   = "last_day";

// Auto-update state (loaded from NVS at boot)
bool autoUpdateEnabled = false;
static unsigned long lastAutoUpdatePoll = 0;  // millis throttle (every 60s)

// OTA check result (written by task, read by HTTP handler)
struct OTACheckResult {
    bool checked   = false;
    bool hasUpdate = false;
    String latestVersion;
    String releaseName;
    String publishedAt;
    String firmwareUrl;
    String spiffsUrl;
    String error;
};
OTACheckResult otaCheckResult;

// ============ Captive Portal ============

bool isCaptivePortalRequest(AsyncWebServerRequest* req) {
    String host = req->host();
    String url = req->url();

    if (url.indexOf("generate_204") >= 0 || url.indexOf("gen_204") >= 0) return true;
    if (url.indexOf("hotspot-detect") >= 0 || url.indexOf("captive.apple.com") >= 0) return true;
    if (url.indexOf("ncsi.txt") >= 0 || url.indexOf("connecttest") >= 0) return true;
    if (url.indexOf("detectportal") >= 0) return true;
    if (host != "192.168.4.1" && host != WiFi.softAPIP().toString()) return true;

    return false;
}

void handleCaptivePortal(AsyncWebServerRequest* req) {
    req->redirect("http://192.168.4.1/");
}

// ============ Display Init ============

void initDisplay() {
    Serial.println("Initializing display...");
    epd = new EPD(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY);
    epd->init(115200, false, 10, false);  // 10ms reset pulse (was 2ms)
    SPI.end();
    SPI.begin(EPD_SCK, EPD_MISO, EPD_MOSI, EPD_CS);
    Serial.println("EPD SPI initialized, panel ready");
    epd->fillScreen(GxEPD_WHITE);
    epd->setTextColor(GxEPD_BLACK);
    Serial.printf("Display initialized. Free heap: %d\n", ESP.getFreeHeap());
}

// Forward declaration (drawQrToEpd defined below showStartupScreen)
void drawQrToEpd(const char* data, int16_t x, int16_t y, int16_t size);

void showStartupScreen(const String& ipAddress) {
    if (!epd || !epd->buffersValid()) {
        Serial.println("showStartupScreen: EPD not ready!");
        return;
    }

    Serial.println("Drawing startup screen...");
    epd->fillScreen(GxEPD_WHITE);

    const GFXfont* titleFont = FontRegistry::findClosest("sans", 24, true);
    const GFXfont* bigFont = FontRegistry::findClosest("sans", 18, true);
    const GFXfont* bodyFont = FontRegistry::findClosest("sans", 12, false);
    const GFXfont* boldFont = FontRegistry::findClosest("sans", 12, true);
    const GFXfont* captionFont = FontRegistry::findClosest("sans", 9, false);
    const GFXfont* captionBoldFont = FontRegistry::findClosest("sans", 9, true);

    // Header bar
    epd->fillRect(0, 0, EPD_WIDTH, 46, GxEPD_BLACK);
    if (titleFont) {
        epd->setFont(titleFont);
        epd->setTextColor(GxEPD_WHITE);
        epd->setCursor(20, 42);
        epd->print("AInk Ready");
    }

    // ---- Left side: instructions ----
    int leftX = 30;
    int midX = EPD_WIDTH / 2;

    if (bigFont) {
        epd->setFont(bigFont);
        epd->setTextColor(GxEPD_BLACK);
        epd->setCursor(leftX, 95);
        epd->print("Ask your AI to:");
    }

    if (bodyFont) {
        epd->setFont(bodyFont);
        epd->setTextColor(GxEPD_BLACK);

        epd->setCursor(leftX, 140);
        epd->print("Discover device at");
    }
    if (boldFont) {
        epd->setFont(boldFont);
        epd->setCursor(leftX, 170);
        epd->print("http://" + ipAddress);
    }

    if (bodyFont) {
        epd->setFont(bodyFont);
        epd->setCursor(leftX, 215);
        epd->print("Read instructions on");
        epd->setCursor(leftX, 245);
        epd->print("how to use the device");
    }

    if (bodyFont) {
        epd->setFont(bodyFont);
        epd->setCursor(leftX, 290);
        epd->print("And show me something useful");
    }

    // Divider line
    epd->drawLine(leftX, 320, midX - 30, 320, GxEPD_BLACK);

    if (captionFont) {
        epd->setFont(captionFont);
        epd->setTextColor(GxEPD_BLACK);
        epd->setCursor(leftX, 345);
        epd->print("Or scan the QR code to get a");
        epd->setCursor(leftX, 367);
        epd->print("ready-to-use prompt for Claude,");
        epd->setCursor(leftX, 389);
        epd->print("ChatGPT, or any AI assistant.");
    }

    // Vertical divider
    epd->drawLine(midX, 60, midX, EPD_HEIGHT - 40, GxEPD_BLACK);

    // ---- Right side: QR code ----
    int rightCx = midX + (EPD_WIDTH - midX) / 2;
    int qrSize = 180;

    if (bigFont) {
        epd->setFont(bigFont);
        epd->setTextColor(GxEPD_BLACK);
        // "Quick Start" ~11 chars * ~11px = ~121px wide
        int16_t tx, ty; uint16_t tw, th;
        epd->getTextBounds("Quick Start", 0, 0, &tx, &ty, &tw, &th);
        epd->setCursor(rightCx - tw / 2, 95);
        epd->print("Quick Start");
    }

    // QR code pointing to device welcome page — centered in right column
    String qrUrl = "http://" + ipAddress + "/start";
    drawQrToEpd(qrUrl.c_str(), rightCx - qrSize / 2, 120, qrSize);

    if (captionBoldFont) {
        epd->setFont(captionBoldFont);
        epd->setTextColor(GxEPD_BLACK);
        int16_t tx, ty; uint16_t tw, th;
        epd->getTextBounds("Scan for AI prompt", 0, 0, &tx, &ty, &tw, &th);
        epd->setCursor(rightCx - tw / 2, 310);
        epd->print("Scan for AI prompt");
    }

    if (bodyFont) {
        epd->setFont(bodyFont);
        int16_t tx, ty; uint16_t tw, th;
        epd->getTextBounds("or open in browser:", 0, 0, &tx, &ty, &tw, &th);
        epd->setCursor(rightCx - tw / 2, 345);
        epd->print("or open in browser:");
    }
    if (boldFont) {
        epd->setFont(boldFont);
        String startUrl = ipAddress + "/start";
        int16_t tx, ty; uint16_t tw, th;
        epd->getTextBounds(startUrl.c_str(), 0, 0, &tx, &ty, &tw, &th);
        epd->setCursor(rightCx - tw / 2, 375);
        epd->print(startUrl);
    }

    // Bottom bar
    int footerY = EPD_HEIGHT - 22;
    epd->fillRect(0, footerY, EPD_WIDTH, 22, GxEPD_BLACK);
    if (captionFont) {
        epd->setFont(captionFont);
        epd->setTextColor(GxEPD_WHITE);
        String ver = VERSION;
        epd->setCursor(20, footerY + 14);
        epd->print("AInk v" + ver + "  |  ukielab.com");
    }

    Serial.println("Sending to display...");
    epd->display(false);
    Serial.println("Startup screen refresh triggered (non-blocking)");
    framebuffer.swapAfterFullRefresh();
}

// Draw a QR code directly to the EPD buffer at (x,y) with given pixel size
void drawQrToEpd(const char* data, int16_t x, int16_t y, int16_t size) {
    if (!epd) return;

    // Try versions 1-10 to find one that fits
    QRCode qrcode;
    uint8_t* qrcodeData = nullptr;
    bool success = false;

    static const int byteCapacity[] = {0, 14, 26, 42, 62, 84, 106, 122, 152, 180, 213};
    int dataLen = strlen(data);
    int minVersion = 1;
    for (int v = 1; v <= 10; v++) {
        if (byteCapacity[v] >= dataLen) { minVersion = v; break; }
        if (v == 10) minVersion = 10;
    }

    for (int v = minVersion; v <= 10; v++) {
        size_t bufSize = qrcode_getBufferSize(v);
        qrcodeData = (uint8_t*)malloc(bufSize);
        if (!qrcodeData) return;
        if (qrcode_initText(&qrcode, qrcodeData, v, ECC_MEDIUM, data) == 0) {
            success = true;
            break;
        }
        free(qrcodeData);
        qrcodeData = nullptr;
    }
    if (!success) return;

    int modules = qrcode.size;
    int moduleSize = size / (modules + 4);
    if (moduleSize < 1) moduleSize = 1;
    int actualSize = moduleSize * (modules + 4);
    int ox = x + (size - actualSize) / 2;
    int oy = y + (size - actualSize) / 2;
    int qz = 2 * moduleSize;

    // White background
    epd->fillRect(x, y, size, size, GxEPD_WHITE);

    // Draw modules
    for (int my = 0; my < modules; my++) {
        for (int mx = 0; mx < modules; mx++) {
            if (qrcode_getModule(&qrcode, mx, my)) {
                epd->fillRect(ox + qz + mx * moduleSize,
                              oy + qz + my * moduleSize,
                              moduleSize, moduleSize, GxEPD_BLACK);
            }
        }
    }

    free(qrcodeData);
}

void showSetupScreen() {
    if (!epd || !epd->buffersValid()) {
        Serial.println("showSetupScreen: EPD not ready!");
        return;
    }

    Serial.println("Drawing setup screen...");
    epd->fillScreen(GxEPD_WHITE);

    const GFXfont* titleFont = FontRegistry::findClosest("sans", 24, true);
    const GFXfont* bigFont = FontRegistry::findClosest("sans", 18, true);
    const GFXfont* bodyFont = FontRegistry::findClosest("sans", 12, false);
    const GFXfont* boldFont = FontRegistry::findClosest("sans", 12, true);
    const GFXfont* captionFont = FontRegistry::findClosest("sans", 9, false);
    const GFXfont* captionBoldFont = FontRegistry::findClosest("sans", 9, true);

    // Header bar
    epd->fillRect(0, 0, EPD_WIDTH, 46, GxEPD_BLACK);
    if (titleFont) {
        epd->setFont(titleFont);
        epd->setTextColor(GxEPD_WHITE);
        epd->setCursor(20, 42);
        epd->print("AInk Setup");
    }

    // ---- STEP 1 (left half) ----
    int step1X = 30;
    int setupMidX = EPD_WIDTH / 2;

    if (bigFont) {
        epd->setFont(bigFont);
        epd->setTextColor(GxEPD_BLACK);
        epd->setCursor(step1X, 95);
        epd->print("Step 1: Connect");
    }

    // WiFi QR code
    drawQrToEpd("WIFI:T:nopass;S:UkieLab-AInk;;", 60, 120, 180);

    if (captionBoldFont) {
        epd->setFont(captionBoldFont);
        epd->setTextColor(GxEPD_BLACK);
        epd->setCursor(70, 310);
        epd->print("Scan to join WiFi");
    }

    if (bodyFont) {
        epd->setFont(bodyFont);
        epd->setCursor(step1X, 345);
        epd->print("or connect manually:");
    }
    if (boldFont) {
        epd->setFont(boldFont);
        epd->setCursor(step1X, 375);
        epd->print("UkieLab-AInk");
    }
    if (captionFont) {
        epd->setFont(captionFont);
        epd->setCursor(step1X, 400);
        epd->print("(open network, no password)");
    }

    // Vertical divider
    epd->drawLine(setupMidX, 60, setupMidX, EPD_HEIGHT - 40, GxEPD_BLACK);

    // ---- STEP 2 (right half) ----
    int step2X = setupMidX + 30;

    if (bigFont) {
        epd->setFont(bigFont);
        epd->setTextColor(GxEPD_BLACK);
        epd->setCursor(step2X, 95);
        epd->print("Step 2: Set Up");
    }

    // URL QR code
    drawQrToEpd("http://192.168.4.1", step2X + 30, 120, 180);

    if (captionBoldFont) {
        epd->setFont(captionBoldFont);
        epd->setTextColor(GxEPD_BLACK);
        epd->setCursor(step2X + 40, 310);
        epd->print("Scan to open setup");
    }

    if (bodyFont) {
        epd->setFont(bodyFont);
        epd->setCursor(step2X, 345);
        epd->print("or open in browser:");
    }
    if (boldFont) {
        epd->setFont(boldFont);
        epd->setCursor(step2X, 375);
        epd->print("http://192.168.4.1");
    }
    if (captionFont) {
        epd->setFont(captionFont);
        epd->setCursor(step2X, 400);
        epd->print("Name display + enter WiFi creds");
    }

    // Bottom bar
    int setupFooterY = EPD_HEIGHT - 22;
    epd->fillRect(0, setupFooterY, EPD_WIDTH, 22, GxEPD_BLACK);
    if (captionFont) {
        epd->setFont(captionFont);
        epd->setTextColor(GxEPD_WHITE);
        String ver = VERSION;
        epd->setCursor(20, setupFooterY + 14);
        epd->print("AInk v" + ver + "  |  ukielab.com");
    }

    Serial.println("Sending setup screen to display...");
    epd->display(false);
    Serial.println("Setup screen refresh triggered");
    if (framebuffer.isValid()) framebuffer.swapAfterFullRefresh();
}

void showWelcomeScreen() {
    if (!epd || !epd->buffersValid()) return;

    epd->fillScreen(GxEPD_WHITE);

    const GFXfont* titleFont = FontRegistry::findClosest("sans", 24, true);
    const GFXfont* bodyFont = FontRegistry::findClosest("sans", 18, false);
    const GFXfont* boldFont = FontRegistry::findClosest("sans", 18, true);
    const GFXfont* captionFont = FontRegistry::findClosest("sans", 9, false);

    // Header bar
    epd->fillRect(0, 0, EPD_WIDTH, 46, GxEPD_BLACK);
    if (titleFont) {
        epd->setFont(titleFont);
        epd->setTextColor(GxEPD_WHITE);
        epd->setCursor(20, 42);
        epd->print("Welcome to AInk");
    }

    // Main message
    if (bodyFont) {
        epd->setFont(bodyFont);
        epd->setTextColor(GxEPD_BLACK);
        epd->setCursor(170, 200);
        epd->print("To begin setup, press the");
    }
    if (boldFont) {
        epd->setFont(boldFont);
        epd->setCursor(270, 250);
        epd->print("RESET button");
    }
    if (bodyFont) {
        epd->setFont(bodyFont);
        epd->setCursor(220, 300);
        epd->print("on the back of the device");
    }

    // Footer
    int footerY = EPD_HEIGHT - 22;
    epd->fillRect(0, footerY, EPD_WIDTH, 22, GxEPD_BLACK);
    if (captionFont) {
        epd->setFont(captionFont);
        epd->setTextColor(GxEPD_WHITE);
        String ver = VERSION;
        epd->setCursor(20, footerY + 14);
        epd->print("AInk v" + ver + "  |  ukielab.com");
    }

    epd->display(false);
    if (framebuffer.isValid()) framebuffer.swapAfterFullRefresh();
}

// ============ NVS Auto-Update Helpers ============

bool nvsGetAutoUpdateEnabled() {
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, true); // read-only
    bool val = prefs.getBool(NVS_KEY_AUTO, false);
    prefs.end();
    return val;
}

void nvsSetAutoUpdateEnabled(bool enabled) {
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, false);
    prefs.putBool(NVS_KEY_AUTO, enabled);
    prefs.end();
    autoUpdateEnabled = enabled;
    Serial.printf("Auto-update %s (saved to NVS)\n", enabled ? "enabled" : "disabled");
}

// Returns "days since Unix epoch" for the given time_t, or 0 if unavailable
static uint32_t getCurrentDay() {
    time_t now = time(nullptr);
    if (now < 1000000) return 0; // NTP not synced yet
    return (uint32_t)(now / 86400UL);
}

uint32_t nvsGetLastAutoUpdateDay() {
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, true);
    uint32_t val = prefs.getUInt(NVS_KEY_LAST, 0);
    prefs.end();
    return val;
}

void nvsSetLastAutoUpdateDay(uint32_t day) {
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, false);
    prefs.putUInt(NVS_KEY_LAST, day);
    prefs.end();
}

// ============ OTA Tasks ============

struct OTACheckTaskParams { bool force; };

void otaCheckTask(void* param) {
    OTACheckTaskParams* p = (OTACheckTaskParams*)param;
    bool force = p->force;
    delete p;

    bool hadFrontBuffer = framebuffer.releaseFrontBuffer();
    Serial.printf("OTA check: Free heap: %d bytes, largest block: %d bytes\n",
                  ESP.getFreeHeap(), ESP.getMaxAllocHeap());

    bool hasUpdate = otaManager.checkForUpdates(force);
    GitHubRelease rel = otaManager.getLatestRelease();

    otaCheckResult.hasUpdate    = hasUpdate;
    otaCheckResult.latestVersion = rel.tagName;
    otaCheckResult.releaseName  = rel.name;
    otaCheckResult.publishedAt  = rel.publishedAt;
    otaCheckResult.firmwareUrl  = rel.firmwareUrl;
    otaCheckResult.spiffsUrl    = rel.spiffsUrl;
    if (!hasUpdate && otaManager.getStatus() != OTAUpdateStatus::UPDATE_AVAILABLE) {
        otaCheckResult.error = otaManager.getErrorMessage();
    } else {
        otaCheckResult.error = "";
    }
    otaCheckResult.checked = true;

    if (hadFrontBuffer) {
        delay(500);
        framebuffer.reacquireFrontBuffer();
    }

    otaTaskRunning = false;
    vTaskDelete(NULL);
}

// Auto-update task: check and install if newer version is available
void otaAutoUpdateTask(void* param) {
    bool hadFrontBuffer = framebuffer.releaseFrontBuffer();
    Serial.println("Auto-update: checking for new firmware...");
    bool hasUpdate = otaManager.checkForUpdates(false);

    GitHubRelease rel = otaManager.getLatestRelease();
    otaCheckResult.hasUpdate     = hasUpdate;
    otaCheckResult.latestVersion = rel.tagName;
    otaCheckResult.releaseName   = rel.name;
    otaCheckResult.publishedAt   = rel.publishedAt;
    otaCheckResult.firmwareUrl   = rel.firmwareUrl;
    otaCheckResult.spiffsUrl     = rel.spiffsUrl;
    otaCheckResult.error         = (!hasUpdate && otaManager.getStatus() != OTAUpdateStatus::UPDATE_AVAILABLE)
                                   ? otaManager.getErrorMessage() : "";
    otaCheckResult.checked       = true;

    // Record that we ran today regardless of outcome
    uint32_t today = getCurrentDay();
    if (today > 0) nvsSetLastAutoUpdateDay(today);

    if (hasUpdate) {
        Serial.printf("Auto-update: newer version %s available, installing...\n", rel.tagName.c_str());
        bool success = otaManager.installFirmwareFromGitHub();
        if (success && otaManager.hasSpiffsUpdate()) {
            Serial.println("Auto-update: firmware done, installing filesystem...");
            bool spiffsOk = otaManager.installSPIFFSFromGitHub();
            if (!spiffsOk) {
                Serial.println("Auto-update: SPIFFS failed, but firmware updated. Restarting anyway.");
            }
        }
        if (success) {
            Serial.println("Auto-update: complete, restarting...");
            delay(1000);
            ESP.restart();
        } else {
            Serial.printf("Auto-update: install failed: %s\n", otaManager.getErrorMessage().c_str());
        }
    } else {
        Serial.println("Auto-update: already up to date.");
    }

    if (hadFrontBuffer) {
        delay(500);
        framebuffer.reacquireFrontBuffer();
    }

    otaTaskRunning = false;
    vTaskDelete(NULL);
}

struct OTAInstallTaskParams { int type; };

void otaInstallTask(void* param) {
    OTAInstallTaskParams* p = (OTAInstallTaskParams*)param;
    int type = p->type;  // 1=firmware, 2=spiffs, 3=full (firmware+spiffs)
    delete p;

    bool hadFrontBuffer = framebuffer.releaseFrontBuffer();
    bool success = false;

    if (type == 3) {
        // Full update: firmware first, then SPIFFS
        Serial.println("Full update: installing firmware...");
        success = otaManager.installFirmwareFromGitHub();
        if (success && otaManager.hasSpiffsUpdate()) {
            Serial.println("Firmware done, installing filesystem...");
            // Settings backup to NVS happens automatically in installUpdate() for SPIFFS
            success = otaManager.installSPIFFSFromGitHub();
            if (!success) {
                Serial.println("SPIFFS update failed, but firmware updated. Restarting anyway.");
                success = true;  // Firmware was updated, still restart
            }
        }
    } else if (type == 2) {
        success = otaManager.installSPIFFSFromGitHub();
    } else {
        success = otaManager.installFirmwareFromGitHub();
    }

    otaTaskRunning = false;

    if (success) {
        delay(1000);
        ESP.restart();
    }

    if (hadFrontBuffer) {
        delay(500);
        framebuffer.reacquireFrontBuffer();
    }
    vTaskDelete(NULL);
}

// ============ Web Server Setup ============

void setupWebServer() {
    // Main config page
    server.serveStatic("/", SPIFFS, "/").setDefaultFile("app_config.html");

    // App config endpoints
    server.on("/api/app/config", HTTP_GET, [](AsyncWebServerRequest* req) {
        String json = "{";
        json += "\"deviceName\":\"" + mySettings->canvasSettings.deviceName + "\",";
        json += "\"wifiConfigured\":" + String(mySettings->connectionSettings.ssid != "" ? "true" : "false");
        json += "}";
        req->send(200, "application/json", json);
    });

    server.on("/api/app/save", HTTP_POST, [](AsyncWebServerRequest* req) {},
        nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total) {
            static String body;
            if (index == 0) body = "";
            body += String((char*)data).substring(0, len);

            if (index + len == total) {
                DynamicJsonDocument doc(512);
                if (deserializeJson(doc, body) == DeserializationError::Ok) {
                    if (doc.containsKey("deviceName"))
                        mySettings->canvasSettings.deviceName = doc["deviceName"].as<String>();
                    mySettings->saveSettings(true);

                    bool wifiConfigured = (mySettings->connectionSettings.ssid != "");
                    if (wifiConfigured) {
                        req->send(200, "application/json", "{\"success\":true,\"needWifi\":false}");
                    } else {
                        req->send(200, "application/json", "{\"success\":true,\"needWifi\":true}");
                    }
                } else {
                    req->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
                }
            }
        });

    // Welcome / quick start page
    server.on("/start", HTTP_GET, [](AsyncWebServerRequest* req) {
        req->send(SPIFFS, "/start.html", "text/html");
    });

    // WiFi endpoints
    server.on("/wifi", HTTP_GET, [](AsyncWebServerRequest* req) {
        req->send(SPIFFS, "/wifi.html", "text/html");
    });

    server.on("/wifi.html", HTTP_GET, [](AsyncWebServerRequest* req) {
        req->send(SPIFFS, "/wifi.html", "text/html");
    });

    server.on("/wifiList", HTTP_GET, [](AsyncWebServerRequest* req) {
        String json = "[";
        int n = WiFi.scanComplete();
        if (n == WIFI_SCAN_FAILED) {
            WiFi.scanNetworks(true);
            json += "]";
        } else if (n > 0) {
            for (int i = 0; i < n; i++) {
                if (i > 0) json += ",";
                json += "{\"ssid\":\"" + WiFi.SSID(i) + "\",";
                json += "\"signalStrength\":" + String(WiFi.RSSI(i)) + ",";
                json += "\"security\":\"" + String(WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "none" : "WPA") + "\"}";
            }
            json += "]";
            WiFi.scanDelete();
            WiFi.scanNetworks(true);
        } else {
            json += "]";
            WiFi.scanNetworks(true);
        }
        req->send(200, "application/json", json);
    });

    server.on("/connectionStatus", HTTP_GET, [](AsyncWebServerRequest* req) {
        bool isConnected = (WiFi.status() == WL_CONNECTED);
        String json = "{\"isConnected\":" + String(isConnected ? "true" : "false");
        if (isConnected) {
            json += ",\"network\":\"" + WiFi.SSID() + "\"";
        }
        json += "}";
        req->send(200, "application/json", json);
    });

    server.on("/wifiSave", HTTP_POST, [](AsyncWebServerRequest* req) {
        String ssid = "";
        String password = "";

        if (req->hasParam("ssid", true)) {
            ssid = req->getParam("ssid", true)->value();
        }
        if (req->hasParam("password", true)) {
            password = req->getParam("password", true)->value();
        } else if (req->hasParam("pass", true)) {
            password = req->getParam("pass", true)->value();
        }

        if (ssid.length() > 0) {
            mySettings->connectionSettings.ssid = ssid;
            mySettings->connectionSettings.password = password;
            mySettings->saveSettings(true);
            Serial.printf("WiFi credentials saved: %s\n", ssid.c_str());
            req->send(200, "text/plain", "WiFi credentials saved. Rebooting...");
            delay(1000);
            ESP.restart();
        } else {
            req->send(400, "text/plain", "Missing SSID parameter");
        }
    });

    // OTA update endpoints
    server.on("/update", HTTP_GET, [](AsyncWebServerRequest* req) {
        req->send(SPIFFS, "/update.html", "text/html");
    });

    server.on("/api/update/check", HTTP_GET, [](AsyncWebServerRequest* req) {
        bool force = req->hasParam("force");
        bool start = req->hasParam("start") || force;

        if (start && !otaTaskRunning) {
            if (force) { otaCheckResult.checked = false; otaCheckResult.error = ""; }
            otaTaskRunning = true;
            OTACheckTaskParams* p = new OTACheckTaskParams{force};
            xTaskCreate(otaCheckTask, "ota_check", 16384, p, 1, NULL);
        }

        DynamicJsonDocument doc(1024);
        doc["currentVersion"] = otaManager.getCurrentVersion();
        doc["currentEnvironment"] = OTAUpdateManager::getEnvironmentFromBuildFlags();

        if (otaTaskRunning) {
            doc["status"] = "checking";
        } else if (otaCheckResult.checked) {
            doc["status"] = "done";
            doc["updateAvailable"] = otaCheckResult.hasUpdate;
            doc["latestVersion"]   = otaCheckResult.latestVersion;
            doc["releaseName"]     = otaCheckResult.releaseName;
            doc["publishedAt"]     = otaCheckResult.publishedAt;
            doc["firmwareUrl"]     = otaCheckResult.firmwareUrl;
            doc["spiffsUrl"]       = otaCheckResult.spiffsUrl;
            if (!otaCheckResult.error.isEmpty()) doc["error"] = otaCheckResult.error;
        } else {
            doc["status"] = "idle";
        }

        String r; serializeJson(doc, r);
        req->send(200, "application/json", r);
    });

    server.on("/api/update/install", HTTP_POST, [](AsyncWebServerRequest* req) {
        DynamicJsonDocument doc(256);
        if (otaTaskRunning) {
            doc["status"] = "error";
            doc["error"] = "Update already in progress";
            String r; serializeJson(doc, r);
            req->send(400, "application/json", r);
            return;
        }
        if (!otaManager.getLatestRelease().isValid) {
            doc["status"] = "error";
            doc["error"] = "No release data. Please check for updates first.";
            String r; serializeJson(doc, r);
            req->send(400, "application/json", r);
            return;
        }
        String type = "full";  // Default to full update
        if (req->hasParam("type", true)) type = req->getParam("type", true)->value();
        int installType = (type == "spiffs") ? 2 : (type == "full") ? 3 : 1;

        doc["status"] = "started";
        doc["type"] = type;
        String r; serializeJson(doc, r);
        req->send(200, "application/json", r);

        otaTaskRunning = true;
        OTAInstallTaskParams* p = new OTAInstallTaskParams{installType};
        xTaskCreatePinnedToCore(otaInstallTask, "ota_task", 8192, p, 1, NULL, 0);
    });

    server.on("/api/update/progress", HTTP_GET, [](AsyncWebServerRequest* req) {
        OTAUpdateStatus st = otaManager.getStatus();
        DynamicJsonDocument doc(256);
        doc["status"] = (int)st;
        doc["progress"] = otaManager.getProgress();
        doc["message"] = otaManager.getStatusMessage();
        if (st >= OTAUpdateStatus::ERROR_NETWORK) {
            doc["error"] = otaManager.getErrorMessage();
        }
        String r; serializeJson(doc, r);
        req->send(200, "application/json", r);
    });

    // Auto-update schedule setting (persisted in NVS, survives firmware updates)
    server.on("/api/update/auto", HTTP_GET, [](AsyncWebServerRequest* req) {
        DynamicJsonDocument doc(128);
        doc["enabled"] = autoUpdateEnabled;
        String r; serializeJson(doc, r);
        req->send(200, "application/json", r);
    });

    server.on("/api/update/auto", HTTP_POST, [](AsyncWebServerRequest* req) {},
        nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total) {
            static String body;
            if (index == 0) body = "";
            body += String((char*)data).substring(0, len);
            if (index + len == total) {
                DynamicJsonDocument doc(128);
                if (deserializeJson(doc, body) == DeserializationError::Ok && doc.containsKey("enabled")) {
                    bool enabled = doc["enabled"].as<bool>();
                    nvsSetAutoUpdateEnabled(enabled);
                    DynamicJsonDocument resp(128);
                    resp["success"] = true;
                    resp["enabled"] = autoUpdateEnabled;
                    String r; serializeJson(resp, r);
                    req->send(200, "application/json", r);
                } else {
                    req->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
                }
            }
        });

    // Manual firmware upload
    server.on("/update", HTTP_POST, [](AsyncWebServerRequest* req) {
        req->send(200, "text/plain", Update.hasError() ? "FAIL" : "OK");
        delay(1000);
        ESP.restart();
    }, [](AsyncWebServerRequest* req, String filename, size_t index, uint8_t* data, size_t len, bool final) {
        if (!index) {
            Serial.printf("Update: %s\n", filename.c_str());
            if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
                Update.printError(Serial);
            }
        }
        if (Update.write(data, len) != len) {
            Update.printError(Serial);
        }
        if (final) {
            if (Update.end(true)) {
                Serial.printf("Update Success: %u bytes\n", index + len);
            } else {
                Update.printError(Serial);
            }
        }
    });

    // AInk API endpoints
    ApiHandlers::setup(server, apiCtx);

    // Captive portal handler
    server.onNotFound([](AsyncWebServerRequest* req) {
        if (isCaptivePortalRequest(req)) {
            handleCaptivePortal(req);
        } else {
            req->redirect("/");
        }
    });

    server.begin();
    Serial.println("Web server started");
}

// ============ mDNS Setup ============

void setupMDNS() {
    String hostname = mySettings->canvasSettings.deviceName;
    if (hostname.length() == 0) hostname = "aink";

    if (MDNS.begin(hostname.c_str())) {
        // Primary service: AI screen protocol
        MDNS.addService("_aiscreen", "_tcp", 80);
#ifdef EPD_PANEL_SSD1677
        MDNS.addServiceTxt("_aiscreen", "_tcp", "model", "ai-display-10.2");
#else
        MDNS.addServiceTxt("_aiscreen", "_tcp", "model", "ai-display-7.5");
#endif
        String ver = VERSION;
        MDNS.addServiceTxt("_aiscreen", "_tcp", "version", ver.c_str());
        MDNS.addServiceTxt("_aiscreen", "_tcp", "width", String(EPD_WIDTH).c_str());
        MDNS.addServiceTxt("_aiscreen", "_tcp", "height", String(EPD_HEIGHT).c_str());
        MDNS.addServiceTxt("_aiscreen", "_tcp", "depth", "1");
        MDNS.addServiceTxt("_aiscreen", "_tcp", "api", "/openapi.json");
        MDNS.addServiceTxt("_aiscreen", "_tcp", "mcp", "/mcp/tools.json");

        // Also advertise as generic HTTP
        MDNS.addService("http", "tcp", 80);

        Serial.printf("mDNS started: %s._aiscreen._tcp\n", hostname.c_str());
    } else {
        Serial.println("mDNS setup failed");
    }
}

// ============ Setup & Loop ============

void setup() {
    Serial.begin(115200);
    delay(100);
    Serial.println("\n\n=== AInk ===");

    // Initialize SPIFFS
    if (!SPIFFS.begin(true)) {
        Serial.println("SPIFFS mount failed");
    }

    // Initialize settings
    Settings::Initialize();
    mySettings->readSettingsWithNVSCheck();

    // Load auto-update preference from NVS (persists across OTA updates)
    autoUpdateEnabled = nvsGetAutoUpdateEnabled();

    // Set OTA repo from settings
    if (!mySettings->generalSettings.githubUser.isEmpty() &&
        !mySettings->generalSettings.githubRepo.isEmpty()) {
        otaManager.setGitHubRepo(
            mySettings->generalSettings.githubUser,
            mySettings->generalSettings.githubRepo);
    }

    // Multi-reset detector
    mrd = new MultiResetDetector(MRD_TIMEOUT, MRD_ADDRESS);
    if (mrd->detectMultiReset()) {
        Serial.println("Multi-reset detected - factory reset");
        mySettings->reset();
    }

    // Create render mutex
    renderMutex = xSemaphoreCreateMutex();

    // Initialize font registry
    FontRegistry::init();

    // Initialize display
    initDisplay();

    // Initialize framebuffer (shares EPD's buffer as back buffer)
    // On 10.2" displays, the front buffer may not fit in memory — that's OK,
    // FramebufferManager falls back to single-buffer mode (always full refresh).
    if (!framebuffer.init(epd->getBlackBuffer())) {
        Serial.println("FATAL: Framebuffer init failed (no back buffer)!");
        while (true) delay(1000);
    }

    Serial.printf("Framebuffer initialized (%s). Free heap: %d\n",
                  framebuffer.isDoubleBuffered() ? "double-buffered" : "single-buffer",
                  ESP.getFreeHeap());

    // Setup API context
    apiCtx.epd = epd;
    apiCtx.fb = &framebuffer;
    apiCtx.zones = &zones;
    apiCtx.log = &commandLog;
    apiCtx.renderMutex = renderMutex;
    apiCtx.displayTask = nullptr;
    apiCtx.refreshRequested = false;
    apiCtx.refreshBusy = false;
    apiCtx.docsRead = false;
    apiCtx.lastFrameId = 0;
    apiCtx.lastRenderTimeMs = 0;
    apiCtx.lastRefreshTimeMs = 0;

    // Create display refresh task on core 0 (async_tcp runs on core 0/1)
    xTaskCreatePinnedToCore(
        ApiHandlers::displayRefreshTask,
        "epd_refresh",
        8192,        // Stack size (SSD1677 needs more for reset+init+76KB SPI transfer)
        &apiCtx,     // Parameter
        1,           // Priority
        &apiCtx.displayTask,
        1            // Core 1 (same core as SPI init)
    );

    // If not configured, start AP mode for setup
    if (!mySettings->isConfigured()) {
        Serial.println("Starting setup mode");

        showSetupScreen();

        // Free display buffers to reclaim memory for WiFi scanning.
        // The e-ink panel retains its image, so we don't need the buffers
        // until normal operation begins after WiFi is configured.
        epd->freeBuffers();
        Serial.printf("Freed EPD buffers for setup mode. Free heap: %d\n", ESP.getFreeHeap());

        // Start AP+STA (STA needed for WiFi scanning)
        WiFi.mode(WIFI_AP_STA);
        WiFi.softAP("UkieLab-AInk");
        delay(100);
        IPAddress apIP(192, 168, 4, 1);
        WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
        Serial.print("AP IP: ");
        Serial.println(WiFi.softAPIP());

        // DNS for captive portal
        dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
        dnsServer.start(53, "*", apIP);

        WiFi.scanNetworks(true);
        setupWebServer();
        setupModeStartTime = millis();

        // Stay in setup loop until WiFi is configured
        while (!mySettings->isConfigured()) {
            dnsServer.processNextRequest();
            mrd->loop();
            mySettings->loop();

            if (!otaTaskRunning && millis() - setupModeStartTime > SETUP_INACTIVITY_TIMEOUT) {
                Serial.println("Setup inactivity timeout - entering deep sleep");
                dnsServer.stop();
                WiFi.disconnect(true);
                WiFi.mode(WIFI_OFF);
                epd->allocateBuffers();
                showWelcomeScreen();
                epd->waitReady();
                epd->powerOff();
                esp_deep_sleep_start();
            }

            delay(10);
        }

        dnsServer.stop();

        // Re-allocate display buffers after setup mode
        if (!epd->buffersValid()) {
            epd->allocateBuffers();
            framebuffer.init(epd->getBlackBuffer());
            Serial.printf("Re-allocated display buffers. Free heap: %d\n", ESP.getFreeHeap());
        }
    }

    // Connect to WiFi
    Serial.println("Connecting to WiFi: " + mySettings->connectionSettings.ssid);
    WiFi.mode(WIFI_STA);
    WiFi.begin(
        mySettings->connectionSettings.ssid.c_str(),
        mySettings->connectionSettings.password.c_str()
    );

    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 30) {
        delay(500);
        Serial.print(".");
        attempts++;
    }

    if (WiFi.status() == WL_CONNECTED) {
        wifiConnected = true;
        Serial.println("\nWiFi connected: " + WiFi.localIP().toString());

        // Sync time via NTP (needed for scheduled auto-updates at 2 AM)
        configTime(0, 0, "pool.ntp.org", "time.nist.gov");
        Serial.println("NTP time sync initiated");

        // Setup mDNS
        setupMDNS();

        // Setup web server with all endpoints
        setupWebServer();

        // Show startup screen with IP
        showStartupScreen(WiFi.localIP().toString());

        Serial.println("AInk ready! Listening for commands...");
        Serial.printf("Free heap: %d bytes\n", ESP.getFreeHeap());
    } else {
        Serial.println("\nWiFi connection failed - starting AP mode");
        WiFi.mode(WIFI_AP_STA);
        WiFi.softAP("UkieLab-AInk");
        setupWebServer();
        showSetupScreen();
    }

    // Don't call mrd->stop() here — it would clear the reset counter before
    // the user can press reset again. Let mrd->loop() handle the timeout.
}

void loop() {
    mrd->loop();
    mySettings->loop();

    // Check scheduled auto-update (throttled to once per minute)
    if (wifiConnected && autoUpdateEnabled && !otaTaskRunning) {
        unsigned long now = millis();
        if (now - lastAutoUpdatePoll >= 60000UL) {
            lastAutoUpdatePoll = now;

            struct tm timeinfo;
            if (getLocalTime(&timeinfo, 0)) {
                // Trigger at 2:00–2:59 AM UTC, once per day
                if (timeinfo.tm_hour == 2) {
                    uint32_t today = getCurrentDay();
                    if (today > 0 && today != nvsGetLastAutoUpdateDay()) {
                        Serial.println("Auto-update: 2 AM window — starting update check");
                        otaTaskRunning = true;
                        xTaskCreate(otaAutoUpdateTask, "ota_auto", 16384, nullptr, 1, NULL);
                    }
                }
            }
        }
    }

    // Light sleep between requests to save power
    // WiFi stays connected, HTTP server stays active
    delay(10);
}

#endif // AI_CANVAS
