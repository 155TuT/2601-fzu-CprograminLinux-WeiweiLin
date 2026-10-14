/* Real file I/O with one-shot failures at the syscall boundary. The linker
 * wraps only this isolated test executable; production binaries are untouched.
 * All checkpoints are owned by this process in a new /tmp directory. */
#include "task4/checkpoint.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum fault {
    NONE, APPEND_SYNC, APPEND_PARTIAL, APPEND_CLOSE,
    REPLACE_SYNC, REPLACE_DIRECTORY, STEER_UNLINK, STEER_DIRECTORY
};
static enum fault armed;
static unsigned hits, directory_syncs;
static int partial_fd = -1;

ssize_t __real_write(int fd, const void *data, size_t length);
int __real_fsync(int fd);
int __real_close(int fd);
int __real_unlink(const char *path);

static int appending(int fd)
{
    int flags = fcntl(fd, F_GETFL);
    return flags >= 0 && (flags & O_APPEND);
}

static int fail_once(void)
{
    armed = NONE; ++hits; errno = EIO; return -1;
}

ssize_t __wrap_write(int fd, const void *data, size_t length)
{
    if (armed == APPEND_PARTIAL && appending(fd)) {
        if (partial_fd < 0) {
            /* Persist only part of the real journal header, then make the
             * write_all loop fail while completing that same record. */
            assert(length > 5);
            partial_fd = fd;
            return __real_write(fd, data, 5);
        }
        assert(partial_fd == fd);
        return fail_once();
    }
    return __real_write(fd, data, length);
}

int __wrap_fsync(int fd)
{
    struct stat metadata;
    assert(fstat(fd, &metadata) == 0);
    if (armed == APPEND_SYNC && appending(fd)) {
        /* The complete record exists even though the caller sees an error. */
        assert(__real_fsync(fd) == 0);
        return fail_once();
    }
    if (armed == REPLACE_SYNC && S_ISREG(metadata.st_mode) && !appending(fd))
        return fail_once();
    if (S_ISDIR(metadata.st_mode)) {
        if (armed == REPLACE_DIRECTORY) return fail_once();
        /* The first directory sync publishes the replacement. The second
         * follows unlink of the now-stale steering sidecar. */
        if (armed == STEER_DIRECTORY && ++directory_syncs == 2) return fail_once();
    }
    return __real_fsync(fd);
}

int __wrap_close(int fd)
{
    if (armed == APPEND_CLOSE && appending(fd)) {
        int result = __real_close(fd);
        assert(result == 0);
        return fail_once();
    }
    return __real_close(fd);
}

int __wrap_unlink(const char *path)
{
    size_t length = strlen(path);
    if (armed == STEER_UNLINK && length >= 6 && !strcmp(path + length - 6, ".steer"))
        return fail_once();
    return __real_unlink(path);
}

static void initialize(const char *path, struct t4_checkpoint *c)
{
    memset(c, 0, sizeof(*c));
    c->answer = malloc(T4_MAX_RESPONSE);
    assert(c->answer);
    strcpy(c->session, "0123456789abcdef0123456789abcdef");
    strcpy((char *)c->prompt, "fault injection question");
    c->request = 1; c->prompt_length = (uint32_t)strlen((char *)c->prompt);
    assert(t4_checkpoint_save(path, c) == 0);
    c->answer[0] = 'A'; c->offset = 1;
    assert(t4_checkpoint_save(path, c) == 0);
}

static void arm(enum fault fault)
{
    assert(armed == NONE);
    armed = fault; hits = directory_syncs = 0; partial_fd = -1;
}

static void verify_saved(const char *path, const struct t4_checkpoint *expected)
{
    struct stat before, after;
    assert(stat(path, &before) == 0);
    struct t4_checkpoint loaded = {0};
    loaded.answer = malloc(T4_MAX_RESPONSE);
    assert(loaded.answer && t4_checkpoint_load(path, &loaded) == 0);
    assert(!strcmp(loaded.session, expected->session));
    assert(loaded.request == expected->request && loaded.offset == expected->offset);
    assert(loaded.done == expected->done && loaded.prompt_length == expected->prompt_length);
    assert(!memcmp(loaded.prompt, expected->prompt, loaded.prompt_length));
    assert(!memcmp(loaded.answer, expected->answer, loaded.offset));
    assert(loaded.queued_length == expected->queued_length);
    assert(stat(path, &after) == 0 && before.st_size == after.st_size);
    free(loaded.answer);
}

static void retry(const char *path, struct t4_checkpoint *c)
{
    assert(t4_checkpoint_save(path, c) < 0);
    assert(hits == 1 && armed == NONE);
    assert(t4_checkpoint_save(path, c) == 0);
    verify_saved(path, c);
    struct stat before, after;
    assert(stat(path, &before) == 0);
    assert(t4_checkpoint_save(path, c) == 0); /* Further retries are idempotent. */
    assert(stat(path, &after) == 0 && before.st_size == after.st_size);
    verify_saved(path, c);
}

static void append_fault(const char *path, enum fault fault, unsigned done)
{
    struct t4_checkpoint c;
    initialize(path, &c);
    if (done) c.done = done;
    else { memcpy(c.answer + c.offset, "中", 3); c.offset += 3; }
    arm(fault); retry(path, &c);
    free(c.answer); assert(unlink(path) == 0);
}

static void replacement_fault(const char *path, enum fault fault)
{
    struct t4_checkpoint c;
    initialize(path, &c);
    c.request = 2; c.offset = 0;
    strcpy((char *)c.prompt, "next question after uncertain replacement");
    c.prompt_length = (uint32_t)strlen((char *)c.prompt);
    arm(fault); retry(path, &c);
    free(c.answer); assert(unlink(path) == 0);
}

static void steering_fault(const char *path, enum fault fault)
{
    struct t4_checkpoint c;
    initialize(path, &c);
    static const unsigned char corrected[] = "corrected question must survive cleanup failure";
    assert(t4_checkpoint_queue(path, &c, corrected, sizeof(corrected) - 1) == 0);
    c.done = 2; assert(t4_checkpoint_save(path, &c) == 0);
    c.request = 2; c.offset = c.done = 0;
    memcpy(c.prompt, corrected, sizeof(corrected)); c.prompt_length = sizeof(corrected) - 1;
    arm(fault);
    assert(t4_checkpoint_save(path, &c) < 0);
    assert(hits == 1 && armed == NONE && c.queued_length);
    /* saved_offset/done already equal the new checkpoint. Cleanup must not
     * disappear into the no-change fast path on the second same-process save. */
    assert(c.offset == c.saved_offset && c.done == c.saved_done);
    assert(t4_checkpoint_save(path, &c) == 0 && !c.queued_length && !c.queued_request);
    char sidecar[4096];
    assert(snprintf(sidecar, sizeof(sidecar), "%s.steer", path) > 0);
    assert(access(sidecar, F_OK) < 0 && errno == ENOENT);
    verify_saved(path, &c);
    assert(c.request == 2 && !c.done);
    free(c.answer); assert(unlink(path) == 0);
}

int main(void)
{
    char home[] = "/tmp/task4-checkpoint-faults-XXXXXX", path[4096];
    assert(mkdtemp(home));
    assert(snprintf(path, sizeof(path), "%s/checkpoint.ckpt", home) > 0);
    append_fault(path, APPEND_SYNC, 0);
    puts("PASS F01: a written UTF-8 delta whose fsync fails is rebuilt without duplicate offsets");
    append_fault(path, APPEND_SYNC, 1);
    puts("PASS F02: an END fsync failure retries without duplicate terminal records");
    append_fault(path, APPEND_SYNC, 2);
    puts("PASS F03: a CANCELLED fsync failure retries without duplicate terminal records");
    append_fault(path, APPEND_PARTIAL, 0);
    puts("PASS F04: partial journal writes are replaced atomically by the complete memory prefix");
    append_fault(path, APPEND_CLOSE, 0);
    puts("PASS F05: a close error after a successful append forces safe reconstruction");
    replacement_fault(path, REPLACE_SYNC);
    puts("PASS F06: replacement file fsync failure keeps same-process retry loadable");
    replacement_fault(path, REPLACE_DIRECTORY);
    puts("PASS F07: directory fsync failure after replacement rename is retried safely");
    steering_fault(path, STEER_UNLINK);
    puts("PASS F08: failed steering unlink is retried even when offset and done are unchanged");
    steering_fault(path, STEER_DIRECTORY);
    puts("PASS F09: failed directory sync after steering removal never repeats the correction");
    assert(rmdir(home) == 0);
    puts("All 9 checkpoint fault-injection checks passed.");
    return 0;
}
