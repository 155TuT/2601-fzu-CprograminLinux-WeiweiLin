#ifndef TASK4_TERMINAL_H
#define TASK4_TERMINAL_H

#include <stddef.h>
#include <stdint.h>

struct t4_terminal;

/* Borrowed strings are copied immediately; one cached status belongs to a turn. */
struct t4_ui_status {
    const char *phase;
    const char *model;
    uint32_t prompt_tokens, context_tokens, generated_tokens;
    double tokens_per_second, elapsed_seconds;
};

/* NULL leaves legacy I/O untouched unless both stdin and stderr are terminals. */
struct t4_terminal *t4_terminal_open(void);
/* Nonblocking: 0 edits/no input, 1 line, 2 EOF, -1 error, -2 invalid long line. */
int t4_terminal_read(struct t4_terminal *terminal, unsigned char line[4097],
                     uint32_t *length);
int t4_terminal_pending(const struct t4_terminal *terminal);
/* Output and prompt/resize updates preserve the unsent draft and its cursor. */
int t4_terminal_prompt(struct t4_terminal *terminal, const char *prompt);
int t4_terminal_write(struct t4_terminal *terminal, const void *data, size_t length);
int t4_terminal_resize(struct t4_terminal *terminal);
int t4_terminal_session(struct t4_terminal *terminal, const char *session, const char *model);
int t4_terminal_begin_turn(struct t4_terminal *terminal, uint32_t request,
                           const char *prompt, uint32_t resume_offset);
int t4_terminal_thinking(struct t4_terminal *terminal, const void *data, size_t length);
int t4_terminal_answer(struct t4_terminal *terminal, const void *data, size_t length);
int t4_terminal_status(struct t4_terminal *terminal, const struct t4_ui_status *status);
int t4_terminal_end_turn(struct t4_terminal *terminal, const char *state, const char *summary);
int t4_terminal_theme(struct t4_terminal *terminal, int dark);
int t4_terminal_thinking_visible(struct t4_terminal *terminal, int show);
/* Call tick every event-loop pass with a poll cap of 33 ms; flush forces paint. */
int t4_terminal_tick(struct t4_terminal *terminal);
int t4_terminal_flush(struct t4_terminal *terminal);
/* Switch conversation while preserving theme and thinking visibility. */
int t4_terminal_reset(struct t4_terminal *terminal);
/* Seed keyboard history from a restored user prompt, without submitting it. */
int t4_terminal_history(struct t4_terminal *terminal, const char *prompt);
void t4_terminal_clear_input(struct t4_terminal *terminal);
/* Call at safe points around job control; none of these are signal handlers. */
int t4_terminal_suspend(struct t4_terminal *terminal);
int t4_terminal_resume(struct t4_terminal *terminal);
void t4_terminal_close(struct t4_terminal *terminal);

#endif
