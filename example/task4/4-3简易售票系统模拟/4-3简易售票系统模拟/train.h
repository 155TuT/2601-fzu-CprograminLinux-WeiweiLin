#include <sys/types.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <stdio.h>
#include <netinet/in.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <signal.h>
#include <time.h>
#include <string.h>
#include <arpa/inet.h>

#define ALL 1
#define ONEID 2
#define ONEPLACE 3
#define BUY 4
#define BUY_SUCCEED 5
#define BUY_FAILED 6
#define RETURN 7
#define RETURN_SUCCEED 8
#define RETURN_FAILED 9
#define TRAIN_NUM 16
#define NUMS 100
struct stMessage
{
        unsigned int msg_type;//结构体的具体类型，其实就是写上面define的东西
        unsigned int train_ID;
        unsigned int ticket_num;
        unsigned int ticket_total_price;
	unsigned int time_sy;//起飞的时间年月日
	unsigned int time_smo;
	unsigned int time_sd;
	unsigned int time_sh;
	unsigned int time_sm;
	 unsigned int time_dy;//抵达的时间年月日
        unsigned int time_dmo;
        unsigned int time_dd;
        unsigned int time_dh;
        unsigned int time_dm;
	unsigned int gate;
	int buyb, retub;
	char start_p[15];
	char dest_p[15];
}message;

int sem_id;

long int price[17]={0, 1000, 1100, 1200, 1300, 1400, 1500, 1600, 1700, 1800, 1900, 2000, 2100, 2200, 2300, 2400, 2500};

char start[4][15]={"Beijing", "Shanghai", "Nanjing", "Fuzhou"};//起点

char dest[4][15]={"Hangzhou", "Kunming", "Chengdu", "Guangzhou"};//终点

int shour[16]={3, 5, 6, 7, 9, 10, 11, 12, 13, 15, 17, 18, 19, 20, 21, 24};//小时
int dhour[16]={4, 6, 7, 9, 10, 12, 13, 14, 15, 16, 18, 20, 21, 22, 23, 3};
int smin[16]={0, 15, 20, 27, 33, 54, 48, 35, 21, 57, 15, 45, 9, 27, 18, 50};//分钟
int dmin[16]={10, 22, 37, 9, 56, 5, 39, 25, 33, 42, 16, 18, 27, 44, 36, 55}; 

union semun//信号量结构体
{
        int val;
        struct semid_ds *buf;
        unsigned short *array;
};

static int semaphore_p(int *tmp)
{
        struct sembuf sem_b;

        sem_b.sem_num=0;
        sem_b.sem_op=-1;//减一标志位
        sem_b.sem_flg=SEM_UNDO;
        if(semop(*tmp, &sem_b, 1)==-1)//系统调用，实现减一
        {
                printf("semaphore_p failed\n");
                return 0;
        }
        return 1;
}


static int semaphore_v(int *tmp)
{
        struct sembuf sem_b;

        sem_b.sem_num=0;
        sem_b.sem_op=1;//加一标志位
	 sem_b.sem_flg=SEM_UNDO;
        if(semop(*tmp, &sem_b, 1)==-1)//系统调用，实现加一
        {
                printf("semaphore_v failed\n");
                return 0;
        }
        return 1;
}
