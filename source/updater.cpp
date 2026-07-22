#include "updater.h"
#include "https_client.h"
#include <3ds.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <errno.h>
#include <limits.h>
#include <ctype.h>

static const char *UPDATE_FINAL_PATH = "sdmc:/3ds/CollabDoodle-update.3dsx";
static const int INSTALL_PROGRESS_OFFSET = 1000000;
static const int MAX_ARTIFACT_BYTES = 64 * 1024 * 1024;
static const char *EXPECTED_APP_ID = "collab-doodle";
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
        size_t read = fread(buffer, 1, sizeof(buffer), file);
        if (read > 0)
            sha256Update(state, buffer, (int)read);
        if (read < sizeof(buffer))
            break;
    }
    bool ok = ferror(file) == 0;
    fclose(file);
    if (!ok)
        return false;
    sha256Final(state, outHex);
    return true;
}

static void skipJsonWhitespace(const char *&ptr)
{
    while (*ptr == ' ' || *ptr == '\t' || *ptr == '\r' || *ptr == '\n')
        ++ptr;
}

static bool appendJsonByte(char *out, size_t outSize, size_t &length, char value)
{
    if (!out || outSize == 0 || length + 1 >= outSize)
        return false;
    out[length++] = value;
    out[length] = '\0';
    return true;
}

static bool parseJsonQuotedString(const char *&ptr, char *out, size_t outSize)
{
    if (!ptr || *ptr != '"' || !out || outSize == 0)
        return false;
    ++ptr;
    size_t length = 0;
    out[0] = '\0';
    while (*ptr && *ptr != '"')
    {
        unsigned char value = (unsigned char)*ptr++;
        if (value < 0x20)
            return false;
        if (value != '\\')
        {
            if (!appendJsonByte(out, outSize, length, (char)value))
                return false;
            continue;
        }

        char escaped = *ptr++;
        if (!escaped)
            return false;
        if (escaped == '"' || escaped == '\\' || escaped == '/')
            value = (unsigned char)escaped;
        else if (escaped == 'b') value = '\b';
        else if (escaped == 'f') value = '\f';
        else if (escaped == 'n') value = '\n';
        else if (escaped == 'r') value = '\r';
        else if (escaped == 't') value = '\t';
        else if (escaped == 'u')
        {
            unsigned int codepoint = 0;
            for (int i = 0; i < 4; ++i)
            {
                unsigned char digit = (unsigned char)*ptr++;
                if (!isxdigit(digit))
                    return false;
                codepoint <<= 4;
                codepoint |= digit >= '0' && digit <= '9' ? digit - '0' :
                             digit >= 'a' && digit <= 'f' ? digit - 'a' + 10 : digit - 'A' + 10;
            }
            value = (codepoint >= 0x20 && codepoint <= 0x7e) ? (unsigned char)codepoint : '?';
        }
        else
            return false;
        if (!appendJsonByte(out, outSize, length, (char)value))
            return false;
    }
    if (*ptr != '"')
        return false;
    ++ptr;
    return true;
}

static bool skipJsonValue(const char *&ptr, int depth);

static bool skipJsonObject(const char *&ptr, int depth)
{
    if (*ptr++ != '{' || depth > 12)
        return false;
    skipJsonWhitespace(ptr);
    if (*ptr == '}')
    {
        ++ptr;
        return true;
    }
    while (*ptr)
    {
        char key[96];
        if (!parseJsonQuotedString(ptr, key, sizeof(key)))
            return false;
        skipJsonWhitespace(ptr);
        if (*ptr++ != ':')
            return false;
        if (!skipJsonValue(ptr, depth + 1))
            return false;
        skipJsonWhitespace(ptr);
        if (*ptr == '}')
        {
            ++ptr;
            return true;
        }
        if (*ptr++ != ',')
            return false;
        skipJsonWhitespace(ptr);
    }
    return false;
}

static bool skipJsonArray(const char *&ptr, int depth)
{
    if (*ptr++ != '[' || depth > 12)
        return false;
    skipJsonWhitespace(ptr);
    if (*ptr == ']')
    {
        ++ptr;
        return true;
    }
    while (*ptr)
    {
        if (!skipJsonValue(ptr, depth + 1))
            return false;
        skipJsonWhitespace(ptr);
        if (*ptr == ']')
        {
            ++ptr;
            return true;
        }
        if (*ptr++ != ',')
            return false;
        skipJsonWhitespace(ptr);
    }
    return false;
}

static bool skipJsonNumber(const char *&ptr)
{
    const char *start = ptr;
    if (*ptr == '-') ++ptr;
    if (*ptr == '0')
        ++ptr;
    else
    {
        if (!isdigit((unsigned char)*ptr)) return false;
        while (isdigit((unsigned char)*ptr)) ++ptr;
    }
    if (*ptr == '.')
    {
        ++ptr;
        if (!isdigit((unsigned char)*ptr)) return false;
        while (isdigit((unsigned char)*ptr)) ++ptr;
    }
    if (*ptr == 'e' || *ptr == 'E')
    {
        ++ptr;
        if (*ptr == '+' || *ptr == '-') ++ptr;
        if (!isdigit((unsigned char)*ptr)) return false;
        while (isdigit((unsigned char)*ptr)) ++ptr;
    }
    return ptr > start;
}

static bool skipJsonValue(const char *&ptr, int depth)
{
    if (depth > 12)
        return false;
    skipJsonWhitespace(ptr);
    if (*ptr == '"')
    {
        char discarded[512];
        return parseJsonQuotedString(ptr, discarded, sizeof(discarded));
    }
    if (*ptr == '{') return skipJsonObject(ptr, depth);
    if (*ptr == '[') return skipJsonArray(ptr, depth);
    if (strncmp(ptr, "true", 4) == 0) { ptr += 4; return true; }
    if (strncmp(ptr, "false", 5) == 0) { ptr += 5; return true; }
    if (strncmp(ptr, "null", 4) == 0) { ptr += 4; return true; }
    return skipJsonNumber(ptr);
}

static bool findTopLevelJsonValue(const char *text, const char *wantedKey, const char *&value)
{
    if (!text || !wantedKey)
        return false;
    const char *ptr = text;
    skipJsonWhitespace(ptr);
    if (*ptr++ != '{')
        return false;
    skipJsonWhitespace(ptr);
    const char *match = NULL;
    if (*ptr == '}')
        ++ptr;
    else
    {
        while (*ptr)
        {
            char key[96];
            if (!parseJsonQuotedString(ptr, key, sizeof(key)))
                return false;
            skipJsonWhitespace(ptr);
            if (*ptr++ != ':')
                return false;
            skipJsonWhitespace(ptr);
            const char *candidate = ptr;
            if (!skipJsonValue(ptr, 1))
                return false;
            if (strcmp(key, wantedKey) == 0)
            {
                if (match)
                    return false;
                match = candidate;
            }
            skipJsonWhitespace(ptr);
            if (*ptr == '}')
            {
                ++ptr;
                break;
            }
            if (*ptr++ != ',')
                return false;
            skipJsonWhitespace(ptr);
        }
    }
    skipJsonWhitespace(ptr);
    if (*ptr != '\0' || !match)
        return false;
    value = match;
    return true;
}

static bool parseJsonString(const char *text, const char *key, char *out, size_t outSize)
{
    const char *value = NULL;
    if (!findTopLevelJsonValue(text, key, value))
        return false;
    return parseJsonQuotedString(value, out, outSize);
}

static bool parseJsonPositiveInt(const char *text, const char *key, int &result)
{
    const char *value = NULL;
    if (!findTopLevelJsonValue(text, key, value) || !isdigit((unsigned char)*value))
        return false;
    unsigned long long parsed = 0;
    while (isdigit((unsigned char)*value))
    {
        parsed = parsed * 10 + (*value++ - '0');
        if (parsed > (unsigned long long)INT_MAX)
            return false;
    }
    skipJsonWhitespace(value);
    if (*value != ',' && *value != '}')
        return false;
    if (parsed == 0)
        return false;
    result = (int)parsed;
    return true;
}

static bool parseVersion(const char *version, int parts[3])
{
    if (!version)
        return false;
    const char *ptr = version;
    for (int i = 0; i < 3; ++i)
    {
        if (!isdigit((unsigned char)*ptr))
            return false;
        int value = 0;
        while (isdigit((unsigned char)*ptr))
        {
            value = value * 10 + (*ptr++ - '0');
            if (value > 999999)
                return false;
        }
        parts[i] = value;
        if (i < 2)
        {
            if (*ptr++ != '.')
                return false;
        }
    }
    return *ptr == '\0';
}

static int compareVersionParts(const int leftParts[3], const int rightParts[3])
{

    for (int i = 0; i < 3; i++)
    {
        if (leftParts[i] > rightParts[i])
            return 1;
        if (leftParts[i] < rightParts[i])
            return -1;
    }
    return 0;
}

static bool isHexSha256(const char *digest)
{
    if (!digest || strlen(digest) != 64)
        return false;
    for (int i = 0; i < 64; ++i)
    {
        if (!isxdigit((unsigned char)digest[i]))
            return false;
    }
    return true;
}

static bool isSafeArtifactName(const char *name)
{
    if (!name || !name[0])
        return false;
    for (const unsigned char *ptr = (const unsigned char *)name; *ptr; ++ptr)
    {
        if (!(isalnum(*ptr) || *ptr == '.' || *ptr == '-' || *ptr == '_'))
            return false;
    }
    return true;
}

static bool artifactNameMatchesType(const char *name, const char *packageType)
{
    if (!name || !packageType)
        return false;
    const char *extension = strcmp(packageType, "cia") == 0 ? ".cia" : ".3dsx";
    size_t nameLength = strlen(name);
    size_t extensionLength = strlen(extension);
    return nameLength > extensionLength && strcasecmp(name + nameLength - extensionLength, extension) == 0;
}

struct ManifestSink
{
    char *buffer;
    size_t capacity;
    size_t length;
};

static bool appendManifestBody(const unsigned char *data, size_t length, void *userData)
{
    ManifestSink *sink = (ManifestSink *)userData;
    if (!sink || sink->length > sink->capacity || length > sink->capacity - sink->length)
        return false;
    for (size_t i = 0; i < length; ++i)
    {
        if (data[i] == 0)
            return false;
    }
    memcpy(sink->buffer + sink->length, data, length);
    sink->length += length;
    sink->buffer[sink->length] = '\0';
    return true;
}

bool Updater::fetchManifest(const char *serverDomain, const char *httpPort, const char *currentVersion, UpdateManifest &manifest)
{
    return fetchManifest(serverDomain, httpPort, currentVersion, "3dsx", manifest);
}

bool Updater::fetchManifest(const char *serverDomain, const char *httpPort, const char *currentVersion,
                            const char *packageType, UpdateManifest &manifest)
{
    setUpdateError("");
    memset(&manifest, 0, sizeof(manifest));

    const char *cleanPackage = (packageType && strcmp(packageType, "cia") == 0) ? "cia" : "3dsx";
    char path[96];
    snprintf(path, sizeof(path), "/api/updates/latest?package=%s", cleanPackage);

    char body[8192];
    ManifestSink sink = {body, sizeof(body) - 1, 0};
    body[0] = '\0';
    HttpsResponse response;
    char httpsError[160];
    if (!HttpsClient::get(serverDomain, httpPort, path, sizeof(body) - 1,
                          appendManifestBody, &sink, response,
                          httpsError, sizeof(httpsError)))
    {
        setUpdateError("MANIFEST HTTPS: %s", httpsError);
        return false;
    }

    if (response.bodyBytes != sink.length)
    {
        setUpdateError("MANIFEST BODY LENGTH MISMATCH");
        return false;
    }

    char appId[32];
    if (!parseJsonString(body, "appId", appId, sizeof(appId)) || strcmp(appId, EXPECTED_APP_ID) != 0 ||
        !parseJsonString(body, "latestVersion", manifest.latestVersion, sizeof(manifest.latestVersion)))
    {
        setUpdateError("MANIFEST JSON INVALID");
        return false;
    }

    int latestParts[3];
    int currentParts[3];
    if (!parseVersion(manifest.latestVersion, latestParts) || !parseVersion(currentVersion, currentParts))
    {
        setUpdateError("MANIFEST VERSION INVALID");
        return false;
    }

    parseJsonString(body, "releaseNotes", manifest.releaseNotes, sizeof(manifest.releaseNotes));
    parseJsonString(body, "artifactUrl", manifest.artifactUrl, sizeof(manifest.artifactUrl));
    parseJsonString(body, "artifactName", manifest.artifactName, sizeof(manifest.artifactName));
    parseJsonString(body, "artifactType", manifest.artifactType, sizeof(manifest.artifactType));
    parseJsonString(body, "sha256", manifest.sha256, sizeof(manifest.sha256));
    parseJsonPositiveInt(body, "artifactSize", manifest.artifactSize);
    manifest.available = compareVersionParts(latestParts, currentParts) > 0;

    if (manifest.available &&
        (!manifest.artifactUrl[0] || !isSafeArtifactName(manifest.artifactName) ||
         !artifactNameMatchesType(manifest.artifactName, cleanPackage) ||
         strcmp(manifest.artifactType, cleanPackage) != 0 || !isHexSha256(manifest.sha256) ||
         manifest.artifactSize <= 0 || manifest.artifactSize > MAX_ARTIFACT_BYTES))
    {
        setUpdateError("MANIFEST ARTIFACT INVALID");
        memset(&manifest, 0, sizeof(manifest));
        return false;
    }
    return true;
}

static bool artifactPathFromUrl(const char *artifactUrl, const char *serverDomain, const char *httpsPort,
                                const char *artifactName, char *outPath, size_t outSize)
{
    if (!artifactUrl || !serverDomain || !httpsPort || !artifactName || !outPath || outSize == 0)
        return false;
    for (const unsigned char *ptr = (const unsigned char *)artifactUrl; *ptr; ++ptr)
    {
        if (*ptr <= 0x20 || *ptr == 0x7f || *ptr == '\\' || *ptr == '#')
            return false;
    }

    const char *path = artifactUrl;
    if (strncasecmp(artifactUrl, "https://", 8) == 0)
    {
        const char *authority = artifactUrl + 8;
        const char *pathStart = strchr(authority, '/');
        if (!pathStart || pathStart == authority)
            return false;
        size_t authorityLength = (size_t)(pathStart - authority);
        if (authorityLength >= 320 || memchr(authority, '@', authorityLength))
            return false;
        char authorityCopy[320];
        memcpy(authorityCopy, authority, authorityLength);
        authorityCopy[authorityLength] = '\0';

        char *host = authorityCopy;
        const char *port = "443";
        char *colon = strrchr(authorityCopy, ':');
        if (colon)
        {
            if (strchr(authorityCopy, ':') != colon)
                return false;
            *colon = '\0';
            port = colon + 1;
        }
        if (!host[0] || !port[0] || strcasecmp(host, serverDomain) != 0 || strcmp(port, httpsPort) != 0)
            return false;
        path = pathStart;
    }
    else if (strstr(artifactUrl, "://"))
        return false;

    char expectedPath[160];
    int expectedLength = snprintf(expectedPath, sizeof(expectedPath), "/updates/%s", artifactName);
    if (expectedLength <= 0 || expectedLength >= (int)sizeof(expectedPath) ||
        strncmp(path, expectedPath, (size_t)expectedLength) != 0 ||
        (path[expectedLength] != '\0' && path[expectedLength] != '?'))
        return false;
    int written = snprintf(outPath, outSize, "%s", path);
    return written > 0 && written < (int)outSize;
}

struct DownloadSink
{
    FILE *file;
    size_t written;
    int expected;
    UpdateProgressCallback progress;
    void *progressUserData;
};

static bool writeDownloadBody(const unsigned char *data, size_t length, void *userData)
{
    DownloadSink *sink = (DownloadSink *)userData;
    if (!sink || !sink->file || length > (size_t)0x7fffffff ||
        sink->written > (size_t)0x7fffffff - length)
        return false;
    if (fwrite(data, 1, length, sink->file) != length)
        return false;
    sink->written += length;
    if (sink->progress)
        sink->progress((int)sink->written, sink->expected, sink->progressUserData);
    return true;
}

static bool makeSiblingPath(const char *targetPath, const char *suffix, char *out, size_t outSize)
{
    int written = snprintf(out, outSize, "%s%s", targetPath && targetPath[0] ? targetPath : UPDATE_FINAL_PATH, suffix);
    return written > 0 && written < (int)outSize;
}

static bool replaceWithBackup(const char *partPath, const char *targetPath)
{
    if (!targetPath || !targetPath[0])
        targetPath = UPDATE_FINAL_PATH;

    char backupPath[320];
    if (!makeSiblingPath(targetPath, ".bak", backupPath, sizeof(backupPath)))
    {
        setUpdateError("UPDATE TARGET PATH TOO LONG");
        return false;
    }

    struct stat targetInfo;
    bool hadTarget = stat(targetPath, &targetInfo) == 0;
    if (hadTarget && remove(backupPath) != 0 && errno != ENOENT)
    {
        setUpdateError("REMOVE OLD BACKUP ERR %d", errno);
        return false;
    }
    if (hadTarget && rename(targetPath, backupPath) != 0)
    {
        setUpdateError("BACKUP UPDATE FILE ERR %d", errno);
        return false;
    }

    if (rename(partPath, targetPath) == 0)
    {
        remove(backupPath);
        return true;
    }

    int replaceError = errno;
    if (hadTarget && rename(backupPath, targetPath) != 0)
        setUpdateError("REPLACE ERR %d; RESTORE ERR %d", replaceError, errno);
    else
        setUpdateError("REPLACE UPDATE FILE ERR %d", replaceError);
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
    if (expectedTitleId == 0)
    {
        setUpdateError("INSTALLED TITLE ID UNAVAILABLE");
        return UPDATE_DOWNLOAD_INSTALL_FAILED;
    }

    FILE *file = fopen(ciaPath, "rb");
    if (!file)
    {
        setUpdateError("OPEN CIA FILE ERR %d", errno);
        return UPDATE_DOWNLOAD_INSTALL_FAILED;
    }

    if (fseek(file, 0, SEEK_END) != 0)
    {
        setUpdateError("CIA SEEK ERR %d", errno);
        fclose(file);
        return UPDATE_DOWNLOAD_INSTALL_FAILED;
    }
    long fileSize = ftell(file);
    if (fileSize <= 0 || fseek(file, 0, SEEK_SET) != 0)
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
        amExit();
        fclose(file);
        return UPDATE_DOWNLOAD_INSTALL_FAILED;
    }
    if (info.titleID != (u64)expectedTitleId)
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
            setUpdateError("CIA READ ERR %d", ferror(file) ? errno : 0);
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
    if (ferror(file))
    {
        setUpdateError("CIA READ ERR %d", errno);
        ok = false;
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
    if (R_FAILED(ret))
        AM_CancelCIAInstall(ciaHandle);
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
    setUpdateError("");
    if (!manifest.available)
        return UPDATE_DOWNLOAD_NO_UPDATE;
    if (!manifest.artifactUrl[0] || !isSafeArtifactName(manifest.artifactName) ||
        (strcmp(manifest.artifactType, "cia") != 0 && strcmp(manifest.artifactType, "3dsx") != 0) ||
        !artifactNameMatchesType(manifest.artifactName, manifest.artifactType))
        return UPDATE_DOWNLOAD_NO_ARTIFACT;
    if (!isHexSha256(manifest.sha256))
    {
        setUpdateError("ARTIFACT CHECKSUM INVALID");
        return UPDATE_DOWNLOAD_CHECKSUM_MISMATCH;
    }
    if (manifest.artifactSize <= 0 || manifest.artifactSize > MAX_ARTIFACT_BYTES)
    {
        setUpdateError("ARTIFACT SIZE INVALID");
        return UPDATE_DOWNLOAD_SIZE_MISMATCH;
    }

    char downloadPath[320];
    if (!artifactPathFromUrl(manifest.artifactUrl, serverDomain, httpPort,
                             manifest.artifactName, downloadPath, sizeof(downloadPath)))
    {
        setUpdateError("ARTIFACT URL MUST BE SAME-ORIGIN HTTPS");
        return UPDATE_DOWNLOAD_NO_ARTIFACT;
    }

    char partPath[320];
    if (targetPath && strncmp(targetPath, "sdmc:/cias/", 11) == 0)
        mkdir("sdmc:/cias", 0777);
    if (!makeSiblingPath(targetPath, ".part", partPath, sizeof(partPath)))
    {
        setUpdateError("UPDATE TARGET PATH TOO LONG");
        return UPDATE_DOWNLOAD_FAILED;
    }
    remove(partPath);

    FILE *file = fopen(partPath, "wb");
    if (!file)
    {
        setUpdateError("OPEN UPDATE FILE ERR %d", errno);
        return UPDATE_DOWNLOAD_FAILED;
    }

    DownloadSink sink = {file, 0, manifest.artifactSize, progress, userData};
    HttpsResponse response;
    char httpsError[160];
    bool downloaded = HttpsClient::get(serverDomain, httpPort, downloadPath,
                                       (size_t)manifest.artifactSize,
                                       writeDownloadBody, &sink, response,
                                       httpsError, sizeof(httpsError));
    int closeResult = fclose(file);
    if (!downloaded)
    {
        setUpdateError("DOWNLOAD HTTPS: %s", httpsError);
        remove(partPath);
        return UPDATE_DOWNLOAD_FAILED;
    }
    if (closeResult != 0)
    {
        setUpdateError("CLOSE UPDATE FILE ERR %d", errno);
        remove(partPath);
        return UPDATE_DOWNLOAD_FAILED;
    }

    if (sink.written != (size_t)manifest.artifactSize)
    {
        setUpdateError("DOWNLOAD SIZE %lu EXPECTED %d", (unsigned long)sink.written, manifest.artifactSize);
        remove(partPath);
        return UPDATE_DOWNLOAD_SIZE_MISMATCH;
    }

    char digest[65];
    if (!fileSha256(partPath, digest) || strcasecmp(digest, manifest.sha256) != 0)
    {
        setUpdateError("DOWNLOAD CHECKSUM MISMATCH");
        remove(partPath);
        return UPDATE_DOWNLOAD_CHECKSUM_MISMATCH;
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
