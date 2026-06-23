#ifndef RENDERER_H
#define RENDERER_H

#include <3ds.h>
#include "canvas_state.h"
#include "ui.h"

class Renderer {
public:
    static void renderViewport(CanvasState &canvas, u8 *buffer, int fbWidth, int fbHeight, bool forceFull);
    static void renderMinimap(CanvasState &canvas, bool connected, bool updateAvailable, Color currentColor);
    static void presentTopFrame();
    static void invalidateMinimap();
};

#endif
