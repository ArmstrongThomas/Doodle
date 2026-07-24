#ifndef CANVAS_SYNC_H
#define CANVAS_SYNC_H

#include <stdint.h>
#include <string.h>

namespace Doodle
{

static const uint64_t CLIENT_MAX_CANVAS_BYTES = 12ULL * 1024ULL * 1024ULL;
static const int CLIENT_MAX_CANVAS_WIDTH = 1920;
static const int CLIENT_MAX_CANVAS_HEIGHT = 1080;
static const int CLIENT_MAX_COMPRESSED_CANVAS_BYTES = 10000000;

inline bool isSupportedCanvasSnapshot(int width, int height, int compressedSize)
{
    if (width <= 0 || height <= 0 ||
        width > CLIENT_MAX_CANVAS_WIDTH ||
        height > CLIENT_MAX_CANVAS_HEIGHT ||
        compressedSize <= 0 ||
        compressedSize > CLIENT_MAX_COMPRESSED_CANVAS_BYTES)
        return false;
    const uint64_t pixelBytes = (uint64_t)width * (uint64_t)height * 3ULL;
    return pixelBytes > 0 && pixelBytes <= CLIENT_MAX_CANVAS_BYTES;
}

inline uint64_t canvasSnapshotTimeoutMs(int compressedSize)
{
    static const uint64_t MIB = 1024ULL * 1024ULL;
    if (compressedSize <= 0)
        return 30000;
    const uint64_t compressedMib =
        ((uint64_t)compressedSize + MIB - 1ULL) / MIB;
    const uint64_t timeout = 30000ULL + compressedMib * 8000ULL;
    return timeout > 90000ULL ? 90000ULL : timeout;
}

inline bool shouldResetCanvasViewport(const char *currentChannel,
                                      const char *incomingChannel)
{
    return !currentChannel || !currentChannel[0] ||
           !incomingChannel || !incomingChannel[0] ||
           strcmp(currentChannel, incomingChannel) != 0;
}

} // namespace Doodle

#endif
