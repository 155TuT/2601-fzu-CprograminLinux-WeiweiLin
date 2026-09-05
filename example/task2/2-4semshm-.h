/*共享内存机制可以直接读取内存，所以其通信效率高于管道和消息队列*/
/*由于多个进程对同一块内存区域具有访问的权限，进程之间的同步就非常重要*/
/*使用信号量机制PV操作来同步的共享内存机制通信*/
/*在一个终端窗口中先运行semshm-write程序，然后在另外一个终端窗口运行semshm-read程序*/

/*semshm-.h */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <error.h>

#define SHM_SIZE     1024

union semun{
    int val;
    struct semid_ds *buf;
    unsigned short *array;
    struct seminfo *buf_info;
    void *pad;
};

/* 创建信号量函数*/
int creatsem(const char *pathname, int proj_id, int members, int init_val)
{
    key_t msgkey;
    int index, sid;
    union semun semopts;
    
    if((msgkey = ftok(pathname, proj_id)) == -1){//利用ftok函数生成键值，自行指定的键值可能会冲突
        perror("ftok error!\n");
        return -1;
    }
    if((sid = semget(msgkey, members, IPC_CREAT|0666)) == -1){//打开键值为msgkey的信号量集，如不存在则创建之，返回信号量集标识符。members为信号量集中含信号量的数目。
        perror("semget call failed.\n");
        return -1;
    }
    semopts.val = init_val;
    for(index = 0; index < members; index++){
        semctl(sid, index, SETVAL, semopts);
    }
    
    return sid;
}

int opensem(const char *pathname, int proj_id)
{
    key_t msgkey;
    int sid;
    
    if((msgkey = ftok(pathname, proj_id)) == -1){
        perror("ftok error!\n");
        return -1;
    }
    
    if((sid = semget(msgkey, 0, 0666)) == -1){
        perror("open semget call failed.\n");
        return -1;
    }
    return sid;
}

/* p操作, 获取信号量*/
int sem_p(int semid, int index)
{
    struct sembuf sbuf = {0, -1, IPC_NOWAIT};//每个sembuf结构描述了一个对信号量的操作
    if(index < 0){
        perror("index of array cannot equals a minus value!\n");
        return -1;
    }
    sbuf.sem_num = index;
    if(semop(semid, &sbuf, 1) == -1){
        perror("A wrong operation to semaphore occurred!\n");
        return -1;
    }
    return 0;
}

/* V操作, 释放信号量*/
int sem_v(int semid, int index)
{
    struct sembuf sbuf = {0, 1, IPC_NOWAIT};//每个sembuf结构描述了一个对信号量的操作
    if(index < 0){
        perror("index of array cannot equals a minus value!\n");
        return -1;
    }
    sbuf.sem_num = index;
    if(semop(semid, &sbuf, 1) == -1){
        perror("A wrong operation to semaphore occurred!\n");
        return -1;
    }
    return 0;
}

/* 删除信号量*/
int sem_delete(int semid)
{
    return (semctl(semid, 0, IPC_RMID));
}

/* 等待信号量*/
int wait_sem(int semid, int index)
{
    while(semctl(semid, index, GETVAL, 0) == 0)
    {
        usleep(500);
    } 
    return 1;

}

/* 创建共享内存*/
int creatshm(char *pathname, int proj_id, size_t size)
{
    key_t shmkey;
    int sid;
    
    if((shmkey = ftok(pathname, proj_id)) == -1){
        perror("ftok error!\n");
        return -1;
    }
    if((sid = shmget(shmkey, size, IPC_CREAT|0666)) == -1){
        perror("shm call failed!\n");
        return -1;
    }
    return sid;
}

/* 删除共享内存*/
int deleteshm(int sid)
{
    void *p = NULL;
    return (shmctl(sid, IPC_RMID, p));
}
