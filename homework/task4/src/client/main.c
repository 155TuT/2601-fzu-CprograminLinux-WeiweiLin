#include "task4/checkpoint.h"
#include "task4/protocol.h"
#include "task4/text.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static volatile sig_atomic_t stopped;
static void stop_signal(int signo) { stopped = signo; }

static int send_frame(int fd, uint32_t type, uint32_t request, uint32_t offset,
                      const void *data, uint32_t length, int idle)
{
    struct t4_frame frame = {.type = type, .request_id = request,
                             .offset = offset, .length = length};
    if (length) memcpy(frame.data, data, length);
    return t4_send(fd, &frame, idle);
}

static int ask(int fd, const char *path, struct t4_checkpoint *c, int idle)
{
    if (send_frame(fd, T4_BEGIN, c->request, c->offset, NULL, 0, idle) < 0) return -1;
    for (uint32_t offset = 0; offset < c->prompt_length;) {
        uint32_t n = c->prompt_length - offset;
        if (n > 128) n = 128;
        if (send_frame(fd, T4_INPUT, c->request, offset, c->prompt + offset, n, idle) < 0) return -1;
        offset += n;
    }
    if (send_frame(fd, T4_COMMIT, c->request, c->prompt_length, NULL, 0, idle) < 0) return -1;
    fprintf(stderr, "[client] turn=%u resume=%u prompt=%u bytes\n", c->request, c->offset, c->prompt_length);
    struct t4_frame frame;
    for (;;) {
        if (t4_recv(fd, &frame, idle) != 1) return -1;
        if (frame.type == T4_ERROR) {
            fprintf(stderr, "[server error] %.*s\n", (int)frame.length, frame.data);
            errno = EPROTO; return -1;
        }
        if (frame.request_id != c->request || frame.offset != c->offset) {
            errno = EPROTO; return -1;
        }
        if (frame.type == T4_HEARTBEAT && !frame.length) continue;
        if (frame.type == T4_END && !frame.length) {
            c->done = 1;
            if (t4_checkpoint_save(path, c) < 0) return -1;
            if (fputc('\n', stdout) == EOF || fflush(stdout) == EOF) return -1;
            fprintf(stderr, "[client] completed turn=%u bytes=%u\n", c->request, c->offset);
            return 0;
        }
        if (frame.type != T4_DELTA || !t4_character_valid(frame.data, frame.length) ||
            frame.length > T4_MAX_RESPONSE - c->offset) { errno = EPROTO; return -1; }
        memcpy(c->answer + c->offset, frame.data, frame.length);
        c->offset += frame.length;
        /* 游标与完整正文先落盘；终端不是持久介质，恢复只打印尚未保存的后缀。 */
        if (t4_checkpoint_save(path, c) < 0 ||
            fwrite(frame.data, 1, frame.length, stdout) != frame.length || fflush(stdout) == EOF)
            return -1;
        if (stopped) return -1;
    }
}

/* 消费超长/NUL整行，尾部不会被错误地变成下一条问题。 */
static int read_prompt(FILE *input, struct t4_checkpoint *c)
{
    int byte, invalid = 0, any = 0;
    c->prompt_length = 0;
    while ((byte = fgetc(input)) != EOF && byte != '\n') {
        any = 1;
        if (!byte || c->prompt_length == T4_MAX_PROMPT) invalid = 1;
        else c->prompt[c->prompt_length++] = (unsigned char)byte;
    }
    c->prompt[c->prompt_length] = 0;
    if (ferror(input)) return -1;
    if (invalid) return -2;
    return byte == EOF && !any ? 0 : 1;
}

int main(int argc, char **argv)
{
    const char *host = "127.0.0.1", *path = "state/client.ckpt", *input_path = NULL;
    uint32_t port = 3340, idle = 10000;
    int resume_only = 0;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--resume-only")) { resume_only = 1; continue; }
        if (i + 1 == argc) goto usage;
        const char *key = argv[i], *value = argv[++i];
        if (!strcmp(key, "--host")) host = value;
        else if (!strcmp(key, "--checkpoint")) path = value;
        else if (!strcmp(key, "--input")) input_path = value;
        else if (!strcmp(key, "--port")) { if (t4_uint(value, 1, 65535, &port) < 0) goto usage; }
        else if (!strcmp(key, "--idle-ms")) { if (t4_uint(value, 1500, 3600000, &idle) < 0) goto usage; }
        else goto usage;
    }
    struct sigaction action = {0};
    action.sa_handler = stop_signal;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) < 0 || sigaction(SIGTERM, &action, NULL) < 0) {
        perror("sigaction"); return 1;
    }
    struct t4_checkpoint c = {0};
    c.answer = malloc(T4_MAX_RESPONSE);
    if (!c.answer) { perror("malloc"); return 1; }
    int lock = -1, fd = -1, result = 1;
    FILE *input = stdin;
    lock = t4_checkpoint_lock(path);
    if (lock < 0 || t4_checkpoint_load(path, &c) < 0) { perror("checkpoint open/lock"); goto cleanup; }
    if (input_path && !(input = fopen(input_path, "r"))) { perror("input"); goto cleanup; }
    if (input_path && !resume_only) {
        /* 批量输入恢复沿用原文件，跳过已持久提交的整行，不重复提问。 */
        for (uint32_t line = 0; line < c.file_lines; ++line) {
            int byte, any = 0;
            while ((byte = fgetc(input)) != EOF && byte != '\n') any = 1;
            if (ferror(input) || (byte == EOF && !any)) { errno = EINVAL; goto failure; }
        }
    }
    fd = t4_connect(host, (uint16_t)port, (int)idle);
    if (fd < 0) { perror("connect (start server first)"); goto cleanup; }
    if (send_frame(fd, T4_HELLO, 0, 0, c.session, (uint32_t)strlen(c.session), (int)idle) < 0)
        goto failure;
    struct t4_frame welcome;
    if (t4_recv(fd, &welcome, (int)idle) != 1) goto failure;
    if (welcome.type == T4_ERROR) {
        fprintf(stderr, "[server error] %.*s\n", (int)welcome.length, welcome.data);
        goto cleanup;
    }
    if (welcome.type != T4_WELCOME || welcome.request_id || welcome.offset || welcome.length != 32)
        goto failure;
    for (uint32_t i = 0; i < 32; ++i) {
        unsigned char b = welcome.data[i];
        if (!((b >= '0' && b <= '9') || (b >= 'a' && b <= 'f'))) goto failure;
    }
    if (c.session[0] && memcmp(c.session, welcome.data, 32)) goto failure;
    memcpy(c.session, welcome.data, 32); c.session[32] = 0;
    if (t4_checkpoint_save(path, &c) < 0) goto failure;
    fprintf(stderr, "[client] session=%s checkpoint=%s\n", c.session, path);
    if (!c.done) {
        fprintf(stderr, "[client] resuming pending answer; durable prefix=%u bytes\n", c.offset);
        if (ask(fd, path, &c, (int)idle) < 0) goto failure;
    }
    if (resume_only) { result = 0; goto cleanup; }
    while (!stopped) {
        if (input == stdin && isatty(STDIN_FILENO)) {
            fprintf(stderr, "question (/quit to exit)> "); fflush(stderr);
        }
        int received = read_prompt(input, &c);
        if (!received) { result = 0; break; }
        if (input_path) ++c.file_lines;
        if (received == -1) goto failure;
        if (received == -2) {
            fprintf(stderr, "[client] invalid line skipped (max %u bytes, no NUL).\n", T4_MAX_PROMPT);
            continue;
        }
        if (c.prompt_length == 5 && !memcmp(c.prompt, "/quit", 5)) { result = 0; break; }
        if (c.request >= 10000) { errno = EOVERFLOW; goto failure; }
        ++c.request;
        c.offset = 0; c.done = 0;
        if (t4_checkpoint_save(path, &c) < 0 || ask(fd, path, &c, (int)idle) < 0) goto failure;
    }
    goto cleanup;
failure:
    if (!errno) errno = EPROTO;
    perror("stream/checkpoint (rerun with the same checkpoint to resume)");
cleanup:
    if (fd >= 0) close(fd);
    if (lock >= 0) close(lock);
    if (input && input != stdin && fclose(input) == EOF) result = 1;
    free(c.answer);
    return stopped ? 128 + stopped : result;
usage:
    fprintf(stderr, "Usage: %s [--host HOST] [--port 1..65535] [--checkpoint FILE]\n"
                    "  [--input FILE] [--resume-only] [--idle-ms 1500..3600000]\n", argv[0]);
    return 2;
}
