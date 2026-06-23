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
    count = 0;
    if (!line || !strstr(line, "\"type\":\"channels\""))
        return false;
    jsonString(line, "\"currentChannel\":\"", currentChannel, 25);

    const char *ptr = strstr(line, "\"channels\":[");
    if (!ptr)
        return true;
    ptr += strlen("\"channels\":[");
    while (*ptr && count < maxChannels)
    {
        while (*ptr && *ptr != '"')
            ptr++;
        if (!*ptr)
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

void Protocol::buildSwitchChannel(char *buffer, size_t size, const char *channel)
{
    snprintf(buffer, size, "{\"type\":\"switchChannel\",\"channel\":\"%s\"}\n", channel);
}

void Protocol::buildUpdateRequest(char *buffer, size_t size)
{
    snprintf(buffer, size, "GET /api/updates/latest HTTP/1.0\r\nHost: server1.rpgwo.org\r\n\r\n");
}
