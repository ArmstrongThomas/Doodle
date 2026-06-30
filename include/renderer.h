#ifndef RENDERER_H
#define RENDERER_H

#include <3ds.h>
#include "canvas_state.h"
#include "protocol.h"
#include "ui.h"

enum TopScreenMode {
    TOP_MODE_CANVAS = 0,
    TOP_MODE_CHANNELS = 1,
    TOP_MODE_CONTROLS = 2,
    TOP_MODE_MENU = 3,
    TOP_MODE_USERS = 4,
    TOP_MODE_ADMIN = 5,
    TOP_MODE_STATUS = 6,
    TOP_MODE_IDENTITY = 7,
    TOP_MODE_RULES = 8,
    TOP_MODE_CHAT = 9
};

class Renderer {
public:
    static void renderViewport(CanvasState &canvas, u8 *buffer, int fbWidth, int fbHeight, bool forceFull);
    static void renderTop(CanvasState &canvas, bool connected, bool updateAvailable, Color currentColor,
                          int brushSize, int brushShape, TopScreenMode mode,
                          char channels[][25], int channelCount, int selectedChannel,
                          int selectedMenuItem = 0, PresenceUser *users = NULL, int userCount = 0,
                          const char *displayName = "3DS User", const char *username = "unknown",
                          const char *role = "user", const char *status = "active",
                          const char *backupCode = "", const char *identityNotice = "",
                          const char *identityStorage = "", int selectedAdminItem = 0,
                          const char *adminNotice = "",
                          const char *rulesVersion = "", bool needsRulesAgreement = false,
                          ChatLine *chatLines = NULL,
                          int chatCount = 0, int chatScroll = 0, int chatSelected = 0, int chatUnread = 0,
                          const char *chatNotice = "");
    static void presentTopFrame();
    static void invalidateMinimap();
};

#endif
