#pragma once

#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Update.h>
#include <ArduinoJson.h>

// Update types
enum class OTAUpdateType {
    FIRMWARE,
    SPIFFS
};

// Update status
enum class OTAUpdateStatus {
    IDLE,
    CHECKING,
    UPDATE_AVAILABLE,
    DOWNLOADING,
    INSTALLING,
    COMPLETE,
    ERROR_NETWORK,
    ERROR_NO_UPDATE,
    ERROR_DOWNLOAD,
    ERROR_INSTALL,
    ERROR_VERIFICATION
};

// Release information from GitHub
struct GitHubRelease {
    String tagName;
    String name;
    String body;  // Release notes
    String publishedAt;
    String firmwareUrl;
    String spiffsUrl;
    size_t firmwareSize;
    size_t spiffsSize;
    bool isValid;
};

// Progress callback function type
typedef std::function<void(size_t current, size_t total)> ProgressCallback;

class OTAUpdateManager {
public:
    OTAUpdateManager();
    ~OTAUpdateManager();

    // Configuration
    void setGitHubRepo(const String& user, const String& repo);
    void setGitHubPAT(const String& token);
    void setCurrentEnvironment(const String& environment);
    void setCurrentVersion(const String& version);

    // Update checking
    bool checkForUpdates(bool force = false);
    GitHubRelease getLatestRelease();
    bool isUpdateAvailable() const;
    String getCurrentVersion() const;
    String getLatestVersion() const;

    // Update installation
    bool installUpdate(OTAUpdateType type, const String& url, ProgressCallback callback = nullptr);
    bool installFirmwareFromGitHub(ProgressCallback callback = nullptr);
    bool installSPIFFSFromGitHub(ProgressCallback callback = nullptr);

    // Status and progress
    OTAUpdateStatus getStatus() const;
    int getProgress() const;  // 0-100
    String getStatusMessage() const;
    String getErrorMessage() const;

    // Utility
    static String getEnvironmentFromBuildFlags();

private:
    // GitHub API
    bool fetchLatestRelease();
    bool fetchReleaseAssets(GitHubRelease& release);
    bool parseReleaseJSONMinimal(const String& json, GitHubRelease& release);

    // HTTP operations
    bool downloadAndInstall(const String& url, int updateType, ProgressCallback callback);
    static void progressCallbackWrapper(size_t current, size_t total);

    // Version comparison
    bool isNewerVersion(const String& current, const String& latest);
    bool parseSemanticVersion(const String& version, int& major, int& minor, int& patch);
    int parseVersionNumber(const String& version);

    // Configuration
    String _githubUser;
    String _githubRepo;
    String _githubPAT;
    String _currentEnvironment;
    String _currentVersion;

    // State
    GitHubRelease _latestRelease;
    OTAUpdateStatus _status;
    String _statusMessage;
    String _errorMessage;
    int _progress;  // 0-100

    // Static callback holder for Update library
    static ProgressCallback _progressCallback;

    // Constants
    static const char* GITHUB_API_HOST;
    static const int GITHUB_API_PORT;
    static const int HTTP_TIMEOUT;
    static const size_t MAX_DOWNLOAD_SIZE;
};
