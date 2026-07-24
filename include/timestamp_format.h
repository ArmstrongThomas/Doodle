#ifndef DOODLE_TIMESTAMP_FORMAT_H
#define DOODLE_TIMESTAMP_FORMAT_H

#include <stddef.h>
#include <stdio.h>
#include <string.h>

namespace Doodle
{

inline bool timestampDigit(char value)
{
    return value >= '0' && value <= '9';
}

inline bool formatIsoMinuteTimestamp(const char *timestamp,
                                     char *output, size_t outputSize)
{
    if (!output || outputSize == 0)
        return false;
    output[0] = '\0';
    if (!timestamp || strlen(timestamp) < 16 ||
        !timestampDigit(timestamp[0]) ||
        !timestampDigit(timestamp[1]) ||
        !timestampDigit(timestamp[2]) ||
        !timestampDigit(timestamp[3]) ||
        timestamp[4] != '-' ||
        !timestampDigit(timestamp[5]) ||
        !timestampDigit(timestamp[6]) ||
        timestamp[7] != '-' ||
        !timestampDigit(timestamp[8]) ||
        !timestampDigit(timestamp[9]) ||
        (timestamp[10] != 'T' && timestamp[10] != ' ') ||
        !timestampDigit(timestamp[11]) ||
        !timestampDigit(timestamp[12]) ||
        timestamp[13] != ':' ||
        !timestampDigit(timestamp[14]) ||
        !timestampDigit(timestamp[15]))
    {
        snprintf(output, outputSize, "Unknown date");
        return false;
    }

    snprintf(output, outputSize,
             "%.4s-%.2s-%.2s %.2s:%.2sZ",
             timestamp, timestamp + 5, timestamp + 8,
             timestamp + 11, timestamp + 14);
    return true;
}

} // namespace Doodle

#endif
