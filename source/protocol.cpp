#include "protocol.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <algorithm>

static const char *findInRange(const char *start, const char *end, const char *key)
{
    if (!start || !key)
        return NULL;
    if (!end)
        return strstr(start, key);

    const size_t keyLength = strlen(key);
    if (keyLength == 0)
        return start;
    for (const char *ptr = start; ptr < end && *ptr; ptr++)
    {
        if ((size_t)(end - ptr) < keyLength)
            break;
        if (memcmp(ptr, key, keyLength) == 0)
            return ptr;
    }
    return NULL;
}

static bool jsonStringRange(const char *start, const char *end, const char *key, char *out, size_t outSize)
{
    if (!out || outSize == 0)
        return false;
    out[0] = '\0';

    const char *ptr = findInRange(start, end, key);
    if (!ptr)
        return false;
    ptr += strlen(key);

    size_t used = 0;
    while ((!end || ptr < end) && *ptr && *ptr != '"')
    {
        char value = *ptr++;
        if (value == '\\' && (!end || ptr < end) && *ptr)
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
    return true;
}

static bool jsonIntRange(const char *start, const char *end, const char *key, int &value)
{
    const char *ptr = findInRange(start, end, key);
    if (!ptr)
        return false;
    ptr += strlen(key);
    while ((!end || ptr < end) && *ptr && (*ptr == ' ' || *ptr == '\t'))
        ptr++;
    value = atoi(ptr);
    return true;
}

static bool jsonBoolRange(const char *start, const char *end, const char *key, bool &value)
{
    const char *ptr = findInRange(start, end, key);
    if (!ptr)
        return false;
    ptr += strlen(key);
    while ((!end || ptr < end) && *ptr && (*ptr == ' ' || *ptr == '\t'))
        ptr++;
    if ((!end || ptr + 4 <= end) && strncmp(ptr, "true", 4) == 0)
    {
        value = true;
        return true;
    }
    if ((!end || ptr + 5 <= end) && strncmp(ptr, "false", 5) == 0)
    {
        value = false;
        return true;
    }
    return false;
}

static const char *jsonObjectEnd(const char *start)
{
    if (!start || *start != '{')
        return NULL;

    int depth = 0;
    bool inString = false;
    bool escaped = false;
    for (const char *ptr = start; *ptr; ptr++)
    {
        if (inString)
        {
            if (escaped)
                escaped = false;
            else if (*ptr == '\\')
                escaped = true;
            else if (*ptr == '"')
                inString = false;
            continue;
        }
        if (*ptr == '"')
        {
            inString = true;
            continue;
        }
        if (*ptr == '{')
            depth++;
        else if (*ptr == '}' && --depth == 0)
            return ptr;
    }
    return NULL;
}

static int jsonInt(const char *line, const char *key)
{
    int value = 0;
    jsonIntRange(line, NULL, key, value);
    return value;
}

static void jsonString(const char *line, const char *key, char *out, size_t outSize)
{
    jsonStringRange(line, NULL, key, out, outSize);
}

static void jsonSafeString(const char *input, char *out, size_t outSize)
{
    size_t j = 0;
    if (!out || outSize == 0)
        return;
    for (size_t i = 0; input && input[i] && j + 1 < outSize; i++)
    {
        const unsigned char c = (unsigned char)input[i];
        const char *escape = NULL;
        if (c == '"')
            escape = "\\\"";
        else if (c == '\\')
            escape = "\\\\";
        else if (c == '\n')
            escape = "\\n";
        else if (c == '\r')
            escape = "\\r";
        else if (c == '\t')
            escape = "\\t";

        if (escape)
        {
            if (j + 2 >= outSize)
                break;
            out[j++] = escape[0];
            out[j++] = escape[1];
        }
        else if (c >= 32)
            out[j++] = (char)c;
    }
    out[j] = '\0';
}

static void parseIdentityFields(const char *start, const char *end, IdentityInfo &identity)
{
    identity.muteSecondsRemaining = 0;
    identity.banSecondsRemaining = 0;
    jsonStringRange(start, end, "\"id\":\"", identity.identityId, sizeof(identity.identityId));
    if (!identity.identityId[0])
        jsonStringRange(start, end, "\"identityId\":\"", identity.identityId, sizeof(identity.identityId));
    jsonStringRange(start, end, "\"username\":\"", identity.username, sizeof(identity.username));
    jsonStringRange(start, end, "\"displayName\":\"", identity.displayName, sizeof(identity.displayName));
    jsonStringRange(start, end, "\"role\":\"", identity.role, sizeof(identity.role));
    jsonStringRange(start, end, "\"status\":\"", identity.status, sizeof(identity.status));
    jsonIntRange(start, end, "\"muteSecondsRemaining\":", identity.muteSecondsRemaining);
    jsonIntRange(start, end, "\"banSecondsRemaining\":", identity.banSecondsRemaining);
    jsonStringRange(start, end, "\"banReason\":\"", identity.restrictionReason, sizeof(identity.restrictionReason));
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
    return parseChannels(line, channels, NULL, maxChannels, count, currentChannel);
}

bool Protocol::parseChannels(const char *line, char channels[][25], ChannelInfo *channelInfo,
                             int maxChannels, int &count, char *currentChannel)
{
    if (!line || !strstr(line, "\"type\":\"channels\""))
        return false;

    count = 0;
    if (currentChannel)
        jsonString(line, "\"currentChannel\":\"", currentChannel, 25);

    const char *ptr = strstr(line, "\"channels\":[");
    if (ptr && channels && maxChannels > 0)
    {
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
    }

    if (!channelInfo || maxChannels <= 0)
        return true;

    for (int i = 0; i < maxChannels; i++)
        memset(&channelInfo[i], 0, sizeof(channelInfo[i]));
    ptr = strstr(line, "\"channelInfo\":[");
    if (!ptr)
        return true;
    ptr += strlen("\"channelInfo\":[");
    int infoOrdinal = 0;
    while (*ptr && *ptr != ']' && infoOrdinal < maxChannels)
    {
        while (*ptr && *ptr != ']' && *ptr != '{')
            ptr++;
        if (!*ptr || *ptr == ']')
            break;
        const char *end = jsonObjectEnd(ptr);
        if (!end)
            break;

        ChannelInfo parsed;
        memset(&parsed, 0, sizeof(parsed));
        jsonStringRange(ptr, end, "\"name\":\"", parsed.name, sizeof(parsed.name));
        jsonIntRange(ptr, end, "\"userCount\":", parsed.userCount);
        jsonIntRange(ptr, end, "\"width\":", parsed.width);
        jsonIntRange(ptr, end, "\"height\":", parsed.height);
        jsonBoolRange(ptr, end, "\"staffOnly\":", parsed.staffOnly);
        jsonBoolRange(ptr, end, "\"adminOnly\":", parsed.adminOnly);
        jsonBoolRange(ptr, end, "\"readOnly\":", parsed.readOnly);

        int target = -1;
        for (int i = 0; i < count; i++)
        {
            if (parsed.name[0] && strcmp(channels[i], parsed.name) == 0)
            {
                target = i;
                break;
            }
        }
        if (target < 0 && infoOrdinal < count)
            target = infoOrdinal;
        if (target >= 0 && target < maxChannels)
        {
            if (!parsed.name[0])
                snprintf(parsed.name, sizeof(parsed.name), "%s", channels[target]);
            channelInfo[target] = parsed;
        }
        infoOrdinal++;
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
    PresenceInfo presence;
    return parsePresence(line, users, maxUsers, count, presence);
}

bool Protocol::parsePresence(const char *line, PresenceUser *users, int maxUsers, int &count,
                             PresenceInfo &presence)
{
    if (!line || !strstr(line, "\"type\":\"presence\""))
        return false;

    count = 0;
    memset(&presence, 0, sizeof(presence));
    const char *usersStart = strstr(line, "\"users\":[");
    const char *envelopeEnd = usersStart ? usersStart : NULL;
    jsonStringRange(line, envelopeEnd, "\"channel\":\"", presence.channel, sizeof(presence.channel));
    jsonIntRange(line, NULL, "\"total\":", presence.total);
    jsonBoolRange(line, NULL, "\"truncated\":", presence.truncated);

    if (!users || maxUsers <= 0 || !usersStart)
        return true;

    const char *ptr = usersStart + strlen("\"users\":[");
    while (*ptr && *ptr != ']' && count < maxUsers)
    {
        while (*ptr && *ptr != ']' && *ptr != '{')
            ptr++;
        if (!*ptr || *ptr == ']')
            break;
        const char *end = jsonObjectEnd(ptr);
        if (!end)
            break;

        memset(&users[count], 0, sizeof(users[count]));
        jsonStringRange(ptr, end, "\"id\":\"", users[count].id, sizeof(users[count].id));
        jsonStringRange(ptr, end, "\"identityId\":\"", users[count].identityId, sizeof(users[count].identityId));
        jsonStringRange(ptr, end, "\"username\":\"", users[count].username, sizeof(users[count].username));
        jsonStringRange(ptr, end, "\"displayName\":\"", users[count].displayName, sizeof(users[count].displayName));
        jsonStringRange(ptr, end, "\"role\":\"", users[count].role, sizeof(users[count].role));
        jsonStringRange(ptr, end, "\"status\":\"", users[count].status, sizeof(users[count].status));
        jsonStringRange(ptr, end, "\"channel\":\"", users[count].channel, sizeof(users[count].channel));
        jsonStringRange(ptr, end, "\"clientType\":\"", users[count].clientType, sizeof(users[count].clientType));
        jsonStringRange(ptr, end, "\"deviceModel\":\"", users[count].deviceModel, sizeof(users[count].deviceModel));
        jsonStringRange(ptr, end, "\"deviceModelLabel\":\"", users[count].deviceModelLabel, sizeof(users[count].deviceModelLabel));
        jsonIntRange(ptr, end, "\"sessionCount\":", users[count].sessionCount);
        jsonIntRange(ptr, end, "\"muteSecondsRemaining\":", users[count].muteSecondsRemaining);
        jsonIntRange(ptr, end, "\"banSecondsRemaining\":", users[count].banSecondsRemaining);
        jsonBoolRange(ptr, end, "\"readOnly\":", users[count].readOnly);
        if (!users[count].displayName[0])
            snprintf(users[count].displayName, sizeof(users[count].displayName), "USER");
        if (!users[count].role[0])
            snprintf(users[count].role, sizeof(users[count].role), "user");
        if (!users[count].status[0])
            snprintf(users[count].status, sizeof(users[count].status), "active");
        if (!users[count].deviceModelLabel[0])
            snprintf(users[count].deviceModelLabel, sizeof(users[count].deviceModelLabel),
                     "%s", users[count].deviceModel[0] ? users[count].deviceModel : "Unknown");
        if (users[count].sessionCount <= 0)
            users[count].sessionCount = 1;
        count++;
        ptr = end + 1;
    }
    if (presence.total <= 0)
        presence.total = count;
    if (presence.total > count)
        presence.truncated = true;
    return true;
}

bool Protocol::parseIdentityAccepted(const char *line, IdentityInfo &identity)
{
    if (!line || !strstr(line, "\"type\":\"identityAccepted\""))
        return false;
    parseIdentityFields(line, NULL, identity);
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

bool Protocol::parseRulesRejected(const char *line, char *reason, size_t reasonSize,
                                  char *version, size_t versionSize)
{
    if (!line || !strstr(line, "\"type\":\"rulesRejected\""))
        return false;
    jsonString(line, "\"reason\":\"", reason, reasonSize);
    jsonString(line, "\"version\":\"", version, versionSize);
    if (reason && reasonSize > 0 && !reason[0])
        snprintf(reason, reasonSize, "rules-rejected");
    return true;
}

bool Protocol::parseOnboardingState(const char *line, bool &needsDisplayName, bool &needsRules,
                                    char *rulesVersion, size_t rulesVersionSize)
{
    if (!line || !strstr(line, "\"type\":\"onboardingState\""))
        return false;
    needsDisplayName = strstr(line, "\"needsDisplayName\":true") != NULL;
    needsRules = strstr(line, "\"needsRules\":true") != NULL;
    jsonString(line, "\"rulesVersion\":\"", rulesVersion, rulesVersionSize);
    if (rulesVersion && rulesVersionSize > 0 && !rulesVersion[0])
        snprintf(rulesVersion, rulesVersionSize, "1");
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
    TicketCursor nextCursor;
    if (!parseTicketListEnd(line, nextCursor))
        return false;
    nextBeforeId = nextCursor.id;
    return true;
}

bool Protocol::parseTicketListEnd(const char *line, TicketCursor &nextCursor)
{
    if (!line || !strstr(line, "\"type\":\"ticketListEnd\""))
        return false;
    memset(&nextCursor, 0, sizeof(nextCursor));
    nextCursor.id = jsonInt(line, "\"nextBeforeId\":");
    jsonString(line, "\"nextBeforeUpdatedAt\":\"", nextCursor.updatedAt, sizeof(nextCursor.updatedAt));
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

bool Protocol::parseModerationResult(const char *line, ModerationResult &result)
{
    if (!line || !strstr(line, "\"type\":\"moderationResult\""))
        return false;

    memset(&result, 0, sizeof(result));
    jsonBoolRange(line, NULL, "\"ok\":", result.ok);
    jsonString(line, "\"action\":\"", result.action, sizeof(result.action));
    jsonString(line, "\"error\":\"", result.error, sizeof(result.error));

    const char *identityKey = strstr(line, "\"identity\":");
    if (identityKey)
    {
        const char *identityStart = strchr(identityKey + strlen("\"identity\":"), '{');
        const char *identityEnd = jsonObjectEnd(identityStart);
        if (identityStart && identityEnd)
        {
            parseIdentityFields(identityStart, identityEnd, result.identity);
            result.hasIdentity = result.identity.identityId[0] || result.identity.username[0];
            if (result.hasIdentity)
            {
                if (!result.identity.displayName[0])
                    snprintf(result.identity.displayName, sizeof(result.identity.displayName), "3DS User");
                if (!result.identity.role[0])
                    snprintf(result.identity.role, sizeof(result.identity.role), "user");
                if (!result.identity.status[0])
                    snprintf(result.identity.status, sizeof(result.identity.status), "active");
            }
        }
    }
    return true;
}

void Protocol::buildHello(char *buffer, size_t size, const char *appId, const char *version, bool updaterSupported,
                          const char *deviceId, const char *deviceSecret, const char *hardwareId, const char *deviceModel,
                          const char *displayName, const char *packageType)
{
    buildHello(buffer, size, appId, version, updaterSupported, deviceId, deviceSecret, hardwareId,
               deviceModel, displayName, packageType, NULL);
}

void Protocol::buildHello(char *buffer, size_t size, const char *appId, const char *version, bool updaterSupported,
                          const char *deviceId, const char *deviceSecret, const char *hardwareId, const char *deviceModel,
                          const char *displayName, const char *packageType, const char *preferredChannel)
{
    char safeAppId[40];
    char safeVersion[32];
    char safePackageType[12];
    char safeDeviceId[48];
    char safeDeviceSecret[64];
    char safeHardwareId[64];
    char safeDeviceModel[24];
    char safeDisplayName[25];
    char safePreferredChannel[25];
    jsonSafeString(appId, safeAppId, sizeof(safeAppId));
    jsonSafeString(version, safeVersion, sizeof(safeVersion));
    jsonSafeString(packageType ? packageType : "3dsx", safePackageType, sizeof(safePackageType));
    jsonSafeString(deviceId, safeDeviceId, sizeof(safeDeviceId));
    jsonSafeString(deviceSecret, safeDeviceSecret, sizeof(safeDeviceSecret));
    jsonSafeString(hardwareId, safeHardwareId, sizeof(safeHardwareId));
    jsonSafeString(deviceModel ? deviceModel : "3ds-family", safeDeviceModel, sizeof(safeDeviceModel));
    jsonSafeString(displayName ? displayName : "3DS User", safeDisplayName, sizeof(safeDisplayName));
    jsonSafeString(preferredChannel, safePreferredChannel, sizeof(safePreferredChannel));

    const char *capabilities =
        "\"capabilities\":[\"ui2-channel-info\",\"ui2-presence-compact\",\"ui2-ticket-cursor\","
        "\"draw-size-tenths\",\"brush-feather\",\"brush-suite-v2\"]";
    if (safePreferredChannel[0])
    {
        snprintf(buffer, size,
                 "{\"type\":\"hello\",\"appId\":\"%s\",\"version\":\"%s\",\"protocol\":6,\"updaterSupported\":%s,"
                 "\"packageType\":\"%s\",\"deviceId\":\"%s\",\"deviceSecret\":\"%s\",\"hardwareId\":\"%s\","
                 "\"deviceModel\":\"%s\",\"displayName\":\"%s\",%s,\"preferredChannel\":\"%s\"}",
                 safeAppId, safeVersion, updaterSupported ? "true" : "false", safePackageType,
                 safeDeviceId, safeDeviceSecret, safeHardwareId, safeDeviceModel, safeDisplayName,
                 capabilities, safePreferredChannel);
    }
    else
    {
        snprintf(buffer, size,
                 "{\"type\":\"hello\",\"appId\":\"%s\",\"version\":\"%s\",\"protocol\":6,\"updaterSupported\":%s,"
                 "\"packageType\":\"%s\",\"deviceId\":\"%s\",\"deviceSecret\":\"%s\",\"hardwareId\":\"%s\","
                 "\"deviceModel\":\"%s\",\"displayName\":\"%s\",%s}",
                 safeAppId, safeVersion, updaterSupported ? "true" : "false", safePackageType,
                 safeDeviceId, safeDeviceSecret, safeHardwareId, safeDeviceModel, safeDisplayName,
                 capabilities);
    }
}

void Protocol::buildSwitchChannel(char *buffer, size_t size, const char *channel)
{
    snprintf(buffer, size, "{\"type\":\"switchChannel\",\"channel\":\"%s\"}", channel);
}

void Protocol::buildGetCanvas(char *buffer, size_t size)
{
    snprintf(buffer, size, "{\"type\":\"getCanvas\"}");
}

void Protocol::buildSetDisplayName(char *buffer, size_t size, const char *displayName)
{
    char safeName[25];
    jsonSafeString(displayName, safeName, sizeof(safeName));
    snprintf(buffer, size, "{\"type\":\"setDisplayName\",\"displayName\":\"%s\"}", safeName);
}

void Protocol::buildRulesAccepted(char *buffer, size_t size, const char *version)
{
    char safeVersion[32];
    jsonSafeString(version, safeVersion, sizeof(safeVersion));
    snprintf(buffer, size, "{\"type\":\"rulesAccepted\",\"version\":\"%s\"}", safeVersion[0] ? safeVersion : "1");
}

void Protocol::buildGetOnboardingState(char *buffer, size_t size)
{
    snprintf(buffer, size, "{\"type\":\"getOnboardingState\"}");
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
             "\"deviceId\":\"%s\",\"deviceSecret\":\"%s\",\"hardwareId\":\"%s\"}",
             safeUsername, safeBackup, safeDeviceId, safeSecret, safeHardware);
}

void Protocol::buildRotateBackupCode(char *buffer, size_t size)
{
    snprintf(buffer, size, "{\"type\":\"rotateBackupCode\"}");
}

void Protocol::buildModerationCommand(char *buffer, size_t size, const char *action, const char *identityId,
                                      int messageId, const char *reason)
{
    char safeAction[24];
    char safeIdentity[48];
    char safeReason[161];
    jsonSafeString(action, safeAction, sizeof(safeAction));
    jsonSafeString(identityId, safeIdentity, sizeof(safeIdentity));
    jsonSafeString(reason, safeReason, sizeof(safeReason));
    snprintf(buffer, size,
             "{\"type\":\"moderation\",\"action\":\"%s\",\"identityId\":\"%s\",\"messageId\":%d,\"reason\":\"%s\"}",
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
             "\"color\":[%d,%d,%d]}",
             safeAction, safeChannel, x, y, width, height, r, g, b);
}

void Protocol::buildTicketCreate(char *buffer, size_t size, const char *category, const char *subject, const char *message)
{
    char safeCategory[24];
    char safeSubject[129];
    char safeMessage[481];
    jsonSafeString(category, safeCategory, sizeof(safeCategory));
    jsonSafeString(subject, safeSubject, sizeof(safeSubject));
    jsonSafeString(message, safeMessage, sizeof(safeMessage));
    snprintf(buffer, size, "{\"type\":\"ticketCreate\",\"category\":\"%s\",\"subject\":\"%s\",\"message\":\"%s\"}",
             safeCategory, safeSubject, safeMessage);
}

void Protocol::buildTicketList(char *buffer, size_t size, bool staff, const char *status, const char *category, int beforeId)
{
    char safeStatus[20];
    char safeCategory[12];
    jsonSafeString(status, safeStatus, sizeof(safeStatus));
    jsonSafeString(category, safeCategory, sizeof(safeCategory));
    snprintf(buffer, size,
             "{\"type\":\"ticketList\",\"scope\":\"%s\",\"status\":\"%s\",\"category\":\"%s\",\"beforeId\":%d,\"limit\":6}",
             staff ? "staff" : "mine", safeStatus, safeCategory, std::max(0, beforeId));
}

void Protocol::buildTicketList(char *buffer, size_t size, bool staff, const char *status, const char *category,
                               const TicketCursor &before)
{
    char safeStatus[20];
    char safeCategory[12];
    char safeUpdatedAt[25];
    jsonSafeString(status, safeStatus, sizeof(safeStatus));
    jsonSafeString(category, safeCategory, sizeof(safeCategory));
    jsonSafeString(before.updatedAt, safeUpdatedAt, sizeof(safeUpdatedAt));
    snprintf(buffer, size,
             "{\"type\":\"ticketList\",\"scope\":\"%s\",\"status\":\"%s\",\"category\":\"%s\","
             "\"beforeUpdatedAt\":\"%s\",\"beforeId\":%d,\"limit\":6}",
             staff ? "staff" : "mine", safeStatus, safeCategory, safeUpdatedAt, std::max(0, before.id));
}

void Protocol::buildTicketGet(char *buffer, size_t size, int ticketId, int beforeMessageId)
{
    snprintf(buffer, size, "{\"type\":\"ticketGet\",\"ticketId\":%d,\"beforeMessageId\":%d,\"limit\":6}",
             std::max(0, ticketId), std::max(0, beforeMessageId));
}

void Protocol::buildTicketReply(char *buffer, size_t size, int ticketId, const char *message, bool staff)
{
    char safeMessage[481];
    jsonSafeString(message, safeMessage, sizeof(safeMessage));
    snprintf(buffer, size, "{\"type\":\"ticketReply\",\"ticketId\":%d,\"staff\":%s,\"message\":\"%s\"}",
             std::max(0, ticketId), staff ? "true" : "false", safeMessage);
}

void Protocol::buildTicketStatus(char *buffer, size_t size, int ticketId, const char *status, const char *message)
{
    char safeStatus[20];
    char safeMessage[481];
    jsonSafeString(status, safeStatus, sizeof(safeStatus));
    jsonSafeString(message, safeMessage, sizeof(safeMessage));
    snprintf(buffer, size, "{\"type\":\"ticketStatus\",\"ticketId\":%d,\"status\":\"%s\",\"message\":\"%s\"}",
             std::max(0, ticketId), safeStatus, safeMessage);
}

void Protocol::buildTicketApproveUnban(char *buffer, size_t size, int ticketId)
{
    snprintf(buffer, size, "{\"type\":\"ticketApproveUnban\",\"ticketId\":%d}", std::max(0, ticketId));
}

void Protocol::buildTicketCounts(char *buffer, size_t size)
{
    snprintf(buffer, size, "{\"type\":\"ticketCounts\"}");
}

void Protocol::buildStaffChatList(char *buffer, size_t size, int beforeId)
{
    snprintf(buffer, size, "{\"type\":\"staffChatList\",\"beforeId\":%d,\"limit\":8}", std::max(0, beforeId));
}

void Protocol::buildStaffChatSend(char *buffer, size_t size, const char *message)
{
    char safeMessage[481];
    jsonSafeString(message, safeMessage, sizeof(safeMessage));
    snprintf(buffer, size, "{\"type\":\"staffChatSend\",\"message\":\"%s\"}", safeMessage);
}

void Protocol::buildStaffChatRead(char *buffer, size_t size, int messageId)
{
    snprintf(buffer, size, "{\"type\":\"staffChatRead\",\"messageId\":%d}", std::max(0, messageId));
}
