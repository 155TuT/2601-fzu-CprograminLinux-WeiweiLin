/*连接时需要使用线程库,所以在使用pthread_create创建线程时，在编译中要加-lpthread参数

gcc server.c -o server -lpthread*/

/*server.c*/

#include "servicethread.h"

int listen_fd, conn_fd;  //监听socket，连接socket
struct sockaddr_in server, cli_addr; //服务器地址信息，客户端地址信息
int ret, buffer_index, i,cli_len;
unsigned long ip_addr;
int flag=1;
char c;
pthread_t listentid,servicetid; //监听线程ID,服务线程ID；


/*监听线程*/
void * listen_thread(void *p)
{
	char msg[512];
	while(1) {			//printf("ok\n");
		/*接受远端的TCP连接请求*/
		cli_len=sizeof(cli_addr);
		conn_fd=accept(listen_fd,(struct sockaddr *)&cli_addr, &cli_len);
		if(conn_fd<0)
		continue;

		ip_addr=ntohl(cli_addr.sin_addr.s_addr);
		/*检测重复连接*/
		check_connection(ip_addr);
		/*分配线程缓冲区*/
		buffer_index=get_free_buff();
		if(buffer_index<0) {
			sprintf(msg,"没用空闲的线程缓冲区。\n");
			add_info(msg);
			close(conn_fd);
			continue;
		}
		/*填写服务线程需要的信息*/
		pthread_mutex_lock(&buff_mutex);
		thread_buff[buffer_index].buff_index=buffer_index;
		thread_buff[buffer_index].ip_addr=ip_addr;
		thread_buff[buffer_index].conn_fd=conn_fd;
		thread_buff[buffer_index].buff_status=BUFF_OCCUPIED;
		pthread_mutex_unlock(&buff_mutex);

		/*创建新的服务线程*/
		ret=pthread_create(&servicetid, NULL, service_thread, &thread_buff[buffer_index]);
		if(ret==-1) {
			sprintf(msg,"创建服务线程出错！\n");
			add_info(msg);
			close(conn_fd);
			/*释放线程缓冲区*/
			pthread_mutex_lock(&buff_mutex);
			thread_buff[buffer_index].tid=0;
			thread_buff[buffer_index].buff_status=BUFF_FREED;
			pthread_mutex_unlock(&buff_mutex);
		}
	}
}

int isserveropened=FALSE; //服务器端是否开启标志位


//消息内容输出函数
void display_info(char *msg)
{
	printf("%s\n",msg);
}

//开启服务器操作
void startserver()
{
	char msg[512]; //提示信息

	//初始化数据结构
	init_thread_buff();
	init_room_list();

	if(!isserveropened)
	{
		/*创建套接字*/
		listen_fd=socket(AF_INET,SOCK_STREAM,0);
		if(listen_fd<0) {
			sprintf(msg,"创建监听套接字出错！ \n");
			display_info(msg);
			return;
		}

		/*填写服务器的地址信息*/
		server.sin_family=AF_INET;
		server.sin_addr.s_addr=htonl(INADDR_ANY);
		server.sin_port=htons(SERVER_PORT_NO);

		setsockopt(listen_fd,SOL_SOCKET,SO_REUSEADDR,(void *)&flag,sizeof(int));

		/*绑定端口*/
		ret=bind(listen_fd,(struct sockaddr*)&server, sizeof(server));
		if(ret<0) {
			sprintf(msg,"绑定TCP端口：%d 出错！ \n",SERVER_PORT_NO);
			display_info(msg);
			close(listen_fd);
			return;
		}

		/*转化成倾听套接字*/
		listen(listen_fd,5);
		ret=pthread_create(&listentid, NULL, listen_thread, NULL);
		if(ret==-1) {
			sprintf(msg,"创建监听线程出错！ \n");
			display_info(msg);
			close(listen_fd);
			return;
		}

		//成功后输出提示信息
		sprintf(msg,"             服务器端开启成功！服务器在端口：%d 准备接受连接！ \n",SERVER_PORT_NO);
		display_info(msg);
		isserveropened=TRUE;

	}
}

/*关闭服务器操作*/
void stopserver()
{

	char msg[512];
	if(isserveropened)
	{

		pthread_mutex_lock(&buff_mutex);
		for(i=0;i<THREAD_BUFF_NUM;i++) {
			if(thread_buff[i].buff_status!=BUFF_FREED) {
				/*退出服务线程*/
				pthread_cancel(thread_buff[i].tid);
				pthread_join(thread_buff[i].tid,NULL);
				/*退出的线程不释放它的缓冲区，释放工作由主线程来处理*/
				thread_buff[i].tid=0;
				thread_buff[i].buff_status=BUFF_FREED;
				close(thread_buff[i].conn_fd);
			}
		}

		pthread_mutex_unlock(&buff_mutex);
		pthread_cancel(listentid);
		pthread_join(listentid,NULL);
		close(listen_fd);
		sprintf(msg,"\t  服务器端成功关闭！ \n");
		display_info(msg);
		isserveropened=FALSE;
	}
}

void inquireall()
{

	int i;
	char msg[512];
	printf("\t当前所有教学楼空闲教室情况为：\n");
	for(i=0;i<ROOM_NUM;i++) {
		sprintf(msg,"\t \t\t%s:  剩余间数：%d",change_id(room_list[i].room_id),room_list[i].room_num);
		display_info(msg);
	}
}

//数据重置
void data_reset()
{

	int i;
	char msg[512];
	FILE *fp;
	fp=fopen("room.txt","w");
	for(i=0;i<ROOM_NUM;i++) {
		sprintf(msg,"%d %d\n",room_list[i].room_id,room_list[i].room_num);
		//display_info(msg);
		fprintf(fp,"%s",msg);
	}
	
	fclose(fp);
}
//执行何种操作
void otherOperate(){
	
     	printf("             关闭服务器请按 s");
	printf("             查询剩余教室请按 q\n");
	printf("-----------------------------------------------------------------------------\n");
	
	while ((c=getchar())!='s'){        
		getchar();
		if(c=='q'||c=='Q') inquireall();
		else if(c=='s'||c=='S') stopserver();
		else {
		     printf("\t\t不正确的指令，请重新输入!\n"); 
		     otherOperate();
		}  
	} 
}


int main(int argc, char *argv[])
{

	init_room_list();
	printf("\n-----------------------------服务器端主界面----------------------------------\n\n");
	startserver();
	otherOperate();
	data_reset();
	stopserver();
	printf("~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~\n");
	return 0;
}

