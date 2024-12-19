#include "network.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>

// Static member initialization
u32* NetworkManager::SOC_buffer = nullptr;
int NetworkManager::sock = -1;

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

    // Create socket
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        printf("socket: %d %s\n", errno, strerror(errno));
        return false;
    }

    // Connect to server
    const char* server_ip = "38.45.65.90";
    int server_port = 3030;

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(server_port);
    inet_pton(AF_INET, server_ip, &sa.sin_addr);

    printf("Connecting to server... ");
    if (connect(sock, (struct sockaddr*)&sa, sizeof(sa)) < 0) {
        printf("Failed to connect: %d %s\n", errno, strerror(errno));
        close(sock);
        sock = -1;
        return false;
    }

    printf("Connected!\n");
    return true;
}

void NetworkManager::shutdown() {
    printf("Shutting down network...\n");
    if (sock >= 0) {
        close(sock);
        sock = -1;
    }
    socExit();
    if (SOC_buffer) {
        free(SOC_buffer);
        SOC_buffer = nullptr;
    }
}

bool NetworkManager::readLine(int s, char* buffer, size_t maxlen) {
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