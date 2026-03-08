#include "Settings.h"
#include <SPIFFS.h>
#include <vector>
#define FLASHFS SPIFFS

// RTC memory to track restore attempts across reboots (survives reboot but not power loss)
RTC_DATA_ATTR int rtc_settingsRestoreAttempts = 0;
const int MAX_RESTORE_ATTEMPTS = 2;

namespace {
const size_t serializeSettingsSize = 1024;

Settings *object = nullptr;

bool settingsChanged = false;
uint32_t settingsSaveTimer = 0;
uint32_t settingsSaveInterval = 5000;

const char *settingsFileName PROGMEM = "/settings.json";
const char *settingsFileNameSave PROGMEM = "/settings.json.save";

String lastSavedHash = "";

String GetUniqueID() { return String((uint32_t)ESP.getEfuseMac(), HEX); }

bool copyFile(String fileFrom, String fileTo) {
  mySettings->busy = true;
  Serial.printf_P(PSTR("Copying file from %s to %s\n"), fileFrom.c_str(),
                  fileTo.c_str());

  File source = FLASHFS.open(fileFrom, "r");
  if (!source) {
    Serial.print("FLASHFS Error reading file: ");
    Serial.println(fileFrom);
    mySettings->busy = false;
    return false;
  }

  if (FLASHFS.exists(fileTo)) {
    FLASHFS.remove(fileTo);
  }

  File dest = FLASHFS.open(fileTo, "w");
  if (!dest) {
    Serial.print("FLASHFS Error opening file: ");
    Serial.println(fileTo);
    source.close();
    mySettings->busy = false;
    return false;
  }

  size_t blockSize = 64;
  uint8_t buf[blockSize];
  while (size_t n = source.read(buf, blockSize)) {
    if (dest.write(buf, n) == 0) {
      Serial.print("FLASHFS Error writing to file: ");
      Serial.println(fileTo);
      source.close();
      dest.close();
      FLASHFS.remove(fileTo);
      mySettings->busy = false;
      return false;
    }
  }

  dest.close();
  source.close();

  mySettings->busy = false;
  return true;
}

void restoreSettingsAndReboot() {
  rtc_settingsRestoreAttempts++;
  Serial.printf("Settings restore attempt %d of %d\n", rtc_settingsRestoreAttempts, MAX_RESTORE_ATTEMPTS);

  if (rtc_settingsRestoreAttempts > MAX_RESTORE_ATTEMPTS) {
    Serial.println("CRITICAL: Max restore attempts exceeded - resetting to factory defaults");
    rtc_settingsRestoreAttempts = 0;

    if (FLASHFS.exists(settingsFileName)) {
      FLASHFS.remove(settingsFileName);
    }
    if (FLASHFS.exists(settingsFileNameSave)) {
      FLASHFS.remove(settingsFileNameSave);
    }

    Serial.println("Corrupt settings files removed - restarting with defaults");
    ESP.restart();
    delay(5000);
    return;
  }

  Serial.println("Restoring settings from backup and rebooting");
  copyFile(settingsFileNameSave, settingsFileName);
  ESP.restart();
  delay(5000);
}

} // namespace

Settings *Settings::instance() { return object; }

void Settings::Initialize(uint32_t saveInterval) {
  if (object) {
    return;
  }

  Serial.println("Initializing Settings");
  object = new Settings(saveInterval);
}

void Settings::loop() {
  if (settingsChanged && settingsSaveTimer > 0 &&
      (millis() - settingsSaveTimer) > settingsSaveInterval) {
    settingsChanged = false;
    settingsSaveTimer = millis();
    saveSettings();
  }
}

void Settings::saveLater() {
  settingsChanged = true;
  settingsSaveTimer = millis();
}

bool Settings::isConfigured() {
  return connectionSettings.ssid != "";
}

bool Settings::hasChanged() {
  String currentHash =
      generalSettings.githubUser + "|" + generalSettings.githubRepo + "|" +
      connectionSettings.ssid + "|" +
      connectionSettings.password + "|" + canvasSettings.deviceName;
  return currentHash != lastSavedHash;
}

bool Settings::saveSettings(bool force) {
  if (!force && !hasChanged()) {
    Serial.println("Settings unchanged, skipping save");
    return true;
  }

  busy = true;

  Serial.print("Saving settings (atomic)... ");

  const char* tempFileName = "/settings.json.tmp";

  File file = FLASHFS.open(tempFileName, "w");
  if (!file) {
    Serial.println("Error opening temp settings file from FLASHFS!");
    restoreSettingsAndReboot();
    busy = false;
    return false;
  }

  DynamicJsonDocument json(serializeSettingsSize);
  JsonObject root = json.to<JsonObject>();
  buildSettingsJson(root);

  size_t bytesWritten = serializeJson(json, file);
  file.close();

  if (bytesWritten == 0) {
    Serial.println("Failed to serialize settings");
    FLASHFS.remove(tempFileName);
    saveLater();
    busy = false;
    return false;
  }

  File verifyFile = FLASHFS.open(tempFileName, "r");
  if (!verifyFile || verifyFile.size() == 0) {
    Serial.println("Temp file verification failed");
    if (verifyFile) verifyFile.close();
    FLASHFS.remove(tempFileName);
    saveLater();
    busy = false;
    return false;
  }
  verifyFile.close();

  if (FLASHFS.exists(settingsFileName)) {
    FLASHFS.remove(settingsFileNameSave);
    FLASHFS.rename(settingsFileName, settingsFileNameSave);
  }
  FLASHFS.rename(tempFileName, settingsFileName);

  lastSavedHash =
      generalSettings.githubUser + "|" + generalSettings.githubRepo + "|" +
      connectionSettings.ssid + "|" +
      connectionSettings.password + "|" + canvasSettings.deviceName;

  Serial.println("Saved settings!");

  busy = false;
  return true;
}

bool Settings::readSettings() {
  bool settingsExists = FLASHFS.exists(settingsFileName);
  bool settingsSaveExists = FLASHFS.exists(settingsFileNameSave);

  if (!settingsExists) {
    if (settingsSaveExists) {
      settingsExists = copyFile(settingsFileNameSave, settingsFileName);
    } else {
      bool saveSuccess = saveSettings();
      if (!saveSuccess) {
        return false;
      }
    }
  }

  if (!settingsSaveExists) {
    copyFile(settingsFileName, settingsFileNameSave);
  }

  File settings = FLASHFS.open(settingsFileName, "r");
  if (!settings || settings.size() == 0) {
    saveSettings();
    return false;
  }

  Serial.println("reading settings.json");
  while (settings.available()) {
    settings.readStringUntil('\n');
  }
  settings.seek(0);

  DynamicJsonDocument json(serializeSettingsSize);
  DeserializationError err = deserializeJson(json, settings);
  settings.close();
  if (err) {
    Serial.println(err.c_str());
    restoreSettingsAndReboot();
    return false;
  }

  JsonObject root = json.as<JsonObject>();
  if (root.size() == 0) {
    Serial.print("FLASHFS root.size() == 0");
    restoreSettingsAndReboot();
    return false;
  }

  if (root.containsKey("connection")) {
    JsonObject connectionObject = root["connection"];
    if (connectionObject.containsKey("ssid")) {
      connectionSettings.ssid = connectionObject["ssid"].as<String>();
    }
    if (connectionObject.containsKey("password")) {
      connectionSettings.password = connectionObject["password"].as<String>();
    }
  }

  if (root.containsKey("githubUser")) {
    generalSettings.githubUser = root["githubUser"].as<String>();
  }
  if (root.containsKey("githubRepo")) {
    generalSettings.githubRepo = root["githubRepo"].as<String>();
  }
  if (root.containsKey("canvas")) {
    JsonObject canvasObject = root["canvas"];
    if (canvasObject.containsKey("deviceName")) {
      canvasSettings.deviceName = canvasObject["deviceName"].as<String>();
    }
  }

  rtc_settingsRestoreAttempts = 0;

  copyFile(settingsFileName, settingsFileNameSave);

  lastSavedHash =
      generalSettings.githubUser + "|" + generalSettings.githubRepo + "|" +
      connectionSettings.ssid + "|" +
      connectionSettings.password + "|" + canvasSettings.deviceName;

  return true;
}

void Settings::buildSettingsJson(JsonObject &root) {
  root["githubUser"] = generalSettings.githubUser;
  root["githubRepo"] = generalSettings.githubRepo;

  JsonObject connectionObject = root.createNestedObject("connection");
  connectionObject["ssid"] = connectionSettings.ssid;
  connectionObject["password"] = connectionSettings.password;

  JsonObject canvasObject = root.createNestedObject("canvas");
  canvasObject["deviceName"] = canvasSettings.deviceName;
}

Settings::Settings(uint32_t saveInterval) {
  settingsSaveInterval = saveInterval;

  lastSavedHash =
      generalSettings.githubUser + "|" + generalSettings.githubRepo + "|" +
      connectionSettings.ssid + "|" +
      connectionSettings.password + "|" + canvasSettings.deviceName;
}

void Settings::reset() {
  Serial.println("Resetting settings to defaults...");

  connectionSettings.ssid = "";
  connectionSettings.password = "";

  canvasSettings.deviceName = "ai-canvas";

  saveSettings(true);

  Serial.println("Settings reset to defaults");
}
