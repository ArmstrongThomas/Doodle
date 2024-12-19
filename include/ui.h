#ifndef UI_H
#define UI_H

#include <3ds.h>
#include <vector>

struct DrawPoint {
    int x, y;
};

struct Color {
    u8 r, g, b;
};

namespace UIInterface {
    void drawHSVSliders(u8* framebuffer, int screenWidth, int screenHeight, float& h, float& s, float& v);
    void drawCurrentSelection(u8* framebuffer, int screenWidth, int screenHeight, Color color);
    void drawUIBackground(u8* framebuffer, int screenWidth, int screenHeight);
}

class UIState {
public:
    static void init();
    static void toggleColorPicker();
    static bool isColorPickerActive();
    static void addPoint(int x, int y);
    static void clearPoints();
    static const std::vector<DrawPoint>& getPoints();
    static void updateHSV(float h, float s, float v);
    static void getHSV(float& h, float& s, float& v);
    static void HSVtoRGB(float h, float s, float v, float& r, float& g, float& b);

    static const int UI_MARGIN_X = 20;
    static const int UI_MARGIN_Y = 10;
    static const int UI_BG_COLOR_R = 200;
    static const int UI_BG_COLOR_G = 200;
    static const int UI_BG_COLOR_B = 200;

private:
    static bool colorPickerActive;
    static std::vector<DrawPoint> pointBuffer;
    static float hue, saturation, value;
};

#endif