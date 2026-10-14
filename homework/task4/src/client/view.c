#define _XOPEN_SOURCE 700

#include "task4/view.h"
#include "task4/text.h"
#include "task4/screen.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wchar.h>

#define TEXT_BUDGET (1024U * 1024U)
#define ITEM_BUDGET 128U

struct text {
    unsigned char *bytes;
    size_t length, capacity;
    unsigned char pending[4];
    unsigned pending_length, pending_width;
};
struct item {
    struct item *previous, *next;
    int turn, trimmed;
    uint32_t request, resumed;
    struct text prompt, thinking, answer;
    char phase[40], model[96], summary[192];
    uint32_t prompt_tokens, context_tokens, generated_tokens;
    double rate, elapsed;
};
struct t4_view {
    struct item *first, *last, *active;
    size_t bytes, items, total_lines, top_line;
    unsigned rows, columns, height;
    int dirty, following, dark, thinking_visible;
    char session[96], model[96];
};

const char *t4_view_style(const struct t4_view *view, enum t4_view_style style)
{
    static const char *const light[] = {
        "\033[0;38;2;0;0;0;48;2;248;249;251m",
        "\033[0;38;2;102;109;121;48;2;248;249;251m",
        "\033[1;38;2;49;66;96;48;2;248;249;251m",
        "\033[0;38;2;199;204;214;48;2;248;249;251m"
    };
    static const char *const dark[] = {
        "\033[0;38;2;227;230;237;48;2;22;25;31m",
        "\033[0;38;2;151;159;174;48;2;22;25;31m",
        "\033[1;38;2;162;189;235;48;2;22;25;31m",
        "\033[0;38;2;66;73;86;48;2;22;25;31m"
    };
    return view->dark ? dark[style] : light[style];
}

static size_t character(const unsigned char *bytes, size_t at, size_t length, int *columns)
{
    unsigned width = t4_character_width(bytes[at]);
    if (!width || width > length - at || !t4_character_valid(bytes + at, width)) {
        *columns = 1; return 1;
    }
    mbstate_t state = {0};
    wchar_t decoded;
    size_t result = mbrtowc(&decoded, (const char *)bytes + at, width, &state);
    *columns = result == (size_t)-1 || result == (size_t)-2 ? 1 : wcwidth(decoded);
    if (*columns < 0) *columns = 1;
    return width;
}

static int reserve(struct text *text, size_t extra)
{
    if (extra > SIZE_MAX - text->length - 1) { errno = EOVERFLOW; return -1; }
    size_t needed = text->length + extra + 1;
    if (needed <= text->capacity) return 0;
    size_t capacity = text->capacity ? text->capacity : 256;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2) { capacity = needed; break; }
        capacity *= 2;
    }
    void *replacement = realloc(text->bytes, capacity);
    if (!replacement) return -1;
    text->bytes = replacement; text->capacity = capacity;
    return 0;
}

/* Transcript text cannot move the cursor or inject ANSI/OSC commands. */
static int append(struct t4_view *view, struct text *text, const void *data, size_t length)
{
    const unsigned char *bytes = data;
    if (!data && length) { errno = EINVAL; return -1; }
    for (size_t at = 0; at < length;) {
        const unsigned char *piece;
        unsigned width;
        if (!text->pending_length) {
            width = t4_character_width(bytes[at]);
            if (width <= 1) { piece = bytes + at++; width = 1; }
            else {
                text->pending_width = width;
                text->pending[0] = bytes[at++]; text->pending_length = 1;
                continue;
            }
        } else {
            if ((bytes[at] & 0xc0U) != 0x80U) {
                piece = (const unsigned char *)"?"; width = 1;
                text->pending_length = 0;
            } else {
                text->pending[text->pending_length++] = bytes[at++];
                if (text->pending_length != text->pending_width) continue;
                piece = text->pending; width = text->pending_width;
                text->pending_length = 0;
            }
        }
        unsigned value = piece[0];
        if (width == 2 && piece[0] == 0xc2) value = piece[1];
        if ((width == 1 && value < 32 && value != '\n' && value != '\r' && value != '\t')
            || (width == 1 && value == 127) || (width == 2 && piece[0] == 0xc2 && value >= 128 && value <= 159)) {
            continue;
        }
        if (reserve(text, width) < 0) return -1;
        if (t4_character_valid(piece, width)) {
            memcpy(text->bytes + text->length, piece, width); text->length += width;
            if (width == 1 && value == '\t') text->bytes[text->length - 1] = ' ';
            view->bytes += width;
        } else { text->bytes[text->length++] = '?'; ++view->bytes; }
        text->bytes[text->length] = 0;
    }
    view->dirty = 1;
    return 0;
}

static void label(char *target, size_t capacity, const char *source)
{
    size_t used = 0, length = source ? strlen(source) : 0;
    for (size_t at = 0; at < length;) {
        int columns;
        size_t width = character((const unsigned char *)source, at, length, &columns);
        (void)columns;
        unsigned char first = (unsigned char)source[at];
        if (used + width >= capacity) break;
        if (first >= 32 && first != 127 && t4_character_valid((const unsigned char *)source + at, width)
            && !(width == 2 && first == 0xc2 && (unsigned char)source[at + 1] >= 128 && (unsigned char)source[at + 1] <= 159)) {
            memcpy(target + used, source + at, width); used += width;
        } else if (first == '\n' || first == '\r' || first == '\t') target[used++] = ' ';
        at += width;
    }
    target[used] = 0;
}

static void destroy_item(struct t4_view *view, struct item *item)
{
    if (item->previous) item->previous->next = item->next; else view->first = item->next;
    if (item->next) item->next->previous = item->previous; else view->last = item->previous;
    view->bytes -= item->prompt.length + item->thinking.length + item->answer.length;
    free(item->prompt.bytes); free(item->thinking.bytes); free(item->answer.bytes); free(item);
    --view->items;
}

static void trim(struct t4_view *view)
{
    while (view->bytes > TEXT_BUDGET || view->items > ITEM_BUDGET) {
        struct item *old = view->first;
        while (old && old == view->active) old = old->next;
        if (old) { destroy_item(view, old); view->top_line = 0; continue; }
        struct item *item = view->active;
        if (!item) break;
        struct text *text = item->thinking.length > item->answer.length ? &item->thinking : &item->answer;
        if (!text->length) break;
        size_t drop = view->bytes - TEXT_BUDGET;
        if (drop < text->length / 4) drop = text->length / 4;
        if (drop > text->length) drop = text->length;
        while (drop < text->length && (text->bytes[drop] & 0xc0U) == 0x80U) ++drop;
        memmove(text->bytes, text->bytes + drop, text->length - drop + 1);
        text->length -= drop; view->bytes -= drop; item->trimmed = 1;
    }
}

static struct item *new_item(struct t4_view *view, int turn)
{
    struct item *item = calloc(1, sizeof(*item));
    if (!item) return NULL;
    item->turn = turn; item->previous = view->last;
    if (view->last) view->last->next = item; else view->first = item;
    view->last = item; ++view->items; view->dirty = 1;
    return item;
}

struct t4_view *t4_view_open(void)
{
    struct t4_view *view = calloc(1, sizeof(*view));
    if (view) { view->following = 1; view->thinking_visible = 1; view->dirty = 1; }
    return view;
}

void t4_view_close(struct t4_view *view)
{
    if (!view) return;
    while (view->first) destroy_item(view, view->first);
    free(view);
}

void t4_view_reset(struct t4_view *view)
{
    while (view->first) destroy_item(view, view->first);
    view->active = NULL; view->top_line = view->total_lines = 0;
    view->session[0] = view->model[0] = 0;
    view->following = 1; view->dirty = 1;
}

void t4_view_resize(struct t4_view *view, unsigned rows, unsigned columns)
{
    view->rows = rows; view->columns = columns; view->dirty = 1;
}
void t4_view_dirty(struct t4_view *view) { view->dirty = 1; }
int t4_view_is_dirty(const struct t4_view *view) { return view->dirty; }

int t4_view_notice(struct t4_view *view, const void *data, size_t length)
{
    if (!length) return 0;
    struct item *item = view->last;
    if (!item || item->turn) item = new_item(view, 0);
    if (!item || append(view, &item->prompt, data, length) < 0) return -1;
    trim(view); return 0;
}

int t4_view_session(struct t4_view *view, const char *session, const char *model)
{
    label(view->session, sizeof(view->session), session);
    if (model && *model) label(view->model, sizeof(view->model), model);
    view->dirty = 1; return 0;
}

int t4_view_begin(struct t4_view *view, uint32_t request, const char *prompt, uint32_t offset)
{
    struct item *item = new_item(view, 1);
    if (!item) return -1;
    item->request = request; item->resumed = offset;
    label(item->phase, sizeof(item->phase), offset ? "resuming" : "processing");
    label(item->model, sizeof(item->model), view->model);
    view->active = item; view->following = 1;
    if (append(view, &item->prompt, prompt, prompt ? strlen(prompt) : 0) < 0) return -1;
    trim(view); return 0;
}

int t4_view_thinking(struct t4_view *view, const void *data, size_t length)
{
    if (!view->active) return t4_view_notice(view, data, length);
    label(view->active->phase, sizeof(view->active->phase), "thinking");
    if (append(view, &view->active->thinking, data, length) < 0) return -1;
    trim(view); return 0;
}

int t4_view_answer(struct t4_view *view, const void *data, size_t length)
{
    if (!view->active) return t4_view_notice(view, data, length);
    label(view->active->phase, sizeof(view->active->phase), "answering");
    if (append(view, &view->active->answer, data, length) < 0) return -1;
    trim(view); return 0;
}

int t4_view_status(struct t4_view *view, const struct t4_ui_status *status)
{
    if (!status) { errno = EINVAL; return -1; }
    if (status->model && *status->model) label(view->model, sizeof(view->model), status->model);
    struct item *item = view->active;
    if (item) {
        if (status->phase && *status->phase) label(item->phase, sizeof(item->phase), status->phase);
        if (status->model && *status->model) label(item->model, sizeof(item->model), status->model);
        item->prompt_tokens = status->prompt_tokens; item->context_tokens = status->context_tokens;
        item->generated_tokens = status->generated_tokens;
        item->rate = isfinite(status->tokens_per_second) && status->tokens_per_second >= 0 ? status->tokens_per_second : 0;
        item->elapsed = isfinite(status->elapsed_seconds) && status->elapsed_seconds >= 0 ? status->elapsed_seconds : 0;
    }
    view->dirty = 1; return 0;
}

int t4_view_end(struct t4_view *view, const char *state, const char *summary)
{
    if (view->active) {
        label(view->active->phase, sizeof(view->active->phase), state);
        label(view->active->summary, sizeof(view->active->summary), summary);
    }
    view->dirty = 1; return 0;
}

void t4_view_theme(struct t4_view *view, int dark) { view->dark = dark != 0; view->dirty = 1; }
void t4_view_thinking_visible(struct t4_view *view, int show) { view->thinking_visible = show != 0; view->dirty = 1; }

void t4_view_page(struct t4_view *view, int backward)
{
    size_t step = view->height > 2 ? view->height - 2 : 1;
    t4_view_scroll(view, backward, step);
}

void t4_view_scroll(struct t4_view *view, int backward, size_t step)
{
    size_t last = view->total_lines > view->height ? view->total_lines - view->height : 0;
    if (view->following) view->top_line = last;
    if (backward) {
        view->top_line = view->top_line > step ? view->top_line - step : 0;
        view->following = 0;
    } else {
        view->top_line = view->top_line + step < last ? view->top_line + step : last;
        view->following = view->top_line == last;
    }
    view->dirty = 1;
}

struct layout { struct t4_view *view; struct t4_screen *screen; size_t line, start, end; unsigned top, width; int paint, error; };

static void row_at(struct layout *layout, const unsigned char *text, size_t length,
                   enum t4_view_style style, unsigned indent)
{
    size_t number = layout->line++;
    if (!layout->paint || number < layout->start || number >= layout->end || layout->error) return;
    if (t4_screen_row(layout->screen, layout->top + (unsigned)(number - layout->start),
                      t4_view_style(layout->view, style)) < 0) { layout->error = 1; return; }
    for (unsigned at = 0; at < indent; ++at)
        if (t4_screen_append(layout->screen, " ", 1) < 0) { layout->error = 1; return; }
    if (length && t4_screen_append(layout->screen, text, length) < 0) layout->error = 1;
}

static void wrapped(struct layout *layout, const unsigned char *text, size_t length,
                    enum t4_view_style style, unsigned indent)
{
    size_t start = 0, at = 0;
    unsigned used = 0, available = layout->width > indent + 1 ? layout->width - indent - 1 : 1;
    while (at < length) {
        if (text[at] == '\n' || text[at] == '\r') {
            row_at(layout, text + start, at - start, style, indent);
            if (text[at++] == '\r' && at < length && text[at] == '\n') ++at;
            start = at; used = 0; continue;
        }
        int columns;
        size_t width = character(text, at, length, &columns);
        if (text[at] == '\t') columns = 1;
        if (used + (unsigned)columns > available && at > start) {
            row_at(layout, text + start, at - start, style, indent);
            start = at; used = 0; continue;
        }
        used += (unsigned)columns; at += width;
    }
    if (start < length || !length) row_at(layout, text ? text + start : NULL, length - start, style, indent);
}

static void line_text(struct layout *layout, const char *text, enum t4_view_style style)
{
    wrapped(layout, (const unsigned char *)text, strlen(text), style, 1);
}

static void transcript(struct layout *layout)
{
    struct t4_view *view = layout->view;
    char text[512];
    for (struct item *item = view->first; item; item = item->next) {
        if (!item->turn) { wrapped(layout, item->prompt.bytes, item->prompt.length, T4_VIEW_MUTED, 1); continue; }
        (void)snprintf(text, sizeof(text), "YOU  /  %u%s", item->request, item->resumed ? "  (resumed)" : "");
        line_text(layout, text, T4_VIEW_ACCENT);
        wrapped(layout, item->prompt.bytes, item->prompt.length, T4_VIEW_TEXT, 2);
        line_text(layout, "", T4_VIEW_TEXT);
        line_text(layout, item->model[0] ? item->model : "ASSISTANT", T4_VIEW_ACCENT);
        if (item->trimmed) line_text(layout, "[earlier display text trimmed; durable checkpoint is complete]", T4_VIEW_MUTED);
        if (item->thinking.length && view->thinking_visible) {
            line_text(layout, "Thinking", T4_VIEW_MUTED);
            wrapped(layout, item->thinking.bytes, item->thinking.length, T4_VIEW_MUTED, 2);
            if (item->answer.length) line_text(layout, "", T4_VIEW_TEXT);
        } else if (item->thinking.length) line_text(layout, "[thinking hidden  /  /thinking to show]", T4_VIEW_MUTED);
        size_t first = 0;
        while (first < item->answer.length && (item->answer.bytes[first] == '\n' || item->answer.bytes[first] == '\r')) ++first;
        if (first < item->answer.length)
            wrapped(layout, item->answer.bytes + first, item->answer.length - first, T4_VIEW_TEXT, 2);
        (void)snprintf(text, sizeof(text), "%s  /  %u tok  /  %.1f tok/s  /  %.1fs  /  ctx %u/%u%s%s",
                       item->phase, item->generated_tokens, item->rate, item->elapsed,
                       item->prompt_tokens, item->context_tokens,
                       item->summary[0] ? "  /  " : "", item->summary);
        line_text(layout, text, T4_VIEW_MUTED);
        line_text(layout, "", T4_VIEW_TEXT);
    }
}

static int fixed_row(struct t4_view *view, struct t4_screen *screen, unsigned row, const char *text, enum t4_view_style style)
{
    if (t4_screen_row(screen, row, t4_view_style(view, style)) < 0) return -1;
    size_t length = strlen(text), at = 0;
    unsigned used = 0;
    while (at < length) {
        int columns;
        size_t width = character((const unsigned char *)text, at, length, &columns);
        if (used + (unsigned)columns >= view->columns) break;
        used += (unsigned)columns; at += width;
    }
    return t4_screen_append(screen, text, at);
}

int t4_view_render(struct t4_view *view, struct t4_screen *screen)
{
    if (view->rows < 4 || view->columns < 12) {
        for (unsigned row = 1; row <= view->rows; ++row)
            if (fixed_row(view, screen, row, "", T4_VIEW_TEXT) < 0) return -1;
        int result = fixed_row(view, screen, 1, "Enlarge window to edit", T4_VIEW_MUTED);
        if (result == 0) view->dirty = 0;
        return result;
    }
    int compact = view->columns < 52 || view->rows < 12;
    unsigned header = compact ? 1 : 2;
    view->height = view->rows - header - (compact ? 2U : 3U);
    struct layout layout = {.view = view, .screen = screen, .top = header + 1, .width = view->columns};
    transcript(&layout); view->total_lines = layout.line;
    size_t last = layout.line > view->height ? layout.line - view->height : 0;
    if (view->following) view->top_line = last;
    if (view->top_line > last) view->top_line = last;
    char text[512];
    (void)snprintf(text, sizeof(text), " TASK4  /  %s", view->model[0] ? view->model : "local conversation");
    if (fixed_row(view, screen, 1, text, T4_VIEW_ACCENT) < 0) return -1;
    if (!compact) {
        struct item *item = view->active;
        (void)snprintf(text, sizeof(text), " session %.12s  /  context %u / %u  /  %s",
                       view->session[0] ? view->session : "connecting", item ? item->prompt_tokens : 0,
                       item ? item->context_tokens : 0, view->following ? "live" : "scrollback - PgDn returns to live");
        if (fixed_row(view, screen, 2, text, T4_VIEW_MUTED) < 0) return -1;
    }
    layout.line = 0; layout.paint = 1; layout.start = view->top_line; layout.end = layout.start + view->height;
    transcript(&layout);
    if (layout.error) return -1;
    size_t shown = layout.line > layout.start ? layout.line - layout.start : 0;
    if (shown > view->height) shown = view->height;
    for (unsigned at = (unsigned)shown; at < view->height; ++at)
        if (fixed_row(view, screen, header + 1 + at, "", T4_VIEW_TEXT) < 0) return -1;
    if (!compact) {
        size_t count = view->columns - 1;
        char *rule = malloc(count + 1);
        if (!rule) return -1;
        memset(rule, '-', count); rule[count] = 0;
        int result = fixed_row(view, screen, view->rows - 2, rule, T4_VIEW_BORDER);
        free(rule); if (result < 0) return -1;
    }
    struct item *item = view->active;
    int idle = !item || !strcmp(item->phase, "completed") || !strcmp(item->phase, "cancelled") || !strcmp(item->phase, "error");
    (void)snprintf(text, sizeof(text), " %s%s  |  Enter send  Ctrl+C %s  Wheel/PgUp scroll  /help",
                   view->following ? "" : "SCROLLBACK  /  ", item && item->phase[0] ? item->phase : "ready",
                   idle ? "clear" : "stop");
    if (fixed_row(view, screen, view->rows - 1, text, T4_VIEW_MUTED) < 0) return -1;
    view->dirty = 0;
    return 0;
}
