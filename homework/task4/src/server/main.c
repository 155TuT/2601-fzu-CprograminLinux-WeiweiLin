#include "task4/backend.h"
#include "task4/discovery.h"
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
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static volatile sig_atomic_t stopped;
static void stop_signal(int signo) { stopped = signo; }

struct configuration {
    const char *bind, *state, *adapter, *model_config, *discovery;
    char instance[33];
    uint32_t port, idle, capacity;
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

static int valid_text(const unsigned char *data, uint32_t length)
{
    for (uint32_t i = 0; i < length;) {
        unsigned width = t4_character_width(data[i]);
        if (!width || width > length - i || !t4_character_valid(data + i, width)) return 0;
        i += width;
    }
    return 1;
}

/* 0 completed, 1 cancelled, -1 failure, -2 error already sent by model adapter. */
static int generate(int client, struct t4_session *session, const char *directory,
                    uint32_t request, uint32_t offset, const struct configuration *cfg,
                    uint32_t *finished_offset)
{
    char path[PATH_MAX];
    struct t4_generation generation = {.pid = -1, .fd = -1};
    int response = -1, result = -1, status, exit_fd = -1;
    if (t4_path(path, directory, "response.txt") < 0) return -1;
    response = open(path, O_WRONLY | O_APPEND | O_NOFOLLOW | O_CLOEXEC);
    if (response < 0 || cfg->backend->start(cfg->adapter, directory, session->context, cfg->model_config,
                                              offset, &generation) < 0) goto cleanup;
    /* Linux pidfd is pollable even if a child closes stdout before exiting.
     * A blocking waitpid at output EOF would make CANCEL stop responding. */
    exit_fd = (int)syscall(SYS_pidfd_open, generation.pid, 0);
    if (exit_fd < 0) goto cleanup;
    printf("[worker %ld] session=%s turn=%u generator=%ld resume=%u\n",
           (long)getpid(), session->token, request, (long)generation.pid, offset);
    int model_end = 0, output_closed = 0;
    for (;;) {
        if (output_closed) {
            pid_t waited = waitpid(generation.pid, &status, WNOHANG);
            if (waited == generation.pid) { generation.pid = -1; break; }
            if (waited < 0 && errno != EINTR) goto cleanup;
        }
        struct pollfd descriptors[3] = {
            {generation.fd, POLLIN, 0}, {client, POLLIN, 0}, {exit_fd, POLLIN, 0}};
        int ready = poll(descriptors, 3, 1000);
        if (stopped) goto cleanup;
        if (ready < 0) { if (errno == EINTR) continue; goto cleanup; }
        if (!ready) {
            /* 心跳只表示连接存活，不能推进恢复游标或伪造生成进度。 */
            if (send_event(client, T4_HEARTBEAT, request, offset, NULL, 0, (int)cfg->idle) < 0)
                goto cleanup;
            continue;
        }
        if (descriptors[1].revents) {
            struct t4_frame control;
            if (t4_recv(client, &control, (int)cfg->idle) != 1) goto cleanup;
            if (control.type != T4_CANCEL || control.request_id != request || control.length) {
                errno = EPROTO; goto cleanup;
            }
            /* Stop inference before acknowledging cancellation or accepting another turn. */
            t4_generation_stop(&generation);
            if (t4_turn_cancel(directory) < 0 || t4_event(session, "cancelled", request, offset) < 0 ||
                send_event(client, T4_CANCELLED, request, offset, NULL, 0, (int)cfg->idle) < 0)
                goto cleanup;
            result = 1;
            goto cleanup;
        }
        if (descriptors[0].revents & (POLLIN | POLLHUP)) {
            struct t4_frame event;
            int got = t4_recv(generation.fd, &event, 1500);
            if (!got) {
                close(generation.fd); generation.fd = -1;
                output_closed = 1;
                continue;
            }
            if (got < 0 || event.request_id || model_end) { errno = EPROTO; goto cleanup; }
            if (event.type == T4_END && !event.length && event.offset == offset) {
                model_end = 1;
                continue; /* Require a successful child exit as well as its END event. */
            }
            if (event.type == T4_ERROR) {
                if (valid_text(event.data, event.length))
                    send_error(client, (char *)event.data, (int)cfg->idle);
                result = -2;
                goto cleanup;
            }
            if ((event.type == T4_THINK || event.type == T4_STATUS) &&
                event.length && valid_text(event.data, event.length)) {
                if (send_event(client, event.type, request,
                        event.type == T4_THINK ? event.offset : offset,
                        event.data, event.length, (int)cfg->idle) < 0) goto cleanup;
                continue;
            }
            if (event.type != T4_DELTA || event.offset != offset || event.length < 1 ||
                event.length > 4 || t4_character_width(event.data[0]) != event.length ||
                !t4_character_valid(event.data, event.length) ||
                offset > T4_MAX_RESPONSE - event.length) { errno = EPROTO; goto cleanup; }
            if (t4_write_all(response, event.data, event.length) < 0 || fsync(response) < 0 ||
                send_event(client, T4_DELTA, request, offset, event.data, event.length,
                           (int)cfg->idle) < 0) goto cleanup;
            offset += event.length;
            continue;
        } else if (descriptors[0].revents) { errno = EIO; goto cleanup; }
        else if (!output_closed && descriptors[2].revents) {
            /* A descendant retaining stdout must not outlive its generator. */
            errno = EIO; goto cleanup;
        }
    }
    if (!model_end ||
        !WIFEXITED(status) || WEXITSTATUS(status) != 0) { errno = EIO; goto cleanup; }
    if (t4_turn_done(directory) < 0 || t4_event(session, "completed", request, offset) < 0 ||
        send_event(client, T4_END, request, offset, NULL, 0, (int)cfg->idle) < 0) goto cleanup;
    result = 0;
cleanup:
    {
        int saved = errno;
        *finished_offset = offset;
        t4_generation_stop(&generation);
        if (exit_fd >= 0) close(exit_fd);
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
    uint32_t last_request = 0, last_offset = 0, last_terminal = 0;
    if (t4_recv(client, &frame, idle) != 1) goto protocol_error;
    if (frame.type == T4_PROBE) {
        if (frame.request_id || frame.offset || frame.length ||
            send_event(client, T4_SERVICE, 0, 0, cfg->instance, 32, idle) < 0) goto protocol_error;
        /* Discovery does not create a session or acquire its conversation lock. */
        if (t4_recv(client, &frame, idle) != 1) goto cleanup;
    }
    if (frame.type != T4_HELLO ||
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
        /* A CANCEL can cross END in flight. Return the existing terminal state. */
        if (frame.type == T4_CANCEL && last_terminal && frame.request_id == last_request &&
            !frame.length) {
            if (send_event(client, last_terminal, last_request, last_offset, NULL, 0, idle) < 0)
                goto cleanup;
            continue;
        }
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
                         cfg->model_config, cfg->backend) < 0 ||
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
            last_terminal = done == 2 ? T4_CANCELLED : T4_END;
            if (send_event(client, last_terminal, request, cached, NULL, 0, idle) < 0) goto cleanup;
        } else {
            int generated = generate(client, &session, directory, request, cached, cfg, &cached);
            if (generated < 0) {
                if (!stopped && generated != -2)
                    send_error(client, "generation interrupted; checkpoint retained; see generator.log", idle);
                goto cleanup;
            }
            last_terminal = generated == 1 ? T4_CANCELLED : T4_END;
        }
        last_request = request; last_offset = cached;
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
    *cfg = (struct configuration){.bind = "127.0.0.1", .state = "state/server",
        .adapter = "src/model/adapter.py", .model_config = "config/model.json", .discovery = "state/services", .port = 0,
        .idle = 600000, .capacity = 32, .backend = &t4_model_backend};
    for (int i = 1; i < argc; i += 2) {
        if (i + 1 == argc) return -1;
        const char *key = argv[i], *value = argv[i + 1];
        if (!strcmp(key, "--bind")) cfg->bind = value;
        else if (!strcmp(key, "--state")) cfg->state = value;
        else if (!strcmp(key, "--adapter")) cfg->adapter = value;
        else if (!strcmp(key, "--model-config")) cfg->model_config = value;
        else if (!strcmp(key, "--discovery-dir")) cfg->discovery = value;
        else if (!strcmp(key, "--backend")) {
            if (strcmp(value, "llama")) return -1;
        }
        else if (!strcmp(key, "--port")) { if (t4_uint(value, 0, 65535, &cfg->port) < 0) return -1; }
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
                        "  [--adapter PATH]\n"
                        "  [--backend llama] [--model-config config/model.json]\n"
                        "  [--discovery-dir DIR] [--idle-ms 100..3600000] [--max-clients 1..64]\n"
                        "Default port: auto (kernel-assigned); clients discover via state/services.\n", argv[0]);
        return 2;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (signals() < 0) { perror("sigaction"); return 1; }
    struct t4_service service;
    struct t4_endpoint existing;
    int acquired = t4_service_acquire(cfg.state, &service, &existing);
    if (acquired < 0) { perror("server state/lease (another server may still be starting)"); return 1; }
    if (acquired == 1) {
        printf("[server] already running on %s:%u for state=%s; reuse with stream_client --server-state '%s'\n",
               existing.host, existing.port, existing.state, existing.state);
        return 0;
    }
    memcpy(cfg.instance, service.instance, sizeof(cfg.instance));
    cfg.state = service.state;
    uint16_t port;
    int listener = t4_listen(cfg.bind, (uint16_t)cfg.port, &port);
    if (listener < 0) {
        perror("listen (omit --port, or use --port 0, for automatic allocation)");
        t4_service_close(&service); return 1;
    }
    if (t4_service_publish(&service, cfg.discovery, cfg.bind, port) < 0) {
        perror("publish service endpoint"); close(listener); t4_service_close(&service); return 1;
    }
    pid_t children[64] = {0};
    printf("[server] pid=%ld listening on %s:%u backend=%s",
           (long)getpid(), cfg.bind, port, cfg.backend->name);
    printf(" model-config=%s (native token speed)\n", cfg.model_config);
    printf("[server] discovery=%s state=%s; clients may omit --port\n", cfg.discovery, cfg.state);
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
            t4_service_child_close(&service);
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
    t4_service_close(&service);
    return stopped ? 0 : 1;
}
