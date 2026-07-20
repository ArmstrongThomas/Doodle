#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <3ds.h>
#include <stddef.h>

struct CanvasMeta {
    int width;
    int height;
    int compressedSize;
    char channel[25];
};

struct IdentityInfo {
    char username[25];
    char displayName[25];
    char role[12];
    char status[12];
    int muteSecondsRemaining;
    int banSecondsRemaining;
    char restrictionReason[81];
    char backupCode[32];
};

struct ChatLine {
    int id;
    bool deleted;
    char identityId[40];
    char timestamp[25];
    char username[25];
    char displayName[25];
    char role[12];
    char message[161];
};

struct SupportTicketSummary {
    int id;
    int messageCount;
    char category[12];
    char status[20];
    char subject[65];
    char identityId[40];
    char username[25];
    char displayName[25];
    char banReason[81];
    char blockTypes[24];
    char createdAt[25];
    char updatedAt[25];
    char lastMessage[121];
};

struct SupportTicketMessage {
    int id;
    int ticketId;
    char authorKind[12];
    char displayName[25];
    char role[12];
    char createdAt[25];
    char message[241];
};

struct StaffChatMessage {
    int id;
    char identityId[40];
    char username[25];
    char displayName[25];
    char role[12];
    char createdAt[25];
    char message[241];
};

struct PresenceUser {
    char identityId[40];
    char username[25];
    char displayName[25];
    char role[12];
    char status[12];
    int muteSecondsRemaining;
    int banSecondsRemaining;
};

class Protocol {
public:
    static bool parseCanvasMeta(const char *line, CanvasMeta &meta);
    static bool parseChannels(const char *line, char channels[][25], int maxChannels, int &count, char *currentChannel);
    static bool parsePresence(const char *line, PresenceUser *users, int maxUsers, int &count);
    static bool parseIdentityAccepted(const char *line, IdentityInfo &identity);
    static bool parseIdentityBackupCode(const char *line, IdentityInfo &identity);
    static bool parseRecoveryFailed(const char *line, char *reason, size_t reasonSize);
    static bool parseRulesRequired(const char *line, char *version, size_t versionSize);
    static bool parseDisplayNameRejected(const char *line, char *reason, size_t reasonSize);
    static bool parseDisconnected(const char *line, char *reason, size_t reasonSize);
    static bool parseSupportOnly(const char *line, char *reason, size_t reasonSize, char *blockTypes, size_t blockTypesSize, int &secondsRemaining);
    static bool parseMuted(const char *line, char *reason, size_t reasonSize, int &secondsRemaining);
    static bool parseTicketSummary(const char *line, SupportTicketSummary &ticket);
    static bool parseTicketMessage(const char *line, SupportTicketMessage &message);
    static bool parseTicketResult(const char *line, bool &ok, char *action, size_t actionSize,
                                  char *error, size_t errorSize, int &ticketId);
    static bool parseTicketListStart(const char *line, char *scope, size_t scopeSize, int &count);
    static bool parseTicketListEnd(const char *line, int &nextBeforeId);
    static bool parseTicketThreadEnd(const char *line, int &ticketId, int &nextBeforeMessageId);
    static bool parseTicketCounts(const char *line, int &mineOpen, int &staffNeedsReply, int &staffChatUnread);
    static bool parseStaffChatMessage(const char *line, StaffChatMessage &message);
    static bool parseStaffChatStart(const char *line, int &count);
    static bool parseStaffChatEnd(const char *line, int &nextBeforeId);
    static bool parseStaffChatResult(const char *line, bool &ok, char *error, size_t errorSize);
    static bool parseUpdateRequired(const char *line, char *latestVersion, size_t latestVersionSize,
                                    char *reason, size_t reasonSize);
    static void buildHello(char *buffer, size_t size, const char *appId, const char *version, bool updaterSupported,
                           const char *deviceId, const char *deviceSecret, const char *hardwareId,
                           const char *displayName, const char *packageType);
    static void buildSwitchChannel(char *buffer, size_t size, const char *channel);
    static void buildSetDisplayName(char *buffer, size_t size, const char *displayName);
    static void buildRulesAccepted(char *buffer, size_t size, const char *version);
    static void buildRecoverIdentity(char *buffer, size_t size, const char *username, const char *backupCode,
                                     const char *deviceId, const char *deviceSecret, const char *hardwareId);
    static void buildRotateBackupCode(char *buffer, size_t size);
    static void buildModerationCommand(char *buffer, size_t size, const char *action, const char *identityId,
                                       int messageId, const char *reason);
    static void buildAdminCanvasCommand(char *buffer, size_t size, const char *action, const char *channel,
                                        int x, int y, int width, int height, int r, int g, int b);
    static void buildTicketCreate(char *buffer, size_t size, const char *category, const char *subject, const char *message);
    static void buildTicketList(char *buffer, size_t size, bool staff, const char *status, const char *category, int beforeId);
    static void buildTicketGet(char *buffer, size_t size, int ticketId, int beforeMessageId = 0);
    static void buildTicketReply(char *buffer, size_t size, int ticketId, const char *message, bool staff = false);
    static void buildTicketStatus(char *buffer, size_t size, int ticketId, const char *status, const char *message = "");
    static void buildTicketApproveUnban(char *buffer, size_t size, int ticketId);
    static void buildTicketCounts(char *buffer, size_t size);
    static void buildStaffChatList(char *buffer, size_t size, int beforeId = 0);
    static void buildStaffChatSend(char *buffer, size_t size, const char *message);
    static void buildStaffChatRead(char *buffer, size_t size, int messageId);
    static void buildUpdateRequest(char *buffer, size_t size);
};

#endif
