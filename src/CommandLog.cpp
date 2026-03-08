#include "CommandLog.h"

CommandLog::CommandLog()
    : _count(0), _frameId(0), _timestamp(0), _totalSize(0)
{
}

void CommandLog::clear()
{
    for (int i = 0; i < _count; i++) {
        _commands[i].op = "";
        _commands[i].meta = "";
    }
    _count = 0;
    _totalSize = 0;
    _frameMeta = "";
}

void CommandLog::beginFrame()
{
    clear();
}

uint32_t CommandLog::endFrame()
{
    _frameId++;
    _timestamp = millis();
    return _frameId;
}

void CommandLog::addCommand(const String& op, int16_t x, int16_t y, const String& meta)
{
    if (_count >= MAX_COMMANDS) return;

    size_t entrySize = op.length() + meta.length() + 8;
    if (_totalSize + entrySize > MAX_LOG_SIZE) return;

    _commands[_count].op = op;
    _commands[_count].x = x;
    _commands[_count].y = y;
    _commands[_count].meta = meta;
    _count++;
    _totalSize += entrySize;
}

void CommandLog::setFrameMeta(const String& meta)
{
    _frameMeta = meta;
}

void CommandLog::toJson(JsonObject& root) const
{
    root["frame_id"] = _frameId;
    root["timestamp"] = _timestamp;
    root["command_count"] = _count;

    if (_frameMeta.length() > 0) {
        DynamicJsonDocument metaDoc(512);
        if (deserializeJson(metaDoc, _frameMeta) == DeserializationError::Ok) {
            root["meta"] = metaDoc.as<JsonObject>();
        }
    }

    JsonArray cmds = root.createNestedArray("commands");
    for (int i = 0; i < _count; i++) {
        JsonObject cmd = cmds.createNestedObject();
        cmd["op"] = _commands[i].op;
        if (_commands[i].x != 0 || _commands[i].y != 0) {
            cmd["x"] = _commands[i].x;
            cmd["y"] = _commands[i].y;
        }
        if (_commands[i].meta.length() > 0) {
            DynamicJsonDocument cmdMeta(256);
            if (deserializeJson(cmdMeta, _commands[i].meta) == DeserializationError::Ok) {
                cmd["meta"] = cmdMeta.as<JsonObject>();
            }
        }
    }
}
