#include "ipc.h"

#include <errno.h>
#include <stdio.h>
#include <sys/sem.h>
#include <time.h>

volatile sig_atomic_t ipc_interrupted = 0;

static void handle_signal(int signo)
{
    /* 不在异步信号处理函数中输出或删除 IPC 对象。 */
    ipc_interrupted = signo;
}

int ipc_setup(void)
{
    struct sigaction action = {0};

    if (setvbuf(stdout, NULL, _IOLBF, 0) != 0) {
        fprintf(stderr, "Cannot enable stdout line buffering.\n");
        return -1;
    }
    action.sa_handler = handle_signal;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) == -1 ||
        sigaction(SIGTERM, &action, NULL) == -1) {
        perror("sigaction");
        return -1;
    }
    return 0;
}

int ipc_make_key(const char *path, key_t *key)
{
    /* 两端使用同一现有路径；共享内存和信号量分属不同的 IPC 名字空间。 */
    *key = ftok(path, '2');
    if (*key == (key_t)-1) {
        perror("ftok");
        return -1;
    }
    return 0;
}

int ipc_wait(int semid, unsigned short index)
{
    struct sembuf operation = {index, -1, 0};

    for (;;) {
        struct timespec timeout = {1, 0};
        if (ipc_interrupted) {
            errno = EINTR;
            return -1;
        }
        /* P 操作：由内核阻塞等待。超时仅用于定期检查退出信号，
         * 不用于决定数据是否就绪，也不靠 sleep 或 GETVAL 忙等。 */
        if (semtimedop(semid, &operation, 1, &timeout) == 0) {
            return 0;
        }
        if (errno != EINTR && errno != EAGAIN) {
            return -1;
        }
    }
}

int ipc_post(int semid, unsigned short index)
{
    struct sembuf operation = {index, 1, 0};
    int result;

    /* V 操作：发布刚写好的请求、回复或最后的退出确认。 */
    do {
        result = semop(semid, &operation, 1);
    } while (result == -1 && errno == EINTR && !ipc_interrupted);
    return result;
}
