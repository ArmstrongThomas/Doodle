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

void Protocol::buildHello(char *buffer, size_t size, const char *appId, const char *version, bool updaterSupported)
{
    snprintf(buffer, size,
             "{\"type\":\"hello\",\"appId\":\"%s\",\"version\":\"%s\",\"protocol\":2,\"updaterSupported\":%s}\n",
             appId, version, updaterSupported ? "true" : "false");
}

void Protocol::buildSwitchChannel(char *buffer, size_t size, const char *channel)
{
    snprintf(buffer, size, "{\"type\":\"switchChannel\",\"channel\":\"%s\"}\n", channel);
}

void Protocol::buildUpdateRequest(char *buffer, size_t size)
{
    snprintf(buffer, size, "GET /api/updates/latest HTTP/1.0\r\nHost: %s\r\n\r\n", SERVER_HOST);
}
