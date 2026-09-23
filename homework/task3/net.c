#include "net.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int setup_output(void)
{
    if (setvbuf(stdout, NULL, _IOLBF, 0) != 0) {
        fprintf(stderr, "Cannot enable stdout line buffering.\n");
        return -1;
    }
    return 0;
}

int parse_port(const char *text, int allow_zero, uint16_t *port)
{
    char *end;
    long value;

    /* 不接受负数、空串、尾随字符或溢出的端口号。 */
    if (*text < '0' || *text > '9') {
        return -1;
    }
    errno = 0;
    value = strtol(text, &end, 10);
    if (errno || *end || value < (allow_zero ? 0 : 1) || value > 65535) {
        return -1;
    }
    *port = (uint16_t)value;
    return 0;
}

int connect_server(const char *host, const char *service)
{
    struct addrinfo hints = {0}, *addresses;
    int fd = -1, saved_errno = ECONNREFUSED;

    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICSERV;
    int error = getaddrinfo(host, service, &hints, &addresses);
    if (error != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(error));
        return -1;
    }
    /* 主机名可能对应多个 IPv4 地址，逐个尝试连接。 */
    for (struct addrinfo *p = addresses; p != NULL; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd != -1) {
            if (connect(fd, p->ai_addr, p->ai_addrlen) == 0) {
                break;
            }
            saved_errno = errno;
            close(fd);
            fd = -1;
        } else {
            saved_errno = errno;
        }
    }
    freeaddrinfo(addresses);
    if (fd == -1) {
        errno = saved_errno;
        perror("connect (start the server first)");
    }
    return fd;
}

static int send_all(int fd, const void *buffer, size_t length)
{
    const unsigned char *p = buffer;
    size_t sent = 0;

    while (sent < length) {
        /* MSG_NOSIGNAL 使断开的连接返回错误，避免 SIGPIPE 直接终止进程。 */
        ssize_t n = send(fd, p + sent, length - sent, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            if (n == 0) {
                errno = EPIPE;
            }
            return -1;
        }
        sent += (size_t)n;
    }
    return 0;
}

static int receive_exact(int fd, void *buffer, size_t length)
{
    unsigned char *p = buffer;
    size_t received = 0;

    while (received < length) {
        ssize_t n = recv(fd, p + received, length - received, 0);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0) {
            return -1;
        }
        if (n == 0) {
            if (received == 0) {
                return 0;
            }
            errno = EPROTO; /* 收到一部分后断开，不是一条完整消息。 */
            return -1;
        }
        received += (size_t)n;
    }
    return 1;
}

int send_frame(int fd, const char *text, size_t length)
{
    unsigned char frame[sizeof(uint32_t) + MAX_TEXT];

    if (length > MAX_TEXT) {
        errno = EMSGSIZE;
        return -1;
    }
    uint32_t header = htonl((uint32_t)length);
    /* 将报头和正文合并提交，减少小消息往返时的小包延迟。 */
    memcpy(frame, &header, sizeof(header));
    memcpy(frame + sizeof(header), text, length);
    return send_all(fd, frame, sizeof(header) + length);
}

int receive_frame(int fd, char text[MAX_TEXT + 1], size_t *length)
{
    uint32_t header;
    int result = receive_exact(fd, &header, sizeof(header));

    if (result != 1) {
        return result;
    }
    *length = ntohl(header);
    if (*length > MAX_TEXT) {
        errno = EMSGSIZE;
        return -1;
    }
    result = receive_exact(fd, text, *length);
    if (result != 1) {
        if (result == 0) {
            errno = EPROTO; /* 已有报头却没有完整正文，属于协议截断。 */
        }
        return -1;
    }
    if (memchr(text, '\0', *length) != NULL) {
        errno = EPROTO;
        return -1;
    }
    text[*length] = '\0'; /* 网络不发送字符串结束符，接收方自行补齐。 */
    return 1;
}

int read_text_line(FILE *input, char text[MAX_TEXT + 1], size_t *length)
{
    int c, invalid = 0, any = 0;

    *length = 0;
    /* fgetc 基于 FILE 缓冲读取；超长或含 NUL 时仍消费完整一行。 */
    while ((c = fgetc(input)) != EOF && c != '\n') {
        any = 1;
        if (c == '\0' || *length == MAX_TEXT) {
            invalid = 1;
        } else {
            text[(*length)++] = (char)c;
        }
    }
    if (ferror(input)) {
        return -1;
    }
    text[*length] = '\0';
    if (invalid) {
        return -2;
    }
    return c == EOF && !any ? 0 : 1;
}
