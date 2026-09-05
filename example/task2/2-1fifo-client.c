/*使用命名管道FIFO机制实现客户到服务器之间传递数据的操作。多客户-单一服务器模式*/
/*命名管道文件需创建在Linux文件系统内*/
/*在一个终端窗口中运行fifo-server程序，然后在另外一个终端窗口运行fifo-client程序*/

/*fifo-client.c */

#include <stdio.h>
#include <stdlib.h>

#define FIFO_FILE "/tmp/MYFIFO"/*命名管道的路径、文件名*/

int main(int argc, char *argv[])
{
	FILE *fp;
	int i;
	if(argc<=1)
	{
		printf("usage: %s <message>\n",argv[0]);
		exit(1);
	}
 
	if((fp=fopen(FIFO_FILE,"w"))==NULL)/*打开命名管道文件*/	
	{
		printf("open fifo failed. \n");
		exit(1);
	}

	for(i=1;i<argc;i++)
	{
		if(fputs(argv[i],fp)==EOF)
		{
			printf("write fifo error. \n");
			exit(1);
		}
		if(fputs(" ",fp)==EOF)
		{
			printf("write fifo error. \n");
			exit(1);
		}
	}
	fclose(fp);
	return 0;
	 
}

