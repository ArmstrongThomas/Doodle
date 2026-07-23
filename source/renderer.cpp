#include "renderer.h"
#include "ui_canvas.h"
#include <algorithm>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static const int MINIMAP_W = 256;
static const int MINIMAP_H = 144;
static const int TOP_SCREEN_W = 400;
static const int TOP_SCREEN_H = 240;
static u8 minimapCache[MINIMAP_W * MINIMAP_H * 3];
static u8 topFrame[TOP_SCREEN_W * TOP_SCREEN_H * 3];
static bool minimapCacheValid = false;
static bool topFrameValid = false;
static int minimapFrameCounter = 0;
static const int BOTTOM_SCREEN_W = 320;
static const int BOTTOM_SCREEN_H = 240;
static int topBatteryPercent = -1;
static bool topBatteryCharging = false;
static u64 topBatteryReadAt = 0;

void Renderer::renderViewport(CanvasState &canvas, u8 *buffer, int fbWidth, int fbHeight, bool forceFull)
{
    if (!canvas.pixels)
        return;

    const bool rotatedFramebuffer = fbWidth < fbHeight;
    for (int screenY = 0; screenY < BOTTOM_SCREEN_H; screenY++)
    {
        for (int screenX = 0; screenX < BOTTOM_SCREEN_W; screenX++)
        {
            int fbX = rotatedFramebuffer ? fbWidth - 1 - screenY : screenX;
            int fbY = rotatedFramebuffer ? screenX : screenY;
            if (fbX < 0 || fbX >= fbWidth || fbY < 0 || fbY >= fbHeight)
                continue;

            int canvasX = canvas.screenToCanvasX(screenX);
            int canvasY = canvas.screenToCanvasY(screenY);
            int bufferIdx = 3 * (fbY * fbWidth + fbX);
            if (canvasX >= 0 && canvasX < canvas.width && canvasY >= 0 && canvasY < canvas.height)
            {
                int canvasIdx = 3 * (canvasY * canvas.width + canvasX);
                buffer[bufferIdx] = canvas.pixels[canvasIdx + 2];
                buffer[bufferIdx + 1] = canvas.pixels[canvasIdx + 1];
                buffer[bufferIdx + 2] = canvas.pixels[canvasIdx];
            }
            else
            {
                buffer[bufferIdx] = buffer[bufferIdx + 1] = buffer[bufferIdx + 2] = 240;
            }
        }
    }
}

static void setTopPixel(u8 *target, int width, int height, int screenX, int screenY, u8 r, u8 g, u8 b)
{
    if (screenX < 0 || screenX >= width || screenY < 0 || screenY >= height)
        return;
    int idx = 3 * (screenY * width + screenX);
    target[idx] = r;
    target[idx + 1] = g;
    target[idx + 2] = b;
}

static void fillTopRect(u8 *target, int width, int height, int x, int y, int w, int h, u8 r, u8 g, u8 b)
{
    for (int py = std::max(0, y); py < std::min(height, y + h); py++)
        for (int px = std::max(0, x); px < std::min(width, x + w); px++)
            setTopPixel(target, width, height, px, py, r, g, b);
}

static void strokeTopRect(u8 *target, int width, int height, int x, int y, int w, int h, u8 r, u8 g, u8 b)
{
    for (int px = x; px < x + w; px++)
    {
        setTopPixel(target, width, height, px, y, r, g, b);
        setTopPixel(target, width, height, px, y + h - 1, r, g, b);
    }
    for (int py = y; py < y + h; py++)
    {
        setTopPixel(target, width, height, x, py, r, g, b);
        setTopPixel(target, width, height, x + w - 1, py, r, g, b);
    }
}


static void drawText(u8 *target, int width, int height, int x, int y, const char *text, u8 r, u8 g, u8 b)
{
    UiCanvas canvas(target, width, height, UI_BUFFER_RGB);
    canvas.textClipped(x, y, text ? text : "", UiColor(r, g, b), std::max(0, width - x));
}

static int drawWrappedText(u8 *target, int width, int height, int x, int y, int maxChars, int maxLines, const char *text, u8 r, u8 g, u8 b)
{
    UiCanvas canvas(target, width, height, UI_BUFFER_RGB);
    return canvas.wrappedText(x, y, text ? text : "", UiColor(r, g, b), maxChars * 6, maxLines);
}

static int wrappedLineCount(int maxChars, int maxLines, const char *text)
{
    int line = 0;
    const char *start = text ? text : "";
    while (*start && line < maxLines)
    {
        int lastSpaceCol = -1;
        int len = 0;
        const char *ptr = start;
        while (*ptr && len < maxChars)
        {
            if (*ptr == ' ')
                lastSpaceCol = len;
            len++;
            ptr++;
        }
        if (*ptr && lastSpaceCol > 8)
            len = lastSpaceCol;
        if (len <= 0)
            len = 1;
        start += len;
        while (*start == ' ')
            start++;
        line++;
    }
    return std::max(1, line);
}

static void drawUpperText(u8 *target, int width, int height, int x, int y, const char *text, u8 r, u8 g, u8 b)
{
    drawText(target, width, height, x, y, text ? text : "", r, g, b);
}

static void formatTitleLabel(const char *source, char *target, size_t targetSize)
{
    if (!target || targetSize == 0) return;
    size_t out = 0;
    bool capitalize = true;
    for (const char *p = source ? source : ""; *p && out + 1 < targetSize; p++)
    {
        char c = *p == '_' ? ' ' : *p;
        if (capitalize && c >= 'a' && c <= 'z') c = c - 'a' + 'A';
        target[out++] = c;
        capitalize = c == ' ';
    }
    target[out] = '\0';
}

static void drawFooterHint(const char *left, const char *right)
{
    fillTopRect(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 0, 212, TOP_SCREEN_W, 1, 206, 214, 220);
    UiCanvas ui(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, UI_BUFFER_RGB);
    const int rightWidth = right && right[0] ? UiCanvas::textWidth(right) : 0;
    const int rightX = rightWidth > 0 ? std::max(206, 388 - rightWidth) : 388;
    if (left && left[0])
        ui.textClipped(12, 224, left, UiTheme::Secondary, std::max(0, rightX - 24));
    if (right && right[0])
        ui.textClipped(rightX, 224, right, UiTheme::Secondary, 388 - rightX);
}

static void drawMenuRow(int y, const char *label, bool selected, bool current = false)
{
    if (selected)
        fillTopRect(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 20, y - 5, 220, 17, 24, 33, 38);
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 30, y, current ? "*" : selected ? ">" : " ", selected ? 245 : 73, selected ? 248 : 82, selected ? 250 : 92);
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 48, y, label, selected ? 245 : 32, selected ? 248 : 36, selected ? 250 : 42);
}

static void drawTopSystemStatus()
{
    time_t now = time(NULL);
    struct tm *local = localtime(&now);
    char clockText[6] = "--:--";
    if (local)
        snprintf(clockText, sizeof(clockText), "%02d:%02d", local->tm_hour, local->tm_min);
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 180, 10, clockText, 218, 226, 232);

    u8 wifi = osGetWifiStrength();
    if (wifi > 3) wifi = 3;
    for (int bar = 0; bar < 3; bar++)
    {
        int barHeight = 3 + bar * 3;
        bool active = wifi > bar;
        fillTopRect(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 220 + bar * 5, 20 - barHeight,
                    3, barHeight, active ? 94 : 73, active ? 234 : 82, active ? 212 : 92);
    }

    u64 readNow = osGetTime();
    if (topBatteryPercent < 0 || readNow - topBatteryReadAt >= 15000ULL)
    {
        u8 level = 0;
        if (R_SUCCEEDED(MCUHWC_GetBatteryLevel(&level)) && level <= 100)
            topBatteryPercent = level;
        else if (R_SUCCEEDED(PTMU_GetBatteryLevel(&level)))
        {
            static const int estimatedPercent[] = { 5, 20, 40, 60, 80, 100 };
            topBatteryPercent = estimatedPercent[std::min((int)level, 5)];
        }
        u8 charging = 0;
        bool adapterConnected = false;
        bool hasChargeState = R_SUCCEEDED(PTMU_GetBatteryChargeState(&charging));
        bool hasAdapterState = R_SUCCEEDED(PTMU_GetAdapterState(&adapterConnected));
        if (hasChargeState || hasAdapterState)
            topBatteryCharging = charging != 0 || adapterConnected;
        topBatteryReadAt = readNow;
    }

    u8 batteryR = topBatteryPercent >= 0 && topBatteryPercent <= 20 ? 255 : (topBatteryPercent <= 40 ? 255 : 94);
    u8 batteryG = topBatteryPercent >= 0 && topBatteryPercent <= 20 ? 115 : (topBatteryPercent <= 40 ? 214 : 234);
    u8 batteryB = topBatteryPercent >= 0 && topBatteryPercent <= 20 ? 115 : (topBatteryPercent <= 40 ? 102 : 212);
    strokeTopRect(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 242, 8, 17, 12, 218, 226, 232);
    fillTopRect(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 259, 11, 2, 6, 218, 226, 232);
    if (topBatteryPercent >= 0)
    {
        int fillWidth = std::max(1, std::min(13, topBatteryPercent * 13 / 100));
        fillTopRect(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 244, 10, fillWidth, 8, batteryR, batteryG, batteryB);
    }
    char batteryText[16];
    if (topBatteryPercent >= 0)
        snprintf(batteryText, sizeof(batteryText), "%d%%%s", topBatteryPercent, topBatteryCharging ? "+" : "");
    else
        snprintf(batteryText, sizeof(batteryText), "--%%");
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 266, 10, batteryText, batteryR, batteryG, batteryB);
}

static void drawTopChrome(bool connected, bool updateAvailable)
{
    fillTopRect(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 0, 0, TOP_SCREEN_W, TOP_SCREEN_H, 232, 236, 239);
    fillTopRect(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 0, 0, TOP_SCREEN_W, 30, 24, 33, 38);
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 12, 10, "Collab Doodle", 245, 248, 250);
    char version[40];
    snprintf(version, sizeof(version), "v%s", APP_BUILD_LABEL);
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 104, 10, version, 160, 176, 184);
    drawTopSystemStatus();
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 316, 10, connected ? "ONLINE" : "OFFLINE",
             connected ? 94 : 255, connected ? 234 : 115, connected ? 212 : 115);
    if (updateAvailable)
        drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 370, 10, "New", 255, 214, 102);
}

void Renderer::invalidateMinimap()
{
    minimapCacheValid = false;
    topFrameValid = false;
}

static void updateMinimapCache(CanvasState &canvas)
{
    if (!canvas.pixels || canvas.width <= 0 || canvas.height <= 0)
        return;

    for (int y = 0; y < MINIMAP_H; y++)
    {
        for (int x = 0; x < MINIMAP_W; x++)
        {
            int cx = x * canvas.width / MINIMAP_W;
            int cy = y * canvas.height / MINIMAP_H;
            int canvasIdx = 3 * (cy * canvas.width + cx);
            int cacheIdx = 3 * (y * MINIMAP_W + x);
            minimapCache[cacheIdx] = canvas.pixels[canvasIdx];
            minimapCache[cacheIdx + 1] = canvas.pixels[canvasIdx + 1];
            minimapCache[cacheIdx + 2] = canvas.pixels[canvasIdx + 2];
        }
    }
    minimapCacheValid = true;
}

static void composeCanvasTopFrame(CanvasState &canvas, bool connected, bool updateAvailable, Color currentColor,
                                   int brushSize, int brushShape, int ticketNeedsReply, int staffChatUnread,
                                   PresenceUser *users, int userCount,
                                   ChannelInfo *channelInfo, int channelInfoCount)
{
    drawTopChrome(connected, updateAvailable);

    if (!canvas.pixels || canvas.width <= 0 || canvas.height <= 0)
    {
        drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 92, 110, "Connecting...", 80, 88, 96);
        topFrameValid = true;
        return;
    }

    const int mapX = 14;
    const int mapY = 44;
    const int mapW = MINIMAP_W;
    const int mapH = MINIMAP_H;
    for (int y = 0; y < mapH; y++)
    {
        for (int x = 0; x < mapW; x++)
        {
            int idx = 3 * (y * MINIMAP_W + x);
            setTopPixel(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, mapX + x, mapY + y,
                        minimapCache[idx], minimapCache[idx + 1], minimapCache[idx + 2]);
        }
    }
    strokeTopRect(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, mapX, mapY, mapW, mapH, 216, 224, 232);

    int viewX = mapX + std::max(0, canvas.offsetX) * mapW / canvas.width;
    int viewY = mapY + std::max(0, canvas.offsetY) * mapH / canvas.height;
    int viewW = std::max(3, canvas.viewWidth(320) * mapW / canvas.width);
    int viewH = std::max(3, canvas.viewHeight(240) * mapH / canvas.height);
    if (viewX + viewW > mapX + mapW) viewW = mapX + mapW - viewX;
    if (viewY + viewH > mapY + mapH) viewH = mapY + mapH - viewY;
    strokeTopRect(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, viewX, viewY, viewW, viewH, 214, 40, 40);
    strokeTopRect(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, viewX + 1, viewY + 1, std::max(1, viewW - 2), std::max(1, viewH - 2), 255, 255, 255);

    int order[24];
    int ordered = 0;
    for (int pass = 0; pass < 2 && ordered < 24; pass++)
    {
        for (int i = 0; users && i < userCount && ordered < 24; i++)
        {
            const char *currentChannel = canvas.channel[0] ? canvas.channel : "main";
            if (users[i].channel[0] && strcmp(users[i].channel, currentChannel) != 0)
                continue;
            bool staff = strcmp(users[i].role, "admin") == 0 || strcmp(users[i].role, "mod") == 0;
            if ((pass == 0 && staff) || (pass == 1 && !staff)) order[ordered++] = i;
        }
    }
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 282, 44, "In channel", 73, 82, 92);
    char onlineCount[16];
    int authoritativeChannelCount = -1;
    const char *currentChannel = canvas.channel[0] ? canvas.channel : "main";
    for (int i = 0; channelInfo && i < channelInfoCount; ++i)
    {
        if (channelInfo[i].name[0] &&
            strcmp(channelInfo[i].name, currentChannel) == 0)
        {
            authoritativeChannelCount = channelInfo[i].userCount;
            break;
        }
    }
    snprintf(onlineCount, sizeof(onlineCount), "%d",
             authoritativeChannelCount >= 0 ? authoritativeChannelCount : ordered);
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 370, 44, onlineCount, 73, 82, 92);
    int visibleUsers = std::min(ordered, 8);
    for (int row = 0; row < visibleUsers; row++)
    {
        PresenceUser &user = users[order[row]];
        int y = 61 + row * 16;
        bool admin = strcmp(user.role, "admin") == 0;
        bool mod = strcmp(user.role, "mod") == 0;
        drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 282, y, admin ? "A" : mod ? "M" : "-",
                 admin || mod ? 13 : 104, admin || mod ? 122 : 114, admin || mod ? 117 : 124);
        char name[17];
        snprintf(name, sizeof(name), "%.16s", user.displayName[0] ? user.displayName : user.username);
        drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 294, y, name, 32, 36, 42);
    }
    if (visibleUsers == 0)
        drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 294, 76, "Nobody yet", 104, 114, 124);

    const char *brushName = brushShape == 1 ? "Square" : brushShape == 2 ? "Dither" : brushShape == 3 ? "Eraser" : "Circle";
    char drawingInfo[44];
    snprintf(drawingInfo, sizeof(drawingInfo), "%.10s | %s %d | %s",
             canvas.channel[0] ? canvas.channel : "main", brushName, brushSize, canvas.zoomLabel());
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 14, 198, drawingInfo, 32, 36, 42);
    fillTopRect(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 252, 196, 14, 12, currentColor.r, currentColor.g, currentColor.b);
    strokeTopRect(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 252, 196, 14, 12, 32, 36, 42);

    if (ticketNeedsReply > 0)
    {
        char text[18];
        snprintf(text, sizeof(text), "%d ticket%s", std::min(ticketNeedsReply, 99), ticketNeedsReply == 1 ? "" : "s");
        drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 282, 194, text, 196, 92, 40);
    }
    if (staffChatUnread > 0)
    {
        char text[18];
        snprintf(text, sizeof(text), "%d chat new", std::min(staffChatUnread, 99));
        drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 282, 204, text, 13, 122, 117);
    }
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 12, 224, "SELECT MENU", 73, 82, 92);
    topFrameValid = true;
}

static ChannelInfo *findChannelInfo(ChannelInfo *items, int count, const char *name)
{
    for (int i = 0; items && i < count; ++i)
        if (strcmp(items[i].name, name ? name : "") == 0)
            return &items[i];
    return NULL;
}

static void composeChannelTopFrame(CanvasState &canvas, bool connected, bool updateAvailable,
                                   char channels[][25], int channelCount, int selectedChannel,
                                   ChannelInfo *channelInfo, int channelInfoCount)
{
    drawTopChrome(connected, updateAvailable);
    UiCanvas ui(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, UI_BUFFER_RGB);
    ui.text(12, 39, "Channels", UiTheme::Ink);
    const char *currentName = canvas.channel[0] ? canvas.channel : "main";
    char currentLabel[48];
    snprintf(currentLabel, sizeof(currentLabel), "Current: %.24s", currentName);
    ui.textClipped(224, 39, currentLabel, UiTheme::Accent, 164);

    const int selected = channelCount > 0 ? std::max(0, std::min(selectedChannel, channelCount - 1)) : 0;
    const int visibleRows = 7;
    int start = std::max(0, selected - visibleRows + 1);
    if (channelCount - start < visibleRows)
        start = std::max(0, channelCount - visibleRows);
    int rows = std::min(visibleRows, std::max(0, channelCount - start));
    for (int row = 0; row < rows; row++)
    {
        const int index = start + row;
        ChannelInfo *info = findChannelInfo(channelInfo, channelInfoCount, channels[index]);
        char meta[48] = "";
        if (info)
        {
            const char *access = info->adminOnly ? "ADMIN" : info->staffOnly ? "STAFF" : "";
            const char *readOnly = info->readOnly ? "READ ONLY" : "";
            if (access[0] && readOnly[0])
                snprintf(meta, sizeof(meta), "%d  %s / %s", info->userCount, access, readOnly);
            else if (access[0] || readOnly[0])
                snprintf(meta, sizeof(meta), "%d  %s", info->userCount, access[0] ? access : readOnly);
            else
                snprintf(meta, sizeof(meta), "%d online", info->userCount);
        }
        UiComponents::listRow(ui, UiRect(12, 56 + row * 22, 376, 20),
                              channels[index], meta, index == selected,
                              strcmp(currentName, channels[index]) == 0);
    }

    if (rows == 0)
    {
        UiComponents::panel(ui, UiRect(12, 56, 376, 50), true);
        ui.text(28, 77, "No channels are available.", UiTheme::Secondary);
    }

    if (channelCount > visibleRows)
    {
        char page[48];
        snprintf(page, sizeof(page), "%d-%d of %d", start + 1, start + rows, channelCount);
        ui.text(12, 205, page, UiTheme::Secondary);
    }

    drawFooterHint("A SWITCH", "B BACK  SELECT MENU");
    topFrameValid = true;
}

static void composeControlsTopFrame(CanvasState &canvas, bool connected, bool updateAvailable)
{
    drawTopChrome(connected, updateAvailable);
    UiCanvas ui(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, UI_BUFFER_RGB);
    ui.text(12, 39, "Controls moved", UiTheme::Ink);
    UiComponents::panel(ui, UiRect(12, 58, 376, 108), true);
    ui.text(28, 74, "Controls & Presets now lives in Options.", UiTheme::Ink);
    ui.wrappedText(28, 94,
                   "Bindings shown in Help are read from your active preset, so hints always match input.",
                   UiTheme::Secondary, 340, 3);
    char channel[48];
    snprintf(channel, sizeof(channel), "Current channel: %.24s", canvas.channel[0] ? canvas.channel : "main");
    ui.textClipped(28, 140, channel, UiTheme::Accent, 340);
    drawFooterHint("A OPEN OPTIONS", "B BACK");

    topFrameValid = true;
}

static void composeMenuTopFrame(CanvasState &canvas, bool connected, bool updateAvailable, int selectedMenuItem,
                                int ticketNeedsReplyCount, int staffChatUnreadCount, bool showAdminTools)
{
    drawTopChrome(connected, updateAvailable);
    UiCanvas ui(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, UI_BUFFER_RGB);
    ui.text(12, 38, "Menu", UiTheme::Ink);

    const char *regularItems[] = {
        "Channels", "People", "Support", "Profile",
        "Options", "Help & Rules", "Exit"
    };
    const char *staffItems[] = {
        "Channels", "People", "Support", "Staff Center",
        "Profile", "Options", "Help & Rules", "Exit"
    };
    const char **items = showAdminTools ? staffItems : regularItems;
    const int itemCount = showAdminTools ? 8 : 7;
    const int selected = std::max(0, std::min(selectedMenuItem, itemCount - 1));
    for (int i = 0; i < itemCount; i++)
        UiComponents::listRow(ui, UiRect(12, 52 + i * 19, 174, 17),
                              items[i], "", i == selected);

    UiComponents::panel(ui, UiRect(196, 52, 192, 144), true);
    ui.textClipped(208, 64, items[selected], UiTheme::Accent, 168);
    static const char *regularDescriptions[] = {
        "Browse rooms and switch after the new canvas is ready.",
        "See who is drawing here, or browse every channel.",
        "Create a request and follow replies from staff.",
        "Display name, account state, and recovery tools.",
        "Controls, palette, connection, and app details.",
        "Community rules plus live control hints.",
        "Close Collab Doodle."
    };
    static const char *staffDescriptions[] = {
        "Browse rooms and switch after the new canvas is ready.",
        "See people and open moderation actions.",
        "Create a request and follow your ticket replies.",
        "Ticket queue, staff chat, and canvas tools.",
        "Display name, account state, and recovery tools.",
        "Controls, palette, connection, and app details.",
        "Community rules plus live control hints.",
        "Close Collab Doodle."
    };
    ui.wrappedText(208, 82, showAdminTools ? staffDescriptions[selected] : regularDescriptions[selected],
                   UiTheme::Secondary, 168, 4);

    if (ticketNeedsReplyCount > 0)
    {
        char ticketText[24];
        snprintf(ticketText, sizeof(ticketText), "%d ticket%s need you", std::min(ticketNeedsReplyCount, 99),
                 ticketNeedsReplyCount == 1 ? "" : "s");
        ui.textClipped(208, 134, ticketText, UiTheme::Warning, 168);
    }
    if (staffChatUnreadCount > 0)
    {
        char chatText[24];
        snprintf(chatText, sizeof(chatText), "%d staff chat new", std::min(staffChatUnreadCount, 99));
        ui.textClipped(208, 148, chatText, UiTheme::Accent, 168);
    }

    char connection[64];
    snprintf(connection, sizeof(connection), "%s / %.24s", connected ? "Online" : "Offline",
             canvas.channel[0] ? canvas.channel : "main");
    ui.textClipped(208, 178, connection, connected ? UiTheme::Accent : UiTheme::Danger, 168);
    drawFooterHint("A OPEN", "B CLOSE");
    topFrameValid = true;
}

static void composeRulesTopFrame(bool connected, bool updateAvailable, const char *requiredVersion,
                                 bool needsAgreement, const char *notice,
                                 const RendererTopState *topState)
{
    drawTopChrome(connected, updateAvailable);
    UiCanvas ui(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, UI_BUFFER_RGB);
    ui.text(12, 38, "Help & Rules", UiTheme::Ink);
    if (requiredVersion && requiredVersion[0])
    {
        char versionText[32];
        snprintf(versionText, sizeof(versionText), "v%s", requiredVersion);
        ui.textClipped(350, 38, versionText, UiTheme::Secondary, 38);
    }

    UiComponents::panel(ui, UiRect(12, 52, 238, 152), true);
    ui.text(22, 62, "Community rules", UiTheme::Accent);
    const char *rules[] = {
        "No sexual content.",
        "No heavy profanity or slurs.",
        "No harassment, threats, hate, or personal info.",
        "No intentional griefing, spam, or vandalism.",
        "Mods may clear, kick, mute, ban, or save evidence.",
    };
    int y = 78;
    for (int i = 0; i < 5; i++)
    {
        ui.text(22, y, "-", UiTheme::Accent);
        ui.wrappedText(34, y, rules[i], UiTheme::Ink, 204, 2, 9);
        y += i >= 2 ? 25 : 18;
    }

    UiComponents::panel(ui, UiRect(258, 52, 130, 152), true);
    ui.text(268, 62, "Live controls", UiTheme::Accent);
    static const char *actions[] = { "Tools", "Pan", "Sample", "Zoom", "Quick Eraser", "Refresh" };
    for (int i = 0; i < 6; ++i)
    {
        const char *binding = topState && topState->controlBindings[i] && topState->controlBindings[i][0]
                                ? topState->controlBindings[i] : "See Options";
        const int rowY = 78 + i * 20;
        ui.textClipped(268, rowY, actions[i], UiTheme::Ink, 110);
        ui.textClipped(268, rowY + 9, binding, UiTheme::Secondary, 110);
    }

    if (notice && notice[0])
    {
        fillTopRect(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 12, 202, 376, 10, 232, 236, 239);
        ui.textClipped(12, 203, notice, UiTheme::Warning, 376);
    }
    drawFooterHint(needsAgreement ? "A AGREE & CONTINUE" : "", needsAgreement ? "B EXIT" : "B BACK");
    topFrameValid = true;
}

static bool isStaffRole(const char *role)
{
    return role && (strcmp(role, "admin") == 0 || strcmp(role, "mod") == 0);
}

static bool isAnonymousUser(const PresenceUser &user)
{
    return !user.identityId[0] && !user.username[0];
}

static int asciiCaseCompare(const char *left, const char *right)
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

static const char *presenceName(const PresenceUser &user)
{
    if (user.displayName[0])
        return user.displayName;
    if (user.username[0])
        return user.username;
    return "Anonymous viewer";
}

static bool isCurrentAccount(const PresenceUser &user, const char *displayName, const char *username)
{
    if (username && username[0] && user.username[0])
        return strcmp(user.username, username) == 0;
    return displayName && displayName[0] && user.displayName[0] &&
           strcmp(user.displayName, displayName) == 0;
}

static void composeUsersTopFrame(CanvasState &canvas, bool connected, bool updateAvailable,
                                 PresenceUser *users, int userCount,
                                 const char *displayName, const char *username, const char *viewerRole,
                                 int selectedUser, bool allChannels,
                                 int presenceTotal, bool presenceTruncated)
{
    drawTopChrome(connected, updateAvailable);
    UiCanvas ui(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, UI_BUFFER_RGB);
    ui.text(12, 38, "People", UiTheme::Ink);

    int order[24];
    int ordered = 0;
    for (int i = 0; users && i < userCount && i < 24; ++i)
    {
        if (!allChannels && users[i].channel[0] && strcmp(users[i].channel, canvas.channel) != 0)
            continue;
        order[ordered++] = i;
    }
    for (int i = 1; i < ordered; ++i)
    {
        int candidate = order[i];
        int j = i - 1;
        while (j >= 0)
        {
            const PresenceUser &a = users[candidate];
            const PresenceUser &b = users[order[j]];
            int aTier = isCurrentAccount(a, displayName, username) ? 0 :
                        isStaffRole(a.role) ? 1 : isAnonymousUser(a) ? 3 : 2;
            int bTier = isCurrentAccount(b, displayName, username) ? 0 :
                        isStaffRole(b.role) ? 1 : isAnonymousUser(b) ? 3 : 2;
            if (aTier > bTier || (aTier == bTier &&
                asciiCaseCompare(presenceName(a), presenceName(b)) >= 0))
                break;
            order[j + 1] = order[j];
            --j;
        }
        order[j + 1] = candidate;
    }

    char scope[64];
    if (allChannels && presenceTruncated && presenceTotal > ordered)
        snprintf(scope, sizeof(scope), "All / %d of %d", ordered, presenceTotal);
    else
        snprintf(scope, sizeof(scope), "%s / %d", allChannels ? "All channels" :
                 (canvas.channel[0] ? canvas.channel : "main"), ordered);
    ui.textClipped(244, 38, scope, UiTheme::Secondary, 144);

    const int selected = ordered > 0 ? std::max(0, std::min(selectedUser, ordered - 1)) : 0;
    const int visibleRows = 7;
    int start = std::max(0, selected - visibleRows + 1);
    if (ordered - start < visibleRows)
        start = std::max(0, ordered - visibleRows);
    const int rows = std::min(visibleRows, ordered - start);
    for (int row = 0; row < rows; ++row)
    {
        const int rank = start + row;
        PresenceUser &user = users[order[rank]];
        char meta[16] = "";
        if (user.sessionCount > 1)
            snprintf(meta, sizeof(meta), "%dx", user.sessionCount);
        else if (isStaffRole(user.role))
            snprintf(meta, sizeof(meta), "%s", strcmp(user.role, "admin") == 0 ? "A" : "M");
        UiComponents::listRow(ui, UiRect(12, 56 + row * 22, 154, 20),
                              presenceName(user), meta, rank == selected,
                              isCurrentAccount(user, displayName, username));
    }

    UiComponents::panel(ui, UiRect(174, 56, 214, 146), true);
    if (ordered == 0)
    {
        ui.wrappedText(186, 78, allChannels ? "Nobody is online." : "Nobody is in this channel.",
                       UiTheme::Secondary, 190, 2);
    }
    else
    {
        PresenceUser &user = users[order[selected]];
        const bool self = isCurrentAccount(user, displayName, username);
        char roleLabel[16], statusLabel[18];
        formatTitleLabel(user.role[0] ? user.role : (isAnonymousUser(user) ? "viewer" : "user"),
                         roleLabel, sizeof(roleLabel));
        formatTitleLabel(user.status[0] ? user.status : "active", statusLabel, sizeof(statusLabel));
        ui.textClipped(186, 68, presenceName(user), UiTheme::Ink, 190);
        UiComponents::badge(ui, UiRect(186, 82, 64, 16), roleLabel,
                            isStaffRole(user.role) ? UiTheme::Accent : UiTheme::Secondary);
        UiComponents::badge(ui, UiRect(256, 82, 72, 16), statusLabel,
                            strcmp(user.status, "active") == 0 ? UiTheme::Accent :
                            strcmp(user.status, "banned") == 0 ? UiTheme::Danger : UiTheme::Warning);
        if (self)
            ui.text(340, 86, "YOU", UiTheme::Accent);

        const char *device = user.deviceModelLabel[0] ? user.deviceModelLabel :
                             user.deviceModel[0] ? user.deviceModel :
                             user.clientType[0] ? user.clientType : "Unknown";
        char sessions[20];
        snprintf(sessions, sizeof(sessions), "%d", std::max(1, user.sessionCount));
        const char *labels[] = { "Channel", "Device", "Sessions" };
        const char *values[] = { user.channel[0] ? user.channel :
                                (canvas.channel[0] ? canvas.channel : "main"), device, sessions };
        for (int i = 0; i < 3; ++i)
        {
            int y = 108 + i * 22;
            ui.text(186, y, labels[i], UiTheme::Secondary);
            ui.textClipped(252, y, values[i], UiTheme::Ink, 122);
        }
        if (self)
            ui.text(186, 178, "Your current account", UiTheme::Secondary);
        else if (isStaffRole(viewerRole) && !isAnonymousUser(user))
            ui.text(186, 178, "Staff actions are below", UiTheme::Accent);
        else if (isAnonymousUser(user))
            ui.text(186, 178, "Anonymous session", UiTheme::Secondary);
    }

    drawFooterHint(isStaffRole(viewerRole) ? "A ACTIONS  X SCOPE" : "X CHANGE SCOPE", "B BACK");
    topFrameValid = true;
}


static void composeTicketsTopFrame(bool connected, bool updateAvailable,
                                   SupportTicketSummary *tickets, int ticketCount, int ticketSelected,
                                   int ticketView, bool staffScope, SupportTicketSummary *activeTicket,
                                   SupportTicketMessage *messages, int messageCount,
                                   int homeSelected, int actionSelected, bool supportOnly,
                                   const char *supportReason, const char *notice, int needsReplyCount,
                                   StaffChatMessage *staffMessages, int staffMessageCount, int staffChatUnread,
                                   int restrictionSecondsRemaining, bool restrictionHasDuration)
{
    drawTopChrome(connected, updateAvailable);
    UiCanvas ui(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, UI_BUFFER_RGB);
    ui.text(12, 38, supportOnly ? "Support Access" : "Support", UiTheme::Ink);
    if (needsReplyCount > 0)
    {
        char countText[24];
        snprintf(countText, sizeof(countText), "%d need reply", std::min(needsReplyCount, 99));
        ui.textClipped(292, 38, countText, UiTheme::Warning, 96);
    }

    if (ticketView == 0)
    {
        const char *regularItems[] = {
            "New Bug Request",
            "New Feature Request",
            "Report a User",
            "My Tickets"
        };
        const char *supportItems[] = {
            "New Appeal",
            "My Appeals",
            "Profile",
            "Exit"
        };
        const char **items = supportOnly ? supportItems : regularItems;
        const int count = 4;
        const int selected = std::max(0, std::min(homeSelected, count - 1));
        for (int i = 0; i < count; i++)
            UiComponents::listRow(ui, UiRect(12, 56 + i * 30, 182, 26),
                                  items[i], "", i == selected);

        UiComponents::panel(ui, UiRect(202, 56, 186, 140), true);
        ui.textClipped(214, 70, items[selected], UiTheme::Accent, 162);
        if (supportOnly)
        {
            const char *descriptions[] = {
                "Ask staff to review the restriction on this account.",
                "Read staff replies and continue an existing appeal.",
                "View your display name, account state, and recovery tools.",
                "Close Collab Doodle."
            };
            ui.wrappedText(214, 90, descriptions[selected], UiTheme::Secondary, 162, 3);
            ui.text(214, 126, "Restriction", UiTheme::Warning);
            ui.wrappedText(214, 138,
                           supportReason && supportReason[0] ? supportReason :
                           "Canvas access is restricted.",
                           UiTheme::Danger, 162, 2);
            char remaining[40];
            if (restrictionHasDuration)
                snprintf(remaining, sizeof(remaining), "Access returns in %02dh %02dm", restrictionSecondsRemaining / 3600, (restrictionSecondsRemaining / 60) % 60);
            else
                snprintf(remaining, sizeof(remaining), "No automatic expiration");
            ui.textClipped(214, 174, remaining, UiTheme::Warning, 162);
        }
        else
        {
            const char *descriptions[] = {
                "Report broken behavior with steps staff can reproduce.",
                "Suggest an improvement or a new quality-of-life feature.",
                "Report harmful behavior with the user and incident details.",
                "Read replies and continue your existing requests."
            };
            ui.wrappedText(214, 90, descriptions[selected], UiTheme::Secondary, 162, 5);
        }
        drawFooterHint("A OPEN", supportOnly ? "SELECT MENU" : "B MENU");
    }
    else if (ticketView == 1)
    {
        drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 20, 54, staffScope ? "Staff queue" : "My tickets", 13, 122, 117);
        for (int i = 0; tickets && i < ticketCount && i < 6; i++)
        {
            int y = 70 + i * 21;
            if (i == ticketSelected)
            {
                fillTopRect(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 14, y - 3, 372, 19, 224, 242, 238);
                strokeTopRect(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 14, y - 3, 372, 19, 13, 122, 117);
            }
            char category[24], status[24], prefix[64];
            formatTitleLabel(tickets[i].category, category, sizeof(category));
            formatTitleLabel(tickets[i].status, status, sizeof(status));
            snprintf(prefix, sizeof(prefix), "#%d %s / %s", tickets[i].id, category, status);
            drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 20, y, prefix, 13, 122, 117);
            char subject[25];
            snprintf(subject, sizeof(subject), "%.24s", tickets[i].subject);
            drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 154, y, subject, 32, 36, 42);
            if (staffScope)
            {
                char requester[13];
                snprintf(requester, sizeof(requester), "%.12s",
                         tickets[i].displayName[0] ? tickets[i].displayName : tickets[i].username);
                drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 310, y, requester, 104, 114, 124);
            }
        }
        if (ticketCount == 0)
            drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 40, 100, "No tickets found", 104, 114, 124);
        drawFooterHint("A OPEN  X REFRESH  Y NEXT", "B BACK");
    }
    else if (ticketView == 2 && activeTicket)
    {
        char category[24], status[24], heading[96];
        formatTitleLabel(activeTicket->category, category, sizeof(category));
        formatTitleLabel(activeTicket->status, status, sizeof(status));
        snprintf(heading, sizeof(heading), "#%d %s / %s", activeTicket->id, category, status);
        drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 20, 54, heading, 13, 122, 117);
        char ticketSubject[61];
        snprintf(ticketSubject, sizeof(ticketSubject), "%.60s", activeTicket->subject);
        drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 20, 68, ticketSubject, 32, 36, 42);
        if (staffScope)
        {
            char context[128];
            snprintf(context, sizeof(context), "%.42s  Blocks: %.12s",
                     activeTicket->banReason[0] ? activeTicket->banReason : "No ban reason",
                     activeTicket->blockTypes[0] ? activeTicket->blockTypes : "None");
            drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 20, 82, context, 196, 92, 40);
        }
        const int firstY = staffScope ? 98 : 82;
        int start = messageCount;
        int usedHeight = 0;
        for (int i = messageCount - 1; messages && i >= 0; i--)
        {
            int lines = wrappedLineCount(58, 3, messages[i].message);
            int rowHeight = 13 + lines * 10;
            if (usedHeight + rowHeight > 200 - firstY) break;
            usedHeight += rowHeight;
            start = i;
        }
        int y = firstY;
        for (int i = start; messages && i < messageCount && y < 200; i++)
        {
            char timeText[6] = "--:--";
            if (messages[i].createdAt[11] && messages[i].createdAt[12] && messages[i].createdAt[14] && messages[i].createdAt[15])
                snprintf(timeText, sizeof(timeText), "%.2s:%.2s", messages[i].createdAt + 11, messages[i].createdAt + 14);
            char authorKind[20], author[72];
            formatTitleLabel(messages[i].authorKind, authorKind, sizeof(authorKind));
            snprintf(author, sizeof(author), "[%s] %s  %s", authorKind,
                     messages[i].displayName[0] ? messages[i].displayName : "Staff", timeText);
            drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 20, y, author, 13, 122, 117);
            int lines = wrappedLineCount(58, 3, messages[i].message);
            drawWrappedText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 32, y + 11, 58, 3, messages[i].message, 32, 36, 42);
            y += 13 + lines * 10;
        }
        bool closed = strcmp(activeTicket->status, "resolved") == 0 || strcmp(activeTicket->status, "rejected") == 0;
        if (staffScope)
            drawFooterHint("A REPLY  X ACTIONS", "B LIST");
        else
            drawFooterHint(closed ? "CLOSED" : "A REPLY", "B LIST");
    }
    else if (ticketView == 3 && activeTicket)
    {
        drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 20, 56, "Staff actions", 13, 122, 117);
        const char *actions[] = { "Mark in progress", "Reply to user", "Resolve", "Reject", "Approve unban", "Reopen" };
        for (int i = 0; i < 6; i++)
        {
            bool unavailable = (i == 4 && strcmp(activeTicket->category, "unban") != 0);
            if (unavailable)
                drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 36, 80 + i * 22, "Approve unban (N/A)", 160, 166, 172);
            else
                drawMenuRow(80 + i * 22, actions[i], i == actionSelected);
        }
        drawFooterHint("A CONFIRM", "B THREAD");
    }
    else if (ticketView == 4)
    {
        char heading[40];
        snprintf(heading, sizeof(heading), "Staff chat%s", staffChatUnread > 0 ? " - new" : "");
        drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 20, 54, heading, 13, 122, 117);
        const int firstY = 70;
        int start = staffMessageCount;
        int usedHeight = 0;
        for (int i = staffMessageCount - 1; staffMessages && i >= 0; i--)
        {
            int lines = wrappedLineCount(58, 3, staffMessages[i].message);
            int rowHeight = 13 + lines * 10;
            if (usedHeight + rowHeight > 200 - firstY) break;
            usedHeight += rowHeight;
            start = i;
        }
        int y = firstY;
        for (int i = start; staffMessages && i < staffMessageCount && y < 200; i++)
        {
            char timeText[6] = "--:--";
            if (staffMessages[i].createdAt[11] && staffMessages[i].createdAt[12] && staffMessages[i].createdAt[14] && staffMessages[i].createdAt[15])
                snprintf(timeText, sizeof(timeText), "%.2s:%.2s", staffMessages[i].createdAt + 11, staffMessages[i].createdAt + 14);
            char role[16], prefix[72];
            const char *name = staffMessages[i].displayName[0] ? staffMessages[i].displayName : staffMessages[i].username;
            formatTitleLabel(staffMessages[i].role, role, sizeof(role));
            snprintf(prefix, sizeof(prefix), "[%s] %s  %s", role, name, timeText);
            drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 20, y, prefix, 13, 122, 117);
            int lines = wrappedLineCount(58, 3, staffMessages[i].message);
            drawWrappedText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 32, y + 11, 58, 3, staffMessages[i].message, 32, 36, 42);
            y += 13 + lines * 10;
        }
        if (staffMessageCount == 0)
            drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 40, 100, "No staff messages yet", 104, 114, 124);
        drawFooterHint("A SEND  X REFRESH  Y OLDER", "B BACK");
    }

    if (notice && notice[0])
    {
        char compactNotice[61];
        snprintf(compactNotice, sizeof(compactNotice), "%.60s", notice);
        drawUpperText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 20, 205, compactNotice, 196, 92, 40);
    }
    topFrameValid = true;
}

static void composeOptionsTopFrame(CanvasState &canvas, bool connected, bool updateAvailable,
                                   Color currentColor, int brushSize, int brushShape,
                                   const RendererTopState *topState)
{
    drawTopChrome(connected, updateAvailable);
    UiCanvas ui(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, UI_BUFFER_RGB);
    ui.text(12, 38, "Options", UiTheme::Ink);

    const char *sections[] = {
        "Controls & Presets",
        "Drawing & Palette",
        "Connection & About"
    };
    const int selected = topState ? std::max(0, std::min(topState->pageSelected, 2)) : 0;
    for (int i = 0; i < 3; ++i)
        UiComponents::listRow(ui, UiRect(12, 56 + i * 28, 164, 24),
                              sections[i], "", i == selected);

    UiComponents::panel(ui, UiRect(184, 56, 204, 146), true);
    ui.textClipped(196, 68, sections[selected], UiTheme::Accent, 180);
    if (selected == 0)
    {
        const char *preset = topState && topState->controlPreset && topState->controlPreset[0]
                                ? topState->controlPreset : "Balanced";
        char presetLabel[48];
        snprintf(presetLabel, sizeof(presetLabel), "Preset: %.24s", preset);
        ui.textClipped(196, 84, presetLabel, UiTheme::Ink, 180);
        static const char *actions[] = { "Tools", "Pan", "Sample", "Zoom", "Eraser", "Refresh" };
        for (int i = 0; i < 6; ++i)
        {
            const char *binding = topState && topState->controlBindings[i] &&
                                  topState->controlBindings[i][0]
                                    ? topState->controlBindings[i] : "Default";
            const int y = 104 + i * 15;
            ui.textClipped(196, y, actions[i], UiTheme::Secondary, 68);
            ui.textClipped(268, y, binding, UiTheme::Ink, 108);
        }
    }
    else if (selected == 1)
    {
        const char *shape = brushShape == 1 ? "Square" : brushShape == 2 ? "Dither" :
                            brushShape == 3 ? "Eraser" : "Circle";
        char brush[48];
        snprintf(brush, sizeof(brush), "%s / size %d", shape, brushSize);
        ui.textClipped(196, 88, brush, UiTheme::Ink, 150);
        ui.fill(UiRect(352, 84, 24, 16), UiColor(currentColor.r, currentColor.g, currentColor.b));
        ui.stroke(UiRect(352, 84, 24, 16), UiTheme::Ink);
        ui.wrappedText(196, 112,
                       "Eight favorite colors, solid color, brush, and zoom-side settings persist on this system.",
                       UiTheme::Secondary, 180, 5);
    }
    else
    {
        char state[64], version[48];
        snprintf(state, sizeof(state), "%s / %.24s", connected ? "Online" : "Offline",
                 canvas.channel[0] ? canvas.channel : "main");
        snprintf(version, sizeof(version), "Client %s", APP_BUILD_LABEL);
        ui.textClipped(196, 88, state, connected ? UiTheme::Accent : UiTheme::Danger, 180);
        ui.textClipped(196, 108, version, UiTheme::Ink, 180);
        ui.wrappedText(196, 132,
                       "Connection recovery and updates appear automatically when attention is needed.",
                       UiTheme::Secondary, 180, 4);
    }
    drawFooterHint("A OPEN", "B BACK");
    topFrameValid = true;
}

static void composeStaffCenterTopFrame(CanvasState &canvas, bool connected, bool updateAvailable,
                                       const char *role, int selectedItem,
                                       int ticketNeedsReply, int staffChatUnread)
{
    drawTopChrome(connected, updateAvailable);
    UiCanvas ui(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, UI_BUFFER_RGB);
    ui.text(12, 38, "Staff Center", UiTheme::Ink);

    if (!isStaffRole(role))
    {
        UiComponents::panel(ui, UiRect(12, 56, 376, 88), true);
        ui.text(28, 76, "Staff access is required.", UiTheme::Danger);
        ui.wrappedText(28, 96, "Your account does not have moderator or administrator access.",
                       UiTheme::Secondary, 340, 3);
        drawFooterHint("", "B BACK");
        topFrameValid = true;
        return;
    }

    const char *items[] = { "Ticket Queue", "Staff Chat", "Canvas Tools" };
    const int selected = std::max(0, std::min(selectedItem, 2));
    for (int i = 0; i < 3; ++i)
    {
        char meta[20] = "";
        if (i == 0 && ticketNeedsReply > 0)
            snprintf(meta, sizeof(meta), "%d need you", std::min(ticketNeedsReply, 99));
        else if (i == 1 && staffChatUnread > 0)
            snprintf(meta, sizeof(meta), "%d new", std::min(staffChatUnread, 99));
        UiComponents::listRow(ui, UiRect(12, 56 + i * 30, 176, 26),
                              items[i], meta, i == selected);
    }

    UiComponents::panel(ui, UiRect(196, 56, 192, 146), true);
    ui.textClipped(208, 70, items[selected], UiTheme::Accent, 168);
    const char *descriptions[] = {
        "Filter requests, reply, resolve, reject, approve appeals, or reopen a ticket.",
        "Coordinate privately with other moderators and administrators.",
        "Snapshot or change the current canvas with preview and confirmation."
    };
    ui.wrappedText(208, 90, descriptions[selected], UiTheme::Secondary, 168, 5);
    if (selected == 2)
    {
        char current[48];
        snprintf(current, sizeof(current), "Channel: %.24s", canvas.channel[0] ? canvas.channel : "main");
        ui.textClipped(208, 166, current, UiTheme::Ink, 168);
    }
    char roleLabel[20];
    formatTitleLabel(role, roleLabel, sizeof(roleLabel));
    ui.textClipped(208, 184, roleLabel, UiTheme::Secondary, 168);
    drawFooterHint("A OPEN", "B BACK");
    topFrameValid = true;
}

static void composeAdminTopFrame(CanvasState &canvas, bool connected, bool updateAvailable,
                                 const char *role, int selectedAdminItem, const char *adminNotice)
{
    drawTopChrome(connected, updateAvailable);
    UiCanvas ui(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, UI_BUFFER_RGB);
    ui.text(12, 38, "Canvas Tools", UiTheme::Ink);
    ui.text(292, 38, "Role", UiTheme::Secondary);
    char roleLabel[12];
    formatTitleLabel(role, roleLabel, sizeof(roleLabel));
    ui.textClipped(330, 38, roleLabel, UiTheme::Ink, 58);

    bool allowed = isStaffRole(role);
    if (!allowed)
    {
        UiComponents::panel(ui, UiRect(12, 56, 376, 72), true);
        ui.text(28, 76, "Moderator or admin access is required.", UiTheme::Danger);
        drawFooterHint("", "B BACK");
        topFrameValid = true;
        return;
    }

    const char *items[] = {
        "Snapshot",
        "Fill Selection",
        "Erase Selection",
        "Clear Channel",
    };
    const int selected = std::max(0, std::min(selectedAdminItem, 3));
    for (int i = 0; i < 4; i++)
        UiComponents::listRow(ui, UiRect(12, 56 + i * 28, 190, 24),
                              items[i], "", i == selected);

    UiComponents::panel(ui, UiRect(210, 56, 178, 136), true);
    ui.text(222, 70, "Current channel", UiTheme::Secondary);
    ui.textClipped(222, 86, canvas.channel[0] ? canvas.channel : "main", UiTheme::Ink, 154);
    ui.wrappedText(222, 112,
                   selected == 0 ? "Save the current canvas for staff records." :
                   selected == 1 ? "Drag a rectangle, preview it, then press A to fill." :
                   selected == 2 ? "Drag a rectangle, preview it, then press A to erase." :
                                   "Clear the whole channel after a confirmation.",
                   selected == 3 ? UiTheme::Danger : UiTheme::Secondary, 154, 5);
    if (adminNotice && adminNotice[0])
        ui.textClipped(12, 202, adminNotice, UiTheme::Warning, 376);
    drawFooterHint("A USE", "B BACK");
    topFrameValid = true;
}

static void composeStatusTopFrame(CanvasState &canvas, bool connected, bool updateAvailable)
{
    drawTopChrome(connected, updateAvailable);
    UiCanvas ui(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, UI_BUFFER_RGB);
    ui.text(12, 38, connected ? "Connection restored" : "Reconnecting", UiTheme::Ink);
    ui.text(246, 38, "Automatic recovery", UiTheme::Secondary);
    UiComponents::panel(ui, UiRect(12, 56, 376, 124), true);
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 24, 78, "Connection", 73, 82, 92);
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 128, 78, connected ? "Online" : "Offline", connected ? 13 : 196, connected ? 122 : 61, connected ? 117 : 61);
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 24, 100, "Update", 73, 82, 92);
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 128, 100, updateAvailable ? "Available" : "Current", updateAvailable ? 196 : 32, updateAvailable ? 92 : 36, updateAvailable ? 40 : 42);
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 24, 122, "Version", 73, 82, 92);
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 128, 122, APP_BUILD_LABEL, 32, 36, 42);
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 24, 144, "Channel", 73, 82, 92);
    drawUpperText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 128, 144, canvas.channel[0] ? canvas.channel : "main", 32, 36, 42);
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 24, 166, "Zoom", 73, 82, 92);
    drawText(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, 128, 166, canvas.zoomLabel(), 32, 36, 42);
    drawFooterHint("A RETRY", "B / SELECT MENU");
    topFrameValid = true;
}

static void composeIdentityTopFrame(bool connected, bool updateAvailable,
                                    const char *displayName, const char *username,
                                    const char *role, const char *status,
                                    const char *backupCode,
                                    const char *identityNotice,
                                     const char *identityStorage, int restrictionSecondsRemaining,
                                     bool restrictionHasDuration, const char *restrictionReason,
                                     bool backupCodeRevealed, bool needsDisplayName)
{
    drawTopChrome(connected, updateAvailable);
    UiCanvas ui(topFrame, TOP_SCREEN_W, TOP_SCREEN_H, UI_BUFFER_RGB);
    ui.text(12, 38, needsDisplayName ? "Welcome" : "Profile", UiTheme::Ink);

    UiComponents::panel(ui, UiRect(12, 54, 184, 106), true);
    ui.text(24, 66, "Display Name", UiTheme::Secondary);
    ui.textClipped(24, 80, displayName && displayName[0] ? displayName : "Not set", UiTheme::Ink, 160);
    ui.text(24, 100, "Account ID", UiTheme::Secondary);
    ui.textClipped(24, 114, username && username[0] ? username : "Pending", UiTheme::Ink, 160);

    char roleLabel[12], statusLabel[16];
    formatTitleLabel(role, roleLabel, sizeof(roleLabel));
    formatTitleLabel(status, statusLabel, sizeof(statusLabel));
    UiComponents::badge(ui, UiRect(24, 134, 66, 16), roleLabel,
                        isStaffRole(role) ? UiTheme::Accent : UiTheme::Secondary);
    UiComponents::badge(ui, UiRect(96, 134, 76, 16), statusLabel,
                        strcmp(status, "active") == 0 ? UiTheme::Accent :
                        strcmp(status, "banned") == 0 ? UiTheme::Danger : UiTheme::Warning);

    UiComponents::panel(ui, UiRect(204, 54, 184, 106), true);
    ui.text(216, 66, "Recovery Code", UiTheme::Secondary);
    if (backupCode && backupCode[0])
        ui.textClipped(216, 82, backupCodeRevealed ? backupCode : "****-****-****",
                       backupCodeRevealed ? UiTheme::Ink : UiTheme::Secondary, 160);
    else
        ui.text(216, 82, "No code loaded", UiTheme::Warning);
    if (identityStorage && identityStorage[0])
        ui.textClipped(216, 100, identityStorage, UiTheme::Secondary, 160);
    ui.wrappedText(216, 118,
                   "Keep it private. Rotate invalidates the previous code.",
                   UiTheme::Secondary, 160, 3);

    bool restricted = strcmp(status, "muted") == 0 || strcmp(status, "banned") == 0;
    if (restricted)
    {
        char remaining[40];
        if (restrictionHasDuration)
            snprintf(remaining, sizeof(remaining), "%02dh %02dm %02ds left", restrictionSecondsRemaining / 3600, (restrictionSecondsRemaining / 60) % 60, restrictionSecondsRemaining % 60);
        else
            snprintf(remaining, sizeof(remaining), "No automatic expiration");
        ui.textClipped(12, 170, remaining, UiTheme::Warning, 376);
        if (restrictionReason && restrictionReason[0])
            ui.textClipped(12, 184, restrictionReason, UiTheme::Danger, 376);
    }
    if (identityNotice && identityNotice[0])
        ui.textClipped(12, restricted ? 198 : 178, identityNotice, UiTheme::Warning, 376);
    else if (!restricted)
        ui.text(12, 178, "Recovery changes require confirmation.", UiTheme::Secondary);
    if (needsDisplayName)
        drawFooterHint("A CREATE / NAME", "X RECOVER  B EXIT");
    else if (strcmp(status, "banned") == 0)
        drawFooterHint(backupCodeRevealed ? "Y HIDE CODE" : "Y REVEAL CODE",
                       "B BACK - READ ONLY");
    else
        drawFooterHint("A NAME  X RECOVER",
                       backupCodeRevealed ? "Y HIDE R ROTATE B BACK" :
                                            "Y REVEAL R ROTATE B BACK");
    topFrameValid = true;
}

static void presentTopFrameToFramebuffer(u8 *fb, u16 fbWidth, u16 fbHeight)
{
    if (!fb || !topFrameValid)
        return;

    const int screenWidth = fbHeight;
    const int screenHeight = fbWidth;
    const int width = std::min(TOP_SCREEN_W, screenWidth);
    const int height = std::min(TOP_SCREEN_H, screenHeight);

    for (int y = 0; y < height; y++)
    {
        for (int x = 0; x < width; x++)
        {
            int srcIdx = 3 * (y * TOP_SCREEN_W + x);
            int fbX = screenHeight - 1 - y;
            int fbY = x;
            int dstIdx = 3 * (fbY * fbWidth + fbX);
            fb[dstIdx] = topFrame[srcIdx + 2];
            fb[dstIdx + 1] = topFrame[srcIdx + 1];
            fb[dstIdx + 2] = topFrame[srcIdx];
        }
    }
}

void Renderer::renderTop(CanvasState &canvas, bool connected, bool updateAvailable, Color currentColor,
                         int brushSize, int brushShape, TopScreenMode mode,
                         char channels[][25], int channelCount, int selectedChannel,
                         int selectedMenuItem, PresenceUser *users, int userCount,
                         const char *displayName, const char *username,
                         const char *role, const char *status,
                         const char *backupCode, const char *identityNotice,
                         const char *identityStorage, int selectedAdminItem,
                         const char *adminNotice,
                         const char *rulesVersion, bool needsRulesAgreement,
                         SupportTicketSummary *tickets, int ticketCount, int ticketSelected,
                         int ticketView, bool ticketStaffScope, SupportTicketSummary *activeTicket,
                         SupportTicketMessage *ticketMessages, int ticketMessageCount,
                         StaffChatMessage *staffChatMessages, int staffChatMessageCount,
                         int ticketHomeSelected, int ticketActionSelected,
                         bool supportOnly, const char *supportReason,
                         const char *ticketNotice, int ticketNeedsReplyCount,
                         int staffChatUnreadCount, int restrictionSecondsRemaining,
                         bool restrictionHasDuration, const char *restrictionReason,
                         const RendererTopState *topState)
{
    minimapFrameCounter++;
    if (mode == TOP_MODE_CANVAS && (!minimapCacheValid || minimapFrameCounter >= 15))
    {
        updateMinimapCache(canvas);
        minimapFrameCounter = 0;
    }

    if (mode == TOP_MODE_CHANNELS)
        composeChannelTopFrame(canvas, connected, updateAvailable, channels, channelCount, selectedChannel,
                               topState ? topState->channelInfo : NULL,
                               topState ? topState->channelInfoCount : 0);
    else if (mode == TOP_MODE_CONTROLS)
        composeControlsTopFrame(canvas, connected, updateAvailable);
    else if (mode == TOP_MODE_MENU)
        composeMenuTopFrame(canvas, connected, updateAvailable, selectedMenuItem, ticketNeedsReplyCount, staffChatUnreadCount,
                            role && (strcmp(role, "mod") == 0 || strcmp(role, "admin") == 0));
    else if (mode == TOP_MODE_USERS)
        composeUsersTopFrame(canvas, connected, updateAvailable, users, userCount,
                             displayName, username, role,
                             topState ? topState->peopleSelected : 0,
                             topState ? topState->peopleAllChannels : false,
                             topState ? topState->presenceTotal : userCount,
                             topState ? topState->presenceTruncated : false);
    else if (mode == TOP_MODE_RULES)
        composeRulesTopFrame(connected, updateAvailable, rulesVersion, needsRulesAgreement, identityNotice,
                             topState);
    else if (mode == TOP_MODE_TICKETS)
        composeTicketsTopFrame(connected, updateAvailable, tickets, ticketCount, ticketSelected,
                               ticketView, ticketStaffScope, activeTicket, ticketMessages, ticketMessageCount,
                               ticketHomeSelected, ticketActionSelected, supportOnly, supportReason,
                               ticketNotice, ticketNeedsReplyCount, staffChatMessages, staffChatMessageCount,
                               staffChatUnreadCount, restrictionSecondsRemaining, restrictionHasDuration);
    else if (mode == TOP_MODE_ADMIN)
        composeAdminTopFrame(canvas, connected, updateAvailable, role, selectedAdminItem, adminNotice);
    else if (mode == TOP_MODE_OPTIONS)
        composeOptionsTopFrame(canvas, connected, updateAvailable, currentColor, brushSize, brushShape, topState);
    else if (mode == TOP_MODE_STAFF_CENTER)
        composeStaffCenterTopFrame(canvas, connected, updateAvailable, role,
                                   topState ? topState->pageSelected : selectedAdminItem,
                                   ticketNeedsReplyCount, staffChatUnreadCount);
    else if (mode == TOP_MODE_STATUS)
        composeStatusTopFrame(canvas, connected, updateAvailable);
    else if (mode == TOP_MODE_IDENTITY)
        composeIdentityTopFrame(connected, updateAvailable, displayName, username, role, status, backupCode,
                                 identityNotice, identityStorage, restrictionSecondsRemaining, restrictionHasDuration,
                                 restrictionReason,
                                 topState ? topState->backupCodeRevealed : false,
                                 topState ? topState->needsDisplayName : false);
    else
        composeCanvasTopFrame(canvas, connected, updateAvailable, currentColor, brushSize, brushShape,
                               ticketNeedsReplyCount, staffChatUnreadCount, users, userCount,
                               topState ? topState->channelInfo : NULL,
                               topState ? topState->channelInfoCount : 0);
}

void Renderer::presentTopFrame()
{
    u16 leftWidth, leftHeight;
    u16 rightWidth, rightHeight;
    u8 *left = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, &leftWidth, &leftHeight);
    u8 *right = gfxGetFramebuffer(GFX_TOP, GFX_RIGHT, &rightWidth, &rightHeight);

    presentTopFrameToFramebuffer(left, leftWidth, leftHeight);
    presentTopFrameToFramebuffer(right, rightWidth, rightHeight);
}
