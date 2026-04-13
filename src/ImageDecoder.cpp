#include "ImageDecoder.h"
#include <PNGdec.h>
#include <mbedtls/base64.h>
#include <SPIFFS.h>
#include <stdlib.h>
#include <string.h>
#include <new>

const uint8_t ImageDecoder::_bayerMatrix[8][8] = {
    {  0, 32,  8, 40,  2, 34, 10, 42},
    { 48, 16, 56, 24, 50, 18, 58, 26},
    { 12, 44,  4, 36, 14, 46,  6, 38},
    { 60, 28, 52, 20, 62, 30, 54, 22},
    {  3, 35, 11, 43,  1, 33,  9, 41},
    { 51, 19, 59, 27, 49, 17, 57, 25},
    { 15, 47,  7, 39, 13, 45,  5, 37},
    { 63, 31, 55, 23, 61, 29, 53, 21}
};

// Static state for PNG callback
struct PNGCallbackState {
    uint8_t* framebuffer;
    int16_t fbWidth;
    int16_t fbHeight;
    int16_t destX;
    int16_t destY;
    int16_t destW;
    int16_t destH;
    int16_t srcW;
    int16_t srcH;
    DitherMode dither;
    int16_t* errorRow;  // For Floyd-Steinberg
};

static PNGCallbackState pngState;

static void setPixelInBuffer(uint8_t* fb, int16_t fbW, int16_t fbH, int16_t x, int16_t y, bool white)
{
    if (x < 0 || x >= fbW || y < 0 || y >= fbH) return;
    uint32_t idx = (uint32_t)y * (fbW / 8) + (x >> 3);
    uint8_t mask = 0x80 >> (x & 7);
    if (white) {
        fb[idx] |= mask;
    } else {
        fb[idx] &= ~mask;
    }
}

// Extract grayscale value from a pixel in the PNG raw data
static uint8_t getGrayFromPixel(PNGDRAW* pDraw, int x)
{
    uint8_t* pPixels = pDraw->pPixels;
    int pixelType = pDraw->iPixelType;
    int bpp = pDraw->iBpp;

    switch (pixelType) {
        case 0: // Grayscale
            if (bpp == 8) return pPixels[x];
            if (bpp == 16) return pPixels[x * 2]; // use high byte
            if (bpp == 4) return (pPixels[x / 2] >> ((1 - (x & 1)) * 4)) * 17;
            if (bpp == 2) return (pPixels[x / 4] >> ((3 - (x & 3)) * 2)) * 85;
            if (bpp == 1) return ((pPixels[x / 8] >> (7 - (x & 7))) & 1) * 255;
            return 128;

        case 2: // RGB
            if (bpp == 8) {
                uint8_t r = pPixels[x * 3];
                uint8_t g = pPixels[x * 3 + 1];
                uint8_t b = pPixels[x * 3 + 2];
                return (uint8_t)((r * 77 + g * 150 + b * 29) >> 8);
            }
            return 128;

        case 3: // Indexed (palette)
            if (pDraw->pPalette) {
                uint8_t idx;
                if (bpp == 8) idx = pPixels[x];
                else if (bpp == 4) idx = (pPixels[x / 2] >> ((1 - (x & 1)) * 4)) & 0x0F;
                else if (bpp == 2) idx = (pPixels[x / 4] >> ((3 - (x & 3)) * 2)) & 0x03;
                else if (bpp == 1) idx = (pPixels[x / 8] >> (7 - (x & 7))) & 1;
                else return 128;
                uint8_t r = pDraw->pPalette[idx * 3];
                uint8_t g = pDraw->pPalette[idx * 3 + 1];
                uint8_t b = pDraw->pPalette[idx * 3 + 2];
                return (uint8_t)((r * 77 + g * 150 + b * 29) >> 8);
            }
            return 128;

        case 4: // Grayscale + Alpha
            if (bpp == 8) return pPixels[x * 2]; // gray channel
            return 128;

        case 6: // RGBA
            if (bpp == 8) {
                uint8_t r = pPixels[x * 4];
                uint8_t g = pPixels[x * 4 + 1];
                uint8_t b = pPixels[x * 4 + 2];
                uint8_t a = pPixels[x * 4 + 3];
                // Alpha blend against white background
                r = (r * a + 255 * (255 - a)) / 255;
                g = (g * a + 255 * (255 - a)) / 255;
                b = (b * a + 255 * (255 - a)) / 255;
                return (uint8_t)((r * 77 + g * 150 + b * 29) >> 8);
            }
            return 128;
    }
    return 128;
}

static int pngDrawCallback(PNGDRAW* pDraw)
{
    PNGCallbackState& st = pngState;
    int srcY = pDraw->y;
    int srcWidth = pDraw->iWidth;

    // Nearest-neighbor scaling Y
    int destRow;
    if (st.destH != st.srcH && st.destH > 0 && st.srcH > 0) {
        destRow = srcY * st.destH / st.srcH;
    } else {
        destRow = srcY;
    }

    int absY = st.destY + destRow;
    if (absY < 0 || absY >= st.fbHeight) return 1;

    int destWidth = (st.destW > 0 && st.destW != st.srcW) ? st.destW : srcWidth;

    // Reset error row for this line (F-S)
    if (st.errorRow) {
        memset(st.errorRow, 0, destWidth * sizeof(int16_t));
    }

    for (int dx = 0; dx < destWidth; dx++) {
        // Nearest-neighbor scaling X
        int srcX;
        if (destWidth != srcWidth) {
            srcX = dx * srcWidth / destWidth;
        } else {
            srcX = dx;
        }
        if (srcX >= srcWidth) srcX = srcWidth - 1;

        int absX = st.destX + dx;
        if (absX < 0 || absX >= st.fbWidth) continue;

        uint8_t gray = getGrayFromPixel(pDraw, srcX);

        bool white;
        switch (st.dither) {
            case DITHER_ORDERED: {
                uint8_t threshold = ImageDecoder::_bayerMatrix[absY & 7][absX & 7] * 4;
                white = gray > threshold;
                break;
            }
            case DITHER_FLOYD_STEINBERG: {
                int val = gray;
                if (st.errorRow) val += st.errorRow[dx];
                if (val < 0) val = 0;
                if (val > 255) val = 255;
                white = val > 127;
                int error = val - (white ? 255 : 0);
                if (st.errorRow && dx + 1 < destWidth) {
                    st.errorRow[dx + 1] += error * 7 / 16;
                }
                break;
            }
            case DITHER_THRESHOLD:
            default:
                white = gray > 127;
                break;
        }

        setPixelInBuffer(st.framebuffer, st.fbWidth, st.fbHeight, absX, absY, white);
    }

    return 1; // continue decoding
}

// Internal helper: setup PNG callback state and decode
static ImageDecoder::DecodeResult decodePNG(PNG& png, const uint8_t* fb, int16_t fbW, int16_t fbH,
                                             int16_t destX, int16_t destY, int16_t destW, int16_t destH,
                                             DitherMode dither, int16_t srcW, int16_t srcH)
{
    ImageDecoder::DecodeResult result = {false, srcW, srcH, ""};

    pngState.framebuffer = const_cast<uint8_t*>(fb);
    pngState.fbWidth = fbW;
    pngState.fbHeight = fbH;
    pngState.destX = destX;
    pngState.destY = destY;
    pngState.destW = destW > 0 ? destW : srcW;
    pngState.destH = destH > 0 ? destH : srcH;
    pngState.srcW = srcW;
    pngState.srcH = srcH;
    pngState.dither = dither;

    // Allocate error diffusion row if needed
    int16_t* errorRow = nullptr;
    if (dither == DITHER_FLOYD_STEINBERG) {
        errorRow = (int16_t*)calloc(pngState.destW, sizeof(int16_t));
    }
    pngState.errorRow = errorRow;

    int rc = png.decode(nullptr, 0);

    free(errorRow);
    png.close();

    if (rc != PNG_SUCCESS) {
        result.error = "PNG decode failed: " + String(rc);
        return result;
    }

    result.success = true;
    return result;
}

ImageDecoder::DecodeResult ImageDecoder::decode(const DecodeParams& params)
{
    DecodeResult result = {false, 0, 0, ""};

    if (!params.base64Data || params.base64Len == 0) {
        result.error = "No image data";
        return result;
    }

    // Decode base64 to binary
    size_t binLen = 0;
    int ret = mbedtls_base64_decode(nullptr, 0, &binLen,
                                     (const uint8_t*)params.base64Data, params.base64Len);
    if (ret != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL || binLen == 0) {
        result.error = "Invalid base64";
        return result;
    }

    if (binLen > 512 * 1024) {
        result.error = "Image too large";
        return result;
    }

    uint8_t* binData = (uint8_t*)malloc(binLen);
    if (!binData) {
        result.error = "Out of memory for image decode";
        return result;
    }

    size_t actualLen = 0;
    ret = mbedtls_base64_decode(binData, binLen, &actualLen,
                                 (const uint8_t*)params.base64Data, params.base64Len);
    if (ret != 0) {
        free(binData);
        result.error = "Base64 decode failed";
        return result;
    }

    // PNGIMAGE struct is ~45KB (zlib state + buffers) — must be heap-allocated
    // to avoid stack overflow on async_tcp or small FreeRTOS tasks.
    PNG* png = new (std::nothrow) PNG();
    if (!png) {
        free(binData);
        result.error = "Out of memory for PNG decoder";
        return result;
    }

    int rc = png->openRAM(binData, actualLen, pngDrawCallback);
    if (rc != PNG_SUCCESS) {
        delete png;
        free(binData);
        result.error = "PNG open failed: " + String(rc);
        return result;
    }

    result = decodePNG(*png, params.framebuffer, params.fbWidth, params.fbHeight,
                       params.destX, params.destY, params.destW, params.destH,
                       params.dither, png->getWidth(), png->getHeight());

    delete png;
    free(binData);
    return result;
}

ImageDecoder::DecodeResult ImageDecoder::decodeBinary(const BinaryDecodeParams& params)
{
    DecodeResult result = {false, 0, 0, ""};

    if (!params.pngData || params.pngLen == 0) {
        result.error = "No image data";
        return result;
    }

    // PNGIMAGE struct is ~45KB — heap-allocate to avoid stack overflow
    PNG* png = new (std::nothrow) PNG();
    if (!png) {
        result.error = "Out of memory for PNG decoder";
        return result;
    }

    int rc = png->openRAM(const_cast<uint8_t*>(params.pngData), params.pngLen, pngDrawCallback);
    if (rc != PNG_SUCCESS) {
        delete png;
        result.error = "PNG open failed: " + String(rc);
        return result;
    }

    result = decodePNG(*png, params.framebuffer, params.fbWidth, params.fbHeight,
                       params.destX, params.destY, params.destW, params.destH,
                       params.dither, png->getWidth(), png->getHeight());

    delete png;
    return result;
}

// SPIFFS file callbacks for PNGdec
static File spiffsFile;

static void* spiffsOpen(const char* filename, int32_t* pFileSize)
{
    spiffsFile = SPIFFS.open(filename, "r");
    if (!spiffsFile) return nullptr;
    *pFileSize = spiffsFile.size();
    return &spiffsFile;
}

static void spiffsClose(void* pHandle)
{
    if (pHandle) {
        ((File*)pHandle)->close();
    }
}

static int32_t spiffsRead(PNGFILE* pFile, uint8_t* pBuf, int32_t iLen)
{
    File* f = (File*)pFile->fHandle;
    return f->read(pBuf, iLen);
}

static int32_t spiffsSeek(PNGFILE* pFile, int32_t iPosition)
{
    File* f = (File*)pFile->fHandle;
    if (iPosition < 0) iPosition = 0;
    f->seek(iPosition);
    return iPosition;
}

ImageDecoder::DecodeResult ImageDecoder::decodeFile(const char* path,
    int16_t destX, int16_t destY, int16_t destW, int16_t destH,
    DitherMode dither, uint8_t* framebuffer, int16_t fbWidth, int16_t fbHeight)
{
    DecodeResult result = {false, 0, 0, ""};

    PNG* png = new (std::nothrow) PNG();
    if (!png) {
        result.error = "Out of memory for PNG decoder";
        return result;
    }

    int rc = png->open(path, spiffsOpen, spiffsClose, spiffsRead, spiffsSeek, pngDrawCallback);
    if (rc != PNG_SUCCESS) {
        delete png;
        result.error = "PNG open failed: " + String(rc);
        return result;
    }

    result = decodePNG(*png, framebuffer, fbWidth, fbHeight,
                       destX, destY, destW, destH,
                       dither, png->getWidth(), png->getHeight());

    delete png;
    return result;
}

