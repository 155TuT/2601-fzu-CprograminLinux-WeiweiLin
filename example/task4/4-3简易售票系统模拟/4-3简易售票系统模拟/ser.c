#include "train.h"

int main(int argc, char *argv[])
{
	int server_sockfd, client_sockfd;//套接字所需的变量
	int server_len, client_len;
	struct sockaddr_in server_address;
	struct sockaddr_in client_address;

	int result;
	int pid;
	fd_set readfds, testfds;
	time_t timer;
	struct tm* t_tm;

	int *ticket;
	void *share1=(void *)0;
	int shmid1;
	int i, j, idij;
	shmid1=shmget(9966, 100, 0666 | IPC_CREAT);//创建共享内存
	share1=shmat(shmid1, (void *)0, 0);
	ticket=(int *)share1;

	union semun sem_union;//对信号量的结构体和参数的设定
	sem_union.val=1;

	FILE *fp;
	fp=fopen("tic", "r");
	for(i=1; i<17; i++)
		fscanf(fp, "%d", ticket+i);
	ticket[18]=1;//服务器开启标志，用于提醒客户端

	fclose(fp);
	printf("server running\n");
	if((pid=fork())==0)//子进程实现与客户端的交互，父进程实现服务器管理员的功能
	{
		server_sockfd=socket(AF_INET, SOCK_STREAM, 0);//创建套接字
		server_address.sin_family=AF_INET;
		server_address.sin_addr.s_addr=htonl(INADDR_ANY);
		server_address.sin_port=htons(9789);
		server_len=sizeof(server_address);

		bind(server_sockfd, (struct sockaddr *)&server_address, server_len);//绑定套接字
		listen(server_sockfd, 5);//监听
		FD_ZERO(&readfds);//初始化打开文件描述符构成的集合
		FD_SET(server_sockfd, &readfds);//设置集合中由参数server_sockfd传递的文件描述符

		if(semget(33, 1, 0666 | IPC_CREAT | IPC_EXCL)>=0)//第一次创建信号量
		{
			sem_id=semget(33, 1, 0666 | IPC_CREAT);
			semctl(sem_id, 0, SETVAL, sem_union);//初始化信号量
		}
		sem_id=semget(33, 1, 0666 | IPC_CREAT);
		ticket[17]=sem_id;


		while(1)
		{
			char se_buf[2048], rec_buf[2048];
			int fd, i;
			int nread, pos;
			testfds=readfds;

                        //用select函数对客户端进行检测
			result=select(FD_SETSIZE, &testfds, (fd_set *)0, (fd_set *)0, (struct timeval *)0);
                        
			fp=fopen("tic", "r");
			for(i=1; i<17; i++)
				fscanf(fp, "%d", ticket+i);
			fclose(fp);

			if(result<1)
			{
				perror("server5");
				exit(1);
			}
			for(fd=0; fd<FD_SETSIZE; fd++)
			{
			    //由参数fd指向的文件描述符是否是由参数testfds指向的集合中的一个元素
                            if(FD_ISSET(fd, &testfds))
				{
                                   //如果是的话，就表明是一个新的连接请求
				    if(fd==server_sockfd)
				       {
					 client_len=sizeof(client_address);
					 client_sockfd=accept(server_sockfd, (struct sockaddr    *)       &client_address, &client_len);//添加新的client_sockfd到描述符集合中
					 FD_SET(client_sockfd, &readfds);
					 printf("adding client on fd %d\n", client_sockfd-3);
					}
					else//否则就说明是已连接客户端上的请求
					{
						ioctl(fd, FIONREAD, &nread);//检测是否收到信号，如果nread为0则表示客户端断开连接，大于0则客户端有数据发送
						if(nread==0)
						{
							nread=read(fd, rec_buf, nread);
							close(fd);
							FD_CLR(fd, &readfds);//清除由参数fd传递的文件描述符
							printf("removing client on fd %d\n",fd-3);
						}
						else 
						{

							printf("The require from client %d\n", fd-3);
							nread=recv(fd, rec_buf, nread, 0);
							memcpy(&message, rec_buf, sizeof(message));
							switch(message.msg_type)
							{
							case ALL://查询所有
								if(!fork())
								{
									pos=0;	
									time(&timer);//获取系统时间
									t_tm=localtime(&timer);

									for(i=0; i<4; i++)
									{
										for(j=0; j<4; j++)
										{
											idij=i*4+j;
											//以下的变量如其名，可以参考train.h头文件
											message.train_ID=idij+1;
											message.ticket_num=ticket[idij+1];
											message.ticket_total_price=price[idij+1];
											strcpy(message.start_p, start[i]);
											strcpy(message.dest_p, dest[j]);
											message.time_sy=t_tm->tm_year+1900;
											message.time_dy=t_tm->tm_year+1900;
											message.time_smo=t_tm->tm_mon+1;
											message.time_dmo=t_tm->tm_mon+1;
											message.time_sd=t_tm->tm_mday;
											message.time_dd=t_tm->tm_mday;
											message.time_sh=shour[idij];
											message.time_dh=dhour[idij];
											message.time_sm=smin[idij];
											message.time_dm=dmin[idij];

											if(shour[idij]>dhour[idij]) message.time_dd+=1;//如果抵达时间小于出发时间，则说明是第二天抵达，因此日期加1
											if(t_tm->tm_hour<shour[idij]) message.buyb=1;
											else if(t_tm->tm_hour==shour[idij]) 
											{
												if(t_tm->tm_min<smin[idij]) message.buyb=1;//message.buyb为1表示时间未到可买，为0不可买
												else message.buyb=0;
											}
											else message.buyb=0;

											memcpy(se_buf+pos, &message, sizeof(message));//每次将一个message结构体大小的message结构体放入se_buf数组中
											pos+=sizeof(message);

										}
									}
									printf("inquireingall...\n");//这边三行是为了演示需要，可以没有
									sleep(5);
									printf("inquire over\n");

									nread=send(fd, se_buf, pos, 0);//向客户端发送数据
									if(nread==-1) printf("Send erro\n");
									else
									{//这是将刚才这个查票的记录写入sale文本，以便后面父进程查询整个售票情况
										fp=fopen("sale", "a");
										fprintf(fp, "%4d%02d/%02d %02d:%02d:%02d the client %d inquire all trains\n", t_tm->tm_year+1900, t_tm->tm_mon+1, t_tm->tm_mday, t_tm->tm_hour, t_tm->tm_min, t_tm->tm_sec, fd-3);
										fclose(fp);

										exit(0);
									}
								}
								break;

							case ONEID://以航班号查询特定航班
								if(!fork())
								{
									time(&timer);
									t_tm=localtime(&timer);

									for(i=0; i<TRAIN_NUM; i++)
									{
										if(message.train_ID==i+1)
										{
											message.ticket_num=ticket[i+1];
											message.ticket_total_price=price[i+1];
											strcpy(message.start_p, start[i/4]);
											strcpy(message.dest_p, dest[i%4]);
											message.time_sy=t_tm->tm_year+1900;
											message.time_dy=t_tm->tm_year+1900;
											message.time_smo=t_tm->tm_mon+1;
											message.time_dmo=t_tm->tm_mon+1;
											message.time_sd=t_tm->tm_mday;
											message.time_dd=t_tm->tm_mday;
											message.time_sh=shour[i];
											message.time_dh=dhour[i];
											message.time_sm=smin[i];
											message.time_dm=dmin[i];
											if(shour[i]>dhour[i]) message.time_dd+=1;
											if(t_tm->tm_hour<shour[i]) message.buyb=1;
											else if(t_tm->tm_hour==shour[i])
											{
												if(t_tm->tm_min<smin[i]) message.buyb=1;
												else message.buyb=0;
											}
											else message.buyb=0;

											memcpy(se_buf, &message, sizeof(message));
										}
									}
									printf("inquireingid...\n");
									sleep(5);
									printf("inquire over\n");

									nread=send(fd, se_buf, sizeof(message), 0);
									if(nread==-1) printf("Send erro\n");
									else
									{
										fp=fopen("sale", "a");
										fprintf(fp, "%4d%02d/%02d %02d:%02d:%02d the client %d inquire the train of %d\n", t_tm->tm_year+1900, t_tm->tm_mon+1, t_tm->tm_mday, t_tm->tm_hour, t_tm->tm_min, t_tm->tm_sec, fd-3, i+1);
										fclose(fp);
									}
									exit(0);
								}		
								break;

							case ONEPLACE://以地点查询特定航班
								if(!fork())
								{
									time(&timer);
									t_tm=localtime(&timer);

									pos=0;
									for(i=0; i<4; i++)
									{
										if(!strcmp(message.start_p, start[i]))
										{
											for(j=0; j<4; j++)
												if(!strcmp(message.dest_p, dest[j]))
												{
													idij=i*4+j;
													message.train_ID=idij+1;
													message.ticket_num=ticket[idij+1];
													message.ticket_total_price=price[idij+1];
													strcpy(message.start_p, start[i]);
													strcpy(message.dest_p, dest[j]);
													message.time_sy=t_tm->tm_year+1900;
													message.time_dy=t_tm->tm_year+1900;
													message.time_smo=t_tm->tm_mon+1;
													message.time_dmo=t_tm->tm_mon+1;
													message.time_sd=t_tm->tm_mday;
													message.time_dd=t_tm->tm_mday;
													message.time_sh=shour[idij];
													message.time_dh=dhour[idij];
													message.time_sm=dmin[idij];
													message.time_dm=smin[idij];
													if(shour[i]>dhour[idij]) message.time_dd+=1;
													if(t_tm->tm_hour<shour[idij]) message.buyb=1;
													else if(t_tm->tm_hour==shour[idij])
													{
														if(t_tm->tm_min<smin[idij]) message.buyb=1;
														else message.buyb=0;
													}
													else message.buyb=0;

													memcpy(se_buf+pos, &message, sizeof(message));
													pos+=sizeof(message);
												}
										}
									}
									printf("inquireingpla...\n");
									sleep(5);
									printf("inquire over\n");
									nread=send(fd, se_buf, pos, 0);
									if(nread==-1) printf("Send erro\n");
									else
									{
										fp=fopen("sale", "a");
										fprintf(fp, "%4d%02d/%02d %02d:%02d:%02d the client %d inquire the train of %d\n", t_tm->tm_year+1900, t_tm->tm_mon+1, t_tm->tm_mday, t_tm->tm_hour, t_tm->tm_min, t_tm->tm_sec, fd-3, idij+1);
										fclose(fp);
									}
									exit(0);
								}

								break;

							case BUY://买票
								if(!fork())
								{
									time(&timer);
									t_tm=localtime(&timer);


									if(semaphore_p(&ticket[17]))//P操作，对信号量sem_id减一，注：这边不是系统调用，函数在train.h中
									{
										for(i=0; i<TRAIN_NUM; i++)
										{
											if(message.train_ID==i+1)//如果航班号有的话
											{
												message.time_sy=t_tm->tm_year+1900;
												message.time_dy=t_tm->tm_year+1900;
												message.time_smo=t_tm->tm_mon+1;
												message.time_dmo=t_tm->tm_mon+1;
												message.time_sd=t_tm->tm_mday;
												message.time_dd=t_tm->tm_mday;
												message.time_sh=shour[i];
												message.time_dh=dhour[i];
												message.time_sm=smin[i];
												message.time_dm=dmin[i];
												if(shour[i]>dhour[i]) message.time_dd+=1;
												if(t_tm->tm_hour<shour[i]) message.buyb=1;
												else if(t_tm->tm_hour==shour[i])
												{
													if(t_tm->tm_min<smin[i]) message.buyb=1;
													else message.buyb=0;
												}
												else message.buyb=0;

												if(message.ticket_num<=ticket[i+1] && message.buyb)
												{
													message.msg_type=BUY_SUCCEED;//购买成功
													message.ticket_total_price=price[i+1]*message.ticket_num;
													ticket[i+1]-=message.ticket_num;
													strcpy(message.start_p, start[i/4]);
													strcpy(message.dest_p, dest[i%4]);
													memcpy(se_buf, &message, sizeof(message));
													int j;
													fp=fopen("tic", "w");
													for(j=1; j<17; j++)
														fprintf(fp, "%d ", ticket[j]);
													fclose(fp);
													fp=fopen("tickets", "w");
													for(j=1; j<17; j++)
														fprintf(fp, "ID %d remain %d tickets\n", j, ticket[j]);
													fclose(fp);

												}
												else
												{	
													message.msg_type=BUY_FAILED;//购买失败
													message.ticket_num=ticket[i+1];
													message.ticket_total_price=price[i+1];
													strcpy(message.start_p, start[i/4]);
													strcpy(message.dest_p, dest[i%4]);
													memcpy(se_buf, &message, sizeof(message));
												}
												printf("dealing...\n");
												sleep(10);
												printf("deal over\n");
												semaphore_v(&ticket[17]);	//V操作，对信号量sem_id加一
												nread=send(fd, se_buf, sizeof(message), 0);
												if(nread==-1) printf("Send erro\n");
												else
												{
													fp=fopen("sale", "a");
													if(message.msg_type==BUY_SUCCEED)
														fprintf(fp, "%4d%02d/%02d %02d:%02d:%02d the client %d buy %d tickets of the train of %d\n", t_tm->tm_year+1900, t_tm->tm_mon+1, t_tm->tm_mday, t_tm->tm_hour, t_tm->tm_min, t_tm->tm_sec, fd-3, message.ticket_num, i+1);
													else
														fprintf(fp, "%4d%02d/%02d %02d:%02d:%02d the client %d buy the train of %d failed\n", t_tm->tm_year+1900, t_tm->tm_mon+1, t_tm->tm_mday, t_tm->tm_hour, t_tm->tm_min, t_tm->tm_sec, fd-3, i+1);
													fclose(fp);
													exit(0);
												}

												break;

											}
										}

									}
								}
								break;
							case RETURN://退票
								time(&timer);
								t_tm=localtime(&timer);
								if(!fork())
								{
									if(semaphore_p(&ticket[17]))
									{
										for(i=0; i<TRAIN_NUM; i++)
										{
											if(message.train_ID==i+1)
											{
												message.time_sy=t_tm->tm_year+1900;
												message.time_dy=t_tm->tm_year+1900;
												message.time_smo=t_tm->tm_mon+1;
												message.time_dmo=t_tm->tm_mon+1;
												message.time_sd=t_tm->tm_mday;
												message.time_dd=t_tm->tm_mday;
												message.time_sh=shour[i];
												message.time_dh=dhour[i];
												message.time_sm=smin[i];
												message.time_dm=dmin[i];
												if(shour[i]>dhour[i]) message.time_dd+=1;
												if(t_tm->tm_hour<shour[i]) message.retub=1;//message.retub与上面message.buyb功能一样
												else if(t_tm->tm_hour==shour[i])
												{
													if(t_tm->tm_min<smin[i]) message.retub=1;
													else message.retub=0;
												}
												else message.retub=0;

												if(message.ticket_num<=NUMS-ticket[i+1] && message.retub)
												{
													message.msg_type=RETURN_SUCCEED;
													// message.ticket_total_price=price[i+1]*message.ticket_num;
													ticket[i+1]+=message.ticket_num;
													strcpy(message.start_p, start[i/4]);
													strcpy(message.dest_p, dest[i%4]);	
													memcpy(se_buf, &message, sizeof(message));
												 int j;
												 fp=fopen("tic", "w");
												 for(j=1; j<17; j++)
													 fprintf(fp, "%d ", ticket[j]);
												 fclose(fp);
												 fp=fopen("tickets", "w");
												 for(j=1; j<17; j++)
													 fprintf(fp, "ID %d remain %d tickets\n", j, ticket[j]);
												 fclose(fp);



												}
												else
												{
													message.msg_type=RETURN_FAILED;
													// message.ticket_num=ticket[i+1];
													message.ticket_total_price=price[i+1];
													strcpy(message.start_p, start[i/4]);
													strcpy(message.dest_p, dest[i%4]);
													memcpy(se_buf, &message, sizeof(message));
												}
												printf("dealing...\n");
												sleep(10);
												printf("deal over\n");
												semaphore_v(&ticket[17]);
												nread=send(fd, se_buf, sizeof(message), 0);
												if(nread==-1) printf("Send erro\n");
												else
												{
													fp=fopen("sale", "a");
													if(message.msg_type==RETURN_SUCCEED)
														fprintf(fp, "%4d%02d/%02d %02d:%02d:%02d the client %d return %d tickets of the train of %d\n", t_tm->tm_year+1900, t_tm->tm_mon+1, t_tm->tm_mday, t_tm->tm_hour, t_tm->tm_min, t_tm->tm_sec, fd-3, message.ticket_num, i+1);
													else
														fprintf(fp, "%4d%02d/%02d %02d:%02d:%02d the client %d return the train of %d failed\n", t_tm->tm_year+1900, t_tm->tm_mon+1, t_tm->tm_mday, t_tm->tm_hour, t_tm->tm_min, t_tm->tm_sec, fd-3, i+1);
													fclose(fp);
												}

												break;

											}
										}


									}exit(0);
								}break;


							}
						}
					}
				}
			}
		}
	}
	else
	{
		while(1)
		{
			printf("\nWhat do you want to do: inquireall or inquireone or sale or clear or update or quit\n");
			char cmd[12], ch, c;
			int id, i, nnum;
			int sl;
			fgets(cmd,sizeof(cmd),stdin);//读入指令
			if(cmd[strlen(cmd)-1] == '\n'){
			cmd[strlen(cmd)-1] = '\0';
			}
			time(&timer);  
			t_tm=localtime(&timer);
			if(!strcmp(cmd, "inquireall"))
			{
				for(i=0; i<TRAIN_NUM; i++)
				{
					sl=0;
					if(t_tm->tm_hour<shour[i]) sl=1;//s1的功能与上面message.buyb功能一样
					else if(t_tm->tm_hour==shour[i])
					{
						if(t_tm->tm_min<smin[i]) sl=1;
						else sl=0;                                                          
					}
					else sl=0;
					if(sl)
					{
						if(shour[i]>dhour[i])
							printf("ID: %d, From: %s To: %s, Leave: %4d/%02d/%02d %02d:%02d, Arrive: %4d/%02d/%02d %02d:%02d   Ticketnums: %d, Price: %ld   sale\n", i+1, start[i/4], dest[i%4], t_tm->tm_year+1900, t_tm->tm_mon+1, t_tm->tm_mday, shour[i], smin[i], t_tm->tm_year+1900, t_tm->tm_mon+1, t_tm->tm_mday+1, dhour[i], dmin[i], ticket[i+1], price[i+1]);
						else
							printf("ID: %d, From: %s To: %s, Leave: %4d/%02d/%02d %02d:%02d, Arrive: %4d/%02d/%02d %02d:%02d   Ticketnums: %d, Price: %ld   sale\n", i+1, start[i/4], dest[i%4], t_tm->tm_year+1900, t_tm->tm_mon+1, t_tm->tm_mday, shour[i], smin[i], t_tm->tm_year+1900, t_tm->tm_mon+1, t_tm->tm_mday, dhour[i], dmin[i], ticket[i+1], price[i+1]);

					}
					else
						printf("ID: %d, From: %s To: %s, Leave: %4d/%02d/%02d %02d:%02d, Arrive: %4d/%02d/%02d %02d:%02d   Ticketnums: %d, Price: %ld  not sale\n", i+1, start[i/4], dest[i%4], t_tm->tm_year+1900, t_tm->tm_mon+1, t_tm->tm_mday, shour[i], smin[i], t_tm->tm_year+1900, t_tm->tm_mon+1, t_tm->tm_mday, dhour[i], dmin[i], ticket[i+1], price[i+1]);
				}
				printf("inquireall over\n");

			}
			else if(!strcmp(cmd, "inquireone"))
			{
				printf("enter the ID you want to inquire\n");
				scanf("%d%c", &i, &ch);
				sl=0;
				if(t_tm->tm_hour<shour[i]) sl=1;
				else if(t_tm->tm_hour==shour[i])
				{
					if(t_tm->tm_min<smin[i]) sl=1;
					else sl=0;
				}
				else sl=0;
				if(sl)
				{
					if(shour[i]>dhour[i])
						printf("ID: %d, From: %s To: %s, Leave: %4d/%02d/%02d %02d:%02d, Arrive: %4d/%02d/%02d %02d:%02d   Ticketnums: %d, Price: %ld   sale\n", i+1, start[i/4], dest[i%4], t_tm->tm_year+1900, t_tm->tm_mon+1, t_tm->tm_mday, shour[i], smin[i], t_tm->tm_year+1900, t_tm->tm_mon+1, t_tm->tm_mday+1, dhour[i], dmin[i], ticket[i+1], price[i+1]);
					else
						printf("ID: %d, From: %s To: %s, Leave: %4d/%02d/%02d %02d:%02d, Arrive: %4d/%02d/%02d %02d:%02d   Ticketnums: %d, Price: %ld   sale\n", i+1, start[i/4], dest[i%4], t_tm->tm_year+1900, t_tm->tm_mon+1, t_tm->tm_mday, shour[i], smin[i], t_tm->tm_year+1900, t_tm->tm_mon+1, t_tm->tm_mday, dhour[i], dmin[i], ticket[i+1], price[i+1]);

				}
				else
					printf("ID: %d, From: %s To: %s, Leave: %4d/%02d/%02d %02d:%02d, Arrive: %4d/%02d/%02d %02d:%02d   Ticketnums: %d, Price: %ld  not sale\n", i+1, start[i/4], dest[i%4], t_tm->tm_year+1900, t_tm->tm_mon+1, t_tm->tm_mday, shour[i], smin[i], t_tm->tm_year+1900, t_tm->tm_mon+1, t_tm->tm_mday, dhour[i], dmin[i], ticket[i+1], price[i+1]);

				printf("inquireone over\n");
			}
			else if(!strcmp(cmd, "sale"))
			{//从sale文本中读取售票情况存入sale_buf数组中，并从中输出
				char sale_buf[4096];
				int j=0;
				fp=fopen("sale", "r");	
				ch=fgetc(fp);
				while(ch!=EOF)
				{
					sale_buf[j++]=ch;
					ch=fgetc(fp);
				}
				sale_buf[j]='\0';
				printf("saling...\n");
				sleep(2);
				for(i=0; i<j; i++)
					printf("%c", sale_buf[i]);
				fclose(fp);
			}
			else if(!strcmp(cmd, "clear"))
			{
				printf("Do you really want to clear the record? you will lose the data  y/n:" );
				scanf("%c%c", &ch, &c);
				if(ch=='y')
				{ fp=fopen("sale", "w");
				fclose(fp);
				printf("clearing...\n");
				sleep(2);
				printf("clear succeed\n");}
				else printf("not clear the record\n");
			}
			else if(!strcmp(cmd, "update"))
			{
				fp=fopen("tic", "r");
				int new=0;
				for(i=1; i<17; i++)
				{
					fscanf(fp, "%d", ticket+i);
					printf("ID %d remain %d tickets\n", i, ticket[i]);
				}
				printf("Do you want to update one data or all data  one/all/no:");
				fgets(cmd,sizeof(cmd),stdin);//读入指令
				if(cmd[strlen(cmd)-1] == '\n'){
				cmd[strlen(cmd)-1] = '\0';
				}
				
				if(!strcmp(cmd, "one"))//对一组数据更新
				{
					printf("please enter the ID and the tickets num you want to update: ");
					scanf("%d%d%c", &i, &nnum, &c);
					ticket[i]=nnum;
					new=1;
				}
				else if(!strcmp(cmd, "all")) //对所有数据更新
				{	
					printf("please enter every ID and the tickets num you want to update\n");
					for(i=1; i<17; i++)
					{
						scanf("%d%d%c", &i, &nnum, &c);
						ticket[i]=nnum;
					}
					new=1;
				}
				else new=0;//不更新
				if(new==1)
				{
					fp=fopen("tic", "w");
					for(i=1; i<17; i++)
						fprintf(fp, "%d ", ticket[i]);
					fclose(fp);
					fp=fopen("tickets", "w");	
					for(i=1; i<17; i++)
						fprintf(fp, "ID %d remain %d tickets\n", i, ticket[i]);
					fclose(fp);
					printf("updating...\n");
					sleep(2);
					printf("update succeed\n");
					printf("the new datas is:\n");
					fp=fopen("tic", "r");
					for(i=1; i<17; i++)
					{
						fscanf(fp, "%d", ticket+i);
						printf("ID %d remain %d tickets\n", i, ticket[i]);
					}
					fclose(fp);

				}
				else printf("no update\n");
			}




			else if(!strcmp(cmd, "quit"))//关闭服务器
			{
				semctl(sem_id, 0, IPC_RMID, sem_union);//去除上面创建的信号量
				kill(pid, SIGTERM);//向子进程发送一个关闭子进程的信号
				wait(0);//等待子进程结束
				fp=fopen("tic", "w");
				for(i=1; i<17; i++)
					fprintf(fp, "%d ", ticket[i]);
				fclose(fp);
				fp=fopen("tickets", "w");
				for(i=1; i<17; i++)
					fprintf(fp, "ID %d remain %d tickets\n", i, ticket[i]);    
				fclose(fp);
				ticket[18]=0;


				printf("Close the server succeed\n");
				exit(0);
			}
			else  printf("the command is unrecoginsed\n");//对输入的命令不合法时，输出该语句
		}
	}

}
