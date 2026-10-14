#include "task4/client_state.h"
#include "task4/files.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

struct listed { size_t count; struct t4_client_session sessions[8]; };
static int collect(const struct t4_client_session *item, void *opaque)
{
    struct listed *list = opaque;
    assert(list->count < 8);
    list->sessions[list->count++] = *item;
    return 0;
}

struct turns { unsigned count; uint32_t requests[8], done[8]; };
static int collect_history(const struct t4_checkpoint *c, void *opaque)
{
    struct turns *turns = opaque;
    assert(turns->count < 8);
    assert(c->prompt_length == strlen("测试问题") && !strcmp((const char *)c->prompt, "测试问题"));
    assert(c->offset == strlen("正式回答。") && !strcmp((const char *)c->answer, "正式回答。"));
    assert(c->queued_length == 0 && c->queued_request == 0);
    turns->requests[turns->count] = c->request;
    turns->done[turns->count++] = c->done;
    return 0;
}

static void path(char out[PATH_MAX], const char *parent, const char *leaf)
{
    assert(t4_path(out, parent, leaf) == 0);
}

static void suffix(char out[PATH_MAX], const char *base, const char *tail)
{
    size_t a = strlen(base), b = strlen(tail);
    assert(a + b < PATH_MAX);
    memcpy(out, base, a); memcpy(out + a, tail, b + 1);
}

static void session_directory(char out[PATH_MAX], const char *checkpoint)
{
    strcpy(out, checkpoint);
    char *slash = strrchr(out, '/');
    assert(slash); *slash = 0;
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    char root[PATH_MAX], first[PATH_MAX], second[PATH_MAX], restored[PATH_MAX], trash[PATH_MAX];
    path(root, argv[1], "clients");
    assert(t4_client_state_new(root, first, sizeof(first)) == 0);
    assert(t4_client_state_new(root, second, sizeof(second)) == 0 && strcmp(first, second));
    struct listed list = {0};
    assert(t4_client_state_scan(root, collect, &list) == 0 && list.count == 2);
    assert(strcmp(list.sessions[0].id, list.sessions[1].id) < 0);
    assert(!strcmp(list.sessions[0].status, "uninitialized"));
    strcpy(first, list.sessions[0].path); strcpy(second, list.sessions[1].path);
    char first_id[15], second_id[15];
    strcpy(first_id, list.sessions[0].id); strcpy(second_id, list.sessions[1].id);
    puts("PASS stable structured session enumeration and independent directories");

    unsigned char answer[] = "正式回答。";
    struct t4_checkpoint c = {0};
    strcpy(c.session, "0123456789abcdef0123456789abcdef");
    strcpy((char *)c.prompt, "测试问题");
    c.prompt_length = (uint32_t)strlen((char *)c.prompt);
    c.answer = answer; c.offset = (uint32_t)strlen((char *)answer);
    c.request = 1; c.done = 1;
    int lock = t4_checkpoint_lock(first);
    assert(lock >= 0 && t4_checkpoint_save(first, &c) == 0);
    assert(t4_client_state_resolve(root, first_id, restored, sizeof(restored)) == 0 && !strcmp(restored, first));
    assert(t4_client_state_resolve(root, first, restored, sizeof(restored)) == 0 && !strcmp(restored, first));
    list.count = 0;
    assert(t4_client_state_scan(root, collect, &list) == 0);
    assert(!strcmp(list.sessions[0].status, "active") && list.sessions[0].owner == getpid());
    errno = 0;
    assert(t4_client_state_delete(root, first_id, trash, sizeof(trash)) < 0 &&
           (errno == EWOULDBLOCK || errno == EAGAIN));
    close(lock);
    puts("PASS ID/path resolution, active lock display and active deletion rejection");

    int ready[2], release[2];
    assert(pipe(ready) == 0 && pipe(release) == 0);
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        close(ready[0]); close(release[1]);
        int owned = t4_checkpoint_lock(first);
        assert(owned >= 0 && write(ready[1], "x", 1) == 1);
        char b; assert(read(release[0], &b, 1) == 1);
        close(owned); _exit(0);
    }
    close(ready[1]); close(release[0]);
    char b; assert(read(ready[0], &b, 1) == 1);
    errno = 0;
    assert(t4_client_state_delete(root, first_id, trash, sizeof(trash)) < 0 &&
           (errno == EWOULDBLOCK || errno == EAGAIN));
    assert(write(release[1], "x", 1) == 1);
    int status; assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    close(ready[0]); close(release[1]);
    puts("PASS independent process flock protects the original session inode");

    lock = t4_checkpoint_lock(first);
    assert(lock >= 0 && t4_client_history_archive(first, &c) == 0);
    char hist[PATH_MAX], turn1[PATH_MAX], turn2[PATH_MAX], temp[PATH_MAX];
    suffix(hist, first, ".history"); path(turn1, hist, "turn-00000001.ckpt");
    struct stat before, after;
    assert(stat(turn1, &before) == 0 && t4_client_history_archive(first, &c) == 0 && stat(turn1, &after) == 0);
    assert(before.st_ino == after.st_ino && before.st_size == 64 + c.prompt_length + c.offset);
    c.queued_request = 1; c.queued_length = 5; memcpy(c.queued_prompt, "queue", 5);
    c.request = 3; c.done = 2; assert(t4_client_history_archive(first, &c) == 0);
    c.request = 2; c.done = 1; assert(t4_client_history_archive(first, &c) == 0);
    path(temp, hist, ".pending-crash-tail");
    int fd = open(temp, O_CREAT | O_WRONLY | O_EXCL, 0600); assert(fd >= 0); close(fd);
    struct turns turns = {0};
    assert(t4_client_history_scan(first, c.session, collect_history, &turns) == 0 && turns.count == 3);
    assert(turns.requests[0] == 1 && turns.requests[1] == 2 && turns.requests[2] == 3 && turns.done[2] == 2);
    puts("PASS compact durable history, sorted replay, idempotency and excluded steering metadata");

    c.request = 1; c.done = 2;
    assert(t4_client_history_archive(first, &c) < 0 && errno == EEXIST);
    c.request = 4; c.done = 0;
    assert(t4_client_history_archive(first, &c) < 0 && errno == EINVAL);
    turns.count = 0;
    assert(t4_client_history_scan(first, "ffffffffffffffffffffffffffffffff", collect_history, &turns) < 0 && errno == EINVAL);
    assert(turns.count == 0);
    path(turn2, hist, "turn-00000002.ckpt");
    fd = open(turn2, O_RDWR); assert(fd >= 0);
    unsigned char original, changed = '!';
    assert(pread(fd, &original, 1, 64) == 1 && pwrite(fd, &changed, 1, 64) == 1); close(fd);
    turns.count = 0;
    assert(t4_client_history_scan(first, c.session, collect_history, &turns) < 0 && errno == EINVAL);
    fd = open(turn2, O_RDWR); assert(fd >= 0 && pwrite(fd, &original, 1, 64) == 1); close(fd);
    puts("PASS conflicting, pending, foreign-session and corrupt history rejected without replacement");

    char original_dir[PATH_MAX], lockpath[PATH_MAX], movedlock[PATH_MAX];
    session_directory(original_dir, first); suffix(lockpath, first, ".lock");
    assert(stat(lockpath, &before) == 0); close(lock);
    assert(t4_client_state_delete(root, first_id, trash, sizeof(trash)) == 0);
    assert(access(original_dir, F_OK) < 0 && errno == ENOENT);
    path(movedlock, trash, "checkpoint.ckpt.lock");
    assert(stat(movedlock, &after) == 0 && before.st_dev == after.st_dev && before.st_ino == after.st_ino);
    assert(rename(trash, original_dir) == 0);
    unsigned char loaded_answer[128]; struct t4_checkpoint loaded = {.answer = loaded_answer};
    lock = t4_checkpoint_lock(first);
    assert(lock >= 0 && t4_checkpoint_load(first, &loaded) == 0 && loaded.request == 1 && loaded.done == 1);
    assert(loaded.offset == strlen("正式回答。") && !memcmp(loaded.answer, "正式回答。", loaded.offset));
    turns.count = 0;
    assert(t4_client_history_scan(first, loaded.session, collect_history, &turns) == 0 && turns.count == 3);
    close(lock);
    puts("PASS recoverable soft deletion preserves checkpoint, history and original lock inode");

    assert(t4_client_state_delete(root, first, trash, sizeof(trash)) < 0 && errno == EINVAL);
    assert(t4_client_state_delete(root, "../session-ABCDEF", trash, sizeof(trash)) < 0 && errno == EINVAL);
    assert(symlink(first, second) == 0);
    assert(t4_client_state_resolve(root, second_id, restored, sizeof(restored)) < 0 && errno == ELOOP);
    assert(t4_client_state_delete(root, second_id, trash, sizeof(trash)) < 0 && errno == EINVAL);
    assert(unlink(second) == 0 && mkfifo(second, 0600) == 0);
    assert(t4_client_state_resolve(root, second_id, restored, sizeof(restored)) < 0 && errno == EINVAL);
    assert(unlink(second) == 0);
    puts("PASS arbitrary deletion paths, symlink checkpoints and FIFO checkpoints rejected");

    char outside[PATH_MAX], trashroot[PATH_MAX], histsecond[PATH_MAX];
    path(outside, argv[1], "outside"); assert(mkdir(outside, 0700) == 0);
    path(trashroot, root, ".trash"); assert(rmdir(trashroot) == 0 && symlink(outside, trashroot) == 0);
    assert(t4_client_state_delete(root, second_id, trash, sizeof(trash)) < 0);
    assert(unlink(trashroot) == 0);
    suffix(histsecond, second, ".history"); assert(symlink(outside, histsecond) == 0);
    c.request = 1; c.done = 1;
    assert(t4_client_history_archive(second, &c) < 0);
    assert(unlink(histsecond) == 0);
    path(temp, hist, "turn-00000004.ckpt"); assert(symlink(turn1, temp) == 0);
    c.request = 4;
    assert(t4_client_history_archive(first, &c) < 0 && errno == ELOOP);
    assert(unlink(temp) == 0 && mkfifo(temp, 0600) == 0);
    turns.count = 0;
    assert(t4_client_history_scan(first, c.session, collect_history, &turns) < 0 && errno == EINVAL);
    assert(unlink(temp) == 0);
    puts("PASS redirected trash/history directories and symlink/FIFO archives rejected");

    fd = open(temp, O_CREAT | O_EXCL | O_WRONLY, 0600);
    assert(fd >= 0 && ftruncate(fd, (off_t)T4_MAX_RESPONSE + T4_MAX_PROMPT + 65) == 0);
    close(fd); turns.count = 0;
    assert(t4_client_history_scan(first, c.session, collect_history, &turns) < 0 && errno == EINVAL);
    assert(unlink(temp) == 0 && link(turn1, temp) == 0);
    turns.count = 0;
    assert(t4_client_history_scan(first, c.session, collect_history, &turns) < 0 && errno == EINVAL);
    assert(unlink(temp) == 0);
    char lockbackup[PATH_MAX]; suffix(lockbackup, lockpath, ".backup");
    assert(rename(lockpath, lockbackup) == 0 && symlink(lockbackup, lockpath) == 0);
    assert(t4_client_state_delete(root, first_id, trash, sizeof(trash)) < 0 && errno == ELOOP);
    assert(unlink(lockpath) == 0 && rename(lockbackup, lockpath) == 0);
    puts("PASS oversized/hardlinked archives and redirected lock files rejected");
    puts("All 9 client session/history checks passed.");
    return 0;
}
