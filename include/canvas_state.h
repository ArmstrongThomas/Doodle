#ifndef CANVAS_STATE_H
#define CANVAS_STATE_H

#include <3ds.h>
#include <cstdlib>
#include <cstring>
#include <algorithm>

struct DirtyRect {
    int minX, minY, maxX, maxY;
    bool valid;
};

class CanvasState {
public:
    CanvasState();
    ~CanvasState();

    bool allocate(int width, int height);
    bool loadFromCompressed(const u8 *compressedData, size_t compressedSize);
    void markFullDirty();
    void markDirty(int x, int y, int radius);
    void clearDirty();
    void clampOffsets(int screenWidth, int screenHeight);
    void zoomIn();
    void zoomOut();
    float zoomScale() const;
    const char *zoomLabel() const;
    int viewWidth(int screenWidth) const;
    int viewHeight(int screenHeight) const;
    int screenToCanvasX(int screenX) const;
    int screenToCanvasY(int screenY) const;
    int screenDeltaToCanvas(int delta) const;
    void setChannel(const char *name);

    int width;
    int height;
    int size;
    int offsetX;
    int offsetY;
    int zoomLevel;
    u8 *pixels;
    DirtyRect dirty;
    char channel[25];
};

#endif
