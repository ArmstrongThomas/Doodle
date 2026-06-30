#include "protocol.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int jsonInt(const char *line, const char *key)
{
    const char *ptr = strstr(line, key);
    if (!ptr)
        return 0;
    return atoi(ptr + strlen(key));
}

static void jsonString(const char *line, const char *key, char *out, size_t outSize)
{
    out[0] = '\0';
    const char *ptr = strstr(line, key);
    if (!ptr)
        return;
    ptr += strlen(key);
    const char *end = strchr(ptr, '"');
    if (!end)
        return;
    size_t len = end - ptr;
    if (len >= outSize)
        len = outSize - 1;
    memcpy(out, ptr, len);
    out[len] = '\0';
}

static void jsonSafeString(const char *input, char *out, size_t outSize)
{
    size_t j = 0;
    if (!out || outSize == 0)
        return;
    for (size_t i = 0; input && input[i] && j + 1 < outSize; i++)
    {
        char c = input[i];
        if (c == '"' || c == '\\')
            continue;
        if ((unsigned char)c < 32)
            continue;
        out[j++] = c;
    }
    out[j] = '\0';
}

bool Protocol::parseCanvasMeta(const char *line, CanvasMeta &meta)
{
    if (!line || !strstr(line, "compressedCanvas"))
        return false;
    meta.width = jsonInt(line, "\"width\":");
    meta.height = jsonInt(line, "\"height\":");
    meta.compressedSize = jsonInt(line, "\"compressedSize\":");
    jsonString(line, "\"channel\":\"", meta.channel, sizeof(meta.channel));
    if (!meta.channel[0])
        strcpy(meta.channel, "main");
    return meta.width > 0 && meta.height > 0 && meta.compressedSize > 0;
}

bool Protocol::parseChannels(const char *line, char channels[][25], int maxChannels, int &count, char *currentChannel)
{
    if (!line || !strstr(line, "\"type\":\"channels\""))
        return false;

    count = 0;
    jsonString(line, "\"currentChannel\":\"", currentChannel, 25);

    const char *ptr = strstr(line, "\"channels\":[");
    if (!ptr)
        return true;
    ptr += strlen("\"channels\":[");
    while (*ptr && *ptr != ']' && count < maxChannels)
    {
        while (*ptr && *ptr != ']' && *ptr != '"')
            ptr++;
        if (!*ptr || *ptr == ']')
            break;
        ptr++;
        const char *end = strchr(ptr, '"');
        if (!end)
            break;
        size_t len = end - ptr;
        if (len > 24)
            len = 24;
        memcpy(channels[count], ptr, len);
        channels[count][len] = '\0';
        count++;
        ptr = end + 1;
    }
    return true;
}

bool Protocol::parseUpdateRequired(const char *line, char *latestVersion, size_t latestVersionSize,
                                   char *reason, size_t reasonSize)
{
    if (!line || !strstr(line, "\"type\":\"updateRequired\""))
        return false;
    jsonString(line, "\"latestVersion\":\"", latestVersion, latestVersionSize);
    jsonString(line, "\"reason\":\"", reason, reasonSize);
    return true;
}

bool Protocol::parsePresence(const char *line, PresenceUser *users, int maxUsers, int &count)
{
    if (!line || !strstr(line, "\"type\":\"presence\""))
        return false;

    count = 0;
    const char *ptr = line;
    while (count < maxUsers)
    {
        ptr = strstr(ptr, "\"id\":\"");
        if (!ptr)
            break;
        memset(&users[count], 0, sizeof(users[count]));
        jsonString(ptr, "\"identityId\":\"", users[count].identityId, sizeof(users[count].identityId));
        jsonString(ptr, "\"username\":\"", users[count].username, sizeof(users[count].username));
        jsonString(ptr, "\"displayName\":\"", users[count].displayName, sizeof(users[count].displayName));
        jsonString(ptr, "\"role\":\"", users[count].role, sizeof(users[count].role));
        jsonString(ptr, "\"status\":\"", users[count].status, sizeof(users[count].status));
        users[count].muteSecondsRemaining = jsonInt(ptr, "\"muteSecondsRemaining\":");
        users[count].banSecondsRemaining = jsonInt(ptr, "\"banSecondsRemaining\":");
        if (!users[count].displayName[0])
            snprintf(users[count].displayName, sizeof(users[count].displayName), "USER");
        if (!users[count].role[0])
            snprintf(users[count].role, sizeof(users[count].role), "user");
        if (!users[count].status[0])
            snprintf(users[count].status, sizeof(users[count].status), "active");
        count++;
        ptr += strlen("\"id\":\"");
    }
    return true;
}

bool Protocol::parseIdentityAccepted(const char *line, IdentityInfo &identity)
{
    if (!line || !strstr(line, "\"type\":\"identityAccepted\""))
        return false;
    jsonString(line, "\"username\":\"", identity.username, sizeof(identity.username));
    jsonString(line, "\"displayName\":\"", identity.displayName, sizeof(identity.displayName));
    jsonString(line, "\"role\":\"", identity.role, sizeof(identity.role));
    jsonString(line, "\"status\":\"", identity.status, sizeof(identity.status));
    identity.muteSecondsRemaining = jsonInt(line, "\"muteSecondsRemaining\":");
    identity.banSecondsRemaining = jsonInt(line, "\"banSecondsRemaining\":");
    if (!identity.displayName[0])
        strcpy(identity.displayName, "3DS User");
    if (!identity.username[0])
        strcpy(identity.username, "unknown");
    if (!identity.role[0])
        strcpy(identity.role, "user");
    if (!identity.status[0])
        strcpy(identity.status, "active");
    return true;
}

bool Protocol::parseIdentityBackupCode(const char *line, IdentityInfo &identity)
{
    if (!line || !strstr(line, "\"type\":\"identityBackupCode\""))
        return false;
    jsonString(line, "\"username\":\"", identity.username, sizeof(identity.username));
    jsonString(line, "\"backupCode\":\"", identity.backupCode, sizeof(identity.backupCode));
    return identity.backupCode[0] != '\0';
}

bool Protocol::parseRecoveryFailed(const char *line, char *reason, size_t reasonSize)
{
    if (!line || !strstr(line, "\"type\":\"recoveryFailed\""))
        return false;
    jsonString(line, "\"reason\":\"", reason, reasonSize);
    if (!reason[0] && reasonSize > 0)
        snprintf(reason, reasonSize, "failed");
    return true;
}

bool Protocol::parseRulesRequired(const char *line, char *version, size_t versionSize)
{
    if (!line || !strstr(line, "\"type\":\"rulesRequired\""))
        return false;
    jsonString(line, "\"version\":\"", version, versionSize);
    if (version && versionSize > 0 && !version[0])
        snprintf(version, versionSize, "1");
    return true;
}

bool Protocol::parseDisplayNameRejected(const char *line, char *reason, size_t reasonSize)
{
    if (!line || !strstr(line, "\"type\":\"displayNameRejected\""))
        return false;
    jsonString(line, "\"reason\":\"", reason, reasonSize);
    if (reason && reasonSize > 0 && !reason[0])
        snprintf(reason, reasonSize, "invalid-name");
    return true;
}

bool Protocol::parseDisconnected(const char *line, char *reason, size_t reasonSize)
{
    if (!line || (!strstr(line, "\"type\":\"disconnected\"") && !strstr(line, "\"type\":\"banned\"") && !strstr(line, "\"type\":\"serverRestarting\"")))
        return false;
    jsonString(line, "\"reason\":\"", reason, reasonSize);
    if (reason && reasonSize > 0 && !reason[0])
        snprintf(reason, reasonSize, strstr(line, "\"type\":\"banned\"") ? "banned" : "disconnected");
    return true;
}

void Protocol::buildHello(char *buffer, size_t size, const char *appId, const char *version, bool updaterSupported,
                          const char *deviceId, const char *deviceSecret, const char *displayName, const char *packageType)
{
    snprintf(buffer, size,
             "{\"type\":\"hello\",\"appId\":\"%s\",\"version\":\"%s\",\"protocol\":4,\"updaterSupported\":%s,"
             "\"packageType\":\"%s\",\"deviceId\":\"%s\",\"deviceSecret\":\"%s\",\"displayName\":\"%s\"}\n",
             appId, version, updaterSupported ? "true" : "false",
             packageType ? packageType : "3dsx",
             deviceId ? deviceId : "", deviceSecret ? deviceSecret : "", displayName ? displayName : "3DS User");
}

void Protocol::buildSwitchChannel(char *buffer, size_t size, const char *channel)
{
    snprintf(buffer, size, "{\"type\":\"switchChannel\",\"channel\":\"%s\"}\n", channel);
}

void Protocol::buildSetDisplayName(char *buffer, size_t size, const char *displayName)
{
    char safeName[25];
    jsonSafeString(displayName, safeName, sizeof(safeName));
    snprintf(buffer, size, "{\"type\":\"setDisplayName\",\"displayName\":\"%s\"}\n", safeName);
}

void Protocol::buildRulesAccepted(char *buffer, size_t size, const char *version)
{
    char safeVersion[32];
    jsonSafeString(version, safeVersion, sizeof(safeVersion));
    snprintf(buffer, size, "{\"type\":\"rulesAccepted\",\"version\":\"%s\"}\n", safeVersion[0] ? safeVersion : "1");
}

void Protocol::buildRecoverIdentity(char *buffer, size_t size, const char *username, const char *backupCode,
                                    const char *deviceId, const char *deviceSecret)
{
    char safeUsername[25];
    char safeBackup[32];
    char safeDeviceId[48];
    char safeSecret[64];
    jsonSafeString(username, safeUsername, sizeof(safeUsername));
    jsonSafeString(backupCode, safeBackup, sizeof(safeBackup));
    jsonSafeString(deviceId, safeDeviceId, sizeof(safeDeviceId));
    jsonSafeString(deviceSecret, safeSecret, sizeof(safeSecret));
    snprintf(buffer, size,
             "{\"type\":\"recoverIdentity\",\"username\":\"%s\",\"backupCode\":\"%s\","
             "\"deviceId\":\"%s\",\"deviceSecret\":\"%s\"}\n",
             safeUsername, safeBackup, safeDeviceId, safeSecret);
}

void Protocol::buildRotateBackupCode(char *buffer, size_t size)
{
    snprintf(buffer, size, "{\"type\":\"rotateBackupCode\"}\n");
}

void Protocol::buildModerationCommand(char *buffer, size_t size, const char *action, const char *identityId,
                                      int messageId, const char *reason)
{
    char safeAction[24];
    char safeIdentity[48];
    char safeReason[80];
    jsonSafeString(action, safeAction, sizeof(safeAction));
    jsonSafeString(identityId, safeIdentity, sizeof(safeIdentity));
    jsonSafeString(reason, safeReason, sizeof(safeReason));
    snprintf(buffer, size,
             "{\"type\":\"moderation\",\"action\":\"%s\",\"identityId\":\"%s\",\"messageId\":%d,\"reason\":\"%s\"}\n",
             safeAction, safeIdentity, messageId, safeReason);
}

void Protocol::buildAdminCanvasCommand(char *buffer, size_t size, const char *action, const char *channel,
                                       int x, int y, int width, int height, int r, int g, int b)
{
    char safeAction[20];
    char safeChannel[25];
    jsonSafeString(action, safeAction, sizeof(safeAction));
    jsonSafeString(channel, safeChannel, sizeof(safeChannel));
    snprintf(buffer, size,
             "{\"type\":\"adminCanvas\",\"action\":\"%s\",\"channel\":\"%s\","
             "\"rect\":{\"x\":%d,\"y\":%d,\"width\":%d,\"height\":%d},"
             "\"color\":[%d,%d,%d]}\n",
             safeAction, safeChannel, x, y, width, height, r, g, b);
}

void Protocol::buildUpdateRequest(char *buffer, size_t size)
{
    snprintf(buffer, size, "GET /api/updates/latest HTTP/1.0\r\nHost: %s\r\n\r\n", SERVER_HTTP_HOST);
}
