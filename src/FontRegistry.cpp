#include "FontRegistry.h"

#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSans12pt7b.h>
#include <Fonts/FreeSans18pt7b.h>
#include <Fonts/FreeSans24pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSansBold24pt7b.h>
#include <Fonts/FreeMonoBold9pt7b.h>

FontRegistry::FontEntry FontRegistry::_entries[] = {
    {"sans",  9,  false, &FreeSans9pt7b},
    {"sans", 12,  false, &FreeSans12pt7b},
    {"sans", 18,  false, &FreeSans18pt7b},
    {"sans", 24,  false, &FreeSans24pt7b},
    {"sans",  9,  true,  &FreeSansBold9pt7b},
    {"sans", 12,  true,  &FreeSansBold12pt7b},
    {"sans", 18,  true,  &FreeSansBold18pt7b},
    {"sans", 24,  true,  &FreeSansBold24pt7b},
    {"mono",  9,  true,  &FreeMonoBold9pt7b},
};

int FontRegistry::_count = sizeof(_entries) / sizeof(_entries[0]);
bool FontRegistry::_initialized = false;

void FontRegistry::init()
{
    _initialized = true;
}

const GFXfont* FontRegistry::find(const char* family, int size, bool bold)
{
    for (int i = 0; i < _count; i++) {
        if (strcmp(_entries[i].family, family) == 0 &&
            _entries[i].size == size &&
            _entries[i].bold == bold) {
            return _entries[i].font;
        }
    }
    return nullptr;
}

const GFXfont* FontRegistry::findClosest(const char* family, int size, bool bold)
{
    // Try exact match first
    const GFXfont* exact = find(family, size, bold);
    if (exact) return exact;

    // Try same family, closest size
    const GFXfont* best = nullptr;
    int bestDiff = 999;
    for (int i = 0; i < _count; i++) {
        if (strcmp(_entries[i].family, family) == 0 && _entries[i].bold == bold) {
            int diff = abs(_entries[i].size - size);
            if (diff < bestDiff) {
                bestDiff = diff;
                best = _entries[i].font;
            }
        }
    }
    if (best) return best;

    // Try same family, any bold
    for (int i = 0; i < _count; i++) {
        if (strcmp(_entries[i].family, family) == 0) {
            int diff = abs(_entries[i].size - size);
            if (diff < bestDiff) {
                bestDiff = diff;
                best = _entries[i].font;
            }
        }
    }
    if (best) return best;

    // Fallback to sans
    return findClosest("sans", size, bold);
}

void FontRegistry::toJson(JsonArray& arr)
{
    for (int i = 0; i < _count; i++) {
        JsonObject entry = arr.createNestedObject();
        entry["family"] = _entries[i].family;
        entry["size"] = _entries[i].size;
        entry["bold"] = _entries[i].bold;
        entry["yAdvance"] = _entries[i].font->yAdvance;
        entry["lineHeight"] = (int)(_entries[i].font->yAdvance * 1.2f);
    }
}

const FontRegistry::FontEntry* FontRegistry::getEntries()
{
    return _entries;
}

int FontRegistry::getEntryCount()
{
    return _count;
}
