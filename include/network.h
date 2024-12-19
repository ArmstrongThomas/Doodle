// network.h
#ifndef NETWORK_H
#define NETWORK_H

#include <3ds.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstdlib>
#include <malloc.h>

#define SOC_ALIGN 0x1000
#define SOC_BUFFERSIZE 0x100000

class NetworkManager
{
private:
    static u32 *SOC_buffer;
    static int sock;

public:
    static bool initialize();
    static void shutdown();
    static bool readLine(int s, char *buffer, size_t maxlen);
    static bool readExact(int s, void *buf, size_t length);
    static int getSocket() { return sock; }
    static void setSocket(int s) { sock = s; }
};

#endif