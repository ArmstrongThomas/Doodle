#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <3ds.h>
#include <stddef.h>

struct CanvasMeta {
    int width;
    int height;
    int compressedSize;
    char channel[25];
};

class Protocol {
public:
    static bool parseCanvasMeta(const char *line, CanvasMeta &meta);
    static bool parseChannels(const char *line, char channels[][25], int maxChannels, int &count, char *currentChannel);
    static void buildSwitchChannel(char *buffer, size_t size, const char *channel);
    static void buildUpdateRequest(char *buffer, size_t size);
};

#endif
