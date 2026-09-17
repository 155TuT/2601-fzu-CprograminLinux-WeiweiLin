#ifndef TASK2_IPC_H
#define TASK2_IPC_H

#include <signal.h>
#include <sys/ipc.h>

#define TEXT_CAPACITY 256
#define IPC_MAGIC 0x49504332U

/* 每次服务器运行只接受一个客户端会话；一个会话可以多次收发。 */
enum { SEM_SESSION, SEM_REQUEST, SEM_RESPONSE, SEM_COUNT };
enum { REQUEST_TEXT = 1, REQUEST_QUIT };

struct shared_data {
    unsigned int magic;
    unsigned int sequence;
    int request_type;
    char request[TEXT_CAPACITY];
    char response[TEXT_CAPACITY];
};

/* glibc 要求应用程序自行定义 semctl 第四个参数所用的联合体。 */
union semun {
    int val;
    struct semid_ds *buf;
    unsigned short *array;
};

extern volatile sig_atomic_t ipc_interrupted;

int ipc_setup(void);
int ipc_make_key(const char *path, key_t *key);
int ipc_wait(int semid, unsigned short index);
int ipc_post(int semid, unsigned short index);

#endif
