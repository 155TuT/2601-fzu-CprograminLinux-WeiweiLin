#include "process.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

/* 信号处理函数只修改标志，避免在异步上下文中调用 printf。 */
static volatile sig_atomic_t notified = 0;
static volatile sig_atomic_t child_exited = 0;

static void handle_signal(int signo)
{
    if (signo == SIGUSR1) {
        notified = 1;
    } else if (signo == SIGCHLD) {
        child_exited = 1;
    }
}

int main(int argc, char *argv[])
{
    char *default_command[] = {"ls", "-l", NULL};
    char **command = argc > 1 ? &argv[1] : default_command;
    struct sigaction action = {0};
    sigset_t blocked, old_mask, wait_mask;
    pid_t parent_pid = getpid();
    pid_t child_pid;
    int status;

    /* fork 前启用行缓冲；每条完整记录在换行时刷新，便于保存日志。 */
    if (setvbuf(stdout, NULL, _IOLBF, 0) != 0) {
        fprintf(stderr, "Cannot enable stdout line buffering.\n");
        return 1;
    }
    action.sa_handler = handle_signal;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGUSR1, &action, NULL) == -1) {
        perror("sigaction (SIGUSR1)");
        return 1;
    }
    action.sa_flags = SA_NOCLDSTOP;
    if (sigaction(SIGCHLD, &action, NULL) == -1) {
        perror("sigaction (SIGCHLD)");
        return 1;
    }

    /* 先屏蔽再 fork，防止通知先于父进程开始等待而丢失。 */
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGUSR1);
    sigaddset(&blocked, SIGCHLD);
    if (sigprocmask(SIG_BLOCK, &blocked, &old_mask) == -1) {
        perror("sigprocmask");
        return 1;
    }
    printf("[parent] started, pid=%ld\n", (long)parent_pid);
    child_pid = fork();
    if (child_pid == -1) {
        perror("fork");
        sigprocmask(SIG_SETMASK, &old_mask, NULL);
        return 1;
    }
    if (child_pid == 0) {
        /* 子进程恢复信号掩码，避免把实验临时屏蔽的信号带入 exec。 */
        if (sigprocmask(SIG_SETMASK, &old_mask, NULL) == -1) {
            perror("sigprocmask (child)");
            _exit(1);
        }
        _exit(run_child(parent_pid, command));
    }

    printf("[parent] fork returned child pid=%ld\n", (long)child_pid);
    do_work("parent", 2);
    printf("[parent] waiting for SIGUSR1 or child exit\n");
    wait_mask = old_mask;
    sigdelset(&wait_mask, SIGUSR1);
    sigdelset(&wait_mask, SIGCHLD);
    while (!notified && !child_exited) {
        /* 原子地解除屏蔽并睡眠；被处理的信号唤醒后重新检查标志。 */
        sigsuspend(&wait_mask);
    }
    if (notified) {
        printf("[parent] received SIGUSR1: child is preparing to exec\n");
    } else {
        printf("[parent] received SIGCHLD before handling SIGUSR1\n");
    }

    /* 信号通知不等于回收进程；仍需 waitpid 获取终止状态。 */
    pid_t waited;
    do {
        waited = waitpid(child_pid, &status, 0);
    } while (waited == -1 && errno == EINTR);
    if (waited == -1) {
        perror("waitpid");
        sigprocmask(SIG_SETMASK, &old_mask, NULL);
        return 1;
    }
    if (sigprocmask(SIG_SETMASK, &old_mask, NULL) == -1) {
        perror("sigprocmask (parent)");
        return 1;
    }
    if (WIFEXITED(status)) {
        int exit_code = WEXITSTATUS(status);
        printf("[parent] reaped child pid=%ld, exit status=%d\n",
               (long)child_pid, exit_code);
        return exit_code;
    }
    if (WIFSIGNALED(status)) {
        int signo = WTERMSIG(status);
        printf("[parent] reaped child pid=%ld, terminated by signal=%d\n",
               (long)child_pid, signo);
        return 128 + signo;
    }
    return 1;
}
