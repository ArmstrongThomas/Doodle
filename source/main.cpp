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

#define SOC_ALIGN       0x1000
#define SOC_BUFFERSIZE  0x100000

static u32 *SOC_buffer = NULL;
static int sock = -1;

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

// A blocking read_line function that reads until '\n' or an error occurs
// This sets the socket to blocking mode temporarily for simplicity.
static bool read_line(int s, char* buffer, size_t maxlen) {
    // Make sure the socket is blocking
    int flags = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, flags & ~O_NONBLOCK);

    size_t pos = 0;
    char c;
    while (pos < maxlen - 1) {

        int ret = recv(s, &c, 1, 0);
        if (ret <= 0) {
            // error or connection closed
            return false;
        }
        if (c == '\n') {
            buffer[pos] = '\0';
            // Restore non-blocking if desired
            fcntl(s, F_SETFL, flags);
            return true;
        }
        buffer[pos++] = c;
    }

    // Restore non-blocking if desired
    fcntl(s, F_SETFL, flags);
    buffer[pos] = '\0';
    return true; // line too long, but return what we got
}

// A blocking read_exact function that reads exactly 'length' bytes
static bool read_exact(int s, void* buf, size_t length) {
    // Make sure the socket is blocking
    int flags = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, flags & ~O_NONBLOCK);

    size_t received = 0;
    char* ptr = (char*)buf;
    while (received < length) {
        int ret = recv(s, ptr + received, length - received, 0);
        if (ret <= 0) {
            // error or connection closed
            fcntl(s, F_SETFL, flags);
            return false;
        }
        received += ret;
    }

    // Restore non-blocking if desired
    fcntl(s, F_SETFL, flags);
    return true;
}

// Define a structure for storing draw points
struct DrawPoint {
    int x, y;
};

// Global buffer for batch points
std::vector<DrawPoint> pointBuffer;

// Integer-based Bresenham's line
void drawLine(u8* buffer, int fbWidth, int fbHeight, int x0, int y0, int x1, int y1) {
    int dx = abs(x1 - x0);
    int sx = (x0 < x1) ? 1 : -1;
    int dy = -abs(y1 - y0);
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx + dy;

    while (true) {
        if (x0 >= 0 && x0 < fbWidth && y0 >= 0 && y0 < fbHeight) {
            int idx = 3 * (y0 * fbWidth + x0);
            buffer[idx] = 0; 
            buffer[idx+1] = 0; 
            buffer[idx+2] = 0; 
        }

        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static void sendDrawBatchCommand(int sock, const std::vector<DrawPoint>& points, const char* color = "#000000", int size = 1) {
    if (sock < 0 || points.empty()) return;

    std::string msg = "{\"type\":\"drawBatch\",\"points\":[";
    for (size_t i = 0; i < points.size(); ++i) {
        msg += "{\"x\":" + std::to_string(points[i].x) + ",\"y\":" + std::to_string(points[i].y) + "}";
        if (i < points.size() - 1) {
            msg += ",";
        }
    }
    msg += "],\"color\":\"" + std::string(color) + "\",\"size\":" + std::to_string(size) + "}\n";

    send(sock, msg.c_str(), msg.size(), 0);
}

static void processServerMessage(u8* buffer, int fbWidth, int fbHeight, const char* msg) {
    int x,y,size;
    char color[8] = "#000000";
    if (strstr(msg, "\"type\":\"draw\"")) {
        char* xPtr = strstr((char*)msg,"\"x\":");
        char* yPtr = strstr((char*)msg,"\"y\":");
        char* cPtr = strstr((char*)msg,"\"color\":\"");
        char* sPtr = strstr((char*)msg,"\"size\":");

        if(xPtr && yPtr && cPtr && sPtr) {
            x = atoi(xPtr+4);
            y = atoi(yPtr+4);
            {
                char* start = cPtr+9; 
                char* end = strchr(start,'"');
                if(end && end - start < 8) {
                    strncpy(color, start, end - start);
                    color[end - start] = '\0';
                }
            }
            size = atoi(sPtr+7);

            int r=0,g=0,b=0;
            if (strlen(color) == 7 && color[0] == '#') {
                char rc[3]={color[1],color[2],'\0'};
                char gc[3]={color[3],color[4],'\0'};
                char bc[3]={color[5],color[6],'\0'};
                r = strtol(rc,NULL,16);
                g = strtol(gc,NULL,16);
                b = strtol(bc,NULL,16);
            }

            for (int dx = -size/2; dx <= size/2; dx++) {
                for (int dy = -size/2; dy <= size/2; dy++) {
                    int nx = x + dx;
                    int ny = y + dy;
                    if (nx >=0 && nx < fbWidth && ny >=0 && ny < fbHeight) {
                        int idx = 3*(ny*fbWidth + nx);
                        buffer[idx+0] = r;
                        buffer[idx+1] = g;
                        buffer[idx+2] = b;
                    }
                }
            }
        }
    }
}

int main() {
    gfxInitDefault();
    consoleInit(GFX_TOP, NULL);

    printf("3DS Drawing App with Networking\n");

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

    // Get framebuffer info
    u16 fbWidth, fbHeight;
    u8* fb = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, &fbWidth, &fbHeight);
    if (!fb) {
        printf("Failed to get framebuffer\n");
        gfxExit();
        return 1;
    }

    printf("fbWidth=%u, fbHeight=%u\n", fbWidth, fbHeight);
    printf("Touch bottom screen to draw. START to clear.\n");

    size_t bufferSize = fbWidth * fbHeight * 3;
    u8* buffer = (u8*)malloc(bufferSize);
    if (!buffer) {
        printf("Failed to allocate buffer\n");
        gfxExit();
        return 1;
    }
    memset(buffer, 255, bufferSize);

    bool isTouching = false;
    int lastDrawX = 0;
    int lastDrawY = 0;

    int canvasWidth = fbHeight;  // Because we want a 320x240 canvas, and fbHeight=320, fbWidth=240
    int canvasHeight = fbWidth;  // swap because of rotation
    int canvasSize = 0; 

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
                printf("Received init: width=%d, height=%d, canvasSize=%d\n", canvasWidth, canvasHeight, canvasSize);

                // Allocate a temporary buffer to read the canvas from server
                u8* tempCanvas = (u8*)malloc(canvasSize);
                if (tempCanvas && read_exact(sock, tempCanvas, canvasSize)) {
                    // Clear buffer
                    memset(buffer, 255, bufferSize);

                    // Now map the canvas directly:
                    // Canvas (C_x, C_y) -> Screen (Sx, Sy) = (C_x, C_y)
                    // We know Sx = FbY and Sy = fbWidth - 1 - FbX
                    // Rearranging: FbX = fbWidth - 1 - C_y, FbY = C_x
                    for (int C_y = 0; C_y < canvasHeight && C_y < (int)fbWidth; C_y++) {
                        for (int C_x = 0; C_x < canvasWidth && C_x < (int)fbHeight; C_x++) {
                            int FbX = fbWidth - 1 - C_y;
                            int FbY = C_x;

                            int bufferIdx = 3 * (FbY * fbWidth + FbX);
                            int canvasIdx = 3 * (C_y * canvasWidth + C_x);

                            buffer[bufferIdx + 0] = tempCanvas[canvasIdx + 0];
                            buffer[bufferIdx + 1] = tempCanvas[canvasIdx + 1];
                            buffer[bufferIdx + 2] = tempCanvas[canvasIdx + 2];
                        }
                    }
                } else {
                    printf("Failed to read canvas data.\n");
                }
                if (tempCanvas) free(tempCanvas);
            } else {
                printf("Invalid init line: %s\n", line);
            }
        }

        // After reading init and canvas, set socket back to non-blocking for main loop
        int flags = fcntl(sock, F_GETFL, 0);
        fcntl(sock, F_SETFL, flags | O_NONBLOCK);
    }

    char netBuf[1024];
    memset(netBuf,0,sizeof(netBuf));
    int netBufPos = 0;

    while (aptMainLoop()) {
        hidScanInput();
        u32 kDown = hidKeysDown();
        u32 kHeld = hidKeysHeld();

        if (kDown & KEY_START) {
            memset(buffer, 255, bufferSize);
            isTouching = false;
        }

        if (kHeld & KEY_TOUCH) {
            touchPosition touch;
            hidTouchRead(&touch);

            // Adjust touch coordinates
            // The screen coordinate (Sx, Sy) = (touch.px, touch.py)
            // We must convert (Sx, Sy) back to FbX, FbY:
            // Sx = FbY
            // Sy = fbWidth - 1 - FbX
            // So FbY = Sx = touch.px, FbX = fbWidth - 1 - Sy = fbWidth - 1 - touch.py
            int FbX = fbWidth - 1 - touch.py;
            int FbY = touch.px;

            if (FbX >= 0 && FbX < fbWidth && FbY >= 0 && FbY < fbHeight) {
                // Draw locally
                drawLine(buffer, fbWidth, fbHeight, lastDrawX, lastDrawY, FbX, FbY);

                // Convert framebuffer coords back to canvas coords if sending to server
                // Canvas coords (C_x, C_y) = (Sx, Sy) = (FbY, fbWidth - 1 - FbX)
                int C_x = FbY;
                int C_y = fbWidth - 1 - FbX;
                pointBuffer.push_back({C_x, C_y});

                if (pointBuffer.size() >= 10) { 
                    sendDrawBatchCommand(sock, pointBuffer, "#000000", 1);
                    pointBuffer.clear();
                }

                lastDrawX = FbX;
                lastDrawY = FbY;
                isTouching = true;
            }
        } else {
            if (isTouching && !pointBuffer.empty()) {
                sendDrawBatchCommand(sock, pointBuffer, "#000000", 1);
                pointBuffer.clear();
            }
            isTouching = false;
        }

        // Non-blocking read for subsequent JSON messages
        if (sock >= 0) {
            int recvLen = recv(sock, netBuf+netBufPos, sizeof(netBuf)-netBufPos-1, 0);
            if (recvLen > 0) {
                netBufPos += recvLen;
                netBuf[netBufPos] = '\0';

                char* start = netBuf;
                char* nl;
                while ((nl = strchr(start, '\n'))) {
                    *nl = '\0';
                    processServerMessage(buffer, fbWidth, fbHeight, start);
                    start = nl+1;
                }

                int leftover = (int)strlen(start);
                memmove(netBuf, start, leftover+1);
                netBufPos = leftover;
            } else if (recvLen == 0) {
                close(sock);
                sock = -1;
                printf("Server disconnected.\n");
            }
            // if recvLen < 0 && errno == EAGAIN, no data this frame
        }

        gspWaitForVBlank();
        fb = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL);
        memcpy(fb, buffer, bufferSize);
        gfxFlushBuffers();
        gfxSwapBuffers();
    }

    if (sock >= 0) close(sock);
    free(buffer);
    gfxExit();
    return 0;
}
