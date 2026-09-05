/*client.c*/

#include "globalapi.h"

int socket_fd;  //连接socket
struct sockaddr_in server; //服务器地址信息，客户端地址信息
int ret,i;
int flag=1;
char c;
int isconnected=FALSE; //是否已连接服务器

//消息内容输出函数
void display_info(char *msg)
{
	printf("%s\n",msg);
}

/*连接服务器操作*/
void connectserver()
{
	char msg[512]; //提示信息
	int i;

	if(!isconnected)
	{
	/*创建套接字*/
	socket_fd=socket(AF_INET,SOCK_STREAM,0);
	if(socket_fd<0) {
		sprintf(msg,"创建套接字出错！ \n");
		display_info(msg);
		return;
	} 
	/*设置接收、发送超时值*/
	struct timeval time_out;
	time_out.tv_sec=5;
	time_out.tv_usec=0;
	setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &time_out, sizeof(time_out));

	/*填写服务器的地址信息*/
	server.sin_family=AF_INET;
	server.sin_addr.s_addr=inet_addr("127.0.0.1");//htonl(INADDR_ANY);
	server.sin_port=htons(SERVER_PORT_NO);

	/*连接服务器*/
	ret=connect(socket_fd,(struct sockaddr*)&server, sizeof(server));
	if(ret<0) {
		sprintf(msg,"连接服务器 %d 出错！\n",SERVER_PORT_NO);
		display_info(msg);
		close(socket_fd);
		re_connect();
		return ;
	}

	//成功后输出提示信息
	printf("\t\t连接服务器成功！");
	
	isconnected=TRUE;

    	}
}

/*断开连接操作*/
void disconnect()
{
	char msg[512];
	if(isconnected)
	{
		close(socket_fd);
		sprintf(msg,"断开连接成功！\n");
		display_info(msg);
		isconnected=FALSE;
	}
}


/*申请教室回调函数*/
int button_applyroom(int x,int y)
{
	char msg[512];
	char send_buf[512],recv_buf[512];

	/*获取输入的间数*/

	int room_NUM=y;
	int room_ID=x;
	if(room_NUM<=0) { //判断输入的间数是否正确，不正确的话，给出提示信息，重新输入。
		          printf("输入的间数错误！请重新输入！\n") ;
		          return(1) ;
	}
	/*申请*/

	init_message();
	message.msg_type=APPLY_ROOM;
	message.room_id=room_ID;
	message.room_num=room_NUM;
	memcpy(send_buf,&message,sizeof(message));
	int ret=send(socket_fd, send_buf,sizeof(message),0);
	/*发送出错*/
	if(ret==-1) {
		printf ("\t发送失败！请重新发送！\n" ) ;
		return(1) ;
	}
	ret=recv(socket_fd,recv_buf,sizeof(message),0);
	if(ret==-1) {
		printf ("\t服务器端接收失败！请重新发送！\n") ;
		return(1) ;
	}
	memcpy(&message,recv_buf,sizeof(message));
	if(message.msg_type==APPLY_SUCCEED)
		sprintf(msg,"\t申请成功！楼号：%s, 间数：%d\n",change_id(message.room_id),message.room_num);
	else
		sprintf(msg,"\t申请失败！楼号：%s, 剩余间数：%d, 请求间数：%d\n",change_id(message.room_id),message.room_num,room_NUM);
	display_info(msg);
}

/*申请教室回调函数*/
void applyroom()
{
	int room_ID;
	int room_NUM;
	printf("\t \t 1.西三   2.西二   3.西一   4.东一   5.东二    6.东三\n\n");
	printf("\t请输入要申请的楼号(1-6)：");
	scanf("%d",&room_ID);
	if (room_ID<=6&&room_ID>=1){                                   
		                           printf("\t请输入要申请的间数：");
		                       		scanf("%d",&room_NUM); getchar();
		                           button_applyroom(room_ID,room_NUM);                                
	    }
	else {printf("\t输入的楼号不正确,请重新输入！\n"); applyroom();}
}

 
void inquireall()
{

	int i,pos;
	char msg[512];
	char send_buf[512], recv_buf[512];
	init_message();
	message.msg_type=INQUIRE_ALL;
	memcpy(send_buf,&message,sizeof(message));
	int ret=send(socket_fd, send_buf,sizeof(message),0);
	/*发送出错*/
	if(ret==-1) {
		printf("发送失败！请重新发送！");
		return ;
	}
	ret=recv(socket_fd,recv_buf,sizeof(recv_buf),0);
	if(ret==-1) {
		printf("服务器端接收失败！请重新发送！");
		return ;
	}
	pos=0;
	sprintf(msg,"\t当前所有教学楼空闲教室情况为：");
	display_info(msg);
	for (i=0;i<ret;i=i+sizeof(message)) {
		memcpy(&message,recv_buf+pos,sizeof(message));
		if(message.msg_type==INQUIRE_SUCCEED)
		sprintf(msg,"\t\t\t %s:    剩余间数：%d",change_id(message.room_id),message.room_num);
		else
		sprintf(msg,"\t\t\t查询失败！%s:      剩余间数：未知",change_id(message.room_id));
		display_info(msg);
		pos+=sizeof(message);
	}
}
//执行何种操作
void otherOperate(){
     	printf("\t  与服务器断开连接请按 d\n");
	printf("\t \t申请教室请按 a");
	printf("\t \t  查询剩余教室请按 q\n");
	//printf("归还教室请按r\n");
	printf("-----------------------------------------------------------------------------\n");
        while((c=getchar())!='d'){
		getchar();
		if(c=='q'||c=='Q') inquireall();
		else if(c=='a'||c=='A') applyroom();
		else if(c=='d'||c=='D') disconnect();
		else {
		     printf("\t\t不正确指令，请重新输入!"); 
		     otherOperate();
		} 
	}  
}

//是否重连？
int re_connect(){
	if(ret<0){
		printf("重连？【y/n】");
		c=getchar();
		getchar();
		if(c=='y'){connectserver();return 1;}
		else return 0;
	}
}

int main(int argc, char *argv[])
{
	printf("\n-------------------------------客户端主界面----------------------------------\n\n");
	connectserver();
	if (ret<0)return 0;//连接服务器失败，退出
	otherOperate();
	disconnect();
	printf("~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~\n");
	return 0;
}
