#ifndef HTTPS_CLIENT_H
#define HTTPS_CLIENT_H

#include <stddef.h>

struct HttpsResponse
{
    int statusCode;
    long long contentLength;
    size_t bodyBytes;
    bool chunked;
};

typedef bool (*HttpsBodyCallback)(const unsigned char* data, size_t length, void* userData);

// Minimal same-origin HTTPS/1.1 client used by the updater. It deliberately
// supports GET only and never falls back to plaintext HTTP.
class HttpsClient
{
public:
    static bool get(const char* host, const char* port, const char* path,
                    size_t maxBodyBytes, HttpsBodyCallback bodyCallback,
                    void* userData, HttpsResponse& response,
                    char* error, size_t errorSize, int maxRedirects = 3);
};

#endif
