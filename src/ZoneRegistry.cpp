#include "ZoneRegistry.h"

ZoneRegistry::ZoneRegistry()
{
    for (int i = 0; i < MAX_ZONES; i++) {
        _zones[i].active = false;
    }
}

bool ZoneRegistry::addZone(const String& id, int16_t x, int16_t y, int16_t w, int16_t h)
{
    // Update existing zone
    for (int i = 0; i < MAX_ZONES; i++) {
        if (_zones[i].active && _zones[i].id == id) {
            _zones[i].x = x;
            _zones[i].y = y;
            _zones[i].w = w;
            _zones[i].h = h;
            return true;
        }
    }

    // Find empty slot
    for (int i = 0; i < MAX_ZONES; i++) {
        if (!_zones[i].active) {
            _zones[i].id = id;
            _zones[i].x = x;
            _zones[i].y = y;
            _zones[i].w = w;
            _zones[i].h = h;
            _zones[i].active = true;
            return true;
        }
    }

    return false; // No space
}

bool ZoneRegistry::removeZone(const String& id)
{
    for (int i = 0; i < MAX_ZONES; i++) {
        if (_zones[i].active && _zones[i].id == id) {
            _zones[i].active = false;
            _zones[i].id = "";
            return true;
        }
    }
    return false;
}

void ZoneRegistry::clearAll()
{
    for (int i = 0; i < MAX_ZONES; i++) {
        _zones[i].active = false;
        _zones[i].id = "";
    }
}

const ZoneRegistry::Zone* ZoneRegistry::findZone(const String& id) const
{
    for (int i = 0; i < MAX_ZONES; i++) {
        if (_zones[i].active && _zones[i].id == id) {
            return &_zones[i];
        }
    }
    return nullptr;
}

int ZoneRegistry::count() const
{
    int n = 0;
    for (int i = 0; i < MAX_ZONES; i++) {
        if (_zones[i].active) n++;
    }
    return n;
}

void ZoneRegistry::toJson(JsonArray& arr) const
{
    for (int i = 0; i < MAX_ZONES; i++) {
        if (_zones[i].active) {
            JsonObject z = arr.createNestedObject();
            z["id"] = _zones[i].id;
            z["x"] = _zones[i].x;
            z["y"] = _zones[i].y;
            z["w"] = _zones[i].w;
            z["h"] = _zones[i].h;
        }
    }
}

bool ZoneRegistry::fromJson(const JsonArray& arr)
{
    clearAll();
    for (size_t i = 0; i < arr.size() && i < MAX_ZONES; i++) {
        JsonObject z = arr[i];
        if (!z.containsKey("id")) continue;
        addZone(
            z["id"].as<String>(),
            z["x"] | 0,
            z["y"] | 0,
            z["w"] | 100,
            z["h"] | 100
        );
    }
    return true;
}
