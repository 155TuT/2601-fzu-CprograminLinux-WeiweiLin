#include "task4/client_state.h"
#include "task4/files.h"
#include "task4/text.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#define SESSION_LIMIT 10000U
#define HISTORY_HEADER 64U

static int private_directory(int fd)
{
    struct stat st;
    if (fstat(fd, &st) < 0) return -1;
    if (!S_ISDIR(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 0022)) {
        errno = EACCES; return -1;
    }
    return 0;
}

static int session_id(const char *id)
{
    if (!id || strlen(id) != 14 || strncmp(id, "session-", 8)) return 0;
    for (size_t i = 8; i < 14; ++i)
        if (!((id[i] >= '0' && id[i] <= '9') || (id[i] >= 'A' && id[i] <= 'Z') ||
              (id[i] >= 'a' && id[i] <= 'z'))) return 0;
    return 1;
}

/* Walk each component through a directory descriptor: existing symlinks and
 * regular files cannot become parents of automatically managed client state. */
static int directory_open(const char *path, int create)
{
    if (!path || !*path) { errno = EINVAL; return -1; }
    if (strlen(path) >= PATH_MAX) { errno = ENAMETOOLONG; return -1; }
    char copy[PATH_MAX];
    strcpy(copy, path);
    int fd = open(path[0] == '/' ? "/" : ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return -1;
    char *save = NULL;
    for (char *part = strtok_r(copy, "/", &save); part; part = strtok_r(NULL, "/", &save)) {
        if (create) {
            if (mkdirat(fd, part, 0700) == 0) {
                if (fsync(fd) < 0) goto fail;
            } else if (errno != EEXIST) goto fail;
        }
        int child = openat(fd, part, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (child < 0) goto fail;
        close(fd);
        fd = child;
    }
    return fd;
fail:
    {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
}

int t4_client_state_new(const char *session_dir, char *out, size_t capacity)
{
    if (!session_dir || !*session_dir || !out || !capacity) { errno = EINVAL; return -1; }
    out[0] = 0;
    char temporary[PATH_MAX];
    int n = snprintf(temporary, sizeof(temporary), "%s/session-XXXXXX", session_dir);
    static const char checkpoint[] = "/checkpoint.ckpt";
    if (n < 0 || (size_t)n >= sizeof(temporary) ||
        (size_t)n + sizeof(checkpoint) > capacity) { errno = ENAMETOOLONG; return -1; }
    int parent = directory_open(session_dir, 1);
    if (parent < 0) return -1;
    if (private_directory(parent) < 0) { int saved = errno; close(parent); errno = saved; return -1; }
    if (!mkdtemp(temporary)) { int saved = errno; close(parent); errno = saved; return -1; }
    if (fsync(parent) < 0) {
        int saved = errno;
        (void)rmdir(temporary);
        close(parent);
        errno = saved;
        return -1;
    }
    close(parent);
    memcpy(out, temporary, (size_t)n);
    memcpy(out + n, checkpoint, sizeof(checkpoint));
    return 0;
}

/* Metadata is informational only. Stale PID text is ignored unless flock says
 * that another client currently holds this exact lock inode. */
static const char *session_status(int directory, long *owner)
{
    *owner = 0;
    int fd = openat(directory, "checkpoint.ckpt.lock",
                    O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    if (fd >= 0) {
        struct stat lock_metadata;
        if (fstat(fd, &lock_metadata) < 0 || !S_ISREG(lock_metadata.st_mode) ||
            lock_metadata.st_nlink != 1 || lock_metadata.st_uid != geteuid()) {
            close(fd);
            return "invalid-lock";
        }
        if (flock(fd, LOCK_EX | LOCK_NB) < 0) {
            if (errno != EWOULDBLOCK && errno != EAGAIN) {
                close(fd);
                return "unavailable";
            }
            char metadata[64], extra;
            ssize_t n = pread(fd, metadata, sizeof(metadata) - 1, 0);
            if (n > 0) {
                metadata[n] = 0;
                long pid;
                if (sscanf(metadata, "pid=%ld %c", &pid, &extra) == 1 && pid > 0) *owner = pid;
            }
            close(fd);
            return "active";
        }
        close(fd);
    } else if (errno != ENOENT) return errno == ELOOP ? "invalid-lock" : "unavailable";
    struct stat st;
    if (fstatat(directory, "checkpoint.ckpt", &st, AT_SYMLINK_NOFOLLOW) < 0)
        return errno == ENOENT ? "uninitialized" : "unavailable";
    return S_ISREG(st.st_mode) ? "saved" : "invalid-file";
}

static int session_compare(const void *left, const void *right)
{
    const struct t4_client_session *a = left, *b = right;
    return strcmp(a->id, b->id);
}

int t4_client_state_scan(const char *session_dir, t4_client_session_fn visit, void *opaque)
{
    if (!visit) { errno = EINVAL; return -1; }
    int fd = directory_open(session_dir, 0);
    if (fd < 0) {
        if (errno != ENOENT) return -1;
        return 0;
    }
    if (private_directory(fd) < 0) { int saved = errno; close(fd); errno = saved; return -1; }
    DIR *directory = fdopendir(fd);
    if (!directory) { int saved = errno; close(fd); errno = saved; return -1; }
    int result = 0;
    size_t count = 0, capacity = 0;
    struct t4_client_session *sessions = NULL;
    struct dirent *entry;
    for (;;) {
        errno = 0;
        entry = readdir(directory);
        if (!entry) { if (errno) result = -1; break; }
        if (!session_id(entry->d_name)) continue;
        int child = openat(fd, entry->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (child < 0) continue;
        if (private_directory(child) < 0) { close(child); continue; }
        long owner;
        const char *status = session_status(child, &owner);
        close(child);
        if (count == SESSION_LIMIT) { errno = EOVERFLOW; result = -1; break; }
        if (count == capacity) {
            size_t next = capacity ? capacity * 2 : 16;
            if (next > SESSION_LIMIT) next = SESSION_LIMIT;
            void *p = realloc(sessions, next * sizeof(*sessions));
            if (!p) { result = -1; break; }
            sessions = p; capacity = next;
        }
        struct t4_client_session *item = &sessions[count];
        snprintf(item->id, sizeof(item->id), "%.14s", entry->d_name);
        snprintf(item->status, sizeof(item->status), "%s", status);
        item->owner = owner;
        int n = snprintf(item->path, sizeof(item->path), "%s/%s/checkpoint.ckpt", session_dir, item->id);
        if (n < 0 || (size_t)n >= sizeof(item->path)) { errno = ENAMETOOLONG; result = -1; break; }
        ++count;
    }
    int saved = errno;
    if (closedir(directory) < 0 && !result) { result = -1; saved = errno; }
    if (!result) {
        if (count > 1) qsort(sessions, count, sizeof(*sessions), session_compare);
        for (size_t i = 0; i < count; ++i) {
            if (visit(&sessions[i], opaque) < 0) { result = -1; saved = errno; break; }
        }
    }
    free(sessions);
    errno = saved;
    return result;
}

static int session_print(const struct t4_client_session *item, void *opaque)
{
    size_t *count = opaque;
    if (printf("checkpoint=%s status=%s", item->path, item->status) < 0 ||
        (item->owner && printf(" pid=%ld", item->owner) < 0) || putchar('\n') == EOF) return -1;
    ++*count;
    return 0;
}

int t4_client_state_list(const char *session_dir)
{
    size_t count = 0;
    if (t4_client_state_scan(session_dir, session_print, &count) < 0) return -1;
    return !count && printf("No client sessions in %s\n", session_dir) < 0 ? -1 : 0;
}

/* Split a leaf from its securely opened real parent. Explicit checkpoints may
 * live outside the managed root, but no path component may be a symlink. */
static int path_parent(const char *path, char leaf[NAME_MAX + 1])
{
    if (!path || !*path) { errno = EINVAL; return -1; }
    char parent[PATH_MAX];
    if (strlen(path) >= sizeof(parent)) { errno = ENAMETOOLONG; return -1; }
    strcpy(parent, path);
    char *slash = strrchr(parent, '/');
    const char *name = slash ? slash + 1 : path;
    if (!*name || !strcmp(name, ".") || !strcmp(name, "..")) { errno = EINVAL; return -1; }
    if (strlen(name) > NAME_MAX) { errno = ENAMETOOLONG; return -1; }
    strcpy(leaf, name);
    if (!slash) strcpy(parent, ".");
    else if (slash == parent) slash[1] = 0;
    else *slash = 0;
    return directory_open(parent, 0);
}

static int regular_owned(int fd, off_t maximum)
{
    struct stat st;
    if (fstat(fd, &st) < 0) return -1;
    if (!S_ISREG(st.st_mode) || st.st_nlink != 1 || st.st_uid != geteuid() ||
        (st.st_mode & 0022) || st.st_size < 0 || st.st_size > maximum) {
        errno = EINVAL; return -1;
    }
    return 0;
}

int t4_client_state_resolve(const char *session_dir, const char *id_or_path,
                          char *out, size_t capacity)
{
    if (!session_dir || !*session_dir || !out || !capacity || !id_or_path || !*id_or_path) {
        errno = EINVAL; return -1;
    }
    out[0] = 0;
    char candidate[PATH_MAX], leaf[NAME_MAX + 1];
    int n = session_id(id_or_path)
        ? snprintf(candidate, sizeof(candidate), "%s/%s/checkpoint.ckpt", session_dir, id_or_path)
        : snprintf(candidate, sizeof(candidate), "%s", id_or_path);
    if (n < 0 || (size_t)n >= sizeof(candidate) || (size_t)n >= capacity) {
        errno = ENAMETOOLONG; return -1;
    }
    int parent = path_parent(candidate, leaf);
    if (parent < 0) return -1;
    int fd = openat(parent, leaf, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    int result = fd < 0 ? -1 : regular_owned(fd, (off_t)T4_MAX_RESPONSE * 20 + T4_MAX_PROMPT + 256);
    int saved = errno;
    if (fd >= 0) close(fd);
    close(parent);
    if (!result) memcpy(out, candidate, (size_t)n + 1);
    errno = saved;
    return result;
}

static int random_name(char *out, size_t size, const char *prefix)
{
    unsigned char bytes[8];
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    size_t offset = 0;
    while (offset < sizeof(bytes)) {
        ssize_t n = read(fd, bytes + offset, sizeof(bytes) - offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { int saved = n ? errno : EIO; close(fd); errno = saved; return -1; }
        offset += (size_t)n;
    }
    close(fd);
    int n = snprintf(out, size, "%s%02x%02x%02x%02x%02x%02x%02x%02x", prefix,
                     bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7]);
    if (n < 0 || (size_t)n >= size) { errno = ENAMETOOLONG; return -1; }
    return 0;
}

int t4_client_state_delete(const char *session_dir, const char *id, char *out, size_t capacity)
{
    if (!session_dir || !*session_dir || !session_id(id) || !out || !capacity) {
        errno = EINVAL; return -1;
    }
    out[0] = 0;
    char destination[64], prefix[32];
    snprintf(prefix, sizeof(prefix), "%s-", id);
    if (random_name(destination, sizeof(destination), prefix) < 0) return -1;
    int n = snprintf(out, capacity, "%s/.trash/%s", session_dir, destination);
    if (n < 0 || (size_t)n >= capacity) { out[0] = 0; errno = ENAMETOOLONG; return -1; }
    int parent = -1, child = -1, trash = -1, lock = -1, result = -1;
    parent = directory_open(session_dir, 0);
    if (parent < 0 || private_directory(parent) < 0) goto cleanup;
    child = openat(parent, id, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (child < 0 || private_directory(child) < 0) goto cleanup;
    /* A symlink checkpoint must not be used as a way to affect external data. */
    struct stat checkpoint;
    if (fstatat(child, "checkpoint.ckpt", &checkpoint, AT_SYMLINK_NOFOLLOW) == 0) {
        if (!S_ISREG(checkpoint.st_mode) || checkpoint.st_uid != geteuid() || checkpoint.st_nlink != 1) {
            errno = EINVAL; goto cleanup;
        }
    } else if (errno != ENOENT) goto cleanup;
    lock = openat(child, "checkpoint.ckpt.lock",
                  O_RDWR | O_CREAT | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (lock < 0 || regular_owned(lock, 1024) < 0 || flock(lock, LOCK_EX | LOCK_NB) < 0) goto cleanup;
    if (mkdirat(parent, ".trash", 0700) < 0 && errno != EEXIST) goto cleanup;
    trash = openat(parent, ".trash", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (trash < 0 || private_directory(trash) < 0) goto cleanup;
    struct stat before, current, held, named;
    if (fstat(child, &before) < 0 || fstatat(parent, id, &current, AT_SYMLINK_NOFOLLOW) < 0 ||
        fstat(lock, &held) < 0 || fstatat(child, "checkpoint.ckpt.lock", &named, AT_SYMLINK_NOFOLLOW) < 0)
        goto cleanup;
    if (before.st_dev != current.st_dev || before.st_ino != current.st_ino ||
        held.st_dev != named.st_dev || held.st_ino != named.st_ino) { errno = ESTALE; goto cleanup; }
    /* Linux renameat2 avoids replacing even an unexpectedly created empty
     * destination. The lock remains held across rename and directory fsync. */
    if (syscall(SYS_renameat2, parent, id, trash, destination, 1U) < 0) goto cleanup;
    if (fsync(trash) < 0 || fsync(parent) < 0) {
        int saved = errno;
        (void)syscall(SYS_renameat2, trash, destination, parent, id, 1U);
        errno = saved; goto cleanup;
    }
    result = 0;
cleanup:
    {
        int saved = errno;
        if (lock >= 0) close(lock);
        if (trash >= 0) close(trash);
        if (child >= 0) close(child);
        if (parent >= 0) close(parent);
        if (result < 0) out[0] = 0;
        errno = saved;
        return result;
    }
}

static int token_valid(const char *token)
{
    if (!token || strlen(token) != 32) return 0;
    for (size_t i = 0; i < 32; ++i)
        if (!((token[i] >= '0' && token[i] <= '9') || (token[i] >= 'a' && token[i] <= 'f'))) return 0;
    return 1;
}

static uint32_t hash_bytes(uint32_t hash, const void *data, size_t length)
{
    const unsigned char *p = data;
    for (size_t i = 0; i < length; ++i) hash = (hash ^ p[i]) * UINT32_C(16777619);
    return hash;
}

static int history_directory(const char *path, int create)
{
    char leaf[NAME_MAX + 1], name[NAME_MAX + 1];
    int parent = path_parent(path, leaf);
    if (parent < 0) return -1;
    int n = snprintf(name, sizeof(name), "%s.history", leaf);
    int fd = -1;
    if (n < 0 || (size_t)n >= sizeof(name)) { errno = ENAMETOOLONG; goto cleanup; }
    if (create && mkdirat(parent, name, 0700) == 0) {
        if (fsync(parent) < 0) goto cleanup;
    } else if (create && errno != EEXIST) goto cleanup;
    fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd >= 0 && private_directory(fd) < 0) { int saved = errno; close(fd); fd = -1; errno = saved; }
cleanup:
    { int saved = errno; close(parent); errno = saved; return fd; }
}

static int exact_read(int fd, void *buffer, size_t length)
{
    unsigned char *p = buffer;
    while (length) {
        ssize_t n = read(fd, p, length);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { if (!n) errno = EINVAL; return -1; }
        p += n; length -= (size_t)n;
    }
    return 0;
}

static int text_valid(const unsigned char *p, uint32_t length)
{
    for (uint32_t offset = 0; offset < length;) {
        unsigned width = t4_character_width(p[offset]);
        if (!width || width > length - offset || !t4_character_valid(p + offset, width)) return 0;
        offset += width;
    }
    return 1;
}

static int history_load(int directory, const char *name, struct t4_checkpoint *c)
{
    memset(c, 0, sizeof(*c));
    int fd = openat(directory, name, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return -1;
    int result = -1;
    unsigned char header[HISTORY_HEADER];
    uint32_t fields[6];
    if (regular_owned(fd, HISTORY_HEADER + T4_MAX_PROMPT + T4_MAX_RESPONSE) < 0 ||
        exact_read(fd, header, sizeof(header)) < 0) goto cleanup;
    if (memcmp(header, "T4HIST1\n", 8)) { errno = EINVAL; goto cleanup; }
    memcpy(c->session, header + 8, 32);
    memcpy(fields, header + 40, sizeof(fields));
    c->request = ntohl(fields[0]); c->prompt_length = ntohl(fields[1]);
    c->offset = ntohl(fields[2]); c->done = ntohl(fields[3]); c->file_lines = ntohl(fields[4]);
    if (!token_valid(c->session) || !c->request || c->request > SESSION_LIMIT ||
        c->prompt_length > T4_MAX_PROMPT || c->offset > T4_MAX_RESPONSE ||
        (c->done != 1 && c->done != 2)) { errno = EINVAL; goto cleanup; }
    struct stat st;
    if (fstat(fd, &st) < 0) goto cleanup;
    if (st.st_size != (off_t)(HISTORY_HEADER + c->prompt_length + c->offset)) { errno = EINVAL; goto cleanup; }
    c->answer = malloc((size_t)c->offset + 1);
    if (!c->answer || exact_read(fd, c->prompt, c->prompt_length) < 0 ||
        exact_read(fd, c->answer, c->offset) < 0) goto cleanup;
    c->answer[c->offset] = 0;
    uint32_t hash = hash_bytes(UINT32_C(2166136261), header, 60);
    hash = hash_bytes(hash, c->prompt, c->prompt_length);
    hash = hash_bytes(hash, c->answer, c->offset);
    if (hash != ntohl(fields[5]) || !text_valid(c->prompt, c->prompt_length) ||
        !text_valid(c->answer, c->offset)) { errno = EINVAL; goto cleanup; }
    result = 0;
cleanup:
    {
        int saved = errno;
        close(fd);
        if (result < 0) { free(c->answer); c->answer = NULL; }
        errno = saved; return result;
    }
}

static int history_same(int directory, const char *name, const struct t4_checkpoint *c)
{
    struct t4_checkpoint old;
    if (history_load(directory, name, &old) < 0) return -1;
    int same = !strcmp(old.session, c->session) && old.request == c->request &&
        old.prompt_length == c->prompt_length && old.offset == c->offset &&
        old.done == c->done && old.file_lines == c->file_lines &&
        !memcmp(old.prompt, c->prompt, c->prompt_length) &&
        (!c->offset || !memcmp(old.answer, c->answer, c->offset));
    free(old.answer);
    if (!same) { errno = EEXIST; return -1; }
    return 0;
}

int t4_client_history_archive(const char *path, const struct t4_checkpoint *c)
{
    if (!c || !token_valid(c->session) || !c->request || c->request > SESSION_LIMIT ||
        (c->done != 1 && c->done != 2) || c->prompt_length > T4_MAX_PROMPT ||
        c->offset > T4_MAX_RESPONSE || (c->offset && !c->answer) ||
        !text_valid(c->prompt, c->prompt_length) || !text_valid(c->answer, c->offset)) {
        errno = EINVAL; return -1;
    }
    int directory = history_directory(path, 1);
    if (directory < 0) return -1;
    char name[32], temporary[64] = "";
    snprintf(name, sizeof(name), "turn-%08u.ckpt", c->request);
    struct stat st;
    int result = -1, fd = -1;
    if (fstatat(directory, name, &st, AT_SYMLINK_NOFOLLOW) == 0) {
        result = history_same(directory, name, c); goto cleanup;
    }
    if (errno != ENOENT || random_name(temporary, sizeof(temporary), ".pending-") < 0) goto cleanup;
    fd = openat(directory, temporary, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) { temporary[0] = 0; goto cleanup; }
    unsigned char header[HISTORY_HEADER];
    uint32_t fields[6] = {htonl(c->request), htonl(c->prompt_length), htonl(c->offset),
                          htonl(c->done), htonl(c->file_lines), 0};
    memcpy(header, "T4HIST1\n", 8); memcpy(header + 8, c->session, 32);
    memcpy(header + 40, fields, sizeof(fields));
    uint32_t hash = hash_bytes(UINT32_C(2166136261), header, 60);
    hash = hash_bytes(hash, c->prompt, c->prompt_length);
    hash = hash_bytes(hash, c->answer, c->offset);
    fields[5] = htonl(hash); memcpy(header + 60, &fields[5], 4);
    if (t4_write_all(fd, header, sizeof(header)) < 0 ||
        t4_write_all(fd, c->prompt, c->prompt_length) < 0 ||
        t4_write_all(fd, c->answer, c->offset) < 0 || fsync(fd) < 0) goto cleanup;
    if (close(fd) < 0) { fd = -1; goto cleanup; }
    fd = -1;
    /* Atomic publish without replacing existing content, including symlinks.
     * Pending files from an interrupted write are ignored by enumeration. */
    if (syscall(SYS_renameat2, directory, temporary, directory, name, 1U) < 0) {
        if (errno == EEXIST) result = history_same(directory, name, c);
        goto cleanup;
    }
    temporary[0] = 0;
    result = fsync(directory);
cleanup:
    {
        int saved = errno;
        if (fd >= 0) close(fd);
        if (*temporary) (void)unlinkat(directory, temporary, 0);
        close(directory);
        errno = saved; return result;
    }
}

static int request_compare(const void *a, const void *b)
{
    uint32_t left = *(const uint32_t *)a, right = *(const uint32_t *)b;
    return left > right ? 1 : left < right ? -1 : 0;
}

int t4_client_history_scan(const char *path, const char *session,
                         t4_client_history_fn visit, void *opaque)
{
    if (!token_valid(session) || !visit) { errno = EINVAL; return -1; }
    int fd = history_directory(path, 0);
    if (fd < 0) return errno == ENOENT ? 0 : -1;
    DIR *directory = fdopendir(fd);
    if (!directory) { int saved = errno; close(fd); errno = saved; return -1; }
    uint32_t *requests = malloc(SESSION_LIMIT * sizeof(*requests));
    if (!requests) { closedir(directory); return -1; }
    size_t count = 0;
    int result = 0;
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(directory);
        if (!entry) { if (errno) result = -1; break; }
        if (strlen(entry->d_name) != 18 || strncmp(entry->d_name, "turn-", 5) ||
            strcmp(entry->d_name + 13, ".ckpt")) continue;
        uint32_t request = 0;
        for (size_t i = 5; i < 13; ++i) {
            if (entry->d_name[i] < '0' || entry->d_name[i] > '9') { request = 0; break; }
            request = request * 10 + (unsigned)(entry->d_name[i] - '0');
        }
        if (!request || request > SESSION_LIMIT || count == SESSION_LIMIT) {
            errno = EINVAL; result = -1; break;
        }
        requests[count++] = request;
    }
    if (!result && count > 1) qsort(requests, count, sizeof(*requests), request_compare);
    for (size_t i = 0; !result && i < count; ++i) {
        char name[32];
        snprintf(name, sizeof(name), "turn-%08u.ckpt", requests[i]);
        struct t4_checkpoint c;
        if (history_load(fd, name, &c) < 0) { result = -1; break; }
        if (c.request != requests[i] || strcmp(c.session, session)) { errno = EINVAL; result = -1; }
        else result = visit(&c, opaque);
        free(c.answer);
    }
    int saved = errno;
    free(requests);
    if (closedir(directory) < 0 && !result) { result = -1; saved = errno; }
    errno = saved; return result;
}

void t4_client_state_lock_error(const char *path, int error)
{
    fprintf(stderr, "[client] checkpoint open/lock %s: ", path ? path : "(unspecified)");
    if (error == EWOULDBLOCK || error == EAGAIN) {
        fputs("checkpoint is in use: another client owns this checkpoint, possibly paused. Continue in its terminal, "
              "or start a new client for an independent session. Resume after the owner exits.\n", stderr);
    } else if (error == EINVAL) {
        fputs("invalid or damaged checkpoint/lock data. Keep this file for inspection; "
              "start a new client to create a separate session.\n", stderr);
    } else if (error == ENOENT) {
        fputs("file or parent directory is missing. --resume requires an existing checkpoint; "
              "a new client creates its own session directory.\n", stderr);
    } else if (error == EACCES || error == EPERM || error == EROFS) {
        fputs("cannot read or write this path. Check directory/file permissions and filesystem access.\n", stderr);
    } else if (error == ENOTDIR || error == ELOOP) {
        fputs("a path component is not a directory or is a symbolic link. "
              "Choose a regular checkpoint in a real directory.\n", stderr);
    } else {
        fprintf(stderr, "%s. Check the checkpoint path and storage.\n", strerror(error));
    }
}
