#include "https_client.h"
#include "tls_stream.h"

#include <3ds.h>
#include <algorithm>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <new>

#ifndef APP_VERSION
#define APP_VERSION "unknown"
#endif

namespace
{
const size_t MAX_HEADER_BYTES = 16 * 1024;
const size_t MAX_CHUNK_LINE_BYTES = 128;
const size_t MAX_TRAILER_BYTES = 8 * 1024;
// The updater uses the same software TLS stack and must remain usable on an
// original 3DS/2DS even when certificate verification takes several seconds.
const int CONNECT_TIMEOUT_MS = 30000;
const int IO_IDLE_TIMEOUT_MS = 20000;
const int REQUEST_TIMEOUT_MS = 5 * 60 * 1000;
const int MAX_INFORMATIONAL_RESPONSES = 4;
const long long RETRY_SLEEP_NS = 5LL * 1000LL * 1000LL;

void setError(char* error, size_t errorSize, const char* message)
{
    if (!error || errorSize == 0)
        return;
    snprintf(error, errorSize, "%s", message ? message : "HTTPS request failed");
}

bool timedOut(u64 lastProgress)
{
    return osGetTime() - lastProgress >= (u64)IO_IDLE_TIMEOUT_MS;
}

bool hasUnsafeRequestChars(const char* text)
{
    return !text || strchr(text, '\r') || strchr(text, '\n');
}

bool isValidHost(const char* host)
{
    if (!host || !host[0] || strlen(host) > 253)
        return false;
    for (const unsigned char* ptr = (const unsigned char*)host; *ptr; ++ptr)
    {
        if (!(isalnum(*ptr) || *ptr == '.' || *ptr == '-'))
            return false;
    }
    return true;
}

bool isValidPort(const char* port)
{
    if (!port || !port[0])
        return false;
    unsigned long value = 0;
    for (const unsigned char* ptr = (const unsigned char*)port; *ptr; ++ptr)
    {
        if (!isdigit(*ptr))
            return false;
        value = value * 10 + (*ptr - '0');
        if (value > 65535)
            return false;
    }
    return value > 0;
}

bool isValidRequestPath(const std::string& path)
{
    if (path.empty() || path[0] != '/' || (path.size() > 1 && path[1] == '/') || path.find('#') != std::string::npos)
        return false;
    for (size_t i = 0; i < path.size(); ++i)
    {
        const unsigned char value = (unsigned char)path[i];
        if (value <= 0x20 || value == 0x7f)
            return false;
    }
    return true;
}

std::string lowerCopy(const std::string& value)
{
    std::string lower(value);
    for (size_t i = 0; i < lower.size(); ++i)
        lower[i] = (char)tolower((unsigned char)lower[i]);
    return lower;
}

std::string trimCopy(const std::string& value)
{
    size_t first = 0;
    while (first < value.size() && isspace((unsigned char)value[first]))
        ++first;
    size_t last = value.size();
    while (last > first && isspace((unsigned char)value[last - 1]))
        --last;
    return value.substr(first, last - first);
}

bool sameText(const std::string& left, const char* right)
{
    return lowerCopy(left) == lowerCopy(right ? std::string(right) : std::string());
}

bool writeAll(TlsStream& stream, const char* data, size_t length, char* error, size_t errorSize)
{
    size_t offset = 0;
    u64 lastProgress = osGetTime();
    while (offset < length)
    {
        size_t written = 0;
        TlsStream::IoResult result = stream.write(data + offset, length - offset, written);
        if (result == TlsStream::IO_OK)
        {
            if (written == 0)
            {
                setError(error, errorSize, "TLS write made no progress");
                return false;
            }
            offset += written;
            lastProgress = osGetTime();
            continue;
        }
        if (result == TlsStream::IO_WOULD_BLOCK)
        {
            if (timedOut(lastProgress))
            {
                setError(error, errorSize, "HTTPS write timed out");
                return false;
            }
            svcSleepThread(RETRY_SLEEP_NS);
            continue;
        }
        setError(error, errorSize, result == TlsStream::IO_CLOSED ? "HTTPS connection closed while writing" : stream.lastError());
        return false;
    }
    return true;
}

bool parseContentLength(const std::string& value, long long& parsed)
{
    const std::string clean = trimCopy(value);
    if (clean.empty())
        return false;
    for (size_t i = 0; i < clean.size(); ++i)
    {
        if (!isdigit((unsigned char)clean[i]))
            return false;
    }
    char* end = NULL;
    unsigned long long result = strtoull(clean.c_str(), &end, 10);
    if (!end || *end != '\0' || result > 0x7fffffffffffffffULL)
        return false;
    parsed = (long long)result;
    return true;
}

bool parseStatusLine(const std::string& line, int& statusCode)
{
    const char* prefixes[] = {"HTTP/1.0 ", "HTTP/1.1 "};
    size_t statusStart = std::string::npos;
    for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); ++i)
    {
        const size_t length = strlen(prefixes[i]);
        if (line.compare(0, length, prefixes[i]) == 0)
        {
            statusStart = length;
            break;
        }
    }
    if (statusStart == std::string::npos || statusStart + 3 > line.size())
        return false;
    for (size_t i = 0; i < 3; ++i)
    {
        if (!isdigit((unsigned char)line[statusStart + i]))
            return false;
    }
    if (statusStart + 3 < line.size() && line[statusStart + 3] != ' ')
        return false;
    for (size_t i = statusStart + 3; i < line.size(); ++i)
    {
        const unsigned char value = (unsigned char)line[i];
        if (value < 0x20 || value == 0x7f)
            return false;
    }
    statusCode = (line[statusStart] - '0') * 100 +
                 (line[statusStart + 1] - '0') * 10 +
                 (line[statusStart + 2] - '0');
    return statusCode >= 100 && statusCode <= 599;
}

bool parseHeaders(const std::string& headerText, HttpsResponse& response,
                  std::string& location, char* error, size_t errorSize)
{
    response.statusCode = 0;
    response.contentLength = -1;
    response.bodyBytes = 0;
    response.chunked = false;
    location.clear();
    bool sawTransferEncoding = false;
    bool sawLocation = false;

    size_t lineEnd = headerText.find("\r\n");
    if (lineEnd == std::string::npos)
    {
        setError(error, errorSize, "Malformed HTTPS status line");
        return false;
    }
    std::string statusLine = headerText.substr(0, lineEnd);
    if (!parseStatusLine(statusLine, response.statusCode))
    {
        setError(error, errorSize, "Invalid HTTPS status code");
        return false;
    }

    size_t cursor = lineEnd + 2;
    while (cursor < headerText.size())
    {
        lineEnd = headerText.find("\r\n", cursor);
        if (lineEnd == std::string::npos)
            break;
        if (lineEnd == cursor)
            break;
        std::string line = headerText.substr(cursor, lineEnd - cursor);
        cursor = lineEnd + 2;
        size_t colon = line.find(':');
        if (colon == std::string::npos || colon == 0 || isspace((unsigned char)line[0]))
        {
            setError(error, errorSize, "Malformed HTTPS header line");
            return false;
        }
        for (size_t i = 0; i < colon; ++i)
        {
            const unsigned char value = (unsigned char)line[i];
            if (!(isalnum(value) || strchr("!#$%&'*+-.^_`|~", value)))
            {
                setError(error, errorSize, "Invalid HTTPS header name");
                return false;
            }
        }
        const std::string name = lowerCopy(trimCopy(line.substr(0, colon)));
        const std::string value = trimCopy(line.substr(colon + 1));
        for (size_t i = 0; i < value.size(); ++i)
        {
            const unsigned char byte = (unsigned char)value[i];
            if ((byte < 0x20 && byte != '\t') || byte == 0x7f)
            {
                setError(error, errorSize, "Invalid HTTPS header value");
                return false;
            }
        }
        if (name == "content-length")
        {
            long long parsed = -1;
            if (!parseContentLength(value, parsed) ||
                (response.contentLength >= 0 && response.contentLength != parsed))
            {
                setError(error, errorSize, "Invalid HTTPS Content-Length");
                return false;
            }
            response.contentLength = parsed;
        }
        else if (name == "transfer-encoding")
        {
            if (sawTransferEncoding || lowerCopy(value) != "chunked")
            {
                setError(error, errorSize, "Unsupported HTTPS Transfer-Encoding");
                return false;
            }
            sawTransferEncoding = true;
            response.chunked = true;
        }
        else if (name == "location")
        {
            if (sawLocation && location != value)
            {
                setError(error, errorSize, "Conflicting HTTPS Location headers");
                return false;
            }
            sawLocation = true;
            location = value;
        }
    }

    if (response.chunked && response.contentLength >= 0)
    {
        setError(error, errorSize, "Conflicting HTTPS body framing headers");
        return false;
    }
    if (response.chunked)
        response.contentLength = -1;
    return true;
}

class BodyDecoder
{
public:
    BodyDecoder(const HttpsResponse& response, size_t maximum,
                HttpsBodyCallback callback, void* userData,
                char* error, size_t errorSize)
        : response_(response), maximum_(maximum), callback_(callback), userData_(userData),
          error_(error), errorSize_(errorSize), received_(0), chunkRemaining_(0),
          state_(response.chunked ? CHUNK_SIZE : IDENTITY),
          done_(!response.chunked && response.contentLength == 0)
    {
    }

    bool feed(const unsigned char* data, size_t length)
    {
        if (done_ && length > 0)
        {
            setError(error_, errorSize_, "Unexpected bytes after HTTPS body");
            return false;
        }
        if (!response_.chunked)
        {
            size_t accepted = length;
            if (response_.contentLength >= 0)
            {
                const unsigned long long expected = (unsigned long long)response_.contentLength;
                if ((unsigned long long)received_ + length > expected)
                {
                    setError(error_, errorSize_, "HTTPS body exceeds Content-Length");
                    return false;
                }
                accepted = length;
            }
            if (!emit(data, accepted))
                return false;
            if (response_.contentLength >= 0 && (unsigned long long)received_ == (unsigned long long)response_.contentLength)
                done_ = true;
            return true;
        }

        pending_.append((const char*)data, length);
        while (!done_)
        {
            if (state_ == CHUNK_SIZE)
            {
                size_t end = pending_.find("\r\n");
                if (end == std::string::npos)
                {
                    if (pending_.size() > MAX_CHUNK_LINE_BYTES)
                    {
                        setError(error_, errorSize_, "HTTPS chunk-size line is too long");
                        return false;
                    }
                    break;
                }
                if (end == 0 || end > MAX_CHUNK_LINE_BYTES)
                {
                    setError(error_, errorSize_, "Invalid HTTPS chunk size");
                    return false;
                }
                std::string line = pending_.substr(0, end);
                pending_.erase(0, end + 2);
                size_t extension = line.find(';');
                if (extension != std::string::npos)
                    line.erase(extension);
                line = trimCopy(line);
                for (size_t i = 0; i < line.size(); ++i)
                {
                    if (!isxdigit((unsigned char)line[i]))
                    {
                        setError(error_, errorSize_, "Invalid HTTPS chunk size");
                        return false;
                    }
                }
                char* parseEnd = NULL;
                unsigned long long size = strtoull(line.c_str(), &parseEnd, 16);
                if (line.empty() || !parseEnd || *parseEnd != '\0' || size > (unsigned long long)maximum_)
                {
                    setError(error_, errorSize_, "Invalid or oversized HTTPS chunk");
                    return false;
                }
                chunkRemaining_ = (size_t)size;
                state_ = chunkRemaining_ == 0 ? CHUNK_TRAILERS : CHUNK_DATA;
            }
            else if (state_ == CHUNK_DATA)
            {
                if (pending_.empty())
                    break;
                size_t count = std::min(chunkRemaining_, pending_.size());
                if (!emit((const unsigned char*)pending_.data(), count))
                    return false;
                pending_.erase(0, count);
                chunkRemaining_ -= count;
                if (chunkRemaining_ == 0)
                    state_ = CHUNK_DATA_END;
            }
            else if (state_ == CHUNK_DATA_END)
            {
                if (pending_.size() < 2)
                    break;
                if (pending_.compare(0, 2, "\r\n") != 0)
                {
                    setError(error_, errorSize_, "Malformed HTTPS chunk terminator");
                    return false;
                }
                pending_.erase(0, 2);
                state_ = CHUNK_SIZE;
            }
            else if (state_ == CHUNK_TRAILERS)
            {
                if (pending_.size() >= 2 && pending_.compare(0, 2, "\r\n") == 0)
                {
                    pending_.erase(0, 2);
                    done_ = true;
                    break;
                }
                size_t end = pending_.find("\r\n\r\n");
                if (end == std::string::npos)
                {
                    if (pending_.size() > MAX_TRAILER_BYTES)
                    {
                        setError(error_, errorSize_, "HTTPS trailers are too large");
                        return false;
                    }
                    break;
                }
                if (end + 4 > MAX_TRAILER_BYTES)
                {
                    setError(error_, errorSize_, "HTTPS trailers are too large");
                    return false;
                }
                pending_.erase(0, end + 4);
                done_ = true;
            }
        }
        if (done_ && !pending_.empty())
        {
            setError(error_, errorSize_, "Unexpected bytes after HTTPS body");
            return false;
        }
        return true;
    }

    bool finishOnClose()
    {
        if (response_.chunked)
        {
            if (!done_)
            {
                setError(error_, errorSize_, "Truncated chunked HTTPS body");
                return false;
            }
            return true;
        }
        if (response_.contentLength >= 0 && (unsigned long long)received_ != (unsigned long long)response_.contentLength)
        {
            setError(error_, errorSize_, "Truncated HTTPS body");
            return false;
        }
        done_ = true;
        return true;
    }

    bool done() const { return done_; }
    size_t received() const { return received_; }

private:
    enum State
    {
        IDENTITY,
        CHUNK_SIZE,
        CHUNK_DATA,
        CHUNK_DATA_END,
        CHUNK_TRAILERS
    };

    bool emit(const unsigned char* data, size_t length)
    {
        if (length == 0)
            return true;
        if (length > maximum_ || received_ > maximum_ - length)
        {
            setError(error_, errorSize_, "HTTPS body exceeds configured limit");
            return false;
        }
        if (callback_ && !callback_(data, length, userData_))
        {
            setError(error_, errorSize_, "HTTPS body consumer failed");
            return false;
        }
        received_ += length;
        return true;
    }

    HttpsResponse response_;
    size_t maximum_;
    HttpsBodyCallback callback_;
    void* userData_;
    char* error_;
    size_t errorSize_;
    size_t received_;
    size_t chunkRemaining_;
    State state_;
    bool done_;
    std::string pending_;
};

bool isRedirectStatus(int status)
{
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

bool resolveRedirect(const std::string& location, const char* expectedHost,
                     const char* expectedPort, std::string& nextPath,
                     char* error, size_t errorSize)
{
    if (location.empty() || location.find('\r') != std::string::npos ||
        location.find('\n') != std::string::npos || location.find('#') != std::string::npos)
    {
        setError(error, errorSize, "HTTPS redirect has no valid Location");
        return false;
    }
    if (location[0] == '/')
    {
        if (!isValidRequestPath(location))
        {
            setError(error, errorSize, "Invalid same-origin HTTPS redirect path");
            return false;
        }
        nextPath = location;
        return true;
    }

    const std::string prefix = "https://";
    if (lowerCopy(location.substr(0, std::min(prefix.size(), location.size()))) != prefix)
    {
        setError(error, errorSize, "HTTPS redirect must remain HTTPS and same-origin");
        return false;
    }
    size_t authorityStart = prefix.size();
    size_t pathStart = location.find('/', authorityStart);
    size_t queryStart = location.find('?', authorityStart);
    size_t authorityEnd = std::min(pathStart == std::string::npos ? location.size() : pathStart,
                                   queryStart == std::string::npos ? location.size() : queryStart);
    std::string authority = location.substr(authorityStart, authorityEnd - authorityStart);
    if (authority.empty() || authority.find('@') != std::string::npos)
    {
        setError(error, errorSize, "Invalid HTTPS redirect authority");
        return false;
    }
    std::string host = authority;
    std::string port = "443";
    size_t colon = authority.rfind(':');
    if (colon != std::string::npos)
    {
        if (authority.find(':') != colon)
        {
            setError(error, errorSize, "Invalid HTTPS redirect authority");
            return false;
        }
        host = authority.substr(0, colon);
        port = authority.substr(colon + 1);
    }
    const std::string wantedPort = (expectedPort && expectedPort[0]) ? expectedPort : "443";
    if (!isValidHost(host.c_str()) || !isValidPort(port.c_str()) ||
        !sameText(host, expectedHost) || port != wantedPort)
    {
        setError(error, errorSize, "Cross-origin HTTPS redirect rejected");
        return false;
    }
    if (pathStart != std::string::npos)
        nextPath = location.substr(pathStart);
    else if (queryStart != std::string::npos)
        nextPath = "/" + location.substr(queryStart);
    else
        nextPath = "/";
    if (!isValidRequestPath(nextPath))
    {
        setError(error, errorSize, "Invalid same-origin HTTPS redirect path");
        return false;
    }
    return true;
}

enum RequestResult
{
    REQUEST_FAILED,
    REQUEST_COMPLETE,
    REQUEST_REDIRECT
};

RequestResult getOnce(const char* host, const char* port, const std::string& path,
                      size_t maxBodyBytes, HttpsBodyCallback bodyCallback,
                      void* userData, HttpsResponse& response,
                      std::string& redirectLocation, char* error, size_t errorSize)
{
    TlsStream stream;
    if (!stream.connect(host, port, CONNECT_TIMEOUT_MS))
    {
        setError(error, errorSize, stream.lastError());
        return REQUEST_FAILED;
    }

    std::string hostHeader(host);
    if (strcmp(port, "443") != 0)
        hostHeader += ":" + std::string(port);
    char request[1024];
    int requestLength = snprintf(request, sizeof(request),
                                 "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: CollabDoodle/%s\r\nAccept: */*\r\nConnection: close\r\n\r\n",
                                 path.c_str(), hostHeader.c_str(), APP_VERSION);
    if (requestLength <= 0 || requestLength >= (int)sizeof(request) ||
        !writeAll(stream, request, (size_t)requestLength, error, errorSize))
    {
        if (requestLength >= (int)sizeof(request))
            setError(error, errorSize, "HTTPS request path is too long");
        stream.close();
        return REQUEST_FAILED;
    }

    std::string headers;
    bool headersDone = false;
    BodyDecoder* decoder = NULL;
    u64 lastProgress = osGetTime();
    const u64 requestStarted = lastProgress;
    unsigned char buffer[4096];
    bool success = false;
    int informationalResponses = 0;

    while (true)
    {
        if (osGetTime() - requestStarted >= (u64)REQUEST_TIMEOUT_MS)
        {
            setError(error, errorSize, "HTTPS request exceeded total timeout");
            break;
        }
        size_t bytesRead = 0;
        TlsStream::IoResult result = stream.read(buffer, sizeof(buffer), bytesRead);
        if (result == TlsStream::IO_OK)
        {
            if (bytesRead == 0)
            {
                setError(error, errorSize, "TLS read made no progress");
                break;
            }
            lastProgress = osGetTime();
            if (!headersDone)
            {
                headers.append((const char*)buffer, bytesRead);
                bool headerFailure = false;
                while (!headersDone)
                {
                    size_t headerEnd = headers.find("\r\n\r\n");
                    if (headerEnd == std::string::npos)
                    {
                        if (headers.size() > MAX_HEADER_BYTES)
                        {
                            setError(error, errorSize, "HTTPS response headers are too large");
                            headerFailure = true;
                        }
                        break;
                    }
                    if (headerEnd + 4 > MAX_HEADER_BYTES)
                    {
                        setError(error, errorSize, "HTTPS response headers are too large");
                        headerFailure = true;
                        break;
                    }

                    std::string headerBlock = headers.substr(0, headerEnd + 2);
                    headers.erase(0, headerEnd + 4);
                    if (!parseHeaders(headerBlock, response, redirectLocation, error, errorSize))
                    {
                        headerFailure = true;
                        break;
                    }
                    if (response.statusCode >= 100 && response.statusCode < 200)
                    {
                        if (response.statusCode == 101 || ++informationalResponses > MAX_INFORMATIONAL_RESPONSES)
                        {
                            setError(error, errorSize, "Unsupported HTTPS informational response");
                            headerFailure = true;
                            break;
                        }
                        continue;
                    }
                    headersDone = true;
                }
                if (headerFailure)
                    break;
                if (!headersDone)
                    continue;

                if (isRedirectStatus(response.statusCode))
                {
                    stream.close();
                    return REQUEST_REDIRECT;
                }
                if (response.statusCode != 200)
                {
                    char statusError[96];
                    snprintf(statusError, sizeof(statusError), "HTTPS server returned status %d", response.statusCode);
                    setError(error, errorSize, statusError);
                    break;
                }
                if (response.contentLength >= 0 && (unsigned long long)response.contentLength > (unsigned long long)maxBodyBytes)
                {
                    setError(error, errorSize, "HTTPS Content-Length exceeds configured limit");
                    break;
                }

                decoder = new (std::nothrow) BodyDecoder(response, maxBodyBytes, bodyCallback, userData, error, errorSize);
                if (!decoder)
                {
                    setError(error, errorSize, "Out of memory creating HTTPS decoder");
                    break;
                }
                if (!headers.empty() && !decoder->feed((const unsigned char*)headers.data(), headers.size()))
                    break;
                headers.clear();
                if (decoder->done())
                {
                    success = true;
                    break;
                }
                continue;
            }
            if (decoder && !decoder->feed(buffer, bytesRead))
                break;
            if (decoder && decoder->done())
            {
                success = true;
                break;
            }
            continue;
        }
        if (result == TlsStream::IO_WOULD_BLOCK)
        {
            if (timedOut(lastProgress))
            {
                setError(error, errorSize, "HTTPS read timed out");
                break;
            }
            svcSleepThread(RETRY_SLEEP_NS);
            continue;
        }
        if (result == TlsStream::IO_CLOSED)
        {
            if (!headersDone)
                setError(error, errorSize, "HTTPS connection closed before response headers");
            else if (decoder && decoder->finishOnClose())
                success = true;
            break;
        }
        setError(error, errorSize, stream.lastError());
        break;
    }

    if (decoder)
    {
        response.bodyBytes = decoder->received();
        delete decoder;
    }
    stream.close();
    return success ? REQUEST_COMPLETE : REQUEST_FAILED;
}
}

bool HttpsClient::get(const char* host, const char* port, const char* path,
                      size_t maxBodyBytes, HttpsBodyCallback bodyCallback,
                      void* userData, HttpsResponse& response,
                      char* error, size_t errorSize, int maxRedirects)
{
    memset(&response, 0, sizeof(response));
    response.contentLength = -1;
    setError(error, errorSize, "");

    if (hasUnsafeRequestChars(host) || hasUnsafeRequestChars(port) || hasUnsafeRequestChars(path) ||
        !isValidHost(host) || !isValidPort(port) || !isValidRequestPath(path ? std::string(path) : std::string()) ||
        maxBodyBytes == 0 || maxRedirects < 0 || maxRedirects > 10)
    {
        setError(error, errorSize, "Invalid HTTPS request target");
        return false;
    }
    if (!TlsStream::initialize())
    {
        setError(error, errorSize, "TLS trust store or system RNG initialization failed");
        return false;
    }

    std::string currentPath(path);
    for (int redirectCount = 0; redirectCount <= maxRedirects; ++redirectCount)
    {
        std::string location;
        RequestResult result = getOnce(host, port, currentPath, maxBodyBytes,
                                       bodyCallback, userData, response,
                                       location, error, errorSize);
        if (result == REQUEST_COMPLETE)
            return true;
        if (result == REQUEST_FAILED)
            return false;
        if (redirectCount == maxRedirects)
        {
            setError(error, errorSize, "Too many HTTPS redirects");
            return false;
        }
        if (!resolveRedirect(location, host, port, currentPath, error, errorSize))
            return false;
    }
    setError(error, errorSize, "HTTPS redirect handling failed");
    return false;
}
