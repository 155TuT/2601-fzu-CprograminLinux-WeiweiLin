#include "train.h"

int main(int argc, char *argv[])//大体上的功能实现与服务器的一样，所以就不描述了
{
	int sockfd;
	int len;
	struct sockaddr_in address;
	int result;
	int *ticket;
	void *share1=(void *)0;
	int shmid1, shmid2;

	shmid1=shmget(9966, 100, 0666 | IPC_CREAT);
	share1=shmat(shmid1, (void *)0, 0);
	ticket=(int *)share1;


	sockfd=socket(AF_INET, SOCK_STREAM, 0);
	address.sin_family=AF_INET;
	address.sin_addr.s_addr=inet_addr("127.0.0.1");//用回环网络通信
	address.sin_port=htons(9789);//端口号
	len=sizeof(address);

	result=connect(sockfd, (struct sockaddr *)&address, len);
	if(result==-1) {perror("oops: client1"); exit(1);}
	if(ticket[18]==1);//为1表示服务端处于开启状态，为0表示关闭
	printf("Have connected the server\n");
	while(1)
	{
		if(ticket[18]==0) printf("the server have been closed, please quit\n");
		else
			printf("\nWhat do you want to do: inquireall or inquireid or inquireplace or buy or return or quit\n");
		char g[15], s[15], d[15], ch;
		char se_buf[2048],rec_buf[2048];
		int nm, id, ticnum=0;
		int ret, pos=0, i;
		fgets(g,sizeof(g),stdin);//读入指令
	    if(g[strlen(g)-1] == '\n'){
		g[strlen(g)-1] = '\0';
		}
		if(!strcmp(g, "quit"))
		{	
			sleep(3);
			if(ticket[18]==1)
				printf("cut off the connect succeed\n");
			else printf("close the client succeed\n");
			break;
		}
		else if(!strcmp(g, "inquireall"))
		{
			message.msg_type=ALL;
			memcpy(se_buf, &message, sizeof(message));
			ret=send(sockfd, se_buf, sizeof(message), 0);		
			if(ret==-1) printf("Send error\n");
			else
			{	printf("waiting...\n");
			ret=recv(sockfd, rec_buf, sizeof(rec_buf), 0);		
			if(ret==-1) printf("Receive error\n");
			else
			{	
				for(i=0; i<ret; i=i+sizeof(message))
				{
					memcpy(&message, rec_buf+pos, sizeof(message));
					if(message.buyb)
						printf("ID: %d, From: %s To: %s, Leave: %4d/%02d/%02d %02d:%02d, Arrive: %4d/%02d/%02d %02d:%02d   Ticketnums: %d, Price: %d   sale\n", message.train_ID, message.start_p, message.dest_p, message.time_sy, message.time_smo, message.time_sd, message.time_sh, message.time_sm, message.time_dy, message.time_dmo, message.time_dd, message.time_dh, message.time_dm, message.ticket_num, message.ticket_total_price);
					else   printf("ID: %d, From: %s To: %s, Leave: %4d/%02d/%02d %02d:%02d, Arrive: %4d/%02d/%02d %02d:%02d   Ticketnums: %d, Price: %d   not sale\n", message.train_ID, message.start_p, message.dest_p, message.time_sy, message.time_smo, message.time_sd, message.time_sh, message.time_sm, message.time_dy, message.time_dmo, message.time_dd, message.time_dh, message.time_dm, message.ticket_num, message.ticket_total_price);	

					pos+=sizeof(message);
				}
			}
			}		
		}	
		else if(!strcmp(g, "inquireid"))
		{
			printf("please enter the ID you want to inquire\n");
			scanf("%d%c", &id, &ch);
			if(id>0 && id<17)
			{
				message.msg_type=ONEID;
				message.train_ID=id;
				memcpy(se_buf, &message, sizeof(message));
				ret=send(sockfd, se_buf, sizeof(message), 0);
				if(ret==-1) printf("Send error\n");
				else
				{	printf("waiting...\n");
				ret=recv(sockfd, rec_buf, sizeof(message), 0);
				if(ret==-1) printf("Receive error\n");
				else 
				{
					memcpy(&message, rec_buf, sizeof(message));
					if(message.buyb)
						printf("ID: %d, From: %s To: %s, Leave: %4d/%02d/%02d %02d:%02d, Arrive: %4d/%02d/%02d %02d:%02d   Ticketnums: %d, Price: %d   sale\n", message.train_ID, message.start_p, message.dest_p, message.time_sy, message.time_smo, message.time_sd, message.time_sh, message.time_sm, message.time_dy, message.time_dmo, message.time_dd, message.time_dh, message.time_dm, message.ticket_num, message.ticket_total_price);
					else   printf("ID: %d, From: %s To: %s, Leave: %4d/%02d/%02d %02d:%02d, Arrive: %4d/%02d/%02d %02d:%02d   Ticketnums: %d, Price: %d   not sale\n", message.train_ID, message.start_p, message.dest_p, message.time_sy, message.time_smo, message.time_sd, message.time_sh, message.time_sm, message.time_dy, message.time_dmo, message.time_dd, message.time_dh, message.time_dm, message.ticket_num, message.ticket_total_price);

				}
				}
			}
			else  printf("the ID is wrong\n");
		}
		else if(!strcmp(g, "inquireplace"))
		{	
			int i, j;
			printf("the start city: ");
			for(i=0; i<4; i++)
				printf("%s   ", start[i]);
			printf("\nthe destination city: ");
			for(i=0; i<4; i++)
				printf("%s   ", dest[i]);
			printf("\nplease enter the start and destination you want to inquire\n");
			printf("the start city:");
			fgets(s,sizeof(s),stdin);//读入指令
			if(s[strlen(s)-1] == '\n'){
			s[strlen(s)-1] = '\0';
			}
			printf("the destinate city:");
			fgets(d,sizeof(d),stdin);//读入指令
			if(d[strlen(d)-1] == '\n'){
			d[strlen(d)-1] = '\0';
			}
			int ok=0;
			for(i=0; i<4; i++)
			{
				if(!strcmp(s, start[i]))
				{
					for(j=0; j<4; j++)
						if(!strcmp(d, dest[j])) 
						{ok=1; break;}
				}
				if(ok)	break;
			}
			printf("%d\n",ok);	
			if(ok)
			{
				message.msg_type=ONEPLACE;
				strcpy(message.start_p, s);
				strcpy(message.dest_p, d);
				memcpy(se_buf, &message, sizeof(message));
				ret=send(sockfd, se_buf, sizeof(message), 0);
				if(ret==-1) printf("Send error\n");
				else
				{
					printf("waiting...\n");
					ret=recv(sockfd, rec_buf, sizeof(rec_buf), 0);
					if(ret==-1) printf("Receive error\n");
					else
					{
						for(i=0; i<ret; i=i+sizeof(message))
						{
							memcpy(&message, rec_buf+pos, sizeof(message));
							if(message.buyb)
								printf("ID: %d, From: %s To: %s, Leave: %4d/%02d/%02d %02d:%02d, Arrive: %4d/%02d/%02d %02d:%02d   Ticketnums: %d, Price: %d   sale\n", message.train_ID, message.start_p, message.dest_p, message.time_sy, message.time_smo, message.time_sd, message.time_sh, message.time_sm, message.time_dy, message.time_dmo, message.time_dd, message.time_dh, message.time_dm, message.ticket_num, message.ticket_total_price);
							else   printf("ID: %d, From: %s To: %s, Leave: %4d/%02d/%02d %02d:%02d, Arrive: %4d/%02d/%02d %02d:%02d   Ticketnums: %d, Price: %d   not sale\n", message.train_ID, message.start_p, message.dest_p, message.time_sy, message.time_smo, message.time_sd, message.time_sh, message.time_sm, message.time_dy, message.time_dmo, message.time_dd, message.time_dh, message.time_dm, message.ticket_num, message.ticket_total_price);
							pos+=sizeof(message);
						}
					}
				}
			}
			else  printf("There is no such flight\n");
		}     
		else if(!strcmp(g, "buy"))
		{
			printf("please enter ID and ticketnum you want to buy\n");
			scanf("%d%d%c", &id, &ticnum, &ch);
			if(id>0 && id<17)
			{
				message.msg_type=BUY;
				message.train_ID=id;
				message.ticket_num=ticnum;
				memcpy(se_buf, &message, sizeof(message));
				ret=send(sockfd, se_buf, sizeof(message), 0);
				if(ret==-1) printf("Send error\n");
				else
				{
					printf("waiting...\n");
					recv(sockfd, rec_buf, sizeof(message), 0);
					memcpy(&message, rec_buf, sizeof(message));
					if(message.msg_type==BUY_SUCCEED)
					{
						printf("buy succeed\n");
						printf("ID: %d, From: %s To: %s, Leave: %4d/%02d/%02d %02d:%02d, Arrive: %4d/%02d/%02d %02d:%02d   Buyticketnums: %d, Price: %d\n", message.train_ID, message.start_p, message.dest_p, message.time_sy, message.time_smo, message.time_sd, message.time_sh, message.time_sm, message.time_dy, message.time_dmo, message.time_dd, message.time_dh, message.time_dm, message.ticket_num, message.ticket_total_price);
					}
					else
					{
						printf("buy failed\n");
						if(message.buyb)
							printf("ID: %d, From: %s To: %s, Leave: %4d/%02d/%02d %02d:%02d, Arrive: %4d/%02d/%02d %02d:%02d   Remainticketnums: %d, Price: %d\n", message.train_ID, message.start_p, message.dest_p, message.time_sy, message.time_smo, message.time_sd, message.time_sh, message.time_sm, message.time_dy, message.time_dmo, message.time_dd, message.time_dh, message.time_dm, message.ticket_num, message.ticket_total_price);
						else   printf("ID: %d, From: %s To: %s, Leave: %4d/%02d/%02d %02d:%02d, Arrive: %4d/%02d/%02d %02d:%02d   Ticketnums: %d, Price: %d   not sale\n", message.train_ID, message.start_p, message.dest_p, message.time_sy, message.time_smo, message.time_sd, message.time_sh, message.time_sm, message.time_dy, message.time_dmo, message.time_dd, message.time_dh, message.time_dm, message.ticket_num, message.ticket_total_price);

					}
				}
			}
			else printf("the ID is wrong\n");
		}
		else if(!strcmp(g, "return"))
		{
			printf("please enter ID and ticketnum you want to return\n");
			scanf("%d%d%c", &id, &ticnum, &ch);
			if(id>0 && id<17)
			{
				message.msg_type=RETURN;
				message.train_ID=id;
				message.ticket_num=ticnum;
				memcpy(se_buf, &message, sizeof(message));
				ret=send(sockfd, se_buf, sizeof(message), 0);
				if(ret==-1) printf("Send error\n");
				else
				{
					printf("waiting...\n");
					recv(sockfd, rec_buf, sizeof(message), 0);
					memcpy(&message, rec_buf, sizeof(message));
					if(message.msg_type==RETURN_SUCCEED)
					{
						printf("return succeed\n");
						printf("ID: %d, From: %s To: %s, Leave: %4d/%02d/%02d %02d:%02d, Arrive: %4d/%02d/%02d %02d:%02d   Returnticketnums: %d, Price: %d\n", message.train_ID, message.start_p, message.dest_p, message.time_sy, message.time_smo, message.time_sd, message.time_sh, message.time_sm, message.time_dy, message.time_dmo, message.time_dd, message.time_dh, message.time_dm, message.ticket_num, message.ticket_total_price);
					}
					else
					{
						printf("return failed\n");
						if(message.retub)
							printf("ID: %d, From: %s To: %s, Leave: %4d/%02d/%02d %02d:%02d, Arrive: %4d/%02d/%02d %02d:%02d   Remainticketnums: %d, Price: %d\n", message.train_ID, message.start_p, message.dest_p, message.time_sy, message.time_smo, message.time_sd, message.time_sh, message.time_sm, message.time_dy, message.time_dmo, message.time_dd, message.time_dh, message.time_dm, message.ticket_num, message.ticket_total_price);
						else   printf("ID: %d, From: %s To: %s, Leave: %4d/%02d/%02d %02d:%02d, Arrive: %4d/%02d/%02d %02d:%02d   Ticketnums: %d, Price: %d   over time\n", message.train_ID, message.start_p, message.dest_p, message.time_sy, message.time_smo, message.time_sd, message.time_sh, message.time_sm, message.time_dy, message.time_dmo, message.time_dd, message.time_dh, message.time_dm, message.ticket_num, message.ticket_total_price);

					}
				}
			}
			else printf("the ID is wrong\n");
		}

		else  printf("The command is unrecoginsed\n");
	}
	close(sockfd);
	exit(0);

}
