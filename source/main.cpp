#include <3ds.h>
#include <stdio.h>
#include <string.h>
#include <malloc.h>
#include <cstdlib>
#include <stdarg.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <vector>
#include <string>
#include <sstream>
#include <math.h>
#include <algorithm>
#include <zlib.h>
#include <unordered_map>
#include <vector>

#define SOC_ALIGN 0x1000
#define SOC_BUFFERSIZE 0x100000

static u32 *SOC_buffer = NULL;
static int sock = -1;

struct DrawPoint
{
    int x, y;
};
struct Color
{
    u8 r, g, b;
} currentColor = {255, 0, 0}; // Red by default

int currentBrushSize = 1;  // Default brush size
int currentBrushShape = 0; // 0 for circle, 1 for square, 2 for antialiased circle

const int UI_MARGIN_X = 20;
const int UI_MARGIN_Y = 10;
const int UI_BG_COLOR_R = 200;
const int UI_BG_COLOR_G = 200;
const int UI_BG_COLOR_B = 200;

// UI State
bool colorPickerActive = false;
std::vector<DrawPoint> pointBuffer;

// HSV values
float hue = 0.0f, saturation = 1.0f, value = 1.0f;

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

static void socShutdown()
{
    printf("Shutting down network...\n");
    socExit();
}

static bool read_line(int s, char *buffer, size_t maxlen)
{
    int flags = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, flags & ~O_NONBLOCK);

    size_t pos = 0;
    char c;
    while (pos < maxlen - 1)
    {
        int ret = recv(s, &c, 1, 0);
        if (ret <= 0)
        {
            fcntl(s, F_SETFL, flags);
            return false;
        }
        if (c == '\n')
        {
            buffer[pos] = '\0';
            fcntl(s, F_SETFL, flags);
            return true;
        }
        buffer[pos++] = c;
    }

    fcntl(s, F_SETFL, flags);
    buffer[pos] = '\0';
    return true;
}

static bool read_exact(int s, void *buf, size_t length)
{
    int flags = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, flags & ~O_NONBLOCK);

    size_t received = 0;
    char *ptr = (char *)buf;
    while (received < length)
    {
        int ret = recv(s, ptr + received, length - received, 0);
        if (ret <= 0)
        {
            fcntl(s, F_SETFL, flags);
            return false;
        }
        received += ret;
    }

    fcntl(s, F_SETFL, flags);
    return true;
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

void HSVtoRGB(float h, float s, float v, float &r, float &g, float &b)
{
    int i = int(h * 6);
    float f = h * 6 - i;
    float p = v * (1 - s);
    float q = v * (1 - f * s);
    float t = v * (1 - (1 - f) * s);

    switch (i % 6)
    {
    case 0:
        r = v, g = t, b = p;
        break;
    case 1:
        r = q, g = v, b = p;
        break;
    case 2:
        r = p, g = v, b = t;
        break;
    case 3:
        r = p, g = q, b = v;
        break;
    case 4:
        r = t, g = p, b = v;
        break;
    case 5:
        r = v, g = p, b = q;
        break;
    }
}

void drawHSVSliders(u8 *framebuffer, int screenWidth, int screenHeight, float &hue, float &saturation, float &value)
{
    int sliderHeight = 280;
    int sliderWidth = 20;
    int startX[] = {screenWidth - 140, screenWidth - 170, screenWidth - 200};

    for (int i = 0; i < 3; i++)
    {
        for (int y = 20; y < 20 + sliderHeight; y++)
        {
            float h = (i == 0) ? (float)(y - 20) / sliderHeight : hue;
            float s = (i == 1) ? (float)(y - 20) / sliderHeight : saturation;
            float v = (i == 2) ? (float)(y - 20) / sliderHeight : value;
            float r, g, b;
            HSVtoRGB(h, s, v, r, g, b);
            for (int x = startX[i] - sliderWidth; x < startX[i]; x++)
            {
                int idx = 3 * (x + y * screenWidth);
                framebuffer[idx] = b * 255;
                framebuffer[idx + 1] = g * 255;
                framebuffer[idx + 2] = r * 255;
            }
        }
    }

    // Draw slider indicators
    for (int i = 0; i < 3; i++)
    {
        float indicatorValue = (i == 0) ? hue : (i == 1) ? saturation
                                                         : value;
        int indicatorY = 20 + (indicatorValue * sliderHeight);
        for (int x = startX[i] - sliderWidth - 5; x < startX[i] + 5; x++)
        {
            int idx = 3 * (x + indicatorY * screenWidth);
            framebuffer[idx] = framebuffer[idx + 1] = framebuffer[idx + 2] = 0; // Black indicator
        }
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

void drawUIBackground(u8 *framebuffer, int screenWidth, int screenHeight)
{
    for (int x = screenWidth - 230; x < screenWidth - 20; x++)
    {
        for (int y = 10; y < 310; y++)
        {
            int idx = 3 * (x + y * screenWidth);
            framebuffer[idx] = UI_BG_COLOR_B;
            framebuffer[idx + 1] = UI_BG_COLOR_G;
            framebuffer[idx + 2] = UI_BG_COLOR_R;
        }
    }
}

void drawCurrentSelection(u8 *framebuffer, int screenWidth, int screenHeight, Color color, bool colorPickerActive)
{
    if (colorPickerActive)
    {
        // Draw color rectangle
        int rectSize = 100;
        int rectX = screenWidth - 30 - rectSize;
        int rectY = 200;

        for (int x = rectX; x < rectX + rectSize; x++)
        {
            for (int y = rectY; y < rectY + rectSize; y++)
            {
                if (x >= 0 && x < screenWidth && y >= 0 && y < screenHeight)
                {
                    int idx = 3 * (x + y * screenWidth);
                    framebuffer[idx] = color.b;
                    framebuffer[idx + 1] = color.g;
                    framebuffer[idx + 2] = color.r;
                }
            }
        }
    }
    else
    {
        // Draw small color rectangle when color picker is not active
        int rectSize = 10;
        int rectX = 10;
        int rectY = 300;

        for (int x = rectX; x < rectX + rectSize; x++)
        {
            for (int y = rectY; y < rectY + rectSize; y++)
            {
                if (x >= 0 && x < screenWidth && y >= 0 && y < screenHeight)
                {
                    int idx = 3 * (x + y * screenWidth);
                    framebuffer[idx] = color.b;
                    framebuffer[idx + 1] = color.g;
                    framebuffer[idx + 2] = color.r;
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

    printf("3DS MultiUser Doodle App \n");

    SOC_buffer = (u32 *)memalign(SOC_ALIGN, SOC_BUFFERSIZE);
    if (SOC_buffer == NULL)
    {
        failExit("memalign: failed to allocate\n");
    }

    if (socInit(SOC_buffer, SOC_BUFFERSIZE) != 0)
    {
        failExit("socInit failed\n");
    }
    atexit(socShutdown);

    const char *server_ip = "38.45.65.90";
    int server_port = 3030;

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
    {
        failExit("socket: %d %s\n", errno, strerror(errno));
    }

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(server_port);
    inet_pton(AF_INET, server_ip, &sa.sin_addr);

    printf("Connecting to %s:%d...\n", server_ip, server_port);
    if (connect(sock, (struct sockaddr *)&sa, sizeof(sa)) < 0)
    {
        printf("Failed to connect: %d %s\n", errno, strerror(errno));
        close(sock);
        sock = -1;
    }
    else
    {
        printf("Connected!\n");
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

    printf("fbWidth=%u, fbHeight=%u\n", fbWidth, fbHeight);
    printf("Touch bottom screen to draw. \nSTART to refresh canvas and SELECT to exit.\nHold LEFT D-Pad or A button for pan.\nToggle the color picker with DOWN D-Pad or B button.\n Hold UP D-Pad or X button and tap to sample a color!\n");

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
        if (!read_line(sock, line, sizeof(line)))
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
                if (compressedCanvas && read_exact(sock, compressedCanvas, compressedSize))
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
            break;
        }

        if (kDown & KEY_START)
        {
            if (sock >= 0)
            {
                char request[] = "getCanvas\n";
                send(sock, request, strlen(request), 0);

                char response[1024];
                if (read_line(sock, response, sizeof(response)) && strstr(response, "compressedCanvas"))
                {
                    char *sPtr = strstr(response, "\"compressedSize\":");
                    if (sPtr)
                    {
                        int compressedSize = atoi(sPtr + 17);
                        u8 *compressedCanvas = (u8 *)malloc(compressedSize);
                        if (compressedCanvas && read_exact(sock, compressedCanvas, compressedSize))
                        {
                            if (decompressData(compressedCanvas, compressedSize, fullCanvas, canvasSize))
                            {
                                printf("Canvas refreshed from server.\n");
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
                    else
                    {
                        printf("Invalid response from server.\n");
                    }
                }
                else
                {
                    printf("Invalid response from server.\n");
                }
            }
        }

        if (kDown & (KEY_DDOWN | KEY_B))
        {
            colorPickerActive = !colorPickerActive;
            printf(colorPickerActive ? "Color picker activated\n" : "Color picker deactivated\n");
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

        if (colorPickerActive && (kHeld & KEY_TOUCH))
        {
            if (touch.py >= 140 && touch.py < 160)
            {
                // Hue slider
                hue = (float)(touch.px - 20) / 280;
            }
            else if (touch.py >= 170 && touch.py < 190)
            {
                // Saturation slider
                saturation = (float)(touch.px - 20) / 280;
            }
            else if (touch.py >= 200 && touch.py < 220)
            {
                // Value slider
                value = (float)(touch.px - 20) / 280;
            }

            // Clamp values
            hue = std::max(0.0f, std::min(1.0f, hue));
            saturation = std::max(0.0f, std::min(1.0f, saturation));
            value = std::max(0.0f, std::min(1.0f, value));

            // Convert HSV to RGB
            float r, g, b;
            HSVtoRGB(hue, saturation, value, r, g, b);
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
        else if (!colorPickerActive)
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
                            // Use currentColor here too
                            drawBrush(fullCanvas, canvasWidth, canvasHeight, C_x, C_y, currentBrushSize, currentBrushShape, currentColor.r, currentColor.g, currentColor.b);
                        }

                        pointBuffer.push_back({C_x, C_y});
                    }

                    if (pointBuffer.size() >= 10)
                    {
                        sendDrawBatchCommand(sock, pointBuffer, currentColor, currentBrushSize, currentBrushShape);
                        pointBuffer.clear();
                    }

                    prevTouchX = touch.px;
                    prevTouchY = touch.py;
                }
            }
            else
            {
                // Stylus released
                if (!pointBuffer.empty())
                {
                    sendDrawBatchCommand(sock, pointBuffer, currentColor, currentBrushSize, currentBrushShape);
                    pointBuffer.clear();
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

        if (colorPickerActive)
        {
            drawUIBackground(fb, fbWidth, fbHeight);
            drawHSVSliders(fb, fbWidth, fbHeight, hue, saturation, value);
            drawBrushSizeSelector(fb, fbWidth, fbHeight);
        }

        drawCurrentSelection(fb, fbWidth, fbHeight, currentColor, colorPickerActive);

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
