#include "../OTAUpdateManager.h"
#include "Version.h"
#include <WiFi.h>
#include <SPIFFS.h>
#include <esp_task_wdt.h>

// Static member initialization
const char* OTAUpdateManager::GITHUB_API_HOST = "api.github.com";
const int OTAUpdateManager::GITHUB_API_PORT = 443;
const int OTAUpdateManager::HTTP_TIMEOUT = 10000;  // 10 seconds
const size_t OTAUpdateManager::MAX_DOWNLOAD_SIZE = 16 * 1024 * 1024;  // 16 MB

ProgressCallback OTAUpdateManager::_progressCallback = nullptr;

OTAUpdateManager::OTAUpdateManager()
    : _githubUser("")
    , _githubRepo("")
    , _currentEnvironment(getEnvironmentFromBuildFlags())
    , _currentVersion(BUILD_TIMESTAMP)
    , _status(OTAUpdateStatus::IDLE)
    , _progress(0)
{
    _latestRelease.isValid = false;
}

OTAUpdateManager::~OTAUpdateManager() {
}

void OTAUpdateManager::setGitHubRepo(const String& user, const String& repo) {
    _githubUser = user;
    _githubRepo = repo;
}

void OTAUpdateManager::setGitHubPAT(const String& token) {
    _githubPAT = token;
}

void OTAUpdateManager::setCurrentEnvironment(const String& environment) {
    _currentEnvironment = environment;
}

void OTAUpdateManager::setCurrentVersion(const String& version) {
    _currentVersion = version;
}

String OTAUpdateManager::getEnvironmentFromBuildFlags() {
    #ifdef FIRMWARE_ENVIRONMENT
    return String(FIRMWARE_ENVIRONMENT);
    #else
    // Fallback: try to detect from build flags
    #if defined(epd102)
        #if defined(battery) && defined(co2sensor)
        return "esp32dev102_battery_co2";
        #elif defined(battery)
        return "esp32dev102_battery";
        #elif defined(co2sensor)
        return "esp32dev102_CO2";
        #elif defined(showClock)
        return "esp32dev102_clock";
        #else
        return "esp32dev102";
        #endif
    #elif defined(epd75)
        #if defined(battery) && defined(co2sensor)
        return "esp32dev75_battery_co2";
        #elif defined(battery)
        return "esp32dev75_battery";
        #elif defined(co2sensor)
        return "esp32dev75_CO2";
        #elif defined(showClock)
        return "esp32dev75_clock";
        #else
        return "esp32dev75";
        #endif
    #else
    return "unknown";
    #endif
    #endif
}

bool OTAUpdateManager::checkForUpdates(bool force) {
    // Clear previous error message at the start of a new check
    _errorMessage = "";

    if (!WiFi.isConnected()) {
        _status = OTAUpdateStatus::ERROR_NETWORK;
        _errorMessage = "WiFi not connected";
        return false;
    }

    _status = OTAUpdateStatus::CHECKING;
    _statusMessage = "Checking for updates...";

    bool success = fetchLatestRelease();

    if (success && _latestRelease.isValid) {
        // If force is true, always consider update available
        if (force || isNewerVersion(_currentVersion, _latestRelease.tagName)) {
            _status = OTAUpdateStatus::UPDATE_AVAILABLE;
            if (force && !isNewerVersion(_currentVersion, _latestRelease.tagName)) {
                _statusMessage = "Ready to reinstall version: " + _latestRelease.tagName;
            } else {
                _statusMessage = "Update available: " + _latestRelease.tagName;
            }
        } else {
            _status = OTAUpdateStatus::ERROR_NO_UPDATE;
            _statusMessage = "Already on latest version";
        }
    } else {
        _status = OTAUpdateStatus::ERROR_NETWORK;
        _statusMessage = "Failed to check for updates";
    }

    return success && _status == OTAUpdateStatus::UPDATE_AVAILABLE;
}

bool OTAUpdateManager::fetchLatestRelease() {
    // NEW STRATEGY: Read only first 2KB to extract tag_name, then construct URLs directly
    // This avoids downloading and parsing 131KB of JSON

    // Give system time to free memory before SSL allocation
    yield();
    delay(100);

    Serial.printf("OTA: Free heap before SSL connection: %d bytes\n", ESP.getFreeHeap());

    WiFiClientSecure client;
    // SECURITY NOTE: setInsecure() disables SSL certificate verification.
    // This is a trade-off for ESP32 memory constraints - CA bundles use significant RAM.
    // Mitigations: GitHub HSTS, ESP32 Update library validates firmware magic bytes and CRC.
    client.setInsecure();
    client.setTimeout(15);  // 15 second timeout

    HTTPClient http;

    String url = String("https://") + GITHUB_API_HOST + "/repos/" +
                 _githubUser + "/" + _githubRepo + "/releases/latest";

    Serial.println("Fetching latest release tag from: " + url);
    Serial.println("Using GitHub user: " + _githubUser + ", repo: " + _githubRepo);
    if (!_githubPAT.isEmpty()) {
        Serial.println("Using GitHub PAT authentication");
    }

    http.begin(client, url);
    http.addHeader("Accept", "application/vnd.github.v3+json");
    http.addHeader("User-Agent", "ESP32-UkieLabDisplay");

    // Add Authorization header if GitHub PAT is provided
    if (!_githubPAT.isEmpty()) {
        http.addHeader("Authorization", "Bearer " + _githubPAT);
    }

    http.setTimeout(10000);

    int httpCode = http.GET();

    Serial.printf("HTTP Response Code: %d\n", httpCode);
    Serial.printf("Content-Length: %d\n", http.getSize());

    // Log response headers for debugging
    if (http.hasHeader("Location")) {
        Serial.println("Redirect Location: " + http.header("Location"));
    }
    if (http.hasHeader("Content-Type")) {
        Serial.println("Content-Type: " + http.header("Content-Type"));
    }

    if (httpCode == HTTP_CODE_OK) {
        WiFiClient* stream = http.getStreamPtr();
        stream->setTimeout(5);  // 5 second socket timeout

        // Only read first 2KB to extract tag_name - it appears early in JSON
        Serial.println("Reading first 2KB only...");
        String payload = "";
        payload.reserve(2048);

        unsigned long startTime = millis();
        int bytesRead = 0;
        const int MAX_READ = 2048;

        esp_task_wdt_reset();

        // Read only first 2KB
        while ((http.connected() || stream->available()) && bytesRead < MAX_READ) {
            if (stream->available()) {
                int c = stream->read();
                if (c >= 0) {
                    payload += (char)c;
                    bytesRead++;
                }
            } else {
                delay(1);
            }

            // Timeout after 5 seconds
            if (millis() - startTime > 5000) {
                break;
            }

            // Feed watchdog every 100 bytes
            if (bytesRead % 100 == 0) {
                yield();
            }
        }

        http.end();  // Close connection immediately - we don't need the rest
        client.stop();  // Explicitly stop the client to free SSL resources

        Serial.printf("Read %d bytes for tag extraction\n", payload.length());
        Serial.println("Preview: " + payload.substring(0, min(300, (int)payload.length())));

        if (payload.length() < 100) {
            Serial.println("Response too small");
            _errorMessage = "Invalid response";
            return false;
        }

        esp_task_wdt_reset();

        // Give SSL client time to fully release memory before next connection
        delay(100);

        // Extract just the tag_name using simple string parsing
        return parseReleaseJSONMinimal(payload, _latestRelease);
    } else if (httpCode == 404) {
        // /releases/latest returns 404 for:
        //   - Draft releases (never visible via API without admin scope)
        //   - Pre-releases (skipped by /releases/latest)
        //   - Repos with no published releases at all
        // Fall back to /releases?per_page=1 which includes pre-releases.
        Serial.println("Got 404 from /releases/latest, trying /releases?per_page=1...");
        http.end();
        client.stop();
        delay(200);

        WiFiClientSecure client2;
        client2.setInsecure();
        client2.setTimeout(15);
        HTTPClient http2;

        String fallbackUrl = String("https://") + GITHUB_API_HOST + "/repos/" +
                             _githubUser + "/" + _githubRepo + "/releases?per_page=1";
        Serial.println("Fallback URL: " + fallbackUrl);

        http2.begin(client2, fallbackUrl);
        http2.addHeader("Accept", "application/vnd.github.v3+json");
        http2.addHeader("User-Agent", "ESP32-UkieLabDisplay");
        if (!_githubPAT.isEmpty()) {
            http2.addHeader("Authorization", "Bearer " + _githubPAT);
        }
        http2.setTimeout(10000);

        int httpCode2 = http2.GET();
        Serial.printf("Fallback HTTP Response Code: %d\n", httpCode2);

        if (httpCode2 == HTTP_CODE_OK) {
            WiFiClient* stream2 = http2.getStreamPtr();
            stream2->setTimeout(5);

            String payload2 = "";
            payload2.reserve(2048);
            unsigned long startTime2 = millis();
            int bytesRead2 = 0;
            esp_task_wdt_reset();

            while ((http2.connected() || stream2->available()) && bytesRead2 < 2048) {
                if (stream2->available()) {
                    int c = stream2->read();
                    if (c >= 0) { payload2 += (char)c; bytesRead2++; }
                } else {
                    delay(1);
                }
                if (millis() - startTime2 > 5000) break;
                if (bytesRead2 % 100 == 0) yield();
            }

            http2.end();
            client2.stop();
            Serial.printf("Fallback read %d bytes\n", payload2.length());

            // Response is an array: [{...}]. Check if empty.
            if (payload2.length() < 10 || payload2.startsWith("[]")) {
                Serial.println("No releases found (empty list)");
                _errorMessage = "No releases published yet (check GitHub for draft releases)";
                return false;
            }

            delay(100);
            return parseReleaseJSONMinimal(payload2, _latestRelease);
        } else {
            Serial.printf("Fallback also failed, HTTP %d\n", httpCode2);
            _errorMessage = "No releases found (HTTP " + String(httpCode2) + ")";
            http2.end();
            return false;
        }
    } else if (httpCode == 301 || httpCode == 302 || httpCode == 307) {
        Serial.printf("Received redirect (%d)\n", httpCode);
        if (http.hasHeader("Location")) {
            Serial.println("Redirect to: " + http.header("Location"));
        }
        _errorMessage = "Unexpected redirect from GitHub API";
        http.end();
        return false;
    } else {
        Serial.printf("HTTP GET failed, error: %s (code: %d)\n", http.errorToString(httpCode).c_str(), httpCode);
        _errorMessage = "HTTP error: " + String(httpCode);
        http.end();
        return false;
    }
}

bool OTAUpdateManager::parseReleaseJSONMinimal(const String& json, GitHubRelease& release) {
    // Simple string parsing to extract just tag_name from first 2KB
    // This avoids memory issues with full JSON parsing

    Serial.println("Extracting tag_name from partial JSON...");

    // Find "tag_name":"vXXXXXX"
    int tagPos = json.indexOf("\"tag_name\":\"");
    if (tagPos < 0) {
        Serial.println("tag_name not found in response");
        _errorMessage = "Could not find version tag";
        return false;
    }

    tagPos += 12;  // Skip past "tag_name":"
    int tagEnd = json.indexOf("\"", tagPos);
    if (tagEnd < 0) {
        Serial.println("tag_name end quote not found");
        _errorMessage = "Malformed tag";
        return false;
    }

    release.tagName = json.substring(tagPos, tagEnd);
    release.name = "Release " + release.tagName;
    release.body = "";  // Don't need release notes
    release.publishedAt = "";

    Serial.println("Found tag: " + release.tagName);

    // Fetch asset download URLs via a separate API call to avoid parsing large JSON

    // Small delay to let previous SSL connection fully cleanup
    delay(200);

    bool assetsFound = fetchReleaseAssets(release);

    if (!assetsFound) {
        Serial.println("Failed to fetch release assets");
        return false;
    }

    Serial.println("Firmware URL: " + release.firmwareUrl);
    Serial.println("SPIFFS URL: " + release.spiffsUrl);

    release.isValid = !release.tagName.isEmpty() && !release.firmwareUrl.isEmpty();
    return release.isValid;
}

bool OTAUpdateManager::fetchReleaseAssets(GitHubRelease& release) {
    // Fetch assets via GitHub API: GET /repos/{owner}/{repo}/releases/tags/{tag}

    Serial.printf("Free heap before assets fetch: %d bytes\n", ESP.getFreeHeap());

    // Feed watchdog before starting network operation
    esp_task_wdt_reset();
    yield();

    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(15);  // 15 second timeout

    HTTPClient http;

    String url = String("https://") + GITHUB_API_HOST + "/repos/" +
                 _githubUser + "/" + _githubRepo + "/releases/tags/" + release.tagName;

    Serial.println("Fetching release assets from: " + url);

    http.begin(client, url);
    http.addHeader("Accept", "application/vnd.github.v3+json");
    http.addHeader("User-Agent", "ESP32-UkieLabDisplay");

    if (!_githubPAT.isEmpty()) {
        http.addHeader("Authorization", "Bearer " + _githubPAT);
    }

    http.setTimeout(10000);

    // Feed watchdog before HTTP request
    esp_task_wdt_reset();
    yield();

    int httpCode = http.GET();

    // Feed watchdog after HTTP request
    esp_task_wdt_reset();
    yield();

    if (httpCode != HTTP_CODE_OK) {
        Serial.printf("Failed to fetch assets, HTTP code: %d\n", httpCode);
        _errorMessage = "Failed to fetch assets: HTTP " + String(httpCode);
        http.end();
        return false;
    }

    // Read response efficiently
    int contentLength = http.getSize();
    Serial.printf("Assets JSON size: %d bytes\n", contentLength);
    Serial.printf("Free heap before read: %d bytes\n", ESP.getFreeHeap());

    // For large payloads, read into string first with yields to prevent watchdog
    // This is safer than stream parsing which can block
    String payload = "";
    payload.reserve(min(contentLength + 1, 32768));  // Cap at 32KB

    WiFiClient* stream = http.getStreamPtr();
    unsigned long startTime = millis();
    int bytesRead = 0;

    while ((http.connected() || stream->available()) && bytesRead < contentLength) {
        if (stream->available()) {
            int toRead = min(512, contentLength - bytesRead);  // Read in 512-byte chunks
            while (toRead > 0 && stream->available()) {
                char c = stream->read();
                payload += c;
                bytesRead++;
                toRead--;
            }
        }

        // Feed watchdog and yield every iteration
        esp_task_wdt_reset();
        yield();

        // Timeout after 15 seconds
        if (millis() - startTime > 15000) {
            Serial.println("Timeout reading assets response");
            http.end();
            _errorMessage = "Timeout fetching assets";
            return false;
        }
    }

    http.end();
    client.stop();  // Free SSL resources immediately

    Serial.printf("Read %d bytes, Free heap after read: %d bytes\n", bytesRead, ESP.getFreeHeap());

    // Feed watchdog before JSON parsing
    esp_task_wdt_reset();
    yield();

    // Use JSON filter to only parse assets array - saves memory
    StaticJsonDocument<256> filter;
    filter["assets"][0]["name"] = true;
    filter["assets"][0]["url"] = true;                    // API URL (authenticated access)
    filter["assets"][0]["browser_download_url"] = true;  // CDN URL (public repos, no auth)
    filter["assets"][0]["size"] = true;

    DynamicJsonDocument doc(24576);  // 24KB for filtered asset list

    DeserializationError error = deserializeJson(doc, payload, DeserializationOption::Filter(filter));

    // Feed watchdog after parsing
    esp_task_wdt_reset();
    yield();

    Serial.printf("Free heap after parse: %d bytes\n", ESP.getFreeHeap());

    if (error) {
        Serial.print("JSON parse failed: ");
        Serial.println(error.c_str());
        _errorMessage = "Failed to parse release data";
        return false;
    }

    // Look for assets matching our environment
    String firmwareFilename = _currentEnvironment + "-firmware.bin";
    String spiffsFilename = _currentEnvironment + "-spiffs.bin";

    JsonArray assets = doc["assets"];
    bool foundFirmware = false;
    bool foundSpiffs = false;

    for (JsonObject asset : assets) {
        String name = asset["name"].as<String>();

        if (name == firmwareFilename) {
            String cdnUrl = asset["browser_download_url"].as<String>();
            String apiUrl = asset["url"].as<String>();
            release.firmwareUrl = (_githubPAT.isEmpty() && !cdnUrl.isEmpty()) ? cdnUrl : apiUrl;
            release.firmwareSize = asset["size"].as<size_t>();
            foundFirmware = true;
            Serial.println("Found firmware asset: " + name);
        } else if (name == spiffsFilename) {
            String cdnUrl = asset["browser_download_url"].as<String>();
            String apiUrl = asset["url"].as<String>();
            release.spiffsUrl = (_githubPAT.isEmpty() && !cdnUrl.isEmpty()) ? cdnUrl : apiUrl;
            release.spiffsSize = asset["size"].as<size_t>();
            foundSpiffs = true;
            Serial.println("Found SPIFFS asset: " + name);
        }

        if (foundFirmware && foundSpiffs) {
            break;
        }
    }

    if (!foundFirmware) {
        Serial.println("Firmware asset not found: " + firmwareFilename);
        _errorMessage = "Firmware not found in release";
        return false;
    }

    return true;
}

bool OTAUpdateManager::isNewerVersion(const String& current, const String& latest) {
    // Try semantic version comparison first, fall back to numeric
    int currentMajor, currentMinor, currentPatch;
    int latestMajor, latestMinor, latestPatch;

    if (parseSemanticVersion(current, currentMajor, currentMinor, currentPatch) &&
        parseSemanticVersion(latest, latestMajor, latestMinor, latestPatch)) {
        // Both are semantic versions - compare properly
        if (latestMajor != currentMajor) return latestMajor > currentMajor;
        if (latestMinor != currentMinor) return latestMinor > currentMinor;
        return latestPatch > currentPatch;
    }

    // Fall back to simple numeric comparison for build timestamps
    int currentNum = parseVersionNumber(current);
    int latestNum = parseVersionNumber(latest);
    return latestNum > currentNum;
}

bool OTAUpdateManager::parseSemanticVersion(const String& version, int& major, int& minor, int& patch) {
    // Parse semantic version like "v1.2.3" or "1.2.3"
    String clean = version;
    clean.replace("v", "");
    clean.replace("V", "");

    // Find dots
    int firstDot = clean.indexOf('.');
    int secondDot = clean.indexOf('.', firstDot + 1);

    if (firstDot < 0) {
        return false; // Not a semantic version
    }

    major = clean.substring(0, firstDot).toInt();

    if (secondDot < 0) {
        // Only major.minor
        minor = clean.substring(firstDot + 1).toInt();
        patch = 0;
    } else {
        minor = clean.substring(firstDot + 1, secondDot).toInt();
        // Handle pre-release suffixes like "1.2.3-beta"
        String patchStr = clean.substring(secondDot + 1);
        int dashPos = patchStr.indexOf('-');
        if (dashPos >= 0) {
            patchStr = patchStr.substring(0, dashPos);
        }
        patch = patchStr.toInt();
    }

    return true;
}

int OTAUpdateManager::parseVersionNumber(const String& version) {
    // Handle build timestamp format like "251026"
    // Just strip non-numeric characters and convert

    String clean = version;
    clean.replace("v", "");
    clean.replace("V", "");
    clean.replace(".", "");
    clean.replace("-", "");

    // Try to convert to integer
    return clean.toInt();
}

GitHubRelease OTAUpdateManager::getLatestRelease() {
    return _latestRelease;
}

bool OTAUpdateManager::isUpdateAvailable() const {
    return _status == OTAUpdateStatus::UPDATE_AVAILABLE;
}

String OTAUpdateManager::getCurrentVersion() const {
    return _currentVersion;
}

String OTAUpdateManager::getLatestVersion() const {
    return _latestRelease.tagName;
}

bool OTAUpdateManager::installFirmwareFromGitHub(ProgressCallback callback) {
    if (!_latestRelease.isValid || _latestRelease.firmwareUrl.isEmpty()) {
        _status = OTAUpdateStatus::ERROR_DOWNLOAD;
        _errorMessage = "No firmware URL available";
        return false;
    }

    return installUpdate(OTAUpdateType::FIRMWARE, _latestRelease.firmwareUrl, callback);
}

bool OTAUpdateManager::installSPIFFSFromGitHub(ProgressCallback callback) {
    if (!_latestRelease.isValid || _latestRelease.spiffsUrl.isEmpty()) {
        _status = OTAUpdateStatus::ERROR_DOWNLOAD;
        _errorMessage = "No SPIFFS URL available";
        return false;
    }

    return installUpdate(OTAUpdateType::SPIFFS, _latestRelease.spiffsUrl, callback);
}

bool OTAUpdateManager::installUpdate(OTAUpdateType type, const String& url, ProgressCallback callback) {
    if (!WiFi.isConnected()) {
        _status = OTAUpdateStatus::ERROR_NETWORK;
        _errorMessage = "WiFi not connected";
        return false;
    }

    // If updating SPIFFS, unmount it first (best practice for ESP32)
    if (type == OTAUpdateType::SPIFFS) {
        Serial.println("Unmounting SPIFFS before update...");
        SPIFFS.end();
    }

    _progressCallback = callback;
    _status = OTAUpdateStatus::DOWNLOADING;
    _progress = 0;

    int updateType = (type == OTAUpdateType::FIRMWARE) ? U_FLASH : U_SPIFFS;
    _statusMessage = (type == OTAUpdateType::FIRMWARE) ?
                     "Downloading firmware..." : "Downloading filesystem...";

    bool success = downloadAndInstall(url, updateType, callback);

    if (success) {
        _status = OTAUpdateStatus::COMPLETE;
        _statusMessage = "Update complete! Rebooting...";
        _progress = 100;
    } else {
        _status = OTAUpdateStatus::ERROR_INSTALL;
        if (_errorMessage.isEmpty()) {
            _errorMessage = "Update installation failed";
        }

        // If SPIFFS update failed, remount SPIFFS
        if (type == OTAUpdateType::SPIFFS) {
            Serial.println("SPIFFS update failed, remounting...");
            SPIFFS.begin(true);
        }
    }

    _progressCallback = nullptr;
    return success;
}

bool OTAUpdateManager::downloadAndInstall(const String& url, int updateType, ProgressCallback callback) {
    WiFiClientSecure client;
    client.setInsecure();  // Use certificate bundle in production

    HTTPClient http;
    http.begin(client, url);

    // GitHub API asset URLs need Accept: application/octet-stream to get the binary
    // (without it, they return JSON metadata instead of the file).
    bool isGitHubUrl = url.indexOf("github.com") >= 0;
    if (isGitHubUrl) {
        http.addHeader("Accept", "application/octet-stream");
        if (!_githubPAT.isEmpty()) {
            Serial.println("Using authenticated API download");
            http.addHeader("Authorization", "Bearer " + _githubPAT);
        }
    }

    http.setTimeout(HTTP_TIMEOUT);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);  // Follow redirects for GitHub

    Serial.println("Downloading from: " + url);

    int httpCode = http.GET();

    if (httpCode != HTTP_CODE_OK) {
        Serial.printf("Download failed, HTTP code: %d\n", httpCode);
        _errorMessage = "Download failed: HTTP " + String(httpCode);
        http.end();
        return false;
    }

    int contentLength = http.getSize();
    if (contentLength <= 0) {
        Serial.println("Content length is unknown or zero");
        _errorMessage = "Invalid content length";
        http.end();
        return false;
    }

    if (contentLength > MAX_DOWNLOAD_SIZE) {
        Serial.println("File too large");
        _errorMessage = "File size exceeds maximum";
        http.end();
        return false;
    }

    WiFiClient* stream = http.getStreamPtr();

    // Begin update
    _status = OTAUpdateStatus::INSTALLING;
    _statusMessage = (updateType == U_FLASH) ? "Installing firmware..." : "Installing filesystem...";

    if (!Update.begin(contentLength, updateType)) {
        Serial.println("Update.begin() failed");
        Update.printError(Serial);
        _errorMessage = "Failed to begin update";
        http.end();
        return false;
    }

    // Set progress callback
    Update.onProgress([](size_t current, size_t total) {
        if (_progressCallback) {
            _progressCallback(current, total);
        }
    });

    // Write update in chunks to prevent watchdog timeout
    // Using chunked writing instead of writeStream() to yield regularly
    size_t written = 0;
    const size_t CHUNK_SIZE = 4096;  // 4KB chunks
    uint8_t* buffer = (uint8_t*)malloc(CHUNK_SIZE);

    if (!buffer) {
        Serial.println("Failed to allocate chunk buffer");
        _errorMessage = "Memory allocation failed";
        Update.abort();
        http.end();
        return false;
    }

    unsigned long lastYield = millis();

    while (written < contentLength) {
        // Feed watchdog and yield every iteration
        esp_task_wdt_reset();

        // Yield more frequently to keep async_tcp happy
        if (millis() - lastYield > 50) {
            yield();
            delay(1);  // Give async_tcp task time to run
            lastYield = millis();
        }

        size_t toRead = min(CHUNK_SIZE, (size_t)(contentLength - written));
        size_t bytesRead = 0;

        // Read chunk from stream with timeout
        unsigned long readStart = millis();
        while (bytesRead < toRead && (millis() - readStart) < 10000) {
            if (stream->available()) {
                size_t available = stream->available();
                size_t canRead = min(available, toRead - bytesRead);
                size_t got = stream->readBytes(buffer + bytesRead, canRead);
                bytesRead += got;
            } else {
                delay(1);
                yield();
            }

            // Feed watchdog during read
            if ((millis() - readStart) % 100 < 10) {
                esp_task_wdt_reset();
            }
        }

        if (bytesRead == 0) {
            Serial.println("Stream read timeout");
            _errorMessage = "Download timeout";
            free(buffer);
            Update.abort();
            http.end();
            return false;
        }

        // Write chunk to flash
        size_t chunkWritten = Update.write(buffer, bytesRead);
        if (chunkWritten != bytesRead) {
            Serial.printf("Write failed: wrote %u of %u bytes\n", chunkWritten, bytesRead);
            _errorMessage = "Flash write failed";
            free(buffer);
            Update.abort();
            http.end();
            return false;
        }

        written += chunkWritten;

        // Update progress
        _progress = (written * 100) / contentLength;
        if (callback) {
            callback(written, contentLength);
        }

        // Log progress every 10%
        static int lastPercent = -1;
        int percent = (written * 100) / contentLength;
        if (percent / 10 != lastPercent / 10) {
            Serial.printf("OTA progress: %d%% (%u/%u bytes)\n", percent, written, contentLength);
            lastPercent = percent;
        }
    }

    free(buffer);

    if (written != contentLength) {
        Serial.printf("Written only %u of %u bytes\n", written, contentLength);
        _errorMessage = "Incomplete download (" + String(written) + "/" + String(contentLength) + " bytes)";

        // Abort the partial update
        Update.abort();
        http.end();

        Serial.println("OTA: Partial download aborted. Device remains on current firmware.");
        return false;
    }

    // End update
    if (!Update.end()) {
        Serial.println("Update.end() failed");
        Update.printError(Serial);
        _errorMessage = "Failed to finalize update";
        http.end();
        return false;
    }

    if (!Update.isFinished()) {
        Serial.println("Update not finished");
        _errorMessage = "Update incomplete";
        http.end();
        return false;
    }

    http.end();

    if (updateType == U_SPIFFS) {
        Serial.println("SPIFFS update completed! Device will restart to remount filesystem...");
    } else {
        Serial.println("Firmware update completed!");
    }

    return true;
}

OTAUpdateStatus OTAUpdateManager::getStatus() const {
    return _status;
}

int OTAUpdateManager::getProgress() const {
    return _progress;
}

String OTAUpdateManager::getStatusMessage() const {
    return _statusMessage;
}

String OTAUpdateManager::getErrorMessage() const {
    return _errorMessage;
}

