#ifndef RENDERER_H
#define RENDERER_H

#include <3ds.h>
#include "canvas_state.h"
#include "ui.h"

enum TopScreenMode {
    TOP_MODE_CANVAS = 0,
    TOP_MODE_CHANNELS = 1,
    TOP_MODE_CONTROLS = 2
};

class Renderer {
public:
    static void renderViewport(CanvasState &canvas, u8 *buffer, int fbWidth, int fbHeight, bool forceFull);
    static void renderTop(CanvasState &canvas, bool connected, bool updateAvailable, Color currentColor,
                          int brushSize, int brushShape, TopScreenMode mode,
                          char channels[][25], int channelCount, int selectedChannel);
    static void presentTopFrame();
    static void invalidateMinimap();
};

#endif
