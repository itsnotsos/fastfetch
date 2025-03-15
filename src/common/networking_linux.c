#include "fastfetch.h"
#include "common/networking.h"
#include "common/time.h"
#include "common/library.h"
#include "util/stringUtils.h"
#include "util/mallocHelper.h"
#include "util/debug.h"

#include <unistd.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h> // For FreeBSD
#include <netinet/tcp.h>
#include <errno.h>
#include <fcntl.h>

// Try to use TCP Fast Open to send data
static const char* tryTcpFastOpen(FFNetworkingState* state)
{
    #if !defined(MSG_FASTOPEN) || !defined(TCP_FASTOPEN)
        FF_DEBUG("TCP Fast Open not supported on this system");
        FF_UNUSED(state);
        return "TCP Fast Open not supported";
    #else
        FF_DEBUG("Attempting to use TCP Fast Open to connect to %s", state->host.chars);

        // Set TCP Fast Open
        int qlen = 5;
        if (setsockopt(state->sockfd, IPPROTO_TCP, TCP_FASTOPEN, &qlen, sizeof(qlen)) != 0) {
            FF_DEBUG("Failed to set TCP_FASTOPEN option: %s", strerror(errno));
        } else {
            FF_DEBUG("Successfully set TCP_FASTOPEN option, queue length: %d", qlen);
        }

        // Set non-blocking mode
        int flags = fcntl(state->sockfd, F_GETFL, 0);
        FF_DEBUG("Current socket flags: 0x%x", flags);

        if (fcntl(state->sockfd, F_SETFL, flags | O_NONBLOCK) < 0) {
            FF_DEBUG("Failed to set non-blocking mode: %s", strerror(errno));
        } else {
            FF_DEBUG("Successfully set non-blocking mode");
        }

        // Try to send data using Fast Open
        FF_DEBUG("Using sendto() + MSG_FASTOPEN to send %u bytes of data", state->command.length);
        ssize_t sent = sendto(state->sockfd,
                             state->command.chars,
                             state->command.length,
                             MSG_FASTOPEN,
                             state->addr->ai_addr,
                             state->addr->ai_addrlen);

        // Restore blocking mode
        fcntl(state->sockfd, F_SETFL, flags);

        if (sent >= 0 || (errno == EINPROGRESS || errno == EAGAIN || errno == EWOULDBLOCK))
        {
            FF_DEBUG("TCP Fast Open succeeded or in progress (sent=%zd, errno=%d: %s)",
                     sent, errno, sent < 0 ? strerror(errno) : "");
            freeaddrinfo(state->addr);
            state->addr = NULL;
            ffStrbufDestroy(&state->host);
            ffStrbufDestroy(&state->command);
            return NULL;
        }

        // Fast Open failed
        FF_DEBUG("TCP Fast Open failed: %s (errno=%d)", strerror(errno), errno);
        return "sendto() failed";
    #endif
}

// Traditional connect and send function
static const char* connectAndSend(FFNetworkingState* state)
{
    const char* ret = NULL;
    FF_DEBUG("Using traditional connection method to connect to %s", state->host.chars);

    FF_DEBUG("Attempting connect() to server...");
    if(connect(state->sockfd, state->addr->ai_addr, state->addr->ai_addrlen) == -1)
    {
        FF_DEBUG("connect() failed: %s (errno=%d)", strerror(errno), errno);
        ret = "connect() failed";
        goto error;
    }
    FF_DEBUG("connect() succeeded");

    FF_DEBUG("Attempting to send %u bytes of data...", state->command.length);
    if(send(state->sockfd, state->command.chars, state->command.length, 0) < 0)
    {
        FF_DEBUG("send() failed: %s (errno=%d)", strerror(errno), errno);
        ret = "send() failed";
        goto error;
    }
    FF_DEBUG("Data sent successfully");

    goto exit;

error:
    FF_DEBUG("Error occurred, closing socket");
    close(state->sockfd);
    state->sockfd = -1;

exit:
    FF_DEBUG("Releasing address info and other resources");
    freeaddrinfo(state->addr);
    state->addr = NULL;
    ffStrbufDestroy(&state->host);
    ffStrbufDestroy(&state->command);

    return ret;
}

FF_THREAD_ENTRY_DECL_WRAPPER(connectAndSend, FFNetworkingState*);

// Parallel DNS resolution and socket creation
static const char* initNetworkingState(FFNetworkingState* state, const char* host, const char* path, const char* headers)
{
    FF_DEBUG("Initializing network connection state: host=%s, path=%s", host, path);

    // Initialize command and host information
    ffStrbufInitS(&state->host, host);

    ffStrbufInitA(&state->command, 64);
    ffStrbufAppendS(&state->command, "GET ");
    ffStrbufAppendS(&state->command, path);
    ffStrbufAppendS(&state->command, " HTTP/1.1\nHost: ");
    ffStrbufAppendS(&state->command, host);
    ffStrbufAppendS(&state->command, "\r\n");

    // Add extra optimized HTTP headers
    ffStrbufAppendS(&state->command, "Connection: close\r\n"); // Explicitly tell the server we don't need to keep the connection

    // If compression needs to be enabled
    if (state->compression) {
        FF_DEBUG("Enabling HTTP content compression");
        ffStrbufAppendS(&state->command, "Accept-Encoding: gzip\r\n");
    }

    ffStrbufAppendS(&state->command, headers);
    ffStrbufAppendS(&state->command, "\r\n");

    #ifdef FF_HAVE_THREADS
    state->thread = 0;
    FF_DEBUG("Thread ID initialized to 0");
    #endif

    const char* ret = NULL;

    struct addrinfo hints = {
        .ai_family = state->ipv6 ? AF_INET6 : AF_INET,
        .ai_socktype = SOCK_STREAM,
        .ai_flags = AI_NUMERICSERV
    };

    FF_DEBUG("Resolving address: %s (%s)", host, state->ipv6 ? "IPv6" : "IPv4");
    // Use AI_NUMERICSERV flag to indicate the service is a numeric port, reducing parsing time

    if(getaddrinfo(host, "80", &hints, &state->addr) != 0)
    {
        FF_DEBUG("getaddrinfo() failed: %s", gai_strerror(errno));
        ret = "getaddrinfo() failed";
        goto error;
    }
    FF_DEBUG("Address resolution successful");

    FF_DEBUG("Creating socket");
    state->sockfd = socket(state->addr->ai_family, state->addr->ai_socktype, state->addr->ai_protocol);
    if(state->sockfd == -1)
    {
        FF_DEBUG("socket() failed: %s (errno=%d)", strerror(errno), errno);
        ret = "socket() failed";
        goto error;
    }
    FF_DEBUG("Socket creation successful: fd=%d", state->sockfd);

    int flag = 1;
    #ifdef TCP_NODELAY
    // Disable Nagle's algorithm to reduce small packet transmission delay
    if (setsockopt(state->sockfd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag)) != 0) {
        FF_DEBUG("Failed to set TCP_NODELAY: %s", strerror(errno));
    } else {
        FF_DEBUG("Successfully disabled Nagle's algorithm");
    }
    #endif

    #ifdef TCP_QUICKACK
    // Set TCP_QUICKACK option to avoid delayed acknowledgments
    if (setsockopt(state->sockfd, IPPROTO_TCP, TCP_QUICKACK, &flag, sizeof(flag)) != 0) {
        FF_DEBUG("Failed to set TCP_QUICKACK: %s", strerror(errno));
    } else {
        FF_DEBUG("Successfully enabled TCP quick acknowledgment");
    }
    #endif

    if (state->timeout > 0)
    {
        FF_DEBUG("Setting connection timeout: %u ms", state->timeout);
        FF_MAYBE_UNUSED uint32_t sec = state->timeout / 1000;
        if (sec == 0) sec = 1;

        #ifdef TCP_CONNECTIONTIMEOUT
        FF_DEBUG("Using TCP_CONNECTIONTIMEOUT: %u seconds", sec);
        setsockopt(state->sockfd, IPPROTO_TCP, TCP_CONNECTIONTIMEOUT, &sec, sizeof(sec));
        #elif defined(TCP_KEEPINIT)
        FF_DEBUG("Using TCP_KEEPINIT: %u seconds", sec);
        setsockopt(state->sockfd, IPPROTO_TCP, TCP_KEEPINIT, &sec, sizeof(sec));
        #elif defined(TCP_USER_TIMEOUT)
        FF_DEBUG("Using TCP_USER_TIMEOUT: %u milliseconds", state->timeout);
        setsockopt(state->sockfd, IPPROTO_TCP, TCP_USER_TIMEOUT, &state->timeout, sizeof(state->timeout));
        #else
        FF_DEBUG("Current platform does not support TCP connection timeout");
        #endif
    }

    return NULL;

error:
    FF_DEBUG("Error occurred during initialization");
    if (state->addr != NULL)
    {
        FF_DEBUG("Releasing address information");
        freeaddrinfo(state->addr);
        state->addr = NULL;
    }

    if (state->sockfd > 0)
    {
        FF_DEBUG("Closing socket: fd=%d", state->sockfd);
        close(state->sockfd);
        state->sockfd = -1;
    }
    return ret;
}

#ifdef FF_HAVE_ZLIB
#include <zlib.h>

struct FFZlibLibrary
{
    FF_LIBRARY_SYMBOL(inflateInit2_)
    FF_LIBRARY_SYMBOL(inflate)
    FF_LIBRARY_SYMBOL(inflateEnd)
    FF_LIBRARY_SYMBOL(inflateGetHeader)

    bool inited;
} zlibData;

static const char* loadZlibLibrary(void)
{
    if (!zlibData.inited)
    {
        zlibData.inited = true;
        FF_LIBRARY_LOAD(zlib, "dlopen libz failed", "libz" FF_LIBRARY_EXTENSION, 2)
        FF_LIBRARY_LOAD_SYMBOL_VAR_MESSAGE(zlib, zlibData, inflateInit2_)
        FF_LIBRARY_LOAD_SYMBOL_VAR_MESSAGE(zlib, zlibData, inflate)
        FF_LIBRARY_LOAD_SYMBOL_VAR_MESSAGE(zlib, zlibData, inflateEnd)
        FF_LIBRARY_LOAD_SYMBOL_VAR_MESSAGE(zlib, zlibData, inflateGetHeader)
        zlib = NULL; // don't auto dlclose
    }
    return zlibData.ffinflateEnd == NULL ? "Failed to load libz" : NULL;
}

// Try to pre-read gzip header to determine uncompressed size
static uint32_t guessGzipOutputSize(const void* data, uint32_t dataSize)
{
    // gzip file format: http://www.zlib.org/rfc-gzip.html
    if (dataSize < 10 || ((const uint8_t*)data)[0] != 0x1f || ((const uint8_t*)data)[1] != 0x8b)
        return 0;

    // Uncompressed size in gzip format is stored in the last 4 bytes, but only valid if data is less than 4GB
    if (dataSize > 18) {
        // Get ISIZE value from the end of file (little endian)
        const uint8_t* tail = (const uint8_t*)data + dataSize - 4;
        uint32_t uncompressedSize = tail[0] | (tail[1] << 8) | (tail[2] << 16) | (tail[3] << 24);

        // For valid gzip files, this value is the length of the uncompressed data modulo 2^32
        if (uncompressedSize > 0) {
            FF_DEBUG("Read uncompressed size from GZIP trailer: %u bytes", uncompressedSize);
            // Add some margin to the estimated size for safety
            return uncompressedSize + 64;
        }
    }

    // If unable to get size from trailer or size is 0, use estimated value
    // Typically, text data compression ratio is between 3-5x, we use the larger value
    uint32_t estimatedSize = dataSize * 5;
    FF_DEBUG("Unable to read exact uncompressed size, estimated as 5x of compressed data: %u bytes", estimatedSize);
    return estimatedSize;
}

// Decompress gzip content
static bool decompressGzip(FFstrbuf* buffer, char* headerEnd)
{
    // Calculate header size
    uint32_t headerSize = (uint32_t) (headerEnd - buffer->chars);

    *headerEnd = '\0'; // Replace delimiter with null character for easier processing
    // Ensure Content-Encoding is in response headers, not in response body
    bool hasGzip = strcasestr(buffer->chars, "\nContent-Encoding: gzip") != NULL;
    *headerEnd = '\r'; // Restore delimiter

    if (!hasGzip) {
        FF_DEBUG("No gzip compressed content detected, skipping decompression");
        return true;
    }

    FF_DEBUG("Gzip compressed content detected, preparing for decompression");

    const char* bodyStart = headerEnd + 4; // Skip delimiter

    // Calculate compressed content size
    uint32_t compressedSize = buffer->length - headerSize - 4;

    if (compressedSize <= 0) {
        // No content to decompress
        FF_DEBUG("Compressed content size is 0, skipping decompression");
        return true;
    }

    // Check if content is actually in gzip format (gzip header magic is 0x1f 0x8b)
    if (compressedSize < 2 || (uint8_t)bodyStart[0] != 0x1f || (uint8_t)bodyStart[1] != 0x8b) {
        FF_DEBUG("Content is not valid gzip format, skipping decompression");
        return false;
    }

    // Predict uncompressed size
    uint32_t estimatedSize = guessGzipOutputSize(bodyStart, compressedSize);

    // Create decompression buffer with estimated size
    FF_STRBUF_AUTO_DESTROY decompressedBuffer = ffStrbufCreateA(estimatedSize > 0 ? estimatedSize : compressedSize * 5);
    FF_DEBUG("Created decompression buffer: %u bytes", decompressedBuffer.allocated);

    // Initialize decompression
    z_stream zs = {
        .zalloc = Z_NULL,
        .zfree = Z_NULL,
        .opaque = Z_NULL,
        .avail_in = (uInt)compressedSize,
        .next_in = (Bytef*)bodyStart,
        .avail_out = (uInt)ffStrbufGetFree(&decompressedBuffer),
        .next_out = (Bytef*)decompressedBuffer.chars,
    };

    // Initialize decompression engine
    if (zlibData.ffinflateInit2_(&zs, 16 + MAX_WBITS, ZLIB_VERSION, (int)sizeof(z_stream)) != Z_OK) {
        FF_DEBUG("Failed to initialize decompression engine");
        return false;
    }
    uInt availableOut = zs.avail_out;

    // Perform decompression
    int result = zlibData.ffinflate(&zs, Z_FINISH);

    // If output buffer is insufficient, try to extend buffer
    while (result == Z_BUF_ERROR || (result != Z_STREAM_END && zs.avail_out == 0))
    {
        FF_DEBUG("Output buffer insufficient, trying to extend");

        // Save already decompressed data amount
        uint32_t alreadyDecompressed = (uint32_t)(availableOut - zs.avail_out);
        decompressedBuffer.length += alreadyDecompressed;
        decompressedBuffer.chars[decompressedBuffer.length] = '\0'; // Ensure null-terminated string

        ffStrbufEnsureFree(&decompressedBuffer, decompressedBuffer.length / 2);

        // Set output parameters to point to new buffer
        zs.avail_out = (uInt)ffStrbufGetFree(&decompressedBuffer);
        zs.next_out = (Bytef*)(decompressedBuffer.chars + decompressedBuffer.length);
        availableOut = zs.avail_out;

        // Decompress again
        result = zlibData.ffinflate(&zs, Z_FINISH);
    }

    zlibData.ffinflateEnd(&zs);

    // Calculate decompressed size
    uint32_t decompressedSize = (uint32_t)(availableOut - zs.avail_out);
    decompressedBuffer.length += decompressedSize;
    decompressedBuffer.chars[decompressedSize] = '\0'; // Ensure null-terminated string
    FF_DEBUG("Successfully decompressed %u bytes compressed data to %u bytes", compressedSize, decompressedBuffer.length);

    // Modify Content-Length header and remove Content-Encoding header
    FF_STRBUF_AUTO_DESTROY newBuffer = ffStrbufCreateA(headerSize + decompressedSize + 64);

    char* line = NULL;
    size_t len = 0;
    while (ffStrbufGetline(&line, &len, buffer))
    {
        if (ffStrStartsWithIgnCase(line, "Content-Encoding:"))
        {
            continue;
        }
        else if (ffStrStartsWithIgnCase(line, "Content-Length:"))
        {
            ffStrbufAppendF(&newBuffer, "Content-Length: %u\r\n", decompressedSize);
            continue;
        }
        else if (line[0] == '\r')
        {
            ffStrbufAppendS(&newBuffer, "\r\n");
            ffStrbufGetlineRestore(&line, &len, buffer);
            break;
        }

        ffStrbufAppendS(&newBuffer, line);
        ffStrbufAppendC(&newBuffer, '\n');
    }

    ffStrbufAppend(&newBuffer, &decompressedBuffer);
    ffStrbufDestroy(buffer);
    ffStrbufInitMove(buffer, &newBuffer);

    return true;
}
#endif

const char* ffNetworkingSendHttpRequest(FFNetworkingState* state, const char* host, const char* path, const char* headers)
{
    FF_DEBUG("Preparing to send HTTP request: host=%s, path=%s", host, path);

    // Initialize with compression disabled by default
    state->compression = false;

    #ifdef FF_HAVE_ZLIB
    const char* zlibError = loadZlibLibrary();
    // Only enable compression if zlib library is successfully loaded
    if (zlibError == NULL)
    {
        state->compression = true;
        FF_DEBUG("Successfully loaded zlib library, compression enabled");
    } else {
        FF_DEBUG("Failed to load zlib library, compression disabled: %s", zlibError);
    }
    #else
    FF_DEBUG("zlib not supported at build time, compression disabled");
    #endif

    const char* initResult = initNetworkingState(state, host, path, headers);
    if (initResult != NULL) {
        FF_DEBUG("Initialization failed: %s", initResult);
        return initResult;
    }
    FF_DEBUG("Network state initialization successful");

    const char* tfoResult = tryTcpFastOpen(state);
    if (tfoResult == NULL) {
        FF_DEBUG("TCP Fast Open succeeded or in progress");
        return NULL;
    }
    FF_DEBUG("TCP Fast Open unavailable or failed: %s, trying traditional connection", tfoResult);

    #ifdef FF_HAVE_THREADS
    if (instance.config.general.multithreading)
    {
        FF_DEBUG("Multithreading mode enabled, creating connection thread");
        state->thread = ffThreadCreate(connectAndSendThreadMain, state);
        if (state->thread) {
            FF_DEBUG("Thread creation successful: thread=%p", (void*)state->thread);
            return NULL;
        }
        FF_DEBUG("Thread creation failed");
    } else {
        FF_DEBUG("Multithreading mode disabled, connecting in main thread");
    }
    #endif

    return connectAndSend(state);
}

const char* ffNetworkingRecvHttpResponse(FFNetworkingState* state, FFstrbuf* buffer)
{
    FF_DEBUG("Preparing to receive HTTP response");
    uint32_t timeout = state->timeout;

    #ifdef FF_HAVE_THREADS
    if (state->thread)
    {
        FF_DEBUG("Connection thread is running, waiting for it to complete (timeout=%u ms)", timeout);
        if (!ffThreadJoin(state->thread, timeout)) {
            FF_DEBUG("Thread join failed or timed out");
            return "ffThreadJoin() failed or timeout";
        }
        FF_DEBUG("Thread completed successfully");
        state->thread = 0;
    }
    #endif

    if(state->sockfd == -1)
    {
        FF_DEBUG("Invalid socket, HTTP request might have failed");
        return "ffNetworkingSendHttpRequest() failed";
    }

    if(timeout > 0)
    {
        FF_DEBUG("Setting receive timeout: %u ms", timeout);
        struct timeval timev;
        timev.tv_sec = timeout / 1000;
        timev.tv_usec = (__typeof__(timev.tv_usec)) ((timeout % 1000) * 1000); //milliseconds to microseconds
        setsockopt(state->sockfd, SOL_SOCKET, SO_RCVTIMEO, &timev, sizeof(timev));
    }

    // Set larger initial receive buffer instead of small repeated receives
    int rcvbuf = 65536; // 64KB
    setsockopt(state->sockfd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    FF_DEBUG("Starting data reception");
    FF_MAYBE_UNUSED int recvCount = 0;
    uint32_t contentLength = 0;
    char* headerEnd = NULL;

    do {
        size_t availSpace = ffStrbufGetFree(buffer);
        FF_DEBUG("Data reception loop #%d, current buffer size: %u, available space: %zu",
                 ++recvCount, buffer->length, availSpace);

        ssize_t received = recv(state->sockfd, buffer->chars + buffer->length, ffStrbufGetFree(buffer), 0);

        if (received <= 0) {
            if (received == 0) {
                FF_DEBUG("Connection closed (received=0)");
            } else {
                FF_DEBUG("Reception failed: %s (errno=%d)", strerror(errno), errno);
            }
            break;
        }

        buffer->length += (uint32_t) received;
        buffer->chars[buffer->length] = '\0';

        FF_DEBUG("Successfully received %zd bytes of data, total: %u bytes", received, buffer->length);

        // Check if HTTP header end marker is found
        if (headerEnd == NULL) {
            headerEnd = memmem(buffer->chars, buffer->length, "\r\n\r\n", 4);
            if (headerEnd != NULL) {
                FF_DEBUG("Found HTTP header end marker, position: %ld", (long)(headerEnd - buffer->chars));

                // Check for Content-Length header to pre-allocate enough memory
                const char* clHeader = strcasestr(buffer->chars, "Content-Length:");
                if (clHeader) {
                    contentLength = (uint32_t) strtoul(clHeader + 16, NULL, 10);
                    if (contentLength > 0) {
                        FF_DEBUG("Detected Content-Length: %u, pre-allocating buffer", contentLength);
                        // Ensure buffer is large enough, adding header size and some margin
                        ffStrbufEnsureFree(buffer, contentLength + 16);
                        FF_DEBUG("Extended receive buffer to %u bytes", buffer->length);
                    }
                }
            }
        }
    } while (ffStrbufGetFree(buffer) > 0);

    FF_DEBUG("Closing socket: fd=%d", state->sockfd);
    close(state->sockfd);
    state->sockfd = -1;

    if (buffer->length == 0) {
        FF_DEBUG("Server response is empty");
        return "Empty server response received";
    }

    if (headerEnd == NULL) {
        FF_DEBUG("No HTTP header end marker found");
        return "No HTTP header end found";
    }
    if (contentLength > 0 && buffer->length != contentLength + (uint32_t)(headerEnd - buffer->chars) + 4) {
        FF_DEBUG("Received content length mismatches: %u != %u", buffer->length, contentLength + (uint32_t)(headerEnd - buffer->chars) + 4);
        return "Content length mismatch";
    }

    if (ffStrbufStartsWithS(buffer, "HTTP/1.1 200 OK\r\n")) {
        FF_DEBUG("Received valid HTTP 200 response, content %u bytes, total %u bytes", contentLength, buffer->length);
    } else {
        FF_DEBUG("Invalid response: %.40s...", buffer->chars);
        return "Invalid response";
    }

    // If compression was used, try to decompress
    #ifdef FF_HAVE_ZLIB
    if (state->compression) {
        FF_DEBUG("Content received, checking if compressed");
        if (!decompressGzip(buffer, headerEnd)) {
            FF_DEBUG("Decompression failed or invalid compression format");
            return "Failed to decompress or invalid format";
        } else {
            FF_DEBUG("Decompression successful or no decompression needed, total length after decompression: %u bytes", buffer->length);
        }
    }
    #endif

    return NULL;
}
