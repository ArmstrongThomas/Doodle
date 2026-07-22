#include "network.h"
#include "tls_stream.h"
#include "websocket_client.h"

#include <stdio.h>
#include <string.h>
#include <malloc.h>
#include <algorithm>
#include <deque>
#include <utility>

namespace
{
struct OutgoingMessage
{
    bool text;
    std::vector<uint8_t> payload;
};

static const size_t OUTGOING_QUEUE_LIMIT = 64 * 1024;
static const size_t INCOMING_QUEUE_LIMIT = 12 * 1024 * 1024;
static const size_t EVENT_COUNT_LIMIT = 128;
// mbedTLS certificate verification has a deep call chain on ARM11. Leave
// enough headroom for both the TLS handshake and WebSocket framing instead of
// relying on the smaller stack used by the former raw-TCP worker.
static const size_t NETWORK_WORKER_STACK_SIZE = 256 * 1024;
// Old 3DS hardware performs the TLS 1.2 certificate and ECDSA work in
// software. Keep this bounded, but do not apply desktop-class handshake
// timing to the ARM11.
static const int CONNECT_TIMEOUT_MS = 30000;
static const int RECONNECT_DELAYS_MS[] = {500, 1000, 2000, 4000, 8000, 15000};
static const u64 WIFI_STATUS_POLL_MS = 250;
static const u64 WIFI_RESTORE_SETTLE_MS = 1000;

static u32 *gSocBuffer = NULL;
static Thread gWorker = NULL;
static bool gAcInitialized = false;
static LightLock gLock;
static CondVar gCondition;
static bool gSynchronizationReady = false;
static bool gInitialized = false;
static bool gStopRequested = false;
static bool gDisconnectRequested = false;
static bool gReconnectRequested = false;
static bool gAutoReconnect = true;
static bool gConnected = false;
static bool gConnecting = false;
static bool gSessionReady = false;
static size_t gOutgoingBytes = 0;
static size_t gIncomingBytes = 0;
static std::deque<OutgoingMessage> gOutgoing;
static std::deque<NetworkEvent> gIncoming;
static char gLastError[160] = "offline";
static char gLastErrorSnapshot[160] = "offline";

static void setLastErrorLocked(const char *message)
{
    snprintf(gLastError, sizeof(gLastError), "%s", message ? message : "network error");
}

static bool enqueueEvent(NetworkEventType type, const void *payload, size_t length, const char *detail)
{
    NetworkEvent event;
    event.type = type;
    if (payload && length)
        event.payload.assign((const uint8_t *)payload, (const uint8_t *)payload + length);
    if (detail)
        event.detail = detail;

    LightLock_Lock(&gLock);
    if (gDisconnectRequested || gReconnectRequested)
    {
        LightLock_Unlock(&gLock);
        return true;
    }
    if (gIncoming.size() >= EVENT_COUNT_LIMIT || gIncomingBytes + event.payload.size() > INCOMING_QUEUE_LIMIT)
    {
        setLastErrorLocked("network receive queue full");
        LightLock_Unlock(&gLock);
        return false;
    }
    gIncomingBytes += event.payload.size();
    gIncoming.push_back(std::move(event));
    CondVar_WakeUp(&gCondition, 1);
    LightLock_Unlock(&gLock);
    return true;
}

static bool enqueueMessageEvent(NetworkEventType type, std::vector<uint8_t> &&payload)
{
    size_t payloadSize = payload.size();
    LightLock_Lock(&gLock);
    if (gDisconnectRequested || gReconnectRequested)
    {
        LightLock_Unlock(&gLock);
        return true;
    }
    if (gIncoming.size() >= EVENT_COUNT_LIMIT || gIncomingBytes + payloadSize > INCOMING_QUEUE_LIMIT)
    {
        setLastErrorLocked("network receive queue full");
        LightLock_Unlock(&gLock);
        return false;
    }
    NetworkEvent event;
    event.type = type;
    event.payload = std::move(payload);
    gIncomingBytes += payloadSize;
    gIncoming.push_back(std::move(event));
    CondVar_WakeUp(&gCondition, 1);
    LightLock_Unlock(&gLock);
    return true;
}

static void setConnectionState(bool connected, bool connecting, const char *error)
{
    LightLock_Lock(&gLock);
    gConnected = connected;
    gConnecting = connecting;
    // A new transport is not application-ready until its mandatory hello has
    // been queued. Any disconnect invalidates the prior session immediately.
    gSessionReady = false;
    if (error && error[0])
        setLastErrorLocked(error);
    LightLock_Unlock(&gLock);
}

static bool takeOutgoing(OutgoingMessage &message)
{
    LightLock_Lock(&gLock);
    if (gDisconnectRequested || gReconnectRequested || gOutgoing.empty())
    {
        LightLock_Unlock(&gLock);
        return false;
    }
    message = std::move(gOutgoing.front());
    gOutgoingBytes -= message.payload.size();
    gOutgoing.pop_front();
    LightLock_Unlock(&gLock);
    return true;
}

static void discardOutgoing()
{
    LightLock_Lock(&gLock);
    gOutgoing.clear();
    gOutgoingBytes = 0;
    LightLock_Unlock(&gLock);
}

static void discardIncoming()
{
    LightLock_Lock(&gLock);
    gIncoming.clear();
    gIncomingBytes = 0;
    LightLock_Unlock(&gLock);
}

static bool queryWifiConnected(bool &connected)
{
    if (!gAcInitialized)
        return false;
    u32 status = 0;
    Result result = ACU_GetStatus(&status);
    if (R_FAILED(result))
        return false;
    connected = status == 3;
    return true;
}

static void networkWorker(void *)
{
    WebSocketClient websocket;
    unsigned int retryIndex = 0;
    u64 nextAttemptAt = 0;
    u64 nextWifiStatusAt = 0;
    bool wifiConnected = true;

    for (;;)
    {
        bool stop;
        bool requestDisconnect;
        bool requestReconnect;
        bool autoReconnect;
        LightLock_Lock(&gLock);
        stop = gStopRequested;
        requestDisconnect = gDisconnectRequested;
        requestReconnect = gReconnectRequested;
        autoReconnect = gAutoReconnect;
        gDisconnectRequested = false;
        gReconnectRequested = false;
        LightLock_Unlock(&gLock);

        if (stop)
            break;

        if (requestDisconnect || requestReconnect)
        {
            bool wasConnected = websocket.isConnected();
            websocket.close();
            discardOutgoing();
            discardIncoming();
            setConnectionState(false, false, requestReconnect ? "reconnecting" : "offline");
            if (wasConnected && requestDisconnect)
                enqueueEvent(NETWORK_EVENT_DISCONNECTED, NULL, 0, "disconnected");
            retryIndex = 0;
            // Resume/network restoration can report an available access point a
            // little before DNS and sockets are usable. A short settling period
            // avoids spending a full TLS timeout on that race.
            nextAttemptAt = requestReconnect ? osGetTime() + WIFI_RESTORE_SETTLE_MS : 0;
        }

        u64 now = osGetTime();
        if (now >= nextWifiStatusAt)
        {
            bool observedConnected = true;
            if (queryWifiConnected(observedConnected) && observedConnected != wifiConnected)
            {
                wifiConnected = observedConnected;
                retryIndex = 0;
                if (!wifiConnected)
                {
                    bool wasConnected = websocket.isConnected();
                    websocket.close();
                    discardOutgoing();
                    discardIncoming();
                    setConnectionState(false, false, "waiting for Wi-Fi");
                    nextAttemptAt = 0;
                    if (wasConnected)
                        enqueueEvent(NETWORK_EVENT_DISCONNECTED, NULL, 0, "Wi-Fi disconnected");
                }
                else
                {
                    nextAttemptAt = now + WIFI_RESTORE_SETTLE_MS;
                    setConnectionState(false, false, "Wi-Fi restored");
                }
            }
            nextWifiStatusAt = now + WIFI_STATUS_POLL_MS;
        }

        if (!websocket.isConnected() && autoReconnect && wifiConnected && now >= nextAttemptAt)
        {
            setConnectionState(false, true, "connecting");
            bool didConnect = websocket.connect(SERVER_WS_HOST, SERVER_WS_PORT, SERVER_WS_PATH,
                                                SERVER_WS_SECURE != 0, CONNECT_TIMEOUT_MS);
            if (didConnect)
            {
                retryIndex = 0;
                setConnectionState(true, false, "connected");
                discardIncoming();
                if (!enqueueEvent(NETWORK_EVENT_CONNECTED, NULL, 0, NULL))
                {
                    websocket.close(1011);
                    setConnectionState(false, false, "unable to announce WebSocket connection");
                    nextAttemptAt = osGetTime() + RECONNECT_DELAYS_MS[0];
                }
            }
            else
            {
                const char *error = websocket.lastError();
                setConnectionState(false, false, error);
                discardIncoming();
                enqueueEvent(NETWORK_EVENT_ERROR, NULL, 0, error);
                unsigned int delayIndex = std::min(retryIndex,
                    (unsigned int)(sizeof(RECONNECT_DELAYS_MS) / sizeof(RECONNECT_DELAYS_MS[0]) - 1));
                nextAttemptAt = osGetTime() + RECONNECT_DELAYS_MS[delayIndex];
                if (retryIndex < sizeof(RECONNECT_DELAYS_MS) / sizeof(RECONNECT_DELAYS_MS[0]) - 1)
                    retryIndex++;
            }
        }

        if (websocket.isConnected())
        {
            OutgoingMessage outgoing;
            int sentMessages = 0;
            while (sentMessages < 16 && takeOutgoing(outgoing))
            {
                bool queued = outgoing.text
                                  ? websocket.sendText(outgoing.payload.data(), outgoing.payload.size())
                                  : websocket.sendBinary(outgoing.payload.data(), outgoing.payload.size());
                if (!queued)
                {
                    setConnectionState(false, false, websocket.lastError());
                    discardIncoming();
                    enqueueEvent(NETWORK_EVENT_ERROR, NULL, 0, websocket.lastError());
                    websocket.close(1011);
                    discardOutgoing();
                    nextAttemptAt = osGetTime() + RECONNECT_DELAYS_MS[0];
                    break;
                }
                sentMessages++;
            }

            if (websocket.isConnected() && !websocket.update())
            {
                const char *error = websocket.lastError();
                setConnectionState(false, false, error);
                discardIncoming();
                enqueueEvent(NETWORK_EVENT_DISCONNECTED, NULL, 0, error);
                discardOutgoing();
                nextAttemptAt = osGetTime() + RECONNECT_DELAYS_MS[std::min(retryIndex, 5U)];
                if (retryIndex < 5)
                    retryIndex++;
            }

            WebSocketClient::Message message;
            bool acceptMessages = websocket.isConnected();
            while (websocket.pollMessage(message))
            {
                // Frames left behind by a socket that failed during this update
                // belong to the old connection generation. Drain them locally
                // instead of letting a stale snapshot overwrite reconnect UI.
                if (!acceptMessages)
                    continue;
                NetworkEventType type = message.type == WebSocketClient::MESSAGE_TEXT
                                            ? NETWORK_EVENT_TEXT
                                            : NETWORK_EVENT_BINARY;
                if (!enqueueMessageEvent(type, std::move(message.payload)))
                {
                    websocket.close(1009);
                    setConnectionState(false, false, "network receive queue full");
                    discardOutgoing();
                    discardIncoming();
                    enqueueEvent(NETWORK_EVENT_ERROR, NULL, 0, "network receive queue full");
                    nextAttemptAt = osGetTime() + RECONNECT_DELAYS_MS[0];
                    break;
                }
            }
        }

        LightLock_Lock(&gLock);
        if (!gStopRequested && !gDisconnectRequested && !gReconnectRequested && gOutgoing.empty())
            CondVar_WaitTimeout(&gCondition, &gLock, 5000000LL);
        LightLock_Unlock(&gLock);
    }

    websocket.close();
    setConnectionState(false, false, "offline");
}

static bool queueOutgoing(bool text, const void *buffer, size_t length, bool sessionHello = false)
{
    if (!buffer || length == 0 || length > 32768)
        return false;

    // Protocol 6 uses message boundaries; old JSON builders may temporarily
    // leave a line terminator during the migration, so never put it on wire.
    if (text)
    {
        const uint8_t *bytes = (const uint8_t *)buffer;
        while (length > 0 && (bytes[length - 1] == '\n' || bytes[length - 1] == '\r'))
            length--;
        if (length == 0)
            return false;
    }

    LightLock_Lock(&gLock);
    if (!gInitialized || !gConnected ||
        (!sessionHello && !gSessionReady) || (sessionHello && gSessionReady) ||
        gOutgoingBytes + length > OUTGOING_QUEUE_LIMIT)
    {
        if (gOutgoingBytes + length > OUTGOING_QUEUE_LIMIT)
            setLastErrorLocked("network send queue full");
        LightLock_Unlock(&gLock);
        return false;
    }
    OutgoingMessage message;
    message.text = text;
    message.payload.assign((const uint8_t *)buffer, (const uint8_t *)buffer + length);
    gOutgoingBytes += length;
    gOutgoing.push_back(std::move(message));
    if (sessionHello)
        gSessionReady = true;
    CondVar_WakeUp(&gCondition, 1);
    LightLock_Unlock(&gLock);
    return true;
}
}

bool NetworkManager::initialize()
{
    if (gInitialized)
        return true;

    LightLock_Init(&gLock);
    CondVar_Init(&gCondition);
    gSynchronizationReady = true;

    gSocBuffer = (u32 *)memalign(SOC_ALIGN, SOC_BUFFERSIZE);
    if (!gSocBuffer)
    {
        printf("Unable to allocate SOC buffer.\n");
        return false;
    }
    Result socResult = socInit(gSocBuffer, SOC_BUFFERSIZE);
    if (R_FAILED(socResult))
    {
        printf("socInit failed: 0x%08lx\n", (unsigned long)socResult);
        free(gSocBuffer);
        gSocBuffer = NULL;
        return false;
    }
    Result acResult = acInit();
    if (R_SUCCEEDED(acResult))
        gAcInitialized = true;
    else
        printf("AC status service unavailable: 0x%08lx; using WebSocket heartbeat recovery.\n",
               (unsigned long)acResult);
    if (!TlsStream::initialize())
    {
        printf("TLS initialization failed.\n");
        if (gAcInitialized)
        {
            acExit();
            gAcInitialized = false;
        }
        socExit();
        free(gSocBuffer);
        gSocBuffer = NULL;
        return false;
    }

    LightLock_Lock(&gLock);
    gStopRequested = false;
    gDisconnectRequested = false;
    gReconnectRequested = false;
    gAutoReconnect = true;
    gConnected = false;
    gConnecting = true;
    gSessionReady = false;
    gOutgoing.clear();
    gIncoming.clear();
    gOutgoingBytes = 0;
    gIncomingBytes = 0;
    setLastErrorLocked("connecting");
    gInitialized = true;
    LightLock_Unlock(&gLock);

    s32 mainPriority = 0x30;
    svcGetThreadPriority(&mainPriority, CUR_THREAD_HANDLE);
    int workerPriority = std::min(0x3f, (int)mainPriority + 1);
    gWorker = threadCreate(networkWorker, NULL, NETWORK_WORKER_STACK_SIZE,
                           workerPriority, -2, false);
    if (!gWorker)
    {
        LightLock_Lock(&gLock);
        gInitialized = false;
        setLastErrorLocked("network worker creation failed");
        LightLock_Unlock(&gLock);
        TlsStream::shutdown();
        if (gAcInitialized)
        {
            acExit();
            gAcInitialized = false;
        }
        socExit();
        free(gSocBuffer);
        gSocBuffer = NULL;
        return false;
    }
    return true;
}

void NetworkManager::shutdown()
{
    if (!gSynchronizationReady)
        return;
    LightLock_Lock(&gLock);
    if (!gInitialized)
    {
        LightLock_Unlock(&gLock);
        return;
    }
    gStopRequested = true;
    gAutoReconnect = false;
    CondVar_WakeUp(&gCondition, ARBITRATION_SIGNAL_ALL);
    LightLock_Unlock(&gLock);

    if (gWorker)
    {
        threadJoin(gWorker, U64_MAX);
        threadFree(gWorker);
        gWorker = NULL;
    }
    LightLock_Lock(&gLock);
    gInitialized = false;
    gOutgoing.clear();
    gIncoming.clear();
    gOutgoingBytes = 0;
    gIncomingBytes = 0;
    LightLock_Unlock(&gLock);
    TlsStream::shutdown();
    if (gAcInitialized)
    {
        acExit();
        gAcInitialized = false;
    }
    socExit();
    free(gSocBuffer);
    gSocBuffer = NULL;
}

bool NetworkManager::sendText(const char *text)
{
    return text && sendText(text, strlen(text));
}

bool NetworkManager::sendText(const void *text, size_t length)
{
    return queueOutgoing(true, text, length);
}

bool NetworkManager::sendSessionHello(const void *text, size_t length)
{
    return queueOutgoing(true, text, length, true);
}

bool NetworkManager::sendBinary(const void *buffer, size_t length)
{
    return queueOutgoing(false, buffer, length);
}

bool NetworkManager::pollEvent(NetworkEvent &event)
{
    LightLock_Lock(&gLock);
    if (gIncoming.empty())
    {
        LightLock_Unlock(&gLock);
        return false;
    }
    event = std::move(gIncoming.front());
    gIncomingBytes -= event.payload.size();
    gIncoming.pop_front();
    LightLock_Unlock(&gLock);
    return true;
}

bool NetworkManager::waitEvent(NetworkEvent &event, int timeoutMs)
{
    u64 deadline = osGetTime() + (u64)std::max(0, timeoutMs);
    while (osGetTime() <= deadline)
    {
        if (pollEvent(event))
            return true;
        svcSleepThread(5000000LL);
    }
    return false;
}

bool NetworkManager::waitForConnected(int timeoutMs)
{
    u64 deadline = osGetTime() + (u64)std::max(0, timeoutMs);
    while (osGetTime() <= deadline)
    {
        LightLock_Lock(&gLock);
        bool connected = gInitialized && gConnected;
        LightLock_Unlock(&gLock);
        if (connected)
            return true;
        svcSleepThread(5000000LL);
    }
    return false;
}

bool NetworkManager::waitForDisconnected(int timeoutMs)
{
    u64 deadline = osGetTime() + (u64)std::max(0, timeoutMs);
    while (osGetTime() <= deadline)
    {
        LightLock_Lock(&gLock);
        bool idle = !gInitialized || (!gConnected && !gConnecting && !gDisconnectRequested);
        LightLock_Unlock(&gLock);
        if (idle)
            return true;
        svcSleepThread(5000000LL);
    }
    return false;
}

bool NetworkManager::disconnect()
{
    LightLock_Lock(&gLock);
    if (!gInitialized)
    {
        LightLock_Unlock(&gLock);
        return false;
    }
    gAutoReconnect = false;
    gDisconnectRequested = true;
    gSessionReady = false;
    gOutgoing.clear();
    gOutgoingBytes = 0;
    gIncoming.clear();
    gIncomingBytes = 0;
    CondVar_WakeUp(&gCondition, 1);
    LightLock_Unlock(&gLock);
    return true;
}

bool NetworkManager::reconnect()
{
    LightLock_Lock(&gLock);
    if (!gInitialized)
    {
        LightLock_Unlock(&gLock);
        return false;
    }
    gAutoReconnect = true;
    gReconnectRequested = true;
    gConnecting = true;
    gSessionReady = false;
    gOutgoing.clear();
    gOutgoingBytes = 0;
    // Anything already queued belongs to the socket generation being replaced.
    // The worker clears once more when it observes this request to close the
    // small producer/consumer race between this call and the next worker tick.
    gIncoming.clear();
    gIncomingBytes = 0;
    CondVar_WakeUp(&gCondition, 1);
    LightLock_Unlock(&gLock);
    return true;
}

bool NetworkManager::checkConnection()
{
    LightLock_Lock(&gLock);
    bool connected = gInitialized && gConnected && gSessionReady;
    LightLock_Unlock(&gLock);
    return connected;
}

bool NetworkManager::ensureConnected()
{
    return checkConnection() || reconnect();
}

bool NetworkManager::isConnected()
{
    return checkConnection();
}

bool NetworkManager::isConnecting()
{
    LightLock_Lock(&gLock);
    bool connecting = gInitialized && gConnecting;
    LightLock_Unlock(&gLock);
    return connecting;
}

const char *NetworkManager::lastError()
{
    LightLock_Lock(&gLock);
    snprintf(gLastErrorSnapshot, sizeof(gLastErrorSnapshot), "%s", gLastError);
    LightLock_Unlock(&gLock);
    return gLastErrorSnapshot;
}
