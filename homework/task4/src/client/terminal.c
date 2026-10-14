#define _XOPEN_SOURCE 700

#include "task4/terminal.h"
#include "task4/text.h"
#include "task4/view.h"
#include "task4/screen.h"

#include <errno.h>
#include <locale.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>
#include <wctype.h>

#define HISTORY 50U
#define LIMIT 4096U

struct t4_terminal {
    struct termios original, editing;
    unsigned rows, columns;
    unsigned char draft[LIMIT + 1], backup[LIMIT + 1], input[4096], utf8[4];
    size_t length, cursor, view, backup_length, backup_cursor;
    size_t input_at, input_length;
    unsigned utf8_have, utf8_width;
    char prompt[128], escape[64];
    size_t escape_length;
    char *history[HISTORY];
    unsigned history_count, history_at;
    int invalid, paste, suspended, discard_escape, mouse_remaining;
    unsigned char mouse[3];
    struct t4_view *view_state;
    struct t4_screen *screen;
    int64_t last_paint;
};

static int output(struct t4_terminal *terminal, const void *data, size_t length)
{ return t4_screen_append(terminal->screen, data, length); }
static int command(struct t4_terminal *terminal, const char *text)
{ return output(terminal, text, strlen(text)); }

static size_t next(const unsigned char *text, size_t at, size_t length)
{
    unsigned width = t4_character_width(text[at]);
    return at + (width && width <= length - at ? width : 1);
}

static size_t previous(const unsigned char *text, size_t at)
{
    if (!at) return 0;
    --at;
    while (at && (text[at] & 0xc0U) == 0x80U) --at;
    return at;
}

static wchar_t wide(const unsigned char *text, size_t width)
{
    mbstate_t state = {0};
    wchar_t result;
    size_t consumed = mbrtowc(&result, (const char *)text, width, &state);
    return consumed == (size_t)-1 || consumed == (size_t)-2 ? L'?' : result;
}

static unsigned cells(const unsigned char *text, size_t from, size_t until)
{
    unsigned total = 0;
    while (from < until) {
        size_t after = next(text, from, until);
        int width = wcwidth(wide(text + from, after - from));
        total += width < 0 ? 1U : (unsigned)width;
        from = after;
    }
    return total;
}

static int geometry(struct t4_terminal *terminal)
{
    struct winsize size = {0};
    if (ioctl(STDIN_FILENO, TIOCGWINSZ, &size) < 0) return -1;
    terminal->rows = size.ws_row ? size.ws_row : 24;
    terminal->columns = size.ws_col ? size.ws_col : 80;
    /* Bound screen memory even if a peer sends an unreasonable winsize. */
    if (terminal->rows > 512) terminal->rows = 512;
    if (terminal->columns > 2048) terminal->columns = 2048;
    if (t4_screen_resize(terminal->screen, terminal->rows, terminal->columns) < 0) return -1;
    if (terminal->view_state) t4_view_resize(terminal->view_state, terminal->rows, terminal->columns);
    return 0;
}

static int paint(struct t4_terminal *terminal)
{
    if (t4_view_render(terminal->view_state, terminal->screen) < 0) return -1;
    if (terminal->rows < 4 || terminal->columns < 12) {
        t4_screen_cursor(terminal->screen, 1, 1);
        return t4_screen_submit(terminal->screen);
    }
    size_t prompt_length = strlen(terminal->prompt), shown_prompt = 0;
    unsigned prompt_cells = 0;
    while (shown_prompt < prompt_length) {
        size_t after = next((const unsigned char *)terminal->prompt, shown_prompt, prompt_length);
        unsigned width = cells((const unsigned char *)terminal->prompt, shown_prompt, after);
        if (prompt_cells + width > terminal->columns / 3) break;
        shown_prompt = after; prompt_cells += width;
    }
    unsigned available = terminal->columns - prompt_cells - 2;
    if (terminal->view > terminal->cursor) terminal->view = terminal->cursor;
    while (cells(terminal->draft, terminal->view, terminal->cursor) >= available)
        terminal->view = next(terminal->draft, terminal->view, terminal->length);
    size_t end = terminal->view;
    unsigned visible = 0;
    while (end < terminal->length) {
        size_t after = next(terminal->draft, end, terminal->length);
        unsigned width = cells(terminal->draft, end, after);
        if (visible + width > available) break;
        visible += width; end = after;
    }
    if (t4_screen_row(terminal->screen, terminal->rows, t4_view_style(terminal->view_state, T4_VIEW_ACCENT)) < 0
        || output(terminal, terminal->prompt, shown_prompt) < 0
        || command(terminal, t4_view_style(terminal->view_state, T4_VIEW_TEXT)) < 0
        || command(terminal, terminal->view ? "<" : " ") < 0
        || output(terminal, terminal->draft + terminal->view, end - terminal->view) < 0
        || command(terminal, end < terminal->length ? ">" : " ") < 0) return -1;
    t4_screen_cursor(terminal->screen, terminal->rows, prompt_cells + 2
                     + cells(terminal->draft, terminal->view, terminal->cursor));
    return t4_screen_submit(terminal->screen);
}

static int refresh(struct t4_terminal *terminal, int force)
{
    if (terminal->suspended) return 0;
    if (t4_screen_drain(terminal->screen) < 0) return -1;
    if (t4_screen_busy(terminal->screen)) return 0;
    if (!force && !t4_view_is_dirty(terminal->view_state)) return 0;
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) return -1;
    int64_t milliseconds = (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
    if (!force && milliseconds - terminal->last_paint < 34) return 0;
    if (paint(terminal) < 0) return -1;
    terminal->last_paint = milliseconds;
    return 0;
}

static int draw(struct t4_terminal *terminal)
{
    t4_view_dirty(terminal->view_state);
    return refresh(terminal, 0);
}

static int layout(struct t4_terminal *terminal)
{
    /* Save only private mouse modes that we change, and restore them on exit.
     * Terminals lacking xterm's save/restore simply ignore those controls; we
     * explicitly disable mouse reporting before returning to the shell. */
    if (t4_screen_control(terminal->screen, "\033[?1000;1006;1007s\033[?1049h\033[r\033[?2004h\033[?1007l\033[?1000h\033[?1006h\033[?25h") < 0) return -1;
    t4_screen_invalidate(terminal->screen);
    t4_view_resize(terminal->view_state, terminal->rows, terminal->columns);
    return refresh(terminal, 1);
}

struct t4_terminal *t4_terminal_open(void)
{
    if (!isatty(STDIN_FILENO) || !isatty(STDERR_FILENO)) return NULL;
    struct t4_terminal *terminal = calloc(1, sizeof(*terminal));
    if (!terminal) return NULL;
    terminal->view_state = t4_view_open();
    if (!terminal->view_state) { free(terminal); return NULL; }
    terminal->screen = t4_screen_open();
    if (!terminal->screen) { t4_view_close(terminal->view_state); free(terminal); return NULL; }
    (void)setlocale(LC_CTYPE, "");
    if (MB_CUR_MAX == 1) (void)setlocale(LC_CTYPE, "C.UTF-8");
    if (tcgetattr(STDIN_FILENO, &terminal->original) < 0 || geometry(terminal) < 0) {
        t4_screen_close(terminal->screen); t4_view_close(terminal->view_state); free(terminal); return NULL;
    }
    terminal->editing = terminal->original;
    terminal->editing.c_lflag &= (tcflag_t)~(ICANON | ECHO | ECHONL | IEXTEN);
    terminal->editing.c_lflag |= ISIG;
    terminal->editing.c_iflag &= (tcflag_t)~(IXON | IXOFF | ICRNL | INLCR | IGNCR);
#ifdef IUTF8
    terminal->editing.c_iflag |= IUTF8;
#endif
    terminal->editing.c_cc[VMIN] = 0; terminal->editing.c_cc[VTIME] = 0;
    strcpy(terminal->prompt, "question> ");
    if (tcsetattr(STDIN_FILENO, TCSANOW, &terminal->editing) < 0) {
        t4_screen_close(terminal->screen); t4_view_close(terminal->view_state); free(terminal); return NULL;
    }
    if (layout(terminal) < 0) { t4_terminal_close(terminal); return NULL; }
    return terminal;
}

int t4_terminal_prompt(struct t4_terminal *terminal, const char *prompt)
{
    if (!terminal || !prompt) { errno = EINVAL; return -1; }
    size_t length = strlen(prompt), copied = 0, at = 0;
    while (at < length) {
        size_t after = next((const unsigned char *)prompt, at, length);
        if (copied + after - at >= sizeof(terminal->prompt)) break;
        wchar_t character = wide((const unsigned char *)prompt + at, after - at);
        if (character >= 32 && character != 127 && !(character >= 128 && character <= 159)) {
            memcpy(terminal->prompt + copied, prompt + at, after - at); copied += after - at;
        }
        at = after;
    }
    terminal->prompt[copied] = 0;
    return draw(terminal);
}

int t4_terminal_write(struct t4_terminal *terminal, const void *data, size_t length)
{
    if (!terminal || (!data && length)) { errno = EINVAL; return -1; }
    if (t4_view_notice(terminal->view_state, data, length) < 0) return -1;
    return refresh(terminal, 0);
}

int t4_terminal_resize(struct t4_terminal *terminal)
{
    if (!terminal) { errno = EINVAL; return -1; }
    if (geometry(terminal) < 0) return -1;
    return refresh(terminal, 1);
}

int t4_terminal_tick(struct t4_terminal *terminal)
{
    if (!terminal) { errno = EINVAL; return -1; }
    return refresh(terminal, 0);
}
int t4_terminal_flush(struct t4_terminal *terminal)
{
    if (!terminal) { errno = EINVAL; return -1; }
    return refresh(terminal, 1);
}
int t4_terminal_reset(struct t4_terminal *terminal)
{
    if (!terminal) { errno = EINVAL; return -1; }
    for (unsigned at = 0; at < terminal->history_count; ++at) free(terminal->history[at]);
    terminal->history_count = terminal->history_at = 0;
    terminal->backup_length = terminal->backup_cursor = 0; terminal->backup[0] = 0;
    terminal->input_at = terminal->input_length = 0;
    terminal->discard_escape = terminal->mouse_remaining = 0;
    t4_view_reset(terminal->view_state);
    t4_terminal_clear_input(terminal);
    return refresh(terminal, 1);
}
int t4_terminal_session(struct t4_terminal *terminal, const char *session, const char *model)
{
    if (!terminal) { errno = EINVAL; return -1; }
    if (t4_view_session(terminal->view_state, session, model) < 0) return -1;
    return refresh(terminal, 0);
}
int t4_terminal_begin_turn(struct t4_terminal *terminal, uint32_t request, const char *prompt, uint32_t offset)
{
    if (!terminal) { errno = EINVAL; return -1; }
    if (t4_view_begin(terminal->view_state, request, prompt, offset) < 0) return -1;
    return refresh(terminal, 0);
}
int t4_terminal_thinking(struct t4_terminal *terminal, const void *data, size_t length)
{
    if (!terminal) { errno = EINVAL; return -1; }
    if (t4_view_thinking(terminal->view_state, data, length) < 0) return -1;
    return refresh(terminal, 0);
}
int t4_terminal_answer(struct t4_terminal *terminal, const void *data, size_t length)
{
    if (!terminal) { errno = EINVAL; return -1; }
    if (t4_view_answer(terminal->view_state, data, length) < 0) return -1;
    return refresh(terminal, 0);
}
int t4_terminal_status(struct t4_terminal *terminal, const struct t4_ui_status *status)
{
    if (!terminal) { errno = EINVAL; return -1; }
    if (t4_view_status(terminal->view_state, status) < 0) return -1;
    return refresh(terminal, 0);
}
int t4_terminal_end_turn(struct t4_terminal *terminal, const char *state, const char *summary)
{
    if (!terminal) { errno = EINVAL; return -1; }
    if (t4_view_end(terminal->view_state, state, summary) < 0) return -1;
    return refresh(terminal, 1);
}
int t4_terminal_theme(struct t4_terminal *terminal, int dark)
{
    if (!terminal) { errno = EINVAL; return -1; }
    t4_view_theme(terminal->view_state, dark);
    return refresh(terminal, 1);
}
int t4_terminal_thinking_visible(struct t4_terminal *terminal, int show)
{
    if (!terminal) { errno = EINVAL; return -1; }
    t4_view_thinking_visible(terminal->view_state, show);
    return refresh(terminal, 1);
}

void t4_terminal_clear_input(struct t4_terminal *terminal)
{
    if (!terminal) return;
    /* Explicit cancellation of an unfinished paste also drops already-read
     * paste bytes. Enter otherwise retains subsequent buffered line events. */
    if (terminal->paste || terminal->escape_length) terminal->input_at = terminal->input_length;
    terminal->length = terminal->cursor = terminal->view = 0;
    terminal->draft[0] = 0; terminal->invalid = 0;
    terminal->utf8_have = terminal->utf8_width = 0;
    terminal->escape_length = 0; terminal->paste = 0;
    terminal->discard_escape = terminal->mouse_remaining = 0;
    terminal->history_at = terminal->history_count;
    (void)draw(terminal);
}

static void remove_range(struct t4_terminal *terminal, size_t from, size_t until)
{
    memmove(terminal->draft + from, terminal->draft + until, terminal->length - until + 1);
    terminal->length -= until - from; terminal->cursor = from;
    if (terminal->view > from) terminal->view = from;
    terminal->invalid = 0;
}

static void insert(struct t4_terminal *terminal, const unsigned char *text, size_t length)
{
    if (length > LIMIT - terminal->length) { terminal->invalid = 1; return; }
    memmove(terminal->draft + terminal->cursor + length, terminal->draft + terminal->cursor,
            terminal->length - terminal->cursor + 1);
    memcpy(terminal->draft + terminal->cursor, text, length);
    terminal->cursor += length; terminal->length += length;
}

static void history_move(struct t4_terminal *terminal, int backward)
{
    if (backward && terminal->history_at == terminal->history_count) {
        memcpy(terminal->backup, terminal->draft, terminal->length + 1);
        terminal->backup_length = terminal->length; terminal->backup_cursor = terminal->cursor;
    }
    if (backward && terminal->history_at) --terminal->history_at;
    else if (!backward && terminal->history_at < terminal->history_count) ++terminal->history_at;
    else return;
    if (terminal->history_at == terminal->history_count) {
        memcpy(terminal->draft, terminal->backup, terminal->backup_length + 1);
        terminal->length = terminal->backup_length; terminal->cursor = terminal->backup_cursor;
    } else {
        strcpy((char *)terminal->draft, terminal->history[terminal->history_at]);
        terminal->length = terminal->cursor = strlen((char *)terminal->draft);
    }
    terminal->view = 0; terminal->invalid = 0;
}

int t4_terminal_history(struct t4_terminal *terminal, const char *prompt)
{
    if (!terminal || !prompt || strlen(prompt) > LIMIT) { errno = EINVAL; return -1; }
    if (!*prompt || (terminal->history_count
        && !strcmp(terminal->history[terminal->history_count - 1], prompt))) return 0;
    char *copy = strdup(prompt);
    if (!copy) return -1;
    if (terminal->history_count == HISTORY) {
        free(terminal->history[0]);
        memmove(terminal->history, terminal->history + 1, (HISTORY - 1) * sizeof(char *));
        --terminal->history_count;
    }
    terminal->history[terminal->history_count++] = copy;
    terminal->history_at = terminal->history_count;
    return 0;
}

static void text_byte(struct t4_terminal *terminal, unsigned char byte)
{
    if (terminal->utf8_have) {
        if ((byte & 0xc0U) != 0x80U) {
            terminal->invalid = 1; terminal->utf8_have = 0;
            text_byte(terminal, byte); return;
        }
        terminal->utf8[terminal->utf8_have++] = byte;
        if (terminal->utf8_have == terminal->utf8_width) {
            if (t4_character_valid(terminal->utf8, terminal->utf8_width))
                insert(terminal, terminal->utf8, terminal->utf8_width);
            else terminal->invalid = 1;
            terminal->utf8_have = 0;
        }
    } else if (byte < 128) {
        if (byte >= 32 && byte != 127) insert(terminal, &byte, 1);
    } else {
        terminal->utf8_width = t4_character_width(byte);
        if (!terminal->utf8_width) terminal->invalid = 1;
        else { terminal->utf8[0] = byte; terminal->utf8_have = 1; }
    }
}

static void mouse_scroll(struct t4_terminal *terminal, unsigned button)
{
    /* Modifier bits do not turn a wheel into a keyboard history event. */
    button &= ~28U;
    if (button == 64 || button == 65)
        t4_view_scroll(terminal->view_state, button == 64, 3);
}

static void mouse_action(struct t4_terminal *terminal)
{
    const char *at = terminal->escape + 3;
    unsigned values[3];
    for (unsigned index = 0; index < 3; ++index) {
        if (*at < '0' || *at > '9') return;
        unsigned value = 0;
        while (*at >= '0' && *at <= '9') {
            if (value > 100000U) return;
            value = value * 10U + (unsigned)(*at++ - '0');
        }
        values[index] = value;
        if (index < 2 && *at++ != ';') return;
    }
    if (*at != 'M' || at[1] || !values[1] || !values[2]) return;
    mouse_scroll(terminal, values[0]);
}

static void escape_action(struct t4_terminal *terminal)
{
    const char *key = terminal->escape;
    if (!strncmp(key, "\033[<", 3)) { mouse_action(terminal); return; }
    if (!strcmp(key, "\033[201~")) { terminal->paste = 0; return; }
    if (terminal->paste) {
        for (size_t at = 1; at < terminal->escape_length; ++at)
            text_byte(terminal, (unsigned char)key[at]);
        return;
    }
    if (!strcmp(key, "\033[200~")) terminal->paste = 1;
    else if (!strcmp(key, "\033[5~")) t4_view_page(terminal->view_state, 1);
    else if (!strcmp(key, "\033[6~")) t4_view_page(terminal->view_state, 0);
    else if (!strcmp(key, "\033[A") || !strcmp(key, "\033OA")) history_move(terminal, 1);
    else if (!strcmp(key, "\033[B") || !strcmp(key, "\033OB")) history_move(terminal, 0);
    else if (!strcmp(key, "\033[D") || !strcmp(key, "\033OD")) terminal->cursor = previous(terminal->draft, terminal->cursor);
    else if ((!strcmp(key, "\033[C") || !strcmp(key, "\033OC")) && terminal->cursor < terminal->length)
        terminal->cursor = next(terminal->draft, terminal->cursor, terminal->length);
    else if (!strcmp(key, "\033[H") || !strcmp(key, "\033OH") || !strcmp(key, "\033[1~") || !strcmp(key, "\033[7~")) terminal->cursor = 0;
    else if (!strcmp(key, "\033[F") || !strcmp(key, "\033OF") || !strcmp(key, "\033[4~") || !strcmp(key, "\033[8~")) terminal->cursor = terminal->length;
    else if (!strcmp(key, "\033[3~") && terminal->cursor < terminal->length)
        remove_range(terminal, terminal->cursor, next(terminal->draft, terminal->cursor, terminal->length));
}

/* Return 1 for Enter, 2 for Ctrl+D on an empty draft, otherwise 0. */
static int key(struct t4_terminal *terminal, unsigned char byte)
{
    if (terminal->mouse_remaining) {
        terminal->mouse[3 - terminal->mouse_remaining--] = byte;
        if (!terminal->mouse_remaining && terminal->mouse[0] >= 32
            && terminal->mouse[1] >= 33 && terminal->mouse[2] >= 33)
            mouse_scroll(terminal, (unsigned)terminal->mouse[0] - 32);
        return 0;
    }
    if (terminal->discard_escape) {
        if (byte == 27) { terminal->discard_escape = 0; terminal->escape_length = 0; return key(terminal, byte); }
        if (byte == 'M' || byte == 'm' || byte == '\r' || byte == '\n') terminal->discard_escape = 0;
        return 0;
    }
    if (terminal->escape_length) {
        if (byte == 27) { terminal->escape_length = 0; return key(terminal, byte); }
        int mouse = terminal->escape_length >= 3 && !strncmp(terminal->escape, "\033[<", 3);
        if (terminal->escape_length + 1 >= sizeof(terminal->escape)) {
            terminal->escape_length = 0;
            terminal->discard_escape = mouse && byte != 'M' && byte != 'm';
            return 0;
        }
        terminal->escape[terminal->escape_length++] = (char)byte;
        terminal->escape[terminal->escape_length] = 0;
        if (!strcmp(terminal->escape, "\033[M")) {
            terminal->escape_length = 0; terminal->mouse_remaining = 3; return 0;
        }
        if (mouse) {
            if (byte == 'M' || byte == 'm') {
                escape_action(terminal); terminal->escape_length = 0;
            } else if (byte == '\r' || byte == '\n') terminal->escape_length = 0;
            return 0;
        }
        if (terminal->escape_length == 2 && byte != '[' && byte != 'O') {
            terminal->escape_length = 0;
            if (terminal->paste) text_byte(terminal, byte);
            else return key(terminal, byte);
        } else if (terminal->escape_length >= 3 && byte >= '@' && byte <= '~') {
            escape_action(terminal); terminal->escape_length = 0;
        }
        return 0;
    }
    if (byte == 27) { terminal->escape[0] = 27; terminal->escape[1] = 0; terminal->escape_length = 1; return 0; }
    if (terminal->paste) {
        if (byte == '\n' || byte == '\r' || byte == '\t') byte = ' ';
        text_byte(terminal, byte); return 0;
    }
    if (byte == '\r' || byte == '\n') {
        if (terminal->utf8_have) terminal->invalid = 1;
        return 1;
    }
    if (byte == 12) { t4_screen_invalidate(terminal->screen); (void)t4_terminal_flush(terminal); return 0; }
    if (byte == 4) {
        if (!terminal->length) return 2;
        if (terminal->cursor < terminal->length)
            remove_range(terminal, terminal->cursor, next(terminal->draft, terminal->cursor, terminal->length));
    } else if ((byte == 127 || byte == 8) && terminal->cursor)
        remove_range(terminal, previous(terminal->draft, terminal->cursor), terminal->cursor);
    else if (byte == 1) terminal->cursor = 0;
    else if (byte == 5) terminal->cursor = terminal->length;
    else if (byte == 21) remove_range(terminal, 0, terminal->cursor);
    else if (byte == 11) remove_range(terminal, terminal->cursor, terminal->length);
    else if (byte == 23) {
        size_t from = terminal->cursor;
        while (from) {
            size_t before = previous(terminal->draft, from);
            if (!iswspace(wide(terminal->draft + before, from - before))) break;
            from = before;
        }
        while (from) {
            size_t before = previous(terminal->draft, from);
            if (iswspace(wide(terminal->draft + before, from - before))) break;
            from = before;
        }
        remove_range(terminal, from, terminal->cursor);
    } else if (byte == '\t') { unsigned char space = ' '; insert(terminal, &space, 1); }
    else text_byte(terminal, byte);
    return 0;
}

int t4_terminal_pending(const struct t4_terminal *terminal)
{
    return terminal && terminal->input_at < terminal->input_length;
}

int t4_terminal_read(struct t4_terminal *terminal, unsigned char line[4097], uint32_t *length)
{
    if (!terminal || !line || !length) { errno = EINVAL; return -1; }
    int edited = 0;
    for (;;) {
        if (!t4_terminal_pending(terminal)) {
            struct pollfd descriptor = {STDIN_FILENO, POLLIN, 0};
            int ready = poll(&descriptor, 1, 0);
            if (ready < 0) return errno == EINTR ? 0 : -1;
            if (!ready) return edited ? draw(terminal) : 0;
            ssize_t count = read(STDIN_FILENO, terminal->input, sizeof(terminal->input));
            if (count < 0) return errno == EINTR || errno == EAGAIN ? 0 : -1;
            if (!count) return descriptor.revents & POLLHUP ? 2 : (edited ? draw(terminal) : 0);
            terminal->input_at = 0; terminal->input_length = (size_t)count;
        }
        int action = key(terminal, terminal->input[terminal->input_at++]);
        edited = 1;
        if (action == 2) return 2;
        if (action == 1) {
            int invalid = terminal->invalid;
            *length = (uint32_t)terminal->length;
            memcpy(line, terminal->draft, terminal->length + 1);
            if (!invalid) (void)t4_terminal_history(terminal, (const char *)terminal->draft);
            t4_terminal_clear_input(terminal);
            return invalid ? -2 : 1;
        }
    }
}

int t4_terminal_suspend(struct t4_terminal *terminal)
{
    if (!terminal || terminal->suspended) return 0;
    int result = tcsetattr(STDIN_FILENO, TCSANOW, &terminal->original);
    if (t4_screen_control(terminal->screen, "\033[0m\033[?25h\033[?1000l\033[?1006l\033[?1007l\033[?1000;1006;1007r\033[?2004l\033[r\033[?1049l") < 0) result = -1;
    terminal->suspended = 1;
    return result;
}

int t4_terminal_resume(struct t4_terminal *terminal)
{
    if (!terminal) { errno = EINVAL; return -1; }
    if (!terminal->suspended) return t4_terminal_resize(terminal);
    if (tcsetattr(STDIN_FILENO, TCSANOW, &terminal->editing) < 0 || geometry(terminal) < 0) return -1;
    terminal->suspended = 0;
    return layout(terminal);
}

void t4_terminal_close(struct t4_terminal *terminal)
{
    if (!terminal) return;
    (void)t4_terminal_suspend(terminal);
    for (unsigned at = 0; at < terminal->history_count; ++at) free(terminal->history[at]);
    t4_view_close(terminal->view_state);
    t4_screen_close(terminal->screen);
    free(terminal);
}
