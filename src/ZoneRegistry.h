#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

// Named display zones for partial updates.
// Max 16 zones. Each zone has an ID, position, and dimensions.
class ZoneRegistry {
public:
    struct Zone {
        String id;
        int16_t x, y, w, h;
        bool active;
    };

    static const int MAX_ZONES = 16;

    ZoneRegistry();

    bool addZone(const String& id, int16_t x, int16_t y, int16_t w, int16_t h);
    bool removeZone(const String& id);
    void clearAll();

    const Zone* findZone(const String& id) const;
    int count() const;

    void toJson(JsonArray& arr) const;
    bool fromJson(const JsonArray& arr);

private:
    Zone _zones[MAX_ZONES];
};
