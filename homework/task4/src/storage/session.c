#include "task4/store.h"
#include "task4/backend.h"
#include "task4/protocol.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

static int make_directory(const char *path)
{
    struct stat st;
    if (mkdir(path, 0700) < 0 && errno != EEXIST) return -1;
    if (lstat(path, &st) < 0) return -1;
    if (!S_ISDIR(st.st_mode)) { errno = ENOTDIR; return -1; }
    return 0;
}

static int token_valid(const char *token)
{
    if (strlen(token) != 32) return 0;
    for (size_t i = 0; i < 32; ++i)
        if (!((token[i] >= '0' && token[i] <= '9') ||
              (token[i] >= 'a' && token[i] <= 'f'))) return 0;
    return 1;
}

int t4_event(struct t4_session *s, const char *event, uint32_t request, uint32_t offset)
{
    char record[256];
    int n = snprintf(record, sizeof(record), "pid=%ld event=%s request=%u offset=%u\n",
                     (long)getpid(), event, request, offset);
    if (n < 0 || (size_t)n >= sizeof(record)) { errno = EOVERFLOW; return -1; }
    return t4_write_all(s->log_fd, record, (size_t)n) < 0 ? -1 : fsync(s->log_fd);
}

int t4_session_open(const char *root, const char *token, struct t4_session *s)
{
    char path[PATH_MAX];
    memset(s, 0, sizeof(*s));
    s->lock_fd = s->log_fd = -1;
    if (make_directory(root) < 0) return -1;
    if (*token) {
        if (!token_valid(token)) { errno = EINVAL; return -1; }
        memcpy(s->token, token, 33);
        if (t4_path(s->directory, root, token) < 0) return -1;
        struct stat st;
        if (lstat(s->directory, &st) < 0) return -1;
        if (!S_ISDIR(st.st_mode)) { errno = ENOTDIR; return -1; }
    } else {
        unsigned char random[16];
        int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
        if (fd < 0) return -1;
        size_t have = 0;
        while (have < sizeof(random)) {
            ssize_t n = read(fd, random + have, sizeof(random) - have);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) { close(fd); return -1; }
            have += (size_t)n;
        }
        close(fd);
        for (size_t i = 0; i < sizeof(random); ++i)
            snprintf(s->token + 2 * i, 3, "%02x", random[i]);
        if (t4_path(s->directory, root, s->token) < 0 ||
            mkdir(s->directory, 0700) < 0 || t4_directory_sync(root) < 0) return -1;
    }
    if (t4_path(path, s->directory, "session.lock") < 0) goto fail;
    s->lock_fd = open(path, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (s->lock_fd < 0 || flock(s->lock_fd, LOCK_EX | LOCK_NB) < 0) goto fail;
    if (t4_path(path, s->directory, "events.log") < 0) goto fail;
    s->log_fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (s->log_fd < 0 || t4_path(s->context, s->directory, "context.txt") < 0 ||
        t4_event(s, "connected", 0, 0) < 0) goto fail;
    return 0;
fail:
    {
        int saved = errno;
        t4_session_close(s);
        errno = saved;
        return -1;
    }
}

void t4_session_close(struct t4_session *s)
{
    if (s->log_fd >= 0) close(s->log_fd);
    if (s->lock_fd >= 0) close(s->lock_fd);
    s->log_fd = s->lock_fd = -1;
}

static int turn_path(char path[PATH_MAX], const char *session, uint32_t id)
{
    char name[32];
    snprintf(name, sizeof(name), "turn-%08u", id);
    return t4_path(path, session, name);
}

static int copy_file(int output, const char *path)
{
    unsigned char buffer[4096];
    int input = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (input < 0) return -1;
    int result = 0;
    for (;;) {
        ssize_t n = read(input, buffer, sizeof(buffer));
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 || (n > 0 && t4_write_all(output, buffer, (size_t)n) < 0)) {
            result = -1; break;
        }
        if (!n) break;
    }
    close(input);
    return result;
}

/* 历史从已完成轮次重建，避免断电发生于追加历史时造成重复对话。 */
static int context_build(struct t4_session *s, uint32_t request)
{
    int fd = open(s->context, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return -1;
    int result = -1;
    for (uint32_t i = 1; i < request; ++i) {
        char directory[PATH_MAX], path[PATH_MAX];
        if (turn_path(directory, s->directory, i) < 0 ||
            t4_path(path, directory, "done") < 0 || access(path, F_OK) < 0) goto cleanup;
        if (t4_write_all(fd, "user: ", 6) < 0 ||
            t4_path(path, directory, "prompt.txt") < 0 || copy_file(fd, path) < 0 ||
            t4_write_all(fd, "\nassistant: ", 12) < 0 ||
            t4_path(path, directory, "response.txt") < 0 || copy_file(fd, path) < 0 ||
            t4_write_all(fd, "\n", 1) < 0) goto cleanup;
    }
    result = fsync(fd);
cleanup:
    close(fd);
    return result;
}

static void staging_remove(const char *directory)
{
    const char *names[] = {"prompt.txt", "source.txt", "response.txt"};
    char path[PATH_MAX];
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
        if (t4_path(path, directory, names[i]) == 0) unlink(path);
    rmdir(directory);
}

int t4_turn_open(struct t4_session *s, uint32_t request,
                 const unsigned char *prompt, uint32_t length,
                 char directory[PATH_MAX], uint32_t *cached, int *done,
                 const char *corpus, const struct t4_backend *backend)
{
    char path[PATH_MAX];
    struct stat st;
    if (!request || request > 10000 || length > T4_MAX_PROMPT ||
        memchr(prompt, 0, length)) { errno = EINVAL; return -1; }
    if (context_build(s, request) < 0 || turn_path(directory, s->directory, request) < 0)
        return -1;
    if (lstat(directory, &st) < 0) {
        if (errno != ENOENT) return -1;
        char staging[PATH_MAX];
        if (t4_path(staging, s->directory, ".pending-XXXXXX") < 0 || !mkdtemp(staging)) return -1;
        int fd = -1, result = -1;
        if (t4_path(path, staging, "prompt.txt") < 0) goto staged;
        fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd < 0 || t4_write_all(fd, prompt, length) < 0 || fsync(fd) < 0) goto staged;
        close(fd); fd = -1;
        if (backend->prepare(corpus, staging) < 0 ||
            t4_path(path, staging, "response.txt") < 0) goto staged;
        fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd < 0 || fsync(fd) < 0) goto staged;
        close(fd); fd = -1;
        if (t4_directory_sync(staging) < 0 || rename(staging, directory) < 0 ||
            t4_directory_sync(s->directory) < 0) goto staged;
        result = 0;
staged:
        if (fd >= 0) close(fd);
        if (result < 0) { int saved = errno; staging_remove(staging); errno = saved; return -1; }
    } else if (!S_ISDIR(st.st_mode)) { errno = ENOTDIR; return -1; }
    if (t4_path(path, directory, "prompt.txt") < 0) return -1;
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    unsigned char original[T4_MAX_PROMPT + 1];
    if (fd < 0) return -1;
    ssize_t n = read(fd, original, sizeof(original));
    close(fd);
    if (n != (ssize_t)length || memcmp(original, prompt, length)) { errno = EINVAL; return -1; }
    if (t4_path(path, directory, "response.txt") < 0 || lstat(path, &st) < 0) return -1;
    if (!S_ISREG(st.st_mode) || st.st_size < 0 || st.st_size > T4_MAX_RESPONSE) {
        errno = EFBIG; return -1;
    }
    *cached = (uint32_t)st.st_size;
    if (t4_path(path, directory, "done") < 0) return -1;
    *done = access(path, F_OK) == 0;
    return t4_event(s, *done ? "replay" : "generate", request, *cached);
}

int t4_turn_done(const char *directory)
{
    char path[PATH_MAX];
    if (t4_path(path, directory, "done") < 0) return -1;
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return -1;
    int result = fsync(fd);
    close(fd);
    return result < 0 ? -1 : t4_directory_sync(directory);
}
