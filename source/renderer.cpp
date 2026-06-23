#include "renderer.h"
#include <algorithm>
#include <math.h>
#include <stdio.h>
#include <string.h>

static const int MINIMAP_W = 256;
static const int MINIMAP_H = 144;
static const int TOP_SCREEN_W = 400;
static const int TOP_SCREEN_H = 240;
static u8 minimapCache[MINIMAP_W * MINIMAP_H * 3];
static u8 topFrame[TOP_SCREEN_W * TOP_SCREEN_H * 3];
static bool minimapCacheValid = false;
static bool topFrameValid = false;
static int minimapFrameCounter = 0;

void Renderer::renderViewport(CanvasState &canvas, u8 *buffer, int fbWidth, int fbHeight, bool forceFull)
{
    if (!canvas.pixels)
        return;

    int startY = 0;
    int endY = fbHeight - 1;
    int startX = 0;
    int endX = fbWidth - 1;

    for (int y = startY; y <= endY; y++)
    {
        for (int x = startX; x <= endX; x++)
        {
            int canvasX = y + canvas.offsetX;
            int canvasY = (fbWidth - 1 - x) + canvas.offsetY;
            int bufferIdx = 3 * (y * fbWidth + x);
            if (canvasX >= 0 && canvasX < canvas.width && canvasY >= 0 && canvasY < canvas.height)
            {
                int canvasIdx = 3 * (canvasY * canvas.width + canvasX);
                buffer[bufferIdx] = canvas.pixels[canvasIdx + 2];
                buffer[bufferIdx + 1] = canvas.pixels[canvasIdx + 1];
                buffer[bufferIdx + 2] = canvas.pixels[canvasIdx];
            }
            else
            {
                buffer[bufferIdx] = buffer[bufferIdx + 1] = buffer[bufferIdx + 2] = 240;
            }
        }
    }
}

static void setTopPixel(u8 *target, int width, int height, int screenX, int screenY, u8 r, u8 g, u8 b)
{
    if (screenX < 0 || screenX >= width || screenY < 0 || screenY >= height)
        return;

    int idx = 3 * (screenY * width + screenX);
    target[idx] = r;
    target[idx + 1] = g;
    target[idx + 2] = b;
}

static void fillTopRect(u8 *target, int width, int height, int x, int y, int w, int h, u8 r, u8 g, u8 b)
{
    for (int py = y; py < y + h; py++)
        for (int px = x; px < x + w; px++)
            setTopPixel(target, width, height, px, py, r, g, b);
}

static void strokeTopRect(u8 *target, int width, int height, int x, int y, int w, int h, u8 r, u8 g, u8 b)
{
    for (int px = x; px < x + w; px++)
    {
        setTopPixel(target, width, height, px, y, r, g, b);
        setTopPixel(target, width, height, px, y + h - 1, r, g, b);
    }
    for (int py = y; py < y + h; py++)
    {
        setTopPixel(target, width, height, x, py, r, g, b);
        setTopPixel(target, width, height, x + w - 1, py, r, g, b);
    }
}

static void drawGlyph(u8 *target, int width, int height, int x, int y, char c, u8 r, u8 g, u8 b)
{
    u8 glyph[7] = {0};
    switch (c)
    {
        case '0': { u8 g[7] = {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}; memcpy(glyph,g,7); break; }
        case '1': { u8 g[7] = {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}; memcpy(glyph,g,7); break; }
        case '2': { u8 g[7] = {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}; memcpy(glyph,g,7); break; }
        case '3': { u8 g[7] = {0x1E,0x01,0x01,0x0E,0x01,0x01,0x1E}; memcpy(glyph,g,7); break; }
        case '4': { u8 g[7] = {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}; memcpy(glyph,g,7); break; }
        case '5': { u8 g[7] = {0x1F,0x10,0x10,0x1E,0x01,0x01,0x1E}; memcpy(glyph,g,7); break; }
        case '6': { u8 g[7] = {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}; memcpy(glyph,g,7); break; }
        case '7': { u8 g[7] = {0x1F,0x01,0x02,0x04,0x08,0x08,0x08}; memcpy(glyph,g,7); break; }
        case '8': { u8 g[7] = {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}; memcpy(glyph,g,7); break; }
        case '9': { u8 g[7] = {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}; memcpy(glyph,g,7); break; }
        case 'A': { u8 g[7] = {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}; memcpy(glyph,g,7); break; }
        case 'B': { u8 g[7] = {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}; memcpy(glyph,g,7); break; }
        case 'C': { u8 g[7] = {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}; memcpy(glyph,g,7); break; }
        case 'D': { u8 g[7] = {0x1E,0x11,0x11,0x11,0x11,0x11,0x1E}; memcpy(glyph,g,7); break; }
        case 'E': { u8 g[7] = {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}; memcpy(glyph,g,7); break; }
        case 'F': { u8 g[7] = {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}; memcpy(glyph,g,7); break; }
        case 'G': { u8 g[7] = {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F}; memcpy(glyph,g,7); break; }
        case 'H': { u8 g[7] = {0x11,0x11,0x11,0x1F,0x11,0x11,0x11}; memcpy(glyph,g,7); break; }
        case 'I': { u8 g[7] = {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}; memcpy(glyph,g,7); break; }
        case 'J': { u8 g[7] = {0x07,0x02,0x02,0x02,0x12,0x12,0x0C}; memcpy(glyph,g,7); break; }
        case 'K': { u8 g[7] = {0x11,0x12,0x14,0x18,0x14,0x12,0x11}; memcpy(glyph,g,7); break; }
        case 'L': { u8 g[7] = {0x10,0x10,0x10,0x10,0x10,0x10,0x1F}; memcpy(glyph,g,7); break; }
        case 'M': { u8 g[7] = {0x11,0x1B,0x15,0x15,0x11,0x11,0x11}; memcpy(glyph,g,7); break; }
        case 'N': { u8 g[7] = {0x11,0x19,0x15,0x13,0x11,0x11,0x11}; memcpy(glyph,g,7); break; }
        case 'O': { u8 g[7] = {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}; memcpy(glyph,g,7); break; }
        case 'P': { u8 g[7] = {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}; memcpy(glyph,g,7); break; }
        case 'Q': { u8 g[7] = {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}; memcpy(glyph,g,7); break; }
        case 'R': { u8 g[7] = {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}; memcpy(glyph,g,7); break; }
        case 'S': { u8 g[7] = {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}; memcpy(glyph,g,7); break; }
        case 'T': { u8 g[7] = {0x1F,0x04,0x04,0x04,0x04,0x04,0x04}; memcpy(glyph,g,7); break; }
        case 'U': { u8 g[7] = {0x11,0x11,0x11,0x11,0x11,0x11,0x0E}; memcpy(glyph,g,7); break; }
        case 'V': { u8 g[7] = {0x11,0x11,0x11,0x11,0x11,0x0A,0x04}; memcpy(glyph,g,7); break; }
        case 'W': { u8 g[7] = {0x11,0x11,0x11,0x15,0x15,0x15,0x0A}; memcpy(glyph,g,7); break; }
        case 'X': { u8 g[7] = {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}; memcpy(glyph,g,7); break; }
        case 'Y': { u8 g[7] = {0x11,0x11,0x0A,0x04,0x04,0x04,0x04}; memcpy(glyph,g,7); break; }
        case 'Z': { u8 g[7] = {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}; memcpy(glyph,g,7); break; }
        case '-': { u8 g[7] = {0x00,0x00,0x00,0x1F,0x00,0x00,0x00}; memcpy(glyph,g,7); break; }
        case ':': { u8 g[7] = {0x00,0x04,0x00,0x00,0x04,0x00,0x00}; memcpy(glyph,g,7); break; }
        case '.': { u8 g[7] = {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C}; memcpy(glyph,g,7); break; }
        case '/': { u8 g[7] = {0x01,0x01,0x02,0x04,0x08,0x10,0x10}; memcpy(glyph,g,7); break; }
        case '<': { u8 g[7] = {0x02,0x04,0x08,0x10,0x08,0x04,0x02}; memcpy(glyph,g,7); break; }
        case '>': { u8 g[7] = {0x08,0x04,0x02,0x01,0x02,0x04,0x08}; memcpy(glyph,g,7); break; }
        default: return;
    }

    for (int gy = 0; gy < 7; gy++)
        for (int gx = 0; gx < 5; gx++)
            if (glyph[gy] & (1 << (4 - gx)))
                setTopPixel(target, width, height, x + gx, y + gy, r, g, b);
}

static void drawText(u8 *target, int width, int height, int x, int y, const char *text, u8 r, u8 g, u8 b)
{
    int cursor = x;
    while (*text)
    {
        if (*text != ' ')
            drawGlyph(target, width, height, cursor, y, *text, r, g, b);
        cursor += 6;
        text++;
    }
}

static void drawUpperText(u8 *target, int width, int height, int x, int y, const char *text, u8 r, u8 g, u8 b)
{
    char buffer[64];
    strncpy(buffer, text ? text : "", sizeof(buffer) - 1);
    buffer[sizeof(buffer) - 1] = '\0';
    for (char *p = buffer; *p; p++)
        if (*p >= 'a' && *p <= 'z')
            *p = *p - 'a' + 'A';
    drawText(target, width, height, x, y, buffer, r, g, b);
}

static void drawTopChrome(bool connected, bool updateAvailable)
{
    fillTopRect(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 0, 0, TOP_SCREEN_W, TOP_SCREEN_H, 232, 236, 239);
    fillTopRect(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 0, 0, TOP_SCREEN_W, 30, 24, 33, 38);
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 12, 10, "COLLAB DOODLE", 245, 248, 250);
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 300, 10, connected ? "CONN" : "OFF", connected ? 94 : 255, connected ? 234 : 115, connected ? 212 : 115);
    if (updateAvailable)
        drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 348, 10, "UP", 255, 214, 102);

    char version[40];
    snprintf(version, sizeof(version), "V %s", APP_VERSION);
    int versionX = TOP_SCREEN_W - 12 - (int)strlen(version) * 6;
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, versionX, 224, version, 104, 114, 124);
}

void Renderer::invalidateMinimap()
{
    minimapCacheValid = false;
    topFrameValid = false;
}

static void updateMinimapCache(CanvasState &canvas)
{
    if (!canvas.pixels || canvas.width <= 0 || canvas.height <= 0)
        return;

    for (int y = 0; y < MINIMAP_H; y++)
    {
        for (int x = 0; x < MINIMAP_W; x++)
        {
            int cx = x * canvas.width / MINIMAP_W;
            int cy = y * canvas.height / MINIMAP_H;
            int canvasIdx = 3 * (cy * canvas.width + cx);
            int cacheIdx = 3 * (y * MINIMAP_W + x);
            minimapCache[cacheIdx] = canvas.pixels[canvasIdx];
            minimapCache[cacheIdx + 1] = canvas.pixels[canvasIdx + 1];
            minimapCache[cacheIdx + 2] = canvas.pixels[canvasIdx + 2];
        }
    }
    minimapCacheValid = true;
}

static void composeCanvasTopFrame(CanvasState &canvas, bool connected, bool updateAvailable, Color currentColor, int brushSize, int brushShape)
{
    drawTopChrome(connected, updateAvailable);

    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 288, 44, "CH", 73, 82, 92);

    char channel[16];
    strncpy(channel, canvas.channel[0] ? canvas.channel : "main", sizeof(channel) - 1);
    channel[sizeof(channel) - 1] = '\0';
    for (char *p = channel; *p; p++)
        if (*p >= 'a' && *p <= 'z')
            *p = *p - 'a' + 'A';
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 288, 76, channel, 32, 36, 42);

    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 288, 100, "BRUSH", 73, 82, 92);
    char brush[16];
    snprintf(brush, sizeof(brush), "%d %s", brushSize, brushShape == 1 ? "SQ" : brushShape == 2 ? "SOFT" : "CIR");
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 288, 112, brush, 32, 36, 42);

    fillTopRect(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 12, 42, 260, 150, 255, 255, 255);
    strokeTopRect(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 12, 42, 260, 150, 169, 180, 190);

    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 288, 138, "COLOR", 73, 82, 92);
    fillTopRect(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 288, 152, 42, 42, currentColor.r, currentColor.g, currentColor.b);
    strokeTopRect(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 288, 152, 42, 42, 32, 36, 42);

    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 288, 206, "L CH", 73, 82, 92);
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 332, 206, "R HELP", 73, 82, 92);

    if (!canvas.pixels || canvas.width <= 0 || canvas.height <= 0)
    {
        drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 92, 110, "CONNECTING", 80, 88, 96);
        topFrameValid = true;
        return;
    }

    const int mapX = 14;
    const int mapY = 44;
    const int mapW = MINIMAP_W;
    const int mapH = MINIMAP_H;
    for (int y = 0; y < mapH; y++)
    {
        for (int x = 0; x < mapW; x++)
        {
            int idx = 3 * (y * MINIMAP_W + x);
            setTopPixel(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, mapX + x, mapY + y,
                        minimapCache[idx], minimapCache[idx + 1], minimapCache[idx + 2]);
        }
    }
    strokeTopRect(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, mapX, mapY, mapW, mapH, 216, 224, 232);

    int viewX = mapX + std::max(0, canvas.offsetX) * mapW / canvas.width;
    int viewY = mapY + std::max(0, canvas.offsetY) * mapH / canvas.height;
    int viewW = std::max(3, 320 * mapW / canvas.width);
    int viewH = std::max(3, 240 * mapH / canvas.height);
    if (viewX + viewW > mapX + mapW) viewW = mapX + mapW - viewX;
    if (viewY + viewH > mapY + mapH) viewH = mapY + mapH - viewY;
    strokeTopRect(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, viewX, viewY, viewW, viewH, 214, 40, 40);
    strokeTopRect(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, viewX + 1, viewY + 1, std::max(1, viewW - 2), std::max(1, viewH - 2), 255, 255, 255);
    topFrameValid = true;
}

static void composeChannelTopFrame(CanvasState &canvas, bool connected, bool updateAvailable,
                                   char channels[][25], int channelCount, int selectedChannel)
{
    drawTopChrome(connected, updateAvailable);
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 24, 46, "CHANNELS", 32, 36, 42);
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 250, 46, "A SWITCH", 73, 82, 92);
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 250, 60, "B CLOSE", 73, 82, 92);
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 250, 74, "UP/DOWN", 73, 82, 92);

    int rows = std::min(channelCount, 8);
    for (int i = 0; i < rows; i++)
    {
        int y = 76 + i * 18;
        bool selected = i == selectedChannel;
        bool current = strcmp(canvas.channel, channels[i]) == 0;
        if (selected)
        {
            fillTopRect(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 24, y - 5, 190, 17, 24, 33, 38);
        }
        drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 34, y, current ? ">" : " ", selected ? 245 : 73, selected ? 248 : 82, selected ? 250 : 92);
        drawUpperText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 52, y, channels[i], selected ? 245 : 32, selected ? 248 : 36, selected ? 250 : 42);
    }

    if (rows == 0)
        drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 52, 92, "NO CHANNELS", 214, 40, 40);

    topFrameValid = true;
}

static void composeControlsTopFrame(CanvasState &canvas, bool connected, bool updateAvailable)
{
    drawTopChrome(connected, updateAvailable);
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 24, 46, "CONTROLS", 32, 36, 42);
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 24, 74, "TOUCH DRAW", 32, 36, 42);
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 24, 92, "LEFT TOUCH PAN", 32, 36, 42);
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 24, 110, "L CHANNELS", 32, 36, 42);
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 24, 128, "B COLOR PICKER", 32, 36, 42);
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 24, 146, "UP TOUCH SAMPLE", 32, 36, 42);
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 24, 164, "X HEX COLOR", 32, 36, 42);
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 24, 182, "START REFRESH", 32, 36, 42);
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 24, 200, "Y UPDATE CHECK", 32, 36, 42);
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 260, 200, "R CLOSE", 73, 82, 92);

    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 260, 74, "CHANNEL", 73, 82, 92);
    drawUpperText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 260, 88, canvas.channel[0] ? canvas.channel : "main", 32, 36, 42);

    topFrameValid = true;
}

static void presentTopFrameToFramebuffer(u8 *fb, u16 fbWidth, u16 fbHeight)
{
    if (!fb || !topFrameValid)
        return;

    const int screenWidth = fbHeight;
    const int screenHeight = fbWidth;
    const int width = std::min(TOP_SCREEN_W, screenWidth);
    const int height = std::min(TOP_SCREEN_H, screenHeight);

    for (int y = 0; y < height; y++)
    {
        for (int x = 0; x < width; x++)
        {
            int srcIdx = 3 * (y * TOP_SCREEN_W + x);
            int fbX = screenHeight - 1 - y;
            int fbY = x;
            int dstIdx = 3 * (fbY * fbWidth + fbX);
            fb[dstIdx] = topFrame[srcIdx + 2];
            fb[dstIdx + 1] = topFrame[srcIdx + 1];
            fb[dstIdx + 2] = topFrame[srcIdx];
        }
    }
}

void Renderer::renderTop(CanvasState &canvas, bool connected, bool updateAvailable, Color currentColor,
                         int brushSize, int brushShape, TopScreenMode mode,
                         char channels[][25], int channelCount, int selectedChannel)
{
    minimapFrameCounter++;
    if (mode == TOP_MODE_CANVAS && (!minimapCacheValid || minimapFrameCounter >= 15))
    {
        updateMinimapCache(canvas);
        minimapFrameCounter = 0;
    }

    if (mode == TOP_MODE_CHANNELS)
        composeChannelTopFrame(canvas, connected, updateAvailable, channels, channelCount, selectedChannel);
    else if (mode == TOP_MODE_CONTROLS)
        composeControlsTopFrame(canvas, connected, updateAvailable);
    else
        composeCanvasTopFrame(canvas, connected, updateAvailable, currentColor, brushSize, brushShape);
}

void Renderer::presentTopFrame()
{
    u16 leftWidth, leftHeight;
    u16 rightWidth, rightHeight;
    u8 *left = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, &leftWidth, &leftHeight);
    u8 *right = gfxGetFramebuffer(GFX_TOP, GFX_RIGHT, &rightWidth, &rightHeight);

    presentTopFrameToFramebuffer(left, leftWidth, leftHeight);
    presentTopFrameToFramebuffer(right, rightWidth, rightHeight);
}
