#include "ipc.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <unistd.h>

/* 返回 1 表示一行，0 表示 EOF，2 表示过长，-1 表示读取失败。 */
static int read_line(char text[TEXT_CAPACITY])
{
    if (fgets(text, TEXT_CAPACITY, stdin) == NULL) {
        return ferror(stdin) ? -1 : 0;
    }
    size_t length = strlen(text);
    if (length > 0 && text[length - 1] == '\n') {
        text[--length] = '\0';
    } else {
        /* 多读一个字符：区分恰好 255 字节与超过容量的输入。 */
        int c = getchar();
        if (c != '\n' && c != EOF) {
            do {
                c = getchar();
            } while (c != '\n' && c != EOF);
            return ferror(stdin) ? -1 : 2;
        }
        if (ferror(stdin)) {
            return -1;
        }
    }
    return 1;
}

int main(int argc, char *argv[])
{
    const char *key_path = argc == 2 ? argv[1] : ".";
    key_t key;
    int result = 1;
    struct shared_data *data = (void *)-1;
    struct shmid_ds info;
    struct sembuf claim = {SEM_SESSION, -1, IPC_NOWAIT};

    if (argc > 2) {
        fprintf(stderr, "Usage: %s [existing-key-path]\n", argv[0]);
        return 2;
    }
    if (ipc_setup() == -1 || ipc_make_key(key_path, &key) == -1) {
        return 1;
    }
    /* 客户端只打开现有对象，不能先于服务器创建未初始化的资源。 */
    int shmid = shmget(key, sizeof(*data), 0600);
    if (shmid == -1) {
        perror("shmget (start the server first)");
        return 1;
    }
    int semid = semget(key, SEM_COUNT, 0600);
    if (semid == -1) {
        perror("semget (start the server first)");
        return 1;
    }
    if (shmctl(shmid, IPC_STAT, &info) == -1) {
        perror("shmctl (IPC_STAT)");
        return 1;
    }
    if (info.shm_segsz != sizeof(*data)) {
        fprintf(stderr, "Shared memory size does not match this program.\n");
        return 1;
    }
    data = shmat(shmid, NULL, 0);
    if (data == (void *)-1) {
        perror("shmat");
        return 1;
    }
    /* 一次性占用会话，拒绝第二客户端，防止请求和回复互相覆盖。
     * 不用 SEM_UNDO 自动重开会话：异常中断后应重启服务器。 */
    if (semop(semid, &claim, 1) == -1) {
        if (errno == EAGAIN) {
            fprintf(stderr, "Server is starting or its client session is already claimed.\n");
        } else {
            perror("claim client session");
        }
        goto cleanup;
    }
    if (data->magic != IPC_MAGIC) {
        fprintf(stderr, "Shared memory protocol does not match this program.\n");
        goto cleanup;
    }
    printf("[client] pid=%ld, connected: shmid=%d, semid=%d\n",
           (long)getpid(), shmid, semid);
    printf("[client] enter text (max 255 bytes); quit or EOF ends the session.\n");

    for (;;) {
        char text[TEXT_CAPACITY];
        if (ipc_interrupted) {
            goto cleanup;
        }
        if (isatty(STDIN_FILENO)) {
            printf("input> ");
            fflush(stdout);
        }
        int read_status = read_line(text);
        if (read_status == -1) {
            perror("read stdin");
            goto cleanup;
        }
        if (read_status == 2) {
            fprintf(stderr, "Input too long: max 255 bytes; line discarded.\n");
            continue;
        }
        int quitting = read_status == 0 || strcmp(text, "quit") == 0;
        if (read_status == 0) {
            printf("[client] EOF -> request shutdown.\n");
        }
        data->request_type = quitting ? REQUEST_QUIT : REQUEST_TEXT;
        strcpy(data->request, quitting ? "quit" : text);
        printf("[client] send: %s\n", data->request);
        if (ipc_post(semid, SEM_REQUEST) == -1 ||
            ipc_wait(semid, SEM_RESPONSE) == -1) {
            perror("request/reply");
            goto cleanup;
        }
        if (strnlen(data->response, TEXT_CAPACITY) == TEXT_CAPACITY) {
            fprintf(stderr, "Invalid response in shared memory.\n");
            goto cleanup;
        }
        if (quitting) {
            printf("[client] final reply: %s\n", data->response);
        } else {
            printf("[client] reply #%u: %s\n", data->sequence, data->response);
        }
        if (quitting) {
            /* 回复已复制到输出后才确认；此后不再访问共享内存数据。 */
            if (ipc_post(semid, SEM_REQUEST) == -1) {
                perror("acknowledge shutdown");
                goto cleanup;
            }
            result = 0;
            break;
        }
    }

cleanup:
    if (ipc_interrupted) {
        printf("[client] interrupted by signal=%d; stop the server to clean up.\n",
               (int)ipc_interrupted);
        result = 128 + ipc_interrupted;
    }
    if (shmdt(data) == -1) {
        perror("shmdt");
        result = 1;
    }
    return result;
}
