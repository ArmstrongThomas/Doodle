#include "websocket_client.h"

#include <3ds.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <utility>

namespace
{
static const size_t HANDSHAKE_LIMIT = 8192;
static const size_t SEND_QUEUE_LIMIT = 64 * 1024;
static const size_t CONTROL_FRAME_RESERVE = 131;
static const size_t MESSAGE_LIMIT = 10 * 1024 * 1024;
static const size_t MESSAGE_QUEUE_LIMIT = 12 * 1024 * 1024;
static const char *WEBSOCKET_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
static const int SOC_IN_PROGRESS = -26;
// The server also sends heartbeats, but a client-side deadline is required for
// Wi-Fi/NAT failures where the local TCP socket remains logically open and no
// reset can make it back to the console.
static const u64 HEARTBEAT_INTERVAL_MS = 15000;
static const u64 HEARTBEAT_TIMEOUT_MS = 15000;

static u32 rotateLeft(u32 value, unsigned int bits)
{
    return (value << bits) | (value >> (32 - bits));
}

static void sha1ProcessBlock(u32 state[5], const uint8_t block[64])
{
    u32 words[80];
    for (int i = 0; i < 16; i++)
    {
        words[i] = ((u32)block[i * 4] << 24) |
                   ((u32)block[i * 4 + 1] << 16) |
                   ((u32)block[i * 4 + 2] << 8) |
                   (u32)block[i * 4 + 3];
    }
    for (int i = 16; i < 80; i++)
        words[i] = rotateLeft(words[i - 3] ^ words[i - 8] ^ words[i - 14] ^ words[i - 16], 1);

    u32 a = state[0];
    u32 b = state[1];
    u32 c = state[2];
    u32 d = state[3];
    u32 e = state[4];
    for (int i = 0; i < 80; i++)
    {
        u32 function;
        u32 constant;
        if (i < 20)
        {
            function = (b & c) | ((~b) & d);
            constant = 0x5a827999;
        }
        else if (i < 40)
        {
            function = b ^ c ^ d;
            constant = 0x6ed9eba1;
        }
        else if (i < 60)
        {
            function = (b & c) | (b & d) | (c & d);
            constant = 0x8f1bbcdc;
        }
        else
        {
            function = b ^ c ^ d;
            constant = 0xca62c1d6;
        }
        u32 next = rotateLeft(a, 5) + function + e + constant + words[i];
        e = d;
        d = c;
        c = rotateLeft(b, 30);
        b = a;
        a = next;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
}

static void sha1(const uint8_t *bytes, size_t length, uint8_t output[20])
{
    u32 state[5] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0};
    size_t offset = 0;
    while (length - offset >= 64)
    {
        sha1ProcessBlock(state, bytes + offset);
        offset += 64;
    }

    uint8_t finalBlocks[128];
    size_t remaining = length - offset;
    memcpy(finalBlocks, bytes + offset, remaining);
    finalBlocks[remaining] = 0x80;
    size_t paddedLength = remaining < 56 ? 64 : 128;
    memset(finalBlocks + remaining + 1, 0, paddedLength - remaining - 1);
    uint64_t bitLength = (uint64_t)length * 8;
    for (int i = 0; i < 8; i++)
        finalBlocks[paddedLength - 1 - i] = (uint8_t)(bitLength >> (i * 8));
    sha1ProcessBlock(state, finalBlocks);
    if (paddedLength == 128)
        sha1ProcessBlock(state, finalBlocks + 64);

    for (int i = 0; i < 5; i++)
    {
        output[i * 4] = (uint8_t)(state[i] >> 24);
        output[i * 4 + 1] = (uint8_t)(state[i] >> 16);
        output[i * 4 + 2] = (uint8_t)(state[i] >> 8);
        output[i * 4 + 3] = (uint8_t)state[i];
    }
}

static bool base64Encode(const uint8_t *input, size_t length, char *output, size_t capacity)
{
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t encodedLength = ((length + 2) / 3) * 4;
    if (!input || !output || capacity <= encodedLength)
        return false;
    size_t in = 0;
    size_t out = 0;
    while (in < length)
    {
        size_t remaining = length - in;
        unsigned int first = input[in++];
        unsigned int second = remaining > 1 ? input[in++] : 0;
        unsigned int third = remaining > 2 ? input[in++] : 0;
        unsigned int value = (first << 16) | (second << 8) | third;
        output[out++] = alphabet[(value >> 18) & 0x3f];
        output[out++] = alphabet[(value >> 12) & 0x3f];
        output[out++] = remaining > 1 ? alphabet[(value >> 6) & 0x3f] : '=';
        output[out++] = remaining > 2 ? alphabet[value & 0x3f] : '=';
    }
    output[out] = '\0';
    return true;
}

static char asciiLower(char value)
{
    return value >= 'A' && value <= 'Z' ? (char)(value + ('a' - 'A')) : value;
}

static bool equalsIgnoreCase(const char *value, size_t length, const char *expected)
{
    size_t expectedLength = strlen(expected);
    if (length != expectedLength)
        return false;
    for (size_t i = 0; i < length; i++)
        if (asciiLower(value[i]) != asciiLower(expected[i]))
            return false;
    return true;
}

static bool tokenListContains(const char *value, size_t length, const char *expected)
{
    size_t offset = 0;
    while (offset < length)
    {
        while (offset < length && (value[offset] == ' ' || value[offset] == '\t' || value[offset] == ','))
            offset++;
        size_t end = offset;
        while (end < length && value[end] != ',')
            end++;
        size_t trimmedEnd = end;
        while (trimmedEnd > offset && (value[trimmedEnd - 1] == ' ' || value[trimmedEnd - 1] == '\t'))
            trimmedEnd--;
        if (equalsIgnoreCase(value + offset, trimmedEnd - offset, expected))
            return true;
        offset = end < length ? end + 1 : end;
    }
    return false;
}

static const char *findCrlf(const char *start, const char *end)
{
    for (const char *cursor = start; cursor + 1 < end; cursor++)
        if (cursor[0] == '\r' && cursor[1] == '\n')
            return cursor;
    return NULL;
}

static size_t findHeaderEnd(const std::vector<uint8_t> &bytes)
{
    for (size_t i = 3; i < bytes.size(); i++)
        if (bytes[i - 3] == '\r' && bytes[i - 2] == '\n' && bytes[i - 1] == '\r' && bytes[i] == '\n')
            return i + 1;
    return 0;
}

static bool isSafeRequestValue(const char *value, size_t maximumLength)
{
    if (!value || !value[0])
        return false;
    size_t length = 0;
    while (value[length])
    {
        uint8_t byte = (uint8_t)value[length];
        if (byte < 0x21 || byte > 0x7e || byte == '\r' || byte == '\n')
            return false;
        if (++length > maximumLength)
            return false;
    }
    return true;
}

static bool isValidUtf8(const uint8_t *bytes, size_t length)
{
    size_t i = 0;
    while (i < length)
    {
        uint8_t first = bytes[i++];
        if (first <= 0x7f)
            continue;
        int continuationCount;
        uint32_t codePoint;
        if (first >= 0xc2 && first <= 0xdf)
        {
            continuationCount = 1;
            codePoint = first & 0x1f;
        }
        else if (first >= 0xe0 && first <= 0xef)
        {
            continuationCount = 2;
            codePoint = first & 0x0f;
        }
        else if (first >= 0xf0 && first <= 0xf4)
        {
            continuationCount = 3;
            codePoint = first & 0x07;
        }
        else
            return false;
        if (i + (size_t)continuationCount > length)
            return false;
        for (int n = 0; n < continuationCount; n++)
        {
            uint8_t continuation = bytes[i++];
            if ((continuation & 0xc0) != 0x80)
                return false;
            codePoint = (codePoint << 6) | (continuation & 0x3f);
        }
        if ((continuationCount == 2 && codePoint < 0x800) ||
            (continuationCount == 3 && codePoint < 0x10000) ||
            codePoint > 0x10ffff || (codePoint >= 0xd800 && codePoint <= 0xdfff))
            return false;
    }
    return true;
}

static bool isValidCloseCode(uint16_t code)
{
    if (code >= 3000 && code <= 4999)
        return true;
    if (code < 1000 || code > 1014)
        return false;
    return code != 1004 && code != 1005 && code != 1006;
}

static bool validateHandshake(const std::vector<uint8_t> &response, size_t headerLength,
                              const char *expectedAccept, char *error, size_t errorSize)
{
    const char *start = (const char *)response.data();
    const char *end = start + headerLength;
    const char *lineEnd = findCrlf(start, end);
    if (!lineEnd)
        return false;
    static const char prefix[] = "HTTP/1.1 101";
    size_t statusLength = (size_t)(lineEnd - start);
    if (statusLength < sizeof(prefix) - 1 || memcmp(start, prefix, sizeof(prefix) - 1) != 0 ||
        (statusLength > sizeof(prefix) - 1 && start[sizeof(prefix) - 1] != ' '))
    {
        size_t copyLength = std::min(statusLength, errorSize ? errorSize - 1 : 0);
        if (errorSize)
        {
            memcpy(error, start, copyLength);
            error[copyLength] = '\0';
        }
        return false;
    }

    bool hasUpgrade = false;
    bool hasConnectionUpgrade = false;
    bool hasAccept = false;
    int acceptCount = 0;
    const char *line = lineEnd + 2;
    while (line < end)
    {
        lineEnd = findCrlf(line, end);
        if (!lineEnd)
            return false;
        if (lineEnd == line)
            break;
        if (*line == ' ' || *line == '\t')
            return false;
        const char *colon = line;
        while (colon < lineEnd && *colon != ':')
            colon++;
        if (colon == lineEnd)
            return false;
        const char *nameEnd = colon;
        while (nameEnd > line && (nameEnd[-1] == ' ' || nameEnd[-1] == '\t'))
            nameEnd--;
        const char *value = colon + 1;
        while (value < lineEnd && (*value == ' ' || *value == '\t'))
            value++;
        const char *valueEnd = lineEnd;
        while (valueEnd > value && (valueEnd[-1] == ' ' || valueEnd[-1] == '\t'))
            valueEnd--;
        size_t nameLength = (size_t)(nameEnd - line);
        size_t valueLength = (size_t)(valueEnd - value);
        if (equalsIgnoreCase(line, nameLength, "Upgrade"))
            hasUpgrade = hasUpgrade || tokenListContains(value, valueLength, "websocket");
        else if (equalsIgnoreCase(line, nameLength, "Connection"))
            hasConnectionUpgrade = hasConnectionUpgrade || tokenListContains(value, valueLength, "upgrade");
        else if (equalsIgnoreCase(line, nameLength, "Sec-WebSocket-Accept"))
        {
            acceptCount++;
            hasAccept = valueLength == strlen(expectedAccept) && memcmp(value, expectedAccept, valueLength) == 0;
        }
        else if ((equalsIgnoreCase(line, nameLength, "Sec-WebSocket-Extensions") ||
                  equalsIgnoreCase(line, nameLength, "Sec-WebSocket-Protocol")) &&
                 valueLength > 0)
            return false;
        line = lineEnd + 2;
    }
    return hasUpgrade && hasConnectionUpgrade && hasAccept && acceptCount == 1;
}
}

WebSocketClient::WebSocketClient()
    : plainSocket(-1), secureTransport(true), connected(false), closeSent(false), awaitingPong(false),
      pingSequence(0), lastPingAt(0), pongDeadline(0),
      sendOffset(0), queuedSendBytes(0), fragmentOpcode(0), queuedMessageBytes(0)
{
    errorText[0] = '\0';
    memset(pingPayload, 0, sizeof(pingPayload));
}

WebSocketClient::~WebSocketClient()
{
    closeStream();
}

void WebSocketClient::setError(const char *message)
{
    snprintf(errorText, sizeof(errorText), "%s", message ? message : "network error");
}

const char *WebSocketClient::lastError() const
{
    return errorText[0] ? errorText : "network error";
}

bool WebSocketClient::connectPlain(const char *host, const char *port, int timeoutMs)
{
    addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo *addresses = NULL;
    int lookup = getaddrinfo(host, port, &hints, &addresses);
    if (lookup != 0 || !addresses)
    {
        setError("DNS lookup failed");
        return false;
    }

    bool didConnect = false;
    for (addrinfo *address = addresses; address; address = address->ai_next)
    {
        int candidate = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (candidate < 0)
            continue;
        int flags = fcntl(candidate, F_GETFL, 0);
        if (flags < 0 || fcntl(candidate, F_SETFL, flags | O_NONBLOCK) < 0)
        {
            ::close(candidate);
            continue;
        }
        int result = ::connect(candidate, address->ai_addr, address->ai_addrlen);
        if (result == 0)
            didConnect = true;
        else if (result == SOC_IN_PROGRESS || errno == SOC_IN_PROGRESS || errno == EINPROGRESS || errno == EWOULDBLOCK)
        {
            fd_set writeSet;
            FD_ZERO(&writeSet);
            FD_SET(candidate, &writeSet);
            timeval timeout;
            timeout.tv_sec = timeoutMs / 1000;
            timeout.tv_usec = (timeoutMs % 1000) * 1000;
            result = select(candidate + 1, NULL, &writeSet, NULL, &timeout);
            if (result > 0 && FD_ISSET(candidate, &writeSet))
            {
                int socketError = 0;
                socklen_t errorLength = sizeof(socketError);
                didConnect = getsockopt(candidate, SOL_SOCKET, SO_ERROR, &socketError, &errorLength) == 0 &&
                             (socketError == 0 || socketError == EISCONN || socketError == SOC_IN_PROGRESS);
            }
        }
        if (didConnect)
        {
            plainSocket = candidate;
            int enabled = 1;
            setsockopt(plainSocket, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled));
            break;
        }
        ::close(candidate);
    }
    freeaddrinfo(addresses);
    if (!didConnect)
        setError("TCP connect failed");
    return didConnect;
}

void WebSocketClient::closeStream()
{
    connected = false;
    awaitingPong = false;
    lastPingAt = 0;
    pongDeadline = 0;
    if (secureTransport)
        tlsStream.close();
    if (plainSocket >= 0)
    {
        ::shutdown(plainSocket, SHUT_RDWR);
        ::close(plainSocket);
        plainSocket = -1;
    }
}

WebSocketClient::StreamResult WebSocketClient::streamRead(void *buffer, size_t capacity, size_t &received)
{
    received = 0;
    if (secureTransport)
    {
        TlsStream::IoResult result = tlsStream.read(buffer, capacity, received);
        if (result == TlsStream::IO_OK)
            return STREAM_OK;
        if (result == TlsStream::IO_WOULD_BLOCK)
            return STREAM_WOULD_BLOCK;
        if (result == TlsStream::IO_CLOSED)
            return STREAM_CLOSED;
        setError(tlsStream.lastError());
        return STREAM_ERROR;
    }
    int result = recv(plainSocket, buffer, capacity, 0);
    if (result > 0)
    {
        received = (size_t)result;
        return STREAM_OK;
    }
    if (result == 0)
        return STREAM_CLOSED;
    if (errno == EINTR || errno == EWOULDBLOCK || errno == EAGAIN)
        return STREAM_WOULD_BLOCK;
    setError("socket read failed");
    return STREAM_ERROR;
}

WebSocketClient::StreamResult WebSocketClient::streamWrite(const void *buffer, size_t length, size_t &written)
{
    written = 0;
    if (secureTransport)
    {
        TlsStream::IoResult result = tlsStream.write(buffer, length, written);
        if (result == TlsStream::IO_OK)
            return STREAM_OK;
        if (result == TlsStream::IO_WOULD_BLOCK)
            return STREAM_WOULD_BLOCK;
        if (result == TlsStream::IO_CLOSED)
            return STREAM_CLOSED;
        setError(tlsStream.lastError());
        return STREAM_ERROR;
    }
    int result = send(plainSocket, buffer, length, MSG_NOSIGNAL);
    if (result > 0)
    {
        written = (size_t)result;
        return STREAM_OK;
    }
    if (result == 0)
        return STREAM_CLOSED;
    if (errno == EINTR || errno == EWOULDBLOCK || errno == EAGAIN)
        return STREAM_WOULD_BLOCK;
    setError("socket write failed");
    return STREAM_ERROR;
}

bool WebSocketClient::connect(const char *host, const char *port, const char *path, bool secure, int connectTimeoutMs)
{
    closeStream();
    receiveBuffer.clear();
    sendFrames.clear();
    fragmentBuffer.clear();
    messages.clear();
    sendOffset = 0;
    queuedSendBytes = 0;
    fragmentOpcode = 0;
    queuedMessageBytes = 0;
    closeSent = false;
    awaitingPong = false;
    lastPingAt = 0;
    pongDeadline = 0;
    errorText[0] = '\0';
    secureTransport = secure;
    if (!isSafeRequestValue(host, 253) || !isSafeRequestValue(port, 7) || !isSafeRequestValue(path, 255) || path[0] != '/')
    {
        setError("invalid WebSocket address");
        return false;
    }
    u64 connectDeadline = osGetTime() + (u64)connectTimeoutMs;
    bool streamConnected = secureTransport ? tlsStream.connect(host, port, connectTimeoutMs)
                                           : connectPlain(host, port, connectTimeoutMs);
    if (!streamConnected)
    {
        if (secureTransport)
            setError(tlsStream.lastError());
        closeStream();
        return false;
    }
    u64 now = osGetTime();
    if (now >= connectDeadline)
    {
        setError("WebSocket connection timeout");
        closeStream();
        return false;
    }
    int handshakeTimeoutMs = (int)(connectDeadline - now);
    if (!performHandshake(host, port, path, handshakeTimeoutMs))
    {
        closeStream();
        return false;
    }
    connected = true;
    lastPingAt = osGetTime();
    return true;
}

bool WebSocketClient::performHandshake(const char *host, const char *port, const char *path, int timeoutMs)
{
    uint8_t nonce[16];
    char websocketKey[32];
    if (!tlsStream.randomBytes(nonce, sizeof(nonce)) ||
        !base64Encode(nonce, sizeof(nonce), websocketKey, sizeof(websocketKey)))
    {
        setError("secure random failed");
        return false;
    }
    char challenge[96];
    snprintf(challenge, sizeof(challenge), "%s%s", websocketKey, WEBSOCKET_GUID);
    uint8_t digest[20];
    char expectedAccept[32];
    sha1((const uint8_t *)challenge, strlen(challenge), digest);
    if (!base64Encode(digest, sizeof(digest), expectedAccept, sizeof(expectedAccept)))
    {
        setError("handshake key failed");
        return false;
    }

    bool defaultPort = (secureTransport && strcmp(port, "443") == 0) || (!secureTransport && strcmp(port, "80") == 0);
    char request[768];
    int requestLength = snprintf(request, sizeof(request),
                                 "GET %s HTTP/1.1\r\n"
                                 "Host: %s%s%s\r\n"
                                 "Upgrade: websocket\r\n"
                                 "Connection: Upgrade\r\n"
                                 "Sec-WebSocket-Key: %s\r\n"
                                 "Sec-WebSocket-Version: 13\r\n"
                                 "User-Agent: CollabDoodle-3DS/%s\r\n"
                                 "\r\n",
                                 path, host, defaultPort ? "" : ":", defaultPort ? "" : port,
                                 websocketKey, APP_VERSION);
    if (requestLength <= 0 || (size_t)requestLength >= sizeof(request))
    {
        setError("handshake request too large");
        return false;
    }

    u64 deadline = osGetTime() + (u64)timeoutMs;
    size_t sent = 0;
    while (sent < (size_t)requestLength)
    {
        size_t written = 0;
        StreamResult result = streamWrite(request + sent, (size_t)requestLength - sent, written);
        if (result == STREAM_OK && written > 0)
        {
            sent += written;
            continue;
        }
        if (result != STREAM_WOULD_BLOCK)
        {
            if (result == STREAM_CLOSED)
                setError("server closed during handshake");
            return false;
        }
        if (osGetTime() >= deadline)
        {
            setError("WebSocket handshake timeout");
            return false;
        }
        svcSleepThread(1000000LL);
    }

    std::vector<uint8_t> response;
    response.reserve(1024);
    size_t headerEnd = 0;
    while (!headerEnd)
    {
        uint8_t chunk[1024];
        size_t received = 0;
        StreamResult result = streamRead(chunk, sizeof(chunk), received);
        if (result == STREAM_OK && received > 0)
        {
            if (response.size() + received > HANDSHAKE_LIMIT)
            {
                setError("WebSocket handshake too large");
                return false;
            }
            response.insert(response.end(), chunk, chunk + received);
            headerEnd = findHeaderEnd(response);
            continue;
        }
        if (result != STREAM_WOULD_BLOCK)
        {
            if (result == STREAM_CLOSED)
                setError("server closed during handshake");
            return false;
        }
        if (osGetTime() >= deadline)
        {
            setError("WebSocket handshake timeout");
            return false;
        }
        svcSleepThread(1000000LL);
    }

    char validationError[128] = "bad WebSocket handshake";
    if (!validateHandshake(response, headerEnd, expectedAccept, validationError, sizeof(validationError)))
    {
        setError(validationError);
        return false;
    }
    if (response.size() > headerEnd)
        receiveBuffer.insert(receiveBuffer.end(), response.begin() + headerEnd, response.end());
    return true;
}

bool WebSocketClient::queueFrame(uint8_t opcode, const void *payloadValue, size_t length)
{
    const uint8_t *payload = (const uint8_t *)payloadValue;
    bool control = (opcode & 0x08) != 0;
    if ((length > 0 && !payload) || length > MESSAGE_LIMIT || (control && length > 125))
    {
        setError("WebSocket frame too large");
        return false;
    }
    size_t headerLength = length <= 125 ? 6 : (length <= 0xffff ? 8 : 14);
    size_t queueLimit = control ? SEND_QUEUE_LIMIT : SEND_QUEUE_LIMIT - CONTROL_FRAME_RESERVE;
    if (headerLength + length > queueLimit || queuedSendBytes + headerLength + length > queueLimit)
    {
        setError("WebSocket send queue full");
        return false;
    }
    uint8_t mask[4];
    if (!tlsStream.randomBytes(mask, sizeof(mask)))
    {
        setError("secure random failed");
        return false;
    }
    std::vector<uint8_t> frame;
    frame.reserve(headerLength + length);
    frame.push_back((uint8_t)(0x80 | (opcode & 0x0f)));
    if (length <= 125)
        frame.push_back((uint8_t)(0x80 | length));
    else if (length <= 0xffff)
    {
        frame.push_back(0x80 | 126);
        frame.push_back((uint8_t)(length >> 8));
        frame.push_back((uint8_t)length);
    }
    else
    {
        frame.push_back(0x80 | 127);
        uint64_t wireLength = (uint64_t)length;
        for (int shift = 56; shift >= 0; shift -= 8)
            frame.push_back((uint8_t)(wireLength >> shift));
    }
    frame.insert(frame.end(), mask, mask + sizeof(mask));
    size_t payloadStart = frame.size();
    frame.resize(payloadStart + length);
    for (size_t i = 0; i < length; i++)
        frame[payloadStart + i] = payload[i] ^ mask[i % 4];
    queuedSendBytes += frame.size();
    if (control)
    {
        // Control frames must not wait behind a full drawing backlog. Never
        // interleave an in-progress frame, but otherwise place ping/pong/close
        // at the front as required for useful heartbeat deadlines.
        std::deque<std::vector<uint8_t> >::iterator position = sendFrames.begin();
        if (sendOffset > 0 && position != sendFrames.end())
            ++position;
        sendFrames.insert(position, std::move(frame));
    }
    else
    {
        sendFrames.push_back(std::move(frame));
    }
    return true;
}

bool WebSocketClient::sendText(const void *payload, size_t length)
{
    return connected && length <= 32768 && queueFrame(0x01, payload, length);
}

bool WebSocketClient::sendBinary(const void *payload, size_t length)
{
    return connected && length <= 32768 && queueFrame(0x02, payload, length);
}

bool WebSocketClient::queueHeartbeatPing(u64 now)
{
    pingSequence++;
    pingPayload[0] = (uint8_t)(pingSequence >> 24);
    pingPayload[1] = (uint8_t)(pingSequence >> 16);
    pingPayload[2] = (uint8_t)(pingSequence >> 8);
    pingPayload[3] = (uint8_t)pingSequence;
    if (!queueFrame(0x09, pingPayload, sizeof(pingPayload)))
        return false;
    awaitingPong = true;
    lastPingAt = now;
    pongDeadline = now + HEARTBEAT_TIMEOUT_MS;
    return true;
}

bool WebSocketClient::flushOutgoing()
{
    while (connected && !sendFrames.empty())
    {
        std::vector<uint8_t> &frame = sendFrames.front();
        size_t written = 0;
        StreamResult result = streamWrite(frame.data() + sendOffset, frame.size() - sendOffset, written);
        if (result == STREAM_OK && written > 0)
        {
            sendOffset += written;
            if (sendOffset == frame.size())
            {
                queuedSendBytes -= frame.size();
                sendFrames.pop_front();
                sendOffset = 0;
            }
            continue;
        }
        if (result == STREAM_WOULD_BLOCK)
            return true;
        if (result == STREAM_CLOSED)
            setError("WebSocket server closed");
        closeStream();
        return false;
    }
    return connected;
}

bool WebSocketClient::queueMessage(uint8_t opcode, const uint8_t *payload, size_t length)
{
    if (queuedMessageBytes + length > MESSAGE_QUEUE_LIMIT)
        return failProtocol("WebSocket receive queue full", 1009);
    if (opcode == 0x01 && !isValidUtf8(payload, length))
        return failProtocol("invalid UTF-8 message", 1007);
    Message message;
    message.type = opcode == 0x01 ? MESSAGE_TEXT : MESSAGE_BINARY;
    message.payload.assign(payload, payload + length);
    queuedMessageBytes += length;
    messages.push_back(std::move(message));
    return true;
}

bool WebSocketClient::failProtocol(const char *message, uint16_t closeCode)
{
    setError(message);
    if (connected && !closeSent)
    {
        uint8_t payload[2] = {(uint8_t)(closeCode >> 8), (uint8_t)closeCode};
        closeSent = true;
        if (queueFrame(0x08, payload, sizeof(payload)))
            flushOutgoing();
    }
    closeStream();
    return false;
}

bool WebSocketClient::parseAvailableFrames()
{
    while (connected)
    {
        if (receiveBuffer.size() < 2)
            return true;
        uint8_t first = receiveBuffer[0];
        uint8_t second = receiveBuffer[1];
        bool finalFrame = (first & 0x80) != 0;
        uint8_t opcode = first & 0x0f;
        if ((first & 0x70) != 0)
            return failProtocol("unsupported WebSocket extension", 1002);
        if ((second & 0x80) != 0)
            return failProtocol("masked server WebSocket frame", 1002);

        uint64_t payloadLength = second & 0x7f;
        size_t headerLength = 2;
        if (payloadLength == 126)
        {
            if (receiveBuffer.size() < 4)
                return true;
            payloadLength = ((uint64_t)receiveBuffer[2] << 8) | receiveBuffer[3];
            if (payloadLength < 126)
                return failProtocol("noncanonical WebSocket length", 1002);
            headerLength = 4;
        }
        else if (payloadLength == 127)
        {
            if (receiveBuffer.size() < 10)
                return true;
            if ((receiveBuffer[2] & 0x80) != 0)
                return failProtocol("invalid WebSocket length", 1002);
            payloadLength = 0;
            for (int i = 2; i < 10; i++)
                payloadLength = (payloadLength << 8) | receiveBuffer[i];
            if (payloadLength < 65536)
                return failProtocol("noncanonical WebSocket length", 1002);
            headerLength = 10;
        }
        if (payloadLength > MESSAGE_LIMIT)
            return failProtocol("WebSocket message too large", 1009);
        bool control = (opcode & 0x08) != 0;
        if (control && (!finalFrame || payloadLength > 125))
            return failProtocol("invalid WebSocket control frame", 1002);
        if (payloadLength > SIZE_MAX - headerLength || receiveBuffer.size() < headerLength + (size_t)payloadLength)
            return true;

        const uint8_t *payload = receiveBuffer.data() + headerLength;
        size_t completeLength = headerLength + (size_t)payloadLength;
        if (opcode == 0x08)
        {
            if (payloadLength == 1)
                return failProtocol("invalid WebSocket close frame", 1002);
            if (payloadLength >= 2)
            {
                uint16_t closeCode = (uint16_t)(((uint16_t)payload[0] << 8) | payload[1]);
                if (!isValidCloseCode(closeCode))
                    return failProtocol("invalid WebSocket close status", 1002);
                if (payloadLength > 2 && !isValidUtf8(payload + 2, (size_t)payloadLength - 2))
                    return failProtocol("invalid WebSocket close reason", 1007);
            }
            if (!closeSent)
            {
                closeSent = true;
                queueFrame(0x08, payload, (size_t)payloadLength);
                flushOutgoing();
            }
            setError("WebSocket server closed");
            closeStream();
            return false;
        }
        if (opcode == 0x09)
        {
            if (!queueFrame(0x0a, payload, (size_t)payloadLength))
                return failProtocol("unable to queue WebSocket pong", 1011);
            receiveBuffer.erase(receiveBuffer.begin(), receiveBuffer.begin() + completeLength);
            continue;
        }
        if (opcode == 0x0a)
        {
            if (awaitingPong && payloadLength == sizeof(pingPayload) &&
                memcmp(payload, pingPayload, sizeof(pingPayload)) == 0)
            {
                awaitingPong = false;
                pongDeadline = 0;
                lastPingAt = osGetTime();
            }
            receiveBuffer.erase(receiveBuffer.begin(), receiveBuffer.begin() + completeLength);
            continue;
        }
        if (opcode != 0x00 && opcode != 0x01 && opcode != 0x02)
            return failProtocol("unknown WebSocket opcode", 1002);

        if (opcode == 0x00)
        {
            if (fragmentOpcode == 0)
                return failProtocol("unexpected WebSocket continuation", 1002);
            if (fragmentBuffer.size() + (size_t)payloadLength > MESSAGE_LIMIT)
                return failProtocol("fragmented WebSocket message too large", 1009);
            fragmentBuffer.insert(fragmentBuffer.end(), payload, payload + payloadLength);
            receiveBuffer.erase(receiveBuffer.begin(), receiveBuffer.begin() + completeLength);
            if (finalFrame)
            {
                uint8_t completeOpcode = fragmentOpcode;
                fragmentOpcode = 0;
                if (!queueMessage(completeOpcode, fragmentBuffer.data(), fragmentBuffer.size()))
                    return false;
                fragmentBuffer.clear();
            }
            continue;
        }
        if (fragmentOpcode != 0)
            return failProtocol("interleaved WebSocket message", 1002);
        if (!finalFrame)
        {
            fragmentOpcode = opcode;
            fragmentBuffer.assign(payload, payload + payloadLength);
            receiveBuffer.erase(receiveBuffer.begin(), receiveBuffer.begin() + completeLength);
            continue;
        }
        if (!queueMessage(opcode, payload, (size_t)payloadLength))
            return false;
        receiveBuffer.erase(receiveBuffer.begin(), receiveBuffer.begin() + completeLength);
    }
    return false;
}

bool WebSocketClient::update()
{
    if (!connected)
        return false;
    u64 now = osGetTime();
    if (awaitingPong && now >= pongDeadline)
    {
        setError("WebSocket heartbeat timeout");
        closeStream();
        return false;
    }
    if (!awaitingPong && now - lastPingAt >= HEARTBEAT_INTERVAL_MS &&
        !queueHeartbeatPing(now))
    {
        setError("unable to queue WebSocket ping");
        closeStream();
        return false;
    }
    if (!flushOutgoing())
        return false;
    if (!parseAvailableFrames())
        return false;

    uint8_t chunk[8192];
    for (int reads = 0; reads < 4 && connected; reads++)
    {
        size_t received = 0;
        StreamResult result = streamRead(chunk, sizeof(chunk), received);
        if (result == STREAM_OK && received > 0)
        {
            if (receiveBuffer.size() + received > MESSAGE_LIMIT + 14)
                return failProtocol("WebSocket receive buffer full", 1009);
            receiveBuffer.insert(receiveBuffer.end(), chunk, chunk + received);
            if (!parseAvailableFrames())
                return false;
            continue;
        }
        if (result == STREAM_WOULD_BLOCK)
            return true;
        if (result == STREAM_CLOSED)
            setError("WebSocket server closed");
        closeStream();
        return false;
    }
    return connected;
}

bool WebSocketClient::pollMessage(Message &message)
{
    if (messages.empty())
        return false;
    message = std::move(messages.front());
    queuedMessageBytes -= message.payload.size();
    messages.pop_front();
    return true;
}

void WebSocketClient::close(uint16_t code)
{
    if (connected && !closeSent)
    {
        uint8_t payload[2] = {(uint8_t)(code >> 8), (uint8_t)code};
        closeSent = true;
        if (queueFrame(0x08, payload, sizeof(payload)))
            flushOutgoing();
    }
    closeStream();
}

bool WebSocketClient::isConnected() const
{
    return connected;
}
