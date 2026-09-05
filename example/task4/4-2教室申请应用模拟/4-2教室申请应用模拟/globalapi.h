#ifndef __GLOBALAPI_H
#define __GLOBALAPI_H

#include <sys/types.h> 
#include <sys/socket.h> 
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/un.h>
#include <string.h>
#include <pthread.h>

/*服务器端使用的端口*/
#define SERVER_PORT_NO 8889

/*客户端与服务器端使用的消息类型定义*/
#define INITIAL_VALUE 65535

/*客户端使用的消息代码含义*/
#define DISCONNECT 0
#define APPLY_ROOM  1
#define INQUIRE_ONE 2
#define INQUIRE_ALL 3

/*服务器端使用的消息代码含义*/
#define APPLY_SUCCEED 255
#define APPLY_FAILED 256
#define INQUIRE_SUCCEED 257
#define UNKNOWN_CODE 258

#define TRUE 1
#define FALSE 0
/*服务器与客户端使用的消息结构定义*/
struct stMessage {
//消息类型。客户端可以取值为DISCONNECT：断开连接，BUY_TICKET：申请教室， INQUIRE_ONE：查询某栋楼剩余教室， INQUIRE_ALL：查询剩余教室
unsigned int  msg_type;
//楼号
unsigned int room_id;
//教室间数
unsigned int room_num;

//unsigned int ticket_total_price;
} message;

void init_message()
{
	message.msg_type=INITIAL_VALUE;
	message.room_id=0;
	message.room_num=0;
	
}

//楼号转换

char * change_id(int room_id){
	char *changed_id;
	switch(room_id){
	case 1:changed_id="西三";break;
	case 2:changed_id="西二";break;
	case 3:changed_id="西一";break;
	case 4:changed_id="东一";break;
	case 5:changed_id="东二";break;
	case 6:changed_id="东三";break;
	default:break;
	}
	return changed_id;
}

/*服务器端的线程缓冲区的最大数量*/
#define THREAD_BUFF_NUM 128

//输出消息
void add_info(char *src)
{ 
	printf("%s\n",src);
}

#endif

int re_connect();


