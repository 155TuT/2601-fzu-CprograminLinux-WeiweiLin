#include "task4/checkpoint.h"
#include "task4/files.h"
#include "task4/text.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

int t4_checkpoint_lock(const char *path)
{
    char lock[PATH_MAX];
    int n = snprintf(lock, sizeof(lock), "%s.lock", path);
    if (n < 0 || n >= PATH_MAX) { errno = ENAMETOOLONG; return -1; }
    int fd = open(lock, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return -1;
    if (flock(fd, LOCK_EX | LOCK_NB) < 0) { int saved = errno; close(fd); errno = saved; return -1; }
    /* Only the lock owner may update this informational PID. A paused/live
     * owner's lock fails above and is never changed or unlinked here. */
    struct stat metadata;
    if (fstat(fd, &metadata) < 0) { int saved = errno; close(fd); errno = saved; return -1; }
    if (!S_ISREG(metadata.st_mode) || metadata.st_nlink != 1) {
        close(fd); errno = EINVAL; return -1;
    }
    char owner[64];
    n = snprintf(owner, sizeof(owner), "pid=%ld\n", (long)getpid());
    if (n < 0 || (size_t)n >= sizeof(owner) || lseek(fd, 0, SEEK_SET) < 0 ||
        t4_write_all(fd, owner, (size_t)n) < 0 || ftruncate(fd, n) < 0 || fsync(fd) < 0) {
        int saved = errno; close(fd); errno = saved; return -1;
    }
    return fd;
}

/* FNV-1a 检测残缺/损坏记录，不作为安全校验或网络认证。 */
static uint32_t checksum(const void *header, const void *body, size_t length)
{
    const unsigned char *p = header, *b = body;
    uint32_t hash = UINT32_C(2166136261);
    for (size_t i = 0; i < 12; ++i) hash = (hash ^ p[i]) * UINT32_C(16777619);
    for (size_t i = 0; i < length; ++i) hash = (hash ^ b[i]) * UINT32_C(16777619);
    return hash;
}

static int record_write(int fd, uint32_t type, uint32_t offset, const void *body, uint32_t length)
{
    unsigned char record[20];
    uint32_t header[4] = {htonl(type), htonl(offset), htonl(length), 0};
    header[3] = htonl(checksum(header, body, length));
    memcpy(record, header, sizeof(header));
    if (length) memcpy(record + sizeof(header), body, length);
    return t4_write_all(fd, record, sizeof(header) + length);
}

static int steer_path(char target[PATH_MAX], const char *path)
{
    int n = snprintf(target, PATH_MAX, "%s.steer", path);
    if (n < 0 || n >= PATH_MAX) { errno = ENAMETOOLONG; return -1; }
    return 0;
}

static int parent_path(char parent[PATH_MAX], const char *path)
{
    if (strlen(path) >= PATH_MAX) { errno = ENAMETOOLONG; return -1; }
    strcpy(parent, path);
    char *slash = strrchr(parent, '/');
    if (!slash) strcpy(parent, ".");
    else if (slash == parent) slash[1] = 0;
    else *slash = 0;
    return 0;
}

/* The main checkpoint lock also covers this sidecar. A queued correction is
 * durable before CANCEL is sent, and disappears only after the next turn's
 * checkpoint has atomically replaced the previous turn. */
static int steer_remove(const char *path, struct t4_checkpoint *c)
{
    char target[PATH_MAX], parent[PATH_MAX];
    if (steer_path(target, path) < 0 || parent_path(parent, path) < 0) return -1;
    if (unlink(target) < 0 && errno != ENOENT) return -1;
    if (t4_directory_sync(parent) < 0) return -1;
    c->queued_request = c->queued_length = 0;
    return 0;
}

static int steer_load(const char *path, struct t4_checkpoint *c)
{
    char target[PATH_MAX];
    if (steer_path(target, path) < 0) return -1;
    int fd = open(target, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return errno == ENOENT ? 0 : -1;
    FILE *f = fdopen(fd, "r");
    if (!f) { close(fd); return -1; }
    char magic[32], token[64], numbers[128], extra;
    int result = -1;
    if (!fgets(magic, sizeof(magic), f) || strcmp(magic, "T4STEER1\n") ||
        !fgets(token, sizeof(token), f) || strlen(token) != 33 || token[32] != '\n' ||
        memcmp(token, c->session, 32) || !fgets(numbers, sizeof(numbers), f) ||
        sscanf(numbers, "%u %u %c", &c->queued_request, &c->queued_length, &extra) != 2 ||
        !c->queued_request || c->queued_request > 10000 || !c->queued_length || c->queued_length > T4_MAX_PROMPT ||
        fread(c->queued_prompt, 1, c->queued_length, f) != c->queued_length ||
        memchr(c->queued_prompt, 0, c->queued_length) || fgetc(f) != EOF || ferror(f)) {
        errno = EINVAL;
        goto cleanup;
    }
    c->queued_prompt[c->queued_length] = 0;
    if (c->queued_request != c->request) {
        /* Crash after replacing the checkpoint but before deleting the queue. */
        if (c->queued_request + 1 != c->request || c->queued_length != c->prompt_length ||
            memcmp(c->queued_prompt, c->prompt, c->prompt_length)) { errno = EINVAL; goto cleanup; }
        result = steer_remove(path, c);
    } else result = 0;
cleanup:
    {
        int saved = errno;
        fclose(f);
        errno = saved;
        return result;
    }
}

int t4_checkpoint_queue(const char *path, struct t4_checkpoint *c,
                        const unsigned char *prompt, uint32_t length)
{
    if (!c->request || !length || length > T4_MAX_PROMPT || memchr(prompt, 0, length) ||
        c->queued_length) { errno = EINVAL; return -1; }
    char target[PATH_MAX], temporary[PATH_MAX], parent[PATH_MAX], header[160];
    if (steer_path(target, path) < 0 || parent_path(parent, path) < 0) return -1;
    int n = snprintf(temporary, sizeof(temporary), "%s.tmp-XXXXXX", target);
    if (n < 0 || n >= PATH_MAX) { errno = ENAMETOOLONG; return -1; }
    int fd = mkstemp(temporary);
    if (fd < 0) return -1;
    int result = -1;
    n = snprintf(header, sizeof(header), "T4STEER1\n%s\n%u %u\n", c->session, c->request, length);
    if (n < 0 || (size_t)n >= sizeof(header) || t4_write_all(fd, header, (size_t)n) < 0 ||
        t4_write_all(fd, prompt, length) < 0 || fsync(fd) < 0) goto cleanup;
    if (close(fd) < 0) { fd = -1; goto cleanup; }
    fd = -1;
    if (rename(temporary, target) < 0 || t4_directory_sync(parent) < 0) goto cleanup;
    c->queued_request = c->request; c->queued_length = length;
    memcpy(c->queued_prompt, prompt, length); c->queued_prompt[length] = 0;
    result = 0;
cleanup:
    {
        int saved = errno;
        if (fd >= 0) close(fd);
        if (result < 0) unlink(temporary);
        errno = saved;
        return result;
    }
}

int t4_checkpoint_load(const char *path, struct t4_checkpoint *c)
{
    c->session[0] = 0;
    c->request = c->offset = c->prompt_length = c->file_lines = 0;
    c->queued_request = c->queued_length = 0;
    c->done = 1; c->journal_loaded = 0;
    int fd = open(path, O_RDWR | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return errno == ENOENT ? 0 : -1;
    FILE *f = fdopen(fd, "r+");
    if (!f) { close(fd); return -1; }
    char magic[32], token[64], numbers[128], extra;
    int result = -1;
    if (!fgets(magic, sizeof(magic), f) || strcmp(magic, "T4CHECK2\n") ||
        !fgets(token, sizeof(token), f) || strlen(token) != 33 || token[32] != '\n' ||
        !fgets(numbers, sizeof(numbers), f) ||
        sscanf(numbers, "%u %u %u %c", &c->request, &c->prompt_length, &c->file_lines, &extra) != 3 ||
        c->prompt_length > T4_MAX_PROMPT || c->request > 10000) goto invalid;
    for (size_t i = 0; i < 32; ++i)
        if (!((token[i] >= '0' && token[i] <= '9') ||
              (token[i] >= 'a' && token[i] <= 'f'))) goto invalid;
    memcpy(c->session, token, 32); c->session[32] = 0;
    if (fread(c->prompt, 1, c->prompt_length, f) != c->prompt_length ||
        memchr(c->prompt, 0, c->prompt_length)) goto invalid;
    c->prompt[c->prompt_length] = 0;
    c->done = 0;
    off_t last_good = ftello(f);
    if (last_good < 0) goto cleanup;
    for (;;) {
        uint32_t header[4];
        unsigned char body[4];
        size_t n = fread(header, 1, sizeof(header), f);
        if (ferror(f)) goto cleanup;
        if (!n) break;
        if (n != sizeof(header)) goto torn_tail;
        uint32_t type = ntohl(header[0]), offset = ntohl(header[1]), length = ntohl(header[2]);
        if (length > 4 || offset != c->offset || c->done) goto invalid;
        if (fread(body, 1, length, f) != length) {
            if (ferror(f)) goto cleanup;
            goto torn_tail;
        }
        if (ntohl(header[3]) != checksum(header, body, length)) goto invalid;
        if (type == T4_DELTA) {
            if (!t4_character_valid(body, length) || length > T4_MAX_RESPONSE - c->offset) goto invalid;
            memcpy(c->answer + c->offset, body, length);
            c->offset += length;
        } else if ((type == T4_END || type == T4_CANCELLED) && !length)
            c->done = type == T4_END ? 1U : 2U;
        else goto invalid;
        last_good = ftello(f);
        if (last_good < 0) goto cleanup;
    }
    goto loaded;
torn_tail:
    /* 崩溃写出半条记录时，只回退最后一条，不承认半个 UTF-8 字符。 */
    if (ftruncate(fd, last_good) < 0 || fsync(fd) < 0) goto cleanup;
loaded:
    c->saved_request = c->request; c->saved_offset = c->offset; c->saved_done = c->done;
    c->journal_loaded = 1;
    result = 0;
    goto cleanup;
invalid:
    errno = EINVAL;
cleanup:
    {
        int saved = errno;
        fclose(f);
        errno = saved;
        return result == 0 ? steer_load(path, c) : result;
    }
}

static int checkpoint_replace(const char *path, struct t4_checkpoint *c)
{
    char temp[PATH_MAX], parent[PATH_MAX], header[160];
    int n = snprintf(temp, sizeof(temp), "%s.tmp-XXXXXX", path);
    if (n < 0 || n >= PATH_MAX || strlen(path) >= PATH_MAX) { errno = ENAMETOOLONG; return -1; }
    strcpy(parent, path);
    char *slash = strrchr(parent, '/');
    if (!slash) strcpy(parent, ".");
    else if (slash == parent) slash[1] = 0;
    else *slash = 0;
    int fd = mkstemp(temp);
    if (fd < 0) return -1;
    n = snprintf(header, sizeof(header), "T4CHECK2\n%s\n%u %u %u\n",
                 c->session, c->request, c->prompt_length, c->file_lines);
    int result = -1;
    if (n < 0 || (size_t)n >= sizeof(header) ||
        t4_write_all(fd, header, (size_t)n) < 0 ||
        t4_write_all(fd, c->prompt, c->prompt_length) < 0) goto cleanup;
    /* 新轮次通常 offset=0；必要时重建也保留完整历史前缀。 */
    for (uint32_t offset = 0; offset < c->offset;) {
        unsigned width = t4_character_width(c->answer[offset]);
        if (!width || width > c->offset - offset ||
            !t4_character_valid(c->answer + offset, width) ||
            record_write(fd, T4_DELTA, offset, c->answer + offset, width) < 0) goto cleanup;
        offset += width;
    }
    if ((c->done && record_write(fd, c->done == 2 ? T4_CANCELLED : T4_END,
                                 c->offset, NULL, 0) < 0) || fsync(fd) < 0) goto cleanup;
    if (close(fd) < 0) { fd = -1; goto cleanup; }
    fd = -1;
    if (rename(temp, path) < 0 || t4_directory_sync(parent) < 0) goto cleanup;
    result = 0;
cleanup:
    {
        int saved = errno;
        if (fd >= 0) close(fd);
        if (result < 0) unlink(temp);
        errno = saved;
        return result;
    }
}

int t4_checkpoint_save(const char *path, struct t4_checkpoint *c)
{
    int result;
    if (!c->journal_loaded || c->request != c->saved_request) result = checkpoint_replace(path, c);
    else {
        if (c->offset == c->saved_offset && c->done == c->saved_done) return 0;
        if (c->offset < c->saved_offset || c->saved_done) { errno = EINVAL; return -1; }
        int fd = open(path, O_WRONLY | O_APPEND | O_NOFOLLOW | O_CLOEXEC);
        if (fd < 0) return -1;
        result = 0;
        uint32_t n = c->offset - c->saved_offset;
        if (n && (n > 4 || !t4_character_valid(c->answer + c->saved_offset, n) ||
                  record_write(fd, T4_DELTA, c->saved_offset, c->answer + c->saved_offset, n) < 0)) result = -1;
        if (!result && c->done && record_write(fd, c->done == 2 ? T4_CANCELLED : T4_END,
                                             c->offset, NULL, 0) < 0) result = -1;
        if (!result) result = fsync(fd);
        int saved = errno;
        if (close(fd) < 0 && !result) { result = -1; saved = errno; }
        errno = saved;
    }
    if (!result) {
        c->journal_loaded = 1; c->saved_request = c->request;
        c->saved_offset = c->offset; c->saved_done = c->done;
        if (c->queued_length && c->request == c->queued_request + 1)
            result = steer_remove(path, c);
    }
    /* A failed write/fsync/close may still have changed the journal. Its last
     * disk offset is now uncertain: a same-process retry must atomically
     * rebuild from the complete in-memory prefix, never append the record
     * again. Also retry a failed steering cleanup even when offset/done are
     * unchanged; otherwise the fast path could leave an old correction live. */
    if (result < 0) c->journal_loaded = 0;
    return result;
}
