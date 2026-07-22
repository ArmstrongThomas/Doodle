#include "tls_stream.h"

#include <3ds.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#include <new>

#include "cacert_pem.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/error.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/platform_time.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"

extern "C" mbedtls_ms_time_t mbedtls_ms_time(void)
{
    return (mbedtls_ms_time_t)osGetTime();
}

namespace
{
const int SOC_IN_PROGRESS = -26;
const char RNG_PERSONALIZATION[] = "CollabDoodle/TlsStream/1";

bool g_initialized = false;
bool g_sslcInitialized = false;
bool g_caInitialized = false;
mbedtls_x509_crt g_caChain;

const int ALLOWED_CIPHERSUITES[] = {
    MBEDTLS_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256,
    MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,
    0
};

const uint16_t ALLOWED_GROUPS[] = {
    MBEDTLS_SSL_IANA_TLS_GROUP_SECP256R1,
    MBEDTLS_SSL_IANA_TLS_GROUP_SECP384R1,
    MBEDTLS_SSL_IANA_TLS_GROUP_NONE
};

void copyError(char* destination, size_t capacity, const char* message)
{
    if (!destination || capacity == 0)
        return;
    snprintf(destination, capacity, "%s", message ? message : "TLS error");
}

void formatMbedError(char* destination, size_t capacity, const char* prefix, int code)
{
    char detail[128];
    detail[0] = '\0';
    mbedtls_strerror(code, detail, sizeof(detail));
    snprintf(destination, capacity, "%s: -0x%04X %s", prefix,
             (unsigned int)(-code), detail[0] ? detail : "unknown Mbed TLS error");
}

void formatVerifyError(char* destination, size_t capacity, uint32_t flags)
{
    if ((flags & (MBEDTLS_X509_BADCERT_EXPIRED | MBEDTLS_X509_BADCERT_FUTURE)) != 0)
    {
        copyError(destination, capacity,
                  "TLS certificate date check failed; verify the 3DS date and time");
        return;
    }

    char detail[128];
    detail[0] = '\0';
    mbedtls_x509_crt_verify_info(detail, sizeof(detail), "", flags);
    snprintf(destination, capacity, "TLS certificate verification failed: %s",
             detail[0] ? detail : "unknown certificate error");
}

bool transientSocketError(int value)
{
    return value == EAGAIN || value == EWOULDBLOCK || value == EINPROGRESS ||
           value == EINTR || value == SOC_IN_PROGRESS;
}

int systemEntropy(void*, unsigned char* output, size_t length, size_t* produced)
{
    if (produced)
        *produced = 0;
    if (!output || !produced || length > 0xFFFFFFFFu)
        return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;

    Result result = sslcGenerateRandomData(output, (u32)length);
    if (R_FAILED(result))
        return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;

    *produced = length;
    return 0;
}

u64 deadlineFromNow(int timeoutMs)
{
    return osGetTime() + (u64)(timeoutMs > 0 ? timeoutMs : 0);
}

int remainingMilliseconds(u64 deadline)
{
    u64 now = osGetTime();
    if (now >= deadline)
        return 0;
    u64 remaining = deadline - now;
    return remaining > 0x7FFFFFFFu ? 0x7FFFFFFF : (int)remaining;
}

bool waitForSocket(int socketFd, bool writeReady, u64 deadline)
{
    for (;;)
    {
        int remaining = remainingMilliseconds(deadline);
        if (remaining <= 0)
        {
            errno = ETIMEDOUT;
            return false;
        }

        fd_set readSet;
        fd_set writeSet;
        FD_ZERO(&readSet);
        FD_ZERO(&writeSet);
        if (writeReady)
            FD_SET(socketFd, &writeSet);
        else
            FD_SET(socketFd, &readSet);

        timeval timeout;
        timeout.tv_sec = remaining / 1000;
        timeout.tv_usec = (remaining % 1000) * 1000;
        int result = select(socketFd + 1, writeReady ? NULL : &readSet,
                            writeReady ? &writeSet : NULL, NULL, &timeout);
        if (result > 0)
            return true;
        if (result == 0)
        {
            errno = ETIMEDOUT;
            return false;
        }
        if (errno != EINTR)
            return false;
    }
}

int socketSend(void* context, const unsigned char* buffer, size_t length)
{
    int socketFd = context ? *static_cast<int*>(context) : -1;
    if (socketFd < 0)
        return MBEDTLS_ERR_NET_INVALID_CONTEXT;

    int result = send(socketFd, buffer, length, MSG_NOSIGNAL | MSG_DONTWAIT);
    if (result >= 0)
        return result;
    if (transientSocketError(errno))
        return MBEDTLS_ERR_SSL_WANT_WRITE;
    if (errno == EPIPE || errno == ECONNRESET || errno == ENOTCONN)
        return MBEDTLS_ERR_NET_CONN_RESET;
    return MBEDTLS_ERR_NET_SEND_FAILED;
}

int socketReceive(void* context, unsigned char* buffer, size_t length)
{
    int socketFd = context ? *static_cast<int*>(context) : -1;
    if (socketFd < 0)
        return MBEDTLS_ERR_NET_INVALID_CONTEXT;

    int result = recv(socketFd, buffer, length, MSG_DONTWAIT);
    if (result >= 0)
        return result;
    if (transientSocketError(errno))
        return MBEDTLS_ERR_SSL_WANT_READ;
    if (errno == ECONNRESET || errno == ENOTCONN)
        return MBEDTLS_ERR_NET_CONN_RESET;
    return MBEDTLS_ERR_NET_RECV_FAILED;
}
}

struct TlsStream::Impl
{
    int socketFd;
    bool open;
    bool rngSeeded;
    bool entropySourceAdded;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config config;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctrDrbg;

    Impl() : socketFd(-1), open(false), rngSeeded(false), entropySourceAdded(false)
    {
        mbedtls_ssl_init(&ssl);
        mbedtls_ssl_config_init(&config);
        mbedtls_entropy_init(&entropy);
        mbedtls_ctr_drbg_init(&ctrDrbg);
    }

    ~Impl()
    {
        mbedtls_ssl_free(&ssl);
        mbedtls_ssl_config_free(&config);
        mbedtls_ctr_drbg_free(&ctrDrbg);
        mbedtls_entropy_free(&entropy);
    }

    void resetTls()
    {
        mbedtls_ssl_free(&ssl);
        mbedtls_ssl_config_free(&config);
        mbedtls_ssl_init(&ssl);
        mbedtls_ssl_config_init(&config);
    }

    bool seedRng(char* error, size_t errorSize)
    {
        if (rngSeeded)
            return true;
        if (!g_initialized)
        {
            copyError(error, errorSize, "TLS global state is not initialized");
            return false;
        }

        int result = 0;
        if (!entropySourceAdded)
        {
            result = mbedtls_entropy_add_source(&entropy, systemEntropy, NULL, 32,
                                                MBEDTLS_ENTROPY_SOURCE_STRONG);
            if (result != 0)
            {
                formatMbedError(error, errorSize, "Could not register the 3DS RNG", result);
                return false;
            }
            entropySourceAdded = true;
        }

        result = mbedtls_ctr_drbg_seed(&ctrDrbg, mbedtls_entropy_func, &entropy,
                                      reinterpret_cast<const unsigned char*>(RNG_PERSONALIZATION),
                                      sizeof(RNG_PERSONALIZATION) - 1);
        if (result != 0)
        {
            formatMbedError(error, errorSize, "Could not seed the TLS DRBG", result);
            return false;
        }

        rngSeeded = true;
        return true;
    }
};

TlsStream::TlsStream() : impl_(new (std::nothrow) Impl())
{
    lastError_[0] = '\0';
    if (!impl_)
        copyError(lastError_, sizeof(lastError_), "Out of memory creating TLS stream");
}

TlsStream::~TlsStream()
{
    close();
    delete impl_;
    impl_ = NULL;
}

bool TlsStream::initialize()
{
    if (g_initialized)
        return true;

    Result sslResult = sslcInit(0);
    if (R_FAILED(sslResult))
    {
        printf("sslcInit RNG service failed: 0x%08lX\n", (unsigned long)sslResult);
        return false;
    }
    g_sslcInitialized = true;

    mbedtls_x509_crt_init(&g_caChain);
    g_caInitialized = true;

    unsigned char* nullTerminatedBundle =
        static_cast<unsigned char*>(malloc(cacert_pem_size + 1));
    if (!nullTerminatedBundle)
    {
        printf("TLS CA bundle allocation failed\n");
        shutdown();
        return false;
    }
    memcpy(nullTerminatedBundle, cacert_pem, cacert_pem_size);
    nullTerminatedBundle[cacert_pem_size] = '\0';

    int parseResult = mbedtls_x509_crt_parse(&g_caChain, nullTerminatedBundle,
                                             cacert_pem_size + 1);
    free(nullTerminatedBundle);
    if (parseResult != 0)
    {
        char detail[128];
        detail[0] = '\0';
        if (parseResult < 0)
            mbedtls_strerror(parseResult, detail, sizeof(detail));
        printf("TLS CA bundle parse failed: %d %s\n", parseResult, detail);
        shutdown();
        return false;
    }

    // Prove the service produces entropy now, rather than discovering a broken
    // RNG only during the first connection handshake.
    unsigned char probe[32];
    Result randomResult = sslcGenerateRandomData(probe, sizeof(probe));
    memset(probe, 0, sizeof(probe));
    if (R_FAILED(randomResult))
    {
        printf("3DS RNG self-check failed: 0x%08lX\n", (unsigned long)randomResult);
        shutdown();
        return false;
    }

    g_initialized = true;
    return true;
}

void TlsStream::shutdown()
{
    g_initialized = false;
    if (g_caInitialized)
    {
        mbedtls_x509_crt_free(&g_caChain);
        g_caInitialized = false;
    }
    if (g_sslcInitialized)
    {
        sslcExit();
        g_sslcInitialized = false;
    }
}

bool TlsStream::connect(const char* host, const char* port, int timeoutMs)
{
    lastError_[0] = '\0';
    if (!impl_)
    {
        copyError(lastError_, sizeof(lastError_), "TLS stream is unavailable");
        return false;
    }
    close();
    if (!g_initialized || !g_caInitialized)
    {
        copyError(lastError_, sizeof(lastError_), "TLS global state is not initialized");
        return false;
    }
    if (!host || !host[0] || !port || !port[0] || timeoutMs <= 0)
    {
        copyError(lastError_, sizeof(lastError_), "Invalid TLS connection parameters");
        return false;
    }
    if (!impl_->seedRng(lastError_, sizeof(lastError_)))
        return false;

    u64 deadline = deadlineFromNow(timeoutMs);
    addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* addresses = NULL;
    int resolveResult = getaddrinfo(host, port, &hints, &addresses);
    if (resolveResult != 0)
    {
        snprintf(lastError_, sizeof(lastError_), "DNS lookup failed for %s: %s", host,
                 gai_strerror(resolveResult));
        return false;
    }

    int lastSocketError = ECONNREFUSED;
    for (addrinfo* address = addresses; address; address = address->ai_next)
    {
        if (remainingMilliseconds(deadline) <= 0)
        {
            lastSocketError = ETIMEDOUT;
            break;
        }

        int socketFd = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (socketFd < 0)
        {
            lastSocketError = errno;
            continue;
        }

        int flags = fcntl(socketFd, F_GETFL, 0);
        if (flags < 0 || fcntl(socketFd, F_SETFL, flags | O_NONBLOCK) < 0)
        {
            lastSocketError = errno;
            ::close(socketFd);
            continue;
        }

        int connectResult = ::connect(socketFd, address->ai_addr, address->ai_addrlen);
        bool connected = connectResult == 0;
        if (!connected && (connectResult == SOC_IN_PROGRESS || transientSocketError(errno)))
        {
            if (waitForSocket(socketFd, true, deadline))
            {
                int socketError = 0;
                socklen_t socketErrorLength = sizeof(socketError);
                if (getsockopt(socketFd, SOL_SOCKET, SO_ERROR, &socketError,
                               &socketErrorLength) == 0 &&
                    (socketError == 0 || socketError == EISCONN ||
                     socketError == SOC_IN_PROGRESS))
                    connected = true;
                else
                    lastSocketError = socketError ? socketError : errno;
            }
            else
                lastSocketError = errno;
        }
        else if (!connected)
            lastSocketError = errno;

        if (!connected)
        {
            ::close(socketFd);
            continue;
        }

        int enabled = 1;
        setsockopt(socketFd, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled));
        impl_->socketFd = socketFd;
        break;
    }
    freeaddrinfo(addresses);

    if (impl_->socketFd < 0)
    {
        snprintf(lastError_, sizeof(lastError_), "TCP connection to %s:%s failed: %d %s",
                 host, port, lastSocketError, strerror(lastSocketError));
        return false;
    }

    int result = mbedtls_ssl_config_defaults(&impl_->config, MBEDTLS_SSL_IS_CLIENT,
                                             MBEDTLS_SSL_TRANSPORT_STREAM,
                                             MBEDTLS_SSL_PRESET_DEFAULT);
    if (result == 0)
    {
        mbedtls_ssl_conf_authmode(&impl_->config, MBEDTLS_SSL_VERIFY_REQUIRED);
        mbedtls_ssl_conf_ca_chain(&impl_->config, &g_caChain, NULL);
        mbedtls_ssl_conf_rng(&impl_->config, mbedtls_ctr_drbg_random, &impl_->ctrDrbg);
        mbedtls_ssl_conf_ciphersuites(&impl_->config, ALLOWED_CIPHERSUITES);
        mbedtls_ssl_conf_groups(&impl_->config, ALLOWED_GROUPS);
        mbedtls_ssl_conf_min_tls_version(&impl_->config, MBEDTLS_SSL_VERSION_TLS1_2);
        mbedtls_ssl_conf_max_tls_version(&impl_->config, MBEDTLS_SSL_VERSION_TLS1_2);
        result = mbedtls_ssl_setup(&impl_->ssl, &impl_->config);
    }
    if (result == 0)
        result = mbedtls_ssl_set_hostname(&impl_->ssl, host);
    if (result != 0)
    {
        formatMbedError(lastError_, sizeof(lastError_), "TLS setup failed", result);
        close();
        return false;
    }

    mbedtls_ssl_set_bio(&impl_->ssl, &impl_->socketFd, socketSend, socketReceive, NULL);
    for (;;)
    {
        result = mbedtls_ssl_handshake(&impl_->ssl);
        if (result == 0)
            break;
        if (result != MBEDTLS_ERR_SSL_WANT_READ && result != MBEDTLS_ERR_SSL_WANT_WRITE)
        {
            uint32_t verifyFlags = mbedtls_ssl_get_verify_result(&impl_->ssl);
            if (verifyFlags != 0)
                formatVerifyError(lastError_, sizeof(lastError_), verifyFlags);
            else
                formatMbedError(lastError_, sizeof(lastError_), "TLS handshake failed", result);
            close();
            return false;
        }
        if (!waitForSocket(impl_->socketFd, result == MBEDTLS_ERR_SSL_WANT_WRITE, deadline))
        {
            snprintf(lastError_, sizeof(lastError_),
                     "TLS handshake timed out at state %d",
                     impl_->ssl.MBEDTLS_PRIVATE(state));
            close();
            return false;
        }
    }

    uint32_t verifyFlags = mbedtls_ssl_get_verify_result(&impl_->ssl);
    if (verifyFlags != 0)
    {
        formatVerifyError(lastError_, sizeof(lastError_), verifyFlags);
        close();
        return false;
    }

    impl_->open = true;
    return true;
}

TlsStream::IoResult TlsStream::read(void* buffer, size_t capacity, size_t& bytesRead)
{
    bytesRead = 0;
    if (!impl_ || !impl_->open || impl_->socketFd < 0)
        return IO_CLOSED;
    if (!buffer || capacity == 0)
    {
        copyError(lastError_, sizeof(lastError_), "Invalid TLS read buffer");
        return IO_ERROR;
    }

    int result = mbedtls_ssl_read(&impl_->ssl, static_cast<unsigned char*>(buffer), capacity);
    if (result > 0)
    {
        bytesRead = (size_t)result;
        return IO_OK;
    }
    if (result == MBEDTLS_ERR_SSL_WANT_READ || result == MBEDTLS_ERR_SSL_WANT_WRITE)
        return IO_WOULD_BLOCK;
    if (result == 0 || result == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY ||
        result == MBEDTLS_ERR_SSL_CONN_EOF)
    {
        close();
        return IO_CLOSED;
    }

    formatMbedError(lastError_, sizeof(lastError_), "TLS read failed", result);
    close();
    return IO_ERROR;
}

TlsStream::IoResult TlsStream::write(const void* buffer, size_t length, size_t& bytesWritten)
{
    bytesWritten = 0;
    if (!impl_ || !impl_->open || impl_->socketFd < 0)
        return IO_CLOSED;
    if (!buffer || length == 0)
    {
        copyError(lastError_, sizeof(lastError_), "Invalid TLS write buffer");
        return IO_ERROR;
    }

    size_t requestLength = length;
    if (requestLength > MBEDTLS_SSL_OUT_CONTENT_LEN)
        requestLength = MBEDTLS_SSL_OUT_CONTENT_LEN;
    int result = mbedtls_ssl_write(&impl_->ssl,
                                   static_cast<const unsigned char*>(buffer), requestLength);
    if (result > 0)
    {
        bytesWritten = (size_t)result;
        return IO_OK;
    }
    if (result == MBEDTLS_ERR_SSL_WANT_READ || result == MBEDTLS_ERR_SSL_WANT_WRITE)
        return IO_WOULD_BLOCK;
    if (result == 0 || result == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY ||
        result == MBEDTLS_ERR_SSL_CONN_EOF)
    {
        close();
        return IO_CLOSED;
    }

    formatMbedError(lastError_, sizeof(lastError_), "TLS write failed", result);
    close();
    return IO_ERROR;
}

void TlsStream::close()
{
    if (!impl_)
        return;
    if (impl_->socketFd >= 0)
    {
        if (impl_->open)
            mbedtls_ssl_close_notify(&impl_->ssl);
        ::shutdown(impl_->socketFd, SHUT_RDWR);
        ::close(impl_->socketFd);
        impl_->socketFd = -1;
    }
    impl_->open = false;
    impl_->resetTls();
}

bool TlsStream::isOpen() const
{
    return impl_ && impl_->open && impl_->socketFd >= 0;
}

const char* TlsStream::lastError() const
{
    return lastError_[0] ? lastError_ : "";
}

bool TlsStream::randomBytes(void* buffer, size_t length)
{
    lastError_[0] = '\0';
    if (length == 0)
        return true;
    if (!impl_ || !buffer)
    {
        copyError(lastError_, sizeof(lastError_), "Invalid random-byte request");
        return false;
    }
    if (!impl_->seedRng(lastError_, sizeof(lastError_)))
        return false;

    unsigned char* output = static_cast<unsigned char*>(buffer);
    size_t offset = 0;
    while (offset < length)
    {
        size_t chunk = length - offset;
        if (chunk > MBEDTLS_CTR_DRBG_MAX_REQUEST)
            chunk = MBEDTLS_CTR_DRBG_MAX_REQUEST;
        int result = mbedtls_ctr_drbg_random(&impl_->ctrDrbg, output + offset, chunk);
        if (result != 0)
        {
            formatMbedError(lastError_, sizeof(lastError_), "TLS random generation failed", result);
            return false;
        }
        offset += chunk;
    }
    return true;
}
