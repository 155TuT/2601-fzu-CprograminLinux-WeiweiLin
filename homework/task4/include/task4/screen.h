#ifndef TASK4_SCREEN_H
#define TASK4_SCREEN_H

#include <stddef.h>

/* Internal, bounded nonblocking screen transport. One immutable frame may be
 * in flight; callers keep newer transcript state until it has drained. */
struct t4_screen;
struct t4_screen *t4_screen_open(void);
void t4_screen_close(struct t4_screen *screen);
int t4_screen_resize(struct t4_screen *screen, unsigned rows, unsigned columns);
void t4_screen_invalidate(struct t4_screen *screen);
int t4_screen_row(struct t4_screen *screen, unsigned row, const char *style);
int t4_screen_append(struct t4_screen *screen, const void *data, size_t length);
void t4_screen_cursor(struct t4_screen *screen, unsigned row, unsigned column);
int t4_screen_submit(struct t4_screen *screen);
int t4_screen_drain(struct t4_screen *screen);
int t4_screen_busy(const struct t4_screen *screen);
int t4_screen_control(struct t4_screen *screen, const char *text);

#endif
