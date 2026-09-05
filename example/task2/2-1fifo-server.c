/*使用命名管道FIFO机制实现客户到服务器之间传递数据的操作。多客户-单一服务器模式*/
/*命名管道文件需创建在Linux文件系统内*/
/*在一个终端窗口中运行fifo-server程序，然后在另外一个终端窗口运行fifo-client程序*/

/*fifo-server.c */

#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include <linux/stat.h>

#define FIFO_FILE "/tmp/MYFIFO" /*命名管道的路径、文件名*/

int main()
{
	FILE *fp;
	char readbuf[80];

	if((fp=fopen(FIFO_FILE,"r"))==NULL)/*如果命名管道文件不存在，要先创建一个*/
	{
		umask(0);//清除文件创建时权限位的屏蔽作用
		mknod(FIFO_FILE,S_IFIFO|0666,0);//创建FIFO文件
		printf("create new fifo successed. \n");
	}
	else
 		fclose(fp);

	while(1)
	{
		if((fp=fopen(FIFO_FILE,"r"))==NULL)/*打开命名管道文件*/	
 		{
			printf("open fifo failed. \n");
			exit(1);
		}

		if(fgets(readbuf,80,fp)!=NULL)/*从命名管道文件中读数据*/
		{
			printf("Received string :%s \n", readbuf);
			fclose(fp);
		}
		else
		{
			if(ferror(fp))
			{
				printf("read fifo failed.\n");
				exit(1);
			}
		}
	}
	return 0;
}
