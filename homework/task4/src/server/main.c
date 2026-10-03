#include "task4/backend.h"
#include "task4/protocol.h"
#include "task4/store.h"
#include "task4/text.h"

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

static volatile sig_atomic_t stopped;
static void stop_signal(int signo) { stopped = signo; }

struct configuration {
    const char *bind, *state, *corpus, *generator;
    uint32_t port, rate, idle, capacity;
    const struct t4_backend *backend;
};

static int signals(void)
{
    struct sigaction action = {0};
    action.sa_handler = stop_signal;
    sigemptyset(&action.sa_mask);
    return sigaction(SIGTERM, &action, NULL) < 0 ||
           sigaction(SIGINT, &action, NULL) < 0 ? -1 : 0;
}

static int send_event(int fd, uint32_t type, uint32_t request, uint32_t offset,
                      const void *data, uint32_t length, int idle)
{
    struct t4_frame frame = {.type = type, .request_id = request,
                             .offset = offset, .length = length};
    if (length) memcpy(frame.data, data, length);
    return t4_send(fd, &frame, idle);
}

static void send_error(int fd, const char *message, int idle)
{
    (void)send_event(fd, T4_ERROR, 0, 0, message, (uint32_t)strlen(message), idle);
}

static int replay(int client, int response, uint32_t request, uint32_t from,
                  uint32_t until, int idle)
{
    if (lseek(response, from, SEEK_SET) < 0) return -1;
    while (from < until && !stopped) {
        unsigned char character[4];
        if (read(response, character, 1) != 1) return -1;
        unsigned width = t4_character_width(character[0]);
        if (!width || width > until - from) { errno = EPROTO; return -1; }
        if (width > 1 && read(response, character + 1, width - 1) != (ssize_t)(width - 1)) return -1;
        if (!t4_character_valid(character, width)) { errno = EPROTO; return -1; }
        if (send_event(client, T4_DELTA, request, from, character, width, idle) < 0) return -1;
        from += width;
    }
    return stopped ? -1 : 0;
}

static int generate(int client, struct t4_session *session, const char *directory,
                    uint32_t request, uint32_t offset, const struct configuration *cfg)
{
    char path[PATH_MAX];
    struct t4_generation generation = {.pid = -1, .fd = -1};
    int response = -1, result = -1, status;
    if (t4_path(path, directory, "response.txt") < 0) return -1;
    response = open(path, O_WRONLY | O_APPEND | O_NOFOLLOW | O_CLOEXEC);
    if (response < 0 || cfg->backend->start(cfg->generator, directory, session->context,
                                              offset, cfg->rate, &generation) < 0) goto cleanup;
    printf("[worker %ld] session=%s turn=%u generator=%ld resume=%u\n",
           (long)getpid(), session->token, request, (long)generation.pid, offset);
    unsigned char character[4];
    unsigned have = 0, width = 0;
    for (;;) {
        struct pollfd descriptors[2] = {{generation.fd, POLLIN, 0}, {client, POLLIN, 0}};
        int ready = poll(descriptors, 2, 1000);
        if (stopped) goto cleanup;
        if (ready < 0) { if (errno == EINTR) continue; goto cleanup; }
        if (!ready) {
            /* 心跳只表示连接存活，不能推进恢复游标或伪造生成进度。 */
            if (send_event(client, T4_HEARTBEAT, request, offset, NULL, 0, (int)cfg->idle) < 0)
                goto cleanup;
            continue;
        }
        if (descriptors[1].revents) { errno = ECONNRESET; goto cleanup; }
        if (descriptors[0].revents & (POLLIN | POLLHUP)) {
            unsigned char bytes[4096];
            ssize_t n = read(generation.fd, bytes, sizeof(bytes));
            if (n < 0 && (errno == EAGAIN || errno == EINTR)) continue;
            if (n < 0) goto cleanup;
            if (!n) break;
            for (ssize_t i = 0; i < n; ++i) {
                if (!have) {
                    width = t4_character_width(bytes[i]);
                    if (!width) { errno = EILSEQ; goto cleanup; }
                }
                character[have++] = bytes[i];
                if (have != width) continue;
                if (!t4_character_valid(character, width) || offset > T4_MAX_RESPONSE - width) {
                    errno = EILSEQ; goto cleanup;
                }
                /* 先持久化完整码点再发送：断连后重放不会丢掉已经确认的字节。 */
                if (t4_write_all(response, character, width) < 0 || fsync(response) < 0 ||
                    send_event(client, T4_DELTA, request, offset, character, width, (int)cfg->idle) < 0)
                    goto cleanup;
                offset += width;
                have = 0;
            }
        } else if (descriptors[0].revents) { errno = EIO; goto cleanup; }
    }
    close(generation.fd); generation.fd = -1;
    pid_t waited;
    do { waited = waitpid(generation.pid, &status, 0); } while (waited < 0 && errno == EINTR && !stopped);
    if (waited < 0) goto cleanup;
    generation.pid = -1;
    if (have || !WIFEXITED(status) || WEXITSTATUS(status) != 0) { errno = EIO; goto cleanup; }
    if (t4_turn_done(directory) < 0 || t4_event(session, "completed", request, offset) < 0 ||
        send_event(client, T4_END, request, offset, NULL, 0, (int)cfg->idle) < 0) goto cleanup;
    result = 0;
cleanup:
    {
        int saved = errno;
        t4_generation_stop(&generation);
        if (response >= 0) close(response);
        errno = saved;
        return result;
    }
}

static int worker(int client, const struct configuration *cfg)
{
    struct t4_session session = {.lock_fd = -1, .log_fd = -1};
    struct t4_frame frame;
    int result = 1, idle = (int)cfg->idle;
    if (t4_recv(client, &frame, idle) != 1 || frame.type != T4_HELLO ||
        frame.request_id || frame.offset || (frame.length != 0 && frame.length != 32) ||
        memchr(frame.data, 0, frame.length)) goto protocol_error;
    frame.data[frame.length] = 0;
    if (t4_session_open(cfg->state, (char *)frame.data, &session) < 0) {
        send_error(client, "session unavailable (unknown token, busy lock or storage error)", idle);
        goto cleanup;
    }
    if (send_event(client, T4_WELCOME, 0, 0, session.token, 32, idle) < 0) goto cleanup;
    printf("[worker %ld] connected session=%s\n", (long)getpid(), session.token);
    while (!stopped) {
        int received = t4_recv(client, &frame, idle);
        if (!received) { result = 0; break; }
        if (received != 1) goto cleanup;
        if (frame.type != T4_BEGIN || !frame.request_id || frame.length) goto protocol_error;
        uint32_t request = frame.request_id, resume = frame.offset, length = 0;
        unsigned char prompt[T4_MAX_PROMPT + 1];
        /* 输入可拆为多个 INPUT 帧；只在 COMMIT 后开始生成。 */
        for (;;) {
            if (t4_recv(client, &frame, idle) != 1 || frame.request_id != request) goto protocol_error;
            if (frame.type == T4_COMMIT) {
                if (frame.length || frame.offset != length) goto protocol_error;
                break;
            }
            if (frame.type != T4_INPUT || !frame.length || frame.offset != length ||
                frame.length > T4_MAX_PROMPT - length || memchr(frame.data, 0, frame.length))
                goto protocol_error;
            memcpy(prompt + length, frame.data, frame.length);
            length += frame.length;
        }
        prompt[length] = 0;
        char directory[PATH_MAX], response_path[PATH_MAX];
        uint32_t cached;
        int done;
        if (t4_turn_open(&session, request, prompt, length, directory, &cached, &done,
                         cfg->corpus, cfg->backend) < 0 ||
            resume > cached || t4_path(response_path, directory, "response.txt") < 0) {
            send_error(client, "request rejected (sequence, changed prompt, offset or storage)", idle);
            goto cleanup;
        }
        int response = open(response_path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        if (response < 0) goto cleanup;
        int replayed = replay(client, response, request, resume, cached, idle);
        close(response);
        if (replayed < 0) goto cleanup;
        if (done) {
            if (send_event(client, T4_END, request, cached, NULL, 0, idle) < 0) goto cleanup;
        } else if (generate(client, &session, directory, request, cached, cfg) < 0) {
            if (!stopped) send_error(client, "generation interrupted; checkpoint retained; see generator.log", idle);
            goto cleanup;
        }
    }
    goto cleanup;
protocol_error:
    send_error(client, "invalid protocol frame or input", idle);
cleanup:
    if (session.log_fd >= 0) (void)t4_event(&session, "disconnected", 0, 0);
    t4_session_close(&session);
    close(client);
    return stopped ? 128 + stopped : result;
}

static int configuration(int argc, char **argv, struct configuration *cfg)
{
    *cfg = (struct configuration){"127.0.0.1", "state/server", "data/corpus",
                                  "bin/text_generator", 3340, 30, 600000, 32, &t4_corpus_backend};
    for (int i = 1; i < argc; i += 2) {
        if (i + 1 == argc) return -1;
        const char *key = argv[i], *value = argv[i + 1];
        if (!strcmp(key, "--bind")) cfg->bind = value;
        else if (!strcmp(key, "--state")) cfg->state = value;
        else if (!strcmp(key, "--corpus")) cfg->corpus = value;
        else if (!strcmp(key, "--generator")) cfg->generator = value;
        else if (!strcmp(key, "--port")) { if (t4_uint(value, 0, 65535, &cfg->port) < 0) return -1; }
        else if (!strcmp(key, "--rate")) { if (t4_uint(value, 0, 10000, &cfg->rate) < 0) return -1; }
        else if (!strcmp(key, "--idle-ms")) { if (t4_uint(value, 100, 3600000, &cfg->idle) < 0) return -1; }
        else if (!strcmp(key, "--max-clients")) { if (t4_uint(value, 1, 64, &cfg->capacity) < 0) return -1; }
        else return -1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    struct configuration cfg;
    if (configuration(argc, argv, &cfg) < 0) {
        fprintf(stderr, "Usage: %s [--bind IPv4] [--port 0..65535] [--state DIR]\n"
                        "  [--corpus DIR] [--generator PATH] [--rate 0..10000]\n"
                        "  [--idle-ms 100..3600000] [--max-clients 1..64]\n", argv[0]);
        return 2;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (signals() < 0) { perror("sigaction"); return 1; }
    uint16_t port;
    int listener = t4_listen(cfg.bind, (uint16_t)cfg.port, &port);
    if (listener < 0) { perror("listen"); return 1; }
    pid_t children[64] = {0};
    printf("[server] pid=%ld listening on %s:%u backend=%s rate=%u chars/s\n",
           (long)getpid(), cfg.bind, port, cfg.backend->name, cfg.rate);
    while (!stopped) {
        for (uint32_t i = 0; i < cfg.capacity; ++i) {
            if (children[i] && waitpid(children[i], NULL, WNOHANG) == children[i]) children[i] = 0;
        }
        struct pollfd descriptor = {listener, POLLIN, 0};
        int ready = poll(&descriptor, 1, 500);
        if (ready < 0) { if (errno == EINTR) continue; perror("poll listener"); break; }
        if (!ready) continue;
        int client = accept(listener, NULL, NULL);
        if (client < 0) { if (errno == EAGAIN || errno == EINTR) continue; perror("accept"); break; }
        int flags = fcntl(client, F_GETFL), enabled = 1;
        if (flags < 0 || fcntl(client, F_SETFL, flags | O_NONBLOCK) < 0 ||
            fcntl(client, F_SETFD, FD_CLOEXEC) < 0 ||
            setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled)) < 0) {
            perror("configure accepted socket"); close(client); continue;
        }
        uint32_t slot = 0;
        /* poll 等待期间旧 worker 也可能结束；接受新连接后再次回收。 */
        for (uint32_t i = 0; i < cfg.capacity; ++i)
            if (children[i] && waitpid(children[i], NULL, WNOHANG) == children[i]) children[i] = 0;
        while (slot < cfg.capacity && children[slot]) ++slot;
        if (slot == cfg.capacity) { send_error(client, "server at connection capacity", 100); close(client); continue; }
        pid_t parent = getpid();
        pid_t pid = fork();
        if (pid == 0) {
            if (prctl(PR_SET_PDEATHSIG, SIGTERM) < 0 || getppid() != parent) _exit(1);
            close(listener);
            int result = worker(client, &cfg);
            _exit(result);
        }
        close(client);
        if (pid < 0) { perror("fork worker"); continue; }
        children[slot] = pid;
    }
    close(listener);
    /* 终止时回收所有工作进程；工作进程负责结束并回收自己的生成器。 */
    for (uint32_t i = 0; i < cfg.capacity; ++i) if (children[i]) kill(children[i], SIGTERM);
    for (uint32_t i = 0; i < cfg.capacity; ++i) if (children[i]) {
        while (waitpid(children[i], NULL, 0) < 0 && errno == EINTR) {}
    }
    printf("[server] all workers reaped; state retained.\n");
    return stopped ? 0 : 1;
}
