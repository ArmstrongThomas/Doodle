#include "updater.h"
#include "network.h"
#include <3ds.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <netdb.h>
#include <sys/stat.h>
#include <errno.h>

static const char *UPDATE_FINAL_PATH = "sdmc:/3ds/CollabDoodle-update.3dsx";
static const int INSTALL_PROGRESS_OFFSET = 1000000;
static char gLastUpdateError[160] = "";

static void setUpdateError(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(gLastUpdateError, sizeof(gLastUpdateError), fmt, ap);
    va_end(ap);
}

static unsigned int rotr(unsigned int value, unsigned int bits)
{
    return (value >> bits) | (value << (32 - bits));
}

struct Sha256State
{
    unsigned int h[8];
    unsigned long long length;
    unsigned char block[64];
    int blockLen;
};

static const unsigned int SHA256_K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

static void sha256Transform(Sha256State &state, const unsigned char *block)
{
    unsigned int w[64];
    for (int i = 0; i < 16; i++)
    {
        w[i] = ((unsigned int)block[i * 4] << 24) |
               ((unsigned int)block[i * 4 + 1] << 16) |
               ((unsigned int)block[i * 4 + 2] << 8) |
               (unsigned int)block[i * 4 + 3];
    }
    for (int i = 16; i < 64; i++)
    {
        unsigned int s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        unsigned int s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    unsigned int a = state.h[0];
    unsigned int b = state.h[1];
    unsigned int c = state.h[2];
    unsigned int d = state.h[3];
    unsigned int e = state.h[4];
    unsigned int f = state.h[5];
    unsigned int g = state.h[6];
    unsigned int h = state.h[7];

    for (int i = 0; i < 64; i++)
    {
        unsigned int s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        unsigned int ch = (e & f) ^ ((~e) & g);
        unsigned int temp1 = h + s1 + ch + SHA256_K[i] + w[i];
        unsigned int s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        unsigned int maj = (a & b) ^ (a & c) ^ (b & c);
        unsigned int temp2 = s0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    state.h[0] += a;
    state.h[1] += b;
    state.h[2] += c;
    state.h[3] += d;
    state.h[4] += e;
    state.h[5] += f;
    state.h[6] += g;
    state.h[7] += h;
}

static void sha256Init(Sha256State &state)
{
    state.h[0] = 0x6a09e667;
    state.h[1] = 0xbb67ae85;
    state.h[2] = 0x3c6ef372;
    state.h[3] = 0xa54ff53a;
    state.h[4] = 0x510e527f;
    state.h[5] = 0x9b05688c;
    state.h[6] = 0x1f83d9ab;
    state.h[7] = 0x5be0cd19;
    state.length = 0;
    state.blockLen = 0;
}

static void sha256Update(Sha256State &state, const unsigned char *data, int len)
{
    state.length += (unsigned long long)len * 8;
    for (int i = 0; i < len; i++)
    {
        state.block[state.blockLen++] = data[i];
        if (state.blockLen == 64)
        {
            sha256Transform(state, state.block);
            state.blockLen = 0;
        }
    }
}

static void sha256Final(Sha256State &state, char outHex[65])
{
    state.block[state.blockLen++] = 0x80;
    if (state.blockLen > 56)
    {
        while (state.blockLen < 64)
            state.block[state.blockLen++] = 0;
        sha256Transform(state, state.block);
        state.blockLen = 0;
    }
    while (state.blockLen < 56)
        state.block[state.blockLen++] = 0;

    for (int i = 7; i >= 0; i--)
        state.block[state.blockLen++] = (state.length >> (i * 8)) & 0xff;
    sha256Transform(state, state.block);

    for (int i = 0; i < 8; i++)
        sprintf(outHex + i * 8, "%08x", state.h[i]);
    outHex[64] = '\0';
}

static bool fileSha256(const char *path, char outHex[65])
{
    FILE *file = fopen(path, "rb");
    if (!file)
        return false;

    Sha256State state;
    sha256Init(state);
    unsigned char buffer[2048];
    while (true)
    {
        int read = fread(buffer, 1, sizeof(buffer), file);
        if (read > 0)
            sha256Update(state, buffer, read);
        if (read < (int)sizeof(buffer))
            break;
    }
    fclose(file);
    sha256Final(state, outHex);
    return true;
}

static bool parseJsonString(const char *text, const char *key, char *out, size_t outSize)
{
    out[0] = '\0';
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

static int compareVersions(const char *left, const char *right)
{
    int leftParts[3] = {0, 0, 0};
    int rightParts[3] = {0, 0, 0};
    sscanf(left ? left : "0.0.0", "%d.%d.%d", &leftParts[0], &leftParts[1], &leftParts[2]);
    sscanf(right ? right : "0.0.0", "%d.%d.%d", &rightParts[0], &rightParts[1], &rightParts[2]);

    for (int i = 0; i < 3; i++)
    {
        if (leftParts[i] > rightParts[i])
            return 1;
        if (leftParts[i] < rightParts[i])
            return -1;
    }
    return 0;
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

static bool readHttpResponse(int sock, char *response, int responseSize, int &responseLen)
{
    responseLen = 0;
    while (responseLen < responseSize - 1)
    {
        int len = recv(sock, response + responseLen, responseSize - 1 - responseLen, 0);
        if (len <= 0)
            break;
        responseLen += len;
    }
    response[responseLen] = '\0';
    return responseLen > 0;
}

static const char *httpBody(char *response)
{
    char *body = strstr(response, "\r\n\r\n");
    return body ? body + 4 : response;
}

bool Updater::fetchManifest(const char *serverDomain, const char *httpPort, const char *currentVersion, UpdateManifest &manifest)
{
    return fetchManifest(serverDomain, httpPort, currentVersion, "3dsx", manifest);
}

bool Updater::fetchManifest(const char *serverDomain, const char *httpPort, const char *currentVersion,
                            const char *packageType, UpdateManifest &manifest)
{
    memset(&manifest, 0, sizeof(manifest));

    int sock = -1;
    if (!connectHttp(serverDomain, httpPort, sock))
        return false;

    const char *cleanPackage = (packageType && strcmp(packageType, "cia") == 0) ? "cia" : "3dsx";
    char request[224];
    snprintf(request, sizeof(request), "GET /api/updates/latest?package=%s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n",
             cleanPackage, serverDomain);
    send(sock, request, strlen(request), 0);

    char response[8192];
    int responseLen = 0;
    bool ok = readHttpResponse(sock, response, sizeof(response), responseLen);
    close(sock);
    if (!ok)
        return false;

    const char *body = httpBody(response);
    if (!parseJsonString(body, "\"latestVersion\":\"", manifest.latestVersion, sizeof(manifest.latestVersion)))
        return false;

    parseJsonString(body, "\"releaseNotes\":\"", manifest.releaseNotes, sizeof(manifest.releaseNotes));
    parseJsonString(body, "\"artifactUrl\":\"", manifest.artifactUrl, sizeof(manifest.artifactUrl));
    parseJsonString(body, "\"artifactName\":\"", manifest.artifactName, sizeof(manifest.artifactName));
    parseJsonString(body, "\"artifactType\":\"", manifest.artifactType, sizeof(manifest.artifactType));
    parseJsonString(body, "\"sha256\":\"", manifest.sha256, sizeof(manifest.sha256));
    manifest.artifactSize = parseJsonInt(body, "\"artifactSize\":");
    manifest.available = compareVersions(manifest.latestVersion, currentVersion) > 0;
    return true;
}

static const char *artifactPathFromUrl(const char *artifactUrl)
{
    const char *path = strstr(artifactUrl, "/updates/");
    return path ? path : NULL;
}

static void makeSiblingPath(const char *targetPath, const char *suffix, char *out, size_t outSize)
{
    snprintf(out, outSize, "%s%s", targetPath && targetPath[0] ? targetPath : UPDATE_FINAL_PATH, suffix);
}

static bool replaceWithBackup(const char *partPath, const char *targetPath)
{
    if (!targetPath || !targetPath[0])
        targetPath = UPDATE_FINAL_PATH;

    char backupPath[320];
    makeSiblingPath(targetPath, ".bak", backupPath, sizeof(backupPath));
    remove(backupPath);
    rename(targetPath, backupPath);

    if (rename(partPath, targetPath) == 0)
    {
        remove(backupPath);
        return true;
    }

    rename(backupPath, targetPath);
    return false;
}

static const char *sdmcPath(const char *path)
{
    if (!path)
        return "";
    return strncmp(path, "sdmc:", 5) == 0 ? path + 5 : path;
}

static FS_MediaType titleDestination(u64 titleId)
{
    u16 platform = (u16)((titleId >> 48) & 0xffff);
    u16 category = (u16)((titleId >> 32) & 0xffff);
    u8 variation = (u8)(titleId & 0xff);
    if (platform == 0x0003 || (platform == 0x0004 && ((category & 0x8011) != 0 || (category == 0x0000 && variation == 0x02))))
        return MEDIATYPE_NAND;
    return MEDIATYPE_SD;
}

static bool readCiaInfo(const char *ciaPath, AM_TitleEntry &info, FS_MediaType &media)
{
    Handle fileHandle = 0;
    Result ret = FSUSER_OpenFileDirectly(&fileHandle, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""),
                                         fsMakePath(PATH_ASCII, sdmcPath(ciaPath)), FS_OPEN_READ, 0);
    if (R_FAILED(ret))
    {
        setUpdateError("OPEN CIA META 0x%08lX", (unsigned long)ret);
        printf("%s\n", gLastUpdateError);
        return false;
    }

    ret = AM_GetCiaFileInfo(MEDIATYPE_SD, &info, fileHandle);
    FSFILE_Close(fileHandle);
    if (R_FAILED(ret))
    {
        setUpdateError("CIA META 0x%08lX", (unsigned long)ret);
        printf("%s\n", gLastUpdateError);
        return false;
    }

    media = titleDestination(info.titleID);
    printf("CIA title: 0x%016llx media=%d\n", (unsigned long long)info.titleID, (int)media);
    return true;
}

static UpdateDownloadResult installCiaFromFile(const char *ciaPath, unsigned long long expectedTitleId,
                                               UpdateProgressCallback progress, void *userData)
{
    if (!ciaPath || !ciaPath[0])
    {
        setUpdateError("NO CIA PATH");
        return UPDATE_DOWNLOAD_INSTALL_FAILED;
    }

    FILE *file = fopen(ciaPath, "rb");
    if (!file)
    {
        setUpdateError("OPEN CIA FILE ERR %d", errno);
        return UPDATE_DOWNLOAD_INSTALL_FAILED;
    }

    fseek(file, 0, SEEK_END);
    long fileSize = ftell(file);
    fseek(file, 0, SEEK_SET);
    if (fileSize <= 0)
    {
        setUpdateError("CIA SIZE %ld", fileSize);
        fclose(file);
        return UPDATE_DOWNLOAD_INSTALL_FAILED;
    }

    Result ret = amInit();
    if (R_FAILED(ret))
    {
        setUpdateError("AM INIT 0x%08lX", (unsigned long)ret);
        printf("%s\n", gLastUpdateError);
        fclose(file);
        return UPDATE_DOWNLOAD_INSTALL_FAILED;
    }

    AM_TitleEntry info;
    memset(&info, 0, sizeof(info));
    FS_MediaType media = MEDIATYPE_SD;
    bool haveCiaInfo = readCiaInfo(ciaPath, info, media);
    if (!haveCiaInfo)
    {
        media = MEDIATYPE_SD;
        printf("CIA metadata unavailable; continuing with SD install.\n");
    }
    if (haveCiaInfo && expectedTitleId != 0 && info.titleID != (u64)expectedTitleId)
    {
        setUpdateError("TITLE MISMATCH GOT %08lX%08lX EXP %08lX%08lX",
                       (unsigned long)(info.titleID >> 32), (unsigned long)(info.titleID & 0xffffffff),
                       (unsigned long)(((u64)expectedTitleId) >> 32), (unsigned long)(((u64)expectedTitleId) & 0xffffffff));
        printf("%s\n", gLastUpdateError);
        amExit();
        fclose(file);
        return UPDATE_DOWNLOAD_INSTALL_FAILED;
    }

    Handle ciaHandle = 0;
    ret = AM_StartCiaInstall(media, &ciaHandle);
    if (R_FAILED(ret))
        ret = AM_StartCiaInstallOverwrite(&ciaHandle, media);

    if (R_FAILED(ret))
    {
        setUpdateError("AM START 0x%08lX MEDIA %d", (unsigned long)ret, (int)media);
        printf("%s\n", gLastUpdateError);
        amExit();
        fclose(file);
        return UPDATE_DOWNLOAD_INSTALL_FAILED;
    }

    unsigned char *buffer = (unsigned char *)malloc(64 * 1024);
    if (!buffer)
    {
        setUpdateError("INSTALL OOM");
        AM_CancelCIAInstall(ciaHandle);
        amExit();
        fclose(file);
        return UPDATE_DOWNLOAD_INSTALL_FAILED;
    }

    long offset = 0;
    bool ok = true;
    while (offset < fileSize)
    {
        size_t read = fread(buffer, 1, 64 * 1024, file);
        if (read == 0)
        {
            ok = false;
            break;
        }

        u32 written = 0;
        ret = FSFILE_Write(ciaHandle, &written, (u64)offset, buffer, (u32)read, FS_WRITE_FLUSH);
        if (R_FAILED(ret) || written != read)
        {
            setUpdateError("CIA WRITE 0x%08lX W%lu R%lu", (unsigned long)ret, (unsigned long)written, (unsigned long)read);
            printf("%s\n", gLastUpdateError);
            ok = false;
            break;
        }

        offset += (long)read;
        if (progress)
            progress(INSTALL_PROGRESS_OFFSET + offset, (int)fileSize, userData);
    }

    free(buffer);
    fclose(file);

    if (!ok)
    {
        AM_CancelCIAInstall(ciaHandle);
        amExit();
        return UPDATE_DOWNLOAD_INSTALL_FAILED;
    }

    ret = AM_FinishCiaInstall(ciaHandle);
    amExit();
    if (R_FAILED(ret))
    {
        setUpdateError("AM FINISH 0x%08lX", (unsigned long)ret);
        printf("%s\n", gLastUpdateError);
        return UPDATE_DOWNLOAD_INSTALL_FAILED;
    }

    remove(ciaPath);
    return UPDATE_DOWNLOAD_OK;
}

const char *Updater::lastError()
{
    return gLastUpdateError[0] ? gLastUpdateError : "NO DETAIL";
}

UpdateDownloadResult Updater::downloadUpdate(const char *serverDomain, const char *httpPort, const UpdateManifest &manifest,
                                             const char *targetPath, UpdateProgressCallback progress, void *userData)
{
    if (!manifest.available)
        return UPDATE_DOWNLOAD_NO_UPDATE;
    if (!manifest.artifactUrl[0])
        return UPDATE_DOWNLOAD_NO_ARTIFACT;

    const char *path = artifactPathFromUrl(manifest.artifactUrl);
    if (!path)
        return UPDATE_DOWNLOAD_NO_ARTIFACT;

    int sock = -1;
    if (!connectHttp(serverDomain, httpPort, sock))
        return UPDATE_DOWNLOAD_FAILED;

    char partPath[320];
    if (targetPath && strncmp(targetPath, "sdmc:/cias/", 11) == 0)
        mkdir("sdmc:/cias", 0777);
    makeSiblingPath(targetPath, ".part", partPath, sizeof(partPath));
    remove(partPath);

    char request[320];
    snprintf(request, sizeof(request), "GET %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n", path, serverDomain);
    send(sock, request, strlen(request), 0);

    FILE *file = fopen(partPath, "wb");
    if (!file)
    {
        close(sock);
        return UPDATE_DOWNLOAD_FAILED;
    }

    char buffer[2048];
    bool headerDone = false;
    int written = 0;
    char headerBuffer[4096];
    int headerLen = 0;

    while (true)
    {
        int len = recv(sock, buffer, sizeof(buffer), 0);
        if (len <= 0)
            break;

        if (!headerDone)
        {
            int copyLen = len;
            if (headerLen + copyLen > (int)sizeof(headerBuffer) - 1)
                copyLen = sizeof(headerBuffer) - 1 - headerLen;
            if (copyLen > 0)
            {
                memcpy(headerBuffer + headerLen, buffer, copyLen);
                headerLen += copyLen;
                headerBuffer[headerLen] = '\0';
            }

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
                    if (progress)
                        progress(written, manifest.artifactSize, userData);
                }
                if (copyLen < len)
                {
                    fwrite(buffer + copyLen, 1, len - copyLen, file);
                    written += len - copyLen;
                    if (progress)
                        progress(written, manifest.artifactSize, userData);
                }
            }
        }
        else
        {
            fwrite(buffer, 1, len, file);
            written += len;
            if (progress)
                progress(written, manifest.artifactSize, userData);
        }
    }

    fclose(file);
    close(sock);

    if (manifest.artifactSize > 0 && written != manifest.artifactSize)
    {
        remove(partPath);
        return UPDATE_DOWNLOAD_SIZE_MISMATCH;
    }

    if (manifest.sha256[0])
    {
        char digest[65];
        if (!fileSha256(partPath, digest) || strcasecmp(digest, manifest.sha256) != 0)
        {
            remove(partPath);
            return UPDATE_DOWNLOAD_CHECKSUM_MISMATCH;
        }
    }

    if (!replaceWithBackup(partPath, targetPath && targetPath[0] ? targetPath : UPDATE_FINAL_PATH))
    {
        remove(partPath);
        return UPDATE_DOWNLOAD_FAILED;
    }

    return UPDATE_DOWNLOAD_OK;
}

UpdateDownloadResult Updater::downloadAndInstallCia(const char *serverDomain, const char *httpPort, const UpdateManifest &manifest,
                                                    const char *stagingPath, unsigned long long expectedTitleId,
                                                    UpdateProgressCallback progress, void *userData)
{
    UpdateDownloadResult result = downloadUpdate(serverDomain, httpPort, manifest, stagingPath, progress, userData);
    if (result != UPDATE_DOWNLOAD_OK)
        return result;
    return installCiaFromFile(stagingPath, expectedTitleId, progress, userData);
}

UpdateDownloadResult Updater::downloadUpdate(const char *serverDomain, const char *httpPort, const UpdateManifest &manifest)
{
    return downloadUpdate(serverDomain, httpPort, manifest, UPDATE_FINAL_PATH, NULL, NULL);
}

bool Updater::checkForUpdate(const char *serverDomain, const char *httpPort, const char *currentVersion)
{
    UpdateManifest manifest;
    if (!fetchManifest(serverDomain, httpPort, currentVersion, manifest))
    {
        printf("Update manifest failed.\n");
        return false;
    }

    if (!manifest.available)
    {
        printf("Already on latest version %s.\n", currentVersion);
        return false;
    }

    printf("Update available: %s\n", manifest.latestVersion);
    UpdateDownloadResult result = downloadUpdate(serverDomain, httpPort, manifest);
    if (result == UPDATE_DOWNLOAD_OK)
    {
        printf("Downloaded update to %s\n", UPDATE_FINAL_PATH);
        return true;
    }

    printf("Update download failed: %d\n", result);
    return true;
}

bool Updater::relaunchInstalledTitle(unsigned long long titleId)
{
    unsigned char param[0x300];
    unsigned char hmac[0x20];
    memset(param, 0, sizeof(param));
    memset(hmac, 0, sizeof(hmac));

    Result ret = APT_PrepareToDoApplicationJump(0, (u64)titleId, MEDIATYPE_SD);
    if (R_FAILED(ret))
    {
        printf("APT prepare jump failed: 0x%08lx\n", (unsigned long)ret);
        return false;
    }

    ret = APT_DoApplicationJump(param, sizeof(param), hmac);
    if (R_FAILED(ret))
    {
        printf("APT jump failed: 0x%08lx\n", (unsigned long)ret);
        return false;
    }

    return true;
}
