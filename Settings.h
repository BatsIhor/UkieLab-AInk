#pragma once
#include <Arduino.h>
#define ARDUINOJSON_ENABLE_PROGMEM 1
#include <ArduinoJson.h>

#define mySettings Settings::instance()

class AsyncWebSocket;
class AsyncWebSocketClient;
class Settings {
public:
  struct GeneralSettings {
    // GitHub OTA settings (defaults to public repo, no auth needed)
    String githubUser = "BatsIhor";
    String githubRepo = "UkieLab-AInk";
  };

  struct ConnectionSettings {
    String ssid;
    String password;
  };

  struct AICanvasSettings {
    String deviceName = "aink";
  };

  static Settings *instance();
  static void Initialize(uint32_t saveInterval = 3000);

  size_t jsonSerializeSize();

  void loop();
  void reset();
  void saveLater();
  bool saveSettings(bool force = false);
  String GetUniqueID();
  bool isConfigured();
  bool hasChanged();

  void buildSettingsJson(JsonObject &root);

  bool readSettings();
  bool readSettingsWithNVSCheck();

  bool backupSettingsToNVS();
  bool hasNVSBackup();
  bool restoreSettingsFromNVS();

  GeneralSettings generalSettings;
  ConnectionSettings connectionSettings;
  AICanvasSettings canvasSettings;

  bool busy = false;

protected:
  Settings(uint32_t saveInterval = 3000);
};
