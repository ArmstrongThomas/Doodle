#include <3ds.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <malloc.h>
#include <cstdlib>
#include <stdarg.h>
#include <vector>
#include <string>
#include <sstream>
#include <math.h>
#include <algorithm>
#include <unordered_map>
#include <sys/stat.h>
#include <errno.h>
#include "ui.h"
#include "network.h"
#include "canvas_state.h"
#include "renderer.h"
#include "protocol.h"
#include "updater.h"

// TLS certificate verification can exceed libctru's 32 KiB default main
// thread stack. The updater deliberately runs HTTPS on the main thread so it
// remains available when the realtime worker cannot connect.
extern "C"
{
u32 __stacksize__ = 256 * 1024;
}

#ifndef TEST_MODE
#error TEST_MODE must be provided by the build system
#endif
#ifndef UPDATER_ENABLED
#error UPDATER_ENABLED must be provided by the build system
#endif
#if (UPDATER_ENABLED != 0) && (UPDATER_ENABLED != 1)
#error UPDATER_ENABLED must be 0 or 1
#endif

#define DOODLE_STRINGIFY_INNER(value) #value
#define DOODLE_STRINGIFY(value) DOODLE_STRINGIFY_INNER(value)

static const char gBuildConfiguration[] =
    "DoodleBuildConfig:test=" DOODLE_STRINGIFY(TEST_MODE)
    ";updater=" DOODLE_STRINGIFY(UPDATER_ENABLED)
    ";ws_secure=" DOODLE_STRINGIFY(SERVER_WS_SECURE)
    ";ws=" SERVER_WS_HOST ":" SERVER_WS_PORT SERVER_WS_PATH
    ";https=" SERVER_HTTPS_HOST ":" SERVER_HTTPS_PORT;

Color currentColor = {255, 0, 0}; // Red by default
int currentBrushSize = 1;
int currentBrushShape = 0;
static const int BRUSH_CIRCLE = 0;
static const int BRUSH_SQUARE = 1;
static const int BRUSH_DITHER = 2;
static const int BRUSH_ERASER = 3;
static bool gRainbowEnabled = false;
static bool gRainbowStrokeColorValid = false;
static Color gRainbowStrokeColor = {255, 0, 0};

struct DeviceIdentity {
    char deviceId[48];
    char deviceSecret[64];
    char displayName[32];
    char username[25];
    char backupCode[32];
    char rulesAcceptedVersion[32];
};

static DeviceIdentity gIdentity = {{0}, {0}, "3DS User", "pending", "", ""};
static char gHardwareId[32] = "";
static char gDeviceModel[24] = "3ds-family";
static char gIdentityStorageStatus[64] = "STORE UNKNOWN";
static char gIdentityBootStatus[64] = "BOOT UNKNOWN";
static const char *IDENTITY_PRIMARY_PATH = "sdmc:/3ds/CollabDoodle/identity.txt";
static const char *IDENTITY_FALLBACK_PATH = "sdmc:/3ds/CollabDoodle.identity.txt";
static int gLastPrimaryReadErr = 0;
static int gLastFallbackReadErr = 0;
static char gRequiredRulesVersion[32] = "";
static bool gNeedsDisplayName = false;
static bool gNeedsRules = false;

enum OnboardingStage
{
    ONBOARDING_READY = 0,
    ONBOARDING_WAITING_DISPLAY_NAME,
    ONBOARDING_SUBMITTING_DISPLAY_NAME,
    ONBOARDING_WAITING_RULES,
    ONBOARDING_SUBMITTING_RULES
};

static char gDisconnectReason[80] = "";
static volatile bool gAptResumeRequested = false;
static volatile bool gAptWentToSleep = false;
static const size_t CONTROL_LINE_CAPACITY = 2048;
static const uint64_t MAX_CANVAS_BYTES = 12ULL * 1024ULL * 1024ULL;

static bool isValidCanvasMeta(const CanvasMeta &meta)
{
    if (meta.width <= 0 || meta.height <= 0 || meta.compressedSize <= 0 || meta.compressedSize > 10000000)
        return false;
    uint64_t pixelBytes = (uint64_t)meta.width * (uint64_t)meta.height * 3ULL;
    return pixelBytes > 0 && pixelBytes <= MAX_CANVAS_BYTES;
}

static void handleAptEvent(APT_HookType hook, void *)
{
    if (hook == APTHOOK_ONSLEEP)
        gAptWentToSleep = true;
    else if (hook == APTHOOK_ONWAKEUP && gAptWentToSleep)
    {
        gAptWentToSleep = false;
        gAptResumeRequested = true;
    }
}

struct ActiveDrawLabel {
    bool active;
    char name[25];
    int canvasX;
    int canvasY;
    float displayX;
    float displayY;
    bool hasDisplay;
    u64 updatedAt;
};

static const int MAX_ACTIVE_DRAW_LABELS = 8;
static const u64 DRAW_LABEL_TTL_MS = 1400;

static void seedRandom()
{
    static bool seeded = false;
    if (!seeded)
    {
        srand((unsigned int)(osGetTime() ^ svcGetSystemTick()));
        seeded = true;
    }
}

static void initHardwareId()
{
    u64 hash = 0;
    Result cfgResult = cfguInit();
    Result hashResult = cfgResult;
    if (R_SUCCEEDED(cfgResult))
    {
        hashResult = CFGU_GenHashConsoleUnique(0xC011AB00, &hash);
        u8 model = 0xFF;
        if (R_SUCCEEDED(CFGU_GetSystemModel(&model)))
        {
            const char *modelName = "3ds-family";
            switch (model)
            {
                case CFG_MODEL_3DS: modelName = "3ds"; break;
                case CFG_MODEL_3DSXL: modelName = "3ds-xl"; break;
                case CFG_MODEL_N3DS: modelName = "new-3ds"; break;
                case CFG_MODEL_2DS: modelName = "2ds"; break;
                case CFG_MODEL_N3DSXL: modelName = "new-3ds-xl"; break;
                case CFG_MODEL_N2DSXL: modelName = "new-2ds-xl"; break;
                default: break;
            }
            snprintf(gDeviceModel, sizeof(gDeviceModel), "%s", modelName);
        }
        cfguExit();
    }
    if (R_SUCCEEDED(hashResult) && hash != 0)
    {
        snprintf(gHardwareId, sizeof(gHardwareId), "3ds-hw-%016llX", (unsigned long long)hash);
    }
    else
    {
        snprintf(gHardwareId, sizeof(gHardwareId), "3ds-hw-unavailable");
    }
}

static void randomHex(char *out, size_t outSize)
{
    static const char *hex = "0123456789abcdef";
    if (!out || outSize == 0)
        return;
    seedRandom();
    for (size_t i = 0; i + 1 < outSize; i++)
        out[i] = hex[rand() & 15];
    out[outSize - 1] = '\0';
}

static void chooseRandomDrawingColor()
{
    seedRandom();
    float h = (float)(rand() % 360) / 360.0f;
    float s = 0.72f + ((float)(rand() % 24) / 100.0f);
    float v = 0.78f + ((float)(rand() % 20) / 100.0f);
    float r, g, b;
    UIState::HSVtoRGB(h, s, v, r, g, b);
    currentColor.r = (u8)(r * 255.0f);
    currentColor.g = (u8)(g * 255.0f);
    currentColor.b = (u8)(b * 255.0f);
    UIState::updateHSV(h, s, v);
}

static void trimLine(char *text)
{
    if (!text)
        return;
    size_t len = strlen(text);
    while (len > 0 && (text[len - 1] == '\n' || text[len - 1] == '\r' || text[len - 1] == ' '))
        text[--len] = '\0';
}

static bool readIdentityFile(const char *path)
{
    errno = 0;
    FILE *file = fopen(path, "r");
    int openErr = errno;
    if (!file)
    {
        if (strcmp(path, IDENTITY_PRIMARY_PATH) == 0)
            gLastPrimaryReadErr = openErr;
        else
            gLastFallbackReadErr = openErr;
        return false;
    }

    char deviceId[96] = "";
    char deviceSecret[128] = "";
    char displayName[64] = "";
    char username[64] = "";
    char backupCode[64] = "";
    char rulesAcceptedVersion[32] = "";
    fgets(deviceId, sizeof(deviceId), file);
    fgets(deviceSecret, sizeof(deviceSecret), file);
    fgets(displayName, sizeof(displayName), file);
    fgets(username, sizeof(username), file);
    fgets(backupCode, sizeof(backupCode), file);
    fgets(rulesAcceptedVersion, sizeof(rulesAcceptedVersion), file);
    fclose(file);

    trimLine(deviceId);
    trimLine(deviceSecret);
    trimLine(displayName);
    trimLine(username);
    trimLine(backupCode);
    trimLine(rulesAcceptedVersion);
    snprintf(gIdentity.deviceId, sizeof(gIdentity.deviceId), "%s", deviceId);
    snprintf(gIdentity.deviceSecret, sizeof(gIdentity.deviceSecret), "%s", deviceSecret);
    snprintf(gIdentity.displayName, sizeof(gIdentity.displayName), "%s", displayName);
    snprintf(gIdentity.username, sizeof(gIdentity.username), "%s", username);
    snprintf(gIdentity.backupCode, sizeof(gIdentity.backupCode), "%s", backupCode);
    snprintf(gIdentity.rulesAcceptedVersion, sizeof(gIdentity.rulesAcceptedVersion), "%s", rulesAcceptedVersion);
    if (!gIdentity.displayName[0])
        strcpy(gIdentity.displayName, "3DS User");
    if (!gIdentity.username[0])
        strcpy(gIdentity.username, "pending");

    return gIdentity.deviceId[0] && gIdentity.deviceSecret[0];
}

static bool writeIdentityFile(const char *path)
{
    errno = 0;
    FILE *file = fopen(path, "w");
    if (!file)
        return false;
    fprintf(file, "%s\n%s\n%s\n%s\n%s\n%s\n", gIdentity.deviceId, gIdentity.deviceSecret,
            gIdentity.displayName, gIdentity.username, gIdentity.backupCode, gIdentity.rulesAcceptedVersion);
    fflush(file);
    bool ok = fclose(file) == 0;
    return ok;
}

static bool verifyIdentityWrite(const char *path)
{
    DeviceIdentity expected = gIdentity;
    DeviceIdentity current = gIdentity;
    bool ok = readIdentityFile(path) &&
              strcmp(gIdentity.deviceId, expected.deviceId) == 0 &&
              strcmp(gIdentity.deviceSecret, expected.deviceSecret) == 0;
    gIdentity = current;
    return ok;
}

static const char *deviceIdSuffix()
{
    size_t len = strlen(gIdentity.deviceId);
    return len > 6 ? gIdentity.deviceId + len - 6 : gIdentity.deviceId;
}

static void saveDeviceIdentity()
{
    mkdir("sdmc:/3ds", 0777);
    mkdir("sdmc:/3ds/CollabDoodle", 0777);

    bool primaryOk = writeIdentityFile(IDENTITY_PRIMARY_PATH);
    int primaryErr = primaryOk ? 0 : errno;
    bool fallbackOk = writeIdentityFile(IDENTITY_FALLBACK_PATH);
    int fallbackErr = fallbackOk ? 0 : errno;
    bool primaryVerify = primaryOk && verifyIdentityWrite(IDENTITY_PRIMARY_PATH);
    bool fallbackVerify = fallbackOk && verifyIdentityWrite(IDENTITY_FALLBACK_PATH);

    if (primaryVerify)
        snprintf(gIdentityStorageStatus, sizeof(gIdentityStorageStatus), "SAVED P %s", deviceIdSuffix());
    else if (fallbackVerify)
        snprintf(gIdentityStorageStatus, sizeof(gIdentityStorageStatus), "SAVED F %s", deviceIdSuffix());
    else if (primaryOk || fallbackOk)
        snprintf(gIdentityStorageStatus, sizeof(gIdentityStorageStatus), "WRITE NOREAD %d/%d", gLastPrimaryReadErr, gLastFallbackReadErr);
    else
        snprintf(gIdentityStorageStatus, sizeof(gIdentityStorageStatus), "SAVE ERR %d/%d", primaryErr, fallbackErr);
}

static void loadDeviceIdentity()
{
    Result fsResult = fsInit();
    mkdir("sdmc:/3ds", 0777);
    mkdir("sdmc:/3ds/CollabDoodle", 0777);

    if (readIdentityFile(IDENTITY_PRIMARY_PATH))
    {
        snprintf(gIdentityBootStatus, sizeof(gIdentityBootStatus), "LOADED P %s", deviceIdSuffix());
        snprintf(gIdentityStorageStatus, sizeof(gIdentityStorageStatus), "%s", gIdentityBootStatus);
        return;
    }
    int primaryErr = errno;

    if (readIdentityFile(IDENTITY_FALLBACK_PATH))
    {
        snprintf(gIdentityBootStatus, sizeof(gIdentityBootStatus), "LOADED F %s", deviceIdSuffix());
        snprintf(gIdentityStorageStatus, sizeof(gIdentityStorageStatus), "%s", gIdentityBootStatus);
        saveDeviceIdentity();
        return;
    }
    int fallbackErr = errno;

    if (!gIdentity.deviceId[0] || !gIdentity.deviceSecret[0])
    {
        strcpy(gIdentity.deviceId, "3ds-");
        randomHex(gIdentity.deviceId + 4, sizeof(gIdentity.deviceId) - 4);
        randomHex(gIdentity.deviceSecret, sizeof(gIdentity.deviceSecret));
        strcpy(gIdentity.displayName, "3DS User");
        strcpy(gIdentity.username, "pending");
        gIdentity.backupCode[0] = '\0';
        gIdentity.rulesAcceptedVersion[0] = '\0';
        snprintf(gIdentityBootStatus, sizeof(gIdentityBootStatus), "NEW %s R%d/%d",
                 deviceIdSuffix(), primaryErr, fallbackErr);
        snprintf(gIdentityStorageStatus, sizeof(gIdentityStorageStatus), "%s", gIdentityBootStatus);
        saveDeviceIdentity();
        snprintf(gIdentityBootStatus, sizeof(gIdentityBootStatus), "NEW %s R%d/%d F%08lX",
                 deviceIdSuffix(), primaryErr, fallbackErr, (unsigned long)fsResult);
    }
}

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

static const u64 COLLAB_DOODLE_RELEASE_TITLE_ID = 0x000400000CE47500ULL;
static const u64 COLLAB_DOODLE_TEST_TITLE_ID = 0x000400000CE47600ULL;

static bool pathEndsWithIgnoreCase(const char *text, const char *suffix)
{
    if (!text || !suffix)
        return false;
    size_t textLen = strlen(text);
    size_t suffixLen = strlen(suffix);
    if (suffixLen > textLen)
        return false;
    return strcasecmp(text + textLen - suffixLen, suffix) == 0;
}

static bool isCiaLaunch(const char *appPath)
{
    u64 programId = 0;
    if (R_SUCCEEDED(APT_GetProgramID(&programId)) &&
        (programId == COLLAB_DOODLE_RELEASE_TITLE_ID || programId == COLLAB_DOODLE_TEST_TITLE_ID))
    {
        return true;
    }
    return !pathEndsWithIgnoreCase(appPath, ".3dsx");
}

static u64 currentInstalledTitleId()
{
    u64 programId = 0;
    if (R_SUCCEEDED(APT_GetProgramID(&programId)) &&
        (programId == COLLAB_DOODLE_RELEASE_TITLE_ID || programId == COLLAB_DOODLE_TEST_TITLE_ID))
    {
        return programId;
    }
    return 0;
}

#if UPDATER_ENABLED
static const char *updateTargetPathForPackage(const char *packageType, const char *appPath)
{
    return (packageType && strcmp(packageType, "cia") == 0) ? "sdmc:/cias/CollabDoodle-update.cia" : appPath;
}
#endif

static bool sendClientHello(const char *packageType)
{
    char hello[512];
    Protocol::buildHello(hello, sizeof(hello), APP_ID, APP_VERSION, UPDATER_ENABLED != 0,
                         gIdentity.deviceId, gIdentity.deviceSecret, gHardwareId, gDeviceModel,
                         gIdentity.displayName, packageType);
    return NetworkManager::sendSessionHello(hello, strlen(hello));
}

static void putPixel(u8 *fb, int width, int height, int x, int y, u8 r, u8 g, u8 b)
{
    if (!fb || x < 0 || y < 0 || x >= width || y >= height)
        return;
    int idx = 3 * (y * width + x);
    fb[idx] = b;
    fb[idx + 1] = g;
    fb[idx + 2] = r;
}

static void putScreenPixel(u8 *fb, int fbWidth, int fbHeight, int screenX, int screenY, u8 r, u8 g, u8 b)
{
    const bool rotatedFramebuffer = fbWidth < fbHeight;
    int fbX = rotatedFramebuffer ? fbWidth - 1 - screenY : screenX;
    int fbY = rotatedFramebuffer ? screenX : screenY;
    putPixel(fb, fbWidth, fbHeight, fbX, fbY, r, g, b);
}

static void fillRect(u8 *fb, int width, int height, int x, int y, int w, int h, u8 r, u8 g, u8 b)
{
    for (int py = y; py < y + h; py++)
        for (int px = x; px < x + w; px++)
            putScreenPixel(fb, width, height, px, py, r, g, b);
}

static void drawRectOutline(u8 *fb, int fbWidth, int fbHeight, int x, int y, int w, int h, u8 r, u8 g, u8 b)
{
    if (!fb || w <= 0 || h <= 0)
        return;
    for (int inset = 0; inset < 2; inset++)
    {
        for (int px = x + inset; px < x + w - inset; px++)
        {
            putScreenPixel(fb, fbWidth, fbHeight, px, y + inset, r, g, b);
            putScreenPixel(fb, fbWidth, fbHeight, px, y + h - 1 - inset, r, g, b);
        }
        for (int py = y + inset; py < y + h - inset; py++)
        {
            putScreenPixel(fb, fbWidth, fbHeight, x + inset, py, r, g, b);
            putScreenPixel(fb, fbWidth, fbHeight, x + w - 1 - inset, py, r, g, b);
        }
    }
}

static void drawMiniText(u8 *fb, int width, int height, int x, int y, const char *text, u8 r, u8 g, u8 b);

static void applyCanvasRectLocal(CanvasState &canvas, int x, int y, int w, int h, Color color)
{
    if (!canvas.pixels || w <= 0 || h <= 0)
        return;
    int minX = std::max(0, x);
    int minY = std::max(0, y);
    int maxX = std::min(canvas.width - 1, x + w - 1);
    int maxY = std::min(canvas.height - 1, y + h - 1);
    if (minX > maxX || minY > maxY)
        return;
    for (int py = minY; py <= maxY; py++)
    {
        for (int px = minX; px <= maxX; px++)
        {
            int idx = 3 * (py * canvas.width + px);
            canvas.pixels[idx] = color.r;
            canvas.pixels[idx + 1] = color.g;
            canvas.pixels[idx + 2] = color.b;
        }
    }
    canvas.markDirty((minX + maxX) / 2, (minY + maxY) / 2, std::max(maxX - minX, maxY - minY) / 2 + 2);
}

static void drawMiniGlyph(u8 *fb, int width, int height, int x, int y, char c, u8 r, u8 g, u8 b)
{
    u8 glyph[7] = {0};
    switch (c)
    {
        case 'A': { u8 g[7] = {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}; memcpy(glyph,g,7); break; }
        case 'B': { u8 g[7] = {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}; memcpy(glyph,g,7); break; }
        case 'C': { u8 g[7] = {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}; memcpy(glyph,g,7); break; }
        case 'D': { u8 g[7] = {0x1E,0x11,0x11,0x11,0x11,0x11,0x1E}; memcpy(glyph,g,7); break; }
        case 'E': { u8 g[7] = {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}; memcpy(glyph,g,7); break; }
        case 'F': { u8 g[7] = {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}; memcpy(glyph,g,7); break; }
        case 'G': { u8 g[7] = {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F}; memcpy(glyph,g,7); break; }
        case 'H': { u8 g[7] = {0x11,0x11,0x11,0x1F,0x11,0x11,0x11}; memcpy(glyph,g,7); break; }
        case 'I': { u8 g[7] = {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}; memcpy(glyph,g,7); break; }
        case 'J': { u8 g[7] = {0x07,0x02,0x02,0x02,0x12,0x12,0x0C}; memcpy(glyph,g,7); break; }
        case 'K': { u8 g[7] = {0x11,0x12,0x14,0x18,0x14,0x12,0x11}; memcpy(glyph,g,7); break; }
        case 'L': { u8 g[7] = {0x10,0x10,0x10,0x10,0x10,0x10,0x1F}; memcpy(glyph,g,7); break; }
        case 'M': { u8 g[7] = {0x11,0x1B,0x15,0x15,0x11,0x11,0x11}; memcpy(glyph,g,7); break; }
        case 'N': { u8 g[7] = {0x11,0x19,0x15,0x13,0x11,0x11,0x11}; memcpy(glyph,g,7); break; }
        case 'O': { u8 g[7] = {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}; memcpy(glyph,g,7); break; }
        case 'P': { u8 g[7] = {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}; memcpy(glyph,g,7); break; }
        case 'Q': { u8 g[7] = {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}; memcpy(glyph,g,7); break; }
        case 'R': { u8 g[7] = {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}; memcpy(glyph,g,7); break; }
        case 'S': { u8 g[7] = {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}; memcpy(glyph,g,7); break; }
        case 'T': { u8 g[7] = {0x1F,0x04,0x04,0x04,0x04,0x04,0x04}; memcpy(glyph,g,7); break; }
        case 'U': { u8 g[7] = {0x11,0x11,0x11,0x11,0x11,0x11,0x0E}; memcpy(glyph,g,7); break; }
        case 'V': { u8 g[7] = {0x11,0x11,0x11,0x11,0x11,0x0A,0x04}; memcpy(glyph,g,7); break; }
        case 'W': { u8 g[7] = {0x11,0x11,0x11,0x15,0x15,0x15,0x0A}; memcpy(glyph,g,7); break; }
        case 'X': { u8 g[7] = {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}; memcpy(glyph,g,7); break; }
        case 'Y': { u8 g[7] = {0x11,0x11,0x0A,0x04,0x04,0x04,0x04}; memcpy(glyph,g,7); break; }
        case 'Z': { u8 g[7] = {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}; memcpy(glyph,g,7); break; }
        case 'a': { u8 g[7] = {0x00,0x00,0x0E,0x01,0x0F,0x11,0x0F}; memcpy(glyph,g,7); break; }
        case 'b': { u8 g[7] = {0x10,0x10,0x16,0x19,0x11,0x11,0x1E}; memcpy(glyph,g,7); break; }
        case 'c': { u8 g[7] = {0x00,0x00,0x0E,0x10,0x10,0x11,0x0E}; memcpy(glyph,g,7); break; }
        case 'd': { u8 g[7] = {0x01,0x01,0x0D,0x13,0x11,0x11,0x0F}; memcpy(glyph,g,7); break; }
        case 'e': { u8 g[7] = {0x00,0x00,0x0E,0x11,0x1F,0x10,0x0E}; memcpy(glyph,g,7); break; }
        case 'f': { u8 g[7] = {0x06,0x08,0x08,0x1C,0x08,0x08,0x08}; memcpy(glyph,g,7); break; }
        case 'g': { u8 g[7] = {0x00,0x00,0x0F,0x11,0x0F,0x01,0x0E}; memcpy(glyph,g,7); break; }
        case 'h': { u8 g[7] = {0x10,0x10,0x16,0x19,0x11,0x11,0x11}; memcpy(glyph,g,7); break; }
        case 'i': { u8 g[7] = {0x04,0x00,0x0C,0x04,0x04,0x04,0x0E}; memcpy(glyph,g,7); break; }
        case 'j': { u8 g[7] = {0x02,0x00,0x06,0x02,0x02,0x12,0x0C}; memcpy(glyph,g,7); break; }
        case 'k': { u8 g[7] = {0x10,0x10,0x12,0x14,0x18,0x14,0x12}; memcpy(glyph,g,7); break; }
        case 'l': { u8 g[7] = {0x0C,0x04,0x04,0x04,0x04,0x04,0x0E}; memcpy(glyph,g,7); break; }
        case 'm': { u8 g[7] = {0x00,0x00,0x1A,0x15,0x15,0x15,0x15}; memcpy(glyph,g,7); break; }
        case 'n': { u8 g[7] = {0x00,0x00,0x16,0x19,0x11,0x11,0x11}; memcpy(glyph,g,7); break; }
        case 'o': { u8 g[7] = {0x00,0x00,0x0E,0x11,0x11,0x11,0x0E}; memcpy(glyph,g,7); break; }
        case 'p': { u8 g[7] = {0x00,0x00,0x1E,0x11,0x1E,0x10,0x10}; memcpy(glyph,g,7); break; }
        case 'q': { u8 g[7] = {0x00,0x00,0x0D,0x13,0x0F,0x01,0x01}; memcpy(glyph,g,7); break; }
        case 'r': { u8 g[7] = {0x00,0x00,0x16,0x19,0x10,0x10,0x10}; memcpy(glyph,g,7); break; }
        case 's': { u8 g[7] = {0x00,0x00,0x0F,0x10,0x0E,0x01,0x1E}; memcpy(glyph,g,7); break; }
        case 't': { u8 g[7] = {0x08,0x08,0x1C,0x08,0x08,0x09,0x06}; memcpy(glyph,g,7); break; }
        case 'u': { u8 g[7] = {0x00,0x00,0x11,0x11,0x11,0x13,0x0D}; memcpy(glyph,g,7); break; }
        case 'v': { u8 g[7] = {0x00,0x00,0x11,0x11,0x11,0x0A,0x04}; memcpy(glyph,g,7); break; }
        case 'w': { u8 g[7] = {0x00,0x00,0x11,0x15,0x15,0x15,0x0A}; memcpy(glyph,g,7); break; }
        case 'x': { u8 g[7] = {0x00,0x00,0x11,0x0A,0x04,0x0A,0x11}; memcpy(glyph,g,7); break; }
        case 'y': { u8 g[7] = {0x00,0x00,0x11,0x11,0x0F,0x01,0x0E}; memcpy(glyph,g,7); break; }
        case 'z': { u8 g[7] = {0x00,0x00,0x1F,0x02,0x04,0x08,0x1F}; memcpy(glyph,g,7); break; }
        case '0': { u8 g[7] = {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}; memcpy(glyph,g,7); break; }
        case '1': { u8 g[7] = {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}; memcpy(glyph,g,7); break; }
        case '2': { u8 g[7] = {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}; memcpy(glyph,g,7); break; }
        case '3': { u8 g[7] = {0x1E,0x01,0x01,0x0E,0x01,0x01,0x1E}; memcpy(glyph,g,7); break; }
        case '4': { u8 g[7] = {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}; memcpy(glyph,g,7); break; }
        case '5': { u8 g[7] = {0x1F,0x10,0x10,0x1E,0x01,0x01,0x1E}; memcpy(glyph,g,7); break; }
        case '6': { u8 g[7] = {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}; memcpy(glyph,g,7); break; }
        case '7': { u8 g[7] = {0x1F,0x01,0x02,0x04,0x08,0x08,0x08}; memcpy(glyph,g,7); break; }
        case '8': { u8 g[7] = {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}; memcpy(glyph,g,7); break; }
        case '9': { u8 g[7] = {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}; memcpy(glyph,g,7); break; }
        case '.': { u8 g[7] = {0,0,0,0,0,0x0C,0x0C}; memcpy(glyph,g,7); break; }
        case '-': { u8 g[7] = {0,0,0,0x1F,0,0,0}; memcpy(glyph,g,7); break; }
        case ':': { u8 g[7] = {0,0x04,0,0,0x04,0,0}; memcpy(glyph,g,7); break; }
        case '(': { u8 g[7] = {0x02,0x04,0x08,0x08,0x08,0x04,0x02}; memcpy(glyph,g,7); break; }
        case ')': { u8 g[7] = {0x08,0x04,0x02,0x02,0x02,0x04,0x08}; memcpy(glyph,g,7); break; }
        case '/': { u8 g[7] = {0x01,0x01,0x02,0x04,0x08,0x10,0x10}; memcpy(glyph,g,7); break; }
        case '+': { u8 g[7] = {0,0x04,0x04,0x1F,0x04,0x04,0}; memcpy(glyph,g,7); break; }
        case '!': { u8 g[7] = {0x04,0x04,0x04,0x04,0x04,0,0x04}; memcpy(glyph,g,7); break; }
        case '?': { u8 g[7] = {0x0E,0x11,0x01,0x02,0x04,0,0x04}; memcpy(glyph,g,7); break; }
        default: { u8 g[7] = {0x1F,0x11,0x15,0x15,0x11,0x11,0x1F}; memcpy(glyph,g,7); break; }
    }
    for (int gy = 0; gy < 7; gy++)
        for (int gx = 0; gx < 5; gx++)
            if (glyph[gy] & (1 << (4 - gx)))
                putScreenPixel(fb, width, height, x + gx, y + gy, r, g, b);
}

static void drawMiniText(u8 *fb, int width, int height, int x, int y, const char *text, u8 r, u8 g, u8 b)
{
    int cx = x;
    while (*text)
    {
        char c = *text;
        if (c != ' ') drawMiniGlyph(fb, width, height, cx, y, c, r, g, b);
        cx += 6;
        text++;
    }
}

static void drawStatusPanel(gfxScreen_t screen, const char *title, const char *line, int progress, int total)
{
    u16 w, h;
    u8 *fb = gfxGetFramebuffer(screen, GFX_LEFT, &w, &h);
    int screenWidth = h;
    int screenHeight = w;
    fillRect(fb, w, h, 0, 0, screenWidth, screenHeight, 24, 33, 38);
    drawMiniText(fb, w, h, 34, 70, title, 245, 248, 250);
    drawMiniText(fb, w, h, 34, 96, line, 180, 202, 212);
    int barWidth = std::max(32, screenWidth - 68);
    fillRect(fb, w, h, 34, 128, barWidth, 16, 58, 72, 80);
    if (total > 0)
    {
        int filled = std::max(0, std::min(barWidth, progress * barWidth / total));
        fillRect(fb, w, h, 34, 128, filled, 16, 13, 122, 117);
    }

    if (screen == GFX_TOP)
    {
        u16 rw, rh;
        u8 *right = gfxGetFramebuffer(GFX_TOP, GFX_RIGHT, &rw, &rh);
        if (right && rw == w && rh == h)
            memcpy(right, fb, (size_t)w * h * 3);
    }
}

static void drawStatusScreen(const char *title, const char *line, int progress, int total)
{
    drawStatusPanel(GFX_BOTTOM, title, line, progress, total);
    gfxFlushBuffers();
    gfxScreenSwapBuffers(GFX_BOTTOM, false);
    gspWaitForVBlank();
}

#if UPDATER_ENABLED
static void drawPromptPanel(gfxScreen_t screen, const char *title, const char *line)
{
    u16 w, h;
    u8 *fb = gfxGetFramebuffer(screen, GFX_LEFT, &w, &h);
    int screenWidth = h;
    int screenHeight = w;
    fillRect(fb, w, h, 0, 0, screenWidth, screenHeight, 24, 33, 38);
    drawMiniText(fb, w, h, 26, 70, title, 245, 248, 250);
    drawMiniText(fb, w, h, 26, 96, line, 180, 202, 212);
    drawMiniText(fb, w, h, 26, 138, "A DOWNLOAD", 94, 234, 212);
    drawMiniText(fb, w, h, 26, 160, "B CANCEL", 255, 115, 115);
}

static void drawPromptScreen(const char *title, const char *line)
{
    drawPromptPanel(GFX_BOTTOM, title, line);
    gfxFlushBuffers();
    gfxScreenSwapBuffers(GFX_BOTTOM, false);
    gspWaitForVBlank();
}

static bool waitForUpdateConfirm(const UpdateManifest &manifest)
{
    while (aptMainLoop())
    {
        drawPromptScreen("Update available", manifest.latestVersion);
        hidScanInput();
        u32 down = hidKeysDown();
        if (down & KEY_A) return true;
        if (down & KEY_B) return false;
    }
    return false;
}

static void updateProgress(int downloaded, int total, void *)
{
    if (downloaded >= 1000000)
        drawStatusScreen("Installing update", "Please wait", downloaded - 1000000, total);
    else
        drawStatusScreen("Downloading update", "Please wait", downloaded, total);
}

static void exitAfterUpdateInstalled(const char *packageType, u64 titleId)
{
    if (packageType && strcmp(packageType, "cia") == 0 && titleId != 0)
    {
        drawStatusScreen("Update ready", "Relaunching", 1, 1);
        if (Updater::relaunchInstalledTitle(titleId))
            return;
    }

    while (aptMainLoop())
    {
        drawStatusScreen("Update ready", "A: Close and reopen app", 1, 1);
        hidScanInput();
        u32 down = hidKeysDown();
        if ((down & KEY_A) || (down & KEY_B))
        {
            break;
        }
    }
    gfxExit();
    exit(0);
}

struct HttpsUpdateAttempt
{
    bool manifestFetched;
    bool updateAvailable;
    bool accepted;
    UpdateDownloadResult result;
};

static HttpsUpdateAttempt offerHttpsUpdate(const char *packageType, const char *updateTargetPath,
                                           u64 installedTitleId)
{
    HttpsUpdateAttempt attempt = {false, false, false, UPDATE_DOWNLOAD_MANIFEST_FAILED};
    UpdateManifest manifest;
    if (!Updater::fetchManifest(SERVER_HTTPS_HOST, SERVER_HTTPS_PORT, APP_VERSION,
                                packageType, manifest))
        return attempt;

    attempt.manifestFetched = true;
    attempt.updateAvailable = manifest.available;
    if (!manifest.available)
    {
        attempt.result = UPDATE_DOWNLOAD_NO_UPDATE;
        return attempt;
    }

    attempt.accepted = waitForUpdateConfirm(manifest);
    if (!attempt.accepted)
    {
        attempt.result = UPDATE_DOWNLOAD_NO_UPDATE;
        return attempt;
    }

    if (strcmp(packageType, "cia") == 0)
    {
        attempt.result = Updater::downloadAndInstallCia(
            SERVER_HTTPS_HOST, SERVER_HTTPS_PORT, manifest, updateTargetPath,
            installedTitleId, updateProgress, NULL);
    }
    else
    {
        attempt.result = Updater::downloadUpdate(
            SERVER_HTTPS_HOST, SERVER_HTTPS_PORT, manifest, updateTargetPath,
            updateProgress, NULL);
    }
    return attempt;
}
#endif

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

static void putBufferScreenPixel(u8 *buffer, int fbWidth, int fbHeight, int screenX, int screenY, u8 r, u8 g, u8 b)
{
    const bool rotatedFramebuffer = fbWidth < fbHeight;
    int fbX = rotatedFramebuffer ? fbWidth - 1 - screenY : screenX;
    int fbY = rotatedFramebuffer ? screenX : screenY;
    if (!buffer || fbX < 0 || fbX >= fbWidth || fbY < 0 || fbY >= fbHeight)
        return;

    int idx = 3 * (fbY * fbWidth + fbX);
    buffer[idx] = b;
    buffer[idx + 1] = g;
    buffer[idx + 2] = r;
}

static void fillBufferScreenRect(u8 *buffer, int fbWidth, int fbHeight, int x, int y, int w, int h, u8 r, u8 g, u8 b)
{
    for (int py = y; py < y + h; py++)
        for (int px = x; px < x + w; px++)
            putBufferScreenPixel(buffer, fbWidth, fbHeight, px, py, r, g, b);
}

static void strokeBufferScreenRect(u8 *buffer, int fbWidth, int fbHeight, int x, int y, int w, int h, u8 r, u8 g, u8 b)
{
    for (int px = x; px < x + w; px++)
    {
        putBufferScreenPixel(buffer, fbWidth, fbHeight, px, y, r, g, b);
        putBufferScreenPixel(buffer, fbWidth, fbHeight, px, y + h - 1, r, g, b);
    }
    for (int py = y; py < y + h; py++)
    {
        putBufferScreenPixel(buffer, fbWidth, fbHeight, x, py, r, g, b);
        putBufferScreenPixel(buffer, fbWidth, fbHeight, x + w - 1, py, r, g, b);
    }
}

static int zoomOverlayX(bool leftSide)
{
    return leftSide ? 8 : 270;
}

static void drawZoomOverlay(u8 *buffer, int fbWidth, int fbHeight, bool leftSide)
{
    const int x = zoomOverlayX(leftSide);
    const int upY = 42;
    const int downY = 144;
    const int w = 42;
    const int h = 58;

    fillBufferScreenRect(buffer, fbWidth, fbHeight, x, upY, w, h, 24, 33, 38);
    fillBufferScreenRect(buffer, fbWidth, fbHeight, x, downY, w, h, 24, 33, 38);
    strokeBufferScreenRect(buffer, fbWidth, fbHeight, x, upY, w, h, 245, 248, 250);
    strokeBufferScreenRect(buffer, fbWidth, fbHeight, x, downY, w, h, 245, 248, 250);

    fillBufferScreenRect(buffer, fbWidth, fbHeight, x + 12, upY + 27, 18, 4, 94, 234, 212);
    fillBufferScreenRect(buffer, fbWidth, fbHeight, x + 19, upY + 20, 4, 18, 94, 234, 212);
    fillBufferScreenRect(buffer, fbWidth, fbHeight, x + 12, downY + 27, 18, 4, 255, 115, 115);
}

static bool pointInRect(int px, int py, int x, int y, int w, int h)
{
    return px >= x && px < x + w && py >= y && py < y + h;
}

static int brushSizeForShapeRow(int shape, int row)
{
    static const int normalSizes[] = {1, 2, 3, 5, 7};
    static const int largeSizes[] = {3, 5, 7, 9, 12};
    row = std::max(0, std::min(4, row));
    return (shape == BRUSH_DITHER || shape == BRUSH_ERASER) ? largeSizes[row] : normalSizes[row];
}

static int clampBrushSizeForShape(int shape, int size)
{
    if (shape == BRUSH_DITHER || shape == BRUSH_ERASER)
        return std::max(3, std::min(12, size));
    return std::max(1, std::min(7, size));
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

static Color effectiveDrawColor()
{
    if (currentBrushShape == BRUSH_ERASER)
    {
        Color white = {255, 255, 255};
        return white;
    }
    if (gRainbowEnabled && gRainbowStrokeColorValid)
        return gRainbowStrokeColor;
    return currentColor;
}

static Color rainbowColorForPoint(int x, int y, u64 nowMs)
{
    int hueDegrees = (int)((((nowMs / 110ULL) * 28ULL) + (u64)std::max(0, x) / 18ULL + (u64)std::max(0, y) / 24ULL) % 360ULL);
    float h = (float)hueDegrees / 60.0f;
    const float s = 0.92f;
    const float l = 0.52f;
    float c = (1.0f - fabsf(2.0f * l - 1.0f)) * s;
    float xx = c * (1.0f - fabsf(fmodf(h, 2.0f) - 1.0f));
    float r1 = 0.0f, g1 = 0.0f, b1 = 0.0f;
    if (h < 1.0f) { r1 = c; g1 = xx; }
    else if (h < 2.0f) { r1 = xx; g1 = c; }
    else if (h < 3.0f) { g1 = c; b1 = xx; }
    else if (h < 4.0f) { g1 = xx; b1 = c; }
    else if (h < 5.0f) { r1 = xx; b1 = c; }
    else { r1 = c; b1 = xx; }
    float m = l - c * 0.5f;
    Color color = {
        (u8)std::max(0, std::min(255, (int)std::round((r1 + m) * 255.0f))),
        (u8)std::max(0, std::min(255, (int)std::round((g1 + m) * 255.0f))),
        (u8)std::max(0, std::min(255, (int)std::round((b1 + m) * 255.0f))),
    };
    return color;
}

static bool sameColor(const Color &a, const Color &b)
{
    return a.r == b.r && a.g == b.g && a.b == b.b;
}

static int effectiveBrushShape()
{
    return currentBrushShape == BRUSH_ERASER ? BRUSH_CIRCLE : currentBrushShape;
}

u8 clampColor(float colorValue)
{
    return static_cast<u8>(std::max(0.0f, std::min(255.0f, colorValue)));
}

void drawBrush(u8 *buffer, int fbWidth, int fbHeight, int centerX, int centerY, int size, int shape, u8 r, u8 g, u8 b)
{
    static const int bayer4[4][4] = {
        {0, 8, 2, 10},
        {12, 4, 14, 6},
        {3, 11, 1, 9},
        {15, 7, 13, 5},
    };
    int radius = std::max(1, size / 2);
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
            else if (shape == 2)
            {
                int dist2 = x * x + y * y;
                if (dist2 > radius * radius)
                    continue;
                float dist = sqrtf((float)dist2);
                float coverage = 1.0f - (dist / (float)(radius + 1));
                int threshold = bayer4[(centerY + y) & 3][(centerX + x) & 3];
                if ((int)(coverage * 16.0f) > threshold)
                    drawPointOnBuffer(buffer, fbWidth, fbHeight, centerX + x, centerY + y, r, g, b);
            }
        }
    }
}

static void drawStrokeSample(u8 *fullCanvas, int canvasWidth, int canvasHeight,
                             int screenX, int screenY, CanvasState &canvas)
{
    int canvasX = canvas.screenToCanvasX(screenX);
    int canvasY = canvas.screenToCanvasY(screenY);
    if (canvasX < 0 || canvasX >= canvasWidth || canvasY < 0 || canvasY >= canvasHeight)
        return;

    Color drawColor = effectiveDrawColor();
    drawBrush(fullCanvas, canvasWidth, canvasHeight, canvasX, canvasY,
              currentBrushSize, effectiveBrushShape(),
              drawColor.r, drawColor.g, drawColor.b);
    canvas.markDirty(canvasX, canvasY, currentBrushSize);
    UIState::addPoint(canvasX, canvasY);
}

static void drawStrokeLine(u8 *fullCanvas, int canvasWidth, int canvasHeight,
                           float x0, float y0, float x1, float y1,
                           CanvasState &canvas)
{
    float zoom = canvas.zoomScale();
    float cx0 = (float)canvas.offsetX + x0 / zoom;
    float cy0 = (float)canvas.offsetY + y0 / zoom;
    float cx1 = (float)canvas.offsetX + x1 / zoom;
    float cy1 = (float)canvas.offsetY + y1 / zoom;
    int steps = std::max(1, (int)std::ceil(std::max(fabsf(cx1 - cx0), fabsf(cy1 - cy0))));
    for (int i = 0; i <= steps; i++)
    {
        float t = (float)i / (float)steps;
        int x = (int)std::round(cx0 + (cx1 - cx0) * t);
        int y = (int)std::round(cy0 + (cy1 - cy0) * t);
        if (x < 0 || x >= canvasWidth || y < 0 || y >= canvasHeight)
            continue;
        Color drawColor = effectiveDrawColor();
        drawBrush(fullCanvas, canvasWidth, canvasHeight, x, y,
                  currentBrushSize, effectiveBrushShape(),
                  drawColor.r, drawColor.g, drawColor.b);
        canvas.markDirty(x, y, currentBrushSize);
        UIState::addPoint(x, y);
    }
}

static void drawStrokeCurve(u8 *fullCanvas, int canvasWidth, int canvasHeight,
                            float x0, float y0, float cx, float cy, float x1, float y1,
                            CanvasState &canvas)
{
    float zoom = canvas.zoomScale();
    float c0x = (float)canvas.offsetX + x0 / zoom;
    float c0y = (float)canvas.offsetY + y0 / zoom;
    float ccx = (float)canvas.offsetX + cx / zoom;
    float ccy = (float)canvas.offsetY + cy / zoom;
    float c1x = (float)canvas.offsetX + x1 / zoom;
    float c1y = (float)canvas.offsetY + y1 / zoom;
    float lengthEstimate = hypotf(ccx - c0x, ccy - c0y) + hypotf(c1x - ccx, c1y - ccy);
    int steps = std::max(2, (int)std::ceil(lengthEstimate * 1.25f));
    for (int i = 0; i <= steps; i++)
    {
        float t = (float)i / (float)steps;
        float inv = 1.0f - t;
        int x = (int)std::round(inv * inv * c0x + 2.0f * inv * t * ccx + t * t * c1x);
        int y = (int)std::round(inv * inv * c0y + 2.0f * inv * t * ccy + t * t * c1y);
        if (x < 0 || x >= canvasWidth || y < 0 || y >= canvasHeight)
            continue;
        Color drawColor = effectiveDrawColor();
        drawBrush(fullCanvas, canvasWidth, canvasHeight, x, y,
                  currentBrushSize, effectiveBrushShape(),
                  drawColor.r, drawColor.g, drawColor.b);
        canvas.markDirty(x, y, currentBrushSize);
        UIState::addPoint(x, y);
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

static bool processRectPacket(const uint8_t *packet, size_t length, CanvasState &canvas)
{
    if (length < 12 || packet[0] != 2)
        return false;

    Color color = { packet[1], packet[2], packet[3] };
    uint16_t x = *(uint16_t *)(packet + 4);
    uint16_t y = *(uint16_t *)(packet + 6);
    uint16_t w = *(uint16_t *)(packet + 8);
    uint16_t h = *(uint16_t *)(packet + 10);
    applyCanvasRectLocal(canvas, x, y, w, h, color);
    return true;
}

static void upsertDrawLabel(ActiveDrawLabel *labels, const char *name, int canvasX, int canvasY)
{
    if (!labels || !name || !name[0])
        return;

    int slot = -1;
    bool matchedExisting = false;
    u64 oldest = UINT64_MAX;
    u64 now = osGetTime();
    for (int i = 0; i < MAX_ACTIVE_DRAW_LABELS; i++)
    {
        if (labels[i].active && strncmp(labels[i].name, name, sizeof(labels[i].name)) == 0)
        {
            slot = i;
            matchedExisting = true;
            break;
        }
        if (!labels[i].active && slot < 0)
            slot = i;
        if (labels[i].updatedAt < oldest)
        {
            oldest = labels[i].updatedAt;
            if (slot < 0)
                slot = i;
        }
    }

    if (slot < 0)
        slot = 0;
    labels[slot].active = true;
    snprintf(labels[slot].name, sizeof(labels[slot].name), "%s", name);
    labels[slot].canvasX = canvasX;
    labels[slot].canvasY = canvasY;
    if (!matchedExisting || !labels[slot].hasDisplay)
    {
        labels[slot].displayX = (float)canvasX;
        labels[slot].displayY = (float)canvasY;
        labels[slot].hasDisplay = true;
    }
    labels[slot].updatedAt = now;
}

static bool processDrawAttributionPacket(const uint8_t *packet, size_t length, ActiveDrawLabel *labels)
{
    if (!packet || length < 6 || packet[0] != 3)
        return false;
    uint16_t x = *(uint16_t *)(packet + 1);
    uint16_t y = *(uint16_t *)(packet + 3);
    uint8_t nameLen = packet[5];
    if (length < (size_t)(6 + nameLen))
        return false;

    char name[25];
    size_t copyLen = std::min((size_t)nameLen, sizeof(name) - 1);
    memcpy(name, packet + 6, copyLen);
    name[copyLen] = '\0';
    upsertDrawLabel(labels, name, x, y);
    return true;
}

static bool processBinaryCanvasPackets(const uint8_t *packets, size_t length, CanvasState &canvas,
                                       u8 *fullCanvas, int canvasWidth, int canvasHeight,
                                       ActiveDrawLabel *labels)
{
    if (!packets || length == 0)
        return false;
    uint8_t type = packets[0];
    if (type == 1)
    {
        if (length < 7 || length != 7 + (size_t)packets[6] * 4)
            return false;
        processDrawPacket(packets, length, NULL, 0, 0, fullCanvas, canvasWidth, canvasHeight);
        return true;
    }
    if (type == 2)
        return length == 12 && processRectPacket(packets, length, canvas);
    if (type == 3)
    {
        if (length < 6 || length != 6 + (size_t)packets[5])
            return false;
        processDrawAttributionPacket(packets, length, labels);
    }
    return false;
}

static void drawActiveDrawLabels(u8 *buffer, int fbWidth, int fbHeight, CanvasState &canvas, ActiveDrawLabel *labels)
{
    if (!labels)
        return;
    u64 now = osGetTime();
    float zoom = canvas.zoomScale();
    for (int i = 0; i < MAX_ACTIVE_DRAW_LABELS; i++)
    {
        if (!labels[i].active)
            continue;
        if (now - labels[i].updatedAt > DRAW_LABEL_TTL_MS)
        {
            labels[i].active = false;
            continue;
        }
        if (!labels[i].hasDisplay)
        {
            labels[i].displayX = (float)labels[i].canvasX;
            labels[i].displayY = (float)labels[i].canvasY;
            labels[i].hasDisplay = true;
        }
        labels[i].displayX += ((float)labels[i].canvasX - labels[i].displayX) * 0.28f;
        labels[i].displayY += ((float)labels[i].canvasY - labels[i].displayY) * 0.28f;

        int screenX = (int)std::round((labels[i].displayX - canvas.offsetX) * zoom) + 7;
        int screenY = (int)std::round((labels[i].displayY - canvas.offsetY) * zoom) - 8;
        int textW = (int)strlen(labels[i].name) * 6;
        screenX = std::max(2, std::min(318 - textW, screenX));
        screenY = std::max(2, std::min(230, screenY));
        drawMiniText(buffer, fbWidth, fbHeight, screenX, screenY, labels[i].name, 13, 122, 117);
    }
}

void drawBrushSizeSelector(u8 *framebuffer, int screenWidth, int screenHeight)
{
    std::vector<int> brushSizes = {1, 2, 3, 5, 7, 9, 12};
    std::vector<int> brushPositions = {24, 46, 68, 90, 112, 134, 156};

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

static void sendDrawBatchCommand(const std::vector<DrawPoint> &points, const Color &color, int size, int shape)
{
    if (!NetworkManager::checkConnection() || points.empty())
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

        if (!NetworkManager::sendBinary(packet, 7 + count * 4))
        {
            printf("Failed to send draw batch.\n");
            return;
        }

        if (start + count >= points.size())
            break;
        start += count - 1; // Overlap one point so remote clients keep a continuous line.
    }
}

static void drawPaletteButton(u8 *framebuffer, int fbWidth, int fbHeight, int x, int y, int w, int h,
                              const char *label, bool active, bool danger)
{
    u8 r = active ? 24 : (danger ? 196 : 230);
    u8 g = active ? 33 : (danger ? 61 : 235);
    u8 b = active ? 38 : (danger ? 61 : 240);
    fillBufferScreenRect(framebuffer, fbWidth, fbHeight, x, y, w, h, r, g, b);
    strokeBufferScreenRect(framebuffer, fbWidth, fbHeight, x, y, w, h, 104, 114, 124);
    drawMiniText(framebuffer, fbWidth, fbHeight, x + 14, y + (h / 2) - 3, label,
                 active || danger ? 245 : 32, active || danger ? 248 : 36, active || danger ? 250 : 42);
}

static void drawPaletteBrushChoice(u8 *framebuffer, int fbWidth, int fbHeight, int x, int y, int size, int shape, bool selected)
{
    if (selected)
    {
        fillBufferScreenRect(framebuffer, fbWidth, fbHeight, x - 13, y - 13, 26, 26, 94, 234, 212);
        fillBufferScreenRect(framebuffer, fbWidth, fbHeight, x - 10, y - 10, 20, 20, 220, 226, 232);
    }
    int radius = std::max(2, size + 2);
    for (int py = -radius; py <= radius; py++)
    {
        for (int px = -radius; px <= radius; px++)
        {
            bool draw = shape == BRUSH_SQUARE || (px * px + py * py <= radius * radius);
            if (shape == 2 && (abs(px) + abs(py)) % 2 == 1)
                draw = false;
            if (draw)
                putBufferScreenPixel(framebuffer, fbWidth, fbHeight, x + px, y + py,
                                     shape == BRUSH_ERASER ? 255 : 24,
                                     shape == BRUSH_ERASER ? 255 : 33,
                                     shape == BRUSH_ERASER ? 255 : 38);
        }
    }
    if (shape == BRUSH_ERASER)
    {
        strokeBufferScreenRect(framebuffer, fbWidth, fbHeight, x - radius, y - radius, radius * 2 + 1, radius * 2 + 1, 196, 61, 61);
        for (int i = -radius; i <= radius; i++)
            putBufferScreenPixel(framebuffer, fbWidth, fbHeight, x + i, y - i, 196, 61, 61);
    }
}

static void drawColorSquare(u8 *framebuffer, int fbWidth, int fbHeight, int x, int y, int w, int h,
                            float hue, float saturation, float value)
{
    for (int py = 0; py < h; py++)
    {
        float v = 1.0f - ((float)py / (float)std::max(1, h - 1));
        for (int px = 0; px < w; px++)
        {
            float s = (float)px / (float)std::max(1, w - 1);
            float rr, gg, bb;
            UIState::HSVtoRGB(hue, s, v, rr, gg, bb);
            putBufferScreenPixel(framebuffer, fbWidth, fbHeight, x + px, y + py,
                                 (u8)(rr * 255.0f), (u8)(gg * 255.0f), (u8)(bb * 255.0f));
        }
    }
    strokeBufferScreenRect(framebuffer, fbWidth, fbHeight, x, y, w, h, 24, 33, 38);
    int knobX = x + std::max(0, std::min(w - 1, (int)(saturation * (float)(w - 1))));
    int knobY = y + std::max(0, std::min(h - 1, (int)((1.0f - value) * (float)(h - 1))));
    strokeBufferScreenRect(framebuffer, fbWidth, fbHeight, knobX - 4, knobY - 4, 9, 9, 245, 248, 250);
    strokeBufferScreenRect(framebuffer, fbWidth, fbHeight, knobX - 3, knobY - 3, 7, 7, 24, 33, 38);
}

static void drawHueStrip(u8 *framebuffer, int fbWidth, int fbHeight, int x, int y, int w, int h, float hue)
{
    for (int px = 0; px < w; px++)
    {
        float hh = (float)px / (float)std::max(1, w - 1);
        float rr, gg, bb;
        UIState::HSVtoRGB(hh, 1.0f, 1.0f, rr, gg, bb);
        fillBufferScreenRect(framebuffer, fbWidth, fbHeight, x + px, y, 1, h,
                             (u8)(rr * 255.0f), (u8)(gg * 255.0f), (u8)(bb * 255.0f));
    }
    strokeBufferScreenRect(framebuffer, fbWidth, fbHeight, x, y, w, h, 24, 33, 38);
    int knobX = x + std::max(0, std::min(w - 1, (int)(hue * (float)(w - 1))));
    fillBufferScreenRect(framebuffer, fbWidth, fbHeight, knobX - 2, y - 3, 5, h + 6, 245, 248, 250);
    strokeBufferScreenRect(framebuffer, fbWidth, fbHeight, knobX - 2, y - 3, 5, h + 6, 24, 33, 38);
}

static void drawToolPalette(u8 *framebuffer, int fbWidth, int fbHeight, int activeTab, bool modAllowed,
                            Color color, const char *notice)
{
    fillBufferScreenRect(framebuffer, fbWidth, fbHeight, 8, 8, 304, 224, 220, 226, 232);
    strokeBufferScreenRect(framebuffer, fbWidth, fbHeight, 8, 8, 304, 224, 104, 114, 124);

    drawPaletteButton(framebuffer, fbWidth, fbHeight, 14, 14, 88, 28, "Color", activeTab == 0, false);
    if (modAllowed)
        drawPaletteButton(framebuffer, fbWidth, fbHeight, 106, 14, 88, 28, "Staff", activeTab == 1, false);

    if (activeTab == 0)
    {
        drawMiniText(framebuffer, fbWidth, fbHeight, 26, 52, "Brush", 73, 82, 92);
        const int shapeXs[] = {30, 60, 90, 120};
        const int sizeYs[] = {70, 92, 114, 136, 158};
        drawMiniText(framebuffer, fbWidth, fbHeight, 16, 184, "Cir", 73, 82, 92);
        drawMiniText(framebuffer, fbWidth, fbHeight, 48, 184, "Box", 73, 82, 92);
        drawMiniText(framebuffer, fbWidth, fbHeight, 78, 184, "Dit", 73, 82, 92);
        drawMiniText(framebuffer, fbWidth, fbHeight, 108, 184, "Erase", 196, 61, 61);
        for (int row = 0; row < 5; row++)
            for (int shape = 0; shape < 4; shape++)
                drawPaletteBrushChoice(framebuffer, fbWidth, fbHeight, shapeXs[shape], sizeYs[row],
                                       brushSizeForShapeRow(shape, row), shape,
                                       currentBrushSize == brushSizeForShapeRow(shape, row) && currentBrushShape == shape);

        float h, s, v;
        UIState::getHSV(h, s, v);
        drawColorSquare(framebuffer, fbWidth, fbHeight, 162, 54, 132, 132, h, s, v);
        drawHueStrip(framebuffer, fbWidth, fbHeight, 162, 198, 132, 14, h);
    }
    else
    {
        if (!modAllowed)
            return;
        drawPaletteButton(framebuffer, fbWidth, fbHeight, 22, 54, 132, 36, "SNAPSHOT", true, false);
        drawPaletteButton(framebuffer, fbWidth, fbHeight, 166, 54, 132, 36, "CLEAR", false, true);
        drawPaletteButton(framebuffer, fbWidth, fbHeight, 22, 102, 276, 36, "FILL RECT", true, false);
        drawPaletteButton(framebuffer, fbWidth, fbHeight, 22, 150, 276, 34,
                          gRainbowEnabled ? "RAINBOW ON" : "RAINBOW OFF", gRainbowEnabled, false);
        drawMiniText(framebuffer, fbWidth, fbHeight, 24, 204, notice && notice[0] ? notice : "STAFF DRAWING TOOLS", 32, 36, 42);
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

static bool promptDisplayName(IdentityInfo &identityInfo, char *displayName, size_t displayNameSize)
{
    SwkbdState swkbd;
    char inputText[25];
    snprintf(inputText, sizeof(inputText), "%.24s", identityInfo.displayName[0] ? identityInfo.displayName : gIdentity.displayName);

    swkbdInit(&swkbd, SWKBD_TYPE_NORMAL, 2, 24);
    swkbdSetHintText(&swkbd, "Display name");
    swkbdSetInitialText(&swkbd, inputText);

    if (swkbdInputText(&swkbd, inputText, sizeof(inputText)) != SWKBD_BUTTON_CONFIRM)
        return false;

    trimLine(inputText);
    if (!inputText[0])
        return false;

    if (!displayName || displayNameSize == 0)
        return false;
    snprintf(displayName, displayNameSize, "%s", inputText);
    return true;
}

static bool readKeyboardText(const char *hint, char *out, size_t outSize, const char *initial = "")
{
    SwkbdState swkbd;
    if (!out || outSize == 0)
        return false;
    snprintf(out, outSize, "%s", initial ? initial : "");
    swkbdInit(&swkbd, SWKBD_TYPE_NORMAL, 2, (int)outSize - 1);
    swkbdSetHintText(&swkbd, hint);
    swkbdSetInitialText(&swkbd, out);
    if (swkbdInputText(&swkbd, out, outSize) != SWKBD_BUTTON_CONFIRM)
        return false;
    trimLine(out);
    return out[0] != '\0';
}

static bool requestBackupCode()
{
    if (!NetworkManager::checkConnection())
        return false;
    char command[48];
    Protocol::buildRotateBackupCode(command, sizeof(command));
    return NetworkManager::sendText(command);
}

static bool recoverIdentity()
{
    if (!NetworkManager::checkConnection())
        return false;
    char username[25];
    char backupCode[32];
    const char *initialUsername = (strcmp(gIdentity.username, "pending") == 0) ? "" : gIdentity.username;
    if (!readKeyboardText("Account ID", username, sizeof(username), initialUsername))
        return false;
    if (!readKeyboardText("Backup code", backupCode, sizeof(backupCode), ""))
        return false;

    char command[280];
    Protocol::buildRecoverIdentity(command, sizeof(command), username, backupCode,
                                   gIdentity.deviceId, gIdentity.deviceSecret, gHardwareId);
    return NetworkManager::sendText(command);
}

static void applyIdentityAccepted(const IdentityInfo &identityInfo)
{
    if (identityInfo.username[0])
        snprintf(gIdentity.username, sizeof(gIdentity.username), "%s", identityInfo.username);
    if (identityInfo.displayName[0])
        snprintf(gIdentity.displayName, sizeof(gIdentity.displayName), "%s", identityInfo.displayName);
    saveDeviceIdentity();
}

static void applyBackupCode(const IdentityInfo &identityInfo)
{
    if (identityInfo.username[0])
        snprintf(gIdentity.username, sizeof(gIdentity.username), "%s", identityInfo.username);
    if (identityInfo.backupCode[0])
        snprintf(gIdentity.backupCode, sizeof(gIdentity.backupCode), "%s", identityInfo.backupCode);
    saveDeviceIdentity();
}

// Function to decompress data
int main(int argc, char **argv)
{
    gfxInitDefault();
    aptHookCookie aptCookie;
    aptHook(&aptCookie, handleAptEvent, NULL);
    if (R_SUCCEEDED(ptmuInit()))
        atexit(ptmuExit);
    if (R_SUCCEEDED(mcuHwcInit()))
        atexit(mcuHwcExit);
    gfxSetDoubleBuffering(GFX_TOP, false);
    UIState::init();
    chooseRandomDrawingColor();
    const char *appPath = (argc > 0 && argv && argv[0] && argv[0][0]) ? argv[0] : "sdmc:/3ds/CollabDoodle-current.3dsx";
    u64 installedTitleId = currentInstalledTitleId();
    const char *packageType = (installedTitleId != 0 || isCiaLaunch(appPath)) ? "cia" : "3dsx";
#if UPDATER_ENABLED
    const char *updateTargetPath = updateTargetPathForPackage(packageType, appPath);
#endif

    drawStatusScreen("Connecting securely", "This may take a few seconds", 0, 0);

    printf("3DS Collab Doodle\n");
    printf("%s\n", gBuildConfiguration);
    printf("Package: %s\n", packageType);
    initHardwareId();
    printf("Device model: %s\n", gDeviceModel);
    loadDeviceIdentity();
    printf("Identity: %s\n", gIdentity.deviceId);
    printf("Hardware: %s\n", gHardwareId);
    printf("Identity boot: %s\n", gIdentityBootStatus);

    if (!NetworkManager::initialize())
    {
        failExit("Network services failed to initialize.");
    }
    atexit(NetworkManager::shutdown);

    if (!NetworkManager::waitForConnected(35000))
    {
        char connectionError[160];
        snprintf(connectionError, sizeof(connectionError), "%s", NetworkManager::lastError());
        NetworkManager::disconnect();
#if UPDATER_ENABLED
        // A failed WSS attempt may still be inside its bounded TLS handshake.
        // Let the worker close it before opening the updater's TLS connection,
        // avoiding two heavyweight TLS sessions on memory-constrained systems.
        if (!NetworkManager::waitForDisconnected(35000))
            failExit("Secure connection worker did not stop.\nRestart Collab Doodle and try again.");
        // The updater deliberately remains reachable even when the realtime
        // endpoint or WebSocket protocol is broken. This prevents future
        // transport migrations from stranding an installed client.
        drawStatusScreen("Connection failed", "Checking for an update", 0, 0);
        HttpsUpdateAttempt updateAttempt = offerHttpsUpdate(packageType, updateTargetPath, installedTitleId);
        if (updateAttempt.result == UPDATE_DOWNLOAD_OK)
            exitAfterUpdateInstalled(packageType, installedTitleId);
        if (strcmp(packageType, "cia") == 0 && updateAttempt.result == UPDATE_DOWNLOAD_INSTALL_FAILED)
            failExit("CIA install failed.\n%s", Updater::lastError());
#endif
        failExit("Secure connection failed.\n%s\nUpdate recovery: %s",
                 connectionError, Updater::lastError());
        return 1;
    }

    if (!sendClientHello(packageType))
    {
        failExit("Failed to send client hello.");
    }
    printf("Client hello sent: %s\n", APP_VERSION);

    std::vector<int> brushSizes = {1, 2, 3, 5, 7, 9, 12}; // Define your brush sizes
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
           "- SELECT: Menu\n"
           "- Menu > Channels: Switch channel\n"
           "- Circle Pad or hold LEFT/A + stylus: Pan viewport\n"
           "- DOWN/B: Toggle Color Picker\n"
           "- Hold UP/X + tap: Sample color\n"
           "- Hold L/R: Temporary eraser\n"
           "- Hold RIGHT/Y + touch: Zoom buttons\n");

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
    int selectedMenuItem = 0;
    int selectedAdminItem = 0;
    int pickerTab = 0;
    bool shoulderEraserActive = false;
    int shoulderSavedBrushShape = currentBrushShape;
    int shoulderSavedBrushSize = currentBrushSize;
    char availableChannels[8][25];
    int availableChannelCount = 0;
    PresenceUser connectedUsers[24];
    int connectedUserCount = 0;
    IdentityInfo identityInfo;
    memset(&identityInfo, 0, sizeof(identityInfo));
    snprintf(identityInfo.displayName, sizeof(identityInfo.displayName), "%.24s", gIdentity.displayName);
    snprintf(identityInfo.username, sizeof(identityInfo.username), "%s", gIdentity.username);
    snprintf(identityInfo.role, sizeof(identityInfo.role), "user");
    snprintf(identityInfo.status, sizeof(identityInfo.status), "active");
    snprintf(identityInfo.backupCode, sizeof(identityInfo.backupCode), "%s", gIdentity.backupCode);
    char identityNotice[40] = "";
    int identityNoticeFrames = 0;
    char adminNotice[40] = "";
    int adminNoticeFrames = 0;
    bool supportOnlyMode = false;
    char supportOnlyReasonText[81] = "";
    char supportOnlyBlockTypes[24] = "";
    SupportTicketSummary ticketList[6];
    memset(ticketList, 0, sizeof(ticketList));
    int ticketListCount = 0;
    int ticketSelected = 0;
    int ticketNextBeforeId = 0;
    bool ticketStaffScope = false;
    SupportTicketSummary activeTicket;
    memset(&activeTicket, 0, sizeof(activeTicket));
    SupportTicketMessage ticketMessages[6];
    memset(ticketMessages, 0, sizeof(ticketMessages));
    int ticketMessageCount = 0;
    int ticketNextBeforeMessageId = 0;
    StaffChatMessage staffChatMessages[8];
    memset(staffChatMessages, 0, sizeof(staffChatMessages));
    int staffChatMessageCount = 0;
    int staffChatNextBeforeId = 0;
    int ticketView = 0;
    int ticketHomeSelected = 0;
    int ticketActionSelected = 0;
    int ticketMineOpen = 0;
    int ticketStaffNeedsReply = 0;
    int staffChatUnread = 0;
    bool restrictionActive = false;
    bool restrictionHasDuration = false;
    int restrictionInitialSeconds = 0;
    int restrictionSecondsRemaining = 0;
    u64 restrictionStartedAt = 0;
    char restrictionReason[81] = "";
    char ticketNotice[64] = "";
    int ticketNoticeFrames = 0;
    enum AdminRectTool {
        ADMIN_RECT_NONE = 0,
        ADMIN_RECT_FILL = 1,
    };
    AdminRectTool pendingAdminRectTool = ADMIN_RECT_NONE;
    bool adminRectDragging = false;
    int adminRectStartX = 0;
    int adminRectStartY = 0;
    int adminRectEndX = 0;
    int adminRectEndY = 0;
    ActiveDrawLabel activeDrawLabels[MAX_ACTIVE_DRAW_LABELS];
    memset(activeDrawLabels, 0, sizeof(activeDrawLabels));
    bool exitRequested = false;
    OnboardingStage onboardingStage = ONBOARDING_READY;
    u64 onboardingSubmissionStartedAt = 0;
    bool onboardingStateQuerySent = false;
    int onboardingRetryCount = 0;
    char pendingDisplayName[25] = "";
    char pendingRulesVersion[32] = "";
    bool rulesRequireFreshAPress = false;
    bool rulesRenderedSinceKeyboard = false;

    auto isModOrAdmin = [&]() -> bool
    {
        return strcmp(identityInfo.role, "mod") == 0 || strcmp(identityInfo.role, "admin") == 0;
    };

    auto setIdentityNotice = [&](const char *notice)
    {
        snprintf(identityNotice, sizeof(identityNotice), "%s", notice ? notice : "");
        identityNoticeFrames = identityNotice[0] ? 180 : 0;
        topRenderFrame = 10;
    };

    auto setAdminNotice = [&](const char *notice)
    {
        snprintf(adminNotice, sizeof(adminNotice), "%.39s", notice ? notice : "");
        adminNoticeFrames = adminNotice[0] ? 180 : 0;
        topRenderFrame = 10;
    };

    auto setTicketNotice = [&](const char *notice)
    {
        snprintf(ticketNotice, sizeof(ticketNotice), "%.63s", notice ? notice : "");
        ticketNoticeFrames = ticketNotice[0] ? 240 : 0;
        topRenderFrame = 10;
    };

    auto clearOnboardingSubmission = [&]()
    {
        onboardingSubmissionStartedAt = 0;
        onboardingStateQuerySent = false;
        onboardingRetryCount = 0;
    };

    auto beginOnboardingSubmission = [&](OnboardingStage stage)
    {
        onboardingStage = stage;
        onboardingSubmissionStartedAt = osGetTime();
        onboardingStateQuerySent = false;
        onboardingRetryCount = 0;
    };

    auto sendDisplayNameValue = [&](const char *displayName) -> bool
    {
        if (!displayName || !displayName[0] || !NetworkManager::checkConnection())
            return false;
        char command[96];
        Protocol::buildSetDisplayName(command, sizeof(command), displayName);
        return NetworkManager::sendText(command);
    };

    auto sendRulesValue = [&](const char *version) -> bool
    {
        if (!version || !version[0] || !NetworkManager::checkConnection())
            return false;
        char command[96];
        Protocol::buildRulesAccepted(command, sizeof(command), version);
        return NetworkManager::sendText(command);
    };

    auto requestOnboardingState = [&]() -> bool
    {
        if (!NetworkManager::checkConnection())
            return false;
        char command[64];
        Protocol::buildGetOnboardingState(command, sizeof(command));
        return NetworkManager::sendText(command);
    };

    auto retryPendingOnboardingSubmission = [&]() -> bool
    {
        if (onboardingRetryCount >= 1)
            return false;

        bool sent = false;
        if (onboardingStage == ONBOARDING_SUBMITTING_DISPLAY_NAME)
            sent = sendDisplayNameValue(pendingDisplayName);
        else if (onboardingStage == ONBOARDING_SUBMITTING_RULES)
            sent = sendRulesValue(pendingRulesVersion);
        if (!sent)
            return false;

        onboardingRetryCount++;
        setIdentityNotice(onboardingStage == ONBOARDING_SUBMITTING_DISPLAY_NAME ? "RETRYING NAME" : "RETRYING RULES");
        return true;
    };

    auto sendTicketCommand = [&](const char *command) -> bool
    {
        if (!command || !command[0] || !NetworkManager::checkConnection())
        {
            setTicketNotice("TICKET CONNECTION FAILED");
            return false;
        }
        if (!NetworkManager::sendText(command))
        {
            setTicketNotice("TICKET SEND FAILED");
            return false;
        }
        return true;
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

    auto handleJsonControl = [&](const char *jsonLine) -> bool
    {
        char currentChannel[25] = "";
        char recoveryReason[32] = "";
        char gateVersion[32] = "";
        char rejectionReason[48] = "";
        char disconnectReason[80] = "";
        char parsedSupportReason[81] = "";
        char parsedBlockTypes[24] = "";
        int parsedRestrictionSeconds = 0;
        if (Protocol::parseSupportOnly(jsonLine, parsedSupportReason, sizeof(parsedSupportReason), parsedBlockTypes, sizeof(parsedBlockTypes), parsedRestrictionSeconds))
        {
            supportOnlyMode = true;
            snprintf(supportOnlyReasonText, sizeof(supportOnlyReasonText), "%s", parsedSupportReason[0] ? parsedSupportReason : "Access restricted");
            snprintf(supportOnlyBlockTypes, sizeof(supportOnlyBlockTypes), "%s", parsedBlockTypes);
            restrictionActive = true;
            restrictionHasDuration = parsedRestrictionSeconds > 0;
            restrictionInitialSeconds = parsedRestrictionSeconds;
            restrictionStartedAt = osGetTime();
            snprintf(restrictionReason, sizeof(restrictionReason), "%s", supportOnlyReasonText);
            topMode = TOP_MODE_TICKETS;
            ticketView = 0;
            ticketHomeSelected = 0;
            topRenderFrame = 10;
            return true;
        }
        int parsedMineOpen = 0;
        int parsedStaffNeedsReply = 0;
        int parsedStaffChatUnread = 0;
        if (Protocol::parseTicketCounts(jsonLine, parsedMineOpen, parsedStaffNeedsReply, parsedStaffChatUnread))
        {
            ticketMineOpen = parsedMineOpen;
            ticketStaffNeedsReply = parsedStaffNeedsReply;
            staffChatUnread = parsedStaffChatUnread;
            topRenderFrame = 10;
            return true;
        }
        int expectedStaffMessages = 0;
        if (Protocol::parseStaffChatStart(jsonLine, expectedStaffMessages))
        {
            (void)expectedStaffMessages;
            staffChatMessageCount = 0;
            return true;
        }
        StaffChatMessage parsedStaffMessage;
        if (Protocol::parseStaffChatMessage(jsonLine, parsedStaffMessage))
        {
            bool duplicate = false;
            for (int i = 0; i < staffChatMessageCount; i++) duplicate = duplicate || staffChatMessages[i].id == parsedStaffMessage.id;
            if (!duplicate)
            {
                if (staffChatMessageCount >= 8)
                {
                    memmove(&staffChatMessages[0], &staffChatMessages[1], sizeof(StaffChatMessage) * 7);
                    staffChatMessageCount = 7;
                }
                staffChatMessages[staffChatMessageCount++] = parsedStaffMessage;
            }
            if (ticketView == 4 && strstr(jsonLine, "\"live\":true"))
            {
                char readCommand[64];
                Protocol::buildStaffChatRead(readCommand, sizeof(readCommand), parsedStaffMessage.id);
                sendTicketCommand(readCommand);
            }
            topRenderFrame = 10;
            return true;
        }
        int parsedStaffNextBeforeId = 0;
        if (Protocol::parseStaffChatEnd(jsonLine, parsedStaffNextBeforeId))
        {
            staffChatNextBeforeId = parsedStaffNextBeforeId;
            ticketView = 4;
            topRenderFrame = 10;
            return true;
        }
        bool staffChatOk = false;
        char staffChatError[48] = "";
        if (Protocol::parseStaffChatResult(jsonLine, staffChatOk, staffChatError, sizeof(staffChatError)))
        {
            setTicketNotice(staffChatOk ? "STAFF MESSAGE SENT" : (staffChatError[0] ? staffChatError : "STAFF CHAT FAILED"));
            return true;
        }
        char listScope[12] = "";
        int expectedTicketCount = 0;
        if (Protocol::parseTicketListStart(jsonLine, listScope, sizeof(listScope), expectedTicketCount))
        {
            (void)expectedTicketCount;
            ticketListCount = 0;
            ticketSelected = 0;
            ticketStaffScope = strcmp(listScope, "staff") == 0;
            return true;
        }
        SupportTicketSummary parsedTicket;
        if (Protocol::parseTicketSummary(jsonLine, parsedTicket))
        {
            if (strstr(jsonLine, "\"type\":\"ticketThreadStart\""))
            {
                activeTicket = parsedTicket;
                ticketMessageCount = 0;
                ticketView = 2;
            }
            else if (strstr(jsonLine, "\"type\":\"ticketSummary\""))
            {
                if (ticketListCount < 6)
                    ticketList[ticketListCount++] = parsedTicket;
            }
            else
            {
                if (activeTicket.id == parsedTicket.id)
                    activeTicket = parsedTicket;
                for (int i = 0; i < ticketListCount; i++)
                    if (ticketList[i].id == parsedTicket.id)
                        ticketList[i] = parsedTicket;
            }
            topRenderFrame = 10;
            return true;
        }
        SupportTicketMessage parsedMessage;
        if (Protocol::parseTicketMessage(jsonLine, parsedMessage))
        {
            if (ticketMessageCount < 6)
                ticketMessages[ticketMessageCount++] = parsedMessage;
            topRenderFrame = 10;
            return true;
        }
        int parsedNextBeforeId = 0;
        if (Protocol::parseTicketListEnd(jsonLine, parsedNextBeforeId))
        {
            ticketNextBeforeId = parsedNextBeforeId;
            ticketSelected = std::max(0, std::min(ticketSelected, std::max(0, ticketListCount - 1)));
            ticketView = 1;
            topRenderFrame = 10;
            return true;
        }
        int parsedThreadId = 0;
        int parsedNextMessage = 0;
        if (Protocol::parseTicketThreadEnd(jsonLine, parsedThreadId, parsedNextMessage))
        {
            (void)parsedThreadId;
            ticketNextBeforeMessageId = parsedNextMessage;
            ticketView = 2;
            topRenderFrame = 10;
            return true;
        }
        bool ticketOk = false;
        char ticketAction[24] = "";
        char ticketError[48] = "";
        int resultTicketId = 0;
        if (Protocol::parseTicketResult(jsonLine, ticketOk, ticketAction, sizeof(ticketAction), ticketError, sizeof(ticketError), resultTicketId))
        {
            if (ticketOk)
            {
                setTicketNotice("TICKET ACTION OK");
                char command[128];
                if ((strcmp(ticketAction, "reply") == 0 || strcmp(ticketAction, "status") == 0 || strcmp(ticketAction, "approveUnban") == 0) && resultTicketId > 0)
                {
                    Protocol::buildTicketGet(command, sizeof(command), resultTicketId);
                    sendTicketCommand(command);
                }
                else if (strcmp(ticketAction, "create") == 0)
                {
                    Protocol::buildTicketList(command, sizeof(command), false, "", supportOnlyMode ? "unban" : "", 0);
                    sendTicketCommand(command);
                }
            }
            else
            {
                char notice[64];
                snprintf(notice, sizeof(notice), "FAILED: %.48s", ticketError[0] ? ticketError : "ticket-error");
                setTicketNotice(notice);
            }
            return true;
        }
        if (strstr(jsonLine, "\"type\":\"accessRestored\""))
        {
            supportOnlyMode = false;
            snprintf(gDisconnectReason, sizeof(gDisconnectReason), "ACCESS RESTORED - PRESS A");
            topMode = TOP_MODE_STATUS;
            setTicketNotice("ACCESS RESTORED");
            return true;
        }
        if (Protocol::parseChannels(jsonLine, availableChannels, 8, availableChannelCount, currentChannel))
        {
            if (currentChannel[0])
                canvas.setChannel(currentChannel);
            syncSelectedChannel();
            topRenderFrame = 10;
            return true;
        }
        if (Protocol::parsePresence(jsonLine, connectedUsers, 24, connectedUserCount))
        {
            topRenderFrame = 10;
            return true;
        }
        bool authoritativeNeedsDisplayName = false;
        bool authoritativeNeedsRules = false;
        char authoritativeRulesVersion[32] = "";
        if (Protocol::parseOnboardingState(jsonLine, authoritativeNeedsDisplayName, authoritativeNeedsRules,
                                           authoritativeRulesVersion, sizeof(authoritativeRulesVersion)))
        {
            OnboardingStage previousStage = onboardingStage;
            gNeedsDisplayName = authoritativeNeedsDisplayName;
            gNeedsRules = authoritativeNeedsRules;
            if (authoritativeRulesVersion[0])
                snprintf(gRequiredRulesVersion, sizeof(gRequiredRulesVersion), "%s", authoritativeRulesVersion);

            u64 submissionElapsed = onboardingSubmissionStartedAt > 0
                                        ? osGetTime() - onboardingSubmissionStartedAt
                                        : 0;
            if (gNeedsDisplayName)
            {
                topMode = TOP_MODE_IDENTITY;
                if (previousStage == ONBOARDING_SUBMITTING_DISPLAY_NAME)
                {
                    onboardingStage = ONBOARDING_SUBMITTING_DISPLAY_NAME;
                    if (onboardingStateQuerySent && submissionElapsed >= 5000 && onboardingRetryCount == 0 &&
                        !retryPendingOnboardingSubmission())
                    {
                        onboardingStage = ONBOARDING_WAITING_DISPLAY_NAME;
                        clearOnboardingSubmission();
                        setIdentityNotice("SEND FAILED - PRESS A TO RETRY");
                    }
                }
                else
                {
                    onboardingStage = ONBOARDING_WAITING_DISPLAY_NAME;
                    clearOnboardingSubmission();
                }
            }
            else if (gNeedsRules)
            {
                if (previousStage == ONBOARDING_SUBMITTING_DISPLAY_NAME && pendingDisplayName[0])
                {
                    snprintf(identityInfo.displayName, sizeof(identityInfo.displayName), "%s", pendingDisplayName);
                    snprintf(gIdentity.displayName, sizeof(gIdentity.displayName), "%s", pendingDisplayName);
                    saveDeviceIdentity();
                    setIdentityNotice("NAME SAVED");
                }
                topMode = TOP_MODE_RULES;
                if (previousStage == ONBOARDING_SUBMITTING_RULES)
                {
                    onboardingStage = ONBOARDING_SUBMITTING_RULES;
                    if (onboardingStateQuerySent && submissionElapsed >= 5000 && onboardingRetryCount == 0 &&
                        !retryPendingOnboardingSubmission())
                    {
                        onboardingStage = ONBOARDING_WAITING_RULES;
                        clearOnboardingSubmission();
                        setIdentityNotice("SEND FAILED - PRESS A TO RETRY");
                    }
                }
                else
                {
                    onboardingStage = ONBOARDING_WAITING_RULES;
                    clearOnboardingSubmission();
                }
            }
            else
            {
                if (previousStage == ONBOARDING_SUBMITTING_DISPLAY_NAME && pendingDisplayName[0])
                {
                    snprintf(identityInfo.displayName, sizeof(identityInfo.displayName), "%s", pendingDisplayName);
                    snprintf(gIdentity.displayName, sizeof(gIdentity.displayName), "%s", pendingDisplayName);
                    setIdentityNotice("NAME SAVED");
                }
                else if (previousStage == ONBOARDING_SUBMITTING_RULES)
                    setIdentityNotice("RULES ACCEPTED");
                if (authoritativeRulesVersion[0])
                    snprintf(gIdentity.rulesAcceptedVersion, sizeof(gIdentity.rulesAcceptedVersion), "%s", authoritativeRulesVersion);
                saveDeviceIdentity();
                onboardingStage = ONBOARDING_READY;
                clearOnboardingSubmission();
                rulesRequireFreshAPress = false;
                if (topMode == TOP_MODE_IDENTITY || topMode == TOP_MODE_RULES)
                    topMode = TOP_MODE_CANVAS;
            }
            topRenderFrame = 10;
            return true;
        }
        if (Protocol::parseIdentityAccepted(jsonLine, identityInfo))
        {
            applyIdentityAccepted(identityInfo);
            if (strcmp(identityInfo.status, "muted") == 0 || strcmp(identityInfo.status, "banned") == 0)
            {
                restrictionActive = true;
                restrictionInitialSeconds = strcmp(identityInfo.status, "muted") == 0 ? identityInfo.muteSecondsRemaining : identityInfo.banSecondsRemaining;
                restrictionHasDuration = restrictionInitialSeconds > 0;
                restrictionStartedAt = osGetTime();
                snprintf(restrictionReason, sizeof(restrictionReason), "%s", identityInfo.restrictionReason[0] ? identityInfo.restrictionReason : identityInfo.status);
            }
            else if (!supportOnlyMode)
            {
                restrictionActive = false;
                restrictionReason[0] = '\0';
            }
            if (strcmp(identityInfo.role, "mod") != 0 && strcmp(identityInfo.role, "admin") != 0)
            {
                gRainbowEnabled = false;
                gRainbowStrokeColorValid = false;
                if (ticketStaffScope)
                {
                    ticketStaffScope = false;
                    ticketView = 0;
                    ticketHomeSelected = 0;
                }
            }
            setIdentityNotice("ACCOUNT OK");
            return true;
        }
        if (strstr(jsonLine, "\"type\":\"identityNeedsDisplayName\""))
        {
            gNeedsDisplayName = true;
            if (onboardingStage != ONBOARDING_SUBMITTING_DISPLAY_NAME)
                onboardingStage = ONBOARDING_WAITING_DISPLAY_NAME;
            topMode = TOP_MODE_IDENTITY;
            setIdentityNotice("CHOOSE A NAME");
            return true;
        }
        if (Protocol::parseDisplayNameRejected(jsonLine, rejectionReason, sizeof(rejectionReason)))
        {
            gNeedsDisplayName = true;
            onboardingStage = ONBOARDING_WAITING_DISPLAY_NAME;
            clearOnboardingSubmission();
            topMode = TOP_MODE_IDENTITY;
            if (strcmp(rejectionReason, "display-name-taken") == 0)
                setIdentityNotice("NAME TAKEN");
            else if (strcmp(rejectionReason, "display-name-reserved") == 0)
                setIdentityNotice("NAME RESERVED");
            else
                setIdentityNotice("NAME REJECTED");
            return true;
        }
        if (strstr(jsonLine, "\"type\":\"displayNameChanged\""))
        {
            gNeedsDisplayName = false;
            onboardingStage = gNeedsRules ? ONBOARDING_WAITING_RULES : ONBOARDING_READY;
            clearOnboardingSubmission();
            setIdentityNotice("NAME SAVED");
            return true;
        }
        char rulesRejectedReason[48] = "";
        char rejectedRulesVersion[32] = "";
        if (Protocol::parseRulesRejected(jsonLine, rulesRejectedReason, sizeof(rulesRejectedReason),
                                         rejectedRulesVersion, sizeof(rejectedRulesVersion)))
        {
            if (rejectedRulesVersion[0])
                snprintf(gRequiredRulesVersion, sizeof(gRequiredRulesVersion), "%s", rejectedRulesVersion);
            gNeedsRules = true;
            onboardingStage = ONBOARDING_WAITING_RULES;
            clearOnboardingSubmission();
            topMode = TOP_MODE_RULES;
            if (strcmp(rulesRejectedReason, "display-name-required") == 0 ||
                strcmp(rulesRejectedReason, "name-required") == 0)
                setIdentityNotice("CHOOSE A NAME FIRST");
            else if (strcmp(rulesRejectedReason, "wrong-version") == 0 ||
                     strcmp(rulesRejectedReason, "invalid-version") == 0 ||
                     strcmp(rulesRejectedReason, "rules-version-mismatch") == 0)
                setIdentityNotice("RULES CHANGED - REVIEW AGAIN");
            else
                setIdentityNotice("RULES NOT ACCEPTED - PRESS A TO RETRY");
            return true;
        }
        if (Protocol::parseRulesRequired(jsonLine, gateVersion, sizeof(gateVersion)))
        {
            snprintf(gRequiredRulesVersion, sizeof(gRequiredRulesVersion), "%s", gateVersion[0] ? gateVersion : "1");
            gNeedsRules = true;
            if (onboardingStage != ONBOARDING_SUBMITTING_RULES)
                onboardingStage = ONBOARDING_WAITING_RULES;
            topMode = TOP_MODE_RULES;
            topRenderFrame = 10;
            return true;
        }
        if (strstr(jsonLine, "\"type\":\"rulesAccepted\""))
        {
            if (Protocol::parseRulesRequired(jsonLine, gateVersion, sizeof(gateVersion)) && gateVersion[0])
                snprintf(gRequiredRulesVersion, sizeof(gRequiredRulesVersion), "%s", gateVersion);
            if (gRequiredRulesVersion[0])
                snprintf(gIdentity.rulesAcceptedVersion, sizeof(gIdentity.rulesAcceptedVersion), "%s", gRequiredRulesVersion);
            gNeedsRules = false;
            onboardingStage = gNeedsDisplayName ? ONBOARDING_WAITING_DISPLAY_NAME : ONBOARDING_READY;
            clearOnboardingSubmission();
            rulesRequireFreshAPress = false;
            saveDeviceIdentity();
            setIdentityNotice("RULES OK");
            topMode = TOP_MODE_CANVAS;
            return true;
        }
        char mutedReason[81] = "";
        int mutedSeconds = 0;
        if (Protocol::parseMuted(jsonLine, mutedReason, sizeof(mutedReason), mutedSeconds))
        {
            restrictionActive = true;
            restrictionHasDuration = mutedSeconds > 0;
            restrictionInitialSeconds = mutedSeconds;
            restrictionStartedAt = osGetTime();
            snprintf(restrictionReason, sizeof(restrictionReason), "%s", mutedReason[0] ? mutedReason : "Drawing is disabled");
            setIdentityNotice("MUTED - DRAWING DISABLED");
            topRenderFrame = 10;
            return true;
        }
        if (Protocol::parseDisconnected(jsonLine, disconnectReason, sizeof(disconnectReason)))
        {
            snprintf(gDisconnectReason, sizeof(gDisconnectReason), "%s", disconnectReason);
            setAdminNotice(gDisconnectReason);
            topMode = TOP_MODE_STATUS;
            return true;
        }
        if (Protocol::parseIdentityBackupCode(jsonLine, identityInfo))
        {
            applyBackupCode(identityInfo);
            setIdentityNotice("BACKUP SAVED");
            return true;
        }
        if (Protocol::parseRecoveryFailed(jsonLine, recoveryReason, sizeof(recoveryReason)))
        {
            setIdentityNotice("RECOVERY FAILED");
            printf("Recovery failed: %s\n", recoveryReason);
            return true;
        }
        if (strstr(jsonLine, "\"type\":\"adminCanvasResult\""))
        {
            if (strstr(jsonLine, "\"ok\":true"))
                setAdminNotice("ADMIN ACTION OK");
            else
                setAdminNotice("ADMIN ACTION DENIED");
            return true;
        }
        if (strstr(jsonLine, "\"type\":\"moderationResult\""))
        {
            if (strstr(jsonLine, "\"ok\":true"))
                setAdminNotice("MOD ACTION OK");
            else if (strstr(jsonLine, "admin-required"))
                setAdminNotice("ADMIN REQUIRED");
            else
                setAdminNotice("MOD ACTION DENIED");
            return true;
        }
        return false;
    };

    {
        CanvasMeta meta;
        bool receivedMeta = false;
        bool initialHelloSent = true;
        std::vector<uint8_t> initialCanvas;
        u64 initialDeadline = osGetTime() + 15000;
        NetworkEvent event;
        while (osGetTime() < initialDeadline)
        {
            if (!NetworkManager::waitEvent(event, 250))
                continue;
            if (event.type == NETWORK_EVENT_ERROR || event.type == NETWORK_EVENT_DISCONNECTED)
            {
                printf("Initial connection failed: %s\n", event.detail.c_str());
                receivedMeta = false;
                initialHelloSent = false;
                initialCanvas.clear();
                continue;
            }
            if (event.type == NETWORK_EVENT_CONNECTED)
            {
                if (!initialHelloSent)
                {
                    initialHelloSent = sendClientHello(packageType);
                    if (!initialHelloSent)
                        NetworkManager::reconnect();
                }
                continue;
            }
            if (event.type == NETWORK_EVENT_BINARY)
            {
                if (receivedMeta)
                {
                    initialCanvas = std::move(event.payload);
                    break;
                }
                continue;
            }
            if (event.type != NETWORK_EVENT_TEXT || event.payload.size() >= CONTROL_LINE_CAPACITY)
                continue;

            std::string line(event.payload.begin(), event.payload.end());
            char latestVersion[32] = "";
            char updateReason[48] = "";
            if (Protocol::parseUpdateRequired(line.c_str(), latestVersion, sizeof(latestVersion), updateReason, sizeof(updateReason)))
            {
                printf("Update required: %s Latest: %s\n", updateReason, latestVersion);
#if !UPDATER_ENABLED
                failExit("Server requested update to %s.\nThis build has the updater disabled.", latestVersion);
#else
                NetworkManager::disconnect();
                if (!NetworkManager::waitForDisconnected(35000))
                    failExit("Secure connection worker did not stop.\nRestart Collab Doodle and try again.");
                HttpsUpdateAttempt updateAttempt = offerHttpsUpdate(packageType, updateTargetPath, installedTitleId);
                if (updateAttempt.result == UPDATE_DOWNLOAD_OK)
                    exitAfterUpdateInstalled(packageType, installedTitleId);
                if (strcmp(packageType, "cia") == 0 && updateAttempt.result == UPDATE_DOWNLOAD_INSTALL_FAILED)
                    failExit("CIA install failed.\n%s", Updater::lastError());
                failExit("Update required.\nDownload the latest Collab Doodle build to continue.");
#endif
            }

            if (handleJsonControl(line.c_str()))
            {
                if (supportOnlyMode)
                    break;
                continue;
            }

            if (Protocol::parseCanvasMeta(line.c_str(), meta))
            {
                if (!isValidCanvasMeta(meta))
                {
                    printf("Invalid initial canvas size: %d\n", meta.compressedSize);
                    break;
                }
                receivedMeta = true;
            }
        }

        if (!receivedMeta && supportOnlyMode)
        {
            canvasWidth = 320;
            canvasHeight = 240;
            canvas.allocate(canvasWidth, canvasHeight);
            canvas.setChannel("support");
            fullCanvas = canvas.pixels;
            Renderer::invalidateMinimap();
        }
        else if (!receivedMeta)
        {
            printf("Failed to read init canvas metadata.\n");
        }
        else
        {
            canvasWidth = meta.width;
            canvasHeight = meta.height;
            canvas.setChannel(meta.channel);
            printf("Received canvas dimensions: W=%d, H=%d, Compressed Size=%d\n", canvasWidth, canvasHeight, meta.compressedSize);

            if (initialCanvas.size() == (size_t)meta.compressedSize)
            {
                if (canvas.allocate(canvasWidth, canvasHeight) && canvas.loadFromCompressed(initialCanvas.data(), initialCanvas.size()))
                {
                    fullCanvas = canvas.pixels;
                    Renderer::invalidateMinimap();
                    printf("Canvas decompressed successfully.\n");
                }
                else
                {
                    printf("Failed to decompress canvas data.\n");
                }
            }
            else
            {
                printf("Failed to read compressed canvas data.\n");
            }
        }

        if (!supportOnlyMode && !fullCanvas)
        {
            // Keep the UI valid while the worker retries. A subsequent
            // protocol-6 canvas message atomically replaces this placeholder.
            canvasWidth = 320;
            canvasHeight = 240;
            canvas.allocate(canvasWidth, canvasHeight);
            canvas.setChannel("main");
            fullCanvas = canvas.pixels;
            snprintf(gDisconnectReason, sizeof(gDisconnectReason), "SYNC FAILED - RETRYING");
            topMode = TOP_MODE_STATUS;
            NetworkManager::reconnect();
        }
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
    CanvasMeta realtimeCanvasMeta;
    memset(&realtimeCanvasMeta, 0, sizeof(realtimeCanvasMeta));
    bool realtimeCanvasPending = false;
    bool sessionAwaitingSnapshot = false;
    u64 sessionSnapshotDeadline = 0;

    auto clampOffsets = [&](int &ox, int &oy)
    {
        canvas.offsetX = ox;
        canvas.offsetY = oy;
        canvas.clampOffsets(fbHeight, fbWidth);
        ox = canvas.offsetX;
        oy = canvas.offsetY;
    };

    auto reconnectSession = [&](const char *context) -> bool
    {
        bool wasSupportOnly = supportOnlyMode;
        supportOnlyMode = false;
        realtimeCanvasPending = false;
        sessionAwaitingSnapshot = false;
        sessionSnapshotDeadline = 0;
        UIState::clearPoints();
        if (!NetworkManager::reconnect())
        {
            supportOnlyMode = wasSupportOnly;
            snprintf(gDisconnectReason, sizeof(gDisconnectReason), "RECONNECT FAILED - PRESS A");
            topMode = TOP_MODE_STATUS;
            return false;
        }
        snprintf(gDisconnectReason, sizeof(gDisconnectReason), "RECONNECTING");
        setAdminNotice(context && context[0] ? "RECONNECTING" : "CONNECTING");
        topMode = TOP_MODE_STATUS;
        topRenderFrame = 10;
        return true;
    };

    auto sendAdminCanvasCommand = [&](const char *action, int x, int y, int w, int h, Color color) -> bool
    {
        if (!isModOrAdmin())
        {
            setAdminNotice("MOD OR ADMIN REQUIRED");
            return false;
        }
        if (!NetworkManager::checkConnection() && !reconnectSession("admin-reconnect"))
        {
            setAdminNotice("CONNECTION FAILED");
            return false;
        }

        char command[256];
        Protocol::buildAdminCanvasCommand(command, sizeof(command), action, canvas.channel[0] ? canvas.channel : "main",
                                          x, y, w, h, color.r, color.g, color.b);
        if (!NetworkManager::sendText(command))
        {
            setAdminNotice("SEND FAILED");
            return false;
        }
        setAdminNotice("COMMAND SENT");
        return true;
    };

    auto switchToSelectedChannel = [&]() -> bool
    {
        if (availableChannelCount <= 0)
            return false;

        selectedChannel = std::max(0, std::min(selectedChannel, availableChannelCount - 1));
        if (strcmp(canvas.channel, availableChannels[selectedChannel]) == 0)
            return true;

        if (!NetworkManager::checkConnection() && !reconnectSession("channel-reconnect"))
        {
            printf("Cannot switch channel - connection failed.\n");
            return false;
        }

        char command[96];
        Protocol::buildSwitchChannel(command, sizeof(command), availableChannels[selectedChannel]);
        if (!NetworkManager::sendText(command))
        {
            printf("Failed to request channel switch.\n");
            return false;
        }

        printf("Switching to channel %s...\n", availableChannels[selectedChannel]);
        // Protocol 6 delivers the metadata and snapshot as distinct WebSocket
        // messages; the realtime event loop consumes both asynchronously.
        return true;
    };

    syncSelectedChannel();

    while (aptMainLoop() && !exitRequested)
    {
        bool resumedFromSleep = gAptResumeRequested;
        gAptResumeRequested = false;
        if (resumedFromSleep)
        {
            setAdminNotice("WAKING - CHECKING VERSION");
            reconnectSession("wake");
        }
        if ((onboardingStage == ONBOARDING_SUBMITTING_DISPLAY_NAME ||
             onboardingStage == ONBOARDING_SUBMITTING_RULES) &&
            onboardingSubmissionStartedAt > 0)
        {
            u64 elapsed = osGetTime() - onboardingSubmissionStartedAt;
            if (elapsed >= 10000)
            {
                bool nameSubmission = onboardingStage == ONBOARDING_SUBMITTING_DISPLAY_NAME;
                onboardingStage = nameSubmission ? ONBOARDING_WAITING_DISPLAY_NAME : ONBOARDING_WAITING_RULES;
                clearOnboardingSubmission();
                setIdentityNotice("NO RESPONSE - PRESS A TO RETRY");
                topMode = nameSubmission ? TOP_MODE_IDENTITY : TOP_MODE_RULES;
            }
            else if (elapsed >= 5000 && !onboardingStateQuerySent)
            {
                onboardingStateQuerySent = true;
                if (requestOnboardingState())
                    setIdentityNotice("CHECKING ACCOUNT STATE");
                else
                    setIdentityNotice("CHECK FAILED - PRESS A TO RETRY");
            }
        }
        if (restrictionActive && restrictionHasDuration)
        {
            int elapsed = (int)((osGetTime() - restrictionStartedAt) / 1000ULL);
            restrictionSecondsRemaining = std::max(0, restrictionInitialSeconds - elapsed);
            if (restrictionSecondsRemaining == 0)
            {
                restrictionActive = false;
                restrictionReason[0] = '\0';
                snprintf(identityInfo.status, sizeof(identityInfo.status), "active");
                reconnectSession("restriction-expired");
            }
        }
        else
        {
            restrictionSecondsRemaining = restrictionInitialSeconds;
        }
        hidScanInput();
        u32 kDown = hidKeysDown();
        u32 kHeld = hidKeysHeld();
        u32 kUp = hidKeysUp();
        touchPosition touch;
        circlePosition circle;
        hidTouchRead(&touch);
        hidCircleRead(&circle);

        if (rulesRequireFreshAPress && topMode == TOP_MODE_RULES &&
            rulesRenderedSinceKeyboard && !(kHeld & KEY_A))
        {
            rulesRequireFreshAPress = false;
        }

        if (kDown & KEY_SELECT)
        {
            if (gNeedsDisplayName)
            {
                topMode = TOP_MODE_IDENTITY;
                setIdentityNotice("CHOOSE A NAME");
            }
            else if (gNeedsRules)
                topMode = TOP_MODE_RULES;
            else if (supportOnlyMode)
            {
                topMode = TOP_MODE_TICKETS;
                ticketView = 0;
                ticketHomeSelected = 0;
            }
            else
            {
                topMode = TOP_MODE_MENU;
                selectedMenuItem = 0;
            }
            topRenderFrame = 10;
            continue;
        }

        if (!supportOnlyMode && (kDown & KEY_START))
        {
            printf("Refreshing canvas from server...\n");
            if (!NetworkManager::checkConnection() && !reconnectSession("refresh-reconnect")) {
                printf("Cannot refresh canvas - connection failed!\n");
                continue;
            }

            char request[48];
            Protocol::buildGetCanvas(request, sizeof(request));
            if (!NetworkManager::sendText(request)) {
                printf("Failed to send refresh request.\n");
                continue;
            }
            setAdminNotice("REFRESH REQUESTED");
        }

        if (gDisconnectReason[0])
        {
            if (kDown & KEY_A)
            {
                setAdminNotice("RECONNECTING");
                reconnectSession("reconnect");
                topRenderFrame = 10;
            }
            else if (kDown & KEY_B)
            {
                topMode = TOP_MODE_MENU;
                topRenderFrame = 10;
            }
        }

        if (gNeedsDisplayName && topMode == TOP_MODE_CANVAS)
        {
            topMode = TOP_MODE_IDENTITY;
            setIdentityNotice("CHOOSE A NAME");
            topRenderFrame = 10;
        }
        else if (gNeedsRules && topMode == TOP_MODE_CANVAS)
        {
            topMode = TOP_MODE_RULES;
            topRenderFrame = 10;
        }

        if (topMode == TOP_MODE_MENU)
        {
            const int menuCount = 8;
            selectedMenuItem = std::max(0, std::min(selectedMenuItem, menuCount - 1));
            if (kDown & KEY_DUP)
            {
                selectedMenuItem = (selectedMenuItem + menuCount - 1) % menuCount;
                topRenderFrame = 10;
            }
            if (kDown & KEY_DDOWN)
            {
                selectedMenuItem = (selectedMenuItem + 1) % menuCount;
                topRenderFrame = 10;
            }
            if (kDown & KEY_B)
            {
                topMode = TOP_MODE_CANVAS;
                topRenderFrame = 10;
                continue;
            }
            if (kDown & KEY_A)
            {
                if (selectedMenuItem == 0)
                {
                    syncSelectedChannel();
                    topMode = TOP_MODE_CHANNELS;
                }
                else if (selectedMenuItem == 1)
                {
                    topMode = TOP_MODE_USERS;
                }
                else if (selectedMenuItem == 2)
                {
                    topMode = TOP_MODE_TICKETS;
                    ticketView = 0;
                    ticketHomeSelected = 0;
                    char command[48];
                    Protocol::buildTicketCounts(command, sizeof(command));
                    sendTicketCommand(command);
                }
                else if (selectedMenuItem == 3)
                    topMode = TOP_MODE_CONTROLS;
                else if (selectedMenuItem == 4)
                {
                    topMode = TOP_MODE_RULES;
                }
                else if (selectedMenuItem == 5)
                {
                    topMode = TOP_MODE_STATUS;
                }
                else if (selectedMenuItem == 6)
                {
                    topMode = TOP_MODE_IDENTITY;
                }
                else if (selectedMenuItem == 7)
                {
                    UIState::clearPoints();
                    NetworkManager::disconnect();
                    for (int i = 0; i < 30; i++)
                        gspWaitForVBlank();
                    break;
                }
                topRenderFrame = 10;
                continue;
            }
        }
        else if (topMode == TOP_MODE_ADMIN)
        {
            if (kDown & KEY_DUP)
            {
                selectedAdminItem = (selectedAdminItem + 2) % 3;
                topRenderFrame = 10;
            }
            if (kDown & KEY_DDOWN)
            {
                selectedAdminItem = (selectedAdminItem + 1) % 3;
                topRenderFrame = 10;
            }
            if (kDown & KEY_A)
            {
                if (!isModOrAdmin())
                {
                    setAdminNotice("MOD OR ADMIN REQUIRED");
                    continue;
                }
                if (selectedAdminItem == 0)
                {
                    sendAdminCanvasCommand("snapshot", 0, 0, 1, 1, currentColor);
                }
                else if (selectedAdminItem == 1)
                {
                    Color white = {255, 255, 255};
                    if (sendAdminCanvasCommand("clear", 0, 0, canvasWidth, canvasHeight, white))
                    {
                        applyCanvasRectLocal(canvas, 0, 0, canvasWidth, canvasHeight, white);
                        Renderer::invalidateMinimap();
                    }
                }
                else if (selectedAdminItem == 2)
                {
                    pendingAdminRectTool = ADMIN_RECT_FILL;
                    adminRectDragging = false;
                    topMode = TOP_MODE_CANVAS;
                    setAdminNotice("DRAG FILL RECT");
                }
                topRenderFrame = 10;
                continue;
            }
        }
        else if (topMode == TOP_MODE_TICKETS)
        {
            const bool staffAllowed = isModOrAdmin() && !supportOnlyMode;
            if (ticketView == 0)
            {
                int homeCount = supportOnlyMode ? 2 : (staffAllowed ? 6 : 4);
                ticketHomeSelected = std::max(0, std::min(ticketHomeSelected, homeCount - 1));
                if (kDown & KEY_DUP)
                {
                    ticketHomeSelected = (ticketHomeSelected + homeCount - 1) % homeCount;
                    topRenderFrame = 10;
                }
                if (kDown & KEY_DDOWN)
                {
                    ticketHomeSelected = (ticketHomeSelected + 1) % homeCount;
                    topRenderFrame = 10;
                }
                if ((kDown & KEY_B) && !supportOnlyMode)
                {
                    topMode = TOP_MODE_MENU;
                    topRenderFrame = 10;
                    continue;
                }
                if (kDown & KEY_A)
                {
                    bool listMine = (supportOnlyMode && ticketHomeSelected == 1) || (!supportOnlyMode && ticketHomeSelected == 3);
                    bool listStaff = !supportOnlyMode && staffAllowed && ticketHomeSelected == 4;
                    bool openStaffChat = !supportOnlyMode && staffAllowed && ticketHomeSelected == 5;
                    if (openStaffChat)
                    {
                        char command[96];
                        Protocol::buildStaffChatList(command, sizeof(command));
                        if (sendTicketCommand(command)) setTicketNotice("LOADING STAFF CHAT");
                        continue;
                    }
                    if (listMine || listStaff)
                    {
                        char command[192];
                        ticketStaffScope = listStaff;
                        Protocol::buildTicketList(command, sizeof(command), listStaff, listStaff ? "unresolved" : "",
                                                  supportOnlyMode ? "unban" : "", 0);
                        if (sendTicketCommand(command))
                            setTicketNotice("LOADING TICKETS");
                    }
                    else
                    {
                        const char *category = supportOnlyMode || ticketHomeSelected == 0 ? "unban" :
                                               ticketHomeSelected == 1 ? "bug" : "feature";
                        char subject[65];
                        char details[241];
                        if (!readKeyboardText("Ticket subject", subject, sizeof(subject), ""))
                            continue;
                        if (!readKeyboardText("Ticket details", details, sizeof(details), ""))
                            continue;
                        char command[512];
                        Protocol::buildTicketCreate(command, sizeof(command), category, subject, details);
                        if (sendTicketCommand(command))
                            setTicketNotice("TICKET SENT");
                    }
                    topRenderFrame = 10;
                    continue;
                }
            }
            else if (ticketView == 1)
            {
                if (ticketListCount > 0 && (kDown & KEY_DUP))
                {
                    ticketSelected = (ticketSelected + ticketListCount - 1) % ticketListCount;
                    topRenderFrame = 10;
                }
                if (ticketListCount > 0 && (kDown & KEY_DDOWN))
                {
                    ticketSelected = (ticketSelected + 1) % ticketListCount;
                    topRenderFrame = 10;
                }
                if (kDown & KEY_B)
                {
                    ticketView = 0;
                    ticketHomeSelected = supportOnlyMode ? 1 : (ticketStaffScope ? 4 : 3);
                    topRenderFrame = 10;
                    continue;
                }
                if ((kDown & KEY_A) && ticketListCount > 0)
                {
                    char command[128];
                    Protocol::buildTicketGet(command, sizeof(command), ticketList[ticketSelected].id);
                    if (sendTicketCommand(command))
                        setTicketNotice("LOADING THREAD");
                    continue;
                }
                if ((kDown & KEY_X) || ((kDown & KEY_Y) && ticketNextBeforeId > 0))
                {
                    char command[192];
                    int beforeId = (kDown & KEY_Y) ? ticketNextBeforeId : 0;
                    Protocol::buildTicketList(command, sizeof(command), ticketStaffScope,
                                              ticketStaffScope ? "unresolved" : "", supportOnlyMode ? "unban" : "", beforeId);
                    if (sendTicketCommand(command))
                        setTicketNotice(beforeId ? "LOADING NEXT PAGE" : "REFRESHING");
                    continue;
                }
            }
            else if (ticketView == 2)
            {
                bool closed = strcmp(activeTicket.status, "resolved") == 0 || strcmp(activeTicket.status, "rejected") == 0;
                if (kDown & KEY_B)
                {
                    ticketView = 1;
                    topRenderFrame = 10;
                    continue;
                }
                if ((kDown & KEY_Y) && ticketNextBeforeMessageId > 0)
                {
                    char command[128];
                    Protocol::buildTicketGet(command, sizeof(command), activeTicket.id, ticketNextBeforeMessageId);
                    sendTicketCommand(command);
                    continue;
                }
                if ((kDown & KEY_A) && (!closed || ticketStaffScope))
                {
                    char reply[241];
                    if (readKeyboardText("Ticket reply", reply, sizeof(reply), ""))
                    {
                        char command[384];
                        Protocol::buildTicketReply(command, sizeof(command), activeTicket.id, reply, ticketStaffScope);
                        if (sendTicketCommand(command))
                            setTicketNotice("REPLY SENT");
                    }
                    continue;
                }
                if ((kDown & KEY_X) && ticketStaffScope && staffAllowed)
                {
                    ticketView = 3;
                    ticketActionSelected = 0;
                    topRenderFrame = 10;
                    continue;
                }
            }
            else if (ticketView == 3)
            {
                if (kDown & KEY_DUP)
                {
                    ticketActionSelected = (ticketActionSelected + 5) % 6;
                    topRenderFrame = 10;
                }
                if (kDown & KEY_DDOWN)
                {
                    ticketActionSelected = (ticketActionSelected + 1) % 6;
                    topRenderFrame = 10;
                }
                if (kDown & KEY_B)
                {
                    ticketView = 2;
                    topRenderFrame = 10;
                    continue;
                }
                if ((kDown & KEY_A) && staffAllowed)
                {
                    char command[512];
                    command[0] = '\0';
                    if (ticketActionSelected == 0)
                        Protocol::buildTicketStatus(command, sizeof(command), activeTicket.id, "in_progress");
                    else if (ticketActionSelected == 1)
                    {
                        char reply[241];
                        if (readKeyboardText("Reply to requester", reply, sizeof(reply), ""))
                            Protocol::buildTicketReply(command, sizeof(command), activeTicket.id, reply, true);
                    }
                    else if (ticketActionSelected == 2 || ticketActionSelected == 3)
                    {
                        char response[241];
                        if (readKeyboardText(ticketActionSelected == 2 ? "Resolution message" : "Rejection reason", response, sizeof(response), ""))
                            Protocol::buildTicketStatus(command, sizeof(command), activeTicket.id,
                                                        ticketActionSelected == 2 ? "resolved" : "rejected", response);
                    }
                    else if (ticketActionSelected == 4)
                    {
                        if (strcmp(activeTicket.category, "unban") != 0)
                        {
                            setTicketNotice("UNBAN TICKETS ONLY");
                            continue;
                        }
                        char confirm[12];
                        if (readKeyboardText("Type APPROVE", confirm, sizeof(confirm), "") && strcasecmp(confirm, "APPROVE") == 0)
                            Protocol::buildTicketApproveUnban(command, sizeof(command), activeTicket.id);
                        else
                        {
                            setTicketNotice("APPROVAL CANCELLED");
                            continue;
                        }
                    }
                    else if (ticketActionSelected == 5)
                        Protocol::buildTicketStatus(command, sizeof(command), activeTicket.id, "open");

                    if (command[0] && sendTicketCommand(command))
                    {
                        setTicketNotice("STAFF ACTION SENT");
                        ticketView = 2;
                    }
                    continue;
                }
            }
            else if (ticketView == 4)
            {
                if (kDown & KEY_B)
                {
                    ticketView = 0;
                    ticketHomeSelected = 5;
                    char counts[48];
                    Protocol::buildTicketCounts(counts, sizeof(counts));
                    sendTicketCommand(counts);
                    topRenderFrame = 10;
                    continue;
                }
                if (kDown & KEY_A)
                {
                    char message[241];
                    if (!readKeyboardText("Staff message", message, sizeof(message), "")) continue;
                    char command[360];
                    Protocol::buildStaffChatSend(command, sizeof(command), message);
                    if (sendTicketCommand(command)) setTicketNotice("STAFF MESSAGE SENT");
                    continue;
                }
                if ((kDown & KEY_X) || ((kDown & KEY_Y) && staffChatNextBeforeId > 0))
                {
                    char command[96];
                    Protocol::buildStaffChatList(command, sizeof(command), (kDown & KEY_Y) ? staffChatNextBeforeId : 0);
                    if (sendTicketCommand(command)) setTicketNotice((kDown & KEY_Y) ? "LOADING OLDER CHAT" : "REFRESHING STAFF CHAT");
                    continue;
                }
            }
        }
        else if (topMode == TOP_MODE_RULES && (kDown & KEY_B) && gNeedsRules)
        {
            exitRequested = true;
            continue;
        }
        else if ((topMode == TOP_MODE_USERS || topMode == TOP_MODE_ADMIN || topMode == TOP_MODE_STATUS || topMode == TOP_MODE_IDENTITY || topMode == TOP_MODE_CONTROLS || topMode == TOP_MODE_RULES) && (kDown & KEY_B))
        {
            pendingAdminRectTool = ADMIN_RECT_NONE;
            adminRectDragging = false;
            topMode = TOP_MODE_MENU;
            topRenderFrame = 10;
            continue;
        }
        else if (topMode == TOP_MODE_IDENTITY && (kDown & KEY_A))
        {
            if (onboardingStage == ONBOARDING_SUBMITTING_DISPLAY_NAME)
            {
                setIdentityNotice("WAITING FOR SERVER");
                continue;
            }

            rulesRequireFreshAPress = true;
            rulesRenderedSinceKeyboard = false;
            char chosenDisplayName[25] = "";
            if (promptDisplayName(identityInfo, chosenDisplayName, sizeof(chosenDisplayName)))
            {
                snprintf(pendingDisplayName, sizeof(pendingDisplayName), "%s", chosenDisplayName);
                if (sendDisplayNameValue(pendingDisplayName))
                {
                    gNeedsDisplayName = true;
                    beginOnboardingSubmission(ONBOARDING_SUBMITTING_DISPLAY_NAME);
                    setIdentityNotice("NAME SENT");
                }
                else
                {
                    onboardingStage = ONBOARDING_WAITING_DISPLAY_NAME;
                    setIdentityNotice("SEND FAILED - PRESS A TO RETRY");
                }
            }
            continue;
        }
        else if (topMode == TOP_MODE_IDENTITY && (kDown & KEY_X))
        {
            if (recoverIdentity())
            {
                setIdentityNotice("RECOVERING");
            }
            continue;
        }
        else if (topMode == TOP_MODE_IDENTITY && (kDown & KEY_Y))
        {
            if (requestBackupCode())
            {
                setIdentityNotice("CODE SENT");
            }
            continue;
        }
        else if (topMode == TOP_MODE_RULES && (kDown & KEY_A) && !rulesRequireFreshAPress)
        {
            if (onboardingStage == ONBOARDING_SUBMITTING_RULES)
            {
                setIdentityNotice("WAITING FOR SERVER");
            }
            else if (gRequiredRulesVersion[0])
            {
                snprintf(pendingRulesVersion, sizeof(pendingRulesVersion), "%s", gRequiredRulesVersion);
                if (sendRulesValue(pendingRulesVersion))
                {
                    gNeedsRules = true;
                    beginOnboardingSubmission(ONBOARDING_SUBMITTING_RULES);
                    setIdentityNotice("RULES SENT");
                }
                else
                {
                    onboardingStage = ONBOARDING_WAITING_RULES;
                    setIdentityNotice("SEND FAILED - PRESS A TO RETRY");
                }
            }
            else if (!gRequiredRulesVersion[0])
                topMode = TOP_MODE_MENU;
            topRenderFrame = 10;
            continue;
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
                    topMode = TOP_MODE_MENU;
                topRenderFrame = 10;
            }
            if (kDown & KEY_B)
            {
                topMode = TOP_MODE_MENU;
                topRenderFrame = 10;
                continue;
            }
        }

        bool zoomOverlayLeft = (kHeld & KEY_Y) && !(kHeld & KEY_DRIGHT);
        bool zoomOverlayActive = topMode == TOP_MODE_CANVAS && (kHeld & (KEY_DRIGHT | KEY_Y));
        bool blockNormalCanvasInput = gDisconnectReason[0] || gNeedsDisplayName || gNeedsRules || restrictionActive;
        if (!shoulderEraserActive && !blockNormalCanvasInput && topMode == TOP_MODE_CANVAS && (kDown & (KEY_L | KEY_R)))
        {
            shoulderSavedBrushShape = currentBrushShape;
            shoulderSavedBrushSize = currentBrushSize;
            currentBrushShape = BRUSH_ERASER;
            currentBrushSize = 7;
            shoulderEraserActive = true;
            topRenderFrame = 10;
        }
        if (shoulderEraserActive && (!(kHeld & (KEY_L | KEY_R)) || (kUp & (KEY_L | KEY_R))))
        {
            currentBrushShape = shoulderSavedBrushShape;
            currentBrushSize = shoulderSavedBrushSize;
            shoulderEraserActive = false;
            topRenderFrame = 10;
        }
        if (!blockNormalCanvasInput && !zoomOverlayActive && topMode == TOP_MODE_CANVAS && !UIState::isColorPickerActive())
        {
            const int deadzone = 18;
            if (abs(circle.dx) > deadzone || abs(circle.dy) > deadzone)
            {
                offsetX += canvas.screenDeltaToCanvas(circle.dx / 12);
                offsetY -= canvas.screenDeltaToCanvas(circle.dy / 12);
                clampOffsets(offsetX, offsetY);
                canvas.offsetX = offsetX;
                canvas.offsetY = offsetY;
                canvas.markFullDirty();
                Renderer::invalidateMinimap();
                topRenderFrame = 10;
                prevTouchX = prevTouchY = -1;
                prevPrevTouchX = prevPrevTouchY = -1;
                hasLastStrokePoint = false;
            }
        }
        if (topMode == TOP_MODE_CANVAS && pendingAdminRectTool != ADMIN_RECT_NONE)
        {
            blockNormalCanvasInput = true;
            if (kDown & KEY_B)
            {
                pendingAdminRectTool = ADMIN_RECT_NONE;
                adminRectDragging = false;
                setAdminNotice("RECT CANCELLED");
                continue;
            }
            if ((kDown & KEY_TOUCH) && !adminRectDragging)
            {
                canvas.offsetX = offsetX;
                canvas.offsetY = offsetY;
                adminRectStartX = canvas.screenToCanvasX(touch.px);
                adminRectStartY = canvas.screenToCanvasY(touch.py);
                adminRectEndX = adminRectStartX;
                adminRectEndY = adminRectStartY;
                adminRectDragging = true;
                setAdminNotice("RELEASE TO FILL");
            }
            else if ((kHeld & KEY_TOUCH) && adminRectDragging)
            {
                canvas.offsetX = offsetX;
                canvas.offsetY = offsetY;
                adminRectEndX = canvas.screenToCanvasX(touch.px);
                adminRectEndY = canvas.screenToCanvasY(touch.py);
            }
            else if (!(kHeld & KEY_TOUCH) && adminRectDragging)
            {
                canvas.offsetX = offsetX;
                canvas.offsetY = offsetY;
                int endX = adminRectEndX;
                int endY = adminRectEndY;
                int minX = std::max(0, std::min(adminRectStartX, endX));
                int minY = std::max(0, std::min(adminRectStartY, endY));
                int maxX = std::min(canvasWidth - 1, std::max(adminRectStartX, endX));
                int maxY = std::min(canvasHeight - 1, std::max(adminRectStartY, endY));
                int rectW = std::max(1, maxX - minX + 1);
                int rectH = std::max(1, maxY - minY + 1);
                Color sendColor = currentColor;
                bool sent = sendAdminCanvasCommand("fillRect", minX, minY, rectW, rectH, sendColor);
                if (sent)
                {
                    applyCanvasRectLocal(canvas, minX, minY, rectW, rectH, sendColor);
                    Renderer::invalidateMinimap();
                }
                pendingAdminRectTool = ADMIN_RECT_NONE;
                adminRectDragging = false;
            }
        }
        if (!blockNormalCanvasInput && zoomOverlayActive && (kDown & KEY_TOUCH))
        {
            const int zoomButtonX = zoomOverlayX(zoomOverlayLeft);
            const int zoomButtonW = 42;
            const int zoomButtonH = 58;
            bool zoomChanged = false;
            if (pointInRect(touch.px, touch.py, zoomButtonX, 42, zoomButtonW, zoomButtonH))
            {
                canvas.zoomIn();
                zoomChanged = true;
            }
            else if (pointInRect(touch.px, touch.py, zoomButtonX, 144, zoomButtonW, zoomButtonH))
            {
                canvas.zoomOut();
                zoomChanged = true;
            }

            if (zoomChanged)
            {
                canvas.offsetX = offsetX;
                canvas.offsetY = offsetY;
                canvas.clampOffsets(fbHeight, fbWidth);
                offsetX = canvas.offsetX;
                offsetY = canvas.offsetY;
                canvas.markFullDirty();
                Renderer::invalidateMinimap();
                topRenderFrame = 10;
                prevTouchX = prevTouchY = -1;
                prevPrevTouchX = prevPrevTouchY = -1;
                hasLastStrokePoint = false;
                printf("Zoom: %s\n", canvas.zoomLabel());
            }
        }

        if (!blockNormalCanvasInput && topMode == TOP_MODE_CANVAS && !(kHeld & (KEY_DRIGHT | KEY_Y)) && (kDown & (KEY_DDOWN | KEY_B)))
        {
            UIState::toggleColorPicker();
            pendingAdminRectTool = ADMIN_RECT_NONE;
            adminRectDragging = false;
            topMode = TOP_MODE_CANVAS;
            printf(UIState::isColorPickerActive() ? "Color picker activated\n" : "Color picker deactivated\n");
        }

        // Eye Dropper functionality
        if (!blockNormalCanvasInput && !zoomOverlayActive && topMode == TOP_MODE_CANVAS && ((kHeld & KEY_DUP) || (kHeld & KEY_X)))
        {
            if (kDown & KEY_TOUCH)
            {
                canvas.offsetX = offsetX;
                canvas.offsetY = offsetY;
                int touchX = canvas.screenToCanvasX(touch.px);
                int touchY = canvas.screenToCanvasY(touch.py);

                if (touchX >= 0 && touchX < canvasWidth && touchY >= 0 && touchY < canvasHeight)
                {
                    int idx = 3 * (touchY * canvasWidth + touchX);
                    currentColor.r = fullCanvas[idx];
                    currentColor.g = fullCanvas[idx + 1];
                    currentColor.b = fullCanvas[idx + 2];
                    float h, s, v;
                    UIState::RGBtoHSV(currentColor.r / 255.0f, currentColor.g / 255.0f, currentColor.b / 255.0f, h, s, v);
                    UIState::updateHSV(h, s, v);

                    printf("Color picked: R=%d, G=%d, B=%d\n", currentColor.r, currentColor.g, currentColor.b);
                }
            }
        }

        if (!blockNormalCanvasInput && !zoomOverlayActive && topMode == TOP_MODE_CANVAS && UIState::isColorPickerActive() && (kDown & KEY_TOUCH))
        {
            if (pointInRect(touch.px, touch.py, 14, 14, 88, 28))
            {
                pickerTab = 0;
                topRenderFrame = 10;
                continue;
            }
            if (isModOrAdmin() && pointInRect(touch.px, touch.py, 106, 14, 88, 28))
            {
                pickerTab = 1;
                topRenderFrame = 10;
                continue;
            }

            if (pickerTab == 1)
            {
                if (!isModOrAdmin())
                {
                    setAdminNotice("MOD OR ADMIN REQUIRED");
                    continue;
                }
                if (pointInRect(touch.px, touch.py, 22, 54, 132, 36))
                {
                    sendAdminCanvasCommand("snapshot", 0, 0, 1, 1, currentColor);
                    continue;
                }
                if (pointInRect(touch.px, touch.py, 166, 54, 132, 36))
                {
                    Color white = {255, 255, 255};
                    if (sendAdminCanvasCommand("clear", 0, 0, canvasWidth, canvasHeight, white))
                    {
                        applyCanvasRectLocal(canvas, 0, 0, canvasWidth, canvasHeight, white);
                        Renderer::invalidateMinimap();
                    }
                    continue;
                }
                if (pointInRect(touch.px, touch.py, 22, 102, 276, 36))
                {
                    pendingAdminRectTool = ADMIN_RECT_FILL;
                    adminRectDragging = false;
                    UIState::setColorPickerActive(false);
                    setAdminNotice("DRAG SELECTION");
                    continue;
                }
                if (pointInRect(touch.px, touch.py, 22, 150, 276, 34))
                {
                    gRainbowEnabled = !gRainbowEnabled;
                    gRainbowStrokeColorValid = false;
                    setAdminNotice(gRainbowEnabled ? "RAINBOW ENABLED" : "RAINBOW DISABLED");
                    topRenderFrame = 10;
                    continue;
                }
                continue;
            }
        }

        if (!blockNormalCanvasInput && !zoomOverlayActive && topMode == TOP_MODE_CANVAS && UIState::isColorPickerActive() && pickerTab == 0 && (kHeld & KEY_TOUCH))
        {
            float h, s, v;
            UIState::getHSV(h, s, v);

            const int colorSquareX = 162;
            const int colorSquareY = 54;
            const int colorSquareSize = 132;
            const int hueStripY = 198;
            const int hueStripH = 14;
            if (pointInRect(touch.px, touch.py, colorSquareX, colorSquareY, colorSquareSize, colorSquareSize))
            {
                s = (float)(touch.px - colorSquareX) / (float)(colorSquareSize - 1);
                v = 1.0f - ((float)(touch.py - colorSquareY) / (float)(colorSquareSize - 1));
            }
            else if (pointInRect(touch.px, touch.py, colorSquareX, hueStripY - 4, colorSquareSize, hueStripH + 8))
            {
                h = (float)(touch.px - colorSquareX) / (float)(colorSquareSize - 1);
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

            const int brushYs[] = {70, 92, 114, 136, 158};
            const int brushXs[] = {30, 60, 90, 120};
            for (int i = 0; i < 5; i++)
            {
                for (int shape = 0; shape < 4; shape++)
                {
                    if (pointInRect(touch.px, touch.py, brushXs[shape] - 14, brushYs[i] - 12, 28, 24))
                    {
                        currentBrushSize = clampBrushSizeForShape(shape, brushSizeForShapeRow(shape, i));
                        currentBrushShape = shape;
                        break;
                    }
                }
            }
        }

        if (!blockNormalCanvasInput && !zoomOverlayActive && topMode == TOP_MODE_CANVAS && (kHeld & (KEY_DLEFT | KEY_A)))
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
                    int deltaX = canvas.screenDeltaToCanvas(touch.px - prevTouchX);
                    int deltaY = canvas.screenDeltaToCanvas(touch.py - prevTouchY);

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
        else if (!blockNormalCanvasInput && !zoomOverlayActive && topMode == TOP_MODE_CANVAS && !UIState::isColorPickerActive())
        {
            // Normal drawing mode
            if (kHeld & KEY_TOUCH)
            {
                if (gRainbowEnabled && currentBrushShape != BRUSH_ERASER)
                {
                    int rainbowX = canvas.screenToCanvasX(touch.px);
                    int rainbowY = canvas.screenToCanvasY(touch.py);
                    Color nextRainbowColor = rainbowColorForPoint(rainbowX, rainbowY, osGetTime());
                    if (gRainbowStrokeColorValid && !sameColor(gRainbowStrokeColor, nextRainbowColor) && !UIState::getPoints().empty())
                    {
                        if (!NetworkManager::checkConnection())
                        {
                            reconnectSession("draw-reconnect");
                            UIState::clearPoints();
                            gRainbowStrokeColorValid = false;
                            continue;
                        }
                        sendDrawBatchCommand(UIState::getPoints(), gRainbowStrokeColor,
                                             currentBrushSize, effectiveBrushShape());
                        UIState::clearPoints();
                    }
                    gRainbowStrokeColor = nextRainbowColor;
                    gRainbowStrokeColorValid = true;
                }
                else
                {
                    gRainbowStrokeColorValid = false;
                }
                if (prevTouchX == -1 && prevTouchY == -1)
                {
                    prevTouchX = touch.px;
                    prevTouchY = touch.py;
                    prevPrevTouchX = prevPrevTouchY = -1;
                    lastStrokeX = (float)touch.px;
                    lastStrokeY = (float)touch.py;
                    hasLastStrokePoint = true;
                    canvas.offsetX = offsetX;
                    canvas.offsetY = offsetY;
                    drawStrokeSample(fullCanvas, canvasWidth, canvasHeight, touch.px, touch.py, canvas);
                }
                else
                {
                    if (prevPrevTouchX == -1 || prevPrevTouchY == -1 || !hasLastStrokePoint)
                    {
                        drawStrokeLine(fullCanvas, canvasWidth, canvasHeight,
                                       (float)prevTouchX, (float)prevTouchY,
                                       (float)touch.px, (float)touch.py,
                                       canvas);
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
                                        canvas);
                        lastStrokeX = endX;
                        lastStrokeY = endY;
                    }

                    if (UIState::getPoints().size() >= 32)
                    {
                        if (!NetworkManager::checkConnection()) {
                            printf("Connection lost while drawing! Attempting to reconnect...\n");
                            reconnectSession("draw-reconnect");
                            UIState::clearPoints();
                            continue;
                        }
                        Color drawColor = effectiveDrawColor();
                        sendDrawBatchCommand(UIState::getPoints(), drawColor,
                                             currentBrushSize, effectiveBrushShape());
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
                                   canvas);
                }

                if (!UIState::getPoints().empty())
                {
                    if (!NetworkManager::checkConnection()) {
                        printf("Connection lost while drawing! Attempting to reconnect...\n");
                        reconnectSession("draw-reconnect");
                        UIState::clearPoints();
                        continue;
                    }
                    Color drawColor = effectiveDrawColor();
                    sendDrawBatchCommand(UIState::getPoints(), drawColor,
                                         currentBrushSize, effectiveBrushShape());
                    UIState::clearPoints();
                }
                gRainbowStrokeColorValid = false;
                prevTouchX = prevTouchY = -1;
                prevPrevTouchX = prevPrevTouchY = -1;
                hasLastStrokePoint = false;
            }
        }

        // WebSocket messages are already framed and copied out of the network
        // worker. Keep a per-frame event budget so drawing/rendering cannot be
        // starved by a busy connection; a canvas snapshot is allowed through as
        // one large event.
        NetworkEvent networkEvent;
        size_t networkBytesThisFrame = 0;
        int networkEventsThisFrame = 0;
        while (networkEventsThisFrame < 32 &&
               (networkBytesThisFrame < 16 * 1024 || networkEventsThisFrame == 0) &&
               NetworkManager::pollEvent(networkEvent))
        {
            networkEventsThisFrame++;
            networkBytesThisFrame += networkEvent.payload.size();

            if (networkEvent.type == NETWORK_EVENT_CONNECTED)
            {
                realtimeCanvasPending = false;
                sessionAwaitingSnapshot = sendClientHello(packageType);
                sessionSnapshotDeadline = sessionAwaitingSnapshot ? osGetTime() + 30000 : 0;
                if (!sessionAwaitingSnapshot)
                {
                    snprintf(gDisconnectReason, sizeof(gDisconnectReason), "HELLO SEND FAILED");
                    topMode = TOP_MODE_STATUS;
                    NetworkManager::reconnect();
                }
                else
                {
                    snprintf(gDisconnectReason, sizeof(gDisconnectReason), "SYNCING CANVAS");
                    setAdminNotice("CONNECTED - SYNCING");
                    topMode = TOP_MODE_STATUS;
                }
                topRenderFrame = 10;
                continue;
            }

            if (networkEvent.type == NETWORK_EVENT_DISCONNECTED || networkEvent.type == NETWORK_EVENT_ERROR)
            {
                realtimeCanvasPending = false;
                sessionAwaitingSnapshot = false;
                sessionSnapshotDeadline = 0;
                UIState::clearPoints();
                snprintf(gDisconnectReason, sizeof(gDisconnectReason), "%s",
                         networkEvent.type == NETWORK_EVENT_ERROR ? "NETWORK ERROR - RETRYING" : "CONNECTION LOST - RETRYING");
                setAdminNotice(networkEvent.detail.empty() ? gDisconnectReason : networkEvent.detail.c_str());
                topMode = TOP_MODE_STATUS;
                topRenderFrame = 10;
                continue;
            }

            if (networkEvent.type == NETWORK_EVENT_TEXT)
            {
                if (networkEvent.payload.empty() || networkEvent.payload.size() > 32768)
                {
                    NetworkManager::disconnect();
                    snprintf(gDisconnectReason, sizeof(gDisconnectReason), "SERVER MESSAGE TOO LARGE");
                    topMode = TOP_MODE_STATUS;
                    topRenderFrame = 10;
                    continue;
                }
                std::string line(networkEvent.payload.begin(), networkEvent.payload.end());
                CanvasMeta meta;
                if (Protocol::parseCanvasMeta(line.c_str(), meta))
                {
                    if (!isValidCanvasMeta(meta))
                    {
                        setAdminNotice("INVALID CANVAS SIZE");
                        NetworkManager::reconnect();
                        continue;
                    }
                    realtimeCanvasMeta = meta;
                    realtimeCanvasPending = true;
                    sessionAwaitingSnapshot = true;
                    sessionSnapshotDeadline = osGetTime() + 30000;
                    continue;
                }

                char latestVersion[32] = "";
                char updateReason[48] = "";
                if (Protocol::parseUpdateRequired(line.c_str(), latestVersion, sizeof(latestVersion),
                                                  updateReason, sizeof(updateReason)))
                {
                    updateAvailable = true;
                    setAdminNotice("UPDATE AVAILABLE");
                    continue;
                }
                handleJsonControl(line.c_str());
                // Support-only sessions intentionally have no drawing canvas.
                // Receiving that gate completes their reconnect handshake.
                if (supportOnlyMode)
                {
                    sessionAwaitingSnapshot = false;
                    sessionSnapshotDeadline = 0;
                    gDisconnectReason[0] = '\0';
                }
                continue;
            }

            if (networkEvent.type == NETWORK_EVENT_BINARY)
            {
                if (realtimeCanvasPending)
                {
                    size_t expectedSize = (size_t)realtimeCanvasMeta.compressedSize;
                    bool loaded = networkEvent.payload.size() == expectedSize;
                    if (loaded)
                    {
                        canvasWidth = realtimeCanvasMeta.width;
                        canvasHeight = realtimeCanvasMeta.height;
                        canvas.setChannel(realtimeCanvasMeta.channel);
                        loaded = canvas.allocate(canvasWidth, canvasHeight) &&
                                 canvas.loadFromCompressed(networkEvent.payload.data(), networkEvent.payload.size());
                    }
                    realtimeCanvasPending = false;
                    if (loaded)
                    {
                        fullCanvas = canvas.pixels;
                        offsetX = offsetY = 0;
                        clampOffsets(offsetX, offsetY);
                        canvas.markFullDirty();
                        Renderer::invalidateMinimap();
                        syncSelectedChannel();
                        sessionAwaitingSnapshot = false;
                        sessionSnapshotDeadline = 0;
                        gDisconnectReason[0] = '\0';
                        setAdminNotice(updateAvailable ? "RECONNECTED - UPDATE AVAILABLE" : "CONNECTED");
                        topMode = supportOnlyMode ? TOP_MODE_TICKETS : TOP_MODE_CANVAS;
                        topRenderFrame = 10;
                        printf("Loaded WebSocket canvas %s.\n", canvas.channel);
                    }
                    else
                    {
                        sessionAwaitingSnapshot = false;
                        sessionSnapshotDeadline = 0;
                        snprintf(gDisconnectReason, sizeof(gDisconnectReason), "CANVAS LOAD FAILED - RETRYING");
                        setAdminNotice("CANVAS LOAD FAILED");
                        topMode = TOP_MODE_STATUS;
                        NetworkManager::reconnect();
                    }
                    continue;
                }

                if (!networkEvent.payload.empty() &&
                    processBinaryCanvasPackets(networkEvent.payload.data(), networkEvent.payload.size(), canvas,
                                               fullCanvas, canvasWidth, canvasHeight, activeDrawLabels))
                {
                    canvas.markFullDirty();
                    Renderer::invalidateMinimap();
                }
            }
        }

        if (sessionAwaitingSnapshot && !supportOnlyMode &&
            sessionSnapshotDeadline > 0 && osGetTime() >= sessionSnapshotDeadline)
        {
            sessionAwaitingSnapshot = false;
            sessionSnapshotDeadline = 0;
            realtimeCanvasPending = false;
            UIState::clearPoints();
            snprintf(gDisconnectReason, sizeof(gDisconnectReason), "SYNC TIMED OUT - RETRYING");
            setAdminNotice("SYNC TIMED OUT - RETRYING");
            topMode = TOP_MODE_STATUS;
            topRenderFrame = 10;
            NetworkManager::reconnect();
        }

        // Rendering
        canvas.offsetX = offsetX;
        canvas.offsetY = offsetY;
        bool canvasWasDirty = canvas.dirty.valid;
        Renderer::renderViewport(canvas, buffer, fbWidth, fbHeight, false);
        if (zoomOverlayActive)
            drawZoomOverlay(buffer, fbWidth, fbHeight, zoomOverlayLeft);
        if (pendingAdminRectTool != ADMIN_RECT_NONE && adminRectDragging)
        {
            int startScreenX = (int)((adminRectStartX - canvas.offsetX) * canvas.zoomScale());
            int startScreenY = (int)((adminRectStartY - canvas.offsetY) * canvas.zoomScale());
            int endScreenX = (int)((adminRectEndX - canvas.offsetX) * canvas.zoomScale());
            int endScreenY = (int)((adminRectEndY - canvas.offsetY) * canvas.zoomScale());
            int rectX = std::max(0, std::min(startScreenX, endScreenX));
            int rectY = std::max(0, std::min(startScreenY, endScreenY));
            int rectMaxX = std::min(fbHeight - 1, std::max(startScreenX, endScreenX));
            int rectMaxY = std::min(fbWidth - 1, std::max(startScreenY, endScreenY));
            if (rectMaxX >= rectX && rectMaxY >= rectY)
            {
                drawRectOutline(buffer, fbWidth, fbHeight, rectX, rectY, rectMaxX - rectX + 1, rectMaxY - rectY + 1, 24, 33, 38);
                if (rectMaxX - rectX > 4 && rectMaxY - rectY > 4)
                    drawRectOutline(buffer, fbWidth, fbHeight, rectX + 2, rectY + 2, rectMaxX - rectX - 3, rectMaxY - rectY - 3,
                                    currentColor.r, currentColor.g, currentColor.b);
            }
        }
        drawActiveDrawLabels(buffer, fbWidth, fbHeight, canvas, activeDrawLabels);
        if (restrictionActive && !supportOnlyMode && topMode == TOP_MODE_CANVAS)
        {
            char remaining[48];
            if (restrictionHasDuration)
                snprintf(remaining, sizeof(remaining), "MUTED - %02d:%02d:%02d LEFT", restrictionSecondsRemaining / 3600, (restrictionSecondsRemaining / 60) % 60, restrictionSecondsRemaining % 60);
            else
                snprintf(remaining, sizeof(remaining), "MUTED - NO AUTOMATIC EXPIRATION");
            fillRect(buffer, fbWidth, fbHeight, 18, 16, 284, 54, 248, 250, 251);
            drawRectOutline(buffer, fbWidth, fbHeight, 18, 16, 284, 54, 196, 92, 40);
            drawMiniText(buffer, fbWidth, fbHeight, 28, 28, remaining, 196, 61, 61);
            char compactReason[43];
            snprintf(compactReason, sizeof(compactReason), "%.42s", restrictionReason);
            drawMiniText(buffer, fbWidth, fbHeight, 28, 48, compactReason, 32, 36, 42);
        }
        if (gDisconnectReason[0])
        {
            fillRect(buffer, fbWidth, fbHeight, 28, 74, 264, 88, 248, 250, 251);
            drawRectOutline(buffer, fbWidth, fbHeight, 28, 74, 264, 88, 196, 204, 212);
            drawMiniText(buffer, fbWidth, fbHeight, 48, 92, "DISCONNECTED", 196, 61, 61);
            drawMiniText(buffer, fbWidth, fbHeight, 48, 110, gDisconnectReason, 32, 36, 42);
            drawMiniText(buffer, fbWidth, fbHeight, 48, 136, "A RECONNECT   B MENU", 13, 122, 117);
        }
        canvas.clearDirty();

        bool activelyDrawing = !UIState::isColorPickerActive() &&
                               topMode == TOP_MODE_CANVAS &&
                               !(kHeld & (KEY_DLEFT | KEY_A)) &&
                               (kHeld & KEY_TOUCH);
        topRenderFrame++;
        if (identityNoticeFrames > 0)
        {
            identityNoticeFrames--;
            if (identityNoticeFrames == 0)
                topRenderFrame = 10;
        }
        if (adminNoticeFrames > 0)
        {
            adminNoticeFrames--;
            if (adminNoticeFrames == 0)
                topRenderFrame = 10;
        }
        if (ticketNoticeFrames > 0)
        {
            ticketNoticeFrames--;
            if (ticketNoticeFrames == 0)
                topRenderFrame = 10;
        }
        if (!activelyDrawing && (topRenderFrame >= 10 || canvasWasDirty))
        {
            bool renderStaffScope = ticketView == 0 ? isModOrAdmin() : ticketStaffScope;
            int renderedOpenCount = isModOrAdmin() ? ticketStaffNeedsReply : 0;
            const char *renderedIdentityStatus = restrictionActive ? (supportOnlyMode ? "banned" : "muted") : identityInfo.status;
            Renderer::renderTop(canvas, NetworkManager::isConnected(), updateAvailable, currentColor,
                                currentBrushSize, currentBrushShape, topMode,
                                availableChannels, availableChannelCount, selectedChannel,
                                selectedMenuItem, connectedUsers, connectedUserCount,
                                identityInfo.displayName, identityInfo.username,
                                identityInfo.role, renderedIdentityStatus,
                                identityInfo.backupCode, identityNoticeFrames > 0 ? identityNotice : "",
                                gIdentityBootStatus, selectedAdminItem,
                                adminNoticeFrames > 0 ? adminNotice : "",
                                gRequiredRulesVersion[0] ? gRequiredRulesVersion : gIdentity.rulesAcceptedVersion,
                                gNeedsRules,
                                ticketList, ticketListCount, ticketSelected,
                                ticketView, renderStaffScope,
                                activeTicket.id > 0 ? &activeTicket : NULL,
                                ticketMessages, ticketMessageCount,
                                staffChatMessages, staffChatMessageCount,
                                ticketHomeSelected, ticketActionSelected,
                                supportOnlyMode, supportOnlyReasonText,
                                ticketNoticeFrames > 0 ? ticketNotice : "", renderedOpenCount,
                                isModOrAdmin() ? staffChatUnread : 0,
                                restrictionSecondsRemaining, restrictionHasDuration,
                                restrictionReason);
            if (topMode == TOP_MODE_RULES)
                rulesRenderedSinceKeyboard = true;
            topRenderFrame = 0;
        }

        gspWaitForVBlank();
        Renderer::presentTopFrame();
        fb = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL);
        memcpy(fb, buffer, bufferSize);
        if (UIState::isColorPickerActive())
        {
            if (!isModOrAdmin() && pickerTab == 1)
                pickerTab = 0;
            drawToolPalette(fb, fbWidth, fbHeight, pickerTab, isModOrAdmin(), currentColor,
                            adminNoticeFrames > 0 ? adminNotice : "");
        }

        if (!UIState::isColorPickerActive())
            UIInterface::drawCurrentSelection(fb, fbWidth, fbHeight, currentColor);

        gfxFlushBuffers();
        gfxSwapBuffers();
    }

    NetworkManager::disconnect();
    free(buffer);
    aptUnhook(&aptCookie);
    gfxExit();
    return 0;
}
