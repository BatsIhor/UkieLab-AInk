#pragma once

#include <Arduino.h>

// Dithering modes for converting grayscale/color images to 1-bit B/W
enum DitherMode {
    DITHER_THRESHOLD,       // Simple threshold at 50% gray
    DITHER_ORDERED,         // 8x8 Bayer matrix ordered dithering
    DITHER_FLOYD_STEINBERG  // Floyd-Steinberg error diffusion
};

// Decodes a base64-encoded PNG image and renders it to a 1-bit buffer.
// Uses PNGdec library for streaming decode (low memory footprint).
class ImageDecoder {
public:
    struct DecodeParams {
        const char* base64Data;
        size_t base64Len;
        int16_t destX;
        int16_t destY;
        int16_t destW;   // 0 = use source width
        int16_t destH;   // 0 = use source height
        DitherMode dither;
        uint8_t* framebuffer;
        int16_t fbWidth;
        int16_t fbHeight;
    };

    struct DecodeResult {
        bool success;
        int16_t width;
        int16_t height;
        String error;
    };

    static DecodeResult decode(const DecodeParams& params);

    static const uint8_t _bayerMatrix[8][8];
};
