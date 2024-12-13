#include <3ds.h>
#include <stdio.h>
#include <string.h>
#include <cstdlib> // For abs()

// Integer-based Bresenham's line drawing algorithm
void drawLine(u8* buffer, int fbWidth, int fbHeight, int x0, int y0, int x1, int y1) {
    int dx = abs(x1 - x0);
    int sx = (x0 < x1) ? 1 : -1;
    int dy = -abs(y1 - y0);
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx + dy; // error value e_xy

    while (true) {
        if (x0 >= 0 && x0 < fbWidth && y0 >= 0 && y0 < fbHeight) {
            buffer[3 * (y0 * fbWidth + x0) + 0] = 0; // Red
            buffer[3 * (y0 * fbWidth + x0) + 1] = 0; // Green
            buffer[3 * (y0 * fbWidth + x0) + 2] = 0; // Blue
        }

        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

int main() {
    // Initialize graphics and console
    gfxInitDefault();
    consoleInit(GFX_TOP, NULL);

    // Get initial bottom framebuffer to determine its width and height
    u16 fbWidth, fbHeight;
    u8* fb = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, &fbWidth, &fbHeight);
    if (!fb) {
        printf("Failed to get framebuffer\n");
        gfxExit();
        return 1;
    }

    // Print out the dimensions for debugging
    printf("fbWidth=%u, fbHeight=%u\n", fbWidth, fbHeight);
    printf("Touch the bottom screen to draw! Press START to clear.\n");

    // Allocate an offscreen buffer (3 bytes per pixel for RGB)
    size_t bufferSize = fbWidth * fbHeight * 3;
    u8* buffer = (u8*)malloc(bufferSize);
    if (!buffer) {
        printf("Failed to allocate buffer\n");
        gfxExit();
        return 1;
    }

    // Initialize the buffer to white
    memset(buffer, 255, bufferSize);

    // Variables for drawing
    bool isTouching = false;
    int lastDrawX = 0;
    int lastDrawY = 0;

    while (aptMainLoop()) {
        // Process input
        hidScanInput();
        u32 kDown = hidKeysDown();
        u32 kHeld = hidKeysHeld();

        // Clear screen on START press
        if (kDown & KEY_START) {
            memset(buffer, 255, bufferSize);
            isTouching = false;
        }

        // Handle touch input
        if (kHeld & KEY_TOUCH) {
            touchPosition touch;
            hidTouchRead(&touch);

            // Raw touch coordinates:
            // touch.px in [0..319], touch.py in [0..239]

            // Transform (px, py) to (bufferX, bufferY) for rotated framebuffer
            int bufferX = (fbWidth - 1) - touch.py;
            int bufferY = touch.px;


            // Check bounds and draw if valid
            if (bufferX >= 0 && bufferX < fbWidth && bufferY >= 0 && bufferY < fbHeight) {
                // Draw a black dot at the transformed coordinates
                int index = 3 * (bufferY * fbWidth + bufferX);
                buffer[index + 0] = 0;
                buffer[index + 1] = 0;
                buffer[index + 2] = 0;

                // If previously touching, draw a line from the last drawn point
                if (isTouching) {
                    drawLine(buffer, fbWidth, fbHeight, lastDrawX, lastDrawY, bufferX, bufferY);
                }

                // Update last draw coordinates
                lastDrawX = bufferX;
                lastDrawY = bufferY;
                isTouching = true;
            }
        } else {
            isTouching = false;
        }

        // Wait for VBlank before getting the current framebuffer
        gspWaitForVBlank();

        // Get the current framebuffer (no need to update fbWidth, fbHeight)
        fb = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL);

        // Copy the offscreen buffer to the current framebuffer
        memcpy(fb, buffer, bufferSize);

        // Flush and swap buffers
        gfxFlushBuffers();
        gfxSwapBuffers();
    }

    // Free the offscreen buffer and exit
    free(buffer);
    gfxExit();
    return 0;
}
