#include "task4/discovery.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>

#define RECORD_LIMIT (PATH_MAX + 160U)
#define ENDPOINT_FILE "server.endpoint"
#define LEASE_FILE "server.lock"

struct record { struct t4_endpoint endpoint; uint64_t device, inode; };

/* Newly created components are private; openat refuses symlink components. */
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
        if (create && mkdirat(fd, part, 0700) == 0) {
            if (fsync(fd) < 0) goto fail;
        } else if (create && errno != EEXIST) goto fail;
        int child = openat(fd, part, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (child < 0) goto fail;
        close(fd); fd = child;
    }
    struct stat st;
    if (fstat(fd, &st) < 0) goto fail;
    if (st.st_uid != geteuid() || (st.st_mode & 0022)) { errno = EACCES; goto fail; }
    return fd;
fail:
    {
        int saved = errno;
        close(fd); errno = saved; return -1;
    }
}

static int regular(int fd)
{
    struct stat st;
    if (fstat(fd, &st) < 0) return -1;
    if (!S_ISREG(st.st_mode) || st.st_nlink != 1 || st.st_uid != geteuid()) {
        errno = EINVAL; return -1;
    }
    return 0;
}

/* O_NONBLOCK prevents a malicious FIFO from hanging before fstat rejects it. */
static int read_fd(int fd, char *buffer, size_t capacity)
{
    if (regular(fd) < 0 || lseek(fd, 0, SEEK_SET) < 0) return -1;
    size_t used = 0;
    while (used + 1 < capacity) {
        ssize_t count = read(fd, buffer + used, capacity - used - 1);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) return -1;
        if (!count) break;
        used += (size_t)count;
    }
    char extra;
    ssize_t count;
    do { count = read(fd, &extra, 1); } while (count < 0 && errno == EINTR);
    if (count < 0) return -1;
    if (count || memchr(buffer, 0, used)) { errno = EINVAL; return -1; }
    buffer[used] = 0; return 0;
}

static int read_at(int directory, const char *name, char *buffer, size_t capacity)
{
    int fd = openat(directory, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return -1;
    int result = read_fd(fd, buffer, capacity), saved = errno;
    close(fd); errno = saved; return result;
}

static int hex(const char *text, size_t width)
{
    if (!text || strlen(text) != width) return 0;
    for (size_t at = 0; at < width; ++at)
        if (!((text[at] >= '0' && text[at] <= '9') || (text[at] >= 'a' && text[at] <= 'f'))) return 0;
    return 1;
}

static int lines(char *buffer, char **fields, unsigned count)
{
    char *at = buffer;
    for (unsigned n = 0; n < count; ++n) {
        fields[n] = at;
        char *end = strchr(at, '\n');
        if (!end) { errno = EINVAL; return -1; }
        *end = 0; at = end + 1;
    }
    if (*at) { errno = EINVAL; return -1; }
    return 0;
}

static int read_instance(int fd, char instance[33])
{
    char buffer[64], *fields[2];
    if (read_fd(fd, buffer, sizeof(buffer)) < 0 || lines(buffer, fields, 2) < 0) return -1;
    if (strcmp(fields[0], "T4LEASE1") || !hex(fields[1], 32)) { errno = EINVAL; return -1; }
    memcpy(instance, fields[1], 33); return 0;
}

static int identity(char *text, uint64_t *device, uint64_t *inode)
{
    char *space = strchr(text, ' ');
    if (!space || strchr(space + 1, ' ')) { errno = EINVAL; return -1; }
    *space = 0;
    char *parts[2] = {text, space + 1};
    uint64_t *out[2] = {device, inode};
    for (unsigned n = 0; n < 2; ++n) {
        size_t length = strlen(parts[n]);
        if (!length || length > 16 || !hex(parts[n], length)) { errno = EINVAL; return -1; }
        char *end;
        errno = 0;
        uintmax_t value = strtoumax(parts[n], &end, 16);
        if (errno || *end || value > UINT64_MAX) { errno = EINVAL; return -1; }
        *out[n] = (uint64_t)value;
    }
    return 0;
}

static int read_record(int directory, const char *name, struct record *record)
{
    char buffer[RECORD_LIMIT], *fields[6];
    if (read_at(directory, name, buffer, sizeof(buffer)) < 0 || lines(buffer, fields, 6) < 0) return -1;
    struct in_addr ipv4;
    if (strcmp(fields[0], "T4ENDPOINT1") || inet_pton(AF_INET, fields[1], &ipv4) != 1 ||
        strlen(fields[1]) >= sizeof(record->endpoint.host) || fields[3][0] != '/' ||
        strlen(fields[3]) >= sizeof(record->endpoint.state) || strchr(fields[3], '\r') ||
        !hex(fields[5], 32) || identity(fields[4], &record->device, &record->inode) < 0) {
        errno = EINVAL; return -1;
    }
    uint32_t port = 0;
    for (const char *at = fields[2]; *at; ++at) {
        if (*at < '0' || *at > '9' || port > 6553) { errno = EINVAL; return -1; }
        port = port * 10 + (unsigned)(*at - '0');
    }
    if (!port || port > 65535) { errno = EINVAL; return -1; }
    strcpy(record->endpoint.host, fields[1]); record->endpoint.port = (uint16_t)port;
    strcpy(record->endpoint.state, fields[3]); memcpy(record->endpoint.instance, fields[5], 33);
    return 0;
}

static int equal(const struct record *left, const struct record *right)
{
    return left->device == right->device && left->inode == right->inode &&
        left->endpoint.port == right->endpoint.port && !strcmp(left->endpoint.host, right->endpoint.host) &&
        !strcmp(left->endpoint.state, right->endpoint.state) && !strcmp(left->endpoint.instance, right->endpoint.instance);
}

static int held(int lease)
{
    if (regular(lease) < 0) return -1;
    if (flock(lease, LOCK_EX | LOCK_NB) == 0) {
        (void)flock(lease, LOCK_UN); errno = ENOENT; return -1;
    }
    if (errno == EWOULDBLOCK || errno == EAGAIN) return 0;
    return -1;
}

static int current(int directory, int lease, const struct record *expected, struct record *out)
{
    char first[33], last[33];
    struct stat st;
    if (held(lease) < 0 || read_instance(lease, first) < 0 || fstat(directory, &st) < 0 ||
        read_record(directory, ENDPOINT_FILE, out) < 0) return -1;
    if (out->device != (uint64_t)st.st_dev || out->inode != (uint64_t)st.st_ino ||
        strcmp(first, out->endpoint.instance) || (expected && !equal(expected, out))) {
        errno = ENOENT; return -1;
    }
    /* A master may die and restart while files are read. Both lease checks and
     * nonce snapshots must still describe the same running instance. */
    if (held(lease) < 0 || read_instance(lease, last) < 0) return -1;
    if (strcmp(first, last)) { errno = ENOENT; return -1; }
    return 0;
}

static int live(const struct record *expected, const char *state, struct record *out, const char *token)
{
    int directory = directory_open(state, 0);
    if (directory < 0) return -1;
    int lease = openat(directory, LEASE_FILE, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    int result = -1;
    if (lease >= 0 && current(directory, lease, expected, out) == 0) {
        result = 0;
        char *absolute = realpath(state, NULL);
        if (!absolute) result = -1;
        else {
            if (strcmp(absolute, out->endpoint.state)) { errno = EINVAL; result = -1; }
            free(absolute);
        }
        if (!result && token && *token) {
            int session = openat(directory, token, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            struct stat st;
            if (session < 0) result = -1;
            else {
                if (fstat(session, &st) < 0 || st.st_uid != geteuid()) { errno = EACCES; result = -1; }
                close(session);
            }
        }
    }
    int saved = errno;
    if (lease >= 0) close(lease);
    close(directory); errno = saved; return result;
}

static int write_all(int fd, const char *text, size_t length)
{
    while (length) {
        ssize_t count = write(fd, text, length);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { if (!count) errno = EIO; return -1; }
        text += count; length -= (size_t)count;
    }
    return 0;
}

static int atomic_file(int directory, const char *name, const char *text, const char *instance)
{
    char temporary[96];
    int size = snprintf(temporary, sizeof(temporary), ".endpoint-%ld-%s", (long)getpid(), instance);
    if (size < 0 || (size_t)size >= sizeof(temporary)) { errno = ENAMETOOLONG; return -1; }
    int fd = openat(directory, temporary, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return -1;
    int result = write_all(fd, text, strlen(text));
    if (!result) result = fsync(fd);
    int saved = errno;
    if (close(fd) < 0 && !result) { result = -1; saved = errno; }
    if (!result && renameat(directory, temporary, directory, name) < 0) { result = -1; saved = errno; }
    if (!result && fsync(directory) < 0) { result = -1; saved = errno; }
    if (result) (void)unlinkat(directory, temporary, 0);
    errno = saved; return result;
}

void t4_service_child_close(struct t4_service *service)
{
    if (!service) return;
    if (service->lease_fd >= 0) close(service->lease_fd);
    if (service->state_fd >= 0) close(service->state_fd);
    if (service->registry_fd >= 0) close(service->registry_fd);
    service->lease_fd = service->state_fd = service->registry_fd = -1;
}

int t4_service_acquire(const char *state, struct t4_service *service, struct t4_endpoint *existing)
{
    if (!service || !existing) { errno = EINVAL; return -1; }
    memset(service, 0, sizeof(*service));
    service->lease_fd = service->state_fd = service->registry_fd = -1;
    service->state_fd = directory_open(state, 1);
    if (service->state_fd < 0) return -1;
    char *absolute = realpath(state, NULL);
    if (!absolute) goto fail;
    if (strlen(absolute) >= sizeof(service->state) || strchr(absolute, '\n') || strchr(absolute, '\r')) {
        free(absolute); errno = EINVAL; goto fail;
    }
    strcpy(service->state, absolute); free(absolute);
    struct stat st;
    if (fstat(service->state_fd, &st) < 0) goto fail;
    int canonical = directory_open(service->state, 0);
    if (canonical < 0) goto fail;
    struct stat checked;
    int verified = fstat(canonical, &checked), saved = errno;
    close(canonical);
    if (verified < 0) { errno = saved; goto fail; }
    if (checked.st_dev != st.st_dev || checked.st_ino != st.st_ino) { errno = EINVAL; goto fail; }
    service->device = (uint64_t)st.st_dev; service->inode = (uint64_t)st.st_ino;
    service->lease_fd = openat(service->state_fd, LEASE_FILE,
        O_RDWR | O_CREAT | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0600);
    if (service->lease_fd < 0 || regular(service->lease_fd) < 0) goto fail;
    if (flock(service->lease_fd, LOCK_EX | LOCK_NB) < 0) {
        if (errno != EWOULDBLOCK && errno != EAGAIN) goto fail;
        struct record record;
        if (current(service->state_fd, service->lease_fd, NULL, &record) < 0 ||
            strcmp(record.endpoint.state, service->state)) { errno = EBUSY; goto fail; }
        *existing = record.endpoint;
        t4_service_child_close(service); return 1;
    }
    unsigned char random[16];
    size_t used = 0;
    while (used < sizeof(random)) {
        ssize_t count = getrandom(random + used, sizeof(random) - used, 0);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { if (!count) errno = EIO; goto fail; }
        used += (size_t)count;
    }
    for (size_t at = 0; at < sizeof(random); ++at)
        (void)snprintf(service->instance + 2 * at, 3, "%02x", random[at]);
    char metadata[64];
    (void)snprintf(metadata, sizeof(metadata), "T4LEASE1\n%s\n", service->instance);
    if (ftruncate(service->lease_fd, 0) < 0 || lseek(service->lease_fd, 0, SEEK_SET) < 0 ||
        write_all(service->lease_fd, metadata, strlen(metadata)) < 0 || fsync(service->lease_fd) < 0) goto fail;
    if (unlinkat(service->state_fd, ENDPOINT_FILE, 0) < 0 && errno != ENOENT) goto fail;
    if (fsync(service->state_fd) < 0) goto fail;
    return 0;
fail:
    {
        int saved = errno;
        t4_service_child_close(service); errno = saved; return -1;
    }
}

int t4_service_publish(struct t4_service *service, const char *registry, const char *host, uint16_t port)
{
    if (!service || service->lease_fd < 0 || service->state_fd < 0 || !port || !host) { errno = EINVAL; return -1; }
    struct in_addr ipv4;
    if (inet_pton(AF_INET, host, &ipv4) != 1) { errno = EINVAL; return -1; }
    const char *advertised = !strcmp(host, "0.0.0.0") ? "127.0.0.1" : host;
    char record[RECORD_LIMIT];
    int count = snprintf(record, sizeof(record), "T4ENDPOINT1\n%s\n%u\n%s\n%" PRIx64 " %" PRIx64 "\n%s\n",
        advertised, port, service->state, service->device, service->inode, service->instance);
    if (count < 0 || (size_t)count >= sizeof(record)) { errno = ENAMETOOLONG; return -1; }
    int directory = directory_open(registry, 1);
    if (directory < 0) return -1;
    count = snprintf(service->entry, sizeof(service->entry), "%" PRIx64 "-%" PRIx64 ".endpoint",
                     service->device, service->inode);
    if (count < 0 || (size_t)count >= sizeof(service->entry)) { close(directory); errno = ENAMETOOLONG; return -1; }
    if (service->registry_fd >= 0) close(service->registry_fd);
    service->registry_fd = directory;
    if (atomic_file(service->state_fd, ENDPOINT_FILE, record, service->instance) < 0) return -1;
    service->published = 1;
    return atomic_file(directory, service->entry, record, service->instance);
}

static void remove_owned(int directory, const char *name, const struct t4_service *service)
{
    if (directory < 0 || !*name) return;
    struct record record;
    if (read_record(directory, name, &record) == 0 && record.device == service->device &&
        record.inode == service->inode && !strcmp(record.endpoint.state, service->state) &&
        !strcmp(record.endpoint.instance, service->instance)) {
        if (unlinkat(directory, name, 0) == 0) (void)fsync(directory);
    }
}

void t4_service_close(struct t4_service *service)
{
    if (!service) return;
    if (service->published && service->lease_fd >= 0) {
        remove_owned(service->registry_fd, service->entry, service);
        remove_owned(service->state_fd, ENDPOINT_FILE, service);
    }
    t4_service_child_close(service); service->published = 0;
}

int t4_service_discover(const char *registry, const char *explicit_state,
                        const char *token, struct t4_endpoint *endpoint)
{
    if (!endpoint || (token && *token && !hex(token, 32))) { errno = EINVAL; return -1; }
    struct record record;
    if (explicit_state && *explicit_state) {
        if (live(NULL, explicit_state, &record, token) < 0) return -1;
        *endpoint = record.endpoint; return 0;
    }
    int fd = directory_open(registry, 0);
    if (fd < 0) return -1;
    DIR *directory = fdopendir(fd);
    if (!directory) { int saved = errno; close(fd); errno = saved; return -1; }
    unsigned found = 0;
    int result = 0, saved = ENOENT;
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(directory);
        if (!entry) { if (errno) { result = -1; saved = errno; } break; }
        size_t length = strlen(entry->d_name);
        if (length < 10 || length >= 80 || strcmp(entry->d_name + length - 9, ".endpoint")) continue;
        struct record advertised;
        if (read_record(fd, entry->d_name, &advertised) < 0) continue;
        char name[80];
        (void)snprintf(name, sizeof(name), "%" PRIx64 "-%" PRIx64 ".endpoint", advertised.device, advertised.inode);
        if (strcmp(name, entry->d_name) || live(&advertised, advertised.endpoint.state, &record, token) < 0) continue;
        *endpoint = record.endpoint;
        if (++found > 1) { result = -1; saved = EEXIST; break; }
    }
    if (!result && !found) result = -1;
    if (closedir(directory) < 0 && !result) { result = -1; saved = errno; }
    if (result) errno = saved;
    return result;
}

int t4_service_validate(const struct t4_endpoint *endpoint)
{
    if (!endpoint || !hex(endpoint->instance, 32)) { errno = EINVAL; return -1; }
    struct record record;
    if (live(NULL, endpoint->state, &record, NULL) < 0) return -1;
    if (record.endpoint.port != endpoint->port || strcmp(record.endpoint.host, endpoint->host) ||
        strcmp(record.endpoint.state, endpoint->state) || strcmp(record.endpoint.instance, endpoint->instance)) {
        errno = ENOENT; return -1;
    }
    return 0;
}
