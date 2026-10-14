#define _POSIX_C_SOURCE 200809L

#include "task4/screen.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define OUTPUT_LIMIT (8U * 1024U * 1024U)

struct buffer { char *data; size_t length, capacity; };
struct t4_screen {
    int fd, invalid;
    unsigned rows, columns, row, cursor_row, cursor_column, old_row, old_column;
    struct buffer *current, *previous, pending;
    size_t sent;
};

static int append(struct buffer *buffer, const void *data, size_t length)
{
    if (length > OUTPUT_LIMIT - buffer->length) { errno = EOVERFLOW; return -1; }
    size_t needed = buffer->length + length + 1;
    if (needed > buffer->capacity) {
        size_t capacity = buffer->capacity ? buffer->capacity : 256;
        while (capacity < needed) capacity *= 2;
        void *copy = realloc(buffer->data, capacity);
        if (!copy) return -1;
        buffer->data = copy; buffer->capacity = capacity;
    }
    if (length) memcpy(buffer->data + buffer->length, data, length);
    buffer->length += length; buffer->data[buffer->length] = 0;
    return 0;
}

static int string(struct buffer *buffer, const char *text)
{
    return append(buffer, text, strlen(text));
}

struct t4_screen *t4_screen_open(void)
{
    /* Opening this proc fd creates an independent tty open-file description:
     * O_NONBLOCK must not change stdin/stdout or the parent shell's flags. */
    int fd = open("/proc/self/fd/2", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return NULL;
    struct t4_screen *screen = calloc(1, sizeof(*screen));
    if (!screen) { close(fd); return NULL; }
    screen->fd = fd; screen->invalid = 1;
    return screen;
}

static void free_rows(struct t4_screen *screen)
{
    for (unsigned at = 0; at < screen->rows; ++at) {
        free(screen->current[at].data); free(screen->previous[at].data);
    }
    free(screen->current); free(screen->previous);
}

int t4_screen_resize(struct t4_screen *screen, unsigned rows, unsigned columns)
{
    if (rows == screen->rows && columns == screen->columns) return 0;
    struct buffer *current = calloc(rows, sizeof(*current));
    struct buffer *previous = calloc(rows, sizeof(*previous));
    if (!current || !previous) { free(current); free(previous); return -1; }
    free_rows(screen); screen->current = current; screen->previous = previous;
    screen->rows = rows; screen->columns = columns; screen->invalid = 1;
    return 0;
}

void t4_screen_invalidate(struct t4_screen *screen) { screen->invalid = 1; }

int t4_screen_row(struct t4_screen *screen, unsigned row, const char *style)
{
    if (!row || row > screen->rows) { errno = EINVAL; return -1; }
    screen->row = row - 1;
    screen->current[screen->row].length = 0;
    return string(&screen->current[screen->row], style);
}

int t4_screen_append(struct t4_screen *screen, const void *data, size_t length)
{
    return append(&screen->current[screen->row], data, length);
}

void t4_screen_cursor(struct t4_screen *screen, unsigned row, unsigned column)
{
    screen->cursor_row = row; screen->cursor_column = column;
}

int t4_screen_busy(const struct t4_screen *screen) { return screen->sent < screen->pending.length; }

int t4_screen_drain(struct t4_screen *screen)
{
    /* A bounded number of writes keeps even a continuously writable terminal
     * from monopolizing the TCP event loop. EAGAIN is normal backpressure. */
    for (unsigned count = 0; count < 4 && t4_screen_busy(screen); ++count) {
        ssize_t written = write(screen->fd, screen->pending.data + screen->sent,
                                screen->pending.length - screen->sent);
        if (written < 0 && errno == EINTR) continue;
        if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
        if (written <= 0) { if (!written) errno = EIO; return -1; }
        screen->sent += (size_t)written;
    }
    if (!t4_screen_busy(screen)) screen->sent = screen->pending.length = 0;
    return 0;
}

int t4_screen_control(struct t4_screen *screen, const char *text)
{
    if (string(&screen->pending, text) < 0) return -1;
    return t4_screen_drain(screen);
}

int t4_screen_submit(struct t4_screen *screen)
{
    if (t4_screen_busy(screen)) { errno = EBUSY; return -1; }
    int changed = screen->invalid || screen->cursor_row != screen->old_row
                  || screen->cursor_column != screen->old_column;
    for (unsigned at = 0; at < screen->rows; ++at) {
        struct buffer *current = &screen->current[at], *previous = &screen->previous[at];
        if (!screen->invalid && current->length == previous->length
            && (!current->length || !memcmp(current->data, previous->data, current->length))) continue;
        if (!screen->pending.length && string(&screen->pending, "\033[?25l") < 0) return -1;
        char position[48];
        int length = snprintf(position, sizeof(position), "\033[%u;1H", at + 1);
        if (append(&screen->pending, position, (size_t)length) < 0
            || append(&screen->pending, current->data, current->length) < 0
            || string(&screen->pending, "\033[K") < 0) return -1;
        /* The final row image is immutable until the queued bytes drain. */
        previous->length = 0;
        if (append(previous, current->data, current->length) < 0) return -1;
        changed = 1;
    }
    if (changed) {
        char position[64];
        int length = snprintf(position, sizeof(position), "\033[%u;%uH\033[?25h",
                              screen->cursor_row, screen->cursor_column);
        if (append(&screen->pending, position, (size_t)length) < 0) return -1;
    }
    screen->old_row = screen->cursor_row; screen->old_column = screen->cursor_column;
    screen->invalid = 0;
    return t4_screen_drain(screen);
}

void t4_screen_close(struct t4_screen *screen)
{
    if (!screen) return;
    /* Normal exits flush immediately. A stopped/unread terminal must not hang
     * shutdown forever; allow a small bounded grace period for mode restore. */
    for (unsigned at = 0; at < 5 && t4_screen_busy(screen); ++at) {
        struct pollfd descriptor = {screen->fd, POLLOUT, 0};
        if (poll(&descriptor, 1, 20) > 0 && t4_screen_drain(screen) < 0) break;
    }
    close(screen->fd); free_rows(screen); free(screen->pending.data); free(screen);
}
