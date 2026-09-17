#include "ipc.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <unistd.h>

int main(int argc, char *argv[])
{
    const char *key_path = argc == 2 ? argv[1] : ".";
    key_t key;
    int shmid = -1, semid = -1, result = 1;
    struct shared_data *data = (void *)-1;
    unsigned short initial_values[SEM_COUNT] = {1, 0, 0};
    union semun options = {.array = initial_values};

    if (argc > 2) {
        fprintf(stderr, "Usage: %s [existing-key-path]\n", argv[0]);
        return 2;
    }
    if (ipc_setup() == -1 || ipc_make_key(key_path, &key) == -1) {
        return 1;
    }
    /* 独占创建，拒绝覆盖另一服务器或异常退出后遗留的资源。 */
    shmid = shmget(key, sizeof(*data), IPC_CREAT | IPC_EXCL | 0600);
    if (shmid == -1) {
        perror("shmget (exclusive create)");
        return 1;
    }
    data = shmat(shmid, NULL, 0);
    if (data == (void *)-1) {
        perror("shmat");
        goto cleanup;
    }
    semid = semget(key, SEM_COUNT, IPC_CREAT | IPC_EXCL | 0600);
    if (semid == -1) {
        perror("semget (exclusive create)");
        goto cleanup;
    }
    memset(data, 0, sizeof(*data));
    data->magic = IPC_MAGIC;
    /* 初始化内存后才开放会话入口，客户端不会读到尚未初始化的数据。 */
    if (semctl(semid, 0, SETALL, options) == -1) {
        perror("semctl (SETALL)");
        goto cleanup;
    }
    printf("[server] pid=%ld, key=0x%08lx, shmid=%d, semid=%d\n",
           (long)getpid(), (unsigned long)(unsigned int)key, shmid, semid);
    printf("[server] ready; waiting for one client session.\n");

    for (;;) {
        if (ipc_wait(semid, SEM_REQUEST) == -1) {
            perror("wait for request");
            goto cleanup;
        }
        if (data->request_type == REQUEST_QUIT) {
            strcpy(data->response, "bye");
            printf("[server] shutdown requested; send reply: bye\n");
            if (ipc_post(semid, SEM_RESPONSE) == -1 ||
                ipc_wait(semid, SEM_REQUEST) == -1) {
                perror("shutdown handshake");
                goto cleanup;
            }
            /* 收到最终确认后再删除信号量，避免客户端尚未取得回复。 */
            printf("[server] client acknowledged the final reply.\n");
            result = 0;
            break;
        }
        size_t length = strnlen(data->request, TEXT_CAPACITY);
        if (data->request_type != REQUEST_TEXT || length == TEXT_CAPACITY) {
            fprintf(stderr, "Invalid request in shared memory.\n");
            goto cleanup;
        }
        ++data->sequence;
        for (size_t i = 0; i < length; ++i) {
            unsigned char c = (unsigned char)data->request[i];
            /* 只转换 ASCII 小写字母，保留 UTF-8 等其他字节。 */
            data->response[i] = (char)(c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c);
        }
        data->response[length] = '\0';
        printf("[server] request #%u (%zu bytes): %s\n",
               data->sequence, length, data->request);
        printf("[server] response #%u: %s\n", data->sequence, data->response);
        if (ipc_post(semid, SEM_RESPONSE) == -1) {
            perror("post response");
            goto cleanup;
        }
    }

cleanup:
    if (ipc_interrupted) {
        printf("[server] interrupted by signal=%d\n", (int)ipc_interrupted);
        result = 128 + ipc_interrupted;
    }
    /* 只清理由本次运行创建成功的对象；所有退出路径汇聚于此。 */
    if (semid != -1 && semctl(semid, 0, IPC_RMID) == -1) {
        perror("semctl (IPC_RMID)");
        result = 1;
    }
    if (shmid != -1 && shmctl(shmid, IPC_RMID, NULL) == -1) {
        perror("shmctl (IPC_RMID)");
        result = 1;
    }
    if (data != (void *)-1 && shmdt(data) == -1) {
        perror("shmdt");
        result = 1;
    }
    printf("[server] cleanup finished.\n");
    return result;
}
