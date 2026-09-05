/*使用消息队列机制实现发送接收消息的操作。*/
/*在一个终端窗口中运行msg-send程序，然后在另外一个终端窗口运行msg-recieve程序*/

/*msg-send.c */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/msg.h>

struct my_msg//消息的组成
{
	long int my_msg_type;//消息的类型域
	char text[BUFSIZ];//消息传递的数据域
} msgbuf;

int main()
{
	int running =1;
	int msgid;
	msgid=msgget((key_t)1234,0666 |IPC_CREAT);//打开key值为1234的消息队列，如不存在则创建之
	if(msgid==-1)
	{
		printf("msgget failed!\n");
		exit(1);
	}
	while(running)
	{
		printf("Enter some text: ");
		fgets(msgbuf.text,BUFSIZ,stdin);//读入键盘输入的消息
		msgbuf.my_msg_type=1;	
		if(msgsnd(msgid,(void *)&msgbuf, BUFSIZ, 0)==-1)//发送消息
		{
			printf("msgsnd failed!\n");
			exit(1);
		}
		if(strncmp(msgbuf.text,"end",3)==0)//输入end表示程序结束
			running=0;
	}
       
	return 0;
}

