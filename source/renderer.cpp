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
            int canvasX = canvas.screenToCanvasX(y);
            int canvasY = canvas.screenToCanvasY(fbWidth - 1 - x);
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
        case '[': { u8 g[7] = {0x0E,0x08,0x08,0x08,0x08,0x08,0x0E}; memcpy(glyph,g,7); break; }
        case ']': { u8 g[7] = {0x0E,0x02,0x02,0x02,0x02,0x02,0x0E}; memcpy(glyph,g,7); break; }
        case '_': { u8 g[7] = {0x00,0x00,0x00,0x00,0x00,0x00,0x1F}; memcpy(glyph,g,7); break; }
        case '!': { u8 g[7] = {0x04,0x04,0x04,0x04,0x04,0x00,0x04}; memcpy(glyph,g,7); break; }
        case '?': { u8 g[7] = {0x0E,0x11,0x01,0x02,0x04,0x00,0x04}; memcpy(glyph,g,7); break; }
        case ',': { u8 g[7] = {0x00,0x00,0x00,0x00,0x00,0x04,0x08}; memcpy(glyph,g,7); break; }
        case '\'': { u8 g[7] = {0x04,0x04,0x08,0x00,0x00,0x00,0x00}; memcpy(glyph,g,7); break; }
        case 'a': { u8 g[7] = {0x00,0x00,0x0E,0x01,0x0F,0x11,0x0F}; memcpy(glyph,g,7); break; }
        case 'b': { u8 g[7] = {0x10,0x10,0x16,0x19,0x11,0x11,0x1E}; memcpy(glyph,g,7); break; }
        case 'c': { u8 g[7] = {0x00,0x00,0x0E,0x10,0x10,0x11,0x0E}; memcpy(glyph,g,7); break; }
        case 'd': { u8 g[7] = {0x01,0x01,0x0D,0x13,0x11,0x11,0x0F}; memcpy(glyph,g,7); break; }
        case 'e': { u8 g[7] = {0x00,0x00,0x0E,0x11,0x1F,0x10,0x0E}; memcpy(glyph,g,7); break; }
        case 'f': { u8 g[7] = {0x06,0x08,0x08,0x1C,0x08,0x08,0x08}; memcpy(glyph,g,7); break; }
        case 'g': { u8 g[7] = {0x00,0x00,0x0F,0x11,0x0F,0x01,0x0E}; memcpy(glyph,g,7); break; }
        case 'h': { u8 g[7] = {0x10,0x10,0x16,0x19,0x11,0x11,0x11}; memcpy(glyph,g,7); break; }
        case 'i': { u8 g[7] = {0x04,0x00,0x0C,0x04,0x04,0x04,0x0E}; memcpy(glyph,g,7); break; }
        case 'j': { u8 g[7] = {0x02,0x00,0x06,0x02,0x02,0x12,0x0C}; memcpy(glyph,g,7); break; }
        case 'k': { u8 g[7] = {0x10,0x10,0x12,0x14,0x18,0x14,0x12}; memcpy(glyph,g,7); break; }
        case 'l': { u8 g[7] = {0x0C,0x04,0x04,0x04,0x04,0x04,0x0E}; memcpy(glyph,g,7); break; }
        case 'm': { u8 g[7] = {0x00,0x00,0x1A,0x15,0x15,0x15,0x15}; memcpy(glyph,g,7); break; }
        case 'n': { u8 g[7] = {0x00,0x00,0x16,0x19,0x11,0x11,0x11}; memcpy(glyph,g,7); break; }
        case 'o': { u8 g[7] = {0x00,0x00,0x0E,0x11,0x11,0x11,0x0E}; memcpy(glyph,g,7); break; }
        case 'p': { u8 g[7] = {0x00,0x00,0x1E,0x11,0x1E,0x10,0x10}; memcpy(glyph,g,7); break; }
        case 'q': { u8 g[7] = {0x00,0x00,0x0D,0x13,0x0F,0x01,0x01}; memcpy(glyph,g,7); break; }
        case 'r': { u8 g[7] = {0x00,0x00,0x16,0x19,0x10,0x10,0x10}; memcpy(glyph,g,7); break; }
        case 's': { u8 g[7] = {0x00,0x00,0x0F,0x10,0x0E,0x01,0x1E}; memcpy(glyph,g,7); break; }
        case 't': { u8 g[7] = {0x08,0x08,0x1C,0x08,0x08,0x09,0x06}; memcpy(glyph,g,7); break; }
        case 'u': { u8 g[7] = {0x00,0x00,0x11,0x11,0x11,0x13,0x0D}; memcpy(glyph,g,7); break; }
        case 'v': { u8 g[7] = {0x00,0x00,0x11,0x11,0x11,0x0A,0x04}; memcpy(glyph,g,7); break; }
        case 'w': { u8 g[7] = {0x00,0x00,0x11,0x15,0x15,0x15,0x0A}; memcpy(glyph,g,7); break; }
        case 'x': { u8 g[7] = {0x00,0x00,0x11,0x0A,0x04,0x0A,0x11}; memcpy(glyph,g,7); break; }
        case 'y': { u8 g[7] = {0x00,0x00,0x11,0x11,0x0F,0x01,0x0E}; memcpy(glyph,g,7); break; }
        case 'z': { u8 g[7] = {0x00,0x00,0x1F,0x02,0x04,0x08,0x1F}; memcpy(glyph,g,7); break; }
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

static int drawWrappedText(u8 *target, int width, int height, int x, int y, int maxChars, int maxLines, const char *text, u8 r, u8 g, u8 b)
{
    int line = 0;
    int lastSpaceCol = -1;
    const char *start = text ? text : "";
    char buffer[80];
    while (*start && line < maxLines)
    {
        lastSpaceCol = -1;
        int len = 0;
        const char *ptr = start;
        while (*ptr && len < maxChars)
        {
            buffer[len] = *ptr;
            if (*ptr == ' ')
                lastSpaceCol = len;
            len++;
            ptr++;
        }
        if (*ptr && lastSpaceCol > 8)
            len = lastSpaceCol;
        buffer[len] = '\0';
        drawText(target, width, height, x, y + line * 10, buffer, r, g, b);
        start += len;
        while (*start == ' ')
            start++;
        line++;
    }
    return line;
}

static int wrappedLineCount(int maxChars, int maxLines, const char *text)
{
    int line = 0;
    const char *start = text ? text : "";
    while (*start && line < maxLines)
    {
        int lastSpaceCol = -1;
        int len = 0;
        const char *ptr = start;
        while (*ptr && len < maxChars)
        {
            if (*ptr == ' ')
                lastSpaceCol = len;
            len++;
            ptr++;
        }
        if (*ptr && lastSpaceCol > 8)
            len = lastSpaceCol;
        if (len <= 0)
            len = 1;
        start += len;
        while (*start == ' ')
            start++;
        line++;
    }
    return std::max(1, line);
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

static void drawFooterHint(const char *left, const char *right)
{
    fillTopRect(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 0, 212, TOP_SCREEN_W, 1, 206, 214, 220);
    if (left && left[0])
        drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 18, 224, left, 73, 82, 92);
    if (right && right[0])
        drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 252, 224, right, 73, 82, 92);
}

static void drawMenuRow(int y, const char *label, bool selected, bool current = false)
{
    if (selected)
        fillTopRect(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 20, y - 5, 220, 17, 24, 33, 38);
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 30, y, current ? "*" : selected ? ">" : " ", selected ? 245 : 73, selected ? 248 : 82, selected ? 250 : 92);
    drawUpperText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 48, y, label, selected ? 245 : 32, selected ? 248 : 36, selected ? 250 : 42);
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
    snprintf(version, sizeof(version), "V %s", APP_BUILD_LABEL);
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

static void composeCanvasTopFrame(CanvasState &canvas, bool connected, bool updateAvailable, Color currentColor,
                                  int brushSize, int brushShape, int chatUnread)
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
    snprintf(brush, sizeof(brush), "%d %s", brushSize,
             brushShape == 1 ? "SQ" : brushShape == 2 ? "DIT" : brushShape == 3 ? "ERS" : "CIR");
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 288, 112, brush, 32, 36, 42);

    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 288, 124, "ZOOM", 73, 82, 92);
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 324, 124, canvas.zoomLabel(), 32, 36, 42);

    fillTopRect(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 12, 42, 260, 150, 255, 255, 255);
    strokeTopRect(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 12, 42, 260, 150, 169, 180, 190);

    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 288, 138, "COLOR", 73, 82, 92);
    fillTopRect(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 288, 152, 42, 42, currentColor.r, currentColor.g, currentColor.b);
    strokeTopRect(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 288, 152, 42, 42, 32, 36, 42);

    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 288, 206, "SELECT MENU", 73, 82, 92);
    if (chatUnread > 0)
    {
        fillTopRect(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 354, 38, 34, 20, 255, 255, 255);
        strokeTopRect(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 354, 38, 34, 20, 196, 92, 40);
        drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 360, 45, "MSG", 196, 92, 40);
    }

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
    int viewW = std::max(3, canvas.viewWidth(320) * mapW / canvas.width);
    int viewH = std::max(3, canvas.viewHeight(240) * mapH / canvas.height);
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
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 250, 46, "CURRENT", 73, 82, 92);
    drawUpperText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 250, 60, canvas.channel[0] ? canvas.channel : "main", 32, 36, 42);

    int rows = std::min(channelCount, 8);
    for (int i = 0; i < rows; i++)
    {
        int y = 76 + i * 18;
        bool selected = i == selectedChannel;
        bool current = strcmp(canvas.channel, channels[i]) == 0;
        drawMenuRow(y, channels[i], selected, current);
    }

    if (rows == 0)
        drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 52, 92, "NO CHANNELS", 214, 40, 40);

    drawFooterHint("A SWITCH", "B BACK");
    topFrameValid = true;
}

static void composeControlsTopFrame(CanvasState &canvas, bool connected, bool updateAvailable)
{
    drawTopChrome(connected, updateAvailable);
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 24, 46, "CONTROLS", 32, 36, 42);
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 24, 74, "TOUCH DRAW", 32, 36, 42);
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 24, 92, "C-PAD OR LEFT/A + DRAG PANS", 32, 36, 42);
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 24, 110, "SELECT MENU", 32, 36, 42);
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 24, 128, "B COLOR PICKER", 32, 36, 42);
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 24, 146, "UP/X + TOUCH SAMPLE", 32, 36, 42);
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 24, 164, "L/R HOLD ERASER", 32, 36, 42);
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 24, 182, "START REFRESH", 32, 36, 42);
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 24, 200, "RIGHT/Y TOUCH ZOOM", 32, 36, 42);
    drawFooterHint("SELECT MENU", "B BACK");

    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 260, 74, "CHANNEL", 73, 82, 92);
    drawUpperText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 260, 88, canvas.channel[0] ? canvas.channel : "main", 32, 36, 42);

    topFrameValid = true;
}

static void composeMenuTopFrame(CanvasState &canvas, bool connected, bool updateAvailable, int selectedMenuItem,
                                int chatUnread, bool showAdminTools)
{
    (void)chatUnread;
    drawTopChrome(connected, updateAvailable);
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 24, 46, "MAIN MENU", 32, 36, 42);

    const char *regularItems[] = {
        "CHANNELS",
        "CONNECTED USERS",
        "CONTROLS",
        "RULES / HELP",
        "STATUS",
        "IDENTITY",
        "EXIT APP",
    };
    const char *staffItems[] = {
        "CHANNELS",
        "CONNECTED USERS",
        "CONTROLS",
        "RULES / HELP",
        "STATUS",
        "IDENTITY",
        "ADMIN TOOLS",
        "EXIT APP",
    };
    const char **items = showAdminTools ? staffItems : regularItems;
    const int itemCount = showAdminTools ? (int)(sizeof(staffItems) / sizeof(staffItems[0]))
                                         : (int)(sizeof(regularItems) / sizeof(regularItems[0]));
    for (int i = 0; i < itemCount; i++)
    {
        int y = 78 + i * 20;
        drawMenuRow(y, items[i], i == selectedMenuItem);
    }

    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 252, 104, "CHANNEL", 73, 82, 92);
    drawUpperText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 252, 118, canvas.channel[0] ? canvas.channel : "main", 32, 36, 42);
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 252, 146, connected ? "ONLINE" : "OFFLINE", connected ? 13 : 196, connected ? 122 : 61, connected ? 117 : 61);
    drawFooterHint("A OPEN", "B CLOSE");
    topFrameValid = true;
}

static void composeRulesTopFrame(bool connected, bool updateAvailable, const char *requiredVersion, bool needsAgreement)
{
    drawTopChrome(connected, updateAvailable);
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 24, 38, "RULES + QUICK START", 32, 36, 42);
    if (requiredVersion && requiredVersion[0])
    {
        char versionText[32];
        snprintf(versionText, sizeof(versionText), "V %s", requiredVersion);
        drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 344, 38, versionText, 73, 82, 92);
    }

    const char *rules[] = {
        "NO SEXUAL CONTENT.",
        "NO HEAVY PROFANITY OR SLURS.",
        "NO HARASSMENT, THREATS, HATE, OR PERSONAL INFO.",
        "NO INTENTIONAL GRIEFING, SPAM, OR VANDALISM.",
        "MODS MAY CLEAR, KICK, MUTE, BAN, OR SAVE EVIDENCE.",
    };
    int y = 64;
    for (int i = 0; i < 5; i++)
    {
        drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 26, y, "-", 13, 122, 117);
        drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 42, y, rules[i], 32, 36, 42);
        y += 18;
    }

    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 24, 166, "DRAW ON THE BOTTOM SCREEN.", 73, 82, 92);
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 24, 184, "C-PAD OR LEFT/A PANS. RIGHT/Y SHOWS ZOOM.", 73, 82, 92);
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 24, 202, "HOLD L/R FOR QUICK ERASER. UP/X SAMPLES COLOR.", 73, 82, 92);
    if (needsAgreement)
        drawUpperText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 24, 220, "PRESS A TO AGREE AND CONTINUE", 196, 92, 40);
    drawFooterHint("", needsAgreement ? "B EXIT" : "B BACK");
    topFrameValid = true;
}

static void composeUsersTopFrame(CanvasState &canvas, bool connected, bool updateAvailable, PresenceUser *users, int userCount)
{
    drawTopChrome(connected, updateAvailable);
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 24, 46, "CONNECTED USERS", 32, 36, 42);
    char countText[20];
    snprintf(countText, sizeof(countText), "%d ONLINE", userCount);
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 276, 46, countText, 73, 82, 92);

    int rows = std::min(userCount, 8);
    for (int i = 0; i < rows; i++)
    {
        int y = 78 + i * 18;
        drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 34, y, "-", 73, 82, 92);
        drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 52, y, users[i].displayName, 32, 36, 42);
        if (strcmp(users[i].role, "admin") == 0 || strcmp(users[i].role, "mod") == 0)
            drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 206, y, users[i].role, 13, 122, 117);
    }
    if (rows == 0)
        drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 52, 92, "NO USERS YET", 104, 114, 124);

    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 252, 104, "CHANNEL", 73, 82, 92);
    drawUpperText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 252, 118, canvas.channel[0] ? canvas.channel : "main", 32, 36, 42);
    drawFooterHint("", "B BACK");
    topFrameValid = true;
}

static void composeChatTopFrame(CanvasState &canvas, bool connected, bool updateAvailable,
                                ChatLine *chatLines, int chatCount,
                                int chatScroll, int chatSelected, int chatUnread, const char *chatNotice)
{
    drawTopChrome(connected, updateAvailable);
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 10, 34, "Public chat", 32, 36, 42);
    if (chatUnread > 0)
    {
        char unreadText[24];
        snprintf(unreadText, sizeof(unreadText), "%d NEW", std::min(chatUnread, 99));
        drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 88, 34, unreadText, 196, 92, 40);
    }
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 346, 34, "global", 73, 82, 92);

    int maxStart = chatCount > 0 ? chatCount - 1 : 0;
    int start = std::max(0, std::min(chatScroll, maxStart));
    fillTopRect(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 8, 48, 384, 158, 248, 250, 251);
    strokeTopRect(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 8, 48, 384, 158, 196, 204, 212);
    int y = 54;
    int lastRendered = start - 1;
    for (int i = start; chatLines && i < chatCount && y < 204; i++)
    {
        ChatLine &line = chatLines[i];
        char timeText[6] = "--:--";
        if (line.timestamp[11] && line.timestamp[12] && line.timestamp[14] && line.timestamp[15])
            snprintf(timeText, sizeof(timeText), "%.2s:%.2s", line.timestamp + 11, line.timestamp + 14);
        char prefix[72];
        const char *name = line.displayName[0] ? line.displayName : line.username[0] ? line.username : "user";
        if (strcmp(line.role, "admin") == 0 || strcmp(line.role, "mod") == 0)
            snprintf(prefix, sizeof(prefix), "<%s>[%s]%s:", timeText, line.role, name);
        else
            snprintf(prefix, sizeof(prefix), "<%s>%s:", timeText, name);
        int used = (int)strlen(prefix) * 6 + 6;
        int messageX = std::min(214, 18 + used);
        int maxChars = std::max(8, (388 - messageX) / 6);
        int lineCount = wrappedLineCount(maxChars, 3, line.message);
        int rowHeight = std::max(15, lineCount * 10 + 5);
        if (y + rowHeight > 206)
            break;

        if (i == chatSelected)
        {
            fillTopRect(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 12, y - 3, 376, rowHeight, 224, 242, 238);
            strokeTopRect(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 12, y - 3, 376, rowHeight, 13, 122, 117);
        }
        drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 14, y, prefix,
                 line.deleted ? 104 : 13, line.deleted ? 114 : 122, line.deleted ? 124 : 117);
        drawWrappedText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, messageX, y, maxChars, 3,
                        line.message, line.deleted ? 104 : 32, line.deleted ? 114 : 36, line.deleted ? 124 : 42);
        y += rowHeight + 1;
        lastRendered = i;
    }
    if (chatCount == 0)
        drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 40, 96, "No public chat messages yet", 104, 114, 124);
    else if (lastRendered >= start)
    {
        char pageText[24];
        int total = std::max(0, std::min(chatCount, 99));
        int first = std::max(1, std::min(start + 1, 99));
        int last = std::max(first, std::min(lastRendered + 1, 99));
        snprintf(pageText, sizeof(pageText), "%02d-%02d/%02d", first, last, total);
        drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 316, 203, pageText, 104, 114, 124);
    }

    if (chatNotice && chatNotice[0])
        drawUpperText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 12, 203, chatNotice, 196, 92, 40);
    drawFooterHint("A SEND  UP/DOWN SELECT", "B MENU");
    topFrameValid = true;
}

static void composeAdminTopFrame(CanvasState &canvas, bool connected, bool updateAvailable,
                                 const char *role, int selectedAdminItem, const char *adminNotice)
{
    drawTopChrome(connected, updateAvailable);
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 24, 46, "ADMIN TOOLS", 32, 36, 42);
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 264, 46, "ROLE", 73, 82, 92);
    drawUpperText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 264, 60, role, 32, 36, 42);

    bool allowed = role && (strcmp(role, "mod") == 0 || strcmp(role, "admin") == 0);
    if (!allowed)
    {
        drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 24, 84, "MOD OR ADMIN REQUIRED", 196, 92, 40);
        drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 24, 108, "SET YOUR ROLE IN WEB ADMIN", 104, 114, 124);
        drawFooterHint("", "B BACK");
        topFrameValid = true;
        return;
    }

    const char *items[] = {
        "SAVE SNAPSHOT",
        "CLEAR CANVAS",
        "FILL RECT",
    };
    for (int i = 0; i < 3; i++)
        drawMenuRow(78 + i * 22, items[i], i == selectedAdminItem);

    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 252, 132, "CHANNEL", 73, 82, 92);
    drawUpperText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 252, 146, canvas.channel[0] ? canvas.channel : "main", 32, 36, 42);
    if (adminNotice && adminNotice[0])
        drawUpperText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 24, 184, adminNotice, 196, 92, 40);
    else
        drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 24, 184, "FILL RECT: DRAG ON BOTTOM", 104, 114, 124);
    drawFooterHint("A USE", "B BACK");
    topFrameValid = true;
}

static void composeStatusTopFrame(CanvasState &canvas, bool connected, bool updateAvailable)
{
    drawTopChrome(connected, updateAvailable);
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 24, 46, "STATUS", 32, 36, 42);
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 24, 78, "CONNECTION", 73, 82, 92);
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 128, 78, connected ? "ONLINE" : "OFFLINE", connected ? 13 : 196, connected ? 122 : 61, connected ? 117 : 61);
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 24, 100, "UPDATE", 73, 82, 92);
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 128, 100, updateAvailable ? "AVAILABLE" : "CURRENT", updateAvailable ? 196 : 32, updateAvailable ? 92 : 36, updateAvailable ? 40 : 42);
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 24, 122, "VERSION", 73, 82, 92);
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 128, 122, APP_BUILD_LABEL, 32, 36, 42);
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 24, 144, "CHANNEL", 73, 82, 92);
    drawUpperText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 128, 144, canvas.channel[0] ? canvas.channel : "main", 32, 36, 42);
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 24, 166, "ZOOM", 73, 82, 92);
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 128, 166, canvas.zoomLabel(), 32, 36, 42);
    drawFooterHint("START REFRESH", "B BACK");
    topFrameValid = true;
}

static void composeIdentityTopFrame(bool connected, bool updateAvailable,
                                    const char *displayName, const char *username,
                                    const char *role, const char *status,
                                    const char *backupCode,
                                    const char *identityNotice,
                                    const char *identityStorage)
{
    drawTopChrome(connected, updateAvailable);
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 24, 46, "IDENTITY", 32, 36, 42);
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 24, 78, "NAME", 73, 82, 92);
    drawUpperText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 128, 78, displayName, 32, 36, 42);
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 24, 100, "ACCOUNT", 73, 82, 92);
    drawUpperText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 128, 100, username, 32, 36, 42);
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 24, 122, "ROLE", 73, 82, 92);
    drawUpperText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 128, 122, role, 32, 36, 42);
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 24, 144, "STATE", 73, 82, 92);
    drawUpperText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 128, 144, status, 32, 36, 42);
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 24, 166, "BACKUP", 73, 82, 92);
    if (backupCode && backupCode[0])
        drawUpperText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 128, 166, backupCode, 32, 36, 42);
    else
        drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 128, 166, "PRESS Y", 196, 92, 40);
    if (identityStorage && identityStorage[0])
        drawUpperText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 256, 166, identityStorage, 104, 114, 124);
    if (identityNotice && identityNotice[0])
        drawUpperText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 24, 190, identityNotice, 196, 92, 40);
    else
        drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 24, 190, "X RECOVER  Y GET CODE", 104, 114, 124);
    drawFooterHint("A EDIT NAME", "B BACK");
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
                         char channels[][25], int channelCount, int selectedChannel,
                         int selectedMenuItem, PresenceUser *users, int userCount,
                         const char *displayName, const char *username,
                         const char *role, const char *status,
                         const char *backupCode, const char *identityNotice,
                         const char *identityStorage, int selectedAdminItem,
                         const char *adminNotice,
                         const char *rulesVersion, bool needsRulesAgreement,
                         ChatLine *chatLines, int chatCount, int chatScroll, int chatSelected, int chatUnread,
                         const char *chatNotice)
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
    else if (mode == TOP_MODE_MENU)
        composeMenuTopFrame(canvas, connected, updateAvailable, selectedMenuItem, chatUnread,
                            role && (strcmp(role, "mod") == 0 || strcmp(role, "admin") == 0));
    else if (mode == TOP_MODE_USERS)
        composeUsersTopFrame(canvas, connected, updateAvailable, users, userCount);
    else if (mode == TOP_MODE_RULES)
        composeRulesTopFrame(connected, updateAvailable, rulesVersion, needsRulesAgreement);
    else if (mode == TOP_MODE_CHAT)
        composeChatTopFrame(canvas, connected, updateAvailable, chatLines, chatCount, chatScroll, chatSelected, chatUnread, chatNotice);
    else if (mode == TOP_MODE_ADMIN)
        composeAdminTopFrame(canvas, connected, updateAvailable, role, selectedAdminItem, adminNotice);
    else if (mode == TOP_MODE_STATUS)
        composeStatusTopFrame(canvas, connected, updateAvailable);
    else if (mode == TOP_MODE_IDENTITY)
        composeIdentityTopFrame(connected, updateAvailable, displayName, username, role, status, backupCode,
                                identityNotice, identityStorage);
    else
        composeCanvasTopFrame(canvas, connected, updateAvailable, currentColor, brushSize, brushShape, chatUnread);
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
