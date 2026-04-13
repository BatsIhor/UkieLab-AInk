#pragma once

#include <Arduino.h>

// Dithering modes for converting grayscale/color images to 1-bit B/W
enum DitherMode {
    DITHER_THRESHOLD,       // Simple threshold at 50% gray
    DITHER_ORDERED,         // 8x8 Bayer matrix ordered dithering
    DITHER_FLOYD_STEINBERG  // Floyd-Steinberg error diffusion
};

// Decodes PNG images and renders them to a 1-bit buffer.
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

    // Parameters for binary (non-base64) decoding — avoids base64 overhead
    struct BinaryDecodeParams {
        const uint8_t* pngData;
        size_t pngLen;
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

    // Decode from base64-encoded PNG (used by JSON image op)
    static DecodeResult decode(const DecodeParams& params);

    // Decode from raw PNG bytes — no base64 overhead, uses ~1x image size in memory
    static DecodeResult decodeBinary(const BinaryDecodeParams& params);

    // Decode from a SPIFFS file path — avoids keeping PNG data in RAM during decode
    static DecodeResult decodeFile(const char* path, int16_t destX, int16_t destY,
                                    int16_t destW, int16_t destH, DitherMode dither,
                                    uint8_t* framebuffer, int16_t fbWidth, int16_t fbHeight);

    static const uint8_t _bayerMatrix[8][8];
};
