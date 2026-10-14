#define _GNU_SOURCE

#include "task4/checkpoint.h"
#include "task4/client_state.h"
#include "task4/discovery.h"
#include "task4/protocol.h"
#include "task4/status.h"
#include "task4/terminal.h"
#include "task4/text.h"

#include <errno.h>
#include <limits.h>
#include <locale.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>

static volatile sig_atomic_t stopped;
static volatile sig_atomic_t interrupt_requested;
static volatile sig_atomic_t resize_requested, suspend_requested;
static int interactive_signals;
static int verbose_status, show_thinking = 1;
static char last_server_error[T4_MAX_FRAME + 1];
static char timeout_detail[256];
static struct t4_terminal *terminal;

static int timeout_failure(const char *operation, int milliseconds, const char *limit)
{
    (void)snprintf(timeout_detail, sizeof(timeout_detail),
                   "Connection timed out while %s (%.1f s %s)",
                   operation, milliseconds / 1000.0, limit);
    errno = ETIMEDOUT;
    return -1;
}
static void stop_signal(int signo)
{
    if (signo == SIGINT && interactive_signals) interrupt_requested = 1;
    else if (signo == SIGWINCH) resize_requested = 1;
    else if (signo == SIGTSTP) suspend_requested = 1;
    else stopped = signo;
}

/* All terminal output uses the same renderer; the editable draft has its own
 * row and is never reconstructed from model text or discarded by an END. */
static void message(const char *format, ...)
{
    char text[8192];
    va_list args;
    va_start(args, format);
    int size = vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    if (size < 0) return;
    size_t length = (size_t)size < sizeof(text) ? (size_t)size : sizeof(text) - 1;
    if (terminal) (void)t4_terminal_write(terminal, text, length);
    else { (void)fwrite(text, 1, length, stderr); (void)fflush(stderr); }
}

static int output(const void *data, size_t length, int answer)
{
    if (terminal && (answer ? t4_terminal_answer(terminal, data, length)
                           : t4_terminal_thinking(terminal, data, length)) < 0) return -1;
    if (!terminal || (answer && !isatty(STDOUT_FILENO))) {
        FILE *stream = answer ? stdout : stderr;
        if (fwrite(data, 1, length, stream) != length || fflush(stream) == EOF) return -1;
    }
    return 0;
}

static int terminal_signals(void)
{
    if (suspend_requested) {
        suspend_requested = 0;
        if (terminal && t4_terminal_suspend(terminal) < 0) return -1;
        /* Restore the shell's tty BEFORE stopping. SIGSTOP also works in a
         * session whose process group is orphaned (e.g. a PTY test harness). */
        if (raise(SIGSTOP)) return -1;
        if (terminal && t4_terminal_resume(terminal) < 0) return -1;
    }
    if (resize_requested) {
        resize_requested = 0;
        if (terminal && t4_terminal_resize(terminal) < 0) return -1;
    }
    return stopped ? (errno = EINTR, -1) : 0;
}

struct controls {
    int interactive, quit, cancel_pending;
    uint32_t previous_request, previous_offset;
    const char *session_dir;
};

struct connection_options;
struct conversation {
    const struct connection_options *options;
    char *path;
    struct t4_checkpoint *checkpoint;
    int *fd, *lock;
};

/* 0: not a navigation command; 1: handled in place; 2: changed conversation.
 * A rejected command must leave ask()'s socket and receive state untouched. */
static int navigation(const unsigned char *line, struct controls *u, struct conversation *chat);

static int session_row(const struct t4_client_session *session, void *opaque)
{
    (void)opaque;
    message("%s  [%s]  /switch %s\n", session->id, session->status, session->id);
    return 0;
}

static void sessions(const char *directory)
{
    message("Conversations in %s (current conversation may be marked active):\n", directory);
    if (t4_client_state_scan(directory, session_row, NULL) < 0)
        message("[sessions] %s\n", strerror(errno));
    message("/new | /switch ID | /delete ID (local recycle bin)\n");
}

static int64_t milliseconds(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) return -1;
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

/* Block handlers while testing their flags, then atomically unblock inside
 * ppoll. Otherwise Ctrl+C between the flag check and poll could strand an
 * idle editor until another key was pressed. */
static int wait_descriptors(struct pollfd *fds, nfds_t count, int milliseconds)
{
    sigset_t mask, old;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT); sigaddset(&mask, SIGTERM); sigaddset(&mask, SIGHUP);
    sigaddset(&mask, SIGTSTP); sigaddset(&mask, SIGWINCH);
    if (sigprocmask(SIG_BLOCK, &mask, &old) < 0) return -1;
    int result;
    if (stopped || interrupt_requested || suspend_requested || resize_requested) {
        errno = EINTR; result = -1;
    } else {
        struct timespec timeout = {milliseconds / 1000, (milliseconds % 1000) * 1000000L};
        result = ppoll(fds, count, milliseconds < 0 ? NULL : &timeout, &old);
    }
    int saved = errno;
    if (sigprocmask(SIG_SETMASK, &old, NULL) < 0) return -1;
    errno = saved;
    return result;
}

static int read_line(FILE *input, unsigned char text[T4_MAX_PROMPT + 1], uint32_t *length);

/* Legacy file/canonical input uses 0 for EOF; the editor uses 0 for an edit
 * without submission. Keep those distinct in the protocol state machine. */
static int control_line(unsigned char *line, uint32_t *length)
{
    if (!terminal) return read_line(stdin, line, length);
    int result = t4_terminal_read(terminal, line, length);
    return result == 0 ? -3 : result == 2 ? 0 : result;
}

static int next_line(FILE *input, unsigned char *line, uint32_t *length)
{
    if (!terminal) return read_line(input, line, length);
    if (t4_terminal_prompt(terminal, "question> ") < 0) return -1;
    for (;;) {
        if (terminal_signals() < 0) return -1;
        if (t4_terminal_tick(terminal) < 0) return -1;
        if (interrupt_requested) {
            interrupt_requested = 0;
            t4_terminal_clear_input(terminal);
            message("[controls] input cleared; no active turn to cancel.\n");
        }
        int result = control_line(line, length);
        if (result != -3) return result;
        struct pollfd tty = {STDIN_FILENO, POLLIN, 0};
        int ready = wait_descriptors(&tty, 1, 33);
        if (ready < 0 && errno != EINTR) return -1;
    }
}

static void help(void)
{
    message("[conversations] /sessions: list | /new: new conversation | /switch ID or FILE\n"
                    "[conversations] /delete [ID]: move local conversation to recycle bin; server copy retained\n"
                    "[recovery] /retry: reconnect and continue the saved turn; errors stay in this screen\n"
                    "[controls] /stop or Ctrl+C: cancel this turn; /steer TEXT: cancel and correct\n"
                    "[controls] /quit: cancel and exit; /help: show controls\n"
                    "[controls] EOF/disconnect keeps the pending checkpoint for recovery.\n"
                    "[controls] While generating, submitting plain text also corrects this turn.\n"
                    "[controls] Arrows/Home/End edit; Up/Down history; pasted newlines become spaces.\n"
                    "[controls] Mouse wheel/PgUp/PgDn scroll chat; Up/Down recall input history.\n"
                    "[controls] /thinking hides/shows reasoning; /theme light|dark.\n"
                    "[controls] --cancel-pending cancels a saved pending turn on reconnect.\n");
}

/* Presentation controls never change a request, generation mode or history. */
static int display_control(const unsigned char *line, uint32_t length)
{
    if (!terminal) return 0;
    if (length == 9 && !memcmp(line, "/thinking", 9)) {
        show_thinking = !show_thinking;
        return t4_terminal_thinking_visible(terminal, show_thinking) < 0 ? -1 : 1;
    }
    if (length == 11 && !memcmp(line, "/theme dark", 11))
        return t4_terminal_theme(terminal, 1) < 0 ? -1 : 1;
    if (length == 12 && !memcmp(line, "/theme light", 12))
        return t4_terminal_theme(terminal, 0) < 0 ? -1 : 1;
    return 0;
}

static int text_valid(const unsigned char *text, uint32_t length)
{
    for (uint32_t at = 0; at < length;) {
        unsigned width = t4_character_width(text[at]);
        if (!width || width > length - at || !t4_character_valid(text + at, width)) return 0;
        at += width;
    }
    return 1;
}

/* A SIGINT must not discard a partially received/sent protocol frame. Defer
 * it only across one bounded frame transfer; SIGTERM still aborts recovery. */
static int frame_signal_mask(sigset_t *old)
{
    sigset_t mask;
    sigemptyset(&mask); sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGWINCH); sigaddset(&mask, SIGTSTP);
    return sigprocmask(SIG_BLOCK, &mask, old);
}

static int receive_frame(int fd, struct t4_frame *frame, int idle)
{
    sigset_t old;
    if (frame_signal_mask(&old) < 0) return -1;
    int result = t4_recv(fd, frame, idle), saved = errno;
    if (sigprocmask(SIG_SETMASK, &old, NULL) < 0) return -1;
    if (result < 0 && saved == ETIMEDOUT)
        return timeout_failure("receiving a complete server frame", idle, "frame deadline");
    if (!result) saved = ECONNRESET;
    errno = saved;
    return result;
}

static int send_frame(int fd, uint32_t type, uint32_t request, uint32_t offset,
                      const void *data, uint32_t length, int idle)
{
    struct t4_frame frame = {.type = type, .request_id = request,
                             .offset = offset, .length = length};
    if (length) memcpy(frame.data, data, length);
    sigset_t old;
    if (frame_signal_mask(&old) < 0) return -1;
    int result = t4_send(fd, &frame, idle), saved = errno;
    if (sigprocmask(SIG_SETMASK, &old, NULL) < 0) return -1;
    if (result < 0 && saved == ETIMEDOUT)
        return timeout_failure("sending a request frame", idle, "frame deadline");
    errno = saved;
    return result;
}

static int steer(const char *path, struct t4_checkpoint *c,
                 const unsigned char *correction, uint32_t length)
{
    static const unsigned char separator[] = "\n\nUser correction: ";
    const uint32_t extra = (uint32_t)sizeof(separator) - 1;
    if (!length || c->request >= 10000 || c->prompt_length > T4_MAX_PROMPT - extra ||
        length > T4_MAX_PROMPT - extra - c->prompt_length) {
        message("[controls] correction rejected: combined prompt must fit %u bytes.\n", T4_MAX_PROMPT);
        return 0;
    }
    if (c->queued_length) {
        message("[controls] correction already queued; waiting for cancellation.\n");
        return 0;
    }
    unsigned char combined[T4_MAX_PROMPT + 1];
    memcpy(combined, c->prompt, c->prompt_length);
    memcpy(combined + c->prompt_length, separator, extra);
    memcpy(combined + c->prompt_length + extra, correction, length);
    uint32_t total = c->prompt_length + extra + length;
    if (t4_checkpoint_queue(path, c, combined, total) < 0) return -1;
    message("[controls] correction queued durably; cancelling turn=%u.\n", c->request);
    return 1;
}

static int ask(int fd, const char *path, struct t4_checkpoint *c, int idle,
               struct controls *u, struct conversation *chat)
{
    last_server_error[0] = timeout_detail[0] = 0;
    if (send_frame(fd, T4_BEGIN, c->request, c->offset, NULL, 0, idle) < 0) return -1;
    for (uint32_t offset = 0; offset < c->prompt_length;) {
        uint32_t n = c->prompt_length - offset;
        if (n > 128) n = 128;
        if (send_frame(fd, T4_INPUT, c->request, offset, c->prompt + offset, n, idle) < 0) return -1;
        offset += n;
    }
    if (send_frame(fd, T4_COMMIT, c->request, c->prompt_length, NULL, 0, idle) < 0) return -1;
    if (terminal) {
        if (t4_terminal_begin_turn(terminal, c->request, (const char *)c->prompt, c->offset) < 0) return -1;
        if (c->offset && t4_terminal_answer(terminal, c->answer, c->offset) < 0) return -1;
    } else message("[client] turn=%u resume=%u prompt=%u bytes\n", c->request, c->offset, c->prompt_length);
    if (terminal && t4_terminal_prompt(terminal, "correct /stop> ") < 0) return -1;
    struct t4_frame frame;
    int cancel = c->queued_length != 0 || u->cancel_pending;
    u->cancel_pending = 0;
    int cancelled_sent = 0, phase = 0, thinking_seen = 0;
    uint32_t thinking_offset = 0;
    struct t4_client_status status;
    t4_status_init(&status);
    int64_t last_network = milliseconds();
    if (last_network < 0) return -1;
    for (;;) {
        if (terminal_signals() < 0) return -1;
        if (terminal && t4_terminal_tick(terminal) < 0) return -1;
        if (interrupt_requested) {
            interrupt_requested = 0; cancel = 1;
            if (terminal) t4_terminal_clear_input(terminal);
        }
        if (cancel && !cancelled_sent) {
            if (send_frame(fd, T4_CANCEL, c->request, c->offset, NULL, 0, idle) < 0) return -1;
            cancelled_sent = 1;
            if (terminal) {
                status.ui.phase = "cancelling";
                if (t4_terminal_status(terminal, &status.ui) < 0) return -1;
            } else message("[controls] cancellation requested for turn=%u; awaiting confirmation.\n", c->request);
        }
        struct pollfd descriptors[2] = {{fd, POLLIN, 0}, {STDIN_FILENO, POLLIN, 0}};
        int buffered = terminal && t4_terminal_pending(terminal);
        int64_t now = milliseconds();
        if (now < 0) return -1;
        int64_t remaining = idle - (now - last_network);
        /* A terminal write or suspended foreground job can delay this loop
         * while valid server frames accumulate in the socket. Inspect its
         * readiness before declaring silence; local rendering time does not
         * prove that the network was idle. Partial frames remain bounded by
         * receive_frame's independent, absolute transfer deadline. */
        int timeout = remaining <= 0 ? 0 : terminal && remaining > 33 ? 33 : (int)remaining;
        int ready = wait_descriptors(descriptors, u->interactive ? 2 : 1, buffered ? 0 : timeout);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0) return -1;
        if (remaining <= 0 && !descriptors[0].revents)
            return timeout_failure("waiting for server data", idle, "network idle limit");
        if (!ready && !buffered) continue; /* Render clock never refreshes the network deadline. */
        if (buffered) descriptors[1].revents |= POLLIN;
        if (u->interactive && descriptors[1].revents) {
            unsigned char line[T4_MAX_PROMPT + 1];
            uint32_t length;
            int received = control_line(line, &length);
            if (received == -1 && errno == EINTR) { clearerr(stdin); continue; }
            if (received != -3 && received <= 0) {
                if (received == -2) {
                    message("[controls] invalid control line skipped.\n");
                    continue;
                }
                errno = received ? errno : ECONNRESET;
                return -1; /* EOF is a resumable disconnect, not an explicit CANCEL. */
            }
            int display = received == 1 ? display_control(line, length) : 0;
            if (display < 0) return -1;
            int navigated = received == 1 && !display ? navigation(line, u, chat) : 0;
            if (navigated < 0 || navigated == 2) return navigated;
            if (received == -3 || display || navigated) { /* UI-only edits never submit a request. */ }
            else if (length == 5 && !memcmp(line, "/help", 5)) help();
            else if (length == 9 && !memcmp(line, "/sessions", 9)) sessions(u->session_dir);
            else if (length == 6 && !memcmp(line, "/retry", 6)) message("[recovery] this turn is already running.\n");
            else if (cancelled_sent) message("[controls] waiting for cancellation confirmation.\n");
            else if (length == 5 && !memcmp(line, "/stop", 5)) cancel = 1;
            else if (length == 5 && !memcmp(line, "/quit", 5)) { cancel = 1; u->quit = 1; }
            else if (length > 7 && !memcmp(line, "/steer ", 7)) {
                int queued = steer(path, c, line + 7, length - 7);
                if (queued < 0) return -1;
                if (queued) cancel = 1;
            } else if (length && line[0] != '/') {
                int queued = steer(path, c, line, length);
                if (queued < 0) return -1;
                if (queued) cancel = 1;
            } else message("[controls] use /steer TEXT to correct an active turn, or /stop.\n");
            if (cancel && !cancelled_sent) continue;
        }
        if (!(descriptors[0].revents)) continue;
        if (receive_frame(fd, &frame, idle) != 1) return -1;
        last_network = milliseconds();
        if (last_network < 0) return -1;
        if (frame.type == T4_ERROR) {
            memcpy(last_server_error, frame.data, frame.length);
            last_server_error[frame.length] = 0;
            if (terminal) (void)t4_terminal_end_turn(terminal, "error", (const char *)frame.data);
            message("[server error] %.*s\n", (int)frame.length, frame.data);
            errno = EPROTO; return -1;
        }
        /* END may win a race with CANCEL. The READY server can then acknowledge
         * the already terminal old turn while we have started the next one. */
        if (u->previous_request && (frame.type == T4_END || frame.type == T4_CANCELLED) && !frame.length &&
            frame.request_id == u->previous_request && frame.offset == u->previous_offset) continue;
        if (frame.request_id != c->request) {
            errno = EPROTO; return -1;
        }
        if (frame.type == T4_THINK) {
            if (!frame.length || !text_valid(frame.data, frame.length) ||
                (thinking_seen && frame.offset != thinking_offset) ||
                frame.offset > T4_MAX_RESPONSE ||
                frame.length > T4_MAX_RESPONSE - frame.offset) { errno = EPROTO; return -1; }
            thinking_seen = 1; thinking_offset = frame.offset + frame.length;
            if (phase != 1) {
                if (!terminal) message("\n[thinking] ");
                phase = 1;
            }
            if (output(frame.data, frame.length, 0) < 0) return -1;
            continue;
        }
        if (frame.offset != c->offset) { errno = EPROTO; return -1; }
        if (frame.type == T4_STATUS && text_valid(frame.data, frame.length)) {
            if (t4_status_update(&status, (const char *)frame.data, frame.length) == 0) {
                /* Generator metrics precede worker exit and durable END. */
                if (!strcmp(status.phase, "completed")) strcpy(status.phase, "finishing");
                if (!strcmp(status.phase, "generating"))
                    status.ui.phase = phase == 2 ? "answering" : phase == 1 ? "thinking" : "generating";
                if (cancelled_sent) status.ui.phase = "cancelling";
                if (terminal && t4_terminal_status(terminal, &status.ui) < 0) return -1;
                if (!terminal && verbose_status) message("\n[status] %.*s\n", (int)frame.length, frame.data);
            } else if (!terminal && verbose_status) {
                /* Retain support for older peers' one-line textual STATUS. */
                message("\n[status] %.*s\n", (int)frame.length, frame.data);
            }
            continue;
        }
        if (frame.type == T4_HEARTBEAT && !frame.length) continue;
        if ((frame.type == T4_END || frame.type == T4_CANCELLED) && !frame.length) {
            c->done = frame.type == T4_END ? 1U : 2U;
            if (t4_checkpoint_save(path, c) < 0) return -1;
            if (!terminal) { if (output("\n", 1, 1) < 0) return -1; }
            else if (!isatty(STDOUT_FILENO) && (fputc('\n', stdout) == EOF || fflush(stdout) == EOF)) return -1;
            if (terminal) {
                if (t4_terminal_end_turn(terminal, c->done == 2 ? "cancelled" : "completed", NULL) < 0) return -1;
            } else {
                message("\n[client] %s turn=%u bytes=%u\n",
                        c->done == 2 ? "cancelled" : "completed", c->request, c->offset);
                if (status.ui.context_tokens)
                    message("[stats] input=%u context=%u generated=%u elapsed=%.1fs rate=%.1f tok/s\n",
                            status.ui.prompt_tokens, status.ui.context_tokens, status.ui.generated_tokens,
                            status.ui.elapsed_seconds, status.ui.tokens_per_second);
            }
            u->previous_request = c->request; u->previous_offset = c->offset;
            return 0;
        }
        if (frame.type != T4_DELTA || !t4_character_valid(frame.data, frame.length) ||
            frame.length > T4_MAX_RESPONSE - c->offset) { errno = EPROTO; return -1; }
        memcpy(c->answer + c->offset, frame.data, frame.length);
        c->offset += frame.length;
        /* 游标与完整正文先落盘；终端不是持久介质，恢复只打印尚未保存的后缀。 */
        if (t4_checkpoint_save(path, c) < 0) return -1;
        if (phase != 2) { if (!terminal) message("\n[answer]\n"); phase = 2; }
        if (output(frame.data, frame.length, 1) < 0) return -1;
        if (stopped) return -1;
    }
}

/* 消费超长/NUL整行，尾部不会被错误地变成下一条问题。 */
static int read_line(FILE *input, unsigned char text[T4_MAX_PROMPT + 1], uint32_t *length)
{
    int byte, invalid = 0, any = 0;
    *length = 0;
    while ((byte = fgetc(input)) != EOF && byte != '\n') {
        any = 1;
        if (!byte || *length == T4_MAX_PROMPT) invalid = 1;
        else text[(*length)++] = (unsigned char)byte;
    }
    text[*length] = 0;
    if (ferror(input)) return -1;
    if (invalid) return -2;
    return byte == EOF && !any ? 0 : 1;
}

static void usage(const char *program)
{
    message("Usage: %s [--host HOST --port 1..65535 | --server-state DIR]\n"
            "  [--discovery-dir DIR]\n"
            "  [--session-dir DIR | --resume FILE | --checkpoint FILE]\n"
            "  [--input FILE] [--resume-only | --cancel-pending] [--idle-ms 1500..3600000]\n"
            "  [--theme light|dark] [--plain] [--verbose-status]\n"
            "  %s --list-sessions [--session-dir DIR]\n"
            "Default: discover one active local server via state/services, then create an independent session.\n"
            "--resume requires an existing checkpoint; --checkpoint creates or resumes.\n", program, program);
}

static int discover(const char *registry, const char *state, const char *token,
                    struct t4_endpoint *endpoint)
{
    if (t4_service_discover(registry, state, token, endpoint) == 0) return 0;
    int saved = errno;
    if (saved == EEXIST)
        message("[client] multiple matching servers; select --server-state DIR or --host HOST --port PORT.\n");
    else if (saved == ENOENT)
        message("[client] no active matching server in %s; start the server first%s.\n",
                state ? state : registry,
                token && *token ? " using the original --state directory for this checkpoint" : "");
    else message("[client] cannot discover server: %s; check --server-state or --discovery-dir.\n", strerror(saved));
    errno = saved;
    return -1;
}

struct connection_options {
    const char *host, *server_state, *discovery_dir;
    uint32_t port, idle;
    int explicit_port;
};

/* Every reconnect resolves the current endpoint; inference and checkpoint
 * identity remain owned by the existing session, never by a cached port. */
static int connect_session(const struct connection_options *options, const char *path,
                           struct t4_checkpoint *c)
{
    last_server_error[0] = timeout_detail[0] = 0;
    struct t4_endpoint endpoint;
    const char *host = options->host;
    uint16_t port = (uint16_t)options->port;
    int idle = (int)options->idle;
    if (!options->explicit_port) {
        if (discover(options->discovery_dir, options->server_state, c->session, &endpoint) < 0) return -1;
        host = endpoint.host; port = endpoint.port;
    }
    int fd = t4_connect(host, port, idle);
    if (fd < 0) {
        if (errno == ETIMEDOUT) return timeout_failure("connecting to the server", idle, "connection deadline");
        return -1;
    }
    struct t4_frame frame;
    if (!options->explicit_port) {
        if (t4_service_validate(&endpoint) < 0 || send_frame(fd, T4_PROBE, 0, 0, NULL, 0, idle) < 0 ||
            receive_frame(fd, &frame, idle) != 1) goto failed;
        if (frame.type == T4_ERROR) goto server_error;
        if (frame.type != T4_SERVICE || frame.request_id || frame.offset || frame.length != 32 ||
            memcmp(frame.data, endpoint.instance, 32) || t4_service_validate(&endpoint) < 0) {
            message("[client] discovered server changed; no HELLO was sent. Retry after startup completes.\n");
            errno = ESTALE; goto failed;
        }
        message("[client] discovered %s:%u state=%s\n", host, port, endpoint.state);
    }
    if (send_frame(fd, T4_HELLO, 0, 0, c->session, (uint32_t)strlen(c->session), idle) < 0 ||
        receive_frame(fd, &frame, idle) != 1) goto failed;
    if (frame.type == T4_ERROR) goto server_error;
    if (frame.type != T4_WELCOME || frame.request_id || frame.offset || frame.length != 32) {
        errno = EPROTO; goto failed;
    }
    for (uint32_t i = 0; i < 32; ++i)
        if (!((frame.data[i] >= '0' && frame.data[i] <= '9') ||
              (frame.data[i] >= 'a' && frame.data[i] <= 'f'))) { errno = EPROTO; goto failed; }
    if (c->session[0] && memcmp(c->session, frame.data, 32)) { errno = EPROTO; goto failed; }
    memcpy(c->session, frame.data, 32); c->session[32] = 0;
    if (t4_checkpoint_save(path, c) < 0) goto failed;
    return fd;
server_error:
    memcpy(last_server_error, frame.data, frame.length); last_server_error[frame.length] = 0;
    errno = EPROTO;
failed:
    {
        int saved = errno ? errno : ECONNRESET;
        close(fd); errno = saved; return -1;
    }
}

static int display_saved(const struct t4_checkpoint *saved, void *opaque)
{
    uint32_t skip = *(const uint32_t *)opaque;
    if (!saved->request || saved->request == skip) return 0;
    return t4_terminal_history(terminal, (const char *)saved->prompt) < 0 ||
           t4_terminal_begin_turn(terminal, saved->request, (const char *)saved->prompt, 0) < 0 ||
           t4_terminal_answer(terminal, saved->answer, saved->offset) < 0 ||
           t4_terminal_end_turn(terminal, saved->done == 2 ? "cancelled" : "completed", "saved conversation") < 0 ? -1 : 0;
}

static int restore_display(const char *path, const struct t4_checkpoint *c, int show_pending)
{
    if (t4_terminal_reset(terminal) < 0 || t4_terminal_session(terminal, c->session, "local") < 0) return -1;
    if (c->session[0] && t4_client_history_scan(path, c->session, display_saved, (void *)&c->request) < 0)
        message("[history] saved history unavailable: %s; current checkpoint retained.\n", strerror(errno));
    if (c->request && t4_terminal_history(terminal, (const char *)c->prompt) < 0) return -1;
    if (c->request && (c->done || show_pending)) {
        if (t4_terminal_begin_turn(terminal, c->request, (const char *)c->prompt, c->offset) < 0 ||
            t4_terminal_answer(terminal, c->answer, c->offset) < 0 ||
            t4_terminal_end_turn(terminal, c->done == 2 ? "cancelled" : c->done ? "completed" : "paused",
                                 c->done ? "restored from checkpoint" : "/retry to continue") < 0) return -1;
    }
    message("Checkpoint: %s\n/help  /sessions  /new\n", path);
    return 0;
}

static int archive_turn(const char *path, const struct t4_checkpoint *c)
{
    if (c->request && c->done && t4_client_history_archive(path, c) < 0) {
        message("[history] could not archive this turn: %s; latest checkpoint remains saved.\n", strerror(errno));
        return -1;
    }
    return 0;
}

static void disconnect_session(int *fd)
{
    if (*fd >= 0) close(*fd);
    *fd = -1;
}

static void conversation_error(int *fd)
{
    int saved = errno ? errno : EPROTO;
    const char *detail = last_server_error[0] ? last_server_error :
        saved == ETIMEDOUT && timeout_detail[0] ? timeout_detail : strerror(saved);
    disconnect_session(fd);
    (void)t4_terminal_end_turn(terminal, "error", detail);
    message("[conversation error] %s\nSaved checkpoint retained. /retry to reconnect; /stop to cancel; /new or /switch to change conversation.\n", detail);
    last_server_error[0] = timeout_detail[0] = 0;
    errno = saved;
}

static int same_file(const char *a, const char *b)
{
    struct stat left, right;
    return stat(a, &left) == 0 && stat(b, &right) == 0 &&
           left.st_dev == right.st_dev && left.st_ino == right.st_ino;
}

/* Stage and lock the destination before releasing the current checkpoint.
 * Return navigation's handled/changed result; -1 is a terminal failure after
 * committing the switch. Rejected targets preserve a live generation too. */
static int switch_session(const char *target, const struct connection_options *options,
                          struct controls *u, char path[PATH_MAX], struct t4_checkpoint *c,
                          int *fd, int *lock)
{
    char next_path[PATH_MAX];
    if (target) {
        if (t4_client_state_resolve(u->session_dir, target, next_path, sizeof(next_path)) < 0) goto rejected;
        if (same_file(path, next_path)) { message("[sessions] already in this conversation.\n"); return 1; }
    } else if (t4_client_state_new(u->session_dir, next_path, sizeof(next_path)) < 0) goto rejected;
    struct t4_checkpoint next = {0};
    next.answer = malloc(T4_MAX_RESPONSE);
    if (!next.answer) goto rejected;
    int next_lock = t4_checkpoint_lock(next_path);
    if (next_lock < 0 || t4_checkpoint_load(next_path, &next) < 0) {
        int saved = errno;
        if (next_lock >= 0) close(next_lock);
        free(next.answer); errno = saved; goto rejected;
    }
    if (archive_turn(path, c) < 0) {
        int saved = errno; close(next_lock); free(next.answer); errno = saved; goto rejected;
    }
    disconnect_session(fd); close(*lock);
    free(c->answer); *c = next; *lock = next_lock;
    strcpy(path, next_path);
    u->previous_request = u->previous_offset = 0; u->cancel_pending = 0;
    if (restore_display(path, c, 1) < 0) return -1;
    *fd = connect_session(options, path, c);
    if (*fd < 0) { conversation_error(fd); return 2; }
    if (t4_terminal_session(terminal, c->session, "local") < 0) return -1;
    if (c->request && !c->done) message("[sessions] pending turn paused; /retry continues its saved prefix.\n");
    return 2;
rejected:
    message("[sessions] switch failed: %s; use /sessions to select an available conversation%s.\n",
            strerror(errno), *fd < 0 && c->request && !c->done ? "; current turn is paused, /retry to continue" : "");
    return 1;
}

static int navigation(const unsigned char *line, struct controls *u, struct conversation *chat)
{
    const char *command = (const char *)line;
    if (strcmp(command, "/new") && strcmp(command, "/switch") && strncmp(command, "/switch ", 8)
        && strcmp(command, "/delete") && strncmp(command, "/delete ", 8)) return 0;
    if (!chat) { message("[sessions] conversation switching requires the interactive TUI.\n"); return 1; }
    if (!strcmp(command, "/switch")) {
        message("[sessions] /switch requires an ID or checkpoint path from /sessions.\n"); return 1;
    }
    if (!strcmp(command, "/new") || !strncmp(command, "/switch ", 8))
        return switch_session(command[1] == 'n' ? NULL : command + 8, chat->options, u,
                              chat->path, chat->checkpoint, chat->fd, chat->lock);

    char id[15], selected[PATH_MAX], trash[PATH_MAX];
    int implicit = !strcmp(command, "/delete");
    const char *target = command + 7;
    if (implicit) {
        char parent[PATH_MAX]; strcpy(parent, chat->path);
        char *slash = strrchr(parent, '/');
        if (slash) *slash = 0;
        const char *base = strrchr(parent, '/'); base = base ? base + 1 : parent;
        if (strlen(base) != 14) { message("[sessions] /delete requires a managed session ID from /sessions.\n"); return 1; }
        strcpy(id, base); target = id;
    } else ++target;
    /* Deletion accepts a managed ID, never a checkpoint path. Validate this
     * before a path alias could cause us to leave the current conversation. */
    if (strlen(target) != 14 || strncmp(target, "session-", 8) ||
        strspn(target + 8, "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz") != 6) {
        message("[sessions] /delete requires a managed session ID from /sessions.\n"); return 1;
    }
    if (t4_client_state_resolve(u->session_dir, target, selected, sizeof(selected)) < 0) {
        message("[sessions] delete failed: %s\n", strerror(errno)); return 1;
    }
    int current = same_file(chat->path, selected);
    if (implicit && !current) {
        message("[sessions] current checkpoint is outside this managed list; select an explicit ID with /sessions.\n");
        return 1;
    }
    if (current) {
        int changed = switch_session(NULL, chat->options, u, chat->path, chat->checkpoint, chat->fd, chat->lock);
        if (changed != 2) return changed;
    }
    if (t4_client_state_delete(u->session_dir, target, trash, sizeof(trash)) < 0)
        message("[sessions] delete failed: %s (active conversations cannot be deleted).\n", strerror(errno));
    else message("[sessions] deleted locally; recoverable at %s; server copy retained.\n", trash);
    return current ? 2 : 1;
}

/* Persistent TUI shell. Network failures end an attempt, never this UI. Batch
 * and --resume-only callers retain their exit-status contract in main(). */
static int interactive_chat(const struct connection_options *options, struct controls *u,
                            char path[PATH_MAX], struct t4_checkpoint *c, int *fd, int *lock)
{
    struct conversation chat = {options, path, c, fd, lock};
    archive_turn(path, c);
    if (restore_display(path, c, 0) < 0) return -1;
    int run = c->request && (!c->done || c->queued_length);
    while (!stopped && !u->quit) {
        if (run) {
            run = 0;
            /* CANCELLED may have committed just before a crash, with the
             * durable correction still queued. Finish that transaction once
             * before accepting any unrelated new question. */
            if (c->done && c->queued_length) {
                if (c->request >= 10000) { errno = EOVERFLOW; conversation_error(fd); continue; }
                if (archive_turn(path, c) < 0) { conversation_error(fd); continue; }
                c->prompt_length = c->queued_length;
                memcpy(c->prompt, c->queued_prompt, c->prompt_length + 1);
                ++c->request; c->offset = 0; c->done = 0;
                if (t4_checkpoint_save(path, c) < 0) { conversation_error(fd); continue; }
            }
            if (*fd < 0) {
                *fd = connect_session(options, path, c);
                if (*fd < 0) { conversation_error(fd); continue; }
            }
            int result = ask(*fd, path, c, (int)options->idle, u, &chat);
            if (stopped) break;
            if (result < 0) { conversation_error(fd); continue; }
            if (result == 2) {
                run = c->done && c->queued_length;
                continue; /* Navigation already committed the new conversation. */
            }
            else {
                int archived = archive_turn(path, c);
                if (u->quit) break;
                if (c->queued_length) {
                    if (archived < 0) { conversation_error(fd); continue; }
                    if (c->request >= 10000) { errno = EOVERFLOW; conversation_error(fd); continue; }
                    c->prompt_length = c->queued_length;
                    memcpy(c->prompt, c->queued_prompt, c->prompt_length + 1);
                    ++c->request; c->offset = 0; c->done = 0;
                    if (t4_checkpoint_save(path, c) < 0) { conversation_error(fd); continue; }
                    run = 1; continue;
                }
            }
        }
        unsigned char line[T4_MAX_PROMPT + 1];
        uint32_t length;
        int received = next_line(stdin, line, &length);
        if (received == -1 && errno == EINTR && !stopped) continue;
        if (!received) return 0;
        if (received == -2) { message("[input] question is too long or invalid; edit and retry.\n"); continue; }
        if (received < 0) return -1;
        if (stopped) break;
        if (display_control(line, length)) continue;
        const char *command = (const char *)line;
        if (!strcmp(command, "/quit")) return 0;
        if (!strcmp(command, "/help")) { help(); continue; }
        if (!strcmp(command, "/sessions")) { sessions(u->session_dir); continue; }
        int navigated = navigation(line, u, &chat);
        if (navigated < 0) return -1;
        if (navigated) {
            if (navigated == 2 && c->done && c->queued_length) run = 1;
            continue;
        }
        if (!strcmp(command, "/retry") || !strcmp(command, "/stop")) {
            if (c->request && (!c->done || c->queued_length)) {
                u->cancel_pending = !strcmp(command, "/stop");
                if (restore_display(path, c, 0) < 0) return -1;
                run = 1;
            } else if (!strcmp(command, "/retry")) {
                disconnect_session(fd); *fd = connect_session(options, path, c);
                if (*fd < 0) conversation_error(fd); else message("[recovery] connected; no pending turn.\n");
            } else message("[controls] no active turn to cancel.\n");
            continue;
        }
        if (length && line[0] == '/' && strncmp(command, "/steer ", 7)) {
            message("[controls] unknown command; /help lists available commands.\n"); continue;
        }
        if (c->done && c->queued_length) {
            message("[recovery] a saved correction is waiting; /retry submits it before another question.\n");
            continue;
        }
        if (c->request && !c->done) {
            const unsigned char *correction = line;
            uint32_t size = length;
            if (!strncmp(command, "/steer ", 7)) { correction += 7; size -= 7; }
            int queued = steer(path, c, correction, size);
            if (queued < 0) { conversation_error(fd); continue; }
            if (queued) { if (restore_display(path, c, 0) < 0) return -1; run = 1; }
            continue;
        }
        if (!strncmp(command, "/steer ", 7)) { message("[controls] no pending turn; enter a new question.\n"); continue; }
        if (c->request >= 10000) { message("[conversation] turn limit reached; /new starts another conversation.\n"); continue; }
        if (*fd < 0) {
            *fd = connect_session(options, path, c);
            if (*fd < 0) { conversation_error(fd); continue; }
        }
        if (archive_turn(path, c) < 0) continue;
        memcpy(c->prompt, line, length + 1); c->prompt_length = length;
        ++c->request; c->offset = 0; c->done = 0;
        if (t4_checkpoint_save(path, c) < 0) { conversation_error(fd); continue; }
        run = 1;
    }
    return stopped ? -1 : 0;
}

int main(int argc, char **argv)
{
    const char *host = "127.0.0.1", *path = NULL, *input_path = NULL;
    const char *session_dir = "state/clients";
    const char *server_state = NULL, *discovery_dir = "state/services";
    struct t4_endpoint endpoint;
    char automatic_path[PATH_MAX];
    uint32_t port = 0, idle = 10000;
    int explicit_port = 0, explicit_host = 0;
    int resume_only = 0, cancel_pending = 0, require_existing = 0, list = 0, plain = 0, dark = 0;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--help") && argc == 2) { usage(argv[0]); help(); return 0; }
        if (!strcmp(argv[i], "--list-sessions")) { list = 1; continue; }
        if (!strcmp(argv[i], "--plain")) { plain = 1; continue; }
        if (!strcmp(argv[i], "--verbose-status")) { verbose_status = 1; continue; }
        if (!strcmp(argv[i], "--resume-only")) { resume_only = 1; continue; }
        if (!strcmp(argv[i], "--cancel-pending")) { cancel_pending = 1; resume_only = 1; continue; }
        if (i + 1 == argc) goto usage;
        const char *key = argv[i], *value = argv[++i];
        if (!strcmp(key, "--host")) { host = value; explicit_host = 1; }
        else if (!strcmp(key, "--checkpoint") || !strcmp(key, "--resume")) {
            if (path) goto usage;
            path = value; require_existing = !strcmp(key, "--resume");
        }
        else if (!strcmp(key, "--session-dir")) session_dir = value;
        else if (!strcmp(key, "--server-state")) server_state = value;
        else if (!strcmp(key, "--discovery-dir")) discovery_dir = value;
        else if (!strcmp(key, "--theme")) {
            if (strcmp(value, "light") && strcmp(value, "dark")) goto usage;
            dark = !strcmp(value, "dark");
        }
        else if (!strcmp(key, "--input")) input_path = value;
        else if (!strcmp(key, "--port")) {
            if (t4_uint(value, 1, 65535, &port) < 0) goto usage;
            explicit_port = 1;
        }
        else if (!strcmp(key, "--idle-ms")) { if (t4_uint(value, 1500, 3600000, &idle) < 0) goto usage; }
        else goto usage;
    }
    if (list) {
        if (path || input_path || resume_only) goto usage;
        if (t4_client_state_list(session_dir) == 0) return 0;
        perror("list sessions"); return 1;
    }
    if ((explicit_host && !explicit_port) || (server_state && (explicit_host || explicit_port))) {
        message("[client] --host requires --port; --server-state selects a discovered local service.\n");
        goto usage;
    }
    if (resume_only && !path) {
        message("[client] recovery requires --resume FILE or --checkpoint FILE.\n");
        goto usage;
    }
    if (require_existing && access(path, F_OK) < 0) {
        message("[client] cannot resume %s: %s; select an existing checkpoint.\n", path, strerror(errno));
        return 1;
    }
    if (!path) {
        /* Do not create an empty client directory when no service is available. */
        if (!explicit_port) {
            if (discover(discovery_dir, server_state, NULL, &endpoint) < 0) return 1;
        }
        if (t4_client_state_new(session_dir, automatic_path, sizeof(automatic_path)) < 0) {
            perror("create independent session"); return 1;
        }
        path = automatic_path;
    }
    (void)setlocale(LC_CTYPE, "");
    struct controls u = {.interactive = !input_path && isatty(STDIN_FILENO), .session_dir = session_dir};
    interactive_signals = u.interactive;
    if (u.interactive) {
        /* Poll and stdio must observe the same tty bytes without read-ahead. */
        if (setvbuf(stdin, NULL, _IONBF, 0) != 0) { message("stdin buffering failed.\n"); return 1; }
    }
    struct sigaction action = {0};
    action.sa_handler = stop_signal;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) < 0 || sigaction(SIGTERM, &action, NULL) < 0 ||
        sigaction(SIGHUP, &action, NULL) < 0 || sigaction(SIGWINCH, &action, NULL) < 0 ||
        (u.interactive && sigaction(SIGTSTP, &action, NULL) < 0)) {
        perror("sigaction"); return 1;
    }
    struct t4_checkpoint c = {0};
    c.answer = malloc(T4_MAX_RESPONSE);
    if (!c.answer) { perror("malloc"); return 1; }
    int lock = -1, fd = -1, result = 1, failure_errno = 0, used_tui = 0;
    FILE *input = stdin;
    lock = t4_checkpoint_lock(path);
    if (lock < 0 || t4_checkpoint_load(path, &c) < 0) {
        t4_client_state_lock_error(path, errno); goto cleanup;
    }
    u.cancel_pending = cancel_pending && !c.done && c.request != 0;
    if (input_path && !(input = fopen(input_path, "r"))) { perror("input"); goto cleanup; }
    if (input_path && !resume_only) {
        /* 批量输入恢复沿用原文件，跳过已持久提交的整行，不重复提问。 */
        for (uint32_t line = 0; line < c.file_lines; ++line) {
            int byte, any = 0;
            while ((byte = fgetc(input)) != EOF && byte != '\n') any = 1;
            if (ferror(input) || (byte == EOF && !any)) { errno = EINVAL; goto failure; }
        }
    }
    const struct connection_options options = {host, server_state, discovery_dir, port, idle, explicit_port};
    fd = connect_session(&options, path, &c);
    if (fd < 0) goto failure;
    if (u.interactive && isatty(STDERR_FILENO) && !plain) {
        terminal = t4_terminal_open();
        if (!terminal) goto failure;
        used_tui = 1;
        if (t4_terminal_theme(terminal, dark) < 0 || t4_terminal_session(terminal, c.session, "local") < 0)
            goto failure;
    }
    if (terminal) {
        message("Checkpoint: %s\n/help for keys; /thinking toggles reasoning visibility.\n", path);
    } else {
        message("[client] session=%s checkpoint=%s\n", c.session, path);
        message("[client] reconnect with --resume %s (same server state; auto-discovery follows port changes).\n", path);
        if (u.interactive) help();
    }
    if (terminal && !resume_only && !input_path) {
        if (path != automatic_path) {
            if (strlen(path) >= sizeof(automatic_path)) { errno = ENAMETOOLONG; goto failure; }
            strcpy(automatic_path, path); path = automatic_path;
        }
        result = interactive_chat(&options, &u, automatic_path, &c, &fd, &lock);
        if (result < 0) goto failure;
        goto cleanup;
    }
    if (terminal && c.done && c.request) {
        /* A local checkpoint contains the latest saved turn. Restore its visible
         * answer for orientation; older server history remains model-owned. */
        if (t4_terminal_begin_turn(terminal, c.request, (const char *)c.prompt, c.offset) < 0 ||
            t4_terminal_answer(terminal, c.answer, c.offset) < 0 ||
            t4_terminal_end_turn(terminal, c.done == 2 ? "cancelled" : "completed", "restored from checkpoint") < 0)
            goto failure;
    }
    if (!c.done) {
        message("[client] resuming pending answer; durable prefix=%u bytes\n", c.offset);
        if (ask(fd, path, &c, (int)idle, &u, NULL) < 0) goto failure;
    }
    while (!stopped) {
        if (u.quit) { result = 0; break; }
        if (c.queued_length) {
            if (c.request >= 10000) { errno = EOVERFLOW; goto failure; }
            c.prompt_length = c.queued_length;
            memcpy(c.prompt, c.queued_prompt, c.prompt_length + 1);
            ++c.request; c.offset = 0; c.done = 0;
            /* Save new request first; checkpoint_save then removes the sidecar.
             * A crash in between is recognized by matching request + prompt. */
            if (t4_checkpoint_save(path, &c) < 0 || ask(fd, path, &c, (int)idle, &u, NULL) < 0)
                goto failure;
            continue;
        }
        if (resume_only) { result = 0; break; }
        if (terminal_signals() < 0) goto failure;
        if (u.interactive && !terminal) {
            message("question (/help for controls)> "); fflush(stderr);
        }
        int received = next_line(input, c.prompt, &c.prompt_length);
        if (received == -1 && u.interactive && errno == EINTR) {
            if (stopped) goto failure;
            clearerr(input); interrupt_requested = 0;
            if (terminal_signals() < 0) goto failure;
            continue;
        }
        if (!received) { result = 0; break; }
        if (input_path) ++c.file_lines;
        if (received == -1) goto failure;
        if (received == -2) {
            message("[client] invalid line skipped (max %u bytes, no NUL).\n", T4_MAX_PROMPT);
            continue;
        }
        int display = display_control(c.prompt, c.prompt_length);
        if (display < 0) goto failure;
        if (display) continue;
        if (c.prompt_length == 5 && !memcmp(c.prompt, "/quit", 5)) { result = 0; break; }
        if (u.interactive && c.prompt_length == 5 && !memcmp(c.prompt, "/help", 5)) { help(); continue; }
        if (u.interactive && ((c.prompt_length == 5 && !memcmp(c.prompt, "/stop", 5)) ||
            (c.prompt_length >= 6 && !memcmp(c.prompt, "/steer", 6)))) {
            message("[controls] no active turn; enter the corrected question.\n");
            continue;
        }
        if (c.request >= 10000) { errno = EOVERFLOW; goto failure; }
        ++c.request;
        c.offset = 0; c.done = 0;
        if (t4_checkpoint_save(path, &c) < 0 || ask(fd, path, &c, (int)idle, &u, NULL) < 0) goto failure;
    }
    goto cleanup;
failure:
    if (!errno) errno = EPROTO;
    failure_errno = errno;
    const char *detail = last_server_error[0] ? last_server_error :
        failure_errno == ETIMEDOUT && timeout_detail[0] ? timeout_detail : strerror(failure_errno);
    if (terminal) (void)t4_terminal_end_turn(terminal, "error", detail);
    else message("[client] stream/checkpoint: %s; reconnect with --resume %s\n", detail, path);
cleanup:
    if (terminal) { t4_terminal_close(terminal); terminal = NULL; }
    if (used_tui) {
        if (last_server_error[0]) message("[server error] %s\n", last_server_error);
        if (failure_errno) message("[client] %s; saved session: --resume %s\n",
                failure_errno == ETIMEDOUT && timeout_detail[0] ? timeout_detail : strerror(failure_errno), path);
        else message("[client] saved session: --resume %s\n", path);
    }
    if (fd >= 0) close(fd);
    if (lock >= 0) close(lock);
    if (input && input != stdin && fclose(input) == EOF) result = 1;
    free(c.answer);
    return stopped ? 128 + stopped : result;
usage:
    usage(argv[0]);
    return 2;
}
