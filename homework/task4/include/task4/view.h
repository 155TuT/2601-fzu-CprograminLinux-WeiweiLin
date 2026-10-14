#ifndef TASK4_VIEW_H
#define TASK4_VIEW_H

#include "task4/terminal.h"

/* Internal transcript/layout component; terminal.c owns keyboard and termios. */
struct t4_view;
struct t4_screen;
enum t4_view_style { T4_VIEW_TEXT, T4_VIEW_MUTED, T4_VIEW_ACCENT, T4_VIEW_BORDER };
struct t4_view *t4_view_open(void);
void t4_view_close(struct t4_view *view);
void t4_view_resize(struct t4_view *view, unsigned rows, unsigned columns);
void t4_view_dirty(struct t4_view *view);
int t4_view_is_dirty(const struct t4_view *view);
int t4_view_render(struct t4_view *view, struct t4_screen *screen);
void t4_view_reset(struct t4_view *view);
const char *t4_view_style(const struct t4_view *view, enum t4_view_style style);
int t4_view_notice(struct t4_view *view, const void *data, size_t length);
int t4_view_session(struct t4_view *view, const char *session, const char *model);
int t4_view_begin(struct t4_view *view, uint32_t request, const char *prompt, uint32_t offset);
int t4_view_thinking(struct t4_view *view, const void *data, size_t length);
int t4_view_answer(struct t4_view *view, const void *data, size_t length);
int t4_view_status(struct t4_view *view, const struct t4_ui_status *status);
int t4_view_end(struct t4_view *view, const char *state, const char *summary);
void t4_view_theme(struct t4_view *view, int dark);
void t4_view_thinking_visible(struct t4_view *view, int show);
void t4_view_page(struct t4_view *view, int backward);
void t4_view_scroll(struct t4_view *view, int backward, size_t lines);

#endif
