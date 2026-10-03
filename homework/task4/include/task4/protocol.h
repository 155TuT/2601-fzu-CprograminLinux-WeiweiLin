#ifndef TASK4_PROTOCOL_H
#define TASK4_PROTOCOL_H

#include <stdint.h>

/* Five network-order uint32_t fields form the fixed 20-byte header. */
#define T4_MAGIC UINT32_C(0x54345331)
#define T4_MAX_FRAME UINT32_C(4096)
#define T4_MAX_PROMPT UINT32_C(4096)
#define T4_MAX_RESPONSE UINT32_C(16777216)

enum t4_frame_type {
    T4_HELLO = 1,
    T4_WELCOME = 2,
    T4_BEGIN = 3,
    T4_INPUT = 4,
    T4_COMMIT = 5,
    T4_DELTA = 6,
    T4_END = 7,
    T4_ERROR = 8,
    T4_HEARTBEAT = 9
};

struct t4_frame {
    uint32_t type;
    uint32_t request_id;
    uint32_t offset;
    uint32_t length;
    unsigned char data[T4_MAX_FRAME + 1U];
};

/*
 * A complete frame has a monotonic-clock deadline of idle_ms milliseconds.
 * Partial transfers are completed internally. EINTR is returned to the caller
 * so a signal handler can ask its event loop to stop. Payloads are raw bytes;
 * applications must check their own text and state-machine requirements.
 */
/* Returns 1 for a complete send, and -1 on error. */
int t4_send(int fd, const struct t4_frame *frame, int idle_ms);
/* Returns 1 for a frame, 0 only for EOF between frames, and -1 on error. */
int t4_recv(int fd, struct t4_frame *frame, int idle_ms);

/* Returned sockets are nonblocking, close-on-exec, and use TCP_NODELAY. */
int t4_connect(const char *host, uint16_t port, int idle_ms);
/* IPv4 only; a NULL bind address selects 127.0.0.1. Port 0 selects a free port. */
int t4_listen(const char *bind_ipv4, uint16_t port, uint16_t *actual_port);

/* Strict unsigned decimal parsing: no signs, whitespace, or trailing text. */
int t4_uint(const char *text, uint32_t minimum, uint32_t maximum,
            uint32_t *value);

#endif
