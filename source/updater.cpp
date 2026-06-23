#include "updater.h"
#include "network.h"
#include <3ds.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <netdb.h>

static bool parseJsonString(const char *text, const char *key, char *out, size_t outSize)
{
    const char *ptr = strstr(text, key);
    if (!ptr)
        return false;
    ptr += strlen(key);
    const char *end = strchr(ptr, '"');
    if (!end)
        return false;
    size_t len = end - ptr;
    if (len >= outSize)
        len = outSize - 1;
    memcpy(out, ptr, len);
    out[len] = '\0';
    return true;
}

static int parseJsonInt(const char *text, const char *key)
{
    const char *ptr = strstr(text, key);
    if (!ptr)
        return 0;
    return atoi(ptr + strlen(key));
}

static bool connectHttp(const char *serverDomain, const char *httpPort, int &sock)
{
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
        return false;

    struct addrinfo hints;
    struct addrinfo *result;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(serverDomain, httpPort, &hints, &result) != 0)
    {
        close(sock);
        sock = -1;
        return false;
    }

    bool connected = false;
    for (struct addrinfo *rp = result; rp != NULL; rp = rp->ai_next)
    {
        if (::connect(sock, rp->ai_addr, rp->ai_addrlen) == 0)
        {
            connected = true;
            break;
        }
    }
    freeaddrinfo(result);
    if (!connected)
    {
        close(sock);
        sock = -1;
    }
    return connected;
}

static bool downloadArtifact(const char *serverDomain, const char *httpPort, const char *artifactUrl, int expectedSize)
{
    const char *path = strstr(artifactUrl, "/updates/");
    if (!path)
        return false;

    int sock = -1;
    if (!connectHttp(serverDomain, httpPort, sock))
        return false;

    char request[256];
    snprintf(request, sizeof(request), "GET %s HTTP/1.0\r\nHost: %s\r\n\r\n", path, serverDomain);
    send(sock, request, strlen(request), 0);

    FILE *file = fopen("sdmc:/3ds/CollabDoodle-update.3dsx", "wb");
    if (!file)
    {
        close(sock);
        return false;
    }

    char buffer[1024];
    bool headerDone = false;
    int written = 0;
    char headerBuffer[2049];
    int headerLen = 0;

    while (true)
    {
        int len = recv(sock, buffer, sizeof(buffer), 0);
        if (len <= 0)
            break;

        if (!headerDone)
        {
            int copyLen = len;
            if (headerLen + copyLen > (int)sizeof(headerBuffer))
                copyLen = sizeof(headerBuffer) - headerLen;
            memcpy(headerBuffer + headerLen, buffer, copyLen);
            headerLen += copyLen;
            headerBuffer[headerLen] = '\0';

            char *body = strstr(headerBuffer, "\r\n\r\n");
            if (body)
            {
                body += 4;
                headerDone = true;
                int bodyOffset = body - headerBuffer;
                int bodyLen = headerLen - bodyOffset;
                if (bodyLen > 0)
                {
                    fwrite(body, 1, bodyLen, file);
                    written += bodyLen;
                }
                if (copyLen < len)
                {
                    fwrite(buffer + copyLen, 1, len - copyLen, file);
                    written += len - copyLen;
                }
            }
        }
        else
        {
            fwrite(buffer, 1, len, file);
            written += len;
        }
    }

    fclose(file);
    close(sock);
    return expectedSize <= 0 || written == expectedSize;
}

bool Updater::checkForUpdate(const char *serverDomain, const char *httpPort, const char *currentVersion)
{
    int sock = -1;
    if (!connectHttp(serverDomain, httpPort, sock))
        return false;

    char request[160];
    snprintf(request, sizeof(request), "GET /api/updates/latest HTTP/1.0\r\nHost: %s\r\n\r\n", serverDomain);
    send(sock, request, strlen(request), 0);

    char response[4096];
    int len = recv(sock, response, sizeof(response) - 1, 0);
    close(sock);
    if (len <= 0)
        return false;
    response[len] = '\0';

    char latest[32];
    if (!parseJsonString(response, "\"latestVersion\":\"", latest, sizeof(latest)))
        return false;

    if (strcmp(latest, currentVersion) <= 0)
        return false;

    char artifactUrl[256];
    if (parseJsonString(response, "\"artifactUrl\":\"", artifactUrl, sizeof(artifactUrl)))
    {
        int artifactSize = parseJsonInt(response, "\"artifactSize\":");
        if (downloadArtifact(serverDomain, httpPort, artifactUrl, artifactSize))
            printf("Downloaded update to sdmc:/3ds/CollabDoodle-update.3dsx\n");
        else
            printf("Update available, but download failed.\n");
    }

    return true;
}
