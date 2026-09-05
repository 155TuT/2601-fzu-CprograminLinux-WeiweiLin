/*共享内存机制可以直接读取内存，所以其通信效率高于管道和消息队列*/
/*由于多个进程对同一块内存区域具有访问的权限，进程之间的同步就非常重要*/
/*使用信号量机制PV操作来同步的共享内存机制通信*/
/*在一个终端窗口中先运行semshm-write程序，然后在另外一个终端窗口运行semshm-read程序*/

/*semshm-read.c */

#include "2-4semshm-.h"

int main(int argc, char** argv)
{
    int semid, shmid;
    char *shmaddr;
    if((shmid = creatshm(".", 57, SHM_SIZE)) == -1)
        return -1;
    if((shmaddr = shmat(shmid, (char*)0, 0)) == (char *)-1){
        perror("attch shared memory error!\n");
        exit(1);
    }
    if((semid = opensem("./", 39)) == -1)
        return -1;
    printf("read start....................\n");        
    while(1){
        printf("read : ");
        wait_sem(semid, 0);  //等待信号量可以获取
        if(sem_p(semid, 0) == -1) //获取信号量失败退出。当server写入'#'时引发
            break;
        printf("%s", shmaddr);

        sem_v(semid, 0);
        usleep(1000);
    }    
    return 0;
}
