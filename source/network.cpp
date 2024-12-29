#include "network.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <netdb.h>

// Static member initialization
u32* NetworkManager::SOC_buffer = nullptr;
int NetworkManager::sock = -1;
bool NetworkManager::isConnected = false;
// const char* NetworkManager::SERVER_DOMAIN = "10.0.0.166";
const char* NetworkManager::SERVER_DOMAIN = "server1.rpgwo.org";
const char* NetworkManager::SERVER_PORT = "3030";

bool NetworkManager::initialize() {
    printf("Initializing network...\n");
    
    // Allocate buffer for SOC service
    SOC_buffer = (u32*)memalign(SOC_ALIGN, SOC_BUFFERSIZE);
    if (SOC_buffer == nullptr) {
        printf("memalign: failed to allocate\n");
        return false;
    }

    // Initialize SOC service
    if (socInit(SOC_buffer, SOC_BUFFERSIZE) != 0) {
        printf("socInit failed\n");
        return false;
    }

    return connect(SERVER_DOMAIN, SERVER_PORT);
}

bool NetworkManager::connect(const char* server_domain, const char* server_port_str) {
    // Create socket
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        printf("socket: %d %s\n", errno, strerror(errno));
        return false;
    }

    struct addrinfo hints;
    struct addrinfo *result;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    printf("Resolving domain name... ");
    int status = getaddrinfo(server_domain, server_port_str, &hints, &result);
    if (status != 0) {
        printf("Failed to resolve domain: %s\n", gai_strerror(status));
        return false;
    }

    printf("Connecting to server... ");
    bool connected = false;
    for (struct addrinfo *rp = result; rp != NULL; rp = rp->ai_next) {
        if (::connect(sock, rp->ai_addr, rp->ai_addrlen) == 0) { 
            connected = true;
            break;
        }
    }

    freeaddrinfo(result);

    if (!connected) {
        printf("Failed to connect: %d %s\n", errno, strerror(errno));
        close(sock);
        sock = -1;
        isConnected = false;
        return false;
    }

    printf("Connected!\n");
    isConnected = true;
    return true;
}

bool NetworkManager::reconnect() {
    if (sock >= 0) {
        disconnect();
    }
    return connect(SERVER_DOMAIN, SERVER_PORT);
}

bool NetworkManager::disconnect() {
    if (sock >= 0) {
        printf("Disconnecting from server...\n");
        // First try graceful shutdown
        if (::shutdown(sock, SHUT_RDWR) == 0) {
            // Wait briefly for any pending data
            char dummy[32];
            recv(sock, dummy, sizeof(dummy), MSG_DONTWAIT);
        }
        
        // Close socket regardless of shutdown result
        close(sock);
        sock = -1;
        isConnected = false;
        printf("Disconnected!\n");
        return true;
    }
    return false;
}

void NetworkManager::shutdown() {
    printf("Shutting down network...\n");
    
    // Make sure to disconnect first
    if (isConnected || sock >= 0) {
        disconnect();
    }
    
    // Clean up SOC service
    socExit();
    
    // Free buffer last
    if (SOC_buffer) {
        free(SOC_buffer);
        SOC_buffer = nullptr;
    }
}

bool NetworkManager::checkConnection() {
    if (!isConnected || sock < 0) return false;
    
    // Try to send empty packet to check connection
    char ping = 0;
    int result = send(sock, &ping, 0, MSG_NOSIGNAL);
    if (result < 0 && (errno == EPIPE || errno == ENOTCONN)) {
        printf("Connection lost!\n");
        disconnect();
        return false;
    }
    return true;
}

bool NetworkManager::ensureConnected() {
    if (!checkConnection()) {
        printf("Attempting to reconnect...\n");
        return reconnect();
    }
    return true;
}

bool NetworkManager::readLine(int s, char* buffer, size_t maxlen) {
    if (!checkConnection()) return false;

    int flags = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, flags & ~O_NONBLOCK);

    size_t pos = 0;
    char c;
    while (pos < maxlen - 1) {
        int ret = recv(s, &c, 1, 0);
        if (ret <= 0) {
            fcntl(s, F_SETFL, flags);
            return false;
        }
        if (c == '\n') {
            buffer[pos] = '\0';
            fcntl(s, F_SETFL, flags);
            return true;
        }
        buffer[pos++] = c;
    }

    fcntl(s, F_SETFL, flags);
    buffer[pos] = '\0';
    return true;
}

bool NetworkManager::readExact(int s, void* buf, size_t length) {
    if (!checkConnection()) return false;

    int flags = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, flags & ~O_NONBLOCK);

    size_t received = 0;
    char* ptr = (char*)buf;
    while (received < length) {
        int ret = recv(s, ptr + received, length - received, 0);
        if (ret <= 0) {
            fcntl(s, F_SETFL, flags);
            return false;
        }
        received += ret;
    }

    fcntl(s, F_SETFL, flags);
    return true;
}