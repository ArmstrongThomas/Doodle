#ifndef UI_CANVAS_H
#define UI_CANVAS_H

#include <3ds.h>
#include <stddef.h>

struct UiColor
{
    u8 r;
    u8 g;
    u8 b;

    UiColor(u8 red = 0, u8 green = 0, u8 blue = 0) : r(red), g(green), b(blue) {}
};

struct UiRect
{
    int x;
    int y;
    int w;
    int h;

    UiRect(int px = 0, int py = 0, int width = 0, int height = 0)
        : x(px), y(py), w(width), h(height) {}

    bool contains(int px, int py) const
    {
        return px >= x && py >= y && px < x + w && py < y + h;
    }
};

namespace UiGeometry
{
    float normalizedPositionClamped(int coordinate, int first, int last);
}

enum UiBufferLayout
{
    UI_BUFFER_RGB = 0,
    UI_BUFFER_BGR = 1,
    UI_BUFFER_3DS_ROTATED_BGR = 2,
};

namespace UiTheme
{
    extern const UiColor Background;
    extern const UiColor Surface;
    extern const UiColor Ink;
    extern const UiColor Secondary;
    extern const UiColor Border;
    extern const UiColor Accent;
    extern const UiColor AccentBright;
    extern const UiColor Warning;
    extern const UiColor Danger;
    extern const UiColor Disabled;
    extern const UiColor White;
}

class UiCanvas
{
public:
    UiCanvas(u8 *buffer, int storageWidth, int storageHeight, UiBufferLayout layout = UI_BUFFER_RGB);

    bool valid() const;
    int logicalWidth() const;
    int logicalHeight() const;

    void pixel(int x, int y, UiColor color);
    void fill(UiRect rect, UiColor color);
    void stroke(UiRect rect, UiColor color, int thickness = 1);
    void glyph(int x, int y, char c, UiColor color, int scale = 1);
    void text(int x, int y, const char *value, UiColor color, int scale = 1);
    void textClipped(int x, int y, const char *value, UiColor color,
                     int maxWidth, int scale = 1, bool ellipsis = true);
    int wrappedText(int x, int y, const char *value, UiColor color,
                    int maxWidth, int maxLines, int lineHeight = 10, int scale = 1);

    static int textWidth(const char *value, int scale = 1);
    static int fitTextScale(const char *value, int maxWidth, int preferredScale = 2);

private:
    u8 *buffer_;
    int storageWidth_;
    int storageHeight_;
    UiBufferLayout layout_;
};

namespace UiComponents
{
    void panel(UiCanvas &canvas, UiRect rect, bool raised = false);
    void button(UiCanvas &canvas, UiRect rect, const char *label,
                bool selected, bool danger = false, bool disabled = false);
    void tab(UiCanvas &canvas, UiRect rect, const char *label, bool selected, bool disabled = false);
    void listRow(UiCanvas &canvas, UiRect rect, const char *label,
                 const char *meta, bool selected, bool current = false, bool disabled = false);
    void badge(UiCanvas &canvas, UiRect rect, const char *label, UiColor color);
    void keycap(UiCanvas &canvas, int x, int y, const char *key, const char *label);
    void actionBar(UiCanvas &canvas, const char *left, const char *center, const char *right);
    void toast(UiCanvas &canvas, const char *message, UiColor color = UiTheme::Accent);
    void modal(UiCanvas &canvas, const char *title, const char *message,
               const char *confirmLabel, const char *cancelLabel, bool danger = false);
}

#endif
