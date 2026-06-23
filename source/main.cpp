#include <3ds.h>
#include <stdio.h>
#include <string.h>
#include <malloc.h>
#include <cstdlib>
#include <stdarg.h>
#include <vector>
#include <string>
#include <sstream>
#include <math.h>
#include <algorithm>
#include <unordered_map>
#include "ui.h"
#include "network.h"
#include "canvas_state.h"
#include "renderer.h"
#include "protocol.h"
#include "updater.h"

#define APP_VERSION "1.0.0"

Color currentColor = {255, 0, 0}; // Red by default
int currentBrushSize = 1;
int currentBrushShape = 0;

static void failExit(const char *fmt, ...)
{
    consoleInit(GFX_TOP, NULL);
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\nPress B to exit.\n");
    while (aptMainLoop())
    {
        gspWaitForVBlank();
        hidScanInput();
        if (hidKeysDown() & KEY_B)
            break;
    }
    gfxExit();
    exit(0);
}

std::unordered_map<int, std::vector<float>> gaussianFalloffTables;

std::vector<float> computeGaussianFalloff(int radius)
{
    float sigma = radius / 1.5f; // Adjust softness (larger = softer)
    float twoSigmaSquared = 2.0f * sigma * sigma;

    int diameter = 2 * radius + 1;
    std::vector<float> table(diameter * diameter, 0.0f);

    for (int y = -radius; y <= radius; y++)
    {
        for (int x = -radius; x <= radius; x++)
        {
            float distanceSquared = x * x + y * y;
            if (distanceSquared <= radius * radius)
            {
                table[(y + radius) * diameter + (x + radius)] = exp(-distanceSquared / twoSigmaSquared);
            }
        }
    }

    return table;
}

void initializeGaussianFalloff(const std::vector<int> &brushSizes)
{
    for (int size : brushSizes)
    {
        int radius = size / 2;
        gaussianFalloffTables[size] = computeGaussianFalloff(radius);
    }
}

void writeColor(u8 *buffer, int idx, u8 r, u8 g, u8 b)
{
    buffer[idx] = b;
    buffer[idx + 1] = g;
    buffer[idx + 2] = r;
}

void drawPointOnBuffer(u8 *buffer, int fbWidth, int fbHeight, int x, int y, u8 r, u8 g, u8 b)
{
    if (x >= 0 && x < fbWidth && y >= 0 && y < fbHeight)
    {
        int idx = 3 * (y * fbWidth + x);
        buffer[idx] = r;     // Red
        buffer[idx + 1] = g; // Green
        buffer[idx + 2] = b; // Blue
    }
}
u8 clampColor(float colorValue)
{
    return static_cast<u8>(std::max(0.0f, std::min(255.0f, colorValue)));
}

void drawBrush(u8 *buffer, int fbWidth, int fbHeight, int centerX, int centerY, int size, int shape, u8 r, u8 g, u8 b)
{
    for (int y = -size / 2; y <= size / 2; y++)
    {
        for (int x = -size / 2; x <= size / 2; x++)
        {
            if (shape == 0)
            { // Circle
                if (x * x + y * y <= (size / 2) * (size / 2))
                {
                    drawPointOnBuffer(buffer, fbWidth, fbHeight, centerX + x, centerY + y, r, g, b);
                }
            }
            else if (shape == 1)
            { // Square
                drawPointOnBuffer(buffer, fbWidth, fbHeight, centerX + x, centerY + y, r, g, b);
            }
            else if (shape == 2) // Gaussian Soft Brush
            {
                int radius = size / 2;
                const std::vector<float> &falloffTable = gaussianFalloffTables[size];
                int diameter = 2 * radius + 1;

                for (int y = -radius; y <= radius; y++)
                {
                    for (int x = -radius; x <= radius; x++)
                    {
                        if (centerY + y < 0 || centerY + y >= fbHeight || centerX + x < 0 || centerX + x >= fbWidth)
                            continue;

                        float intensity = falloffTable[(y + radius) * diameter + (x + radius)];
                        if (intensity <= 0.0f)
                            continue;

                        int idx = 3 * ((centerY + y) * fbWidth + (centerX + x));
                        u8 baseB = buffer[idx + 2];
                        u8 baseG = buffer[idx + 1];
                        u8 baseR = buffer[idx];

                        u8 blendedR = std::round(r * intensity + baseR * (1.0f - intensity));
                        u8 blendedG = std::round(g * intensity + baseG * (1.0f - intensity));
                        u8 blendedB = std::round(b * intensity + baseB * (1.0f - intensity));

                        drawPointOnBuffer(buffer, fbWidth, fbHeight, centerX + x, centerY + y, blendedR, blendedG, blendedB);
                    }
                }
            }
        }
    }
}

static void drawStrokeSample(u8 *fullCanvas, int canvasWidth, int canvasHeight,
                             int screenX, int screenY, int offsetX, int offsetY, CanvasState &canvas)
{
    int canvasX = screenX + offsetX;
    int canvasY = screenY + offsetY;
    if (canvasX < 0 || canvasX >= canvasWidth || canvasY < 0 || canvasY >= canvasHeight)
        return;

    drawBrush(fullCanvas, canvasWidth, canvasHeight, canvasX, canvasY,
              currentBrushSize, currentBrushShape,
              currentColor.r, currentColor.g, currentColor.b);
    canvas.markDirty(canvasX, canvasY, currentBrushSize);
    UIState::addPoint(canvasX, canvasY);
}

static void drawStrokeLine(u8 *fullCanvas, int canvasWidth, int canvasHeight,
                           float x0, float y0, float x1, float y1,
                           int offsetX, int offsetY, CanvasState &canvas)
{
    int steps = std::max(1, (int)std::ceil(std::max(fabsf(x1 - x0), fabsf(y1 - y0))));
    for (int i = 0; i <= steps; i++)
    {
        float t = (float)i / (float)steps;
        int x = (int)std::round(x0 + (x1 - x0) * t);
        int y = (int)std::round(y0 + (y1 - y0) * t);
        drawStrokeSample(fullCanvas, canvasWidth, canvasHeight, x, y, offsetX, offsetY, canvas);
    }
}

static void drawStrokeCurve(u8 *fullCanvas, int canvasWidth, int canvasHeight,
                            float x0, float y0, float cx, float cy, float x1, float y1,
                            int offsetX, int offsetY, CanvasState &canvas)
{
    float lengthEstimate = hypotf(cx - x0, cy - y0) + hypotf(x1 - cx, y1 - cy);
    int steps = std::max(2, (int)std::ceil(lengthEstimate * 1.25f));
    for (int i = 0; i <= steps; i++)
    {
        float t = (float)i / (float)steps;
        float inv = 1.0f - t;
        int x = (int)std::round(inv * inv * x0 + 2.0f * inv * t * cx + t * t * x1);
        int y = (int)std::round(inv * inv * y0 + 2.0f * inv * t * cy + t * t * y1);
        drawStrokeSample(fullCanvas, canvasWidth, canvasHeight, x, y, offsetX, offsetY, canvas);
    }
}

void processDrawPacket(const uint8_t *packet, size_t length, u8 *buffer, int fbWidth, int fbHeight,
                       u8 *fullCanvas, int canvasWidth, int canvasHeight)
{
    if (length < 7)
        return; // Packet too short

    uint8_t r = packet[1];
    uint8_t g = packet[2];
    uint8_t b = packet[3];
    uint8_t size = packet[4];
    uint8_t shape = packet[5];
    uint8_t numPoints = packet[6];

    if (length != static_cast<size_t>(7 + numPoints * 4))
        return; // Invalid packet length

    int prevX = -1, prevY = -1;

    for (int i = 0; i < numPoints; i++)
    {
        uint16_t x = *(uint16_t *)(packet + 7 + i * 4);
        uint16_t y = *(uint16_t *)(packet + 9 + i * 4);

        if (prevX != -1 && prevY != -1)
        {
            // Draw line on fullCanvas
            int steps = std::max(abs(x - prevX), abs(y - prevY));
            for (int j = 0; j <= steps; j++)
            {
                float t = (steps == 0) ? 0.0f : static_cast<float>(j) / steps;
                int drawX = prevX + (x - prevX) * t;
                int drawY = prevY + (y - prevY) * t;
                drawBrush(fullCanvas, canvasWidth, canvasHeight, drawX, drawY, size, shape, r, g, b);
            }
        }

        prevX = x;
        prevY = y;
    }
}

void drawBrushSizeSelector(u8 *framebuffer, int screenWidth, int screenHeight)
{
    std::vector<int> brushSizes = {1, 2, 3, 5, 7};
    std::vector<int> brushPositions = {30, 60, 90, 120, 150};

    for (size_t i = 0; i < brushSizes.size(); i++)
    {
        int size = brushSizes[i];
        int y = brushPositions[i];

        // Draw circular brush
        int x = screenWidth - 40;
        drawBrush(framebuffer, screenWidth, screenHeight, x, y, size, 0, 0, 0, 0);

        // Draw square brush
        int squareX = screenWidth - 70;
        drawBrush(framebuffer, screenWidth, screenHeight, squareX, y, size, 1, 0, 0, 0);

        // Draw antialiased circular brush
        int antialiasedX = screenWidth - 100;
        drawBrush(framebuffer, screenWidth, screenHeight, antialiasedX, y, size, 2, 0, 0, 0);

        // Highlight the selected brush size and shape
        if (currentBrushSize == size)
        {
            int highlightX = (currentBrushShape == 0) ? x : (currentBrushShape == 1) ? squareX
                                                                                     : antialiasedX;
            int highlightY = y;
            for (int angle = 0; angle < 360; angle++)
            {
                float rad = angle * M_PI / 180.0f;
                int px = highlightX + 12 * cos(rad);
                int py = highlightY + 12 * sin(rad);
                if (px >= 0 && px < screenWidth && py >= 0 && py < screenHeight)
                {
                    int idx = 3 * (px + py * screenWidth);
                    framebuffer[idx] = 255; // Yellow border
                    framebuffer[idx + 1] = 255;
                    framebuffer[idx + 2] = 0;
                }
            }
        }
    }
}

static void sendDrawBatchCommand(int sock, const std::vector<DrawPoint> &points, const Color &color, int size, int shape)
{
    if (sock < 0 || points.empty())
        return;

    const size_t maxPointsPerPacket = 64;
    size_t start = 0;
    while (start < points.size())
    {
        size_t count = std::min(maxPointsPerPacket, points.size() - start);
        uint8_t packet[7 + maxPointsPerPacket * 4];
        packet[0] = 1; // Type: drawBatch
        packet[1] = color.r;
        packet[2] = color.g;
        packet[3] = color.b;
        packet[4] = size;
        packet[5] = shape;
        packet[6] = (uint8_t)count;

        for (size_t i = 0; i < count; i++)
        {
            *(uint16_t *)(packet + 7 + i * 4) = points[start + i].x;
            *(uint16_t *)(packet + 9 + i * 4) = points[start + i].y;
        }

        send(sock, packet, 7 + count * 4, 0);

        if (start + count >= points.size())
            break;
        start += count - 1; // Overlap one point so remote clients keep a continuous line.
    }
}

void handleHexColorInput()
{
    SwkbdState swkbd;
    char inputText[8];
    swkbdInit(&swkbd, SWKBD_TYPE_NORMAL, 2, -1);
    swkbdSetHintText(&swkbd, "Enter Hex Color (e.g., FF00FF)");

    if (swkbdInputText(&swkbd, inputText, sizeof(inputText)) == SWKBD_BUTTON_CONFIRM)
    {
        unsigned int hexValue;
        sscanf(inputText, "%x", &hexValue);

        currentColor.r = (hexValue >> 16) & 0xFF;
        currentColor.g = (hexValue >> 8) & 0xFF;
        currentColor.b = hexValue & 0xFF;
    }
}

// Function to decompress data
int main()
{
    gfxInitDefault();
    gfxSetDoubleBuffering(GFX_TOP, false);
    UIState::init();

    printf("3DS Collab Doodle\n");

    if (!NetworkManager::initialize())
    {
        failExit("Network connection failed or timed out.");
    }
    atexit(NetworkManager::shutdown);

    int sock = NetworkManager::getSocket();
    if (sock < 0)
    {
        failExit("No valid socket connection\n");
        return 1;
    }

    std::vector<int> brushSizes = {1, 2, 3, 5, 7}; // Define your brush sizes
    initializeGaussianFalloff(brushSizes);

    u16 fbWidth, fbHeight;
    u8 *fb = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, &fbWidth, &fbHeight);
    if (!fb)
    {
        printf("Failed to get framebuffer\n");
        gfxExit();
        return 1;
    }    
    printf("Controls:\n"
           "- Touch the bottom screen to draw\n"
           "- START: Refresh canvas from server\n"
           "- SELECT: Exit\n"
           "- Hold LEFT/A + stylus: Pan viewport\n"
           "- DOWN/B: Toggle Color Picker\n"
           "- Hold UP + tap: Sample color\n"
           "- X: Input Hex color code\n"
           "- Y: Check for updates\n"
           "- L: Switch channel\n");

    size_t bufferSize = fbWidth * fbHeight * 3;
    u8 *buffer = (u8 *)malloc(bufferSize);
    if (!buffer)
    {
        printf("Failed to allocate buffer\n");
        gfxExit();
        return 1;
    }
    memset(buffer, 255, bufferSize);

    int canvasWidth = 0;
    int canvasHeight = 0;
    u8 *fullCanvas = NULL;
    CanvasState canvas;
    bool updateAvailable = false;
    int topRenderFrame = 10;
    TopScreenMode topMode = TOP_MODE_CANVAS;
    int selectedChannel = 0;
    char availableChannels[8][25];
    int availableChannelCount = 0;

    if (sock >= 0)
    {
        char line[1024];
        CanvasMeta meta;
        bool receivedMeta = false;
        while (NetworkManager::readLine(sock, line, sizeof(line)))
        {
            char currentChannel[25] = "";
            if (Protocol::parseChannels(line, availableChannels, 8, availableChannelCount, currentChannel))
            {
                if (currentChannel[0])
                    canvas.setChannel(currentChannel);
                continue;
            }

            if (Protocol::parseCanvasMeta(line, meta))
            {
                receivedMeta = true;
                break;
            }
        }

        if (!receivedMeta)
        {
            printf("Failed to read init canvas metadata.\n");
        }
        else
        {
            canvasWidth = meta.width;
            canvasHeight = meta.height;
            canvas.setChannel(meta.channel);
            printf("Received canvas dimensions: W=%d, H=%d, Compressed Size=%d\n", canvasWidth, canvasHeight, meta.compressedSize);

            u8 *compressedCanvas = (u8 *)malloc(meta.compressedSize);
            if (compressedCanvas && NetworkManager::readExact(sock, compressedCanvas, meta.compressedSize))
            {
                if (canvas.allocate(canvasWidth, canvasHeight) && canvas.loadFromCompressed(compressedCanvas, meta.compressedSize))
                {
                    fullCanvas = canvas.pixels;
                    Renderer::invalidateMinimap();
                    printf("Canvas decompressed successfully.\n");
                }
                else
                {
                    printf("Failed to decompress canvas data.\n");
                }
                free(compressedCanvas);
            }
            else
            {
                printf("Failed to read compressed canvas data.\n");
            }
        }

        int flags = fcntl(sock, F_GETFL, 0);
        fcntl(sock, F_SETFL, flags | O_NONBLOCK);
    }

    int offsetX = 0;
    int offsetY = 0;
    int prevTouchX = -1;
    int prevTouchY = -1;
    int prevPrevTouchX = -1;
    int prevPrevTouchY = -1;
    float lastStrokeX = 0.0f;
    float lastStrokeY = 0.0f;
    bool hasLastStrokePoint = false;

    auto clampOffsets = [&](int &ox, int &oy)
    {
        ox = std::max(-20, std::min(ox, canvasWidth - fbWidth));
        oy = std::max(-20, std::min(oy, canvasHeight - fbHeight + 100));
    };

    auto syncSelectedChannel = [&]()
    {
        if (availableChannelCount <= 0)
        {
            selectedChannel = 0;
            return;
        }

        selectedChannel = std::max(0, std::min(selectedChannel, availableChannelCount - 1));
        for (int i = 0; i < availableChannelCount; i++)
        {
            if (strcmp(canvas.channel, availableChannels[i]) == 0)
            {
                selectedChannel = i;
                return;
            }
        }
    };

    auto switchToSelectedChannel = [&]() -> bool
    {
        if (availableChannelCount <= 0)
            return false;

        selectedChannel = std::max(0, std::min(selectedChannel, availableChannelCount - 1));
        if (strcmp(canvas.channel, availableChannels[selectedChannel]) == 0)
            return true;

        if (!NetworkManager::ensureConnected())
        {
            printf("Cannot switch channel - connection failed.\n");
            return false;
        }

        char command[96];
        Protocol::buildSwitchChannel(command, sizeof(command), availableChannels[selectedChannel]);
        if (send(NetworkManager::getSocket(), command, strlen(command), 0) <= 0)
        {
            printf("Failed to request channel switch.\n");
            return false;
        }

        printf("Switching to channel %s...\n", availableChannels[selectedChannel]);
        char response[1024];
        CanvasMeta meta;
        if (!NetworkManager::readLine(NetworkManager::getSocket(), response, sizeof(response)) ||
            !Protocol::parseCanvasMeta(response, meta))
        {
            printf("Channel switch response was invalid.\n");
            return false;
        }

        u8 *compressedCanvas = (u8 *)malloc(meta.compressedSize);
        if (!compressedCanvas)
        {
            printf("Failed to allocate channel canvas.\n");
            return false;
        }

        bool switched = false;
        if (NetworkManager::readExact(NetworkManager::getSocket(), compressedCanvas, meta.compressedSize))
        {
            canvasWidth = meta.width;
            canvasHeight = meta.height;
            canvas.setChannel(meta.channel);
            if (canvas.allocate(canvasWidth, canvasHeight) && canvas.loadFromCompressed(compressedCanvas, meta.compressedSize))
            {
                fullCanvas = canvas.pixels;
                offsetX = offsetY = 0;
                clampOffsets(offsetX, offsetY);
                Renderer::invalidateMinimap();
                printf("Switched to %s.\n", canvas.channel);
                switched = true;
            }
        }

        free(compressedCanvas);
        syncSelectedChannel();
        return switched;
    };

    syncSelectedChannel();

    while (aptMainLoop())
    {
        hidScanInput();
        u32 kDown = hidKeysDown();
        u32 kHeld = hidKeysHeld();

        if (kDown & KEY_SELECT)
        {
            printf("Select key pressed. Exiting...\n");
            
            // Clear any pending points
            UIState::clearPoints();
            
            // Ensure proper disconnection
            NetworkManager::disconnect();
            
            // Wait briefly to ensure disconnection completes
            for (int i = 0; i < 30; i++) {
                gspWaitForVBlank();
            }
            
            break;
        }

        if (kDown & KEY_START)
        {
            printf("Refreshing canvas from server...\n");
            if (!NetworkManager::ensureConnected()) {
                printf("Cannot refresh canvas - connection failed!\n");
                continue;
            }

            char request[] = "getCanvas\n";
            if (send(NetworkManager::getSocket(), request, strlen(request), 0) <= 0) {
                printf("Failed to send refresh request. Attempting to reconnect...\n");
                if (!NetworkManager::reconnect()) {
                    printf("Reconnection failed. Please try again later.\n");
                    continue;
                }
                // Retry sending after reconnection
                if (send(NetworkManager::getSocket(), request, strlen(request), 0) <= 0) {
                    printf("Failed to send refresh request even after reconnection.\n");
                    continue;
                }
            }

            char response[1024];
            if (!NetworkManager::readLine(NetworkManager::getSocket(), response, sizeof(response))) {
                printf("Failed to read server response.\n");
                continue;
            }

            CanvasMeta refreshMeta;
            if (!Protocol::parseCanvasMeta(response, refreshMeta)) {
                printf("Invalid response format from server: %s\n", response);
                continue;
            }

            int compressedSize = refreshMeta.compressedSize;
            if (compressedSize <= 0 || compressedSize > 10000000) { // Sanity check for size
                printf("Invalid compressed size: %d\n", compressedSize);
                continue;
            }

            u8 *compressedCanvas = (u8 *)malloc(compressedSize);
            if (!compressedCanvas) {
                printf("Failed to allocate memory for compressed canvas.\n");
                continue;
            }

            bool success = false;
            if (NetworkManager::readExact(NetworkManager::getSocket(), compressedCanvas, compressedSize)) {
                if (canvas.loadFromCompressed(compressedCanvas, compressedSize)) {
                    fullCanvas = canvas.pixels;
                    Renderer::invalidateMinimap();
                    printf("Canvas refreshed successfully!\n");
                    success = true;
                } else {
                    printf("Failed to decompress canvas data.\n");
                }
            } else {
                printf("Failed to read compressed canvas data.\n");
            }

            free(compressedCanvas);
            
            if (!success) {
                printf("Canvas refresh failed. Try again?\n");
            }
        }

        if (kDown & KEY_Y)
        {
            printf("Checking for updates...\n");
            updateAvailable = Updater::checkForUpdate("192.168.1.46", "3000", APP_VERSION);
            printf(updateAvailable ? "Update available.\n" : "No update available or check failed.\n");
            topMode = TOP_MODE_CANVAS;
            topRenderFrame = 10;
        }

        if (kDown & KEY_R)
        {
            topMode = (topMode == TOP_MODE_CONTROLS) ? TOP_MODE_CANVAS : TOP_MODE_CONTROLS;
            topRenderFrame = 10;
        }

        if (kDown & KEY_L)
        {
            if (topMode == TOP_MODE_CHANNELS)
            {
                topMode = TOP_MODE_CANVAS;
            }
            else
            {
                syncSelectedChannel();
                topMode = TOP_MODE_CHANNELS;
            }
            topRenderFrame = 10;
        }

        if (topMode == TOP_MODE_CHANNELS)
        {
            if ((kDown & KEY_DUP) && availableChannelCount > 0)
            {
                selectedChannel = (selectedChannel + availableChannelCount - 1) % availableChannelCount;
                topRenderFrame = 10;
            }
            if ((kDown & KEY_DDOWN) && availableChannelCount > 0)
            {
                selectedChannel = (selectedChannel + 1) % availableChannelCount;
                topRenderFrame = 10;
            }
            if (kDown & KEY_A)
            {
                if (switchToSelectedChannel())
                    topMode = TOP_MODE_CANVAS;
                topRenderFrame = 10;
            }
            if (kDown & KEY_B)
            {
                topMode = TOP_MODE_CANVAS;
                topRenderFrame = 10;
                continue;
            }
        }

        if (topMode != TOP_MODE_CHANNELS && (kDown & (KEY_DDOWN | KEY_B)))
        {
            UIState::toggleColorPicker();
            topMode = TOP_MODE_CANVAS;
            printf(UIState::isColorPickerActive() ? "Color picker activated\n" : "Color picker deactivated\n");
        }

        if (kDown & KEY_X)
        {
            handleHexColorInput();
        }

        touchPosition touch;
        hidTouchRead(&touch);

        // Eye Dropper functionality
        if (topMode == TOP_MODE_CANVAS && ((kHeld & KEY_DUP) || (kHeld & KEY_X)))
        {
            if (kDown & KEY_TOUCH)
            {
                int touchX = touch.px + offsetX;
                int touchY = touch.py + offsetY;

                if (touchX >= 0 && touchX < canvasWidth && touchY >= 0 && touchY < canvasHeight)
                {
                    int idx = 3 * (touchY * canvasWidth + touchX);
                    currentColor.r = fullCanvas[idx];
                    currentColor.g = fullCanvas[idx + 1];
                    currentColor.b = fullCanvas[idx + 2];

                    printf("Color picked: R=%d, G=%d, B=%d\n", currentColor.r, currentColor.g, currentColor.b);
                }
            }
        }

        if (topMode == TOP_MODE_CANVAS && UIState::isColorPickerActive() && (kHeld & KEY_TOUCH))
        {
            float h, s, v;
            UIState::getHSV(h, s, v);

            if (touch.py >= 140 && touch.py < 160)
            {
                h = (float)(touch.px - 20) / 280;
            }
            else if (touch.py >= 170 && touch.py < 190)
            {
                s = (float)(touch.px - 20) / 280;
            }
            else if (touch.py >= 200 && touch.py < 220)
            {
                v = (float)(touch.px - 20) / 280;
            }

            h = std::max(0.0f, std::min(1.0f, h));
            s = std::max(0.0f, std::min(1.0f, s));
            v = std::max(0.0f, std::min(1.0f, v));

            UIState::updateHSV(h, s, v);

            // Convert HSV to RGB
            float r, g, b;
            UIState::HSVtoRGB(h, s, v, r, g, b);
            currentColor.r = r * 255;
            currentColor.g = g * 255;
            currentColor.b = b * 255;

            // Check if touch is on brush size selectors
            std::vector<int> brushSizes = {1, 2, 3, 5, 7};
            std::vector<int> brushPositions = {30, 60, 90, 120, 150};
            for (size_t i = 0; i < brushSizes.size(); i++)
            {
                int x = brushPositions[i];
                if (touch.px >= x - 30 && touch.px <= x + 10)
                {
                    if (touch.py >= 20 && touch.py <= 40)
                    {
                        currentBrushSize = brushSizes[i];
                        currentBrushShape = 0; // Circle
                        break;
                    }
                    else if (touch.py >= 50 && touch.py <= 70)
                    {
                        currentBrushSize = brushSizes[i];
                        currentBrushShape = 1; // Square
                        break;
                    }
                    else if (touch.py >= 80 && touch.py <= 100)
                    {
                        currentBrushSize = brushSizes[i];
                        currentBrushShape = 2; // Antialiased Circle
                        break;
                    }
                }
            }
        }

        if (topMode == TOP_MODE_CANVAS && (kHeld & (KEY_DLEFT | KEY_A)))
        {
            // Panning mode
            if (kHeld & KEY_TOUCH)
            {
                if (prevTouchX == -1 && prevTouchY == -1)
                {
                    prevTouchX = touch.px;
                    prevTouchY = touch.py;
                    prevPrevTouchX = prevPrevTouchY = -1;
                    hasLastStrokePoint = false;
                }
                else
                {
                    int deltaX = touch.px - prevTouchX;
                    int deltaY = touch.py - prevTouchY;

                    offsetX -= deltaX;
                    offsetY -= deltaY;

                    clampOffsets(offsetX, offsetY);
                    canvas.offsetX = offsetX;
                    canvas.offsetY = offsetY;
                    canvas.markFullDirty();

                    prevTouchX = touch.px;
                    prevTouchY = touch.py;
                }
            }
            else
            {
                prevTouchX = prevTouchY = -1;
                prevPrevTouchX = prevPrevTouchY = -1;
                hasLastStrokePoint = false;
            }
        }
        else if (topMode == TOP_MODE_CANVAS && !UIState::isColorPickerActive())
        {
            // Normal drawing mode
            if (kHeld & KEY_TOUCH)
            {
                if (prevTouchX == -1 && prevTouchY == -1)
                {
                    prevTouchX = touch.px;
                    prevTouchY = touch.py;
                    prevPrevTouchX = prevPrevTouchY = -1;
                    lastStrokeX = (float)touch.px;
                    lastStrokeY = (float)touch.py;
                    hasLastStrokePoint = true;
                    drawStrokeSample(fullCanvas, canvasWidth, canvasHeight, touch.px, touch.py, offsetX, offsetY, canvas);
                }
                else
                {
                    if (prevPrevTouchX == -1 || prevPrevTouchY == -1 || !hasLastStrokePoint)
                    {
                        drawStrokeLine(fullCanvas, canvasWidth, canvasHeight,
                                       (float)prevTouchX, (float)prevTouchY,
                                       (float)touch.px, (float)touch.py,
                                       offsetX, offsetY, canvas);
                        lastStrokeX = (float)touch.px;
                        lastStrokeY = (float)touch.py;
                    }
                    else
                    {
                        float endX = ((float)prevTouchX + (float)touch.px) * 0.5f;
                        float endY = ((float)prevTouchY + (float)touch.py) * 0.5f;
                        drawStrokeCurve(fullCanvas, canvasWidth, canvasHeight,
                                        lastStrokeX, lastStrokeY,
                                        (float)prevTouchX, (float)prevTouchY,
                                        endX, endY,
                                        offsetX, offsetY, canvas);
                        lastStrokeX = endX;
                        lastStrokeY = endY;
                    }

                    if (UIState::getPoints().size() >= 32)
                    {
                        if (!NetworkManager::checkConnection()) {
                            printf("Connection lost while drawing! Attempting to reconnect...\n");
                            if (!NetworkManager::reconnect()) {
                                UIState::clearPoints(); // Clear points if reconnect failed
                                continue;
                            }
                        }
                        sendDrawBatchCommand(sock, UIState::getPoints(), currentColor,
                                             currentBrushSize, currentBrushShape);
                        UIState::clearPoints();
                    }

                    prevPrevTouchX = prevTouchX;
                    prevPrevTouchY = prevTouchY;
                    prevTouchX = touch.px;
                    prevTouchY = touch.py;
                }
            }
            else
            {
                // Stylus released
                if (hasLastStrokePoint && prevTouchX != -1 && prevTouchY != -1)
                {
                    drawStrokeLine(fullCanvas, canvasWidth, canvasHeight,
                                   lastStrokeX, lastStrokeY,
                                   (float)prevTouchX, (float)prevTouchY,
                                   offsetX, offsetY, canvas);
                }

                if (!UIState::getPoints().empty())
                {
                    if (!NetworkManager::checkConnection()) {
                        printf("Connection lost while drawing! Attempting to reconnect...\n");
                        if (!NetworkManager::reconnect()) {
                            UIState::clearPoints(); // Clear points if reconnect failed
                            continue;
                        }
                    }
                    sendDrawBatchCommand(sock, UIState::getPoints(), currentColor,
                                         currentBrushSize, currentBrushShape);
                    UIState::clearPoints();
                }
                prevTouchX = prevTouchY = -1;
                prevPrevTouchX = prevPrevTouchY = -1;
                hasLastStrokePoint = false;
            }
        }

        // Server messages
        if (sock >= 0)
        {
            uint8_t packetBuffer[1024];
            int recvLen = recv(sock, packetBuffer, sizeof(packetBuffer), 0);
            if (recvLen > 0)
            {
                if (packetBuffer[0] == '{')
                {
                    packetBuffer[std::min(recvLen, (int)sizeof(packetBuffer) - 1)] = '\0';
                    CanvasMeta meta;
                    char currentChannel[25] = "";
                    if (Protocol::parseChannels((char *)packetBuffer, availableChannels, 8, availableChannelCount, currentChannel))
                    {
                        if (currentChannel[0])
                            canvas.setChannel(currentChannel);
                        syncSelectedChannel();
                        topRenderFrame = 10;
                    }
                    else if (strstr((char *)packetBuffer, "channelChanged"))
                    {
                        char changed[25] = "";
                        int ignoredCount = 0;
                        Protocol::parseChannels((char *)packetBuffer, availableChannels, 8, ignoredCount, changed);
                    }
                    else if (Protocol::parseCanvasMeta((char *)packetBuffer, meta))
                    {
                        canvasWidth = meta.width;
                        canvasHeight = meta.height;
                        canvas.setChannel(meta.channel);
                        if (canvas.allocate(canvasWidth, canvasHeight))
                        {
                            u8 *compressedCanvas = (u8 *)malloc(meta.compressedSize);
                            if (compressedCanvas && NetworkManager::readExact(sock, compressedCanvas, meta.compressedSize))
                            {
                                if (canvas.loadFromCompressed(compressedCanvas, meta.compressedSize))
                                {
                                    fullCanvas = canvas.pixels;
                                    Renderer::invalidateMinimap();
                                }
                            }
                            if (compressedCanvas)
                                free(compressedCanvas);
                        }
                    }
                }
                else
                {
                    processDrawPacket(packetBuffer, recvLen, buffer, fbWidth, fbHeight, fullCanvas, canvasWidth, canvasHeight);
                    canvas.markFullDirty();
                    Renderer::invalidateMinimap();
                }
            }
            else if (recvLen == 0)
            {
                close(sock);
                sock = -1;
                printf("Server disconnected.\n");
            }
        }

        // Rendering
        canvas.offsetX = offsetX;
        canvas.offsetY = offsetY;
        bool canvasWasDirty = canvas.dirty.valid;
        Renderer::renderViewport(canvas, buffer, fbWidth, fbHeight, false);
        canvas.clearDirty();

        bool activelyDrawing = !UIState::isColorPickerActive() &&
                               topMode == TOP_MODE_CANVAS &&
                               !(kHeld & (KEY_DLEFT | KEY_A)) &&
                               (kHeld & KEY_TOUCH);
        topRenderFrame++;
        if (!activelyDrawing && (topRenderFrame >= 10 || canvasWasDirty))
        {
            Renderer::renderTop(canvas, NetworkManager::isSocketConnected(), updateAvailable, currentColor,
                                currentBrushSize, currentBrushShape, topMode,
                                availableChannels, availableChannelCount, selectedChannel);
            topRenderFrame = 0;
        }

        gspWaitForVBlank();
        Renderer::presentTopFrame();
        fb = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL);
        memcpy(fb, buffer, bufferSize);
        if (UIState::isColorPickerActive())
        {
            UIInterface::drawUIBackground(fb, fbWidth, fbHeight);
            float h, s, v;
            UIState::getHSV(h, s, v);
            UIInterface::drawHSVSliders(fb, fbWidth, fbHeight, h, s, v);
            drawBrushSizeSelector(fb, fbWidth, fbHeight);
        }

        UIInterface::drawCurrentSelection(fb, fbWidth, fbHeight, currentColor);

        gfxFlushBuffers();
        gfxSwapBuffers();
    }

    if (sock >= 0)
        close(sock);
    free(buffer);
    gfxExit();
    return 0;
}
