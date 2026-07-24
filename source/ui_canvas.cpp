#include "ui_canvas.h"
#include <algorithm>
#include <string.h>

namespace UiTheme
{
    const UiColor Background(232, 236, 239);
    const UiColor Surface(248, 250, 251);
    const UiColor Ink(24, 33, 38);
    const UiColor Secondary(73, 82, 92);
    const UiColor Border(196, 204, 212);
    const UiColor Accent(13, 122, 117);
    const UiColor AccentBright(94, 234, 212);
    const UiColor Warning(196, 92, 40);
    const UiColor Danger(196, 61, 61);
    const UiColor Disabled(160, 166, 172);
    const UiColor White(245, 248, 250);
}

float UiGeometry::normalizedPositionClamped(int coordinate, int first, int last)
{
    if (last <= first)
        return 0.0f;
    coordinate = std::max(first, std::min(coordinate, last));
    return (float)(coordinate - first) / (float)(last - first);
}

UiCanvas::UiCanvas(u8 *buffer, int storageWidth, int storageHeight, UiBufferLayout layout)
    : buffer_(buffer), storageWidth_(storageWidth), storageHeight_(storageHeight), layout_(layout)
{
}

bool UiCanvas::valid() const
{
    return buffer_ && storageWidth_ > 0 && storageHeight_ > 0;
}

int UiCanvas::logicalWidth() const
{
    return layout_ == UI_BUFFER_3DS_ROTATED_BGR ? storageHeight_ : storageWidth_;
}

int UiCanvas::logicalHeight() const
{
    return layout_ == UI_BUFFER_3DS_ROTATED_BGR ? storageWidth_ : storageHeight_;
}

void UiCanvas::pixel(int x, int y, UiColor color)
{
    if (!valid() || x < 0 || y < 0 || x >= logicalWidth() || y >= logicalHeight())
        return;

    int storageX = x;
    int storageY = y;
    if (layout_ == UI_BUFFER_3DS_ROTATED_BGR)
    {
        storageX = storageWidth_ - 1 - y;
        storageY = x;
    }
    if (storageX < 0 || storageY < 0 || storageX >= storageWidth_ || storageY >= storageHeight_)
        return;

    const int index = 3 * (storageY * storageWidth_ + storageX);
    if (layout_ == UI_BUFFER_RGB)
    {
        buffer_[index] = color.r;
        buffer_[index + 1] = color.g;
        buffer_[index + 2] = color.b;
    }
    else
    {
        buffer_[index] = color.b;
        buffer_[index + 1] = color.g;
        buffer_[index + 2] = color.r;
    }
}

void UiCanvas::fill(UiRect rect, UiColor color)
{
    const int startX = std::max(0, rect.x);
    const int startY = std::max(0, rect.y);
    const int endX = std::min(logicalWidth(), rect.x + std::max(0, rect.w));
    const int endY = std::min(logicalHeight(), rect.y + std::max(0, rect.h));
    for (int y = startY; y < endY; ++y)
        for (int x = startX; x < endX; ++x)
            pixel(x, y, color);
}

void UiCanvas::stroke(UiRect rect, UiColor color, int thickness)
{
    if (rect.w <= 0 || rect.h <= 0)
        return;
    thickness = std::max(1, std::min(thickness, std::min(rect.w, rect.h)));
    fill(UiRect(rect.x, rect.y, rect.w, thickness), color);
    fill(UiRect(rect.x, rect.y + rect.h - thickness, rect.w, thickness), color);
    fill(UiRect(rect.x, rect.y + thickness, thickness, std::max(0, rect.h - thickness * 2)), color);
    fill(UiRect(rect.x + rect.w - thickness, rect.y + thickness, thickness,
                std::max(0, rect.h - thickness * 2)), color);
}

static void glyphRows(char c, u8 rows[7])
{
    memset(rows, 0, 7);
    switch (c)
    {
        case '0': { const u8 g[7]={0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}; memcpy(rows,g,7); break; }
        case '1': { const u8 g[7]={0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}; memcpy(rows,g,7); break; }
        case '2': { const u8 g[7]={0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}; memcpy(rows,g,7); break; }
        case '3': { const u8 g[7]={0x1E,0x01,0x01,0x0E,0x01,0x01,0x1E}; memcpy(rows,g,7); break; }
        case '4': { const u8 g[7]={0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}; memcpy(rows,g,7); break; }
        case '5': { const u8 g[7]={0x1F,0x10,0x10,0x1E,0x01,0x01,0x1E}; memcpy(rows,g,7); break; }
        case '6': { const u8 g[7]={0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}; memcpy(rows,g,7); break; }
        case '7': { const u8 g[7]={0x1F,0x01,0x02,0x04,0x08,0x08,0x08}; memcpy(rows,g,7); break; }
        case '8': { const u8 g[7]={0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}; memcpy(rows,g,7); break; }
        case '9': { const u8 g[7]={0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}; memcpy(rows,g,7); break; }
        case 'A': { const u8 g[7]={0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}; memcpy(rows,g,7); break; }
        case 'B': { const u8 g[7]={0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}; memcpy(rows,g,7); break; }
        case 'C': { const u8 g[7]={0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}; memcpy(rows,g,7); break; }
        case 'D': { const u8 g[7]={0x1E,0x11,0x11,0x11,0x11,0x11,0x1E}; memcpy(rows,g,7); break; }
        case 'E': { const u8 g[7]={0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}; memcpy(rows,g,7); break; }
        case 'F': { const u8 g[7]={0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}; memcpy(rows,g,7); break; }
        case 'G': { const u8 g[7]={0x0E,0x11,0x10,0x17,0x11,0x11,0x0F}; memcpy(rows,g,7); break; }
        case 'H': { const u8 g[7]={0x11,0x11,0x11,0x1F,0x11,0x11,0x11}; memcpy(rows,g,7); break; }
        case 'I': { const u8 g[7]={0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}; memcpy(rows,g,7); break; }
        case 'J': { const u8 g[7]={0x07,0x02,0x02,0x02,0x12,0x12,0x0C}; memcpy(rows,g,7); break; }
        case 'K': { const u8 g[7]={0x11,0x12,0x14,0x18,0x14,0x12,0x11}; memcpy(rows,g,7); break; }
        case 'L': { const u8 g[7]={0x10,0x10,0x10,0x10,0x10,0x10,0x1F}; memcpy(rows,g,7); break; }
        case 'M': { const u8 g[7]={0x11,0x1B,0x15,0x15,0x11,0x11,0x11}; memcpy(rows,g,7); break; }
        case 'N': { const u8 g[7]={0x11,0x19,0x15,0x13,0x11,0x11,0x11}; memcpy(rows,g,7); break; }
        case 'O': { const u8 g[7]={0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}; memcpy(rows,g,7); break; }
        case 'P': { const u8 g[7]={0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}; memcpy(rows,g,7); break; }
        case 'Q': { const u8 g[7]={0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}; memcpy(rows,g,7); break; }
        case 'R': { const u8 g[7]={0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}; memcpy(rows,g,7); break; }
        case 'S': { const u8 g[7]={0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}; memcpy(rows,g,7); break; }
        case 'T': { const u8 g[7]={0x1F,0x04,0x04,0x04,0x04,0x04,0x04}; memcpy(rows,g,7); break; }
        case 'U': { const u8 g[7]={0x11,0x11,0x11,0x11,0x11,0x11,0x0E}; memcpy(rows,g,7); break; }
        case 'V': { const u8 g[7]={0x11,0x11,0x11,0x11,0x11,0x0A,0x04}; memcpy(rows,g,7); break; }
        case 'W': { const u8 g[7]={0x11,0x11,0x11,0x15,0x15,0x15,0x0A}; memcpy(rows,g,7); break; }
        case 'X': { const u8 g[7]={0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}; memcpy(rows,g,7); break; }
        case 'Y': { const u8 g[7]={0x11,0x11,0x0A,0x04,0x04,0x04,0x04}; memcpy(rows,g,7); break; }
        case 'Z': { const u8 g[7]={0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}; memcpy(rows,g,7); break; }
        case 'a': { const u8 g[7]={0x00,0x00,0x0E,0x01,0x0F,0x11,0x0F}; memcpy(rows,g,7); break; }
        case 'b': { const u8 g[7]={0x10,0x10,0x16,0x19,0x11,0x11,0x1E}; memcpy(rows,g,7); break; }
        case 'c': { const u8 g[7]={0x00,0x00,0x0E,0x10,0x10,0x11,0x0E}; memcpy(rows,g,7); break; }
        case 'd': { const u8 g[7]={0x01,0x01,0x0D,0x13,0x11,0x11,0x0F}; memcpy(rows,g,7); break; }
        case 'e': { const u8 g[7]={0x00,0x00,0x0E,0x11,0x1F,0x10,0x0E}; memcpy(rows,g,7); break; }
        case 'f': { const u8 g[7]={0x06,0x08,0x08,0x1C,0x08,0x08,0x08}; memcpy(rows,g,7); break; }
        case 'g': { const u8 g[7]={0x00,0x00,0x0F,0x11,0x0F,0x01,0x0E}; memcpy(rows,g,7); break; }
        case 'h': { const u8 g[7]={0x10,0x10,0x16,0x19,0x11,0x11,0x11}; memcpy(rows,g,7); break; }
        case 'i': { const u8 g[7]={0x04,0x00,0x0C,0x04,0x04,0x04,0x0E}; memcpy(rows,g,7); break; }
        case 'j': { const u8 g[7]={0x02,0x00,0x06,0x02,0x02,0x12,0x0C}; memcpy(rows,g,7); break; }
        case 'k': { const u8 g[7]={0x10,0x10,0x12,0x14,0x18,0x14,0x12}; memcpy(rows,g,7); break; }
        case 'l': { const u8 g[7]={0x0C,0x04,0x04,0x04,0x04,0x04,0x0E}; memcpy(rows,g,7); break; }
        case 'm': { const u8 g[7]={0x00,0x00,0x1A,0x15,0x15,0x15,0x15}; memcpy(rows,g,7); break; }
        case 'n': { const u8 g[7]={0x00,0x00,0x16,0x19,0x11,0x11,0x11}; memcpy(rows,g,7); break; }
        case 'o': { const u8 g[7]={0x00,0x00,0x0E,0x11,0x11,0x11,0x0E}; memcpy(rows,g,7); break; }
        case 'p': { const u8 g[7]={0x00,0x00,0x1E,0x11,0x1E,0x10,0x10}; memcpy(rows,g,7); break; }
        case 'q': { const u8 g[7]={0x00,0x00,0x0D,0x13,0x0F,0x01,0x01}; memcpy(rows,g,7); break; }
        case 'r': { const u8 g[7]={0x00,0x00,0x16,0x19,0x10,0x10,0x10}; memcpy(rows,g,7); break; }
        case 's': { const u8 g[7]={0x00,0x00,0x0F,0x10,0x0E,0x01,0x1E}; memcpy(rows,g,7); break; }
        case 't': { const u8 g[7]={0x08,0x08,0x1C,0x08,0x08,0x09,0x06}; memcpy(rows,g,7); break; }
        case 'u': { const u8 g[7]={0x00,0x00,0x11,0x11,0x11,0x13,0x0D}; memcpy(rows,g,7); break; }
        case 'v': { const u8 g[7]={0x00,0x00,0x11,0x11,0x11,0x0A,0x04}; memcpy(rows,g,7); break; }
        case 'w': { const u8 g[7]={0x00,0x00,0x11,0x15,0x15,0x15,0x0A}; memcpy(rows,g,7); break; }
        case 'x': { const u8 g[7]={0x00,0x00,0x11,0x0A,0x04,0x0A,0x11}; memcpy(rows,g,7); break; }
        case 'y': { const u8 g[7]={0x00,0x00,0x11,0x11,0x0F,0x01,0x0E}; memcpy(rows,g,7); break; }
        case 'z': { const u8 g[7]={0x00,0x00,0x1F,0x02,0x04,0x08,0x1F}; memcpy(rows,g,7); break; }
        case '-': { const u8 g[7]={0,0,0,0x1F,0,0,0}; memcpy(rows,g,7); break; }
        case ':': { const u8 g[7]={0,0x04,0,0,0x04,0,0}; memcpy(rows,g,7); break; }
        case '.': { const u8 g[7]={0,0,0,0,0,0x0C,0x0C}; memcpy(rows,g,7); break; }
        case '/': { const u8 g[7]={0x01,0x01,0x02,0x04,0x08,0x10,0x10}; memcpy(rows,g,7); break; }
        case '<': { const u8 g[7]={0x02,0x04,0x08,0x10,0x08,0x04,0x02}; memcpy(rows,g,7); break; }
        case '>': { const u8 g[7]={0x08,0x04,0x02,0x01,0x02,0x04,0x08}; memcpy(rows,g,7); break; }
        case '[': { const u8 g[7]={0x0E,0x08,0x08,0x08,0x08,0x08,0x0E}; memcpy(rows,g,7); break; }
        case ']': { const u8 g[7]={0x0E,0x02,0x02,0x02,0x02,0x02,0x0E}; memcpy(rows,g,7); break; }
        case '_': { const u8 g[7]={0,0,0,0,0,0,0x1F}; memcpy(rows,g,7); break; }
        case '!': { const u8 g[7]={0x04,0x04,0x04,0x04,0x04,0,0x04}; memcpy(rows,g,7); break; }
        case '?': { const u8 g[7]={0x0E,0x11,0x01,0x02,0x04,0,0x04}; memcpy(rows,g,7); break; }
        case ',': { const u8 g[7]={0,0,0,0,0,0x04,0x08}; memcpy(rows,g,7); break; }
        case '\'': { const u8 g[7]={0x04,0x04,0x08,0,0,0,0}; memcpy(rows,g,7); break; }
        case '"': { const u8 g[7]={0x0A,0x0A,0x14,0,0,0,0}; memcpy(rows,g,7); break; }
        case '#': { const u8 g[7]={0x0A,0x1F,0x0A,0x0A,0x1F,0x0A,0}; memcpy(rows,g,7); break; }
        case '%': { const u8 g[7]={0x19,0x19,0x02,0x04,0x08,0x13,0x13}; memcpy(rows,g,7); break; }
        case '&': { const u8 g[7]={0x0C,0x12,0x14,0x08,0x15,0x12,0x0D}; memcpy(rows,g,7); break; }
        case '(': { const u8 g[7]={0x02,0x04,0x08,0x08,0x08,0x04,0x02}; memcpy(rows,g,7); break; }
        case ')': { const u8 g[7]={0x08,0x04,0x02,0x02,0x02,0x04,0x08}; memcpy(rows,g,7); break; }
        case '*': { const u8 g[7]={0,0x15,0x0E,0x1F,0x0E,0x15,0}; memcpy(rows,g,7); break; }
        case '+': { const u8 g[7]={0,0x04,0x04,0x1F,0x04,0x04,0}; memcpy(rows,g,7); break; }
        case '=': { const u8 g[7]={0,0x1F,0,0x1F,0,0,0}; memcpy(rows,g,7); break; }
        case '@': { const u8 g[7]={0x0E,0x11,0x17,0x15,0x17,0x10,0x0E}; memcpy(rows,g,7); break; }
        case '|': { const u8 g[7]={0x04,0x04,0x04,0x04,0x04,0x04,0x04}; memcpy(rows,g,7); break; }
        case ' ': break;
        default: { const u8 g[7]={0x1F,0x11,0x15,0x15,0x11,0x11,0x1F}; memcpy(rows,g,7); break; }
    }
}

void UiCanvas::glyph(int x, int y, char c, UiColor color, int scale)
{
    scale = std::max(1, scale);
    u8 rows[7];
    glyphRows(c, rows);
    for (int gy = 0; gy < 7; ++gy)
    {
        for (int gx = 0; gx < 5; ++gx)
        {
            if (!(rows[gy] & (1 << (4 - gx))))
                continue;
            fill(UiRect(x + gx * scale, y + gy * scale, scale, scale), color);
        }
    }
}

void UiCanvas::text(int x, int y, const char *value, UiColor color, int scale)
{
    if (!value)
        return;
    const int advance = 6 * std::max(1, scale);
    for (const char *p = value; *p; ++p, x += advance)
        if (*p != ' ')
            glyph(x, y, *p, color, scale);
}

int UiCanvas::textWidth(const char *value, int scale)
{
    return value ? (int)strlen(value) * 6 * std::max(1, scale) : 0;
}

int UiCanvas::fitTextScale(const char *value, int maxWidth, int preferredScale)
{
    int scale = std::max(1, preferredScale);
    while (scale > 1 && textWidth(value, scale) > maxWidth)
        --scale;
    return scale;
}

void UiCanvas::textClipped(int x, int y, const char *value, UiColor color,
                           int maxWidth, int scale, bool ellipsis)
{
    if (!value || maxWidth <= 0)
        return;
    const int advance = 6 * std::max(1, scale);
    const int capacity = std::max(0, maxWidth / advance);
    const int length = (int)strlen(value);
    if (length <= capacity)
    {
        text(x, y, value, color, scale);
        return;
    }
    if (capacity <= 0)
        return;

    char clipped[96];
    const int safeCapacity = std::min(capacity, (int)sizeof(clipped) - 1);
    int copyLength = safeCapacity;
    if (ellipsis && safeCapacity >= 3)
        copyLength -= 3;
    memcpy(clipped, value, copyLength);
    if (ellipsis && safeCapacity >= 3)
    {
        clipped[copyLength++] = '.';
        clipped[copyLength++] = '.';
        clipped[copyLength++] = '.';
    }
    clipped[copyLength] = '\0';
    text(x, y, clipped, color, scale);
}

int UiCanvas::wrappedText(int x, int y, const char *value, UiColor color,
                          int maxWidth, int maxLines, int lineHeight, int scale)
{
    if (!value || maxWidth <= 0 || maxLines <= 0)
        return 0;
    const int maxChars = std::max(1, maxWidth / (6 * std::max(1, scale)));
    const char *cursor = value;
    int line = 0;
    while (*cursor && line < maxLines)
    {
        int length = 0;
        int lastSpace = -1;
        while (cursor[length] && cursor[length] != '\n' && length < maxChars)
        {
            if (cursor[length] == ' ')
                lastSpace = length;
            ++length;
        }
        const bool explicitBreak = cursor[length] == '\n';
        if (!explicitBreak && cursor[length] && lastSpace > 0)
            length = lastSpace;
        if (length <= 0 && !explicitBreak)
            length = 1;

        char textLine[96];
        const int copyLength = std::min(length, (int)sizeof(textLine) - 1);
        memcpy(textLine, cursor, copyLength);
        textLine[copyLength] = '\0';
        cursor += length;
        if (*cursor == '\n')
            ++cursor;
        else
            while (*cursor == ' ')
                ++cursor;

        if (line == maxLines - 1 && *cursor)
        {
            const int visibleChars = std::max(1, std::min(
                std::min(copyLength, maxChars), (int)sizeof(textLine) - 1));
            const int dotCount = std::min(3, visibleChars);
            const int ellipsisStart = visibleChars - dotCount;
            for (int dot = 0; dot < dotCount; ++dot)
                textLine[ellipsisStart + dot] = '.';
            textLine[visibleChars] = '\0';
            text(x, y + line * lineHeight, textLine, color, scale);
        }
        else
            text(x, y + line * lineHeight, textLine, color, scale);
        ++line;
    }
    return line;
}

namespace UiComponents
{
    void panel(UiCanvas &canvas, UiRect rect, bool raised)
    {
        canvas.fill(rect, raised ? UiTheme::Surface : UiTheme::Background);
        canvas.stroke(rect, UiTheme::Border);
    }

    void button(UiCanvas &canvas, UiRect rect, const char *label,
                bool selected, bool danger, bool disabled)
    {
        UiColor fill = selected ? UiTheme::Ink : UiTheme::Surface;
        UiColor textColor = selected ? UiTheme::White : UiTheme::Ink;
        if (danger)
        {
            fill = selected ? UiTheme::Danger : UiColor(255, 236, 236);
            textColor = selected ? UiTheme::White : UiTheme::Danger;
        }
        if (disabled)
        {
            fill = UiTheme::Background;
            textColor = UiTheme::Disabled;
        }
        canvas.fill(rect, fill);
        canvas.stroke(rect, selected ? (danger ? UiTheme::Danger : UiTheme::Accent) : UiTheme::Border,
                      selected ? 2 : 1);
        const int labelWidth = UiCanvas::textWidth(label);
        canvas.textClipped(rect.x + std::max(4, (rect.w - labelWidth) / 2),
                           rect.y + std::max(3, (rect.h - 7) / 2), label, textColor,
                           rect.w - 8);
    }

    void tab(UiCanvas &canvas, UiRect rect, const char *label, bool selected, bool disabled)
    {
        button(canvas, rect, label, selected, false, disabled);
        if (selected)
            canvas.fill(UiRect(rect.x + 2, rect.y + rect.h - 3, rect.w - 4, 3), UiTheme::AccentBright);
    }

    void listRow(UiCanvas &canvas, UiRect rect, const char *label,
                 const char *meta, bool selected, bool current, bool disabled)
    {
        const UiColor fill = selected ? UiTheme::Ink : (current ? UiColor(224, 242, 238) : UiTheme::Surface);
        canvas.fill(rect, fill);
        canvas.stroke(rect, selected ? UiTheme::Accent : UiTheme::Border, selected ? 2 : 1);
        canvas.text(rect.x + 8, rect.y + (rect.h - 7) / 2, selected ? ">" : (current ? "*" : " "),
                    selected ? UiTheme::AccentBright : UiTheme::Secondary);
        const UiColor textColor = disabled ? UiTheme::Disabled : (selected ? UiTheme::White : UiTheme::Ink);
        const int metaWidth = meta ? UiCanvas::textWidth(meta) : 0;
        canvas.textClipped(rect.x + 22, rect.y + (rect.h - 7) / 2, label, textColor,
                           rect.w - 34 - metaWidth);
        if (meta && meta[0])
            canvas.text(rect.x + rect.w - metaWidth - 8, rect.y + (rect.h - 7) / 2, meta,
                        selected ? UiTheme::White : UiTheme::Secondary);
    }

    void badge(UiCanvas &canvas, UiRect rect, const char *label, UiColor color)
    {
        canvas.fill(rect, color);
        canvas.stroke(rect, UiTheme::Ink);
        canvas.textClipped(rect.x + 4, rect.y + (rect.h - 7) / 2, label, UiTheme::White, rect.w - 8);
    }

    void keycap(UiCanvas &canvas, int x, int y, const char *key, const char *label)
    {
        const int keyWidth = std::max(18, UiCanvas::textWidth(key) + 8);
        canvas.fill(UiRect(x, y, keyWidth, 15), UiTheme::Ink);
        canvas.stroke(UiRect(x, y, keyWidth, 15), UiTheme::Accent);
        canvas.text(x + 4, y + 4, key, UiTheme::White);
        if (label && label[0])
            canvas.text(x + keyWidth + 5, y + 4, label, UiTheme::Secondary);
    }

    void actionBar(UiCanvas &canvas, const char *left, const char *center, const char *right)
    {
        const int y = canvas.logicalHeight() - 22;
        canvas.fill(UiRect(0, y, canvas.logicalWidth(), 22), UiTheme::Background);
        canvas.fill(UiRect(0, y, canvas.logicalWidth(), 1), UiTheme::Border);
        if (left && left[0])
            canvas.textClipped(8, y + 8, left, UiTheme::Secondary, canvas.logicalWidth() / 3 - 8);
        if (center && center[0])
            canvas.textClipped(canvas.logicalWidth() / 3, y + 8, center, UiTheme::Secondary,
                               canvas.logicalWidth() / 3);
        if (right && right[0])
        {
            const int width = UiCanvas::textWidth(right);
            canvas.textClipped(std::max(8, canvas.logicalWidth() - width - 8), y + 8,
                               right, UiTheme::Secondary, width);
        }
    }

    void toast(UiCanvas &canvas, const char *message, UiColor color)
    {
        if (!message || !message[0])
            return;
        UiRect rect(8, canvas.logicalHeight() - 46, canvas.logicalWidth() - 16, 22);
        canvas.fill(rect, UiTheme::Ink);
        canvas.stroke(rect, color, 2);
        canvas.textClipped(rect.x + 8, rect.y + 8, message, UiTheme::White,
                           rect.w - 54);
        canvas.text(rect.x + rect.w - 28, rect.y + 8, "TAP",
                    UiTheme::AccentBright);
    }

    void modal(UiCanvas &canvas, const char *title, const char *message,
               const char *confirmLabel, const char *cancelLabel, bool danger)
    {
        canvas.fill(UiRect(0, 0, canvas.logicalWidth(), canvas.logicalHeight()), UiColor(210, 216, 220));
        UiRect box(18, 46, canvas.logicalWidth() - 36, 142);
        panel(canvas, box, true);
        canvas.stroke(box, danger ? UiTheme::Danger : UiTheme::Accent, 2);
        const int titleWidth = box.w - 24;
        const int titleScale = UiCanvas::fitTextScale(title, titleWidth, 2);
        const int titleY = box.y + (titleScale == 2 ? 12 : 16);
        canvas.textClipped(box.x + 12, titleY, title,
                           danger ? UiTheme::Danger : UiTheme::Ink,
                           titleWidth, titleScale);
        canvas.wrappedText(box.x + 12, box.y + 42, message, UiTheme::Secondary,
                           box.w - 24, 5, 10);
        button(canvas, UiRect(box.x + 12, box.y + box.h - 38, (box.w - 32) / 2, 28),
               confirmLabel, true, danger);
        button(canvas, UiRect(box.x + 20 + (box.w - 32) / 2, box.y + box.h - 38,
                              (box.w - 32) / 2, 28), cancelLabel, false);
    }
}
