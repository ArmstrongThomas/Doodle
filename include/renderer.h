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
    TOP_MODE_TICKETS = 9,
    TOP_MODE_OPTIONS = 10,
    TOP_MODE_STAFF_CENTER = 11
};

// Optional route-specific state for the 1.6 top-screen views. Keeping this
// behind a single pointer lets older call sites continue to render unchanged
// while the route/input layer is migrated.
struct RendererTopState {
    int peopleSelected;
    bool peopleAllChannels;
    int presenceTotal;
    bool presenceTruncated;
    ChannelInfo *channelInfo;
    int channelInfoCount;
    bool backupCodeRevealed;
    bool needsDisplayName;
    int pageSelected;
    const char *controlPreset;
    const char *controlBindings[6];
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
                          SupportTicketSummary *tickets = NULL, int ticketCount = 0, int ticketSelected = 0,
                          int ticketView = 0, bool ticketStaffScope = false,
                          SupportTicketSummary *activeTicket = NULL,
                          SupportTicketMessage *ticketMessages = NULL, int ticketMessageCount = 0,
                          StaffChatMessage *staffChatMessages = NULL, int staffChatMessageCount = 0,
                          int ticketHomeSelected = 0, int ticketActionSelected = 0,
                          bool supportOnly = false, const char *supportReason = "",
                          const char *ticketNotice = "", int ticketNeedsReplyCount = 0,
                          int staffChatUnreadCount = 0, int restrictionSecondsRemaining = 0,
                          bool restrictionHasDuration = false, const char *restrictionReason = "",
                          const RendererTopState *topState = NULL);
    static void presentTopFrame();
    static void invalidateMinimap();
};

#endif
