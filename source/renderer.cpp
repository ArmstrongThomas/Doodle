#include "renderer.h"
#include <algorithm>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static const int MINIMAP_W = 256;
static const int MINIMAP_H = 144;
static const int TOP_SCREEN_W = 400;
static const int TOP_SCREEN_H = 240;
static u8 minimapCache[MINIMAP_W * MINIMAP_H * 3];
static u8 topFrame[TOP_SCREEN_W * TOP_SCREEN_H * 3];
static bool minimapCacheValid = false;
static bool topFrameValid = false;
static int minimapFrameCounter = 0;
static const int BOTTOM_SCREEN_W = 320;
static const int BOTTOM_SCREEN_H = 240;
static int topBatteryPercent = -1;
static bool topBatteryCharging = false;
static u64 topBatteryReadAt = 0;

void Renderer::renderViewport(CanvasState &canvas, u8 *buffer, int fbWidth, int fbHeight, bool forceFull)
{
    if (!canvas.pixels)
        return;

    const bool rotatedFramebuffer = fbWidth < fbHeight;
    for (int screenY = 0; screenY < BOTTOM_SCREEN_H; screenY++)
    {
        for (int screenX = 0; screenX < BOTTOM_SCREEN_W; screenX++)
        {
            int fbX = rotatedFramebuffer ? fbWidth - 1 - screenY : screenX;
            int fbY = rotatedFramebuffer ? screenX : screenY;
            if (fbX < 0 || fbX >= fbWidth || fbY < 0 || fbY >= fbHeight)
                continue;

            int canvasX = canvas.screenToCanvasX(screenX);
            int canvasY = canvas.screenToCanvasY(screenY);
            int bufferIdx = 3 * (fbY * fbWidth + fbX);
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
        case '"': { u8 g[7] = {0x0A,0x0A,0x14,0x00,0x00,0x00,0x00}; memcpy(glyph,g,7); break; }
        case '#': { u8 g[7] = {0x0A,0x1F,0x0A,0x0A,0x1F,0x0A,0x00}; memcpy(glyph,g,7); break; }
        case '$': { u8 g[7] = {0x04,0x0F,0x14,0x0E,0x05,0x1E,0x04}; memcpy(glyph,g,7); break; }
        case '%': { u8 g[7] = {0x19,0x19,0x02,0x04,0x08,0x13,0x13}; memcpy(glyph,g,7); break; }
        case '&': { u8 g[7] = {0x0C,0x12,0x14,0x08,0x15,0x12,0x0D}; memcpy(glyph,g,7); break; }
        case '(': { u8 g[7] = {0x02,0x04,0x08,0x08,0x08,0x04,0x02}; memcpy(glyph,g,7); break; }
        case ')': { u8 g[7] = {0x08,0x04,0x02,0x02,0x02,0x04,0x08}; memcpy(glyph,g,7); break; }
        case '*': { u8 g[7] = {0x00,0x15,0x0E,0x1F,0x0E,0x15,0x00}; memcpy(glyph,g,7); break; }
        case '+': { u8 g[7] = {0x00,0x04,0x04,0x1F,0x04,0x04,0x00}; memcpy(glyph,g,7); break; }
        case ';': { u8 g[7] = {0x00,0x04,0x00,0x00,0x04,0x04,0x08}; memcpy(glyph,g,7); break; }
        case '=': { u8 g[7] = {0x00,0x1F,0x00,0x1F,0x00,0x00,0x00}; memcpy(glyph,g,7); break; }
        case '@': { u8 g[7] = {0x0E,0x11,0x17,0x15,0x17,0x10,0x0E}; memcpy(glyph,g,7); break; }
        case '\\': { u8 g[7] = {0x10,0x10,0x08,0x04,0x02,0x01,0x01}; memcpy(glyph,g,7); break; }
        case '^': { u8 g[7] = {0x04,0x0A,0x11,0x00,0x00,0x00,0x00}; memcpy(glyph,g,7); break; }
        case '`': { u8 g[7] = {0x08,0x04,0x02,0x00,0x00,0x00,0x00}; memcpy(glyph,g,7); break; }
        case '{': { u8 g[7] = {0x02,0x04,0x04,0x08,0x04,0x04,0x02}; memcpy(glyph,g,7); break; }
        case '|': { u8 g[7] = {0x04,0x04,0x04,0x04,0x04,0x04,0x04}; memcpy(glyph,g,7); break; }
        case '}': { u8 g[7] = {0x08,0x04,0x04,0x02,0x04,0x04,0x08}; memcpy(glyph,g,7); break; }
        case '~': { u8 g[7] = {0x00,0x00,0x09,0x16,0x00,0x00,0x00}; memcpy(glyph,g,7); break; }
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
        default: { u8 g[7] = {0x1F,0x11,0x15,0x15,0x11,0x11,0x1F}; memcpy(glyph,g,7); break; }
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
    drawText(target, width, height, x, y, text ? text : "", r, g, b);
}

static void formatTitleLabel(const char *source, char *target, size_t targetSize)
{
    if (!target || targetSize == 0) return;
    size_t out = 0;
    bool capitalize = true;
    for (const char *p = source ? source : ""; *p && out + 1 < targetSize; p++)
    {
        char c = *p == '_' ? ' ' : *p;
        if (capitalize && c >= 'a' && c <= 'z') c = c - 'a' + 'A';
        target[out++] = c;
        capitalize = c == ' ';
    }
    target[out] = '\0';
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
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 48, y, label, selected ? 245 : 32, selected ? 248 : 36, selected ? 250 : 42);
}

static void drawTopSystemStatus()
{
    time_t now = time(NULL);
    struct tm *local = localtime(&now);
    char clockText[6] = "--:--";
    if (local)
        snprintf(clockText, sizeof(clockText), "%02d:%02d", local->tm_hour, local->tm_min);
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 180, 10, clockText, 218, 226, 232);

    u8 wifi = osGetWifiStrength();
    if (wifi > 3) wifi = 3;
    for (int bar = 0; bar < 3; bar++)
    {
        int barHeight = 3 + bar * 3;
        bool active = wifi > bar;
        fillTopRect(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 220 + bar * 5, 20 - barHeight,
                    3, barHeight, active ? 94 : 73, active ? 234 : 82, active ? 212 : 92);
    }

    u64 readNow = osGetTime();
    if (topBatteryPercent < 0 || readNow - topBatteryReadAt >= 15000ULL)
    {
        u8 level = 0;
        if (R_SUCCEEDED(MCUHWC_GetBatteryLevel(&level)) && level <= 100)
            topBatteryPercent = level;
        else if (R_SUCCEEDED(PTMU_GetBatteryLevel(&level)))
        {
            static const int estimatedPercent[] = { 5, 20, 40, 60, 80, 100 };
            topBatteryPercent = estimatedPercent[std::min((int)level, 5)];
        }
        u8 charging = 0;
        bool adapterConnected = false;
        bool hasChargeState = R_SUCCEEDED(PTMU_GetBatteryChargeState(&charging));
        bool hasAdapterState = R_SUCCEEDED(PTMU_GetAdapterState(&adapterConnected));
        if (hasChargeState || hasAdapterState)
            topBatteryCharging = charging != 0 || adapterConnected;
        topBatteryReadAt = readNow;
    }

    u8 batteryR = topBatteryPercent >= 0 && topBatteryPercent <= 20 ? 255 : (topBatteryPercent <= 40 ? 255 : 94);
    u8 batteryG = topBatteryPercent >= 0 && topBatteryPercent <= 20 ? 115 : (topBatteryPercent <= 40 ? 214 : 234);
    u8 batteryB = topBatteryPercent >= 0 && topBatteryPercent <= 20 ? 115 : (topBatteryPercent <= 40 ? 102 : 212);
    strokeTopRect(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 242, 8, 17, 12, 218, 226, 232);
    fillTopRect(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 259, 11, 2, 6, 218, 226, 232);
    if (topBatteryPercent >= 0)
    {
        int fillWidth = std::max(1, std::min(13, topBatteryPercent * 13 / 100));
        fillTopRect(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 244, 10, fillWidth, 8, batteryR, batteryG, batteryB);
    }
    char batteryText[16];
    if (topBatteryPercent >= 0)
        snprintf(batteryText, sizeof(batteryText), "%d%%%s", topBatteryPercent, topBatteryCharging ? "+" : "");
    else
        snprintf(batteryText, sizeof(batteryText), "--%%");
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 266, 10, batteryText, batteryR, batteryG, batteryB);
}

static void drawTopChrome(bool connected, bool updateAvailable)
{
    fillTopRect(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 0, 0, TOP_SCREEN_W, TOP_SCREEN_H, 232, 236, 239);
    fillTopRect(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 0, 0, TOP_SCREEN_W, 30, 24, 33, 38);
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 12, 10, "Collab Doodle", 245, 248, 250);
    drawTopSystemStatus();
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 320, 10, connected ? "On" : "Off", connected ? 94 : 255, connected ? 234 : 115, connected ? 212 : 115);
    if (updateAvailable)
        drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 370, 10, "New", 255, 214, 102);

    char version[40];
    snprintf(version, sizeof(version), "v%s", APP_BUILD_LABEL);
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
                                  int brushSize, int brushShape, int ticketNeedsReply, int staffChatUnread,
                                  PresenceUser *users, int userCount)
{
    drawTopChrome(connected, updateAvailable);

    if (!canvas.pixels || canvas.width <= 0 || canvas.height <= 0)
    {
        drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 92, 110, "Connecting...", 80, 88, 96);
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

    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 282, 44, "Online", 73, 82, 92);
    char onlineCount[8];
    snprintf(onlineCount, sizeof(onlineCount), "%d", std::max(0, std::min(userCount, 99)));
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 370, 44, onlineCount, 73, 82, 92);

    int order[24];
    int ordered = 0;
    for (int pass = 0; pass < 2 && ordered < 24; pass++)
    {
        for (int i = 0; users && i < userCount && ordered < 24; i++)
        {
            bool staff = strcmp(users[i].role, "admin") == 0 || strcmp(users[i].role, "mod") == 0;
            if ((pass == 0 && staff) || (pass == 1 && !staff)) order[ordered++] = i;
        }
    }
    int visibleUsers = std::min(ordered, 8);
    for (int row = 0; row < visibleUsers; row++)
    {
        PresenceUser &user = users[order[row]];
        int y = 61 + row * 16;
        bool admin = strcmp(user.role, "admin") == 0;
        bool mod = strcmp(user.role, "mod") == 0;
        drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 282, y, admin ? "A" : mod ? "M" : "-",
                 admin || mod ? 13 : 104, admin || mod ? 122 : 114, admin || mod ? 117 : 124);
        char name[17];
        snprintf(name, sizeof(name), "%.16s", user.displayName[0] ? user.displayName : user.username);
        drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 294, y, name, 32, 36, 42);
    }
    if (visibleUsers == 0)
        drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 294, 76, "Nobody yet", 104, 114, 124);

    const char *brushName = brushShape == 1 ? "Square" : brushShape == 2 ? "Dither" : brushShape == 3 ? "Eraser" : "Circle";
    char drawingInfo[44];
    snprintf(drawingInfo, sizeof(drawingInfo), "%.10s | %s %d | %s",
             canvas.channel[0] ? canvas.channel : "main", brushName, brushSize, canvas.zoomLabel());
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 14, 198, drawingInfo, 32, 36, 42);
    fillTopRect(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 252, 196, 14, 12, currentColor.r, currentColor.g, currentColor.b);
    strokeTopRect(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 252, 196, 14, 12, 32, 36, 42);

    if (ticketNeedsReply > 0)
    {
        char text[18];
        snprintf(text, sizeof(text), "%d ticket%s", std::min(ticketNeedsReply, 99), ticketNeedsReply == 1 ? "" : "s");
        drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 282, 194, text, 196, 92, 40);
    }
    if (staffChatUnread > 0)
    {
        char text[18];
        snprintf(text, sizeof(text), "%d chat new", std::min(staffChatUnread, 99));
        drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 282, 204, text, 13, 122, 117);
    }
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 12, 224, "SELECT MENU", 73, 82, 92);
    topFrameValid = true;
}

static void composeChannelTopFrame(CanvasState &canvas, bool connected, bool updateAvailable,
                                   char channels[][25], int channelCount, int selectedChannel)
{
    drawTopChrome(connected, updateAvailable);
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 24, 46, "Channels", 32, 36, 42);
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 250, 46, "Current", 73, 82, 92);
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
        drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 52, 92, "No channels", 214, 40, 40);

    drawFooterHint("A SWITCH", "B BACK");
    topFrameValid = true;
}

static void composeControlsTopFrame(CanvasState &canvas, bool connected, bool updateAvailable)
{
    drawTopChrome(connected, updateAvailable);
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 24, 46, "Controls", 32, 36, 42);
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 24, 74, "Touch: Draw", 32, 36, 42);
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 24, 92, "C-PAD or LEFT/A + drag: Pan", 32, 36, 42);
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 24, 110, "SELECT: Menu", 32, 36, 42);
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 24, 128, "B: Color picker", 32, 36, 42);
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 24, 146, "UP/X + touch: Sample", 32, 36, 42);
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 24, 164, "Hold L/R: Eraser", 32, 36, 42);
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 24, 182, "START: Refresh", 32, 36, 42);
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 24, 200, "RIGHT/Y + touch: Zoom", 32, 36, 42);
    drawFooterHint("SELECT MENU", "B BACK");

    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 260, 74, "Channel", 73, 82, 92);
    drawUpperText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 260, 88, canvas.channel[0] ? canvas.channel : "main", 32, 36, 42);

    topFrameValid = true;
}

static void composeMenuTopFrame(CanvasState &canvas, bool connected, bool updateAvailable, int selectedMenuItem,
                                int ticketNeedsReplyCount, int staffChatUnreadCount, bool showAdminTools)
{
    (void)showAdminTools;
    drawTopChrome(connected, updateAvailable);
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 24, 46, "Main menu", 32, 36, 42);

    const char *regularItems[] = {
        "Channels",
        "Connected users",
        "Tickets",
        "Controls",
        "Rules / help",
        "Status",
        "Identity",
        "Exit app",
    };
    const char **items = regularItems;
    const int itemCount = (int)(sizeof(regularItems) / sizeof(regularItems[0]));
    for (int i = 0; i < itemCount; i++)
    {
        int y = 70 + i * 18;
        drawMenuRow(y, items[i], i == selectedMenuItem);
    }

    if (ticketNeedsReplyCount > 0)
    {
        char ticketText[24];
        snprintf(ticketText, sizeof(ticketText), "%d new ticket", std::min(ticketNeedsReplyCount, 99));
        drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 252, 82, ticketText, 196, 92, 40);
    }
    if (staffChatUnreadCount > 0)
    {
        char chatText[24];
        snprintf(chatText, sizeof(chatText), "%d new chat", std::min(staffChatUnreadCount, 99));
        drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 252, 96, chatText, 13, 122, 117);
    }

    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 252, 104, "Channel", 73, 82, 92);
    drawUpperText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 252, 118, canvas.channel[0] ? canvas.channel : "main", 32, 36, 42);
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 252, 146, connected ? "Online" : "Offline", connected ? 13 : 196, connected ? 122 : 61, connected ? 117 : 61);
    drawFooterHint("A OPEN", "B CLOSE");
    topFrameValid = true;
}

static void composeRulesTopFrame(bool connected, bool updateAvailable, const char *requiredVersion, bool needsAgreement)
{
    drawTopChrome(connected, updateAvailable);
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 24, 38, "Rules + quick start", 32, 36, 42);
    if (requiredVersion && requiredVersion[0])
    {
        char versionText[32];
        snprintf(versionText, sizeof(versionText), "v%s", requiredVersion);
        drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 344, 38, versionText, 73, 82, 92);
    }

    const char *rules[] = {
        "No sexual content.",
        "No heavy profanity or slurs.",
        "No harassment, threats, hate, or personal info.",
        "No intentional griefing, spam, or vandalism.",
        "Mods may clear, kick, mute, ban, or save evidence.",
    };
    int y = 64;
    for (int i = 0; i < 5; i++)
    {
        drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 26, y, "-", 13, 122, 117);
        drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 42, y, rules[i], 32, 36, 42);
        y += 18;
    }

    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 24, 166, "Draw on the bottom screen.", 73, 82, 92);
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 24, 184, "C-PAD or LEFT/A pans. RIGHT/Y shows zoom.", 73, 82, 92);
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 24, 202, "Hold L/R for eraser. UP/X samples color.", 73, 82, 92);
    if (needsAgreement)
        drawUpperText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 24, 220, "PRESS A TO AGREE AND CONTINUE", 196, 92, 40);
    drawFooterHint("", needsAgreement ? "B EXIT" : "B BACK");
    topFrameValid = true;
}

static void composeUsersTopFrame(CanvasState &canvas, bool connected, bool updateAvailable, PresenceUser *users, int userCount)
{
    drawTopChrome(connected, updateAvailable);
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 24, 46, "Connected users", 32, 36, 42);
    char countText[20];
    snprintf(countText, sizeof(countText), "%d online", userCount);
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 276, 46, countText, 73, 82, 92);

    int rows = std::min(userCount, 8);
    for (int i = 0; i < rows; i++)
    {
        int y = 78 + i * 18;
        drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 34, y, "-", 73, 82, 92);
        drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 52, y, users[i].displayName, 32, 36, 42);
        if (strcmp(users[i].role, "admin") == 0 || strcmp(users[i].role, "mod") == 0)
        {
            char roleLabel[12];
            formatTitleLabel(users[i].role, roleLabel, sizeof(roleLabel));
            drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 206, y, roleLabel, 13, 122, 117);
        }
    }
    if (rows == 0)
        drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 52, 92, "No users yet", 104, 114, 124);

    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 252, 104, "Channel", 73, 82, 92);
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
        snprintf(unreadText, sizeof(unreadText), "%d new", std::min(chatUnread, 99));
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
        {
            char roleLabel[12];
            formatTitleLabel(line.role, roleLabel, sizeof(roleLabel));
            snprintf(prefix, sizeof(prefix), "<%s>[%s]%s:", timeText, roleLabel, name);
        }
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

static void composeTicketsTopFrame(bool connected, bool updateAvailable,
                                   SupportTicketSummary *tickets, int ticketCount, int ticketSelected,
                                   int ticketView, bool staffScope, SupportTicketSummary *activeTicket,
                                   SupportTicketMessage *messages, int messageCount,
                                   int homeSelected, int actionSelected, bool supportOnly,
                                   const char *supportReason, const char *notice, int needsReplyCount,
                                   StaffChatMessage *staffMessages, int staffMessageCount, int staffChatUnread,
                                   int restrictionSecondsRemaining, bool restrictionHasDuration)
{
    drawTopChrome(connected, updateAvailable);
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 20, 36, supportOnly ? "Support access" : "Tickets", 32, 36, 42);
    if (needsReplyCount > 0)
    {
        char countText[24];
        snprintf(countText, sizeof(countText), "%d need reply", std::min(needsReplyCount, 99));
        drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 320, 36, countText, 196, 92, 40);
    }

    if (ticketView == 0)
    {
        const char *regularItems[] = { "New unban request", "Report a bug", "Request a feature", "My tickets" };
        const char *staffItems[] = { "New unban request", "Report a bug", "Request a feature", "My tickets", "Staff queue", "Staff chat" };
        const char *supportItems[] = { "New unban request", "My unban tickets" };
        const char **items = supportOnly ? supportItems : (staffScope ? staffItems : regularItems);
        int count = supportOnly ? 2 : (staffScope ? 6 : 4);
        for (int i = 0; i < count; i++)
            drawMenuRow(70 + i * 24, items[i], i == homeSelected);
        if (supportOnly)
        {
            drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 24, 150, "Canvas access is disabled.", 196, 92, 40);
            drawWrappedText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 24, 166, 54, 2,
                            supportReason && supportReason[0] ? supportReason : "Use an unban ticket to contact staff.",
                            73, 82, 92);
            char remaining[40];
            if (restrictionHasDuration)
                snprintf(remaining, sizeof(remaining), "Access returns in %02dh %02dm", restrictionSecondsRemaining / 3600, (restrictionSecondsRemaining / 60) % 60);
            else
                snprintf(remaining, sizeof(remaining), "No automatic expiration");
            drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 24, 190, remaining, 196, 92, 40);
        }
        drawFooterHint("A OPEN", supportOnly ? "" : "B MENU");
    }
    else if (ticketView == 1)
    {
        drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 20, 54, staffScope ? "Staff queue" : "My tickets", 13, 122, 117);
        for (int i = 0; tickets && i < ticketCount && i < 6; i++)
        {
            int y = 70 + i * 21;
            if (i == ticketSelected)
            {
                fillTopRect(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 14, y - 3, 372, 19, 224, 242, 238);
                strokeTopRect(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 14, y - 3, 372, 19, 13, 122, 117);
            }
            char category[24], status[24], prefix[64];
            formatTitleLabel(tickets[i].category, category, sizeof(category));
            formatTitleLabel(tickets[i].status, status, sizeof(status));
            snprintf(prefix, sizeof(prefix), "#%d %s / %s", tickets[i].id, category, status);
            drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 20, y, prefix, 13, 122, 117);
            char subject[25];
            snprintf(subject, sizeof(subject), "%.24s", tickets[i].subject);
            drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 154, y, subject, 32, 36, 42);
            if (staffScope)
            {
                char requester[13];
                snprintf(requester, sizeof(requester), "%.12s",
                         tickets[i].displayName[0] ? tickets[i].displayName : tickets[i].username);
                drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 310, y, requester, 104, 114, 124);
            }
        }
        if (ticketCount == 0)
            drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 40, 100, "No tickets found", 104, 114, 124);
        drawFooterHint("A OPEN  X REFRESH  Y NEXT", "B BACK");
    }
    else if (ticketView == 2 && activeTicket)
    {
        char category[24], status[24], heading[96];
        formatTitleLabel(activeTicket->category, category, sizeof(category));
        formatTitleLabel(activeTicket->status, status, sizeof(status));
        snprintf(heading, sizeof(heading), "#%d %s / %s", activeTicket->id, category, status);
        drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 20, 54, heading, 13, 122, 117);
        char ticketSubject[61];
        snprintf(ticketSubject, sizeof(ticketSubject), "%.60s", activeTicket->subject);
        drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 20, 68, ticketSubject, 32, 36, 42);
        if (staffScope)
        {
            char context[128];
            snprintf(context, sizeof(context), "%.42s  Blocks: %.12s",
                     activeTicket->banReason[0] ? activeTicket->banReason : "No ban reason",
                     activeTicket->blockTypes[0] ? activeTicket->blockTypes : "None");
            drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 20, 82, context, 196, 92, 40);
        }
        const int firstY = staffScope ? 98 : 82;
        int start = messageCount;
        int usedHeight = 0;
        for (int i = messageCount - 1; messages && i >= 0; i--)
        {
            int lines = wrappedLineCount(58, 3, messages[i].message);
            int rowHeight = 13 + lines * 10;
            if (usedHeight + rowHeight > 200 - firstY) break;
            usedHeight += rowHeight;
            start = i;
        }
        int y = firstY;
        for (int i = start; messages && i < messageCount && y < 200; i++)
        {
            char timeText[6] = "--:--";
            if (messages[i].createdAt[11] && messages[i].createdAt[12] && messages[i].createdAt[14] && messages[i].createdAt[15])
                snprintf(timeText, sizeof(timeText), "%.2s:%.2s", messages[i].createdAt + 11, messages[i].createdAt + 14);
            char authorKind[20], author[72];
            formatTitleLabel(messages[i].authorKind, authorKind, sizeof(authorKind));
            snprintf(author, sizeof(author), "[%s] %s  %s", authorKind,
                     messages[i].displayName[0] ? messages[i].displayName : "Staff", timeText);
            drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 20, y, author, 13, 122, 117);
            int lines = wrappedLineCount(58, 3, messages[i].message);
            drawWrappedText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 32, y + 11, 58, 3, messages[i].message, 32, 36, 42);
            y += 13 + lines * 10;
        }
        bool closed = strcmp(activeTicket->status, "resolved") == 0 || strcmp(activeTicket->status, "rejected") == 0;
        if (staffScope)
            drawFooterHint("A REPLY  X ACTIONS", "B LIST");
        else
            drawFooterHint(closed ? "CLOSED" : "A REPLY", "B LIST");
    }
    else if (ticketView == 3 && activeTicket)
    {
        drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 20, 56, "Staff actions", 13, 122, 117);
        const char *actions[] = { "Mark in progress", "Reply to user", "Resolve", "Reject", "Approve unban", "Reopen" };
        for (int i = 0; i < 6; i++)
        {
            bool unavailable = (i == 4 && strcmp(activeTicket->category, "unban") != 0);
            if (unavailable)
                drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 36, 80 + i * 22, "Approve unban (N/A)", 160, 166, 172);
            else
                drawMenuRow(80 + i * 22, actions[i], i == actionSelected);
        }
        drawFooterHint("A CONFIRM", "B THREAD");
    }
    else if (ticketView == 4)
    {
        char heading[40];
        snprintf(heading, sizeof(heading), "Staff chat%s", staffChatUnread > 0 ? " - new" : "");
        drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 20, 54, heading, 13, 122, 117);
        const int firstY = 70;
        int start = staffMessageCount;
        int usedHeight = 0;
        for (int i = staffMessageCount - 1; staffMessages && i >= 0; i--)
        {
            int lines = wrappedLineCount(58, 3, staffMessages[i].message);
            int rowHeight = 13 + lines * 10;
            if (usedHeight + rowHeight > 200 - firstY) break;
            usedHeight += rowHeight;
            start = i;
        }
        int y = firstY;
        for (int i = start; staffMessages && i < staffMessageCount && y < 200; i++)
        {
            char timeText[6] = "--:--";
            if (staffMessages[i].createdAt[11] && staffMessages[i].createdAt[12] && staffMessages[i].createdAt[14] && staffMessages[i].createdAt[15])
                snprintf(timeText, sizeof(timeText), "%.2s:%.2s", staffMessages[i].createdAt + 11, staffMessages[i].createdAt + 14);
            char role[16], prefix[72];
            const char *name = staffMessages[i].displayName[0] ? staffMessages[i].displayName : staffMessages[i].username;
            formatTitleLabel(staffMessages[i].role, role, sizeof(role));
            snprintf(prefix, sizeof(prefix), "[%s] %s  %s", role, name, timeText);
            drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 20, y, prefix, 13, 122, 117);
            int lines = wrappedLineCount(58, 3, staffMessages[i].message);
            drawWrappedText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 32, y + 11, 58, 3, staffMessages[i].message, 32, 36, 42);
            y += 13 + lines * 10;
        }
        if (staffMessageCount == 0)
            drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 40, 100, "No staff messages yet", 104, 114, 124);
        drawFooterHint("A SEND  X REFRESH  Y OLDER", "B BACK");
    }

    if (notice && notice[0])
    {
        char compactNotice[61];
        snprintf(compactNotice, sizeof(compactNotice), "%.60s", notice);
        drawUpperText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 20, 205, compactNotice, 196, 92, 40);
    }
    topFrameValid = true;
}

static void composeAdminTopFrame(CanvasState &canvas, bool connected, bool updateAvailable,
                                 const char *role, int selectedAdminItem, const char *adminNotice)
{
    drawTopChrome(connected, updateAvailable);
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 24, 46, "Staff tools", 32, 36, 42);
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 264, 46, "Role", 73, 82, 92);
    char roleLabel[12];
    formatTitleLabel(role, roleLabel, sizeof(roleLabel));
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 264, 60, roleLabel, 32, 36, 42);

    bool allowed = role && (strcmp(role, "mod") == 0 || strcmp(role, "admin") == 0);
    if (!allowed)
    {
        drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 24, 84, "Mod or admin required", 196, 92, 40);
        drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 24, 108, "Set your role in web admin", 104, 114, 124);
        drawFooterHint("", "B BACK");
        topFrameValid = true;
        return;
    }

    const char *items[] = {
        "Save snapshot",
        "Clear canvas",
        "Fill rectangle",
    };
    for (int i = 0; i < 3; i++)
        drawMenuRow(78 + i * 22, items[i], i == selectedAdminItem);

    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 252, 132, "Channel", 73, 82, 92);
    drawUpperText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 252, 146, canvas.channel[0] ? canvas.channel : "main", 32, 36, 42);
    if (adminNotice && adminNotice[0])
        drawUpperText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 24, 184, adminNotice, 196, 92, 40);
    else
        drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 24, 184, "Fill rectangle: Drag below", 104, 114, 124);
    drawFooterHint("A USE", "B BACK");
    topFrameValid = true;
}

static void composeStatusTopFrame(CanvasState &canvas, bool connected, bool updateAvailable)
{
    drawTopChrome(connected, updateAvailable);
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 24, 46, "Status", 32, 36, 42);
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 24, 78, "Connection", 73, 82, 92);
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 128, 78, connected ? "Online" : "Offline", connected ? 13 : 196, connected ? 122 : 61, connected ? 117 : 61);
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 24, 100, "Update", 73, 82, 92);
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 128, 100, updateAvailable ? "Available" : "Current", updateAvailable ? 196 : 32, updateAvailable ? 92 : 36, updateAvailable ? 40 : 42);
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 24, 122, "Version", 73, 82, 92);
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 128, 122, APP_BUILD_LABEL, 32, 36, 42);
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 24, 144, "Channel", 73, 82, 92);
    drawUpperText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 128, 144, canvas.channel[0] ? canvas.channel : "main", 32, 36, 42);
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 24, 166, "Zoom", 73, 82, 92);
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 128, 166, canvas.zoomLabel(), 32, 36, 42);
    drawFooterHint("START REFRESH", "B BACK");
    topFrameValid = true;
}

static void composeIdentityTopFrame(bool connected, bool updateAvailable,
                                    const char *displayName, const char *username,
                                    const char *role, const char *status,
                                    const char *backupCode,
                                    const char *identityNotice,
                                    const char *identityStorage, int restrictionSecondsRemaining,
                                    bool restrictionHasDuration, const char *restrictionReason)
{
    drawTopChrome(connected, updateAvailable);
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 24, 46, "Identity", 32, 36, 42);
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 24, 78, "Name", 73, 82, 92);
    drawUpperText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 128, 78, displayName, 32, 36, 42);
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 24, 100, "Account", 73, 82, 92);
    drawUpperText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 128, 100, username, 32, 36, 42);
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 24, 122, "Role", 73, 82, 92);
    char roleLabel[12], statusLabel[16];
    formatTitleLabel(role, roleLabel, sizeof(roleLabel));
    formatTitleLabel(status, statusLabel, sizeof(statusLabel));
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 128, 122, roleLabel, 32, 36, 42);
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 24, 144, "State", 73, 82, 92);
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 128, 144, statusLabel, 32, 36, 42);
    bool restricted = strcmp(status, "muted") == 0 || strcmp(status, "banned") == 0;
    if (restricted)
    {
        char remaining[40];
        if (restrictionHasDuration)
            snprintf(remaining, sizeof(remaining), "%02dh %02dm %02ds left", restrictionSecondsRemaining / 3600, (restrictionSecondsRemaining / 60) % 60, restrictionSecondsRemaining % 60);
        else
            snprintf(remaining, sizeof(remaining), "No automatic expiration");
        drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 214, 144, remaining, 196, 92, 40);
        if (restrictionReason && restrictionReason[0])
        {
            char compactReason[61];
            snprintf(compactReason, sizeof(compactReason), "%.60s", restrictionReason);
            drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 24, 184, compactReason, 196, 92, 40);
        }
    }
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 24, 166, "Backup", 73, 82, 92);
    if (backupCode && backupCode[0])
        drawUpperText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 128, 166, backupCode, 32, 36, 42);
    else
        drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 128, 166, "PRESS Y", 196, 92, 40);
    if (identityStorage && identityStorage[0])
        drawUpperText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 256, 166, identityStorage, 104, 114, 124);
    if (identityNotice && identityNotice[0])
    {
        char compactNotice[61];
        snprintf(compactNotice, sizeof(compactNotice), "%.60s", identityNotice);
        drawUpperText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 24, restricted ? 202 : 190, compactNotice, 196, 92, 40);
    }
    else if (!restricted)
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
                         SupportTicketSummary *tickets, int ticketCount, int ticketSelected,
                         int ticketView, bool ticketStaffScope, SupportTicketSummary *activeTicket,
                         SupportTicketMessage *ticketMessages, int ticketMessageCount,
                         StaffChatMessage *staffChatMessages, int staffChatMessageCount,
                         int ticketHomeSelected, int ticketActionSelected,
                         bool supportOnly, const char *supportReason,
                         const char *ticketNotice, int ticketNeedsReplyCount,
                         int staffChatUnreadCount, int restrictionSecondsRemaining,
                         bool restrictionHasDuration, const char *restrictionReason)
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
        composeMenuTopFrame(canvas, connected, updateAvailable, selectedMenuItem, ticketNeedsReplyCount, staffChatUnreadCount,
                            role && (strcmp(role, "mod") == 0 || strcmp(role, "admin") == 0));
    else if (mode == TOP_MODE_USERS)
        composeUsersTopFrame(canvas, connected, updateAvailable, users, userCount);
    else if (mode == TOP_MODE_RULES)
        composeRulesTopFrame(connected, updateAvailable, rulesVersion, needsRulesAgreement);
    else if (mode == TOP_MODE_TICKETS)
        composeTicketsTopFrame(connected, updateAvailable, tickets, ticketCount, ticketSelected,
                               ticketView, ticketStaffScope, activeTicket, ticketMessages, ticketMessageCount,
                               ticketHomeSelected, ticketActionSelected, supportOnly, supportReason,
                               ticketNotice, ticketNeedsReplyCount, staffChatMessages, staffChatMessageCount,
                               staffChatUnreadCount, restrictionSecondsRemaining, restrictionHasDuration);
    else if (mode == TOP_MODE_ADMIN)
        composeAdminTopFrame(canvas, connected, updateAvailable, role, selectedAdminItem, adminNotice);
    else if (mode == TOP_MODE_STATUS)
        composeStatusTopFrame(canvas, connected, updateAvailable);
    else if (mode == TOP_MODE_IDENTITY)
        composeIdentityTopFrame(connected, updateAvailable, displayName, username, role, status, backupCode,
                                identityNotice, identityStorage, restrictionSecondsRemaining, restrictionHasDuration, restrictionReason);
    else
        composeCanvasTopFrame(canvas, connected, updateAvailable, currentColor, brushSize, brushShape,
                              ticketNeedsReplyCount, staffChatUnreadCount, users, userCount);
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
