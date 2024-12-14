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

#define SOC_ALIGN       0x1000
#define SOC_BUFFERSIZE  0x100000

static u32 *SOC_buffer = NULL;
static int sock = -1;

struct DrawPoint {
    int x, y;
};

std::vector<DrawPoint> pointBuffer;

static void failExit(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\nPress B to exit.\n");
    while (aptMainLoop()) {
        gspWaitForVBlank();
        hidScanInput();
        if (hidKeysDown() & KEY_B) break;
    }
    gfxExit();
    exit(0);
}

static void socShutdown() {
    printf("Shutting down network...\n");
    socExit();
}

static bool read_line(int s, char* buffer, size_t maxlen) {
    int flags = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, flags & ~O_NONBLOCK);

    size_t pos = 0;
    char c;
    while (pos < maxlen - 1) {
        int ret = recv(s, &c, 1, 0);
        if (ret <= 0) {
            fcntl(s, F_SETFL, flags);
            return false;
        }
        if (c == '\n') {
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

static bool read_exact(int s, void* buf, size_t length) {
    int flags = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, flags & ~O_NONBLOCK);

    size_t received = 0;
    char* ptr = (char*)buf;
    while (received < length) {
        int ret = recv(s, ptr + received, length - received, 0);
        if (ret <= 0) {
            fcntl(s, F_SETFL, flags);
            return false;
        }
        received += ret;
    }

    fcntl(s, F_SETFL, flags);
    return true;
}

void drawPointOnBuffer(u8* buffer, int fbWidth, int fbHeight, int x, int y, u8 r, u8 g, u8 b) {
    if (x >= 0 && x < fbWidth && y >= 0 && y < fbHeight) {
        int idx = 3 * (y * fbWidth + x);
        buffer[idx] = r;
        buffer[idx+1] = g;
        buffer[idx+2] = b;
    }
}

void drawLine(u8* buffer, int fbWidth, int fbHeight, int x0, int y0, int x1, int y1, u8 r, u8 g, u8 b) {
    int dx = abs(x1 - x0);
    int dy = abs(y1 - y0);
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx - dy;
    int steps = std::max(dx, dy);

    for (int i = 0; i <= steps; i++) {
        float t = (steps == 0) ? 0.0f : static_cast<float>(i) / steps;
        int x = x0 + (x1 - x0) * t;
        int y = y0 + (y1 - y0) * t;

        drawPointOnBuffer(buffer, fbWidth, fbHeight, x, y, r, g, b);

        if (x == x1 && y == y1) break;
    }
}

void processDrawPacket(const uint8_t* packet, size_t length, u8* buffer, int fbWidth, int fbHeight,
                       u8* fullCanvas, int canvasWidth, int canvasHeight) {
    if (length < 6) return; // Packet too short

    uint8_t r = packet[1];
    uint8_t g = packet[2];
    uint8_t b = packet[3];
    uint8_t size = packet[4];
    uint8_t numPoints = packet[5];

    if (length != static_cast<size_t>(6 + numPoints * 4)) return; // Invalid packet length

    int prevX = -1, prevY = -1;

    for (int i = 0; i < numPoints; i++) {
        uint16_t x = *(uint16_t*)(packet + 6 + i * 4);
        uint16_t y = *(uint16_t*)(packet + 8 + i * 4);

        if (prevX != -1 && prevY != -1) {
            // Draw line on fullCanvas
            drawLine(fullCanvas, canvasWidth, canvasHeight, prevX, prevY, x, y, r, g, b);
        }

        prevX = x;
        prevY = y;
    }
}

static void sendDrawBatchCommand(int sock, const std::vector<DrawPoint>& points, const char* color = "#000000", int size = 1) {
    if (sock < 0 || points.empty()) return;

    uint8_t packet[1024]; // Adjust size as needed
    packet[0] = 1; // Type: drawBatch
    sscanf(color + 1, "%2hhx%2hhx%2hhx", &packet[1], &packet[2], &packet[3]);
    packet[4] = size;
    packet[5] = points.size() > 255 ? 255 : points.size();

    for (size_t i = 0; i < packet[5]; i++) {
        *(uint16_t*)(packet + 6 + i * 4) = points[i].x;
        *(uint16_t*)(packet + 8 + i * 4) = points[i].y;
    }

    send(sock, packet, 6 + packet[5] * 4, 0);
}

int main() {
    gfxInitDefault();
    consoleInit(GFX_TOP, NULL);

    printf("3DS Drawing App with Panning\n");

    SOC_buffer = (u32*)memalign(SOC_ALIGN, SOC_BUFFERSIZE);
    if(SOC_buffer == NULL) {
        failExit("memalign: failed to allocate\n");
    }

    if (socInit(SOC_buffer, SOC_BUFFERSIZE) != 0) {
        failExit("socInit failed\n");
    }
    atexit(socShutdown);

    const char* server_ip = "10.0.0.166";
    int server_port = 3030;

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        failExit("socket: %d %s\n", errno, strerror(errno));
    }

    struct sockaddr_in sa;
    memset(&sa,0,sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(server_port);
    inet_pton(AF_INET, server_ip, &sa.sin_addr);

    printf("Connecting to %s:%d...\n", server_ip, server_port);
    if (connect(sock, (struct sockaddr*)&sa, sizeof(sa)) < 0) {
        printf("Failed to connect: %d %s\n", errno, strerror(errno));
        close(sock);
        sock = -1;
    } else {
        printf("Connected!\n");
    }

    u16 fbWidth, fbHeight;
    u8* fb = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, &fbWidth, &fbHeight);
    if (!fb) {
        printf("Failed to get framebuffer\n");
        gfxExit();
        return 1;
    }

    printf("fbWidth=%u, fbHeight=%u\n", fbWidth, fbHeight);
    printf("Touch bottom screen to draw. \nSTART to refresh canvas and SELECT to exit.\nHold LEFT D-Pad for pan.\n");

    size_t bufferSize = fbWidth * fbHeight * 3;
    u8* buffer = (u8*)malloc(bufferSize);
    if (!buffer) {
        printf("Failed to allocate buffer\n");
        gfxExit();
        return 1;
    }
    memset(buffer, 255, bufferSize);

    int canvasWidth = 0;
    int canvasHeight = 0;
    int canvasSize = 0;
    u8* fullCanvas = NULL;

    if (sock >= 0) {
        char line[1024];
        if (!read_line(sock, line, sizeof(line))) {
            printf("Failed to read init line.\n");
        } else {
            char* wPtr = strstr(line, "\"width\":");
            char* hPtr = strstr(line, "\"height\":");
            char* sPtr = strstr(line, "\"canvasSize\":");
            if (wPtr && hPtr && sPtr) {
                canvasWidth = atoi(wPtr+8);
                canvasHeight = atoi(hPtr+9);
                canvasSize = atoi(sPtr+13);
                printf("Received canvas dimensions: W=%d, H=%d\n", canvasWidth, canvasHeight);

                fullCanvas = (u8*)malloc(canvasSize);
                if (fullCanvas && read_exact(sock, fullCanvas, canvasSize)) {
                } else {
                    printf("Failed to read canvas data.\n");
                    if (fullCanvas) { free(fullCanvas); fullCanvas = NULL; }
                }
            } else {
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

    auto clampOffsets = [&](int &ox, int &oy) {
        ox = std::max(0, std::min(ox, canvasWidth - fbWidth));
        oy = std::max(0, std::min(oy, canvasHeight - fbHeight));
    };

    while (aptMainLoop()) {
        hidScanInput();
        u32 kDown = hidKeysDown();
        u32 kHeld = hidKeysHeld();

        if (kDown & KEY_SELECT) {
            printf("Select key pressed. Exiting...\n");
            break;
        }

        if (kDown & KEY_START) {
            if (sock >= 0) {
                char request[] = "getCanvas\n";
                send(sock, request, strlen(request), 0);
                
                char response[1024];
                if (read_line(sock, response, sizeof(response)) && strstr(response, "canvasData")) {
                    if (read_exact(sock, fullCanvas, canvasSize)) {
                        printf("Canvas refreshed from server.\n");
                    } else {
                        printf("Failed to read canvas data.\n");
                    }
                } else {
                    printf("Invalid response from server.\n");
                }
            }
        }

        touchPosition touch;
        hidTouchRead(&touch);

        if (kHeld & KEY_DLEFT) {
            // Panning mode
            if (kHeld & KEY_TOUCH) {
                if (prevTouchX == -1 && prevTouchY == -1) {
                    prevTouchX = touch.px;
                    prevTouchY = touch.py;
                } else {
                    int deltaX = touch.px - prevTouchX;
                    int deltaY = touch.py - prevTouchY;
                    
                    offsetX -= deltaX;
                    offsetY -= deltaY;

                    clampOffsets(offsetX, offsetY);

                    prevTouchX = touch.px;
                    prevTouchY = touch.py;
                }
            } else {
                prevTouchX = prevTouchY = -1;
            }
        } else {
            // Normal drawing mode
            if (kHeld & KEY_TOUCH) {
                if (prevTouchX == -1 && prevTouchY == -1) {
                    prevTouchX = touch.px;
                    prevTouchY = touch.py;
                } else {
                    int steps = std::max(abs(touch.px - prevTouchX), abs(touch.py - prevTouchY));
                    for (int i = 0; i <= steps; i++) {
                        float t = (steps == 0) ? 0.0f : static_cast<float>(i) / steps;
                        int x = prevTouchX + (touch.px - prevTouchX) * t;
                        int y = prevTouchY + (touch.py - prevTouchY) * t;
                        
                        int FbX = x;
                        int FbY = y;

                        drawPointOnBuffer(buffer, fbWidth, fbHeight, FbX, FbY, 0, 0, 0);

                        int C_x = FbX + offsetX;
                        int C_y = FbY + offsetY;
                        if (C_x >= 0 && C_x < canvasWidth && C_y >= 0 && C_y < canvasHeight) {
                            int canvasIdx = 3 * (C_y * canvasWidth + C_x);
                            fullCanvas[canvasIdx] = fullCanvas[canvasIdx + 1] = fullCanvas[canvasIdx + 2] = 0;
                        }

                        pointBuffer.push_back({C_x, C_y});
                    }

                    if (pointBuffer.size() >= 10) {
                        sendDrawBatchCommand(sock, pointBuffer, "#000000", 1);
                        pointBuffer.clear();
                    }

                    prevTouchX = touch.px;
                    prevTouchY = touch.py;
                }
            } else {
                // Stylus released
                if (!pointBuffer.empty()) {
                    sendDrawBatchCommand(sock, pointBuffer, "#000000", 1);
                    pointBuffer.clear();
                }
                prevTouchX = prevTouchY = -1;
            }
        }

        // Server messages
        if (sock >= 0) {
            uint8_t packetBuffer[1024];
            int recvLen = recv(sock, packetBuffer, sizeof(packetBuffer), 0);
            if (recvLen > 0) {
                processDrawPacket(packetBuffer, recvLen, buffer, fbWidth, fbHeight, fullCanvas, canvasWidth, canvasHeight);
            } else if (recvLen == 0) {
                close(sock);
                sock = -1;
                printf("Server disconnected.\n");
            }
        }

        // Rendering
        if (fullCanvas) {
        for (int y = 0; y < fbHeight; y++) {
            for (int x = 0; x < fbWidth; x++) { 
                int C_x = y + offsetX;
                int C_y = (fbWidth - 1 - x) + offsetY;
                if (C_x >= 0 && C_x < canvasWidth && C_y >= 0 && C_y < canvasHeight) {
                    int bufferIdx = 3 * (y * fbWidth + x);
                    int canvasIdx = 3 * (C_y * canvasWidth + C_x);
                    buffer[bufferIdx] = fullCanvas[canvasIdx];
                    buffer[bufferIdx + 1] = fullCanvas[canvasIdx + 1];
                    buffer[bufferIdx + 2] = fullCanvas[canvasIdx + 2];
                } else {
                    // Draw white for areas outside the canvas
                    int bufferIdx = 3 * (y * fbWidth + x);
                    buffer[bufferIdx] = buffer[bufferIdx + 1] = buffer[bufferIdx + 2] = 255;
                }
            }
        }
    }

        gspWaitForVBlank();
        fb = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL);
        memcpy(fb, buffer, bufferSize);
        gfxFlushBuffers();
        gfxSwapBuffers();
    }

    if (sock >= 0) close(sock);
    if (fullCanvas) free(fullCanvas);
    free(buffer);
    gfxExit();
    return 0;
}