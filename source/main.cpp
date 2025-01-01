// This comment is so that you can see I changed a file in github desktop
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
#include <zlib.h>
#include <unordered_map>
#include "ui.h"
#include "network.h"

Color currentColor = {255, 0, 0}; // Red by default
int currentBrushSize = 1;
int currentBrushShape = 0;

static void failExit(const char *fmt, ...)
{
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

    uint8_t packet[7 + points.size() * 4];
    packet[0] = 1; // Type: drawBatch
    packet[1] = color.r;
    packet[2] = color.g;
    packet[3] = color.b;
    packet[4] = size;
    packet[5] = shape;
    packet[6] = points.size() > 255 ? 255 : points.size();

    for (size_t i = 0; i < packet[6]; i++)
    {
        *(uint16_t *)(packet + 7 + i * 4) = points[i].x;
        *(uint16_t *)(packet + 9 + i * 4) = points[i].y;
    }

    send(sock, packet, 7 + packet[6] * 4, 0);
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
bool decompressData(const u8 *compressedData, size_t compressedSize, u8 *decompressedData, size_t decompressedSize)
{
    z_stream strm;
    strm.zalloc = Z_NULL;
    strm.zfree = Z_NULL;
    strm.opaque = Z_NULL;
    strm.avail_in = 0;
    strm.next_in = Z_NULL;

    if (inflateInit(&strm) != Z_OK)
    {
        return false;
    }

    strm.avail_in = compressedSize;
    strm.next_in = (Bytef *)compressedData;
    strm.avail_out = decompressedSize;
    strm.next_out = decompressedData;

    int ret = inflate(&strm, Z_FINISH);
    inflateEnd(&strm);

    return ret == Z_STREAM_END;
}

int main()
{
    gfxInitDefault();
    consoleInit(GFX_TOP, NULL);
    UIState::init();

    printf("3DS Collab Doodle\n");

    if (!NetworkManager::initialize())
    {
        // Handle error
        return 1;
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
           "- X: Input Hex color code\n");

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
    int canvasSize = 0;
    u8 *fullCanvas = NULL;

    if (sock >= 0)
    {
        char line[1024];
        if (!NetworkManager::readLine(sock, line, sizeof(line)))
        {
            printf("Failed to read init line.\n");
        }
        else
        {
            char *wPtr = strstr(line, "\"width\":");
            char *hPtr = strstr(line, "\"height\":");
            char *sPtr = strstr(line, "\"compressedSize\":");
            if (wPtr && hPtr && sPtr)
            {
                canvasWidth = atoi(wPtr + 8);
                canvasHeight = atoi(hPtr + 9);
                int compressedSize = atoi(sPtr + 17);
                printf("Received canvas dimensions: W=%d, H=%d, Compressed Size=%d\n", canvasWidth, canvasHeight, compressedSize);

                u8 *compressedCanvas = (u8 *)malloc(compressedSize);
                if (compressedCanvas && NetworkManager::readExact(sock, compressedCanvas, compressedSize))
                {
                    canvasSize = canvasWidth * canvasHeight * 3;
                    fullCanvas = (u8 *)malloc(canvasSize);
                    if (fullCanvas && decompressData(compressedCanvas, compressedSize, fullCanvas, canvasSize))
                    {
                        printf("Canvas decompressed successfully.\n");
                    }
                    else
                    {
                        printf("Failed to decompress canvas data.\n");
                        if (fullCanvas)
                        {
                            free(fullCanvas);
                            fullCanvas = NULL;
                        }
                    }
                    free(compressedCanvas);
                }
                else
                {
                    printf("Failed to read compressed canvas data.\n");
                }
            }
            else
            {
                printf("Invalid init line: %s\n", line);
            }
        }

        int flags = fcntl(sock, F_GETFL, 0);
        fcntl(sock, F_SETFL, flags | O_NONBLOCK);
    }

    int offsetX = 0;
    int offsetY = 0;
    int prevTouchX = -1;
    int prevTouchY = -1;

    auto clampOffsets = [&](int &ox, int &oy)
    {
        ox = std::max(-20, std::min(ox, canvasWidth - fbWidth));
        oy = std::max(-20, std::min(oy, canvasHeight - fbHeight + 100));
    };

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

            if (!strstr(response, "compressedCanvas")) {
                printf("Invalid response format from server: %s\n", response);
                continue;
            }

            char *sPtr = strstr(response, "\"compressedSize\":");
            if (!sPtr) {
                printf("Missing compressed size in server response.\n");
                continue;
            }

            int compressedSize = atoi(sPtr + 17);
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
                if (decompressData(compressedCanvas, compressedSize, fullCanvas, canvasSize)) {
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

        if (kDown & (KEY_DDOWN | KEY_B))
        {
            UIState::toggleColorPicker();
            printf(UIState::isColorPickerActive() ? "Color picker activated\n" : "Color picker deactivated\n");
        }

        if (kDown & KEY_X)
        {
            handleHexColorInput();
        }

        touchPosition touch;
        hidTouchRead(&touch);

        // Eye Dropper functionality
        if ((kHeld & KEY_DUP) || (kHeld & KEY_X))
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

        if (UIState::isColorPickerActive() && (kHeld & KEY_TOUCH))
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

        if (kHeld & (KEY_DLEFT | KEY_A))
        {
            // Panning mode
            if (kHeld & KEY_TOUCH)
            {
                if (prevTouchX == -1 && prevTouchY == -1)
                {
                    prevTouchX = touch.px;
                    prevTouchY = touch.py;
                }
                else
                {
                    int deltaX = touch.px - prevTouchX;
                    int deltaY = touch.py - prevTouchY;

                    offsetX -= deltaX;
                    offsetY -= deltaY;

                    clampOffsets(offsetX, offsetY);

                    prevTouchX = touch.px;
                    prevTouchY = touch.py;
                }
            }
            else
            {
                prevTouchX = prevTouchY = -1;
            }
        }
        else if (!UIState::isColorPickerActive())
        {
            // Normal drawing mode
            if (kHeld & KEY_TOUCH)
            {
                if (prevTouchX == -1 && prevTouchY == -1)
                {
                    prevTouchX = touch.px;
                    prevTouchY = touch.py;
                }
                else
                {
                    int steps = std::max(abs(touch.px - prevTouchX), abs(touch.py - prevTouchY));
                    for (int i = 0; i <= steps; i++)
                    {
                        float t = (steps == 0) ? 0.0f : static_cast<float>(i) / steps;
                        int x = prevTouchX + (touch.px - prevTouchX) * t;
                        int y = prevTouchY + (touch.py - prevTouchY) * t;

                        // Use currentColor here
                        drawBrush(buffer, fbWidth, fbHeight, x, y, currentBrushSize, currentBrushShape, currentColor.r, currentColor.g, currentColor.b);

                        int C_x = x + offsetX;
                        int C_y = y + offsetY;
                        if (C_x >= 0 && C_x < canvasWidth && C_y >= 0 && C_y < canvasHeight)
                        {
                            drawBrush(fullCanvas, canvasWidth, canvasHeight, C_x, C_y,
                                      currentBrushSize, currentBrushShape,
                                      currentColor.r, currentColor.g, currentColor.b);
                        }

                        UIState::addPoint(C_x, C_y);
                    }

                    if (UIState::getPoints().size() >= 10)
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

                    prevTouchX = touch.px;
                    prevTouchY = touch.py;
                }
            }
            else
            {
                // Stylus released
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
            }
        }

        // Server messages
        if (sock >= 0)
        {
            uint8_t packetBuffer[1024];
            int recvLen = recv(sock, packetBuffer, sizeof(packetBuffer), 0);
            if (recvLen > 0)
            {
                processDrawPacket(packetBuffer, recvLen, buffer, fbWidth, fbHeight, fullCanvas, canvasWidth, canvasHeight);
            }
            else if (recvLen == 0)
            {
                close(sock);
                sock = -1;
                printf("Server disconnected.\n");
            }
        }

        // Rendering
        if (fullCanvas)
        {
            for (int y = 0; y < fbHeight; y++)
            {
                for (int x = 0; x < fbWidth; x++)
                {
                    int C_x = y + offsetX;
                    int C_y = (fbWidth - 1 - x) + offsetY;
                    if (C_x >= 0 && C_x < canvasWidth && C_y >= 0 && C_y < canvasHeight)
                    {
                        int bufferIdx = 3 * (y * fbWidth + x);
                        int canvasIdx = 3 * (C_y * canvasWidth + C_x);
                        buffer[bufferIdx] = fullCanvas[canvasIdx + 2];     // B
                        buffer[bufferIdx + 1] = fullCanvas[canvasIdx + 1]; // G
                        buffer[bufferIdx + 2] = fullCanvas[canvasIdx];     // R
                    }
                    else
                    {
                        // Draw a light gray color for areas outside the canvas
                        int bufferIdx = 3 * (y * fbWidth + x);
                        buffer[bufferIdx] = buffer[bufferIdx + 1] = buffer[bufferIdx + 2] = 240;
                    }
                }
            }
        }

        gspWaitForVBlank();
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
    if (fullCanvas)
        free(fullCanvas);
    free(buffer);
    gfxExit();
    return 0;
}
