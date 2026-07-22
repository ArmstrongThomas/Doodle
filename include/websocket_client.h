#ifndef WEBSOCKET_CLIENT_H
#define WEBSOCKET_CLIENT_H

#include <stddef.h>
#include <stdint.h>
#include <deque>
#include <string>
#include <vector>

#include "tls_stream.h"

class WebSocketClient
{
public:
    enum MessageType
    {
        MESSAGE_TEXT,
        MESSAGE_BINARY
    };

    struct Message
    {
        MessageType type;
        std::vector<uint8_t> payload;
    };

    WebSocketClient();
    ~WebSocketClient();

    bool connect(const char *host, const char *port, const char *path, bool secure,
                 int connectTimeoutMs = 8000);
    void close(uint16_t code = 1000);
    bool isConnected() const;
    const char *lastError() const;

    bool sendText(const void *payload, size_t length);
    bool sendBinary(const void *payload, size_t length);
    bool update();
    bool pollMessage(Message &message);

private:
    WebSocketClient(const WebSocketClient &);
    WebSocketClient &operator=(const WebSocketClient &);

    enum StreamResult
    {
        STREAM_OK,
        STREAM_WOULD_BLOCK,
        STREAM_CLOSED,
        STREAM_ERROR
    };

    TlsStream tlsStream;
    int plainSocket;
    bool secureTransport;
    bool connected;
    bool closeSent;
    bool awaitingPong;
    char errorText[128];
    uint8_t pingPayload[4];
    uint32_t pingSequence;
    uint64_t lastPingAt;
    uint64_t pongDeadline;

    std::vector<uint8_t> receiveBuffer;
    std::deque<std::vector<uint8_t> > sendFrames;
    size_t sendOffset;
    size_t queuedSendBytes;
    std::vector<uint8_t> fragmentBuffer;
    uint8_t fragmentOpcode;
    std::deque<Message> messages;
    size_t queuedMessageBytes;

    bool connectPlain(const char *host, const char *port, int timeoutMs);
    void closeStream();
    StreamResult streamRead(void *buffer, size_t capacity, size_t &received);
    StreamResult streamWrite(const void *buffer, size_t length, size_t &written);
    bool performHandshake(const char *host, const char *port, const char *path, int timeoutMs);
    bool queueFrame(uint8_t opcode, const void *payload, size_t length);
    bool flushOutgoing();
    bool parseAvailableFrames();
    bool queueMessage(uint8_t opcode, const uint8_t *payload, size_t length);
    bool queueHeartbeatPing(uint64_t now);
    bool failProtocol(const char *message, uint16_t closeCode);
    void setError(const char *message);
};

#endif
