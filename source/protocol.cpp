#include "protocol.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <algorithm>

static int jsonInt(const char *line, const char *key)
{
    const char *ptr = strstr(line, key);
    if (!ptr)
        return 0;
    return atoi(ptr + strlen(key));
}

static void jsonString(const char *line, const char *key, char *out, size_t outSize)
{
    if (!out || outSize == 0)
        return;
    out[0] = '\0';
    const char *ptr = strstr(line, key);
    if (!ptr)
        return;
    ptr += strlen(key);
    size_t used = 0;
    while (*ptr && *ptr != '"')
    {
        char value = *ptr++;
        if (value == '\\' && *ptr)
        {
            char escaped = *ptr++;
            if (escaped == 'n' || escaped == 'r' || escaped == 't')
                value = ' ';
            else
                value = escaped;
        }
        if (used + 1 < outSize)
            out[used++] = value;
    }
    out[used] = '\0';
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
    jsonString(line, "\"banReason\":\"", identity.restrictionReason, sizeof(identity.restrictionReason));
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

bool Protocol::parseSupportOnly(const char *line, char *reason, size_t reasonSize, char *blockTypes, size_t blockTypesSize, int &secondsRemaining)
{
    if (!line || !strstr(line, "\"type\":\"supportOnly\""))
        return false;
    jsonString(line, "\"reason\":\"", reason, reasonSize);
    secondsRemaining = jsonInt(line, "\"secondsRemaining\":");
    if (blockTypes && blockTypesSize > 0)
    {
        blockTypes[0] = '\0';
        const char *types = strstr(line, "\"blockTypes\":[");
        if (types)
        {
            bool hasIp = strstr(types, "\"ip\"") != NULL;
            bool hasDevice = strstr(types, "\"device\"") != NULL;
            snprintf(blockTypes, blockTypesSize, "%s%s%s", hasIp ? "IP" : "", hasIp && hasDevice ? " + " : "", hasDevice ? "DEVICE" : "");
        }
    }
    return true;
}

bool Protocol::parseMuted(const char *line, char *reason, size_t reasonSize, int &secondsRemaining)
{
    if (!line || !strstr(line, "\"type\":\"muted\""))
        return false;
    jsonString(line, "\"reason\":\"", reason, reasonSize);
    secondsRemaining = jsonInt(line, "\"secondsRemaining\":");
    return true;
}

bool Protocol::parseTicketSummary(const char *line, SupportTicketSummary &ticket)
{
    if (!line || (!strstr(line, "\"type\":\"ticketSummary\"") &&
                  !strstr(line, "\"type\":\"ticketThreadStart\"") &&
                  !strstr(line, "\"type\":\"ticketUpdated\"")))
        return false;
    memset(&ticket, 0, sizeof(ticket));
    ticket.id = jsonInt(line, "\"id\":");
    ticket.messageCount = jsonInt(line, "\"messageCount\":");
    jsonString(line, "\"category\":\"", ticket.category, sizeof(ticket.category));
    jsonString(line, "\"status\":\"", ticket.status, sizeof(ticket.status));
    jsonString(line, "\"subject\":\"", ticket.subject, sizeof(ticket.subject));
    jsonString(line, "\"identityId\":\"", ticket.identityId, sizeof(ticket.identityId));
    jsonString(line, "\"username\":\"", ticket.username, sizeof(ticket.username));
    jsonString(line, "\"displayName\":\"", ticket.displayName, sizeof(ticket.displayName));
    jsonString(line, "\"banReason\":\"", ticket.banReason, sizeof(ticket.banReason));
    jsonString(line, "\"createdAt\":\"", ticket.createdAt, sizeof(ticket.createdAt));
    jsonString(line, "\"updatedAt\":\"", ticket.updatedAt, sizeof(ticket.updatedAt));
    jsonString(line, "\"lastMessage\":\"", ticket.lastMessage, sizeof(ticket.lastMessage));
    const char *types = strstr(line, "\"blockTypes\":[");
    if (types)
    {
        bool hasIp = strstr(types, "\"ip\"") != NULL;
        bool hasDevice = strstr(types, "\"device\"") != NULL;
        snprintf(ticket.blockTypes, sizeof(ticket.blockTypes), "%s%s%s", hasIp ? "IP" : "", hasIp && hasDevice ? " + " : "", hasDevice ? "DEVICE" : "");
    }
    return ticket.id > 0;
}

bool Protocol::parseTicketMessage(const char *line, SupportTicketMessage &message)
{
    if (!line || !strstr(line, "\"type\":\"ticketMessage\""))
        return false;
    memset(&message, 0, sizeof(message));
    message.id = jsonInt(line, "\"id\":");
    message.ticketId = jsonInt(line, "\"ticketId\":");
    jsonString(line, "\"authorKind\":\"", message.authorKind, sizeof(message.authorKind));
    jsonString(line, "\"displayName\":\"", message.displayName, sizeof(message.displayName));
    jsonString(line, "\"role\":\"", message.role, sizeof(message.role));
    jsonString(line, "\"createdAt\":\"", message.createdAt, sizeof(message.createdAt));
    jsonString(line, "\"message\":\"", message.message, sizeof(message.message));
    return message.id > 0;
}

bool Protocol::parseTicketResult(const char *line, bool &ok, char *action, size_t actionSize,
                                 char *error, size_t errorSize, int &ticketId)
{
    if (!line || !strstr(line, "\"type\":\"ticketResult\""))
        return false;
    ok = strstr(line, "\"ok\":true") != NULL;
    jsonString(line, "\"action\":\"", action, actionSize);
    jsonString(line, "\"error\":\"", error, errorSize);
    ticketId = jsonInt(line, "\"ticketId\":");
    return true;
}

bool Protocol::parseTicketListStart(const char *line, char *scope, size_t scopeSize, int &count)
{
    if (!line || !strstr(line, "\"type\":\"ticketListStart\""))
        return false;
    jsonString(line, "\"scope\":\"", scope, scopeSize);
    count = jsonInt(line, "\"count\":");
    return true;
}

bool Protocol::parseTicketListEnd(const char *line, int &nextBeforeId)
{
    if (!line || !strstr(line, "\"type\":\"ticketListEnd\""))
        return false;
    nextBeforeId = jsonInt(line, "\"nextBeforeId\":");
    return true;
}

bool Protocol::parseTicketThreadEnd(const char *line, int &ticketId, int &nextBeforeMessageId)
{
    if (!line || !strstr(line, "\"type\":\"ticketThreadEnd\""))
        return false;
    ticketId = jsonInt(line, "\"ticketId\":");
    nextBeforeMessageId = jsonInt(line, "\"nextBeforeMessageId\":");
    return true;
}

bool Protocol::parseTicketCounts(const char *line, int &mineOpen, int &staffNeedsReply, int &staffChatUnread)
{
    if (!line || !strstr(line, "\"type\":\"ticketCounts\""))
        return false;
    mineOpen = 0;
    staffNeedsReply = 0;
    staffChatUnread = 0;
    const char *mine = strstr(line, "\"mine\":{");
    const char *staff = strstr(line, "\"staff\":{");
    if (mine)
        mineOpen = jsonInt(mine, "\"unresolved\":");
    if (staff)
    {
        staffNeedsReply = jsonInt(staff, "\"needsStaffReply\":");
        staffChatUnread = jsonInt(staff, "\"staffChatUnread\":");
    }
    return true;
}

bool Protocol::parseStaffChatMessage(const char *line, StaffChatMessage &message)
{
    if (!line || !strstr(line, "\"type\":\"staffChatMessage\""))
        return false;
    memset(&message, 0, sizeof(message));
    message.id = jsonInt(line, "\"id\":");
    jsonString(line, "\"identityId\":\"", message.identityId, sizeof(message.identityId));
    jsonString(line, "\"username\":\"", message.username, sizeof(message.username));
    jsonString(line, "\"displayName\":\"", message.displayName, sizeof(message.displayName));
    jsonString(line, "\"role\":\"", message.role, sizeof(message.role));
    jsonString(line, "\"createdAt\":\"", message.createdAt, sizeof(message.createdAt));
    jsonString(line, "\"message\":\"", message.message, sizeof(message.message));
    return message.id > 0;
}

bool Protocol::parseStaffChatStart(const char *line, int &count)
{
    if (!line || !strstr(line, "\"type\":\"staffChatStart\"")) return false;
    count = jsonInt(line, "\"count\":");
    return true;
}

bool Protocol::parseStaffChatEnd(const char *line, int &nextBeforeId)
{
    if (!line || !strstr(line, "\"type\":\"staffChatEnd\"")) return false;
    nextBeforeId = jsonInt(line, "\"nextBeforeId\":");
    return true;
}

bool Protocol::parseStaffChatResult(const char *line, bool &ok, char *error, size_t errorSize)
{
    if (!line || !strstr(line, "\"type\":\"staffChatResult\"")) return false;
    ok = strstr(line, "\"ok\":true") != NULL;
    jsonString(line, "\"error\":\"", error, errorSize);
    return true;
}

void Protocol::buildHello(char *buffer, size_t size, const char *appId, const char *version, bool updaterSupported,
                          const char *deviceId, const char *deviceSecret, const char *hardwareId,
                          const char *displayName, const char *packageType)
{
    snprintf(buffer, size,
             "{\"type\":\"hello\",\"appId\":\"%s\",\"version\":\"%s\",\"protocol\":5,\"updaterSupported\":%s,"
             "\"packageType\":\"%s\",\"deviceId\":\"%s\",\"deviceSecret\":\"%s\",\"hardwareId\":\"%s\",\"displayName\":\"%s\"}\n",
             appId, version, updaterSupported ? "true" : "false",
             packageType ? packageType : "3dsx",
             deviceId ? deviceId : "", deviceSecret ? deviceSecret : "", hardwareId ? hardwareId : "",
             displayName ? displayName : "3DS User");
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
                                    const char *deviceId, const char *deviceSecret, const char *hardwareId)
{
    char safeUsername[25];
    char safeBackup[32];
    char safeDeviceId[48];
    char safeSecret[64];
    char safeHardware[64];
    jsonSafeString(username, safeUsername, sizeof(safeUsername));
    jsonSafeString(backupCode, safeBackup, sizeof(safeBackup));
    jsonSafeString(deviceId, safeDeviceId, sizeof(safeDeviceId));
    jsonSafeString(deviceSecret, safeSecret, sizeof(safeSecret));
    jsonSafeString(hardwareId, safeHardware, sizeof(safeHardware));
    snprintf(buffer, size,
             "{\"type\":\"recoverIdentity\",\"username\":\"%s\",\"backupCode\":\"%s\","
             "\"deviceId\":\"%s\",\"deviceSecret\":\"%s\",\"hardwareId\":\"%s\"}\n",
             safeUsername, safeBackup, safeDeviceId, safeSecret, safeHardware);
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

void Protocol::buildTicketCreate(char *buffer, size_t size, const char *category, const char *subject, const char *message)
{
    char safeCategory[12];
    char safeSubject[65];
    char safeMessage[241];
    jsonSafeString(category, safeCategory, sizeof(safeCategory));
    jsonSafeString(subject, safeSubject, sizeof(safeSubject));
    jsonSafeString(message, safeMessage, sizeof(safeMessage));
    snprintf(buffer, size, "{\"type\":\"ticketCreate\",\"category\":\"%s\",\"subject\":\"%s\",\"message\":\"%s\"}\n",
             safeCategory, safeSubject, safeMessage);
}

void Protocol::buildTicketList(char *buffer, size_t size, bool staff, const char *status, const char *category, int beforeId)
{
    char safeStatus[20];
    char safeCategory[12];
    jsonSafeString(status, safeStatus, sizeof(safeStatus));
    jsonSafeString(category, safeCategory, sizeof(safeCategory));
    snprintf(buffer, size,
             "{\"type\":\"ticketList\",\"scope\":\"%s\",\"status\":\"%s\",\"category\":\"%s\",\"beforeId\":%d,\"limit\":6}\n",
             staff ? "staff" : "mine", safeStatus, safeCategory, std::max(0, beforeId));
}

void Protocol::buildTicketGet(char *buffer, size_t size, int ticketId, int beforeMessageId)
{
    snprintf(buffer, size, "{\"type\":\"ticketGet\",\"ticketId\":%d,\"beforeMessageId\":%d,\"limit\":6}\n",
             std::max(0, ticketId), std::max(0, beforeMessageId));
}

void Protocol::buildTicketReply(char *buffer, size_t size, int ticketId, const char *message, bool staff)
{
    char safeMessage[241];
    jsonSafeString(message, safeMessage, sizeof(safeMessage));
    snprintf(buffer, size, "{\"type\":\"ticketReply\",\"ticketId\":%d,\"staff\":%s,\"message\":\"%s\"}\n",
             std::max(0, ticketId), staff ? "true" : "false", safeMessage);
}

void Protocol::buildTicketStatus(char *buffer, size_t size, int ticketId, const char *status, const char *message)
{
    char safeStatus[20];
    char safeMessage[241];
    jsonSafeString(status, safeStatus, sizeof(safeStatus));
    jsonSafeString(message, safeMessage, sizeof(safeMessage));
    snprintf(buffer, size, "{\"type\":\"ticketStatus\",\"ticketId\":%d,\"status\":\"%s\",\"message\":\"%s\"}\n",
             std::max(0, ticketId), safeStatus, safeMessage);
}

void Protocol::buildTicketApproveUnban(char *buffer, size_t size, int ticketId)
{
    snprintf(buffer, size, "{\"type\":\"ticketApproveUnban\",\"ticketId\":%d}\n", std::max(0, ticketId));
}

void Protocol::buildTicketCounts(char *buffer, size_t size)
{
    snprintf(buffer, size, "{\"type\":\"ticketCounts\"}\n");
}

void Protocol::buildStaffChatList(char *buffer, size_t size, int beforeId)
{
    snprintf(buffer, size, "{\"type\":\"staffChatList\",\"beforeId\":%d,\"limit\":8}\n", std::max(0, beforeId));
}

void Protocol::buildStaffChatSend(char *buffer, size_t size, const char *message)
{
    char safeMessage[241];
    jsonSafeString(message, safeMessage, sizeof(safeMessage));
    snprintf(buffer, size, "{\"type\":\"staffChatSend\",\"message\":\"%s\"}\n", safeMessage);
}

void Protocol::buildStaffChatRead(char *buffer, size_t size, int messageId)
{
    snprintf(buffer, size, "{\"type\":\"staffChatRead\",\"messageId\":%d}\n", std::max(0, messageId));
}

void Protocol::buildUpdateRequest(char *buffer, size_t size)
{
    snprintf(buffer, size, "GET /api/updates/latest HTTP/1.0\r\nHost: %s\r\n\r\n", SERVER_HTTP_HOST);
}
