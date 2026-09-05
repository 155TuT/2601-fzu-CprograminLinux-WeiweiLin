/*共享内存机制可以直接读取内存，所以其通信效率高于管道和消息队列*/
/*由于多个进程对同一块内存区域具有访问的权限，进程之间的同步就非常重要*/
/*使用信号量机制PV操作来同步的共享内存机制通信*/
/*在一个终端窗口中先运行semshm-write程序，然后在另外一个终端窗口运行semshm-read程序*/

/*semshm-write.c */

#include "2-4semshm-.h"

int main(int argc, char** argv)
{
    int semid, shmid;
    char *shmaddr;
    char write_str[SHM_SIZE];
    char *ret;
    if((shmid = creatshm(".", 57, SHM_SIZE)) == -1) //创建或者获取共享内存
        return -1;
/*建立进程和共享内存连接*/
    if((shmaddr = shmat(shmid, (char*)0, 0)) == (char *)-1){
        perror("attch shared memory error!\n");
        exit(1);
    }    
    if((semid = creatsem("./", 39, 1, 1)) == -1)//创建信号量
        return -1;
    while(1){
        wait_sem(semid, 0);//等待信号量可以被获取
        sem_p(semid, 0);  //获取信号量
/***************写共享内存***************************************************/
        printf("write : ");
        ret = fgets(write_str, 1024, stdin);
        if(write_str[0] == '#') // '#'结束读写进程
            break;
        int len = strlen(write_str);
        write_str[len] = '\0';
        strcpy(shmaddr, write_str);
/****************************************************************************/
        sem_v(semid, 0); //释放信号量
        usleep(1000);  //本进程睡眠.
    }
    sem_delete(semid); //把semid指定的信号集从系统中删除
    deleteshm(shmid);   //从系统中删除shmid标识的共享内存
    return 0;
}
