#ifndef TLS_STREAM_H
#define TLS_STREAM_H

#include <stddef.h>

// A TLS 1.2 client stream backed by a nonblocking socket. Global TLS state must
// be initialized after socInit() and shut down before socExit(). Each instance
// is intended to be owned by one worker/thread at a time.
class TlsStream
{
public:
    enum IoResult
    {
        IO_OK,
        IO_WOULD_BLOCK,
        IO_CLOSED,
        IO_ERROR
    };

    TlsStream();
    ~TlsStream();

    // Idempotent global setup/teardown for the 3DS system RNG and embedded CA
    // trust store. initialize() returns false on any setup or CA parsing error.
    static bool initialize();
    static void shutdown();

    // TCP connect and the verified TLS handshake share timeoutMs and should
    // run on a network/update worker. DNS uses the platform resolver.
    bool connect(const char* host, const char* port, int timeoutMs);

    // Once connected, I/O is nonblocking and may complete only part of the
    // supplied buffer. A write retried after IO_WOULD_BLOCK must use the same
    // buffer and length until Mbed TLS reports progress.
    IoResult read(void* buffer, size_t capacity, size_t& bytesRead);
    IoResult write(const void* buffer, size_t length, size_t& bytesWritten);

    void close();
    bool isOpen() const;
    const char* lastError() const;

    // Cryptographically secure bytes from a per-stream DRBG seeded exclusively
    // by the checked 3DS system RNG service.
    bool randomBytes(void* buffer, size_t length);

private:
    TlsStream(const TlsStream&);
    TlsStream& operator=(const TlsStream&);

    struct Impl;
    Impl* impl_;
    char lastError_[192];
};

#endif
