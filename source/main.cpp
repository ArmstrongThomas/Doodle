#include <3ds.h>
#include <stdio.h>

int main() {
    gfxInitDefault();
    consoleInit(GFX_TOP, NULL);

    printf("Hello, Nintendo 3DS!");

    while (aptMainLoop()) {
        hidScanInput();
        u32 kDown = hidKeysDown();
        if (kDown & KEY_START) break; // Exit on START button

        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    }

    gfxExit();
    return 0;
}
