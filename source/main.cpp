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
#include <sys/stat.h>
#include <errno.h>
#include "ui.h"
#include "network.h"
#include "canvas_state.h"
#include "renderer.h"
#include "protocol.h"
#include "updater.h"
#include "ui_canvas.h"
#include "ui_route.h"
#include "client_settings.h"
#include "input_bindings.h"

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
int currentBrushSizeTenths = Doodle::CLIENT_BRUSH_SIZE_MIN_TENTHS;
int currentBrushShape = 0;
static const int BRUSH_CIRCLE = 0;
static const int BRUSH_SQUARE = 1;
static const int BRUSH_DITHER = 2;
static const int BRUSH_ERASER = 3;
static bool gRainbowEnabled = false;
static bool gRainbowStrokeColorValid = false;
static Color gRainbowStrokeColor = {255, 0, 0};
static Color gPreviousColor = {255, 255, 255};
static Color gCustomPalette[8] = {
    {0, 0, 0},
    {255, 255, 255},
    {229, 57, 53},
    {246, 201, 69},
    {63, 185, 80},
    {37, 194, 199},
    {57, 119, 232},
    {198, 75, 201},
};
static bool gPaletteAssignMode = false;
static Doodle::ClientSettings gClientSettings;
static char gPreferredChannel[Doodle::CLIENT_CHANNEL_CAPACITY] = "";

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
// A compact protocol-6 presence envelope can contain 24 grouped sessions.
// Keep this bounded, but large enough that the negotiated response is not
// silently discarded before Protocol gets a chance to validate it.
static const size_t CONTROL_LINE_CAPACITY = 12 * 1024;
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
    char hello[768];
    Protocol::buildHello(hello, sizeof(hello), APP_ID, APP_VERSION, UPDATER_ENABLED != 0,
                         gIdentity.deviceId, gIdentity.deviceSecret, gHardwareId, gDeviceModel,
                         gIdentity.displayName, packageType,
                         gPreferredChannel[0] ? gPreferredChannel : NULL);
    return NetworkManager::sendSessionHello(hello, strlen(hello));
}

static void rememberSuccessfulChannel(const char *channel)
{
    if (!channel || !channel[0] ||
        !Doodle::setLastSuccessfulChannel(gClientSettings, channel))
        return;
    gClientSettings.brushShape = (Doodle::ClientBrushShape)std::max(
        0, std::min(currentBrushShape, (int)Doodle::CLIENT_BRUSH_SHAPE_COUNT - 1));
    gClientSettings.brushSizeTenths = std::max(
        Doodle::CLIENT_BRUSH_SIZE_MIN_TENTHS,
        std::min(currentBrushSizeTenths, Doodle::CLIENT_BRUSH_SIZE_MAX_TENTHS));
    gClientSettings.solidColor.r = currentColor.r;
    gClientSettings.solidColor.g = currentColor.g;
    gClientSettings.solidColor.b = currentColor.b;
    for (int slot = 0; slot < Doodle::CLIENT_PALETTE_COLOR_COUNT; ++slot)
    {
        gClientSettings.palette[slot].r = gCustomPalette[slot].r;
        gClientSettings.palette[slot].g = gCustomPalette[slot].g;
        gClientSettings.palette[slot].b = gCustomPalette[slot].b;
    }
    snprintf(gPreferredChannel, sizeof(gPreferredChannel), "%s", channel);
    Doodle::SettingsSaveResult result = Doodle::saveClientSettings(gClientSettings);
    if (result != Doodle::SETTINGS_SAVE_OK)
        printf("Could not remember channel: %s\n",
               Doodle::settingsSaveResultLabel(result));
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

static int clampBrushSizeTenths(int sizeTenths)
{
    return std::max(Doodle::CLIENT_BRUSH_SIZE_MIN_TENTHS,
                    std::min(sizeTenths, Doodle::CLIENT_BRUSH_SIZE_MAX_TENTHS));
}

static int brushDirtyRadius(int sizeTenths)
{
    return std::max(1, (clampBrushSizeTenths(sizeTenths) + 19) / 20);
}

static int clampRenderedBrushSizeTenths(int sizeTenths)
{
    return std::max(Doodle::CLIENT_BRUSH_SIZE_MIN_TENTHS,
                    std::min(sizeTenths, 310));
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

static void syncPickerToColor(const Color &color)
{
    float h, s, v;
    UIState::RGBtoHSV(color.r / 255.0f, color.g / 255.0f, color.b / 255.0f, h, s, v);
    UIState::updateHSV(h, s, v);
}

static int effectiveBrushShape()
{
    return currentBrushShape == BRUSH_ERASER ? BRUSH_CIRCLE : currentBrushShape;
}

u8 clampColor(float colorValue)
{
    return static_cast<u8>(std::max(0.0f, std::min(255.0f, colorValue)));
}

static bool brushContainsPixelAtWholeSize(int centerX, int centerY,
                                         int x, int y, int size, int shape)
{
    static const int bayer4[4][4] = {
        {0, 8, 2, 10},
        {12, 4, 14, 6},
        {3, 11, 1, 9},
        {15, 7, 13, 5},
    };
    const int extent = size / 2;
    if (abs(x) > extent || abs(y) > extent)
        return false;

    if (shape == BRUSH_SQUARE)
        return true;

    const int radius = shape == BRUSH_DITHER ? std::max(1, extent) : extent;
    const int dist2 = x * x + y * y;
    if (dist2 > radius * radius)
        return false;
    if (shape != BRUSH_DITHER)
        return true;

    const float dist = sqrtf((float)dist2);
    const float coverage = 1.0f - (dist / (float)(radius + 1));
    const int threshold = bayer4[(centerY + y) & 3][(centerX + x) & 3];
    return (int)(coverage * 16.0f) > threshold;
}

void drawBrush(u8 *buffer, int fbWidth, int fbHeight, int centerX, int centerY,
               int sizeTenths, int shape, u8 r, u8 g, u8 b)
{
    static const int bayer4[4][4] = {
        {0, 8, 2, 10},
        {12, 4, 14, 6},
        {3, 11, 1, 9},
        {15, 7, 13, 5},
    };
    sizeTenths = clampRenderedBrushSizeTenths(sizeTenths);
    const int lowerSize = sizeTenths / 10;
    const int fraction = sizeTenths % 10;
    const int upperSize = std::min(31, lowerSize + (fraction ? 1 : 0));
    const int extent = upperSize / 2;

    for (int y = -extent; y <= extent; y++)
    {
        for (int x = -extent; x <= extent; x++)
        {
            const bool lowerContains =
                brushContainsPixelAtWholeSize(centerX, centerY, x, y, lowerSize, shape);
            const bool upperContains = fraction > 0 &&
                brushContainsPixelAtWholeSize(centerX, centerY, x, y, upperSize, shape);
            const int transitionThreshold =
                (bayer4[(centerY + y) & 3][(centerX + x) & 3] * 10) / 16;
            if (lowerContains ||
                (upperContains && !lowerContains && transitionThreshold < fraction))
                drawPointOnBuffer(buffer, fbWidth, fbHeight,
                                  centerX + x, centerY + y, r, g, b);
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
              currentBrushSizeTenths, effectiveBrushShape(),
              drawColor.r, drawColor.g, drawColor.b);
    canvas.markDirty(canvasX, canvasY, brushDirtyRadius(currentBrushSizeTenths));
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
                  currentBrushSizeTenths, effectiveBrushShape(),
                  drawColor.r, drawColor.g, drawColor.b);
        canvas.markDirty(x, y, brushDirtyRadius(currentBrushSizeTenths));
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
                  currentBrushSizeTenths, effectiveBrushShape(),
                  drawColor.r, drawColor.g, drawColor.b);
        canvas.markDirty(x, y, brushDirtyRadius(currentBrushSizeTenths));
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
    int sizeTenths = packet[0] == 4 ? packet[4] : packet[4] * 10;
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
                drawBrush(fullCanvas, canvasWidth, canvasHeight, drawX, drawY,
                          sizeTenths, shape, r, g, b);
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
    if (type == 1 || type == 4)
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

static void sendDrawBatchCommand(const std::vector<DrawPoint> &points, const Color &color,
                                 int sizeTenths, int shape)
{
    if (!NetworkManager::checkConnection() || points.empty())
        return;

    const size_t maxPointsPerPacket = 64;
    size_t start = 0;
    while (start < points.size())
    {
        size_t count = std::min(maxPointsPerPacket, points.size() - start);
        uint8_t packet[7 + maxPointsPerPacket * 4];
        packet[0] = 4; // Type: drawBatch with size encoded in tenths.
        packet[1] = color.r;
        packet[2] = color.g;
        packet[3] = color.b;
        packet[4] = (uint8_t)clampBrushSizeTenths(sizeTenths);
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

static void drawColorSquare(u8 *framebuffer, int fbWidth, int fbHeight, int x, int y, int w, int h,
                            float hue, float saturation, float value)
{
    const int innerWidth = std::max(1, w - 2);
    const int innerHeight = std::max(1, h - 2);
    for (int py = 0; py < innerHeight; py++)
    {
        const float v = 1.0f - UiGeometry::normalizedPositionClamped(
            py, 0, innerHeight - 1);
        for (int px = 0; px < innerWidth; px++)
        {
            const float s = UiGeometry::normalizedPositionClamped(
                px, 0, innerWidth - 1);
            float rr, gg, bb;
            UIState::HSVtoRGB(hue, s, v, rr, gg, bb);
            putBufferScreenPixel(
                framebuffer, fbWidth, fbHeight, x + 1 + px, y + 1 + py,
                clampColor(std::round(rr * 255.0f)),
                clampColor(std::round(gg * 255.0f)),
                clampColor(std::round(bb * 255.0f)));
        }
    }
    strokeBufferScreenRect(framebuffer, fbWidth, fbHeight, x, y, w, h, 24, 33, 38);
    const int knobX = x + 1 + std::max(
        0, std::min(innerWidth - 1,
                    (int)std::round(saturation * (float)(innerWidth - 1))));
    const int knobY = y + 1 + std::max(
        0, std::min(innerHeight - 1,
                    (int)std::round((1.0f - value) * (float)(innerHeight - 1))));
    strokeBufferScreenRect(framebuffer, fbWidth, fbHeight, knobX - 4, knobY - 4, 9, 9, 245, 248, 250);
    strokeBufferScreenRect(framebuffer, fbWidth, fbHeight, knobX - 3, knobY - 3, 7, 7, 24, 33, 38);
}

static void drawHueStrip(u8 *framebuffer, int fbWidth, int fbHeight, int x, int y, int w, int h, float hue)
{
    const int innerWidth = std::max(1, w - 2);
    for (int px = 0; px < innerWidth; px++)
    {
        const float hh = UiGeometry::normalizedPositionClamped(
            px, 0, innerWidth - 1);
        float rr, gg, bb;
        UIState::HSVtoRGB(hh, 1.0f, 1.0f, rr, gg, bb);
        fillBufferScreenRect(
            framebuffer, fbWidth, fbHeight, x + 1 + px, y + 1, 1,
            std::max(1, h - 2),
            clampColor(std::round(rr * 255.0f)),
            clampColor(std::round(gg * 255.0f)),
            clampColor(std::round(bb * 255.0f)));
    }
    strokeBufferScreenRect(framebuffer, fbWidth, fbHeight, x, y, w, h, 24, 33, 38);
    const int knobX = x + 1 + std::max(
        0, std::min(innerWidth - 1,
                    (int)std::round(hue * (float)(innerWidth - 1))));
    fillBufferScreenRect(framebuffer, fbWidth, fbHeight, knobX - 2, y - 3, 5, h + 6, 245, 248, 250);
    strokeBufferScreenRect(framebuffer, fbWidth, fbHeight, knobX - 2, y - 3, 5, h + 6, 24, 33, 38);
}

static const int PICKER_COLOR_FIELD_X = 8;
static const int PICKER_COLOR_FIELD_Y = 108;
static const int PICKER_COLOR_FIELD_SIZE = 92;
static const int PICKER_COLOR_FIELD_INNER_X = PICKER_COLOR_FIELD_X + 1;
static const int PICKER_COLOR_FIELD_INNER_Y = PICKER_COLOR_FIELD_Y + 1;
static const int PICKER_COLOR_FIELD_INNER_SIZE = PICKER_COLOR_FIELD_SIZE - 2;
static const int PICKER_COLOR_FIELD_HIT_SLOP = 4;
static const int PICKER_HUE_STRIP_Y = 214;
static const int PICKER_HUE_STRIP_HEIGHT = 12;
static const int PICKER_SIZE_SLIDER_X = 82;
static const int PICKER_SIZE_SLIDER_Y = 84;
static const int PICKER_SIZE_SLIDER_WIDTH = 220;
static const int PICKER_SIZE_SLIDER_HIT_Y = 72;
static const int PICKER_SIZE_SLIDER_HIT_HEIGHT = 32;

static void drawToolPalette(u8 *framebuffer, int fbWidth, int fbHeight, int activeTab,
                            bool modAllowed, Color color, const char *notice)
{
    UiCanvas ui(framebuffer, fbWidth, fbHeight, UI_BUFFER_3DS_ROTATED_BGR);
    ui.fill(UiRect(0, 0, 320, 240), UiTheme::Ink);
    UiComponents::panel(ui, UiRect(4, 4, 312, 232), false);
    UiComponents::tab(ui, UiRect(8, 8, 92, 28), "Draw", activeTab == 0);
    if (modAllowed)
        UiComponents::tab(ui, UiRect(104, 8, 92, 28), "Staff", activeTab == 1);

    if (activeTab == 0)
    {
        static const char *shapeLabels[] = {"Circle", "Square", "Dither", "Eraser"};
        for (int shape = 0; shape < 4; ++shape)
        {
            UiComponents::button(ui, UiRect(8 + shape * 76, 42, 72, 28), shapeLabels[shape],
                                 currentBrushShape == shape, shape == BRUSH_ERASER);
        }

        char sizeLabel[32];
        snprintf(sizeLabel, sizeof(sizeLabel), "Size %d.%d",
                 currentBrushSizeTenths / 10, currentBrushSizeTenths % 10);
        ui.text(10, 82, sizeLabel, UiTheme::Secondary);
        const int sliderTravel = PICKER_SIZE_SLIDER_WIDTH - 1;
        const int sliderFill = (currentBrushSizeTenths - Doodle::CLIENT_BRUSH_SIZE_MIN_TENTHS) *
                               sliderTravel /
                               (Doodle::CLIENT_BRUSH_SIZE_MAX_TENTHS -
                                Doodle::CLIENT_BRUSH_SIZE_MIN_TENTHS);
        ui.fill(UiRect(PICKER_SIZE_SLIDER_X, PICKER_SIZE_SLIDER_Y,
                       PICKER_SIZE_SLIDER_WIDTH, 6), UiTheme::Border);
        ui.fill(UiRect(PICKER_SIZE_SLIDER_X, PICKER_SIZE_SLIDER_Y,
                       sliderFill + 1, 6), UiTheme::Accent);
        ui.fill(UiRect(PICKER_SIZE_SLIDER_X + sliderFill - 3,
                       PICKER_SIZE_SLIDER_Y - 5, 7, 16), UiTheme::White);
        ui.stroke(UiRect(PICKER_SIZE_SLIDER_X + sliderFill - 3,
                         PICKER_SIZE_SLIDER_Y - 5, 7, 16), UiTheme::Ink);

        float h, s, v;
        UIState::getHSV(h, s, v);
        drawColorSquare(framebuffer, fbWidth, fbHeight,
                        PICKER_COLOR_FIELD_X, PICKER_COLOR_FIELD_Y,
                        PICKER_COLOR_FIELD_SIZE, PICKER_COLOR_FIELD_SIZE,
                        h, s, v);
        drawHueStrip(framebuffer, fbWidth, fbHeight,
                     PICKER_COLOR_FIELD_X, PICKER_HUE_STRIP_Y,
                     PICKER_COLOR_FIELD_SIZE, PICKER_HUE_STRIP_HEIGHT, h);

        ui.text(110, 108, "Now", UiTheme::Secondary);
        ui.fill(UiRect(110, 120, 30, 28), UiColor(color.r, color.g, color.b));
        ui.stroke(UiRect(110, 120, 30, 28), UiTheme::Ink);
        ui.text(110, 154, "Prev", UiTheme::Secondary);
        ui.fill(UiRect(110, 166, 30, 28), UiColor(gPreviousColor.r, gPreviousColor.g, gPreviousColor.b));
        ui.stroke(UiRect(110, 166, 30, 28), UiTheme::Ink);

        for (int slot = 0; slot < 8; ++slot)
        {
            const int column = slot % 4;
            const int row = slot / 4;
            UiRect swatch(150 + column * 39, 108 + row * 39, 34, 34);
            ui.fill(swatch, UiColor(gCustomPalette[slot].r, gCustomPalette[slot].g, gCustomPalette[slot].b));
            const bool selected = sameColor(color, gCustomPalette[slot]);
            ui.stroke(swatch, gPaletteAssignMode ? UiTheme::Warning :
                              (selected ? UiTheme::Accent : UiTheme::Ink),
                      selected || gPaletteAssignMode ? 2 : 1);
            char slotLabel[3];
            snprintf(slotLabel, sizeof(slotLabel), "%d", slot + 1);
            const int brightness = (int)gCustomPalette[slot].r + gCustomPalette[slot].g + gCustomPalette[slot].b;
            ui.text(swatch.x + 4, swatch.y + 4, slotLabel,
                    brightness > 420 ? UiTheme::Ink : UiTheme::White);
        }

        UiComponents::button(ui, UiRect(110, 204, 46, 28), gPaletteAssignMode ? "Pick" : "Save",
                             gPaletteAssignMode);
        UiComponents::button(ui, UiRect(158, 204, 48, 28), "Reset", false);
        UiComponents::button(ui, UiRect(208, 204, 46, 28), "Hex", false);
        UiComponents::button(ui, UiRect(256, 204, 52, 28),
                             gRainbowEnabled ? "Rainbow*" : "Rainbow", gRainbowEnabled);
        if (notice && notice[0])
            UiComponents::toast(ui, notice, UiTheme::Accent);
    }
    else if (modAllowed)
    {
        UiComponents::button(ui, UiRect(8, 48, 148, 38), "Snapshot", false);
        UiComponents::button(ui, UiRect(164, 48, 148, 38), "Clear", false, true);
        UiComponents::button(ui, UiRect(8, 94, 148, 38), "Fill select", false);
        UiComponents::button(ui, UiRect(164, 94, 148, 38), "Erase select", false, true);
        UiComponents::panel(ui, UiRect(8, 142, 304, 62), true);
        ui.text(20, 154, "Canvas changes apply to", UiTheme::Secondary);
        ui.text(20, 168, "the current channel.", UiTheme::Secondary);
        ui.textClipped(20, 186, notice && notice[0] ? notice : "Choose a staff action.",
                       notice && notice[0] ? UiTheme::Warning : UiTheme::Ink, 280);
        UiComponents::actionBar(ui, "A/Touch Use", "", "B Close");
    }
}

static bool handleHexColorInput(Color &result)
{
    SwkbdState swkbd;
    char inputText[9] = "";
    char initial[8];
    snprintf(initial, sizeof(initial), "%02X%02X%02X", result.r, result.g, result.b);
    swkbdInit(&swkbd, SWKBD_TYPE_NORMAL, 2, 7);
    swkbdSetHintText(&swkbd, "RRGGBB or #RRGGBB");
    swkbdSetInitialText(&swkbd, initial);
    if (swkbdInputText(&swkbd, inputText, sizeof(inputText)) != SWKBD_BUTTON_CONFIRM)
        return false;

    const char *hex = inputText[0] == '#' ? inputText + 1 : inputText;
    if (strlen(hex) != 6)
        return false;
    for (int i = 0; i < 6; ++i)
        if (!((hex[i] >= '0' && hex[i] <= '9') ||
              (hex[i] >= 'a' && hex[i] <= 'f') ||
              (hex[i] >= 'A' && hex[i] <= 'F')))
            return false;

    unsigned int value = 0;
    if (sscanf(hex, "%x", &value) != 1)
        return false;
    result.r = (value >> 16) & 0xFF;
    result.g = (value >> 8) & 0xFF;
    result.b = value & 0xFF;
    return true;
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

static int rootMenuItemCount(bool staff)
{
    return staff ? 8 : 7;
}

static const char *rootMenuItemLabel(int index, bool staff)
{
    static const char *USER_ITEMS[] = {
        "Channels", "People", "Support", "Profile",
        "Options", "Help & Rules", "Exit"};
    static const char *STAFF_ITEMS[] = {
        "Channels", "People", "Support", "Staff Center",
        "Profile", "Options", "Help & Rules", "Exit"};
    const int count = rootMenuItemCount(staff);
    if (index < 0 || index >= count)
        return "";
    return staff ? STAFF_ITEMS[index] : USER_ITEMS[index];
}

static bool firstBindableButton(u32 keys, Doodle::ButtonToken &button)
{
    static const u32 ORDER[] = {
        KEY_A, KEY_B, KEY_X, KEY_Y, KEY_L, KEY_R, KEY_START,
        KEY_DUP, KEY_DDOWN, KEY_DLEFT, KEY_DRIGHT};
    for (size_t index = 0; index < sizeof(ORDER) / sizeof(ORDER[0]); ++index)
    {
        if ((keys & ORDER[index]) && Doodle::buttonFromKeyMask(ORDER[index], button))
            return true;
    }
    return false;
}

static const char *peopleName(const PresenceUser &person)
{
    return person.displayName[0] ? person.displayName :
           person.username[0] ? person.username : "Anonymous viewer";
}

static int caseInsensitiveCompare(const char *left, const char *right)
{
    const unsigned char *a = (const unsigned char *)(left ? left : "");
    const unsigned char *b = (const unsigned char *)(right ? right : "");
    while (*a && *b)
    {
        unsigned char ca = (*a >= 'A' && *a <= 'Z') ? *a + ('a' - 'A') : *a;
        unsigned char cb = (*b >= 'A' && *b <= 'Z') ? *b + ('a' - 'A') : *b;
        if (ca != cb)
            return ca < cb ? -1 : 1;
        ++a;
        ++b;
    }
    return *a == *b ? 0 : (*a ? 1 : -1);
}

static bool isCurrentPerson(const PresenceUser &person,
                            const char *displayName, const char *username)
{
    if (username && username[0] && person.username[0])
        return strcmp(person.username, username) == 0;
    return displayName && displayName[0] && person.displayName[0] &&
           strcmp(person.displayName, displayName) == 0;
}

static int buildPeopleIndex(const PresenceUser *users, int userCount,
                            bool allChannels, const char *currentChannel,
                            const char *displayName, const char *username,
                            int *indices, int maxIndices)
{
    int count = 0;
    for (int index = 0; index < userCount && count < maxIndices; ++index)
    {
        if (!allChannels && currentChannel && currentChannel[0] &&
            strcmp(users[index].channel, currentChannel) != 0)
            continue;
        indices[count++] = index;
    }
    for (int index = 1; index < count; ++index)
    {
        const int candidate = indices[index];
        int insert = index - 1;
        while (insert >= 0)
        {
            const PresenceUser &left = users[candidate];
            const PresenceUser &right = users[indices[insert]];
            const bool leftAnonymous = !left.identityId[0] && !left.username[0];
            const bool rightAnonymous = !right.identityId[0] && !right.username[0];
            const bool leftStaff = strcmp(left.role, "admin") == 0 || strcmp(left.role, "mod") == 0;
            const bool rightStaff = strcmp(right.role, "admin") == 0 || strcmp(right.role, "mod") == 0;
            const int leftTier = isCurrentPerson(left, displayName, username) ? 0 :
                                 leftStaff ? 1 : leftAnonymous ? 3 : 2;
            const int rightTier = isCurrentPerson(right, displayName, username) ? 0 :
                                  rightStaff ? 1 : rightAnonymous ? 3 : 2;
            if (leftTier > rightTier ||
                (leftTier == rightTier &&
                 caseInsensitiveCompare(peopleName(left), peopleName(right)) >= 0))
                break;
            indices[insert + 1] = indices[insert];
            --insert;
        }
        indices[insert + 1] = candidate;
    }
    return count;
}

struct BottomUiViewModel
{
    TopScreenMode mode;
    bool staff;
    bool admin;
    int menuSelected;
    const char (*channels)[25];
    const ChannelInfo *channelInfo;
    int channelCount;
    int channelSelected;
    const char *currentChannel;
    const PresenceUser *users;
    int userCount;
    int personSelected;
    int peopleScroll;
    bool peopleAllChannels;
    bool peopleActionMode;
    bool reportUserSelectionMode;
    int peopleActionSelected;
    const char *viewerDisplayName;
    const char *viewerUsername;
    int staffSelected;
    int optionsPage;
    int optionsSelected;
    int optionsBindingSlot;
    bool optionsBindingCapture;
    bool optionsBindingConflict;
    const Doodle::ClientSettings *settings;
    int ticketView;
    int ticketHomeSelected;
    bool ticketStaffScope;
    bool supportOnly;
    const SupportTicketSummary *tickets;
    int ticketCount;
    int ticketSelected;
    int ticketActionSelected;
    bool ticketHasNext;
    bool ticketHasPrevious;
    bool ticketActiveIsUnban;
    bool backupCodeRevealed;
    int profileSelected;
    bool needsDisplayName;
    bool needsRules;
    const char *notice;
};

static void drawBottomRoute(u8 *framebuffer, int fbWidth, int fbHeight,
                            const BottomUiViewModel &view)
{
    UiCanvas ui(framebuffer, fbWidth, fbHeight, UI_BUFFER_3DS_ROTATED_BGR);
    ui.fill(UiRect(0, 0, 320, 240), UiTheme::Background);

    if (view.mode == TOP_MODE_MENU)
    {
        const int count = rootMenuItemCount(view.staff);
        for (int item = 0; item < count; ++item)
        {
            const bool danger = item == count - 1;
            if (danger)
                UiComponents::button(ui, UiRect(4, 4 + item * 28, 312, 28),
                                     rootMenuItemLabel(item, view.staff),
                                     item == view.menuSelected, true);
            else
                UiComponents::listRow(ui, UiRect(4, 4 + item * 28, 312, 28),
                                      rootMenuItemLabel(item, view.staff), "",
                                      item == view.menuSelected);
        }
        return;
    }

    if (view.mode == TOP_MODE_CHANNELS)
    {
        for (int item = 0; item < view.channelCount && item < 8; ++item)
        {
            char meta[18];
            const ChannelInfo *info = view.channelInfo ? &view.channelInfo[item] : NULL;
            const bool hasInfo = info && info->name[0];
            if (hasInfo && info->readOnly)
                snprintf(meta, sizeof(meta), "%d  READ", info->userCount);
            else if (hasInfo)
                snprintf(meta, sizeof(meta), "%d", info->userCount);
            else
                meta[0] = '\0';
            UiComponents::listRow(ui, UiRect(4, 4 + item * 28, 312, 28),
                                  view.channels[item], meta,
                                  item == view.channelSelected,
                                   view.currentChannel &&
                                       strcmp(view.channels[item], view.currentChannel) == 0,
                                   hasInfo && (info->adminOnly || info->staffOnly) && !view.staff);
        }
        UiComponents::actionBar(ui, "A Switch", "", "B Back");
        return;
    }

    if (view.mode == TOP_MODE_USERS)
    {
        if (view.peopleActionMode)
        {
            static const char *ACTIONS[] = {"Kick", "Mute 30m", "Unmute", "Ban"};
            ui.text(10, 12, "Moderation action", UiTheme::Secondary);
            for (int item = 0; item < 4; ++item)
                UiComponents::button(ui, UiRect(8, 34 + item * 42, 304, 36),
                                     ACTIONS[item], item == view.peopleActionSelected,
                                     item == 3, item == 3 && !view.admin);
            UiComponents::actionBar(ui, "A Continue", "", "B People");
            return;
        }
        UiComponents::tab(ui, UiRect(8, 4, 148, 30), "Current", !view.peopleAllChannels);
        UiComponents::tab(ui, UiRect(164, 4, 148, 30), "All Channels", view.peopleAllChannels);
        int indices[24];
        const int filteredCount = buildPeopleIndex(view.users, view.userCount,
                                                   view.peopleAllChannels,
                                                   view.currentChannel,
                                                   view.viewerDisplayName,
                                                   view.viewerUsername,
                                                   indices, 24);
        const int scroll = std::max(0, std::min(view.peopleScroll,
                                                std::max(0, filteredCount - 5)));
        for (int row = 0; row < 5; ++row)
        {
            const int filteredIndex = scroll + row;
            if (filteredIndex >= filteredCount)
                break;
            const PresenceUser &person = view.users[indices[filteredIndex]];
            char label[38];
            snprintf(label, sizeof(label), "%s%s",
                     person.identityId[0] ? "" : "Viewer: ",
                     person.displayName[0] ? person.displayName :
                     (person.username[0] ? person.username : "Anonymous"));
            char meta[24];
            if (person.sessionCount > 1)
                snprintf(meta, sizeof(meta), "%s x%d", person.role, person.sessionCount);
            else
                snprintf(meta, sizeof(meta), "%s", person.role);
            UiComponents::listRow(ui, UiRect(8, 40 + row * 32, 304, 30),
                                  label, meta, filteredIndex == view.personSelected);
        }
        UiComponents::actionBar(ui, view.reportUserSelectionMode ? "A Report user" :
                                (view.staff ? "A Actions" : ""),
                                "X Scope", "B Back");
        return;
    }

    if (view.mode == TOP_MODE_STAFF_CENTER)
    {
        static const char *ITEMS[] = {"Ticket Queue", "Staff Chat", "Canvas Tools"};
        static const char *META[] = {"needs staff", "team only", "current channel"};
        for (int item = 0; item < 3; ++item)
            UiComponents::listRow(ui, UiRect(8, 44 + item * 48, 304, 40),
                                  ITEMS[item], META[item],
                                  item == view.staffSelected);
        UiComponents::actionBar(ui, "A Open", "", "B Back");
        return;
    }

    if (view.mode == TOP_MODE_OPTIONS)
    {
        if (view.optionsPage == 0)
        {
            static const char *ITEMS[] = {
                "Controls & Presets", "Drawing & Palette", "Connection & About"};
            static const char *META[] = {"bindings", "color + zoom", APP_VERSION};
            for (int item = 0; item < 3; ++item)
                UiComponents::listRow(ui, UiRect(8, 44 + item * 48, 304, 40),
                                      ITEMS[item], META[item],
                                      item == view.optionsSelected);
            UiComponents::actionBar(ui, "A Open", "", "B Back");
        }
        else if (view.optionsPage == 1 && view.settings)
        {
            char preset[34];
            snprintf(preset, sizeof(preset), "< %s >",
                     Doodle::controlPresetLabel(view.settings->controlPreset));
            UiComponents::listRow(ui, UiRect(8, 8, 304, 28), "Preset", preset,
                                  view.optionsSelected == 0);
            for (int action = 0; action < Doodle::INPUT_ACTION_COUNT; ++action)
            {
                const Doodle::ActionBinding &binding = view.settings->bindings.action[action];
                char keys[32];
                snprintf(keys, sizeof(keys), "%s%s%s",
                         Doodle::buttonLabel(binding.button[0]),
                         binding.button[1] == Doodle::BUTTON_NONE ? "" : " / ",
                         binding.button[1] == Doodle::BUTTON_NONE ? "" :
                         Doodle::buttonLabel(binding.button[1]));
                UiComponents::listRow(ui, UiRect(8, 38 + action * 30, 304, 28),
                                      Doodle::inputActionLabel((Doodle::InputAction)action),
                                      keys, view.optionsSelected == action + 1);
                if (view.optionsSelected == action + 1)
                {
                    const int markerX = view.optionsBindingSlot == 0 ? 214 : 272;
                    ui.fill(UiRect(markerX, 62 + action * 30, 32, 2), UiTheme::Accent);
                }
            }
            UiComponents::actionBar(
                ui,
                view.optionsBindingCapture ? "Press a button" : "A Rebind",
                view.optionsBindingCapture ? "" : "X Slot / Y Clear",
                view.optionsBindingCapture ? "SELECT Cancel" : "B Back");
            if (view.optionsBindingCapture)
                UiComponents::toast(ui, "Press any bindable button", UiTheme::Accent);
        }
        else if (view.optionsPage == 2 && view.settings)
        {
            char side[24];
            snprintf(side, sizeof(side), "%s",
                     Doodle::zoomOverlaySideLabel(view.settings->zoomOverlaySide));
            UiComponents::listRow(ui, UiRect(8, 44, 304, 40),
                                  "Zoom overlay side", side, view.optionsSelected == 0);
            UiComponents::listRow(ui, UiRect(8, 92, 304, 40),
                                  "Open palette", "8 favorites", view.optionsSelected == 1);
            UiComponents::listRow(ui, UiRect(8, 140, 304, 40),
                                  "Reset palette", "defaults", view.optionsSelected == 2);
            UiComponents::actionBar(ui, "A Change", "", "B Back");
        }
        else
        {
            UiComponents::panel(ui, UiRect(8, 28, 304, 154), true);
            ui.text(20, 42, "Collab Doodle " APP_VERSION, UiTheme::Ink, 2);
            ui.text(20, 72, "Native protocol 6", UiTheme::Secondary);
            ui.text(20, 90, "Settings are stored on this device.", UiTheme::Secondary);
            ui.text(20, 108, "A reconnects and resyncs safely.", UiTheme::Secondary);
            UiComponents::button(ui, UiRect(20, 136, 280, 34), "Reconnect", true);
            UiComponents::actionBar(ui, "A Reconnect", "", "B Back");
        }
        return;
    }

    if (view.mode == TOP_MODE_TICKETS)
    {
        if (view.ticketView == 0)
        {
            static const char *USER_ITEMS[] = {
                "New bug request", "New feature request",
                "Report a user", "My tickets"};
            static const char *RESTRICTED_ITEMS[] = {
                "New appeal", "My appeals", "Profile", "Exit"};
            const char **items = view.supportOnly ? RESTRICTED_ITEMS : USER_ITEMS;
            const int count = 4;
            for (int item = 0; item < count; ++item)
                UiComponents::listRow(ui, UiRect(8, 8 + item * 34, 304, 32),
                                      items[item], "", item == view.ticketHomeSelected);
            UiComponents::actionBar(ui, "A Open", "", view.supportOnly ? "" : "B Back");
        }
        else if (view.ticketView == 1)
        {
            for (int row = 0; row < view.ticketCount && row < 6; ++row)
            {
                const SupportTicketSummary &ticket = view.tickets[row];
                char status[24];
                snprintf(status, sizeof(status), "#%d %s", ticket.id, ticket.status);
                UiComponents::listRow(ui, UiRect(8, 4 + row * 27, 304, 25),
                                      ticket.subject, status, row == view.ticketSelected);
            }
            UiComponents::button(ui, UiRect(8, 172, 94, 36), "Previous",
                                 false, false, !view.ticketHasPrevious);
            UiComponents::button(ui, UiRect(110, 172, 94, 36), "Refresh", false);
            UiComponents::button(ui, UiRect(212, 172, 100, 36), "Next",
                                 false, false, !view.ticketHasNext);
            UiComponents::actionBar(ui, "A Open", "L/X/Y Page", "B Back");
        }
        else if (view.ticketView == 2)
        {
            UiComponents::button(ui, UiRect(8, 164, 148, 40), "Reply", true);
            UiComponents::button(ui, UiRect(164, 164, 148, 40), "Staff actions",
                                 false, false, !view.ticketStaffScope);
            UiComponents::actionBar(ui, "A Reply", view.ticketStaffScope ? "X Staff" : "", "B Back");
        }
        else if (view.ticketView == 3)
        {
            static const char *ACTIONS[] = {
                "Mark in progress", "Reply to user", "Resolve",
                "Reject", "Approve unban", "Reopen"};
            for (int item = 0; item < 6; ++item)
                UiComponents::listRow(ui, UiRect(8, 4 + item * 31, 304, 29),
                                      ACTIONS[item], "", item == view.ticketActionSelected,
                                      false, item == 4 && !view.ticketActiveIsUnban);
            UiComponents::actionBar(ui, "A Apply", "", "B Thread");
        }
        else if (view.ticketView == 4)
        {
            UiComponents::button(ui, UiRect(8, 44, 304, 40), "Send message", true);
            UiComponents::button(ui, UiRect(8, 92, 304, 40), "Refresh chat",
                                 false, false, false);
            UiComponents::button(ui, UiRect(8, 140, 304, 40), "Load older",
                                 false, false, !view.ticketHasNext);
            UiComponents::actionBar(ui, "A Send", "X/Y Refresh/Older", "B Staff");
        }
        return;
    }

    if (view.mode == TOP_MODE_IDENTITY)
    {
        if (view.needsDisplayName)
        {
            UiComponents::panel(ui, UiRect(8, 34, 304, 138), true);
            ui.text(20, 48, "Welcome to Collab Doodle", UiTheme::Ink, 2);
            ui.wrappedText(20, 72,
                           "Create this device profile with a display name, or recover an existing account.",
                           UiTheme::Secondary, 280, 4, 11);
            UiComponents::button(ui, UiRect(8, 180, 148, 36), "Create / name", true);
            UiComponents::button(ui, UiRect(164, 180, 148, 36), "Recover",
                                 false, false, false);
            UiComponents::actionBar(ui, "A Create", "X Recover", "B Exit");
            return;
        }
        UiComponents::button(ui, UiRect(8, 46, 304, 38), "Change display name",
                             view.profileSelected == 0, false, view.supportOnly);
        UiComponents::button(ui, UiRect(8, 92, 148, 38),
                             view.backupCodeRevealed ? "Hide code" : "Reveal code",
                             view.profileSelected == 1);
        UiComponents::button(ui, UiRect(164, 92, 148, 38), "Rotate code",
                             view.profileSelected == 2, true, view.supportOnly);
        UiComponents::button(ui, UiRect(8, 138, 304, 38), "Recover account",
                             view.profileSelected == 3, false, view.supportOnly);
        UiComponents::actionBar(ui, view.supportOnly ? "A Reveal only" : "A Use",
                                "", "B Back");
        return;
    }

    if (view.mode == TOP_MODE_RULES)
    {
        UiComponents::button(ui, UiRect(24, 168, 272, 40),
                             view.needsRules ? "Accept rules" : "Done", true);
        UiComponents::actionBar(ui, view.needsRules ? "A Accept" : "A Done", "",
                                view.needsRules ? "B Exit" : "B Back");
        return;
    }

    if (view.mode == TOP_MODE_STATUS)
    {
        UiComponents::modal(ui, "Connection",
                            view.notice && view.notice[0] ? view.notice :
                            "The client is waiting for the server.",
                            "A Reconnect", "B Menu");
    }
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
    hidSetRepeatParameters(18, 5);
    Doodle::SettingsLoadResult settingsLoad = Doodle::SETTINGS_LOAD_DEFAULTS;
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
    // Identity initialization mounts the SD filesystem shared by both files.
    // Settings remain independent and never modify identity credentials.
    settingsLoad = Doodle::loadClientSettings(gClientSettings);
    currentBrushShape = (int)gClientSettings.brushShape;
    currentBrushSizeTenths = gClientSettings.brushSizeTenths;
    currentColor.r = gClientSettings.solidColor.r;
    currentColor.g = gClientSettings.solidColor.g;
    currentColor.b = gClientSettings.solidColor.b;
    for (int paletteIndex = 0; paletteIndex < Doodle::CLIENT_PALETTE_COLOR_COUNT; ++paletteIndex)
    {
        gCustomPalette[paletteIndex].r = gClientSettings.palette[paletteIndex].r;
        gCustomPalette[paletteIndex].g = gClientSettings.palette[paletteIndex].g;
        gCustomPalette[paletteIndex].b = gClientSettings.palette[paletteIndex].b;
    }
    snprintf(gPreferredChannel, sizeof(gPreferredChannel), "%s",
             gClientSettings.lastSuccessfulChannel);
    syncPickerToColor(currentColor);
    printf("Settings: %s (%s)\n", Doodle::clientSettingsPath(),
           Doodle::settingsLoadResultLabel(settingsLoad));
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
    UiRouteStack routeStack;
    int selectedChannel = 0;
    int selectedMenuItem = 0;
    int selectedAdminItem = 0;
    int optionsPage = 0;
    int optionsSelected = 0;
    int optionsBindingSlot = 0;
    bool optionsBindingCapture = false;
    bool optionsBindingConflict = false;
    Doodle::InputAction pendingBindingAction = Doodle::INPUT_ACTION_TOOLS;
    Doodle::ButtonToken pendingBindingButton = Doodle::BUTTON_NONE;
    Doodle::BindingConflict pendingBindingConflict;
    memset(&pendingBindingConflict, 0, sizeof(pendingBindingConflict));
    int selectedPerson = 0;
    int peopleScroll = 0;
    bool peopleAllChannels = false;
    bool peopleActionMode = false;
    bool reportUserSelectionMode = false;
    int peopleActionSelected = 0;
    bool moderationConfirmation = false;
    char pendingModerationAction[24] = "";
    char pendingModerationIdentity[40] = "";
    char pendingModerationTargetName[49] = "";
    char pendingModerationReason[81] = "";
    bool backupCodeRevealed = false;
    bool recoveryCodeExplanation = false;
    int profileSelected = 0;
    bool rotateBackupConfirmation = false;
    int pickerTab = 0;
    enum PickerDragTarget
    {
        PICKER_DRAG_NONE = 0,
        PICKER_DRAG_SIZE,
        PICKER_DRAG_COLOR,
        PICKER_DRAG_HUE,
    };
    PickerDragTarget pickerDragTarget = PICKER_DRAG_NONE;
    bool pickerReturnToRoute = false;
    bool shoulderEraserActive = false;
    int shoulderSavedBrushShape = currentBrushShape;
    int shoulderSavedBrushSizeTenths = currentBrushSizeTenths;
    char availableChannels[8][25];
    ChannelInfo availableChannelInfo[8];
    memset(availableChannelInfo, 0, sizeof(availableChannelInfo));
    int availableChannelCount = 0;
    char pendingChannelSwitch[25] = "";
    u64 pendingChannelSwitchDeadline = 0;
    PresenceUser connectedUsers[24];
    int connectedUserCount = 0;
    PresenceInfo connectedPresenceInfo;
    memset(&connectedPresenceInfo, 0, sizeof(connectedPresenceInfo));
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
    u64 adminNoticeExpiresAt = 0;
    bool suppressTouchUntilRelease = false;
    char pendingAdminAction[20] = "";
    bool pendingAdminLocalApply = false;
    int pendingAdminApplyX = 0;
    int pendingAdminApplyY = 0;
    int pendingAdminApplyW = 0;
    int pendingAdminApplyH = 0;
    Color pendingAdminApplyColor = {255, 255, 255};
    bool supportOnlyMode = false;
    char supportOnlyReasonText[81] = "";
    char supportOnlyBlockTypes[24] = "";
    SupportTicketSummary ticketList[6];
    memset(ticketList, 0, sizeof(ticketList));
    int ticketListCount = 0;
    int ticketSelected = 0;
    TicketCursor ticketNextCursor;
    memset(&ticketNextCursor, 0, sizeof(ticketNextCursor));
    TicketCursor ticketCurrentCursor;
    memset(&ticketCurrentCursor, 0, sizeof(ticketCurrentCursor));
    TicketCursor ticketCursorHistory[8];
    memset(ticketCursorHistory, 0, sizeof(ticketCursorHistory));
    int ticketCursorHistoryCount = 0;
    bool ticketListLoading = false;
    TicketCursor pendingTicketCurrentCursor;
    memset(&pendingTicketCurrentCursor, 0, sizeof(pendingTicketCurrentCursor));
    TicketCursor pendingTicketCursorHistory[8];
    memset(pendingTicketCursorHistory, 0, sizeof(pendingTicketCursorHistory));
    int pendingTicketCursorHistoryCount = 0;
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
    char ticketDraftCategory[12] = "";
    char ticketDraftSubject[65] = "";
    char ticketDraftDetails[241] = "";
    bool ticketDraftPreview = false;
    bool ticketDraftSendPending = false;
    char ticketReplyDraft[241] = "";
    int ticketReplyDraftTicketId = 0;
    bool ticketReplyDraftStaff = false;
    bool ticketReplyPreview = false;
    bool ticketReplySendPending = false;
    enum AdminRectTool {
        ADMIN_RECT_NONE = 0,
        ADMIN_RECT_FILL = 1,
        ADMIN_RECT_ERASE = 2,
    };
    AdminRectTool pendingAdminRectTool = ADMIN_RECT_NONE;
    bool adminRectDragging = false;
    bool adminRectAwaitingConfirm = false;
    bool confirmClearCanvas = false;
    bool confirmPaletteReset = false;
    bool confirmExit = false;
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
    bool clientSettingsDirty = false;
    u64 clientSettingsSaveAfter = 0;

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
        adminNoticeFrames = adminNotice[0] ? 90 : 0;
        adminNoticeExpiresAt = adminNotice[0] ? osGetTime() + 1500 : 0;
        topRenderFrame = 10;
    };

    auto clearAdminNotice = [&]()
    {
        adminNotice[0] = '\0';
        adminNoticeFrames = 0;
        adminNoticeExpiresAt = 0;
        topRenderFrame = 10;
    };

    auto requestExitConfirmation = [&]()
    {
        confirmExit = true;
        clearAdminNotice();
    };

    auto cancelTransientOverlaysForConnection = [&]()
    {
        ticketDraftPreview = false;
        ticketReplyPreview = false;
        moderationConfirmation = false;
        rotateBackupConfirmation = false;
        confirmClearCanvas = false;
        confirmPaletteReset = false;
        confirmExit = false;
        optionsBindingConflict = false;
        pendingAdminRectTool = ADMIN_RECT_NONE;
        adminRectDragging = false;
        adminRectAwaitingConfirm = false;
        gPaletteAssignMode = false;
        UIState::setColorPickerActive(false);
    };

    auto setTicketNotice = [&](const char *notice)
    {
        snprintf(ticketNotice, sizeof(ticketNotice), "%.63s", notice ? notice : "");
        ticketNoticeFrames = ticketNotice[0] ? 240 : 0;
        topRenderFrame = 10;
    };

    auto markClientSettingsDirty = [&]()
    {
        clientSettingsDirty = true;
        clientSettingsSaveAfter = osGetTime() + 500;
    };

    auto resetPaletteToDefaults = [&]()
    {
        Doodle::resetClientPalette(gClientSettings);
        for (int slot = 0; slot < Doodle::CLIENT_PALETTE_COLOR_COUNT; ++slot)
        {
            gCustomPalette[slot].r = gClientSettings.palette[slot].r;
            gCustomPalette[slot].g = gClientSettings.palette[slot].g;
            gCustomPalette[slot].b = gClientSettings.palette[slot].b;
        }
        gPaletteAssignMode = false;
        markClientSettingsDirty();
    };

    auto flushClientSettings = [&]() -> bool
    {
        const int persistedBrushShape = shoulderEraserActive
                                            ? shoulderSavedBrushShape
                                            : currentBrushShape;
        const int persistedBrushSizeTenths = shoulderEraserActive
                                                 ? shoulderSavedBrushSizeTenths
                                                 : currentBrushSizeTenths;
        gClientSettings.brushShape = (Doodle::ClientBrushShape)std::max(
            0, std::min(persistedBrushShape, (int)Doodle::CLIENT_BRUSH_SHAPE_COUNT - 1));
        gClientSettings.brushSizeTenths = clampBrushSizeTenths(persistedBrushSizeTenths);
        gClientSettings.solidColor.r = currentColor.r;
        gClientSettings.solidColor.g = currentColor.g;
        gClientSettings.solidColor.b = currentColor.b;
        for (int paletteIndex = 0; paletteIndex < Doodle::CLIENT_PALETTE_COLOR_COUNT; ++paletteIndex)
        {
            gClientSettings.palette[paletteIndex].r = gCustomPalette[paletteIndex].r;
            gClientSettings.palette[paletteIndex].g = gCustomPalette[paletteIndex].g;
            gClientSettings.palette[paletteIndex].b = gCustomPalette[paletteIndex].b;
        }
        Doodle::SettingsSaveResult result = Doodle::saveClientSettings(gClientSettings);
        clientSettingsDirty = result != Doodle::SETTINGS_SAVE_OK;
        if (!clientSettingsDirty)
            clientSettingsSaveAfter = 0;
        else
            clientSettingsSaveAfter = osGetTime() + 5000;
        if (result != Doodle::SETTINGS_SAVE_OK)
            printf("Settings save failed: %s\n", Doodle::settingsSaveResultLabel(result));
        return result == Doodle::SETTINGS_SAVE_OK;
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

    auto requestTicketList = [&](bool staff, const char *status, const char *category,
                                 const TicketCursor &cursor,
                                 const TicketCursor *history, int historyCount,
                                 const char *notice) -> bool
    {
        if (ticketListLoading)
        {
            setTicketNotice("WAIT FOR CURRENT PAGE");
            return false;
        }
        char command[192];
        Protocol::buildTicketList(command, sizeof(command), staff, status, category, cursor);
        if (!sendTicketCommand(command))
            return false;
        pendingTicketCurrentCursor = cursor;
        pendingTicketCursorHistoryCount = std::max(
            0, std::min(historyCount,
                        (int)(sizeof(pendingTicketCursorHistory) /
                              sizeof(pendingTicketCursorHistory[0]))));
        memset(pendingTicketCursorHistory, 0, sizeof(pendingTicketCursorHistory));
        if (history && pendingTicketCursorHistoryCount > 0)
            memcpy(pendingTicketCursorHistory, history,
                   sizeof(TicketCursor) * pendingTicketCursorHistoryCount);
        ticketListLoading = true;
        setTicketNotice(notice);
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
        TicketCursor parsedNextCursor;
        memset(&parsedNextCursor, 0, sizeof(parsedNextCursor));
        if (Protocol::parseTicketListEnd(jsonLine, parsedNextCursor))
        {
            ticketNextCursor = parsedNextCursor;
            if (ticketListLoading)
            {
                ticketCurrentCursor = pendingTicketCurrentCursor;
                ticketCursorHistoryCount = pendingTicketCursorHistoryCount;
                memcpy(ticketCursorHistory, pendingTicketCursorHistory,
                       sizeof(ticketCursorHistory));
                ticketListLoading = false;
            }
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
                if (strcmp(ticketAction, "create") == 0 && ticketDraftSendPending)
                {
                    ticketDraftSendPending = false;
                    ticketDraftSubject[0] = '\0';
                    ticketDraftDetails[0] = '\0';
                }
                if (strcmp(ticketAction, "reply") == 0 && ticketReplySendPending)
                {
                    ticketReplySendPending = false;
                    ticketReplyDraft[0] = '\0';
                }
                char command[128];
                if ((strcmp(ticketAction, "reply") == 0 || strcmp(ticketAction, "status") == 0 || strcmp(ticketAction, "approveUnban") == 0) && resultTicketId > 0)
                {
                    Protocol::buildTicketGet(command, sizeof(command), resultTicketId);
                    sendTicketCommand(command);
                }
                else if (strcmp(ticketAction, "create") == 0)
                {
                    TicketCursor firstPage;
                    memset(&firstPage, 0, sizeof(firstPage));
                    requestTicketList(false, "", supportOnlyMode ? "unban" : "",
                                      firstPage, NULL, 0, "REFRESHING TICKETS");
                }
            }
            else
            {
                if (strcmp(ticketAction, "list") == 0)
                    ticketListLoading = false;
                if (ticketDraftSendPending)
                {
                    ticketDraftSendPending = false;
                    ticketDraftPreview = true;
                }
                if (ticketReplySendPending)
                {
                    ticketReplySendPending = false;
                    ticketReplyPreview = true;
                }
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
        if (pendingChannelSwitch[0] && strstr(jsonLine, "\"type\":\"error\""))
        {
            pendingChannelSwitch[0] = '\0';
            pendingChannelSwitchDeadline = 0;
            setAdminNotice(strstr(jsonLine, "Unknown channel") ?
                               "CHANNEL UNAVAILABLE" : "CHANNEL SWITCH FAILED");
            topMode = TOP_MODE_CHANNELS;
            topRenderFrame = 10;
            return true;
        }
        if (ticketListLoading && strstr(jsonLine, "\"type\":\"error\""))
        {
            ticketListLoading = false;
            setTicketNotice("TICKET PAGE LOAD FAILED");
            return true;
        }
        if (Protocol::parseChannels(jsonLine, availableChannels, availableChannelInfo,
                                    8, availableChannelCount, currentChannel))
        {
            if (currentChannel[0])
                canvas.setChannel(currentChannel);
            syncSelectedChannel();
            topRenderFrame = 10;
            return true;
        }
        if (Protocol::parsePresence(jsonLine, connectedUsers, 24, connectedUserCount,
                                    connectedPresenceInfo))
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
                {
                    if (previousStage == ONBOARDING_SUBMITTING_RULES &&
                        strncmp(gIdentityBootStatus, "NEW", 3) == 0)
                    {
                        recoveryCodeExplanation = true;
                        backupCodeRevealed = true;
                        topMode = TOP_MODE_IDENTITY;
                    }
                    else
                        topMode = TOP_MODE_CANVAS;
                }
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
            if (strcmp(recoveryReason, "recovery-device-in-use") == 0)
                setIdentityNotice("DEVICE ALREADY HAS AN ACCOUNT");
            else
                setIdentityNotice("RECOVERY FAILED");
            printf("Recovery failed: %s\n", recoveryReason);
            return true;
        }
        if (strstr(jsonLine, "\"type\":\"adminCanvasResult\""))
        {
            if (strstr(jsonLine, "\"ok\":true"))
            {
                if (pendingAdminLocalApply)
                {
                    applyCanvasRectLocal(canvas, pendingAdminApplyX, pendingAdminApplyY,
                                         pendingAdminApplyW, pendingAdminApplyH,
                                         pendingAdminApplyColor);
                    Renderer::invalidateMinimap();
                }
                if (strcmp(pendingAdminAction, "snapshot") == 0)
                    setAdminNotice("SNAPSHOT SAVED");
                else if (strcmp(pendingAdminAction, "clear") == 0)
                    setAdminNotice("CHANNEL CLEARED");
                else if (strcmp(pendingAdminAction, "fillRect") == 0)
                    setAdminNotice("SELECTION FILLED");
                else if (strcmp(pendingAdminAction, "eraseRect") == 0)
                    setAdminNotice("SELECTION ERASED");
                else
                    setAdminNotice("STAFF ACTION COMPLETE");
            }
            else if (strstr(jsonLine, "rate-limited"))
                setAdminNotice("ACTION RATE LIMITED");
            else if (strstr(jsonLine, "admin-required"))
                setAdminNotice("ADMIN REQUIRED");
            else
                setAdminNotice("STAFF ACTION DENIED");
            pendingAdminAction[0] = '\0';
            pendingAdminLocalApply = false;
            return true;
        }
        ModerationResult moderationResult;
        if (Protocol::parseModerationResult(jsonLine, moderationResult))
        {
            if (moderationResult.ok)
            {
                char notice[40];
                snprintf(notice, sizeof(notice), "%s OK",
                         moderationResult.action[0] ? moderationResult.action : "MODERATION");
                setAdminNotice(notice);
            }
            else if (strcmp(moderationResult.error, "admin-required") == 0)
                setAdminNotice("ADMIN REQUIRED");
            else
                setAdminNotice(moderationResult.error[0] ? moderationResult.error : "MOD ACTION DENIED");
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
                    rememberSuccessfulChannel(canvas.channel);
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
    int circleNavRepeatFrames = 0;
    u32 circleNavDirection = 0;

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
        ticketListLoading = false;
        pendingAdminAction[0] = '\0';
        pendingAdminLocalApply = false;
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
        if (pendingAdminAction[0])
        {
            setAdminNotice("WAIT FOR CURRENT ACTION");
            return false;
        }
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
        snprintf(pendingAdminAction, sizeof(pendingAdminAction), "%s", action);
        pendingAdminLocalApply = strcmp(action, "clear") == 0 ||
                                 strcmp(action, "fillRect") == 0 ||
                                 strcmp(action, "eraseRect") == 0;
        if (pendingAdminLocalApply)
        {
            pendingAdminApplyX = x;
            pendingAdminApplyY = y;
            pendingAdminApplyW = w;
            pendingAdminApplyH = h;
            pendingAdminApplyColor = color;
        }
        if (strcmp(action, "snapshot") == 0)
            setAdminNotice("SNAPSHOT PENDING");
        else if (strcmp(action, "clear") == 0)
            setAdminNotice("CLEAR PENDING");
        else if (strcmp(action, "fillRect") == 0)
            setAdminNotice("FILL PENDING");
        else if (strcmp(action, "eraseRect") == 0)
            setAdminNotice("ERASE PENDING");
        else
            setAdminNotice("ACTION PENDING");
        return true;
    };

    auto switchToSelectedChannel = [&]() -> bool
    {
        if (availableChannelCount <= 0)
            return false;

        selectedChannel = std::max(0, std::min(selectedChannel, availableChannelCount - 1));
        if (strcmp(canvas.channel, availableChannels[selectedChannel]) == 0)
        {
            pendingChannelSwitch[0] = '\0';
            pendingChannelSwitchDeadline = 0;
            return true;
        }

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
        snprintf(pendingChannelSwitch, sizeof(pendingChannelSwitch), "%s",
                 availableChannels[selectedChannel]);
        pendingChannelSwitchDeadline = osGetTime() + 15000;
        setAdminNotice("WAITING FOR CHANNEL");
        // Protocol 6 delivers the metadata and snapshot as distinct WebSocket
        // messages; the realtime event loop consumes both asynchronously.
        return true;
    };

    syncSelectedChannel();

    while (aptMainLoop() && !exitRequested)
    {
        if (adminNoticeFrames > 0 && adminNoticeExpiresAt > 0 &&
            osGetTime() >= adminNoticeExpiresAt)
            clearAdminNotice();
        if (clientSettingsDirty && clientSettingsSaveAfter > 0 &&
            osGetTime() >= clientSettingsSaveAfter)
            flushClientSettings();

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
        if (suppressTouchUntilRelease)
        {
            const bool touchStillHeld = (kHeld & KEY_TOUCH) != 0;
            kDown &= ~KEY_TOUCH;
            kHeld &= ~KEY_TOUCH;
            kUp &= ~KEY_TOUCH;
            if (!touchStillHeld)
                suppressTouchUntilRelease = false;
        }
        Doodle::SemanticInputFrame semanticInput(kDown, kHeld, kUp);
        touchPosition touch;
        circlePosition circle;
        hidTouchRead(&touch);
        hidCircleRead(&circle);
        u32 navDown = kDown | (hidKeysDownRepeat() &
                               (KEY_DUP | KEY_DDOWN | KEY_DLEFT | KEY_DRIGHT));
        u32 nextCircleDirection = 0;
        if (abs(circle.dy) > abs(circle.dx) && abs(circle.dy) > 90)
            nextCircleDirection = circle.dy > 0 ? KEY_DUP : KEY_DDOWN;
        else if (abs(circle.dx) > 90)
            nextCircleDirection = circle.dx < 0 ? KEY_DLEFT : KEY_DRIGHT;
        if (nextCircleDirection == 0)
        {
            circleNavDirection = 0;
            circleNavRepeatFrames = 0;
        }
        else if (nextCircleDirection != circleNavDirection)
        {
            circleNavDirection = nextCircleDirection;
            circleNavRepeatFrames = 18;
            navDown |= nextCircleDirection;
        }
        else if (circleNavRepeatFrames > 0)
        {
            --circleNavRepeatFrames;
        }
        else
        {
            navDown |= nextCircleDirection;
            circleNavRepeatFrames = 5;
        }

        if (rulesRequireFreshAPress && topMode == TOP_MODE_RULES &&
            rulesRenderedSinceKeyboard && !(kHeld & KEY_A))
        {
            rulesRequireFreshAPress = false;
        }

        if (gDisconnectReason[0])
        {
            cancelTransientOverlaysForConnection();
            const bool touchReconnect =
                (kDown & KEY_TOUCH) &&
                pointInRect(touch.px, touch.py, 30, 150, 126, 28);
            const bool touchMenu =
                (kDown & KEY_TOUCH) &&
                pointInRect(touch.px, touch.py, 164, 150, 126, 28);
            if ((kDown & KEY_A) || touchReconnect)
            {
                if (touchReconnect)
                    suppressTouchUntilRelease = true;
                setAdminNotice("RECONNECTING");
                reconnectSession("reconnect");
                topRenderFrame = 10;
                continue;
            }
            if ((kDown & KEY_B) || touchMenu)
            {
                if (touchMenu)
                    suppressTouchUntilRelease = true;
                gDisconnectReason[0] = '\0';
                topMode = TOP_MODE_MENU;
                routeStack.reset(TOP_MODE_MENU);
                topRenderFrame = 10;
                continue;
            }
            kDown = 0;
            kHeld = 0;
            kUp = 0;
            navDown = 0;
        }

        if (recoveryCodeExplanation)
        {
            const bool continueOnboarding =
                (kDown & KEY_A) ||
                ((kDown & KEY_TOUCH) &&
                 pointInRect(touch.px, touch.py, 24, 186, 272, 40));
            if (continueOnboarding)
            {
                if (kDown & KEY_TOUCH)
                    suppressTouchUntilRelease = true;
                recoveryCodeExplanation = false;
                backupCodeRevealed = false;
                topMode = TOP_MODE_CANVAS;
                routeStack.reset(TOP_MODE_CANVAS);
                topRenderFrame = 10;
                continue;
            }
            kDown = 0;
            navDown = 0;
        }

        const bool higherPriorityConfirmation =
            confirmClearCanvas || confirmPaletteReset || confirmExit ||
            adminRectAwaitingConfirm || moderationConfirmation ||
            rotateBackupConfirmation || optionsBindingConflict ||
            ticketDraftPreview || ticketReplyPreview;
        if (adminNoticeFrames > 0 && !higherPriorityConfirmation &&
            (kDown & KEY_TOUCH) &&
            pointInRect(touch.px, touch.py, 8, 194, 304, 22))
        {
            clearAdminNotice();
            suppressTouchUntilRelease = true;
            continue;
        }

        if (confirmExit)
        {
            const bool cancelExit =
                (kDown & KEY_B) ||
                ((kDown & KEY_TOUCH) &&
                 pointInRect(touch.px, touch.py, 164, 150, 126, 28));
            const bool applyExit =
                (kDown & KEY_A) ||
                ((kDown & KEY_TOUCH) &&
                 pointInRect(touch.px, touch.py, 30, 150, 126, 28));
            if (cancelExit)
            {
                if (kDown & KEY_TOUCH)
                    suppressTouchUntilRelease = true;
                confirmExit = false;
                continue;
            }
            if (applyExit)
            {
                if (kDown & KEY_TOUCH)
                    suppressTouchUntilRelease = true;
                confirmExit = false;
                UIState::clearPoints();
                exitRequested = true;
                continue;
            }
            kDown = 0;
            kHeld = 0;
            navDown = 0;
        }

        if (confirmPaletteReset)
        {
            const bool cancelReset =
                (kDown & KEY_B) ||
                ((kDown & KEY_TOUCH) &&
                 pointInRect(touch.px, touch.py, 164, 150, 126, 28));
            const bool applyReset =
                (kDown & KEY_A) ||
                ((kDown & KEY_TOUCH) &&
                 pointInRect(touch.px, touch.py, 30, 150, 126, 28));
            if (cancelReset)
            {
                if (kDown & KEY_TOUCH)
                    suppressTouchUntilRelease = true;
                confirmPaletteReset = false;
                clearAdminNotice();
                continue;
            }
            else if (applyReset)
            {
                if (kDown & KEY_TOUCH)
                    suppressTouchUntilRelease = true;
                confirmPaletteReset = false;
                resetPaletteToDefaults();
                setAdminNotice("PALETTE RESET");
                continue;
            }
            kDown = 0;
            kHeld = 0;
            navDown = 0;
        }

        if (kDown & KEY_SELECT)
        {
            if (optionsBindingCapture)
            {
                optionsBindingCapture = false;
                setAdminNotice("BINDING CAPTURE CANCELLED");
                continue;
            }
            ticketDraftPreview = false;
            ticketReplyPreview = false;
            moderationConfirmation = false;
            peopleActionMode = false;
            reportUserSelectionMode = false;
            rotateBackupConfirmation = false;
            confirmClearCanvas = false;
            confirmPaletteReset = false;
            confirmExit = false;
            pendingAdminRectTool = ADMIN_RECT_NONE;
            adminRectDragging = false;
            adminRectAwaitingConfirm = false;
            optionsBindingCapture = false;
            optionsBindingConflict = false;
            UIState::setColorPickerActive(false);
            clearAdminNotice();
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
            routeStack.reset(topMode);
            topRenderFrame = 10;
            continue;
        }

        if (!supportOnlyMode && topMode == TOP_MODE_CANVAS &&
            !gDisconnectReason[0] && !higherPriorityConfirmation &&
            !UIState::isColorPickerActive() &&
            semanticInput.consumeDown(gClientSettings.bindings, Doodle::INPUT_ACTION_REFRESH))
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
            const bool staffMenu = isModOrAdmin();
            const int menuCount = rootMenuItemCount(staffMenu);
            selectedMenuItem = std::max(0, std::min(selectedMenuItem, menuCount - 1));
            if (navDown & KEY_DUP)
            {
                selectedMenuItem = (selectedMenuItem + menuCount - 1) % menuCount;
                topRenderFrame = 10;
            }
            if (navDown & KEY_DDOWN)
            {
                selectedMenuItem = (selectedMenuItem + 1) % menuCount;
                topRenderFrame = 10;
            }
            if (kDown & KEY_B)
            {
                topMode = TOP_MODE_CANVAS;
                routeStack.reset(TOP_MODE_CANVAS);
                topRenderFrame = 10;
                continue;
            }
            bool activateMenuItem = (kDown & KEY_A) != 0;
            if (kDown & KEY_TOUCH)
            {
                for (int item = 0; item < menuCount; ++item)
                {
                    if (pointInRect(touch.px, touch.py, 4, 4 + item * 28, 312, 28))
                    {
                        selectedMenuItem = item;
                        activateMenuItem = true;
                        break;
                    }
                }
            }
            if (activateMenuItem)
            {
                if (selectedMenuItem == 0)
                {
                    syncSelectedChannel();
                    topMode = TOP_MODE_CHANNELS;
                }
                else if (selectedMenuItem == 1)
                {
                    topMode = TOP_MODE_USERS;
                    selectedPerson = 0;
                    peopleScroll = 0;
                    peopleActionMode = false;
                    reportUserSelectionMode = false;
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
                else if (staffMenu && selectedMenuItem == 3)
                {
                    selectedAdminItem = 0;
                    topMode = TOP_MODE_STAFF_CENTER;
                }
                else if (selectedMenuItem == (staffMenu ? 4 : 3))
                {
                    backupCodeRevealed = false;
                    topMode = TOP_MODE_IDENTITY;
                }
                else if (selectedMenuItem == (staffMenu ? 5 : 4))
                {
                    optionsPage = 0;
                    optionsSelected = 0;
                    optionsBindingCapture = false;
                    topMode = TOP_MODE_OPTIONS;
                }
                else if (selectedMenuItem == (staffMenu ? 6 : 5))
                {
                    topMode = TOP_MODE_RULES;
                }
                else if (selectedMenuItem == menuCount - 1)
                {
                    requestExitConfirmation();
                    continue;
                }
                routeStack.push(topMode);
                topRenderFrame = 10;
                continue;
            }
        }
        else if (topMode == TOP_MODE_STAFF_CENTER)
        {
            if (kDown & KEY_B)
            {
                if (routeStack.pop())
                    topMode = routeStack.current();
                else
                    topMode = TOP_MODE_MENU;
                topRenderFrame = 10;
                continue;
            }
            if (navDown & KEY_DUP)
            {
                selectedAdminItem = (selectedAdminItem + 2) % 3;
                topRenderFrame = 10;
            }
            if (navDown & KEY_DDOWN)
            {
                selectedAdminItem = (selectedAdminItem + 1) % 3;
                topRenderFrame = 10;
            }
            bool activateStaffItem = (kDown & KEY_A) != 0;
            if (kDown & KEY_TOUCH)
            {
                for (int item = 0; item < 3; ++item)
                {
                    if (pointInRect(touch.px, touch.py, 8, 44 + item * 48, 304, 40))
                    {
                        selectedAdminItem = item;
                        activateStaffItem = true;
                        break;
                    }
                }
            }
            if (activateStaffItem)
            {
                if (!isModOrAdmin())
                {
                    setAdminNotice("MOD OR ADMIN REQUIRED");
                    continue;
                }
                if (selectedAdminItem == 0)
                {
                    ticketStaffScope = true;
                    TicketCursor firstPage;
                    memset(&firstPage, 0, sizeof(firstPage));
                    requestTicketList(true, "unresolved", "", firstPage, NULL, 0,
                                      "LOADING STAFF QUEUE");
                    topMode = TOP_MODE_TICKETS;
                }
                else if (selectedAdminItem == 1)
                {
                    char command[96];
                    Protocol::buildStaffChatList(command, sizeof(command));
                    if (sendTicketCommand(command))
                        setTicketNotice("LOADING STAFF CHAT");
                    topMode = TOP_MODE_TICKETS;
                }
                else if (selectedAdminItem == 2)
                {
                    if (kDown & KEY_TOUCH)
                        suppressTouchUntilRelease = true;
                    topMode = TOP_MODE_CANVAS;
                    pickerTab = 1;
                    pickerReturnToRoute = true;
                    UIState::setColorPickerActive(true);
                }
                routeStack.push(topMode);
                topRenderFrame = 10;
                continue;
            }
        }
        else if (topMode == TOP_MODE_OPTIONS)
        {
            if (optionsBindingConflict)
            {
                const bool cancelConflict = (kDown & KEY_B) ||
                                            ((kDown & KEY_TOUCH) &&
                                             pointInRect(touch.px, touch.py, 164, 150, 126, 28));
                const bool swapConflict = (kDown & KEY_A) ||
                                          ((kDown & KEY_TOUCH) &&
                                           pointInRect(touch.px, touch.py, 30, 150, 126, 28));
                if (cancelConflict)
                {
                    if (kDown & KEY_TOUCH)
                        suppressTouchUntilRelease = true;
                    optionsBindingConflict = false;
                    setAdminNotice("BINDING CANCELLED");
                }
                else if (swapConflict)
                {
                    if (kDown & KEY_TOUCH)
                        suppressTouchUntilRelease = true;
                    Doodle::BindingConflict conflict;
                    Doodle::BindingEditResult result = Doodle::editClientBinding(
                        gClientSettings, pendingBindingAction, optionsBindingSlot,
                        pendingBindingButton, Doodle::BINDING_CONFLICT_SWAP, &conflict);
                    optionsBindingConflict = false;
                    if (result == Doodle::BINDING_EDIT_OK || result == Doodle::BINDING_EDIT_SWAPPED)
                    {
                        markClientSettingsDirty();
                        setAdminNotice(result == Doodle::BINDING_EDIT_SWAPPED ? "BUTTONS SWAPPED" : "BINDING SAVED");
                    }
                    else
                        setAdminNotice("BINDING FAILED");
                }
                continue;
            }
            if (optionsBindingCapture)
            {
                Doodle::ButtonToken captured = Doodle::BUTTON_NONE;
                if (firstBindableButton(kDown, captured))
                {
                    Doodle::BindingConflict conflict;
                    Doodle::BindingEditResult result = Doodle::editClientBinding(
                        gClientSettings, pendingBindingAction, optionsBindingSlot,
                        captured, Doodle::BINDING_CONFLICT_CANCEL, &conflict);
                    optionsBindingCapture = false;
                    if (result == Doodle::BINDING_EDIT_CONFLICT)
                    {
                        pendingBindingButton = captured;
                        pendingBindingConflict = conflict;
                        optionsBindingConflict = true;
                        setAdminNotice("A SWAP  B CANCEL");
                    }
                    else if (result == Doodle::BINDING_EDIT_OK || result == Doodle::BINDING_EDIT_SWAPPED)
                    {
                        markClientSettingsDirty();
                        setAdminNotice("BINDING SAVED");
                    }
                    else if (result == Doodle::BINDING_EDIT_UNCHANGED)
                        setAdminNotice("BINDING UNCHANGED");
                    else
                        setAdminNotice("BUTTON NOT AVAILABLE");
                }
                continue;
            }

            if (optionsPage == 0)
            {
                const int sectionCount = 3;
                if (navDown & KEY_DUP)
                    optionsSelected = (optionsSelected + sectionCount - 1) % sectionCount;
                if (navDown & KEY_DDOWN)
                    optionsSelected = (optionsSelected + 1) % sectionCount;
                if (kDown & KEY_B)
                {
                    if (routeStack.pop())
                        topMode = routeStack.current();
                    else
                        topMode = TOP_MODE_MENU;
                    topRenderFrame = 10;
                    continue;
                }
                bool activate = (kDown & KEY_A) != 0;
                if (kDown & KEY_TOUCH)
                {
                    for (int item = 0; item < sectionCount; ++item)
                    {
                        if (pointInRect(touch.px, touch.py, 8, 44 + item * 48, 304, 40))
                        {
                            optionsSelected = item;
                            activate = true;
                            break;
                        }
                    }
                }
                if (activate)
                {
                    optionsPage = optionsSelected + 1;
                    optionsSelected = 0;
                    topRenderFrame = 10;
                    continue;
                }
            }
            else if (optionsPage == 1)
            {
                const int itemCount = 1 + Doodle::INPUT_ACTION_COUNT;
                optionsSelected = std::max(0, std::min(optionsSelected, itemCount - 1));
                if (navDown & KEY_DUP)
                    optionsSelected = (optionsSelected + itemCount - 1) % itemCount;
                if (navDown & KEY_DDOWN)
                    optionsSelected = (optionsSelected + 1) % itemCount;
                if (kDown & KEY_X)
                    optionsBindingSlot = 1 - optionsBindingSlot;
                if ((kDown & KEY_Y) && optionsSelected > 0)
                {
                    Doodle::BindingConflict ignoredConflict;
                    Doodle::BindingEditResult result = Doodle::editClientBinding(
                        gClientSettings,
                        (Doodle::InputAction)(optionsSelected - 1),
                        optionsBindingSlot, Doodle::BUTTON_NONE,
                        Doodle::BINDING_CONFLICT_CANCEL, &ignoredConflict);
                    if (result == Doodle::BINDING_EDIT_OK)
                    {
                        markClientSettingsDirty();
                        setAdminNotice("BINDING SLOT CLEARED");
                    }
                    else
                        setAdminNotice("BINDING SLOT ALREADY EMPTY");
                }
                if (kDown & KEY_B)
                {
                    optionsPage = 0;
                    optionsSelected = 0;
                    continue;
                }
                if ((navDown & (KEY_DLEFT | KEY_DRIGHT)) && optionsSelected == 0)
                {
                    int direction = (navDown & KEY_DRIGHT) ? 1 : -1;
                    int preset = (int)gClientSettings.controlPreset;
                    if (preset >= (int)Doodle::CONTROL_PRESET_CUSTOM)
                        preset = direction > 0 ? -1 : 0;
                    preset = (preset + direction + 3) % 3;
                    Doodle::applyClientControlPreset(gClientSettings, (Doodle::ControlPreset)preset);
                    markClientSettingsDirty();
                }
                bool activate = (kDown & KEY_A) != 0;
                if (kDown & KEY_TOUCH)
                {
                    for (int item = 0; item < itemCount; ++item)
                    {
                        if (pointInRect(touch.px, touch.py, 8, 8 + item * 30, 304, 28))
                        {
                            optionsSelected = item;
                            if (item > 0)
                                optionsBindingSlot = touch.px >= 246 ? 1 : 0;
                            activate = true;
                            break;
                        }
                    }
                }
                if (activate)
                {
                    if (optionsSelected == 0)
                    {
                        int preset = (int)gClientSettings.controlPreset;
                        if (preset >= (int)Doodle::CONTROL_PRESET_CUSTOM)
                            preset = -1;
                        preset = (preset + 1) % 3;
                        Doodle::applyClientControlPreset(gClientSettings, (Doodle::ControlPreset)preset);
                        markClientSettingsDirty();
                    }
                    else
                    {
                        pendingBindingAction = (Doodle::InputAction)(optionsSelected - 1);
                        optionsBindingCapture = true;
                        setAdminNotice("PRESS A BUTTON");
                    }
                    continue;
                }
            }
            else if (optionsPage == 2)
            {
                const int itemCount = 3;
                if (navDown & KEY_DUP)
                    optionsSelected = (optionsSelected + itemCount - 1) % itemCount;
                if (navDown & KEY_DDOWN)
                    optionsSelected = (optionsSelected + 1) % itemCount;
                if (kDown & KEY_B)
                {
                    optionsPage = 0;
                    optionsSelected = 1;
                    continue;
                }
                bool activate = (kDown & KEY_A) != 0;
                if (kDown & KEY_TOUCH)
                {
                    for (int item = 0; item < itemCount; ++item)
                    {
                        if (pointInRect(touch.px, touch.py, 8, 44 + item * 48, 304, 40))
                        {
                            optionsSelected = item;
                            activate = true;
                            break;
                        }
                    }
                }
                if (activate)
                {
                    if (optionsSelected == 0)
                    {
                        gClientSettings.zoomOverlaySide = (Doodle::ZoomOverlaySide)(
                            ((int)gClientSettings.zoomOverlaySide + 1) %
                            (int)Doodle::ZOOM_OVERLAY_SIDE_COUNT);
                        markClientSettingsDirty();
                        setAdminNotice(Doodle::zoomOverlaySideLabel(gClientSettings.zoomOverlaySide));
                    }
                    else if (optionsSelected == 1)
                    {
                        if (kDown & KEY_TOUCH)
                            suppressTouchUntilRelease = true;
                        topMode = TOP_MODE_CANVAS;
                        pickerTab = 0;
                        pickerReturnToRoute = true;
                        UIState::setColorPickerActive(true);
                        routeStack.push(topMode);
                    }
                    else
                    {
                        confirmPaletteReset = true;
                        clearAdminNotice();
                    }
                    continue;
                }
            }
            else
            {
                if (kDown & KEY_B)
                {
                    optionsPage = 0;
                    optionsSelected = 2;
                    continue;
                }
                if ((kDown & KEY_A) ||
                    ((kDown & KEY_TOUCH) &&
                     pointInRect(touch.px, touch.py, 20, 136, 280, 34)))
                {
                    setAdminNotice("RECONNECTING");
                    reconnectSession("options-reconnect");
                    continue;
                }
            }
        }
        else if (topMode == TOP_MODE_TICKETS)
        {
            const bool staffAllowed = isModOrAdmin() && !supportOnlyMode;
            if (ticketDraftPreview)
            {
                const bool sendPreview = (kDown & KEY_A) ||
                                         ((kDown & KEY_TOUCH) &&
                                          pointInRect(touch.px, touch.py, 8, 200, 148, 34));
                if (sendPreview)
                {
                    char command[1024];
                    Protocol::buildTicketCreate(command, sizeof(command),
                                                ticketDraftCategory,
                                                ticketDraftSubject,
                                                ticketDraftDetails);
                    if (sendTicketCommand(command))
                    {
                        ticketDraftPreview = false;
                        ticketDraftSendPending = true;
                        setTicketNotice("SENDING REQUEST");
                    }
                }
                else if ((kDown & KEY_B) ||
                         ((kDown & KEY_TOUCH) &&
                          pointInRect(touch.px, touch.py, 164, 200, 148, 34)))
                {
                    readKeyboardText("Ticket subject", ticketDraftSubject,
                                     sizeof(ticketDraftSubject), ticketDraftSubject);
                    readKeyboardText("Ticket details", ticketDraftDetails,
                                     sizeof(ticketDraftDetails), ticketDraftDetails);
                }
                continue;
            }
            if (ticketReplyPreview)
            {
                const bool sendPreview = (kDown & KEY_A) ||
                                         ((kDown & KEY_TOUCH) &&
                                          pointInRect(touch.px, touch.py, 8, 200, 148, 34));
                if (sendPreview)
                {
                    char command[640];
                    Protocol::buildTicketReply(command, sizeof(command),
                                               ticketReplyDraftTicketId,
                                               ticketReplyDraft,
                                               ticketReplyDraftStaff);
                    if (sendTicketCommand(command))
                    {
                        ticketReplyPreview = false;
                        ticketReplySendPending = true;
                        setTicketNotice("SENDING REPLY");
                    }
                }
                else if ((kDown & KEY_B) ||
                         ((kDown & KEY_TOUCH) &&
                          pointInRect(touch.px, touch.py, 164, 200, 148, 34)))
                {
                    readKeyboardText("Ticket reply", ticketReplyDraft,
                                     sizeof(ticketReplyDraft), ticketReplyDraft);
                }
                continue;
            }
            if (ticketView == 0)
            {
                int homeCount = 4;
                ticketHomeSelected = std::max(0, std::min(ticketHomeSelected, homeCount - 1));
                if (navDown & KEY_DUP)
                {
                    ticketHomeSelected = (ticketHomeSelected + homeCount - 1) % homeCount;
                    topRenderFrame = 10;
                }
                if (navDown & KEY_DDOWN)
                {
                    ticketHomeSelected = (ticketHomeSelected + 1) % homeCount;
                    topRenderFrame = 10;
                }
                if ((kDown & KEY_B) && !supportOnlyMode)
                {
                    if (routeStack.pop())
                        topMode = routeStack.current();
                    else
                        topMode = TOP_MODE_MENU;
                    topRenderFrame = 10;
                    continue;
                }
                bool activateTicketHome = (kDown & KEY_A) != 0;
                if (kDown & KEY_TOUCH)
                {
                    for (int item = 0; item < homeCount; ++item)
                    {
                        if (pointInRect(touch.px, touch.py, 8, 8 + item * 34, 304, 32))
                        {
                            ticketHomeSelected = item;
                            activateTicketHome = true;
                            break;
                        }
                    }
                }
                if (activateTicketHome)
                {
                    const bool startsComposer =
                        (!supportOnlyMode && ticketHomeSelected <= 2) ||
                        (supportOnlyMode && ticketHomeSelected == 0);
                    if (startsComposer &&
                        (ticketDraftSendPending || ticketReplySendPending))
                    {
                        setTicketNotice("WAIT FOR CURRENT SEND");
                        continue;
                    }
                    if (supportOnlyMode && ticketHomeSelected == 2)
                    {
                        profileSelected = 0;
                        backupCodeRevealed = false;
                        topMode = TOP_MODE_IDENTITY;
                        routeStack.push(topMode);
                        topRenderFrame = 10;
                        continue;
                    }
                    if (supportOnlyMode && ticketHomeSelected == 3)
                    {
                        requestExitConfirmation();
                        continue;
                    }
                    bool listMine = (supportOnlyMode && ticketHomeSelected == 1) ||
                                    (!supportOnlyMode && ticketHomeSelected == 3);
                    bool listStaff = false;
                    if (listMine || listStaff)
                    {
                        ticketStaffScope = listStaff;
                        TicketCursor firstPage;
                        memset(&firstPage, 0, sizeof(firstPage));
                        requestTicketList(listStaff, listStaff ? "unresolved" : "",
                                          supportOnlyMode ? "unban" : "",
                                          firstPage, NULL, 0, "LOADING TICKETS");
                    }
                    else if (!supportOnlyMode && ticketHomeSelected == 2)
                    {
                        reportUserSelectionMode = true;
                        peopleActionMode = false;
                        peopleAllChannels = false;
                        selectedPerson = 0;
                        peopleScroll = 0;
                        topMode = TOP_MODE_USERS;
                        routeStack.push(topMode);
                        setTicketNotice("SELECT A USER TO REPORT");
                    }
                    else
                    {
                        const char *category = supportOnlyMode ? "unban" :
                                               (ticketHomeSelected == 0 ? "bug" : "feature");
                        snprintf(ticketDraftCategory, sizeof(ticketDraftCategory), "%s", category);
                        if (!readKeyboardText("Ticket subject", ticketDraftSubject,
                                              sizeof(ticketDraftSubject), ticketDraftSubject))
                            continue;
                        if (!readKeyboardText("Ticket details", ticketDraftDetails,
                                              sizeof(ticketDraftDetails), ticketDraftDetails))
                            continue;
                        ticketDraftPreview = true;
                        setTicketNotice("REVIEW BEFORE SENDING");
                    }
                    topRenderFrame = 10;
                    continue;
                }
            }
            else if (ticketView == 1)
            {
                if (ticketListCount > 0 && (navDown & KEY_DUP))
                {
                    ticketSelected = (ticketSelected + ticketListCount - 1) % ticketListCount;
                    topRenderFrame = 10;
                }
                if (ticketListCount > 0 && (navDown & KEY_DDOWN))
                {
                    ticketSelected = (ticketSelected + 1) % ticketListCount;
                    topRenderFrame = 10;
                }
                if (kDown & KEY_B)
                {
                    if (ticketStaffScope && !supportOnlyMode)
                    {
                        if (routeStack.pop())
                            topMode = routeStack.current();
                        else
                            topMode = TOP_MODE_STAFF_CENTER;
                        selectedAdminItem = 0;
                        topRenderFrame = 10;
                        continue;
                    }
                    ticketView = 0;
                    ticketHomeSelected = supportOnlyMode ? 1 : 3;
                    topRenderFrame = 10;
                    continue;
                }
                bool openTicket = (kDown & KEY_A) != 0;
                bool touchRefresh = false;
                bool touchNext = false;
                bool touchPrevious = false;
                if (kDown & KEY_TOUCH)
                {
                    for (int row = 0; row < ticketListCount && row < 6; ++row)
                    {
                        if (pointInRect(touch.px, touch.py, 8, 4 + row * 27, 304, 25))
                        {
                            ticketSelected = row;
                            openTicket = true;
                            break;
                        }
                    }
                    touchPrevious = pointInRect(touch.px, touch.py, 8, 172, 94, 36);
                    touchRefresh = pointInRect(touch.px, touch.py, 110, 172, 94, 36);
                    touchNext = pointInRect(touch.px, touch.py, 212, 172, 100, 36);
                }
                if (openTicket && ticketListCount > 0)
                {
                    char command[128];
                    Protocol::buildTicketGet(command, sizeof(command), ticketList[ticketSelected].id);
                    if (sendTicketCommand(command))
                        setTicketNotice("LOADING THREAD");
                    continue;
                }
                const bool refreshPage = (kDown & KEY_X) != 0 || touchRefresh;
                const bool nextPage = ((kDown & KEY_Y) != 0 || touchNext) &&
                                      ticketNextCursor.id > 0;
                const bool previousPage = ((kDown & KEY_L) != 0 || touchPrevious) &&
                                          ticketCursorHistoryCount > 0;
                if (refreshPage || nextPage || previousPage)
                {
                    TicketCursor requestedCursor = ticketCurrentCursor;
                    TicketCursor requestedHistory[8];
                    memcpy(requestedHistory, ticketCursorHistory, sizeof(requestedHistory));
                    int requestedHistoryCount = ticketCursorHistoryCount;
                    if (refreshPage)
                    {
                        memset(&requestedCursor, 0, sizeof(requestedCursor));
                        requestedHistoryCount = 0;
                    }
                    else if (nextPage)
                    {
                        if (requestedHistoryCount < (int)(sizeof(requestedHistory) / sizeof(requestedHistory[0])))
                            requestedHistory[requestedHistoryCount++] = ticketCurrentCursor;
                        else
                        {
                            memmove(&requestedHistory[0], &requestedHistory[1],
                                    sizeof(TicketCursor) * (requestedHistoryCount - 1));
                            requestedHistory[requestedHistoryCount - 1] = ticketCurrentCursor;
                        }
                        requestedCursor = ticketNextCursor;
                    }
                    else
                    {
                        requestedCursor = requestedHistory[--requestedHistoryCount];
                    }
                    requestTicketList(ticketStaffScope,
                                      ticketStaffScope ? "unresolved" : "",
                                      supportOnlyMode ? "unban" : "",
                                      requestedCursor, requestedHistory,
                                      requestedHistoryCount,
                                      nextPage ? "LOADING NEXT PAGE" :
                                      (previousPage ? "LOADING PREVIOUS PAGE" :
                                                      "REFRESHING"));
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
                const bool touchReply = (kDown & KEY_TOUCH) &&
                                        pointInRect(touch.px, touch.py, 8, 164, 148, 40);
                const bool touchStaffActions = (kDown & KEY_TOUCH) &&
                                               pointInRect(touch.px, touch.py, 164, 164, 148, 40);
                if (((kDown & KEY_A) || touchReply) && (!closed || ticketStaffScope))
                {
                    if (ticketDraftSendPending || ticketReplySendPending)
                    {
                        setTicketNotice("WAIT FOR CURRENT SEND");
                        continue;
                    }
                    if (readKeyboardText("Ticket reply", ticketReplyDraft,
                                         sizeof(ticketReplyDraft), ticketReplyDraft))
                    {
                        ticketReplyDraftTicketId = activeTicket.id;
                        ticketReplyDraftStaff = ticketStaffScope;
                        ticketReplyPreview = true;
                        setTicketNotice("REVIEW BEFORE SENDING");
                    }
                    continue;
                }
                if (((kDown & KEY_X) || touchStaffActions) && ticketStaffScope && staffAllowed)
                {
                    ticketView = 3;
                    ticketActionSelected = 0;
                    topRenderFrame = 10;
                    continue;
                }
            }
            else if (ticketView == 3)
            {
                if (navDown & KEY_DUP)
                {
                    ticketActionSelected = (ticketActionSelected + 5) % 6;
                    topRenderFrame = 10;
                }
                if (navDown & KEY_DDOWN)
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
                bool activateTicketAction = (kDown & KEY_A) != 0;
                if (kDown & KEY_TOUCH)
                {
                    for (int action = 0; action < 6; ++action)
                    {
                        if (pointInRect(touch.px, touch.py, 8, 4 + action * 31, 304, 29))
                        {
                            ticketActionSelected = action;
                            activateTicketAction = true;
                            break;
                        }
                    }
                }
                if (activateTicketAction && staffAllowed)
                {
                    char command[768];
                    command[0] = '\0';
                    if (ticketActionSelected == 0)
                        Protocol::buildTicketStatus(command, sizeof(command), activeTicket.id, "in_progress");
                    else if (ticketActionSelected == 1)
                    {
                        if (ticketDraftSendPending || ticketReplySendPending)
                        {
                            setTicketNotice("WAIT FOR CURRENT SEND");
                            continue;
                        }
                        if (readKeyboardText("Reply to requester", ticketReplyDraft,
                                             sizeof(ticketReplyDraft), ticketReplyDraft))
                        {
                            ticketReplyDraftTicketId = activeTicket.id;
                            ticketReplyDraftStaff = true;
                            ticketReplyPreview = true;
                            setTicketNotice("REVIEW BEFORE SENDING");
                        }
                        continue;
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
                    if (routeStack.pop())
                        topMode = routeStack.current();
                    else
                        topMode = TOP_MODE_STAFF_CENTER;
                    selectedAdminItem = 1;
                    topRenderFrame = 10;
                    continue;
                }
                const bool touchSend = (kDown & KEY_TOUCH) &&
                                       pointInRect(touch.px, touch.py, 8, 44, 304, 40);
                const bool touchRefresh = (kDown & KEY_TOUCH) &&
                                          pointInRect(touch.px, touch.py, 8, 92, 304, 40);
                const bool touchOlder = (kDown & KEY_TOUCH) &&
                                        pointInRect(touch.px, touch.py, 8, 140, 304, 40);
                if ((kDown & KEY_A) || touchSend)
                {
                    char message[241];
                    if (!readKeyboardText("Staff message", message, sizeof(message), "")) continue;
                    char command[640];
                    Protocol::buildStaffChatSend(command, sizeof(command), message);
                    if (sendTicketCommand(command)) setTicketNotice("STAFF MESSAGE SENT");
                    continue;
                }
                if ((kDown & KEY_X) || touchRefresh ||
                    (((kDown & KEY_Y) || touchOlder) && staffChatNextBeforeId > 0))
                {
                    char command[96];
                    const bool older = ((kDown & KEY_Y) || touchOlder) &&
                                       staffChatNextBeforeId > 0;
                    Protocol::buildStaffChatList(command, sizeof(command),
                                                 older ? staffChatNextBeforeId : 0);
                    if (sendTicketCommand(command))
                        setTicketNotice(older ? "LOADING OLDER CHAT" :
                                                  "REFRESHING STAFF CHAT");
                    continue;
                }
            }
        }
        else if (topMode == TOP_MODE_USERS)
        {
            int peopleIndices[24];
            int filteredPeopleCount = buildPeopleIndex(
                connectedUsers, connectedUserCount, peopleAllChannels,
                canvas.channel, identityInfo.displayName, identityInfo.username,
                peopleIndices, 24);
            selectedPerson = filteredPeopleCount > 0
                                 ? std::max(0, std::min(selectedPerson, filteredPeopleCount - 1))
                                 : 0;
            if (selectedPerson < peopleScroll)
                peopleScroll = selectedPerson;
            if (selectedPerson >= peopleScroll + 5)
                peopleScroll = selectedPerson - 4;
            peopleScroll = std::max(0, std::min(peopleScroll,
                                                std::max(0, filteredPeopleCount - 5)));

            if (moderationConfirmation)
            {
                const bool confirm = (kDown & KEY_A) ||
                                     ((kDown & KEY_TOUCH) &&
                                      pointInRect(touch.px, touch.py, 30, 150, 126, 28));
                const bool cancel = (kDown & KEY_B) ||
                                    ((kDown & KEY_TOUCH) &&
                                     pointInRect(touch.px, touch.py, 164, 150, 126, 28));
                if (cancel)
                {
                    if (kDown & KEY_TOUCH)
                        suppressTouchUntilRelease = true;
                    moderationConfirmation = false;
                    setAdminNotice("MODERATION CANCELLED");
                }
                else if (confirm)
                {
                    if (kDown & KEY_TOUCH)
                        suppressTouchUntilRelease = true;
                    char command[256];
                    Protocol::buildModerationCommand(
                        command, sizeof(command), pendingModerationAction,
                        pendingModerationIdentity, 0, pendingModerationReason);
                    if (NetworkManager::checkConnection() &&
                        NetworkManager::sendText(command))
                        setAdminNotice("MODERATION PENDING");
                    else
                        setAdminNotice("MODERATION SEND FAILED");
                    moderationConfirmation = false;
                    peopleActionMode = false;
                }
                continue;
            }

            if (peopleActionMode)
            {
                if (navDown & KEY_DUP)
                    peopleActionSelected = (peopleActionSelected + 3) % 4;
                if (navDown & KEY_DDOWN)
                    peopleActionSelected = (peopleActionSelected + 1) % 4;
                if (kDown & KEY_B)
                {
                    peopleActionMode = false;
                    continue;
                }
                bool activateAction = (kDown & KEY_A) != 0;
                if (kDown & KEY_TOUCH)
                {
                    for (int action = 0; action < 4; ++action)
                    {
                        if (pointInRect(touch.px, touch.py, 8, 34 + action * 42, 304, 36))
                        {
                            peopleActionSelected = action;
                            activateAction = true;
                            break;
                        }
                    }
                }
                if (activateAction && filteredPeopleCount > 0)
                {
                    const PresenceUser &target = connectedUsers[peopleIndices[selectedPerson]];
                    if (!target.identityId[0])
                    {
                        setAdminNotice("ACCOUNT ACTIONS REQUIRE SIGN-IN");
                        continue;
                    }
                    if (isCurrentPerson(target, identityInfo.displayName, identityInfo.username))
                    {
                        setAdminNotice("CANNOT MODERATE YOURSELF");
                        continue;
                    }
                    if (peopleActionSelected == 3 && strcmp(identityInfo.role, "admin") != 0)
                    {
                        setAdminNotice("ADMIN REQUIRED FOR BAN");
                        continue;
                    }
                    static const char *ACTIONS[] = {"kick", "mute", "unmute", "ban"};
                    snprintf(pendingModerationAction, sizeof(pendingModerationAction),
                             "%s", ACTIONS[peopleActionSelected]);
                    snprintf(pendingModerationIdentity, sizeof(pendingModerationIdentity),
                             "%s", target.identityId);
                    snprintf(pendingModerationTargetName, sizeof(pendingModerationTargetName),
                             "%s", peopleName(target));
                    pendingModerationReason[0] = '\0';
                    if (peopleActionSelected != 2 &&
                        !readKeyboardText("Moderation reason", pendingModerationReason,
                                          sizeof(pendingModerationReason), ""))
                        continue;
                    moderationConfirmation = true;
                    setAdminNotice("CONFIRM MODERATION");
                    continue;
                }
            }
            else
            {
                bool activatePerson = (kDown & KEY_A) != 0;
                if (navDown & KEY_DUP && filteredPeopleCount > 0)
                    selectedPerson = (selectedPerson + filteredPeopleCount - 1) % filteredPeopleCount;
                if (navDown & KEY_DDOWN && filteredPeopleCount > 0)
                    selectedPerson = (selectedPerson + 1) % filteredPeopleCount;
                if (kDown & KEY_X)
                {
                    peopleAllChannels = !peopleAllChannels;
                    selectedPerson = 0;
                    peopleScroll = 0;
                }
                if (kDown & KEY_TOUCH)
                {
                    if (pointInRect(touch.px, touch.py, 8, 4, 148, 30))
                    {
                        peopleAllChannels = false;
                        selectedPerson = peopleScroll = 0;
                    }
                    else if (pointInRect(touch.px, touch.py, 164, 4, 148, 30))
                    {
                        peopleAllChannels = true;
                        selectedPerson = peopleScroll = 0;
                    }
                    else
                    {
                        for (int row = 0; row < 5; ++row)
                        {
                            if (pointInRect(touch.px, touch.py, 8, 40 + row * 32, 304, 30) &&
                                peopleScroll + row < filteredPeopleCount)
                            {
                                selectedPerson = peopleScroll + row;
                                activatePerson = true;
                                break;
                            }
                        }
                    }
                }
                if (activatePerson && reportUserSelectionMode && filteredPeopleCount > 0)
                {
                    if (ticketDraftSendPending || ticketReplySendPending)
                    {
                        setTicketNotice("WAIT FOR CURRENT SEND");
                        continue;
                    }
                    const PresenceUser &target = connectedUsers[peopleIndices[selectedPerson]];
                    const char *name = peopleName(target);
                    snprintf(ticketDraftCategory, sizeof(ticketDraftCategory), "report");
                    snprintf(ticketDraftSubject, sizeof(ticketDraftSubject),
                             "Report: %.48s", name);
                    snprintf(ticketDraftDetails, sizeof(ticketDraftDetails),
                             "Reported user: %.48s\nAccount: %.24s\nIdentity/session: %.39s\nChannel: %.24s\nReason: ",
                             name,
                             target.username[0] ? target.username : "anonymous",
                             target.identityId[0] ? target.identityId : target.id,
                             target.channel[0] ? target.channel : canvas.channel);
                    if (readKeyboardText("Report details", ticketDraftDetails,
                                         sizeof(ticketDraftDetails), ticketDraftDetails))
                    {
                        reportUserSelectionMode = false;
                        ticketDraftPreview = true;
                        ticketView = 0;
                        if (routeStack.pop())
                            topMode = routeStack.current();
                        else
                            topMode = TOP_MODE_TICKETS;
                        setTicketNotice("REVIEW USER REPORT");
                    }
                    continue;
                }
                if (activatePerson && isModOrAdmin() && filteredPeopleCount > 0)
                {
                    peopleActionMode = true;
                    peopleActionSelected = 0;
                }
                if (kDown & KEY_B)
                {
                    reportUserSelectionMode = false;
                    if (routeStack.pop())
                        topMode = routeStack.current();
                    else
                        topMode = TOP_MODE_MENU;
                    topRenderFrame = 10;
                    continue;
                }
            }
            topRenderFrame = 10;
        }
        else if (topMode == TOP_MODE_IDENTITY && !gNeedsDisplayName)
        {
            if (rotateBackupConfirmation)
            {
                const bool confirm = (kDown & KEY_A) ||
                                     ((kDown & KEY_TOUCH) &&
                                      pointInRect(touch.px, touch.py, 30, 150, 126, 28));
                const bool cancel = (kDown & KEY_B) ||
                                    ((kDown & KEY_TOUCH) &&
                                     pointInRect(touch.px, touch.py, 164, 150, 126, 28));
                if (cancel)
                {
                    if (kDown & KEY_TOUCH)
                        suppressTouchUntilRelease = true;
                    rotateBackupConfirmation = false;
                    setIdentityNotice("ROTATION CANCELLED");
                }
                else if (confirm)
                {
                    if (kDown & KEY_TOUCH)
                        suppressTouchUntilRelease = true;
                    if (requestBackupCode())
                        setIdentityNotice("ROTATION PENDING");
                    else
                        setIdentityNotice("ROTATION SEND FAILED");
                    rotateBackupConfirmation = false;
                }
                continue;
            }

            if (navDown & KEY_DUP)
                profileSelected = (profileSelected + 3) % 4;
            if (navDown & KEY_DDOWN)
                profileSelected = (profileSelected + 1) % 4;
            if (kDown & KEY_B)
            {
                backupCodeRevealed = false;
                if (routeStack.pop())
                    topMode = routeStack.current();
                else
                    topMode = TOP_MODE_MENU;
                topRenderFrame = 10;
                continue;
            }
            bool activateProfile = (kDown & KEY_A) != 0;
            if (kDown & KEY_X)
                profileSelected = 3, activateProfile = true;
            if (kDown & KEY_Y)
                profileSelected = 1, activateProfile = true;
            if (kDown & KEY_TOUCH)
            {
                if (pointInRect(touch.px, touch.py, 8, 46, 304, 38))
                    profileSelected = 0, activateProfile = true;
                else if (pointInRect(touch.px, touch.py, 8, 92, 148, 38))
                    profileSelected = 1, activateProfile = true;
                else if (pointInRect(touch.px, touch.py, 164, 92, 148, 38))
                    profileSelected = 2, activateProfile = true;
                else if (pointInRect(touch.px, touch.py, 8, 138, 304, 38))
                    profileSelected = 3, activateProfile = true;
            }
            if (activateProfile)
            {
                if (supportOnlyMode && profileSelected != 1)
                {
                    setIdentityNotice("PROFILE IS READ-ONLY WHILE RESTRICTED");
                    continue;
                }
                if (profileSelected == 0)
                {
                    char chosenDisplayName[25] = "";
                    if (promptDisplayName(identityInfo, chosenDisplayName,
                                          sizeof(chosenDisplayName)) &&
                        sendDisplayNameValue(chosenDisplayName))
                        setIdentityNotice("NAME CHANGE SENT");
                }
                else if (profileSelected == 1)
                {
                    backupCodeRevealed = !backupCodeRevealed;
                    setIdentityNotice(backupCodeRevealed ? "CODE REVEALED" : "CODE HIDDEN");
                }
                else if (profileSelected == 2)
                {
                    rotateBackupConfirmation = true;
                }
                else if (recoverIdentity())
                {
                    setIdentityNotice("RECOVERING");
                }
                continue;
            }
            topRenderFrame = 10;
        }
        else if (topMode == TOP_MODE_RULES && (kDown & KEY_B) && gNeedsRules)
        {
            requestExitConfirmation();
            continue;
        }
        else if (topMode == TOP_MODE_IDENTITY && (kDown & KEY_B) && gNeedsDisplayName)
        {
            requestExitConfirmation();
            continue;
        }
        else if ((topMode == TOP_MODE_USERS || topMode == TOP_MODE_ADMIN ||
                  topMode == TOP_MODE_STAFF_CENTER || topMode == TOP_MODE_STATUS ||
                  topMode == TOP_MODE_IDENTITY || topMode == TOP_MODE_CONTROLS ||
                  topMode == TOP_MODE_OPTIONS || topMode == TOP_MODE_RULES) &&
                 (kDown & KEY_B))
        {
            pendingAdminRectTool = ADMIN_RECT_NONE;
            adminRectDragging = false;
            if (routeStack.pop())
                topMode = routeStack.current();
            else
                topMode = TOP_MODE_MENU;
            topRenderFrame = 10;
            continue;
        }
        else if (topMode == TOP_MODE_IDENTITY &&
                 ((kDown & KEY_A) ||
                  ((kDown & KEY_TOUCH) &&
                   pointInRect(touch.px, touch.py, 8, 180, 148, 36))))
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
        else if (topMode == TOP_MODE_IDENTITY &&
                 ((kDown & KEY_X) ||
                  ((kDown & KEY_TOUCH) &&
                   pointInRect(touch.px, touch.py, 164, 180, 148, 36))))
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
        else if (topMode == TOP_MODE_RULES &&
                 ((kDown & KEY_A) ||
                  ((kDown & KEY_TOUCH) &&
                   pointInRect(touch.px, touch.py, 24, 168, 272, 40))) &&
                 (!gNeedsRules || !rulesRequireFreshAPress))
        {
            if (kDown & KEY_TOUCH)
                suppressTouchUntilRelease = true;
            if (!gNeedsRules)
            {
                if (routeStack.pop())
                    topMode = routeStack.current();
                else
                    topMode = TOP_MODE_MENU;
            }
            else if (onboardingStage == ONBOARDING_SUBMITTING_RULES)
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
            if ((navDown & KEY_DUP) && availableChannelCount > 0)
            {
                selectedChannel = (selectedChannel + availableChannelCount - 1) % availableChannelCount;
                topRenderFrame = 10;
            }
            if ((navDown & KEY_DDOWN) && availableChannelCount > 0)
            {
                selectedChannel = (selectedChannel + 1) % availableChannelCount;
                topRenderFrame = 10;
            }
            bool activateChannel = (kDown & KEY_A) != 0;
            if (kDown & KEY_TOUCH)
            {
                for (int item = 0; item < availableChannelCount && item < 8; ++item)
                {
                    if (pointInRect(touch.px, touch.py, 4, 4 + item * 28, 312, 28))
                    {
                        selectedChannel = item;
                        activateChannel = true;
                        break;
                    }
                }
            }
            if (activateChannel)
            {
                if (switchToSelectedChannel() && !pendingChannelSwitch[0])
                {
                    if (routeStack.pop())
                        topMode = routeStack.current();
                    else
                        topMode = TOP_MODE_MENU;
                    setAdminNotice("CHANNEL READY");
                }
                topRenderFrame = 10;
            }
            if (kDown & KEY_B)
            {
                if (routeStack.pop())
                    topMode = routeStack.current();
                else
                    topMode = TOP_MODE_MENU;
                topRenderFrame = 10;
                continue;
            }
        }

        const bool zoomActionHeld = topMode == TOP_MODE_CANVAS &&
                                    !higherPriorityConfirmation &&
                                    !UIState::isColorPickerActive() &&
                                    semanticInput.consumeHeld(gClientSettings.bindings, Doodle::INPUT_ACTION_ZOOM);
        bool zoomOverlayLeft = gClientSettings.zoomOverlaySide == Doodle::ZOOM_OVERLAY_LEFT ||
                               (gClientSettings.zoomOverlaySide == Doodle::ZOOM_OVERLAY_AUTO &&
                                (kHeld & KEY_Y) && !(kHeld & KEY_DRIGHT));
        if (gClientSettings.zoomOverlaySide == Doodle::ZOOM_OVERLAY_RIGHT)
            zoomOverlayLeft = false;
        bool zoomOverlayActive = topMode == TOP_MODE_CANVAS && zoomActionHeld;
        bool blockNormalCanvasInput = gDisconnectReason[0] || gNeedsDisplayName ||
                                      gNeedsRules || restrictionActive ||
                                      higherPriorityConfirmation ||
                                      recoveryCodeExplanation;
        if (UIState::isColorPickerActive() &&
            (topMode != TOP_MODE_CANVAS || blockNormalCanvasInput))
        {
            UIState::setColorPickerActive(false);
            gPaletteAssignMode = false;
            pickerReturnToRoute = false;
            pickerTab = 0;
            pickerDragTarget = PICKER_DRAG_NONE;
        }
        if (!shoulderEraserActive && !blockNormalCanvasInput &&
            !UIState::isColorPickerActive() && topMode == TOP_MODE_CANVAS &&
            semanticInput.consumeDown(gClientSettings.bindings, Doodle::INPUT_ACTION_QUICK_ERASER))
        {
            shoulderSavedBrushShape = currentBrushShape;
            shoulderSavedBrushSizeTenths = currentBrushSizeTenths;
            currentBrushShape = BRUSH_ERASER;
            currentBrushSizeTenths = 70;
            shoulderEraserActive = true;
            topRenderFrame = 10;
        }
        if (shoulderEraserActive &&
            !Doodle::actionIsHeld(gClientSettings.bindings, Doodle::INPUT_ACTION_QUICK_ERASER, kHeld))
        {
            currentBrushShape = shoulderSavedBrushShape;
            currentBrushSizeTenths = shoulderSavedBrushSizeTenths;
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
        if (topMode == TOP_MODE_CANVAS && confirmClearCanvas)
        {
            blockNormalCanvasInput = true;
            const bool cancelClear = (kDown & KEY_B) ||
                                     ((kDown & KEY_TOUCH) &&
                                      pointInRect(touch.px, touch.py, 164, 150, 126, 28));
            const bool applyClear = (kDown & KEY_A) ||
                                    ((kDown & KEY_TOUCH) &&
                                     pointInRect(touch.px, touch.py, 30, 150, 126, 28));
            if (cancelClear)
            {
                if (kDown & KEY_TOUCH)
                    suppressTouchUntilRelease = true;
                confirmClearCanvas = false;
                setAdminNotice("CLEAR CANCELLED");
                continue;
            }
            if (applyClear)
            {
                if (kDown & KEY_TOUCH)
                    suppressTouchUntilRelease = true;
                Color white = {255, 255, 255};
                sendAdminCanvasCommand("clear", 0, 0, canvasWidth, canvasHeight, white);
                confirmClearCanvas = false;
                continue;
            }
        }
        if (topMode == TOP_MODE_CANVAS && pendingAdminRectTool != ADMIN_RECT_NONE)
        {
            blockNormalCanvasInput = true;
            if (adminRectAwaitingConfirm)
            {
                const bool cancelRect = (kDown & KEY_B) ||
                                        ((kDown & KEY_TOUCH) &&
                                         pointInRect(touch.px, touch.py, 164, 198, 148, 36));
                const bool applyRect = (kDown & KEY_A) ||
                                       ((kDown & KEY_TOUCH) &&
                                        pointInRect(touch.px, touch.py, 8, 198, 148, 36));
                if (cancelRect)
                {
                    if (kDown & KEY_TOUCH)
                        suppressTouchUntilRelease = true;
                    pendingAdminRectTool = ADMIN_RECT_NONE;
                    adminRectAwaitingConfirm = false;
                    if (pickerReturnToRoute)
                    {
                        pickerTab = 1;
                        UIState::setColorPickerActive(true);
                    }
                    setAdminNotice("RECT CANCELLED");
                    continue;
                }
                if (applyRect)
                {
                    if (kDown & KEY_TOUCH)
                        suppressTouchUntilRelease = true;
                    const int minX = std::max(0, std::min(adminRectStartX, adminRectEndX));
                    const int minY = std::max(0, std::min(adminRectStartY, adminRectEndY));
                    const int maxX = std::min(canvasWidth - 1, std::max(adminRectStartX, adminRectEndX));
                    const int maxY = std::min(canvasHeight - 1, std::max(adminRectStartY, adminRectEndY));
                    const int rectW = std::max(1, maxX - minX + 1);
                    const int rectH = std::max(1, maxY - minY + 1);
                    Color sendColor = pendingAdminRectTool == ADMIN_RECT_ERASE
                                          ? Color{255, 255, 255}
                                          : currentColor;
                    const char *action = pendingAdminRectTool == ADMIN_RECT_ERASE ? "eraseRect" : "fillRect";
                    sendAdminCanvasCommand(action, minX, minY, rectW, rectH, sendColor);
                    pendingAdminRectTool = ADMIN_RECT_NONE;
                    adminRectAwaitingConfirm = false;
                    if (pickerReturnToRoute)
                    {
                        pickerTab = 1;
                        UIState::setColorPickerActive(true);
                    }
                    continue;
                }
            }
            else if (kDown & KEY_B)
            {
                pendingAdminRectTool = ADMIN_RECT_NONE;
                adminRectDragging = false;
                if (pickerReturnToRoute)
                {
                    pickerTab = 1;
                    UIState::setColorPickerActive(true);
                }
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
                setAdminNotice(pendingAdminRectTool == ADMIN_RECT_ERASE ? "RELEASE TO PREVIEW ERASE" : "RELEASE TO PREVIEW FILL");
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
                adminRectDragging = false;
                adminRectAwaitingConfirm = true;
                setAdminNotice(pendingAdminRectTool == ADMIN_RECT_ERASE ? "A ERASE  B CANCEL" : "A FILL  B CANCEL");
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

        const bool toolsToggle = !blockNormalCanvasInput &&
                                 topMode == TOP_MODE_CANVAS &&
                                 !zoomActionHeld &&
                                 semanticInput.consumeDown(
                                     gClientSettings.bindings,
                                     Doodle::INPUT_ACTION_TOOLS);
        if (toolsToggle && UIState::isColorPickerActive())
        {
            UIState::setColorPickerActive(false);
            gPaletteAssignMode = false;
            pickerDragTarget = PICKER_DRAG_NONE;
            clearAdminNotice();
            pickerTab = 0;
            if (pickerReturnToRoute)
            {
                pickerReturnToRoute = false;
                if (routeStack.pop())
                    topMode = routeStack.current();
                else
                    topMode = TOP_MODE_MENU;
            }
            topRenderFrame = 10;
            continue;
        }
        if (toolsToggle && !UIState::isColorPickerActive())
        {
            pickerReturnToRoute = false;
            UIState::setColorPickerActive(true);
            pendingAdminRectTool = ADMIN_RECT_NONE;
            adminRectDragging = false;
            adminRectAwaitingConfirm = false;
            gPaletteAssignMode = false;
            pickerDragTarget = PICKER_DRAG_NONE;
            pickerTab = 0;
            topMode = TOP_MODE_CANVAS;
            printf("Color picker activated\n");
        }

        // Eye Dropper functionality
        const bool sampleActionHeld = !blockNormalCanvasInput && !zoomOverlayActive &&
                                      topMode == TOP_MODE_CANVAS && !UIState::isColorPickerActive() &&
                                      semanticInput.consumeHeld(gClientSettings.bindings, Doodle::INPUT_ACTION_SAMPLE);
        if (sampleActionHeld)
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
                    gPreviousColor = currentColor;
                    currentColor.r = fullCanvas[idx];
                    currentColor.g = fullCanvas[idx + 1];
                    currentColor.b = fullCanvas[idx + 2];
                    syncPickerToColor(currentColor);
                    markClientSettingsDirty();

                    printf("Color picked: R=%d, G=%d, B=%d\n", currentColor.r, currentColor.g, currentColor.b);
                }
            }
        }

        if (!(kHeld & KEY_TOUCH))
            pickerDragTarget = PICKER_DRAG_NONE;
        if (!blockNormalCanvasInput && !zoomOverlayActive &&
            topMode == TOP_MODE_CANVAS && UIState::isColorPickerActive() &&
            pickerTab == 0 && (kDown & KEY_TOUCH))
        {
            if (pointInRect(touch.px, touch.py,
                            PICKER_SIZE_SLIDER_X, PICKER_SIZE_SLIDER_HIT_Y,
                            PICKER_SIZE_SLIDER_WIDTH, PICKER_SIZE_SLIDER_HIT_HEIGHT))
            {
                pickerDragTarget = PICKER_DRAG_SIZE;
            }
            else if (pointInRect(
                         touch.px, touch.py,
                         PICKER_COLOR_FIELD_X - PICKER_COLOR_FIELD_HIT_SLOP,
                         PICKER_COLOR_FIELD_Y - PICKER_COLOR_FIELD_HIT_SLOP,
                         PICKER_COLOR_FIELD_SIZE + PICKER_COLOR_FIELD_HIT_SLOP * 2,
                         PICKER_COLOR_FIELD_SIZE + PICKER_COLOR_FIELD_HIT_SLOP * 2))
            {
                pickerDragTarget = PICKER_DRAG_COLOR;
            }
            else if (pointInRect(
                         touch.px, touch.py,
                         PICKER_COLOR_FIELD_X - PICKER_COLOR_FIELD_HIT_SLOP,
                         PICKER_HUE_STRIP_Y - 3 - PICKER_COLOR_FIELD_HIT_SLOP,
                         PICKER_COLOR_FIELD_SIZE + PICKER_COLOR_FIELD_HIT_SLOP * 2,
                         PICKER_HUE_STRIP_HEIGHT + 6 + PICKER_COLOR_FIELD_HIT_SLOP * 2))
            {
                pickerDragTarget = PICKER_DRAG_HUE;
            }
        }
        const bool paletteActivate = (kDown & KEY_TOUCH) != 0 ||
                                     (pickerDragTarget == PICKER_DRAG_SIZE &&
                                      (kHeld & KEY_TOUCH));

        if (!blockNormalCanvasInput && !zoomOverlayActive &&
            topMode == TOP_MODE_CANVAS && UIState::isColorPickerActive() &&
            paletteActivate &&
            pickerDragTarget != PICKER_DRAG_COLOR &&
            pickerDragTarget != PICKER_DRAG_HUE)
        {
            if (pointInRect(touch.px, touch.py, 8, 8, 92, 28))
            {
                pickerTab = 0;
                pickerDragTarget = PICKER_DRAG_NONE;
                topRenderFrame = 10;
                continue;
            }
            if (isModOrAdmin() && pointInRect(touch.px, touch.py, 104, 8, 92, 28))
            {
                pickerTab = 1;
                pickerDragTarget = PICKER_DRAG_NONE;
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
                if (pointInRect(touch.px, touch.py, 8, 48, 148, 38))
                {
                    sendAdminCanvasCommand("snapshot", 0, 0, 1, 1, currentColor);
                    continue;
                }
                if (pointInRect(touch.px, touch.py, 164, 48, 148, 38))
                {
                    confirmClearCanvas = true;
                    setAdminNotice("CONFIRM CLEAR");
                    continue;
                }
                if (pointInRect(touch.px, touch.py, 8, 94, 148, 38))
                {
                    pendingAdminRectTool = ADMIN_RECT_FILL;
                    adminRectDragging = false;
                    adminRectAwaitingConfirm = false;
                    UIState::setColorPickerActive(false);
                    setAdminNotice("DRAG SELECTION");
                    continue;
                }
                if (pointInRect(touch.px, touch.py, 164, 94, 148, 38))
                {
                    pendingAdminRectTool = ADMIN_RECT_ERASE;
                    adminRectDragging = false;
                    adminRectAwaitingConfirm = false;
                    UIState::setColorPickerActive(false);
                    setAdminNotice("DRAG ERASE AREA");
                    continue;
                }
                continue;
            }

            if (pickerDragTarget == PICKER_DRAG_SIZE)
            {
                const int sliderX = std::max(
                    PICKER_SIZE_SLIDER_X,
                    std::min((int)touch.px,
                             PICKER_SIZE_SLIDER_X + PICKER_SIZE_SLIDER_WIDTH - 1));
                const int sliderOffset = sliderX - PICKER_SIZE_SLIDER_X;
                const int sizeRange = Doodle::CLIENT_BRUSH_SIZE_MAX_TENTHS -
                                      Doodle::CLIENT_BRUSH_SIZE_MIN_TENTHS;
                const int nextSizeTenths =
                    Doodle::CLIENT_BRUSH_SIZE_MIN_TENTHS +
                    (sliderOffset * sizeRange +
                     (PICKER_SIZE_SLIDER_WIDTH - 1) / 2) /
                        (PICKER_SIZE_SLIDER_WIDTH - 1);
                if (nextSizeTenths != currentBrushSizeTenths)
                {
                    currentBrushSizeTenths = nextSizeTenths;
                    markClientSettingsDirty();
                    topRenderFrame = 10;
                }
                continue;
            }

            for (int shape = 0; shape < 4; ++shape)
            {
                if (pointInRect(touch.px, touch.py, 8 + shape * 76, 42, 72, 28))
                {
                    currentBrushShape = shape;
                    markClientSettingsDirty();
                    continue;
                }
            }
            if (pointInRect(touch.px, touch.py, 110, 166, 30, 28))
            {
                Color swap = currentColor;
                currentColor = gPreviousColor;
                gPreviousColor = swap;
                syncPickerToColor(currentColor);
                markClientSettingsDirty();
                continue;
            }
            for (int slot = 0; slot < 8; ++slot)
            {
                const int column = slot % 4;
                const int row = slot / 4;
                if (!pointInRect(touch.px, touch.py, 150 + column * 39, 108 + row * 39, 34, 34))
                    continue;
                if (gPaletteAssignMode)
                {
                    gCustomPalette[slot] = currentColor;
                    gPaletteAssignMode = false;
                    setAdminNotice("COLOR SAVED");
                    markClientSettingsDirty();
                }
                else
                {
                    gPreviousColor = currentColor;
                    currentColor = gCustomPalette[slot];
                    syncPickerToColor(currentColor);
                    markClientSettingsDirty();
                }
                continue;
            }
            if (pointInRect(touch.px, touch.py, 110, 204, 46, 28))
            {
                gPaletteAssignMode = !gPaletteAssignMode;
                setAdminNotice(gPaletteAssignMode ? "CHOOSE A SWATCH" : "SAVE CANCELLED");
                continue;
            }
            if (pointInRect(touch.px, touch.py, 158, 204, 48, 28))
            {
                confirmPaletteReset = true;
                clearAdminNotice();
                continue;
            }
            if (pointInRect(touch.px, touch.py, 208, 204, 46, 28))
            {
                Color entered = currentColor;
                if (handleHexColorInput(entered))
                {
                    gPreviousColor = currentColor;
                    currentColor = entered;
                    syncPickerToColor(currentColor);
                    setAdminNotice("HEX COLOR SET");
                    markClientSettingsDirty();
                }
                else
                    setAdminNotice("INVALID HEX COLOR");
                continue;
            }
            if (pointInRect(touch.px, touch.py, 256, 204, 52, 28))
            {
                gRainbowEnabled = !gRainbowEnabled;
                gRainbowStrokeColorValid = false;
                setAdminNotice(gRainbowEnabled ? "RAINBOW ENABLED" : "RAINBOW DISABLED");
                continue;
            }
        }

        if (!blockNormalCanvasInput && !zoomOverlayActive && topMode == TOP_MODE_CANVAS &&
            UIState::isColorPickerActive() && pickerTab == 0 && (kHeld & KEY_TOUCH))
        {
            float h, s, v;
            UIState::getHSV(h, s, v);

            bool hsvChanged = false;
            if (pickerDragTarget == PICKER_DRAG_COLOR)
            {
                if (kDown & KEY_TOUCH)
                    gPreviousColor = currentColor;
                s = UiGeometry::normalizedPositionClamped(
                    touch.px,
                    PICKER_COLOR_FIELD_INNER_X,
                    PICKER_COLOR_FIELD_INNER_X + PICKER_COLOR_FIELD_INNER_SIZE - 1);
                v = 1.0f - UiGeometry::normalizedPositionClamped(
                    touch.py,
                    PICKER_COLOR_FIELD_INNER_Y,
                    PICKER_COLOR_FIELD_INNER_Y + PICKER_COLOR_FIELD_INNER_SIZE - 1);
                hsvChanged = true;
            }
            else if (pickerDragTarget == PICKER_DRAG_HUE)
            {
                if (kDown & KEY_TOUCH)
                    gPreviousColor = currentColor;
                h = UiGeometry::normalizedPositionClamped(
                    touch.px,
                    PICKER_COLOR_FIELD_INNER_X,
                    PICKER_COLOR_FIELD_INNER_X + PICKER_COLOR_FIELD_INNER_SIZE - 1);
                hsvChanged = true;
            }

            if (hsvChanged)
            {
                h = std::max(0.0f, std::min(1.0f, h));
                s = std::max(0.0f, std::min(1.0f, s));
                v = std::max(0.0f, std::min(1.0f, v));

                UIState::updateHSV(h, s, v);

                // Convert HSV to RGB
                float r, g, b;
                UIState::HSVtoRGB(h, s, v, r, g, b);
                currentColor.r = clampColor(std::round(r * 255.0f));
                currentColor.g = clampColor(std::round(g * 255.0f));
                currentColor.b = clampColor(std::round(b * 255.0f));
                markClientSettingsDirty();
            }

        }

        const bool panActionHeld = !blockNormalCanvasInput && !zoomOverlayActive &&
                                   topMode == TOP_MODE_CANVAS &&
                                   !UIState::isColorPickerActive() &&
                                   semanticInput.consumeHeld(gClientSettings.bindings, Doodle::INPUT_ACTION_PAN);
        if (panActionHeld)
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
                                             currentBrushSizeTenths, effectiveBrushShape());
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
                                             currentBrushSizeTenths, effectiveBrushShape());
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
                                         currentBrushSizeTenths, effectiveBrushShape());
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
                        const bool completedChannelSwitch =
                            pendingChannelSwitch[0] &&
                            strcmp(pendingChannelSwitch, canvas.channel) == 0;
                        rememberSuccessfulChannel(canvas.channel);
                        pendingChannelSwitch[0] = '\0';
                        pendingChannelSwitchDeadline = 0;
                        setAdminNotice(completedChannelSwitch ? "CHANNEL READY" :
                                       (updateAvailable ? "RECONNECTED - UPDATE AVAILABLE" : "CONNECTED"));
                        if (supportOnlyMode)
                            topMode = TOP_MODE_TICKETS;
                        else if (completedChannelSwitch)
                        {
                            if (routeStack.pop())
                                topMode = routeStack.current();
                            else
                                topMode = TOP_MODE_MENU;
                        }
                        else
                            topMode = TOP_MODE_CANVAS;
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
        if (pendingChannelSwitch[0] && pendingChannelSwitchDeadline > 0 &&
            osGetTime() >= pendingChannelSwitchDeadline)
        {
            pendingChannelSwitch[0] = '\0';
            pendingChannelSwitchDeadline = 0;
            setAdminNotice("CHANNEL SWITCH TIMED OUT");
            topMode = TOP_MODE_CHANNELS;
            topRenderFrame = 10;
        }

        // Rendering
        if (topMode == TOP_MODE_STATUS || gDisconnectReason[0])
            routeStack.showOverlay(UI_OVERLAY_DISCONNECTED);
        else if (confirmClearCanvas || confirmPaletteReset || confirmExit ||
                 adminRectAwaitingConfirm ||
                 moderationConfirmation || rotateBackupConfirmation ||
                 optionsBindingConflict || ticketDraftPreview || ticketReplyPreview)
            routeStack.showOverlay(UI_OVERLAY_CONFIRMATION);
        else if (restrictionActive)
            routeStack.showOverlay(UI_OVERLAY_RESTRICTED);
        else
            routeStack.clearOverlay();

        if (topMode != TOP_MODE_STATUS && routeStack.current() != topMode)
        {
            if (topMode == TOP_MODE_CANVAS)
                routeStack.reset(TOP_MODE_CANVAS);
            else
                routeStack.replace(topMode);
        }
        UiRouteEntry &routeState = routeStack.state();
        if (topMode == TOP_MODE_MENU)
            routeState.focus = selectedMenuItem;
        else if (topMode == TOP_MODE_CHANNELS)
            routeState.focus = selectedChannel;
        else if (topMode == TOP_MODE_USERS)
        {
            routeState.focus = selectedPerson;
            routeState.scroll = peopleScroll;
            routeState.tab = peopleAllChannels ? 1 : 0;
        }
        else if (topMode == TOP_MODE_TICKETS)
        {
            routeState.focus = ticketView == 0 ? ticketHomeSelected : ticketSelected;
            routeState.tab = ticketView;
        }
        else if (topMode == TOP_MODE_OPTIONS)
        {
            routeState.focus = optionsSelected;
            routeState.tab = optionsPage;
        }

        canvas.offsetX = offsetX;
        canvas.offsetY = offsetY;
        bool canvasWasDirty = canvas.dirty.valid;
        Renderer::renderViewport(canvas, buffer, fbWidth, fbHeight, false);
        if (zoomOverlayActive)
            drawZoomOverlay(buffer, fbWidth, fbHeight, zoomOverlayLeft);
        if (pendingAdminRectTool != ADMIN_RECT_NONE &&
            (adminRectDragging || adminRectAwaitingConfirm))
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
                {
                    const u8 previewR = pendingAdminRectTool == ADMIN_RECT_ERASE ? 196 : currentColor.r;
                    const u8 previewG = pendingAdminRectTool == ADMIN_RECT_ERASE ? 61 : currentColor.g;
                    const u8 previewB = pendingAdminRectTool == ADMIN_RECT_ERASE ? 61 : currentColor.b;
                    drawRectOutline(buffer, fbWidth, fbHeight, rectX + 2, rectY + 2, rectMaxX - rectX - 3, rectMaxY - rectY - 3,
                                    previewR, previewG, previewB);
                }
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
            UiCanvas restrictedUi(buffer, fbWidth, fbHeight, UI_BUFFER_3DS_ROTATED_BGR);
            UiComponents::panel(restrictedUi, UiRect(12, 12, 296, 62), true);
            restrictedUi.stroke(UiRect(12, 12, 296, 62), UiTheme::Warning, 2);
            restrictedUi.textClipped(24, 24, remaining, UiTheme::Danger, 272);
            restrictedUi.textClipped(24, 46, restrictionReason, UiTheme::Secondary, 272);
        }
        canvas.clearDirty();

        bool activelyDrawing = !UIState::isColorPickerActive() &&
                               topMode == TOP_MODE_CANVAS &&
                               !panActionHeld &&
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
            {
                adminNoticeExpiresAt = 0;
                topRenderFrame = 10;
            }
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
            char bindingLabels[Doodle::INPUT_ACTION_COUNT][36];
            RendererTopState rendererState;
            memset(&rendererState, 0, sizeof(rendererState));
            rendererState.peopleSelected = selectedPerson;
            rendererState.peopleAllChannels = peopleAllChannels;
            rendererState.presenceTotal = connectedPresenceInfo.total;
            rendererState.presenceTruncated = connectedPresenceInfo.truncated;
            rendererState.channelInfo = availableChannelInfo;
            rendererState.channelInfoCount = availableChannelCount;
            rendererState.backupCodeRevealed = backupCodeRevealed;
            rendererState.needsDisplayName = gNeedsDisplayName;
            rendererState.pageSelected = topMode == TOP_MODE_STAFF_CENTER
                                             ? selectedAdminItem
                                             : (optionsPage == 0 ? optionsSelected :
                                                std::max(0, optionsPage - 1));
            rendererState.controlPreset =
                Doodle::controlPresetLabel(gClientSettings.controlPreset);
            for (int action = 0; action < Doodle::INPUT_ACTION_COUNT; ++action)
            {
                const Doodle::ActionBinding &binding = gClientSettings.bindings.action[action];
                snprintf(bindingLabels[action], sizeof(bindingLabels[action]), "%s%s%s",
                         Doodle::buttonLabel(binding.button[0]),
                         binding.button[1] == Doodle::BUTTON_NONE ? "" : " / ",
                         binding.button[1] == Doodle::BUTTON_NONE ? "" :
                         Doodle::buttonLabel(binding.button[1]));
                rendererState.controlBindings[action] = bindingLabels[action];
            }
            Renderer::renderTop(canvas, NetworkManager::isConnected(), updateAvailable, currentColor,
                                currentBrushSizeTenths, currentBrushShape, topMode,
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
                                restrictionReason, &rendererState);
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
            drawToolPalette(fb, fbWidth, fbHeight, pickerTab,
                            isModOrAdmin(), currentColor,
                            adminNoticeFrames > 0 ? adminNotice : "");
        }
        else if (topMode == TOP_MODE_CANVAS)
            UIInterface::drawCurrentSelection(fb, fbWidth, fbHeight, currentColor);
        else
        {
            BottomUiViewModel bottomView;
            memset(&bottomView, 0, sizeof(bottomView));
            bottomView.mode = topMode;
            bottomView.staff = isModOrAdmin();
            bottomView.admin = strcmp(identityInfo.role, "admin") == 0;
            bottomView.menuSelected = selectedMenuItem;
            bottomView.channels = availableChannels;
            bottomView.channelInfo = availableChannelInfo;
            bottomView.channelCount = availableChannelCount;
            bottomView.channelSelected = selectedChannel;
            bottomView.currentChannel = canvas.channel;
            bottomView.users = connectedUsers;
            bottomView.userCount = connectedUserCount;
            bottomView.personSelected = selectedPerson;
            bottomView.peopleScroll = peopleScroll;
            bottomView.peopleAllChannels = peopleAllChannels;
            bottomView.peopleActionMode = peopleActionMode;
            bottomView.reportUserSelectionMode = reportUserSelectionMode;
            bottomView.peopleActionSelected = peopleActionSelected;
            bottomView.viewerDisplayName = identityInfo.displayName;
            bottomView.viewerUsername = identityInfo.username;
            bottomView.staffSelected = selectedAdminItem;
            bottomView.optionsPage = optionsPage;
            bottomView.optionsSelected = optionsSelected;
            bottomView.optionsBindingSlot = optionsBindingSlot;
            bottomView.optionsBindingCapture = optionsBindingCapture;
            bottomView.optionsBindingConflict = optionsBindingConflict;
            bottomView.settings = &gClientSettings;
            bottomView.ticketView = ticketView;
            bottomView.ticketHomeSelected = ticketHomeSelected;
            bottomView.ticketStaffScope = ticketStaffScope;
            bottomView.supportOnly = supportOnlyMode;
            bottomView.tickets = ticketList;
            bottomView.ticketCount = ticketListCount;
            bottomView.ticketSelected = ticketSelected;
            bottomView.ticketActionSelected = ticketActionSelected;
            bottomView.ticketHasNext = ticketView == 4
                                           ? staffChatNextBeforeId > 0
                                           : ticketNextCursor.id > 0;
            bottomView.ticketHasPrevious = ticketCursorHistoryCount > 0;
            bottomView.ticketActiveIsUnban =
                strcmp(activeTicket.category, "unban") == 0;
            bottomView.backupCodeRevealed = backupCodeRevealed;
            bottomView.profileSelected = profileSelected;
            bottomView.needsDisplayName = gNeedsDisplayName;
            bottomView.needsRules = gNeedsRules;
            bottomView.notice = gDisconnectReason[0] ? gDisconnectReason :
                                (adminNoticeFrames > 0 ? adminNotice : "");
            drawBottomRoute(fb, fbWidth, fbHeight, bottomView);
        }

        UiCanvas overlayUi(fb, fbWidth, fbHeight, UI_BUFFER_3DS_ROTATED_BGR);
        if (adminNoticeFrames > 0 && adminNotice[0] &&
            !UIState::isColorPickerActive() &&
            !confirmClearCanvas && !confirmPaletteReset && !confirmExit &&
            !adminRectAwaitingConfirm &&
            !moderationConfirmation && !rotateBackupConfirmation &&
            !optionsBindingConflict && !ticketDraftPreview &&
            !ticketReplyPreview && !gDisconnectReason[0])
        {
            UiComponents::toast(overlayUi, adminNotice, UiTheme::Accent);
        }
        if (gDisconnectReason[0])
            UiComponents::modal(overlayUi, "Connection",
                                gDisconnectReason, "A Reconnect", "B Menu");
        else if (confirmExit)
            UiComponents::modal(
                overlayUi, "Exit Collab Doodle?",
                "You will disconnect from the current channel. Device settings and saved palette colors will be kept.",
                "A Exit", "B Stay", true);
        else if (confirmClearCanvas)
            UiComponents::modal(overlayUi, "Clear channel?",
                                "Every pixel in this channel will be erased. This cannot be undone.",
                                "A Clear", "B Cancel", true);
        else if (confirmPaletteReset)
            UiComponents::modal(
                overlayUi, "Reset palette?",
                "All eight saved swatches will return to their defaults. Your current drawing color will not change.",
                "A Reset", "B Cancel", true);
        else if (pendingAdminRectTool != ADMIN_RECT_NONE && adminRectAwaitingConfirm)
        {
            UiComponents::button(
                overlayUi, UiRect(8, 198, 148, 36),
                pendingAdminRectTool == ADMIN_RECT_ERASE ? "A Erase area" :
                                                           "A Fill area",
                true, pendingAdminRectTool == ADMIN_RECT_ERASE);
            UiComponents::button(overlayUi, UiRect(164, 198, 148, 36),
                                 "B Cancel", false, false, false);
        }
        else if (optionsBindingConflict)
        {
            char conflictText[120];
            snprintf(conflictText, sizeof(conflictText),
                     "%s is already assigned to %s. Swap the two bindings?",
                     Doodle::buttonLabel(pendingBindingButton),
                     Doodle::inputActionLabel(pendingBindingConflict.action));
            UiComponents::modal(overlayUi, "Binding conflict", conflictText,
                                "A Swap", "B Cancel");
        }
        else if (rotateBackupConfirmation)
            UiComponents::modal(overlayUi, "Rotate recovery code?",
                                "The previous recovery code becomes invalid immediately.",
                                "A Rotate", "B Cancel", true);
        else if (moderationConfirmation)
        {
            const char *actionLabel =
                strcmp(pendingModerationAction, "mute") == 0 ? "Mute 30m" :
                strcmp(pendingModerationAction, "unmute") == 0 ? "Unmute" :
                strcmp(pendingModerationAction, "ban") == 0 ? "Ban" : "Kick";
            char moderationTitle[80];
            char moderationText[180];
            snprintf(moderationTitle, sizeof(moderationTitle), "Confirm %s?",
                     actionLabel);
            snprintf(moderationText, sizeof(moderationText),
                     "User: %.24s\nAccount: %.39s\nReason: %s",
                     pendingModerationTargetName[0] ?
                         pendingModerationTargetName : "Selected user",
                     pendingModerationIdentity,
                     pendingModerationReason[0] ? pendingModerationReason :
                                                  "No reason entered");
            UiComponents::modal(
                overlayUi, moderationTitle, moderationText,
                "A Apply", "B Cancel",
                strcmp(pendingModerationAction, "unmute") != 0);
        }
        else if (ticketDraftPreview)
        {
            overlayUi.fill(UiRect(0, 0, 320, 240), UiTheme::Background);
            UiComponents::panel(overlayUi, UiRect(8, 8, 304, 184), true);
            overlayUi.stroke(UiRect(8, 8, 304, 184), UiTheme::Accent, 2);
            overlayUi.text(20, 20, "Review request", UiTheme::Ink, 2);
            UiComponents::badge(overlayUi, UiRect(220, 18, 80, 20),
                                ticketDraftCategory, UiTheme::Accent);
            overlayUi.text(20, 48, "Subject", UiTheme::Secondary);
            overlayUi.wrappedText(20, 62, ticketDraftSubject,
                                  UiTheme::Ink, 280, 2, 10);
            overlayUi.text(20, 84, "Details", UiTheme::Secondary);
            overlayUi.wrappedText(20, 98, ticketDraftDetails,
                                  UiTheme::Ink, 280, 8, 11);
            UiComponents::button(overlayUi, UiRect(8, 200, 148, 34),
                                 "A Send", true);
            UiComponents::button(overlayUi, UiRect(164, 200, 148, 34),
                                 "B Edit", false, false, false);
        }
        else if (ticketReplyPreview)
        {
            overlayUi.fill(UiRect(0, 0, 320, 240), UiTheme::Background);
            UiComponents::panel(overlayUi, UiRect(8, 8, 304, 184), true);
            overlayUi.stroke(UiRect(8, 8, 304, 184), UiTheme::Accent, 2);
            overlayUi.text(20, 20, "Review reply", UiTheme::Ink, 2);
            overlayUi.text(20, 52, "Message", UiTheme::Secondary);
            overlayUi.wrappedText(20, 68, ticketReplyDraft,
                                  UiTheme::Ink, 280, 10, 11);
            UiComponents::button(overlayUi, UiRect(8, 200, 148, 34),
                                 "A Send", true);
            UiComponents::button(overlayUi, UiRect(164, 200, 148, 34),
                                 "B Edit", false, false, false);
        }
        else if (recoveryCodeExplanation)
        {
            overlayUi.fill(UiRect(0, 0, 320, 240), UiTheme::Background);
            UiComponents::panel(overlayUi, UiRect(12, 18, 296, 154), true);
            overlayUi.stroke(UiRect(12, 18, 296, 154),
                             UiTheme::Accent, 2);
            overlayUi.text(24, 32, "Save your recovery code",
                           UiTheme::Ink, 2);
            overlayUi.textClipped(
                24, 66,
                identityInfo.backupCode[0] ? identityInfo.backupCode :
                                             gIdentity.backupCode,
                UiTheme::Accent, 272, 2);
            overlayUi.wrappedText(
                24, 96,
                "Write this down and keep it private. It is required to recover this account on another device.",
                UiTheme::Secondary, 272, 5, 11);
            UiComponents::button(overlayUi, UiRect(24, 186, 272, 40),
                                 "A Continue", true);
        }

        gfxFlushBuffers();
        gfxSwapBuffers();
    }

    if (clientSettingsDirty)
        flushClientSettings();
    NetworkManager::disconnect();
    free(buffer);
    aptUnhook(&aptCookie);
    gfxExit();
    return 0;
}
