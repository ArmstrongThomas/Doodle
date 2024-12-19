// UI-related functionality
#include "ui.h"

const int UI_MARGIN_X = 20;
const int UI_MARGIN_Y = 10;
const int UI_BG_COLOR_R = 200;
const int UI_BG_COLOR_G = 200;
const int UI_BG_COLOR_B = 200;


// UIState static member definitions
bool UIState::colorPickerActive = false;
std::vector<DrawPoint> UIState::pointBuffer;
float UIState::hue = 0.0f;
float UIState::saturation = 1.0f;
float UIState::value = 1.0f;

void UIState::init() {
    colorPickerActive = false;
    pointBuffer.clear();
    hue = 0.0f;
    saturation = 1.0f;
    value = 1.0f;
}

void UIState::toggleColorPicker() {
    colorPickerActive = !colorPickerActive;
}

bool UIState::isColorPickerActive() {
    return colorPickerActive;
}

void UIState::addPoint(int x, int y) {
    DrawPoint point = {x, y};
    pointBuffer.push_back(point);
}

void UIState::clearPoints() {
    pointBuffer.clear();
}

const std::vector<DrawPoint>& UIState::getPoints() {
    return pointBuffer;
}

void UIState::updateHSV(float h, float s, float v) {
    hue = h;
    saturation = s;
    value = v;
}

void UIState::getHSV(float& h, float& s, float& v) {
    h = hue;
    s = saturation;
    v = value;
}

void UIState::HSVtoRGB(float h, float s, float v, float& r, float& g, float& b) {
    if (s == 0.0f) {
        r = g = b = v;
        return;
    }
    h *= 6.0f;
    int i = (int)h;
    float f = h - i;
    float p = v * (1 - s);
    float q = v * (1 - s * f);
    float t = v * (1 - s * (1 - f));

    switch (i % 6) {
        case 0: r = v; g = t; b = p; break;
        case 1: r = q; g = v; b = p; break;
        case 2: r = p; g = v; b = t; break;
        case 3: r = p; g = q; b = v; break;
        case 4: r = t; g = p; b = v; break;
        case 5: r = v; g = p; b = q; break;
    }
}

namespace UIInterface {
    void drawHSVSliders(u8* framebuffer, int screenWidth, int screenHeight, float& hue, float& saturation, float& value) {
        int sliderHeight = 280;
        int sliderWidth = 20;
        int startX[] = {screenWidth - 140, screenWidth - 170, screenWidth - 200};

        for (int i = 0; i < 3; i++) {
            for (int y = 20; y < 20 + sliderHeight; y++) {
                float h = (i == 0) ? (float)(y - 20) / sliderHeight : hue;
                float s = (i == 1) ? (float)(y - 20) / sliderHeight : saturation;
                float v = (i == 2) ? (float)(y - 20) / sliderHeight : value;
                float r, g, b;
                UIState::HSVtoRGB(h, s, v, r, g, b);
                for (int x = startX[i] - sliderWidth; x < startX[i]; x++) {
                    int idx = 3 * (x + y * screenWidth);
                    framebuffer[idx] = b * 255;
                    framebuffer[idx + 1] = g * 255;
                    framebuffer[idx + 2] = r * 255;
                }
            }
        }

        // Draw slider indicators
        for (int i = 0; i < 3; i++) {
            float indicatorValue = (i == 0) ? hue : (i == 1) ? saturation : value;
            int indicatorY = 20 + (indicatorValue * sliderHeight);
            for (int x = startX[i] - sliderWidth - 5; x < startX[i] + 5; x++) {
                int idx = 3 * (x + indicatorY * screenWidth);
                framebuffer[idx] = framebuffer[idx + 1] = framebuffer[idx + 2] = 0; // Black indicator
            }
        }
    }

    void drawUIBackground(u8* framebuffer, int screenWidth, int screenHeight) {
        for (int x = screenWidth - 230; x < screenWidth - UIState::UI_MARGIN_X; x++) {
            for (int y = UIState::UI_MARGIN_Y; y < 310; y++) {
                int idx = 3 * (x + y * screenWidth);
                framebuffer[idx] = UIState::UI_BG_COLOR_B;
                framebuffer[idx + 1] = UIState::UI_BG_COLOR_G;
                framebuffer[idx + 2] = UIState::UI_BG_COLOR_R;
            }
        }
    }

    void drawCurrentSelection(u8* framebuffer, int screenWidth, int screenHeight, Color color) {
        if (UIState::isColorPickerActive()) {
            // Draw color rectangle
            int rectSize = 100;
            int rectX = screenWidth - UIState::UI_MARGIN_X - rectSize;
            int rectY = 200;

            for (int x = rectX; x < rectX + rectSize; x++) {
                for (int y = rectY; y < rectY + rectSize; y++) {
                    if (x >= 0 && x < screenWidth && y >= 0 && y < screenHeight) {
                        int idx = 3 * (x + y * screenWidth);
                        framebuffer[idx] = color.b;
                        framebuffer[idx + 1] = color.g;
                        framebuffer[idx + 2] = color.r;
                    }
                }
            }
        } else {
            // Draw small color rectangle when color picker is not active
            int rectSize = 10;
            int rectX = UIState::UI_MARGIN_X;
            int rectY = 300;

            for (int x = rectX; x < rectX + rectSize; x++) {
                for (int y = rectY; y < rectY + rectSize; y++) {
                    if (x >= 0 && x < screenWidth && y >= 0 && y < screenHeight) {
                        int idx = 3 * (x + y * screenWidth);
                        framebuffer[idx] = color.b;
                        framebuffer[idx + 1] = color.g;
                        framebuffer[idx + 2] = color.r;
                    }
                }
            }
        }
    }
}