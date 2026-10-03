#define _GNU_SOURCE

#include "task4/backend.h"
#include "task4/protocol.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static int random_bytes(void *destination, size_t length)
{
    unsigned char *bytes = destination;
    size_t done = 0;
    int fallback = -1;

    while (done < length) {
        ssize_t count = getrandom(bytes + done, length - done, 0);

        if (count > 0) {
            done += (size_t)count;
        } else if (count == -1 && errno == ENOSYS) {
            fallback = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
            break;
        } else {
            if (count == 0) {
                errno = EIO;
            }
            return -1;
        }
    }
    if (done == length) {
        return 0;
    }
    if (fallback == -1) {
        return -1;
    }
    while (done < length) {
        ssize_t count = read(fallback, bytes + done, length - done);

        if (count <= 0) {
            int saved_error = count == 0 ? EIO : errno;

            (void)close(fallback);
            errno = saved_error;
            return -1;
        }
        done += (size_t)count;
    }
    return close(fallback);
}

static int uniform_below(uint64_t limit, uint64_t *value)
{
    uint64_t random_value;
    uint64_t threshold = (uint64_t)(-limit) % limit;

    /* Rejection sampling avoids modulo bias, including in reservoir selection. */
    do {
        if (random_bytes(&random_value, sizeof(random_value)) == -1) {
            return -1;
        }
    } while (random_value < threshold);
    *value = random_value % limit;
    return 0;
}

static int select_source(const char *corpus_dir)
{
    DIR *directory = opendir(corpus_dir);
    struct dirent *entry;
    uint64_t candidates = 0;
    int selected = -1;
    int failure = 0;

    if (directory == NULL) {
        return -1;
    }
    for (;;) {
        size_t name_length;
        struct stat metadata;
        uint64_t pick;

        errno = 0;
        entry = readdir(directory);
        if (entry == NULL) {
            failure = errno;
            break;
        }
        name_length = strlen(entry->d_name);
        if (name_length < 4
            || strcmp(entry->d_name + name_length - 4, ".txt") != 0) {
            continue;
        }
        if (fstatat(dirfd(directory), entry->d_name, &metadata,
                    AT_SYMLINK_NOFOLLOW) == -1) {
            failure = errno;
            break;
        }
        if (!S_ISREG(metadata.st_mode)) {
            continue;
        }
        if (candidates == UINT64_MAX) {
            failure = EOVERFLOW;
            break;
        }
        candidates++;
        if (uniform_below(candidates, &pick) == -1) {
            failure = errno;
            break;
        }
        if (pick == 0) {
            int candidate = openat(dirfd(directory), entry->d_name,
                                   O_RDONLY | O_CLOEXEC | O_NOFOLLOW);

            if (candidate == -1) {
                failure = errno;
                break;
            }
            if (fstat(candidate, &metadata) == -1) {
                failure = errno;
                (void)close(candidate);
                break;
            }
            if (!S_ISREG(metadata.st_mode)) {
                failure = EINVAL;
                (void)close(candidate);
                break;
            }
            if (selected != -1) {
                (void)close(selected);
            }
            selected = candidate;
        }
    }
    if (closedir(directory) == -1 && failure == 0) {
        failure = errno;
    }
    if (failure == 0 && candidates == 0) {
        failure = ENOENT;
    }
    if (failure != 0) {
        if (selected != -1) {
            (void)close(selected);
        }
        errno = failure;
        return -1;
    }
    return selected;
}

static int copy_source(int source, int destination)
{
    unsigned char buffer[16384];
    uint32_t total = 0;
    struct stat metadata;

    if (fstat(source, &metadata) == -1) {
        return -1;
    }
    if (metadata.st_size < 0 || metadata.st_size > (off_t)T4_MAX_RESPONSE) {
        errno = EFBIG;
        return -1;
    }
    for (;;) {
        ssize_t count = read(source, buffer, sizeof(buffer));
        size_t done = 0;

        if (count == -1) {
            return -1;
        }
        if (count == 0) {
            return 0;
        }
        if ((uint32_t)count > T4_MAX_RESPONSE - total) {
            errno = EFBIG;
            return -1;
        }
        total += (uint32_t)count;
        while (done < (size_t)count) {
            ssize_t written = write(destination, buffer + done,
                                    (size_t)count - done);

            if (written <= 0) {
                if (written == 0) {
                    errno = EIO;
                }
                return -1;
            }
            done += (size_t)written;
        }
    }
}

static int corpus_prepare(const char *corpus_dir, const char *turn_dir)
{
    int source;
    int directory;
    int destination;
    int saved_error;
    int result;
    char temporary[48];
    uint64_t random_suffix;

    if (corpus_dir == NULL || turn_dir == NULL) {
        errno = EINVAL;
        return -1;
    }
    source = select_source(corpus_dir);
    if (source == -1) {
        return -1;
    }
    directory = open(turn_dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory == -1) {
        saved_error = errno;
        (void)close(source);
        errno = saved_error;
        return -1;
    }
    if (random_bytes(&random_suffix, sizeof(random_suffix)) == -1) {
        saved_error = errno;
        (void)close(directory);
        (void)close(source);
        errno = saved_error;
        return -1;
    }
    (void)snprintf(temporary, sizeof(temporary), "source.tmp.%016llx",
                   (unsigned long long)random_suffix);
    destination = openat(directory, temporary,
                         O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                         0600);
    if (destination == -1) {
        saved_error = errno;
        (void)close(directory);
        (void)close(source);
        errno = saved_error;
        return -1;
    }
    result = copy_source(source, destination);
    saved_error = errno;
    if (result == 0 && fsync(destination) == -1) {
        result = -1;
        saved_error = errno;
    }
    if (close(destination) == -1 && result == 0) {
        result = -1;
        saved_error = errno;
    }
    (void)close(source);
    if (result == 0
        && renameat(directory, temporary, directory, "source.txt") == -1) {
        result = -1;
        saved_error = errno;
    }
    if (result == 0 && fsync(directory) == -1) {
        result = -1;
        saved_error = errno;
    }
    if (result == -1) {
        (void)unlinkat(directory, temporary, 0);
    }
    (void)close(directory);
    errno = saved_error;
    return result;
}

static int join_path(char *buffer, size_t capacity, const char *directory,
                     const char *name)
{
    int length = snprintf(buffer, capacity, "%s/%s", directory, name);

    if (length < 0 || (size_t)length >= capacity) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

static void restore_child_signals(void)
{
    const int signals[] = { SIGTERM, SIGINT, SIGHUP, SIGPIPE };
    struct sigaction action;
    sigset_t mask;
    size_t index;

    memset(&action, 0, sizeof(action));
    action.sa_handler = SIG_DFL;
    (void)sigemptyset(&action.sa_mask);
    for (index = 0; index < sizeof(signals) / sizeof(signals[0]); index++) {
        if (sigaction(signals[index], &action, NULL) == -1) {
            _exit(126);
        }
    }
    (void)sigemptyset(&mask);
    if (sigprocmask(SIG_SETMASK, &mask, NULL) == -1) {
        _exit(126);
    }
}

static int corpus_start(const char *generator, const char *turn_dir,
                        const char *context_path, uint32_t offset, uint32_t rate,
                        struct t4_generation *generation)
{
    char source[PATH_MAX];
    char prompt[PATH_MAX];
    char log_path[PATH_MAX];
    char offset_text[11];
    char rate_text[11];
    int pipe_fds[2];
    int log_fd;
    int flags;
    int saved_error;
    pid_t child;
    pid_t parent;

    if (generator == NULL || *generator == '\0' || turn_dir == NULL
        || context_path == NULL || generation == NULL
        || offset > T4_MAX_RESPONSE || rate > 10000U) {
        errno = EINVAL;
        return -1;
    }
    generation->pid = -1;
    generation->fd = -1;
    if (join_path(source, sizeof(source), turn_dir, "source.txt") == -1
        || join_path(prompt, sizeof(prompt), turn_dir, "prompt.txt") == -1
        || join_path(log_path, sizeof(log_path), turn_dir, "generator.log") == -1) {
        return -1;
    }
    (void)snprintf(offset_text, sizeof(offset_text), "%u", (unsigned int)offset);
    (void)snprintf(rate_text, sizeof(rate_text), "%u", (unsigned int)rate);
    if (pipe2(pipe_fds, O_CLOEXEC) == -1) {
        return -1;
    }
    flags = fcntl(pipe_fds[0], F_GETFL);
    if (flags == -1 || fcntl(pipe_fds[0], F_SETFL, flags | O_NONBLOCK) == -1) {
        saved_error = errno;
        (void)close(pipe_fds[0]);
        (void)close(pipe_fds[1]);
        errno = saved_error;
        return -1;
    }
    log_fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW,
                  0600);
    if (log_fd == -1) {
        saved_error = errno;
        (void)close(pipe_fds[0]);
        (void)close(pipe_fds[1]);
        errno = saved_error;
        return -1;
    }
    parent = getpid();
    child = fork();
    if (child == -1) {
        saved_error = errno;
        (void)close(log_fd);
        (void)close(pipe_fds[0]);
        (void)close(pipe_fds[1]);
        errno = saved_error;
        return -1;
    }
    if (child == 0) {
        int null_input;

        restore_child_signals();
        /* A crashed worker must not leave its generator running in the background. */
        if (prctl(PR_SET_PDEATHSIG, SIGTERM) == -1 || getppid() != parent) {
            _exit(126);
        }
        (void)close(pipe_fds[0]);
        null_input = open("/dev/null", O_RDONLY);
        if (null_input == -1 || dup2(null_input, STDIN_FILENO) == -1
            || dup2(pipe_fds[1], STDOUT_FILENO) == -1
            || dup2(log_fd, STDERR_FILENO) == -1
            || fcntl(STDIN_FILENO, F_SETFD, 0) == -1
            || fcntl(STDOUT_FILENO, F_SETFD, 0) == -1
            || fcntl(STDERR_FILENO, F_SETFD, 0) == -1) {
            _exit(126);
        }
        if (null_input != STDIN_FILENO) {
            (void)close(null_input);
        }
        if (pipe_fds[1] != STDOUT_FILENO) {
            (void)close(pipe_fds[1]);
        }
        if (log_fd != STDERR_FILENO) {
            (void)close(log_fd);
        }
        execlp(generator, generator, "--source", source, "--prompt", prompt,
               "--context", context_path, "--offset", offset_text,
               "--rate", rate_text, (char *)NULL);
        (void)dprintf(STDERR_FILENO, "exec generator: %s\n", strerror(errno));
        _exit(127);
    }
    (void)close(log_fd);
    (void)close(pipe_fds[1]);
    generation->pid = child;
    generation->fd = pipe_fds[0];
    return 0;
}

const struct t4_backend t4_corpus_backend = {
    .name = "corpus",
    .prepare = corpus_prepare,
    .start = corpus_start
};

void t4_generation_stop(struct t4_generation *generation)
{
    int saved_error = errno;

    if (generation == NULL) {
        return;
    }
    if (generation->fd >= 0) {
        (void)close(generation->fd);
        generation->fd = -1;
    }
    if (generation->pid > 0) {
        pid_t reaped;

        (void)kill(generation->pid, SIGTERM);
        do {
            reaped = waitpid(generation->pid, NULL, WNOHANG);
        } while (reaped == -1 && errno == EINTR);
        if (reaped == 0) {
            /* Cancellation must stay bounded even for a future misbehaving backend. */
            (void)kill(generation->pid, SIGKILL);
            while (waitpid(generation->pid, NULL, 0) == -1 && errno == EINTR) {
                /* Reap even when a second shutdown signal interrupts waitpid. */
            }
        }
    }
    generation->pid = -1;
    errno = saved_error;
}
