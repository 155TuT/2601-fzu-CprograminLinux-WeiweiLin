#define _POSIX_C_SOURCE 200809L

#include "task4/protocol.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static int deadline_after(int milliseconds, struct timespec *deadline)
{
    if (milliseconds <= 0) {
        errno = EINVAL;
        return -1;
    }
    if (clock_gettime(CLOCK_MONOTONIC, deadline) == -1) {
        return -1;
    }
    deadline->tv_sec += milliseconds / 1000;
    deadline->tv_nsec += (long)(milliseconds % 1000) * 1000000L;
    if (deadline->tv_nsec >= 1000000000L) {
        deadline->tv_sec++;
        deadline->tv_nsec -= 1000000000L;
    }
    return 0;
}

static int wait_ready(int fd, short events, const struct timespec *deadline)
{
    struct timespec now;
    struct pollfd descriptor = { .fd = fd, .events = events, .revents = 0 };
    int64_t remaining;
    int result;

    if (clock_gettime(CLOCK_MONOTONIC, &now) == -1) {
        return -1;
    }
    remaining = (int64_t)(deadline->tv_sec - now.tv_sec) * INT64_C(1000000000)
              + deadline->tv_nsec - now.tv_nsec;
    if (remaining <= 0) {
        errno = ETIMEDOUT;
        return -1;
    }
    /* Round upward: a sub-millisecond remainder must still wait for data. */
    remaining = (remaining + 999999) / 1000000;
    result = poll(&descriptor, 1, remaining > INT_MAX ? INT_MAX : (int)remaining);
    if (result == -1) {
        return -1;
    }
    if (result == 0) {
        errno = ETIMEDOUT;
        return -1;
    }
    if ((descriptor.revents & POLLNVAL) != 0) {
        errno = EBADF;
        return -1;
    }
    /* recv/send and SO_ERROR report the precise hangup or socket error. */
    return 0;
}

static int transfer(int fd, void *buffer, size_t length, int writing,
                    const struct timespec *deadline, int boundary_eof)
{
    unsigned char *bytes = buffer;
    size_t done = 0;

    while (done < length) {
        ssize_t count;

        if (wait_ready(fd, writing ? POLLOUT : POLLIN, deadline) == -1) {
            return -1;
        }
        count = writing ? send(fd, bytes + done, length - done,
                               MSG_NOSIGNAL | MSG_DONTWAIT)
                        : recv(fd, bytes + done, length - done, MSG_DONTWAIT);
        if (count > 0) {
            done += (size_t)count;
        } else if (count == 0) {
            if (!writing && done == 0 && boundary_eof) {
                return 0;
            }
            errno = writing ? EPIPE : ECONNRESET;
            return -1;
        } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
            return -1;
        }
    }
    return 1;
}

int t4_send(int fd, const struct t4_frame *frame, int idle_ms)
{
    uint32_t header[5];
    struct timespec deadline;

    if (frame == NULL || frame->length > T4_MAX_FRAME) {
        errno = EINVAL;
        return -1;
    }
    if (deadline_after(idle_ms, &deadline) == -1) {
        return -1;
    }
    header[0] = htonl(T4_MAGIC);
    header[1] = htonl(frame->type);
    header[2] = htonl(frame->request_id);
    header[3] = htonl(frame->offset);
    header[4] = htonl(frame->length);
    if (transfer(fd, header, sizeof(header), 1, &deadline, 0) == -1) {
        return -1;
    }
    if (transfer(fd, (void *)frame->data, frame->length, 1, &deadline, 0) == -1) {
        return -1;
    }
    return 1;
}

int t4_recv(int fd, struct t4_frame *frame, int idle_ms)
{
    uint32_t header[5];
    struct timespec deadline;
    int result;

    if (frame == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (deadline_after(idle_ms, &deadline) == -1) {
        return -1;
    }
    result = transfer(fd, header, sizeof(header), 0, &deadline, 1);
    if (result != 1) {
        return result;
    }
    if (ntohl(header[0]) != T4_MAGIC || ntohl(header[4]) > T4_MAX_FRAME) {
        errno = EPROTO;
        return -1;
    }
    frame->type = ntohl(header[1]);
    frame->request_id = ntohl(header[2]);
    frame->offset = ntohl(header[3]);
    frame->length = ntohl(header[4]);
    result = transfer(fd, frame->data, frame->length, 0, &deadline, 0);
    if (result == -1) {
        return -1;
    }
    /* Convenience terminator; the framing layer accepts embedded NUL bytes. */
    frame->data[frame->length] = '\0';
    return 1;
}

static int configure_socket(int fd)
{
    int flags = fcntl(fd, F_GETFL);
    int enabled = 1;

    if (flags == -1 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1
        || fcntl(fd, F_SETFD, FD_CLOEXEC) == -1
        || setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enabled,
                      sizeof(enabled)) == -1) {
        return -1;
    }
    return 0;
}

int t4_connect(const char *host, uint16_t port, int idle_ms)
{
    struct addrinfo hints;
    struct addrinfo *addresses = NULL;
    struct addrinfo *address;
    struct timespec deadline;
    char service[6];
    int connected = -1;
    int saved_error = ECONNREFUSED;
    int lookup;

    if (host == NULL || *host == '\0' || port == 0) {
        errno = EINVAL;
        return -1;
    }
    if (deadline_after(idle_ms, &deadline) == -1) {
        return -1;
    }
    (void)snprintf(service, sizeof(service), "%u", (unsigned int)port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    lookup = getaddrinfo(host, service, &hints, &addresses);
    if (lookup != 0) {
        if (lookup != EAI_SYSTEM) {
            errno = EHOSTUNREACH;
        }
        return -1;
    }
    for (address = addresses; address != NULL; address = address->ai_next) {
        int fd = socket(address->ai_family, address->ai_socktype,
                        address->ai_protocol);
        int error = 0;
        socklen_t error_length = sizeof(error);

        if (fd == -1) {
            saved_error = errno;
            continue;
        }
        if (configure_socket(fd) == -1) {
            saved_error = errno;
            (void)close(fd);
            continue;
        }
        if (connect(fd, address->ai_addr, address->ai_addrlen) == 0) {
            connected = fd;
            break;
        }
        if (errno == EINPROGRESS) {
            if (wait_ready(fd, POLLOUT, &deadline) == 0
                && getsockopt(fd, SOL_SOCKET, SO_ERROR, &error,
                              &error_length) == 0) {
                if (error == 0) {
                    connected = fd;
                    break;
                }
                errno = error;
            }
        }
        saved_error = errno;
        (void)close(fd);
        if (saved_error == EINTR || saved_error == ETIMEDOUT) {
            break;
        }
    }
    freeaddrinfo(addresses);
    if (connected == -1) {
        errno = saved_error;
    }
    return connected;
}

int t4_listen(const char *bind_ipv4, uint16_t port, uint16_t *actual_port)
{
    struct sockaddr_in address;
    socklen_t address_length = sizeof(address);
    int enabled = 1;
    int fd;

    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (inet_pton(AF_INET, bind_ipv4 == NULL ? "127.0.0.1" : bind_ipv4,
                  &address.sin_addr) != 1) {
        errno = EINVAL;
        return -1;
    }
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == -1) {
        return -1;
    }
    if (configure_socket(fd) == -1
        || setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enabled,
                      sizeof(enabled)) == -1
        || bind(fd, (struct sockaddr *)&address, sizeof(address)) == -1
        || listen(fd, 64) == -1
        || getsockname(fd, (struct sockaddr *)&address, &address_length) == -1) {
        int saved_error = errno;

        (void)close(fd);
        errno = saved_error;
        return -1;
    }
    if (actual_port != NULL) {
        *actual_port = ntohs(address.sin_port);
    }
    return fd;
}

int t4_uint(const char *text, uint32_t minimum, uint32_t maximum,
            uint32_t *value)
{
    uint32_t parsed = 0;
    const unsigned char *cursor = (const unsigned char *)text;

    if (text == NULL || *text == '\0' || value == NULL || minimum > maximum) {
        errno = EINVAL;
        return -1;
    }
    while (*cursor != '\0') {
        uint32_t digit;

        if (*cursor < '0' || *cursor > '9') {
            errno = EINVAL;
            return -1;
        }
        digit = (uint32_t)(*cursor - '0');
        if (parsed > UINT32_MAX / 10U
            || (parsed == UINT32_MAX / 10U && digit > UINT32_MAX % 10U)) {
            errno = ERANGE;
            return -1;
        }
        parsed = parsed * 10U + digit;
        cursor++;
    }
    if (parsed < minimum || parsed > maximum) {
        errno = ERANGE;
        return -1;
    }
    *value = parsed;
    return 0;
}
