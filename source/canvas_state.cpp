#include "canvas_state.h"
#include <zlib.h>
#include <cmath>

CanvasState::CanvasState()
    : width(0), height(0), size(0), offsetX(0), offsetY(0), zoomLevel(0), pixels(NULL)
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
    int visibleWidth = viewWidth(screenWidth);
    int visibleHeight = viewHeight(screenHeight);
    offsetX = std::max(-20, std::min(offsetX, width - visibleWidth));
    offsetY = std::max(-20, std::min(offsetY, height - visibleHeight + 100));
}

void CanvasState::zoomIn()
{
    zoomLevel = std::min(2, zoomLevel + 1);
}

void CanvasState::zoomOut()
{
    zoomLevel = std::max(-1, zoomLevel - 1);
}

float CanvasState::zoomScale() const
{
    if (zoomLevel <= -1) return 0.5f;
    if (zoomLevel == 1) return 2.0f;
    if (zoomLevel >= 2) return 4.0f;
    return 1.0f;
}

const char *CanvasState::zoomLabel() const
{
    if (zoomLevel <= -1) return "0.5X";
    if (zoomLevel == 1) return "2X";
    if (zoomLevel >= 2) return "4X";
    return "1X";
}

int CanvasState::viewWidth(int screenWidth) const
{
    if (zoomLevel <= -1) return screenWidth * 2;
    if (zoomLevel == 1) return std::max(1, screenWidth / 2);
    if (zoomLevel >= 2) return std::max(1, screenWidth / 4);
    return screenWidth;
}

int CanvasState::viewHeight(int screenHeight) const
{
    if (zoomLevel <= -1) return screenHeight * 2;
    if (zoomLevel == 1) return std::max(1, screenHeight / 2);
    if (zoomLevel >= 2) return std::max(1, screenHeight / 4);
    return screenHeight;
}

int CanvasState::screenToCanvasX(int screenX) const
{
    if (zoomLevel <= -1) return offsetX + screenX * 2;
    if (zoomLevel == 1) return offsetX + screenX / 2;
    if (zoomLevel >= 2) return offsetX + screenX / 4;
    return offsetX + screenX;
}

int CanvasState::screenToCanvasY(int screenY) const
{
    if (zoomLevel <= -1) return offsetY + screenY * 2;
    if (zoomLevel == 1) return offsetY + screenY / 2;
    if (zoomLevel >= 2) return offsetY + screenY / 4;
    return offsetY + screenY;
}

int CanvasState::screenDeltaToCanvas(int delta) const
{
    float scaled = (float)delta / zoomScale();
    return (int)std::round(scaled);
}

void CanvasState::setChannel(const char *name)
{
    strncpy(channel, name ? name : "main", sizeof(channel) - 1);
    channel[sizeof(channel) - 1] = '\0';
}
