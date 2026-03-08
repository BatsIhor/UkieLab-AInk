#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

// Stores the last frame's drawing commands for GET /canvas retrieval.
// Caps at MAX_COMMANDS commands or MAX_LOG_SIZE bytes.
class CommandLog {
public:
    static const int MAX_COMMANDS = 128;
    static const size_t MAX_LOG_SIZE = 12288;

    CommandLog();

    void clear();
    void beginFrame();
    uint32_t endFrame();

    // Store a compact summary of a command (not the full JSON)
    void addCommand(const String& op, int16_t x = 0, int16_t y = 0, const String& meta = "");

    // Store frame-level metadata
    void setFrameMeta(const String& meta);

    uint32_t getFrameId() const { return _frameId; }
    unsigned long getTimestamp() const { return _timestamp; }
    int getCommandCount() const { return _count; }

    void toJson(JsonObject& root) const;

private:
    struct CmdEntry {
        String op;
        int16_t x, y;
        String meta; // Raw JSON string of meta object (empty if none)
    };

    CmdEntry _commands[MAX_COMMANDS];
    int _count;
    uint32_t _frameId;
    unsigned long _timestamp;
    size_t _totalSize;
    String _frameMeta; // Frame-level metadata JSON string
};
