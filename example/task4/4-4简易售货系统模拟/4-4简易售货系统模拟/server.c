#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/msg.h>
#include <sys/ipc.h>
#include <sys/types.h>
#include <signal.h>
#define MSGKEY 75



typedef struct Node{
	int flag;				// flag=1:cHELP,-1:sHELP;2:conInfo,-2:chgInfo
	char mtext[5001];
	char chg[200];
	char username[20];
	char password[20];	
	double clientMoney, sumPrice;
	int na, nb, nc;
	int isRight;
	int comment;
	int comArray[5];
}Node;

struct msgform{
	long mtype;
	Node node;
//	char mtext[5001];
}msg;

int numA = 100;
int numB = 100;
int numC = 100;
int comment[5] = {0};

char username[] = "fzulinux";
char password[] = "helloworld";

int msgqid;
char helpPage[5001];
void getHelp(){					//read help from the help page(file operation)		
	char fn[] = "help.dat";
	FILE *fd;
	fd = fopen(fn, "r");   
	if(fd == NULL){ 
		printf("error!\n");
		return;
	}
	int i = fread(helpPage,1,5001,fd);
	helpPage[i] = 0;
	fclose(fd);
//	printf("%s", helpPage);
} 

char res[100] = "";
void change(double a,double b)   	     //calculate the change(the maximum money you afford can not larger than 100)
{
	strcpy(res, "");
	int i;                    
	a=a-b;
	int j=a,k=j;

	if(a==0) strcat(res, "0");	

	if(j-50>0)				
	{
		strcat(res, "1*50");
		j=j-50;
	}

	i=j/10;
	if(i>0)
	{
		char s1[20];
		s1[0] = i+'0';
		s1[1] = '\0';
		strcat(res, " ");
		strcat(res,strcat(s1,"*10"));
		j=j-i*10;
	}

	i=j/5;
	if(i>0)
	{
		char s1[20];
		s1[0] = i+'0';
		s1[1] = '\0';
		strcat(res, " ");
		strcat(res,strcat(s1,"*5"));
		j=j-i*5;
	}

	i=j/1;
	if(i>0)
	{
		char s1[20];
		s1[0] = i+'0';
		s1[1] = '\0';
		strcat(res, " ");
		strcat(res,strcat(s1,"*1"));
		j=j-i*1;
	}

	if((a-k)!=0)
	   strcat(res, " 1*0.5");
}

int main(){
	int i;
	extern void cleanup();			//software interapter
    //cleanup();			//software interapter
	for(i = 0; i < 20; ++i){
		signal(i, cleanup);
	}
	
	msgqid = msgget(MSGKEY, 0777|IPC_CREAT);
	for(;;){
		msgrcv(msgqid, &msg, sizeof(msg.node), 1, 0);		//receive message from the client
									//judge the message type
		if(msg.node.flag == 1){					//the help information
//			printf("receive %s\n", msg.node.mtext);
			puts("the client request for help");
			getHelp();
			strcpy(msg.node.mtext,helpPage);
			msg.mtype = 1;
			msgsnd(msgqid, &msg, sizeof(msg.node), 0);
		}
		else if(msg.node.flag ==2){				//the shopping information
			puts("the client have choose what he or she want:");

			printf("coke : 	%d\n", msg.node.na);
			printf("orange : %d\n", msg.node.nb);
			printf("milk juice : %d\n",msg.node.nc);

			numA -=msg.node.na;
			numB -=msg.node.nb;
			numC -=msg.node.nc;
			
			double aa = msg.node.clientMoney;
			double bb = msg.node.sumPrice;
			change(aa, bb);
			strcpy(msg.node.chg,res);
			msg.mtype = 1;
			
			msg.node.na = numA;
			msg.node.nb = numB;
			msg.node.nc = numC;

			msgsnd(msgqid, &msg, sizeof(msg.node), 0);
		}
		else if(msg.node.flag == 3){
			puts("client request for the number of the juice");
			msg.mtype = 1;
			msg.node.na = numA;
			msg.node.nb = numB;
			msg.node.nc = numC;	
			msgsnd(msgqid, &msg, sizeof(msg.node), 0);		
		}
		else if(msg.node.flag == 4){
 			puts("The client is trying to login");
			msg.mtype = 1;			
			if(strcmp(msg.node.username, username) != 0){
				msg.node.isRight = 0;
				puts("login fail");
			}
			else if(strcmp(msg.node.password, password) != 0){
				msg.node.isRight = 0;
				puts("login fail");
			}
			else msg.node.isRight = 1;
			msgsnd(msgqid, &msg, sizeof(msg.node), 0);
			puts("login sucess");
		}
		else if(msg.node.flag == 5){
			puts("The client has just gave a comment");
			comment[0]++;
			comment[msg.node.comment]++;	
		}
		else if(msg.node.flag == 6){
			puts("The admin request for adding drinks");
			numA = 100;
			numB = 100;
			numC = 100;
			printf(" numA = %d\n numB = %d\n numC = %d\n", numA, numB, numC);
			msg.mtype = 1;
			msg.node.isRight = 1;
			msgsnd(msgqid, &msg, sizeof(msg.node), 0);
		}
		else if(msg.node.flag ==7){
			puts("The admin is checking the record of comments");
			msg.mtype = 1;
			int ii;
			for(ii = 0; ii < 5; ++ii){
				msg.node.comArray[ii] = comment[ii];
			}
			msgsnd(msgqid, &msg, sizeof(msg.node), 0);
		}
	}
}

void cleanup(){
	msgctl(msgqid, IPC_RMID, 0);	
}

