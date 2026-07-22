#ifndef NETWORK_H
#define NETWORK_H

#include <3ds.h>
#include <stddef.h>
#include <stdint.h>
#include <string>
#include <vector>

#define SOC_ALIGN 0x1000
#define SOC_BUFFERSIZE 0x100000

enum NetworkEventType
{
    NETWORK_EVENT_CONNECTED,
    NETWORK_EVENT_TEXT,
    NETWORK_EVENT_BINARY,
    NETWORK_EVENT_DISCONNECTED,
    NETWORK_EVENT_ERROR
};

struct NetworkEvent
{
    NetworkEventType type;
    std::vector<uint8_t> payload;
    std::string detail;
};

// Thread-safe facade used by the application. All DNS, TCP, TLS, WebSocket,
// reads, and writes are owned by a lower-priority worker thread.
class NetworkManager
{
public:
    static bool initialize();
    static void shutdown();

    static bool sendText(const char *text);
    static bool sendText(const void *text, size_t length);
    // Queues the mandatory first application message for a new WebSocket.
    // Normal sends remain gated until this succeeds.
    static bool sendSessionHello(const void *text, size_t length);
    static bool sendBinary(const void *buffer, size_t length);
    static bool pollEvent(NetworkEvent &event);
    static bool waitEvent(NetworkEvent &event, int timeoutMs);
    static bool waitForConnected(int timeoutMs);
    static bool waitForDisconnected(int timeoutMs);

    static bool disconnect();
    static bool reconnect();
    static bool checkConnection();
    static bool ensureConnected();
    static bool isConnected();
    static bool isConnecting();
    static const char *lastError();
};

#endif
