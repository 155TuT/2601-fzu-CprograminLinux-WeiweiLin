#include "process.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>

void do_work(const char *role, int steps)
{
    for (int i = 1; i <= steps; ++i) {
        printf("[%s] pid=%ld, work step %d/%d\n",
               role, (long)getpid(), i, steps);
        sleep(1); /* 暂停一秒便于观察，不用于保证进程的执行顺序。 */
    }
}

int run_child(pid_t parent_pid, char *const command[])
{
    printf("[child] pid=%ld, ppid=%ld\n",
           (long)getpid(), (long)getppid());
    do_work("child", 3);

    /* 用符号常量表示信号，不依赖具体平台上的信号编号。 */
    printf("[child] send SIGUSR1 to parent pid=%ld\n", (long)parent_pid);
    if (kill(parent_pid, SIGUSR1) == -1) {
        perror("kill");
        return 1;
    }

    printf("[child] before execvp: pid=%ld, command=%s\n",
           (long)getpid(), command[0]);
    execvp(command[0], command);

    /* exec 成功会替换进程映像，因此只有失败时才执行这里。 */
    int saved_errno = errno;
    perror("execvp");
    return saved_errno == ENOENT ? 127 : 126;
}
