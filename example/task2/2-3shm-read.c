/*使用共享内存机制实现通信*/
/*在一个终端窗口中先运行shm-write程序，然后在另外一个终端窗口运行shm-read程序*/

/*shm-read.c */

#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


int main()
{
	
	int 	shmid;
	char 	c;
	char	*shmptr, *s;
	if((shmid=shmget(1234,256, 0666))<0)//打开key值为1234的共享内存
	{
		printf("shmget failed.\n");
		exit(1);
	}
	if((shmptr=shmat(shmid,0,0))==(char*)-1)//附加此共享内存至自己的地址空间
	{
		shmctl(shmid,IPC_RMID,(struct shmid_ds*)shmptr);	
		printf("shmat failed.\n");
		exit(2);
	}
	for(s=shmptr;*s!='\0';s++)//读出26个字母
		putchar(*s);
	printf("\n");
	*shmptr='*';//写入“*”到共享内存表示读完数据
	return 0;
}
