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
    static bool parseUpdateRequired(const char *line, char *latestVersion, size_t latestVersionSize,
                                    char *reason, size_t reasonSize);
    static void buildHello(char *buffer, size_t size, const char *appId, const char *version, bool updaterSupported,
                           const char *deviceId, const char *deviceSecret, const char *displayName, const char *packageType);
    static void buildSwitchChannel(char *buffer, size_t size, const char *channel);
    static void buildSetDisplayName(char *buffer, size_t size, const char *displayName);
    static void buildRulesAccepted(char *buffer, size_t size, const char *version);
    static void buildRecoverIdentity(char *buffer, size_t size, const char *username, const char *backupCode,
                                     const char *deviceId, const char *deviceSecret);
    static void buildRotateBackupCode(char *buffer, size_t size);
    static void buildModerationCommand(char *buffer, size_t size, const char *action, const char *identityId,
                                       int messageId, const char *reason);
    static void buildAdminCanvasCommand(char *buffer, size_t size, const char *action, const char *channel,
                                        int x, int y, int width, int height, int r, int g, int b);
    static void buildUpdateRequest(char *buffer, size_t size);
};

#endif
