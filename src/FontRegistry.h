#pragma once

#include <Arduino.h>
#include "gfxfont.h"
#include <ArduinoJson.h>

// Maps (family, size, bold) -> GFXfont*
// Provides font lookup and enumeration for the API.
class FontRegistry {
public:
    struct FontEntry {
        const char* family;    // "sans", "mono"
        int size;              // point size (9, 12, 18, 24)
        bool bold;
        const GFXfont* font;
    };

    static void init();

    // Find exact match or closest available
    static const GFXfont* find(const char* family, int size, bool bold = false);
    static const GFXfont* findClosest(const char* family, int size, bool bold = false);

    // Enumerate for GET /device
    static void toJson(JsonArray& arr);

    static const FontEntry* getEntries();
    static int getEntryCount();

private:
    static FontEntry _entries[];
    static int _count;
    static bool _initialized;
};
