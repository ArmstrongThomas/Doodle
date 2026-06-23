#include "canvas_state.h"
#include <zlib.h>

CanvasState::CanvasState()
    : width(0), height(0), size(0), offsetX(0), offsetY(0), pixels(NULL)
{
    channel[0] = '\0';
    clearDirty();
}

CanvasState::~CanvasState()
{
    if (pixels)
        free(pixels);
}

bool CanvasState::allocate(int newWidth, int newHeight)
{
    int newSize = newWidth * newHeight * 3;
    if (newSize <= 0)
        return false;

    if (newSize != size)
    {
        u8 *nextPixels = (u8 *)realloc(pixels, newSize);
        if (!nextPixels)
            return false;
        pixels = nextPixels;
    }

    width = newWidth;
    height = newHeight;
    size = newSize;
    memset(pixels, 255, size);
    markFullDirty();
    return true;
}

bool CanvasState::loadFromCompressed(const u8 *compressedData, size_t compressedSize)
{
    if (!pixels || size <= 0)
        return false;

    z_stream strm;
    strm.zalloc = Z_NULL;
    strm.zfree = Z_NULL;
    strm.opaque = Z_NULL;
    strm.avail_in = compressedSize;
    strm.next_in = (Bytef *)compressedData;
    strm.avail_out = size;
    strm.next_out = pixels;

    if (inflateInit(&strm) != Z_OK)
        return false;

    int ret = inflate(&strm, Z_FINISH);
    inflateEnd(&strm);

    if (ret == Z_STREAM_END)
    {
        markFullDirty();
        return true;
    }

    return false;
}

void CanvasState::markFullDirty()
{
    dirty.minX = 0;
    dirty.minY = 0;
    dirty.maxX = width - 1;
    dirty.maxY = height - 1;
    dirty.valid = true;
}

void CanvasState::markDirty(int x, int y, int radius)
{
    int minX = std::max(0, x - radius);
    int minY = std::max(0, y - radius);
    int maxX = std::min(width - 1, x + radius);
    int maxY = std::min(height - 1, y + radius);

    if (!dirty.valid)
    {
        dirty.minX = minX;
        dirty.minY = minY;
        dirty.maxX = maxX;
        dirty.maxY = maxY;
        dirty.valid = true;
        return;
    }

    dirty.minX = std::min(dirty.minX, minX);
    dirty.minY = std::min(dirty.minY, minY);
    dirty.maxX = std::max(dirty.maxX, maxX);
    dirty.maxY = std::max(dirty.maxY, maxY);
}

void CanvasState::clearDirty()
{
    dirty.minX = dirty.minY = dirty.maxX = dirty.maxY = 0;
    dirty.valid = false;
}

void CanvasState::clampOffsets(int screenWidth, int screenHeight)
{
    offsetX = std::max(-20, std::min(offsetX, width - screenWidth));
    offsetY = std::max(-20, std::min(offsetY, height - screenHeight + 100));
}

void CanvasState::setChannel(const char *name)
{
    strncpy(channel, name ? name : "main", sizeof(channel) - 1);
    channel[sizeof(channel) - 1] = '\0';
}
