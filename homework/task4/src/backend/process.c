#define _GNU_SOURCE
#include "task4/backend.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

/* The adapter has a bounded lifetime and owns only its stdio descriptors. */
int t4_generation_spawn(char *const argv[], const char *log_path,
                        struct t4_generation *generation)
{
    int pair[2], log = -1;
    generation->pid = generation->fd = -1;
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) < 0) return -1;
    int flags = fcntl(pair[0], F_GETFL);
    if (flags < 0 || fcntl(pair[0], F_SETFL, flags | O_NONBLOCK) < 0) goto fail;
    log = open(log_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (log < 0) goto fail;
    pid_t parent = getpid(), child = fork();
    if (child < 0) goto fail;
    if (!child) {
        struct sigaction action = {.sa_handler = SIG_DFL};
        sigset_t mask;
        sigemptyset(&action.sa_mask);
        const int signals[] = {SIGTERM, SIGINT, SIGHUP, SIGPIPE};
        for (unsigned i = 0; i < sizeof(signals) / sizeof(signals[0]); ++i)
            if (sigaction(signals[i], &action, NULL) < 0) _exit(126);
        sigemptyset(&mask);
        if (sigprocmask(SIG_SETMASK, &mask, NULL) < 0 ||
            prctl(PR_SET_PDEATHSIG, SIGTERM) < 0 || getppid() != parent) _exit(126);
        close(pair[0]);
        int null = open("/dev/null", O_RDONLY);
        if (null < 0 || dup2(null, STDIN_FILENO) < 0 ||
            dup2(pair[1], STDOUT_FILENO) < 0 || dup2(log, STDERR_FILENO) < 0 ||
            fcntl(STDIN_FILENO, F_SETFD, 0) < 0 ||
            fcntl(STDOUT_FILENO, F_SETFD, 0) < 0 ||
            fcntl(STDERR_FILENO, F_SETFD, 0) < 0) _exit(126);
        if (null > 2) close(null);
        if (pair[1] > 2) close(pair[1]);
        if (log > 2) close(log);
        execvp(argv[0], argv);
        dprintf(STDERR_FILENO, "exec generator: %s\n", strerror(errno));
        _exit(127);
    }
    close(pair[1]); close(log);
    generation->pid = child; generation->fd = pair[0];
    return 0;
fail:
    {
        int saved = errno;
        close(pair[0]); close(pair[1]);
        if (log >= 0) close(log);
        errno = saved;
        return -1;
    }
}

void t4_generation_stop(struct t4_generation *generation)
{
    int saved = errno;
    if (!generation) return;
    if (generation->fd >= 0) close(generation->fd);
    generation->fd = -1;
    if (generation->pid > 0) {
        kill(generation->pid, SIGTERM);
        pid_t reaped;
        do { reaped = waitpid(generation->pid, NULL, WNOHANG); } while (reaped < 0 && errno == EINTR);
        if (!reaped) {
            /* Cancellation remains bounded even for a misbehaving adapter. */
            kill(generation->pid, SIGKILL);
            while (waitpid(generation->pid, NULL, 0) < 0 && errno == EINTR) {}
        }
    }
    generation->pid = -1;
    errno = saved;
}
