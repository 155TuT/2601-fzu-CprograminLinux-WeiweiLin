/*使用共享内存机制实现通信*/
/*在一个终端窗口中先运行shm-write程序，然后在另外一个终端窗口运行shm-read程序*/

/*shm-write.c */

#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main()
{
	int 	shmid;
	char 	c;
	char 	*shmptr, *s;
	if((shmid=shmget(1234,256,IPC_CREAT | 0666))<0)//打开key值为1234的共享内存，如不存在则创建之
	{
		printf("shmget failed.\n");
		exit(1);
	}
	if((shmptr=shmat(shmid,0,0))==(char*)-1)//附加此共享内存至自己的地址空间，返回内存区域的指针
	{
		shmctl(shmid, IPC_RMID, (struct shmid_ds*)shmptr);		
		printf("shmat failed.\n");
		exit(2);
	}
	s=shmptr;// 写共享内存通过指针s操作
	for(c='a';c<='z';c++)//写入26个字母
		*s++=c;
	s='\0';
	while(*shmptr!='*')//等待直到读进程已写入“*”表示读完数据
		sleep(1);
	shmctl(shmid, IPC_RMID, (struct shmid_ds*)shmptr);//删除共享内存		
	return 0;
}
