#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/msg.h>
#include <sys/ipc.h>
#include <sys/types.h>
#include <unistd.h>
#define MSGKEY 75

typedef struct Node{					//the message text
	int flag;// flag=1:cHELP,2:conInfo,3:juice number,4:check,5:commnet,6:update,7:rComment
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

struct msgform{						//message structure
	long mtype;
	Node node;
//	char mtext[5001];
};
int numA, numB, numC;
char comm[4][10] = {"good", "normal", "soso", "bad"};


void call_MAIN_GUI(){			//print main GUI		
	puts("                    *                             ");
	puts("                   * *           /                ");
	puts("      .       \\ \\ *   *   / /                   ");
	puts("           \\     *     *     /  /   .            ");
	puts("          \\  \\ \\*       *  / /                 ");
	puts("           \\ \\ *  \\  /   *  / /                ");
	puts("              *   | | |   *                       ");
	puts("             * \\ | | |  /  *                     ");
	puts("*************               ***************       ");
	puts(" *            <<.........>>             *         ");
	puts("   *            .........              *           ");
	puts("     *          .........           *             ");
	puts("       *       ...........        *               ");
	puts("         *    .           .     *                 ");
	puts("         *   .    good     .     *                ");
	puts("        *    .    juice    .      *               ");
	puts("       *     .             .       *              ");
	puts("      *     *  ...........   *      *             ");
	puts("     *    *     .........      *     *            ");
	puts("    *  *         .......          *   *           ");
	puts("   * *           .......             * *          ");
	puts("                .........                         ");
	puts("               ===========                        ");
	puts("==================================================");
	puts("   Enter '1' : begin to shopping	Enter '2' : help ");
	puts("	     Enter '3' administrator login               ");
	puts("==================================================");
};

void callShopping(){					//display object

	struct msgform msg;					//send shopping information to server
    	int msgqid;
	msgqid = msgget(MSGKEY, 0777);
	msg.node.flag = 3;
	msg.mtype = 1;	
	msgsnd(msgqid, &msg, sizeof(msg.node), 0);
	msgrcv(msgqid, &msg, sizeof(msg.node), 1, 0);		//receive the number

	numA = msg.node.na;
	numB = msg.node.nb;
	numC = msg.node.nc; 
		
	puts("=============================================================");
	printf("               HAPPY JUICE PARTY    ");
	printf("\n");
	puts("=============================================================");
	printf("\n");
	printf("  a : coke                 price  2.5$   Number : %d\n\n",numA);
	printf("  b : orange               price  2.5$   Number : %d\n\n",numB);
	printf("  c : milk juice           price  4.0$   Number : %d\n\n",numC);
	puts("=============================================================");
}

void callLastPrint(){					//the end GUI
	puts("                                                                                 ");
	puts("                         *         *          *                                  ");	
	puts("                          *        *         *                                   ");	
	puts("                           *       *        *                                    ");	
	puts("                            *      *       *                                     ");	
	puts("              ********************************************                       ");	
	puts("             |                                            |                      ");	
	puts("             |                                            |                      ");	
	puts("             |           *                   *            |                      ");	
	puts("             |          * *                 * *           |                      ");	
	puts("     ********|         *   *               *   *          |*********             ");	
	puts("     *       |        *     *             *     *         |        *             ");	
	puts("     *       |                                            |        *             ");	
	puts("     *       |                                            |        *             ");	
	puts("     *       |                                            |        *             ");	
	puts("     *       |           ********************             |        *             ");	
	puts("     ********|            *  *  *    *  *  *              |*********             ");	
	puts("             |             * *  *    *  * *               |                      ");	
	puts("             |              **  *    *  **                |                      ");	
	puts("             |               ************                 |                      ");	
	puts("             |                                            |                      ");	
	puts("             |                                            |                      ");	
	puts("             **********************************************                      ");	
	puts("                                                                                 ");	
	puts("================================================================================");	
	puts("                    thank you for your coming!!!                                 ");	
	puts("================================================================================");
}
void adminGUI(){
	puts("**********************************************");
	puts("*                                            *");
	puts("*    1 : update the storehouse               *");
	puts("*                                            *");
	puts("*    2 : examine the comment information     *");
	puts("*                                            *");
	puts("*     any other key to exit	           *");
	puts("**********************************************");
	puts("input your order(1-2) please:");
}

void callManage(){
	adminGUI();
	int order;
	while(scanf("%d", &order)){
		struct msgform msg;					//send shopping information to server
	    	int msgqid;
		msg.mtype = 1;
		msgqid = msgget(MSGKEY, 0777);
		if(order == 1){	
			msg.node.flag = 6;
			msgsnd(msgqid, &msg, sizeof(msg.node), 0);
			msgrcv(msgqid, &msg, sizeof(msg.node), 1, 0);
			if(msg.node.isRight == 1){
				puts("update successfully!");
			}
			else puts("update fail!");	
//			callShopping();
		}
		else if(order == 2){
			msg.node.flag = 7;
			msgsnd(msgqid, &msg, sizeof(msg.node), 0);
			msgrcv(msgqid, &msg, sizeof(msg.node), 1, 0);	
			printf("There %d people's comment\n", msg.node.comArray[0]);
			int ii;
			for(ii = 1; ii <5; ++ii){
				printf("%s :		%d\n", comm[ii-1], msg.node.comArray[ii]);
			}
		}
		else return;
	}
}

void callLogin(){
	char name[20],pwd[20];

	int count = 0;
	struct msgform msg;					
    	int msgqid;
	msgqid = msgget(MSGKEY, 0777);
	msg.node.flag = 4;
	msg.mtype = 1;	
	while(1){
		puts("please enter the username: ");
		scanf("%s", name);
		puts("please enter the password: ");
		scanf("%s", pwd);
		strcpy(msg.node.username, name);
		strcpy(msg.node.password, pwd);
		msgsnd(msgqid, &msg, sizeof(msg.node), 0);
		msgrcv(msgqid, &msg, sizeof(msg.node), 1, 0);	
		int isRight;
		isRight = msg.node.isRight;	
		if(isRight == 0){
			puts("incorrect username or password,pease check again");
			if(++count >= 3)	return;
		}
		else break;
	}
	callManage();	
}

void callComment(){
	puts("===========================================================");
	puts("===========================================================");
	puts("            please comment on our shopping system!        |");
	puts("  *                                                       |");
	puts("    *        *         *                                  |");
	puts("            * *                                           |");
	puts("           *...*      *           1 : good                |");
	puts("          *.....*                                         |");
	puts("         *.......*                2 : normal              |");
	puts("    *   ****   ****                                       |");
	puts("           *....*                 3 : soso                |");
	puts(" *        *......*    *                                   |");
	puts("         *........*               4 : bad                 |");
	puts("        *..........*                                      |");
	puts("  *    *............*     *                               |");
	puts("      *..............*                                    |");
	puts("     ******      ******                                   |");
	puts("          | |  | |                                        |");
	puts("          | |  | |                                        |");
	puts("          | |  | |           thank you very much!!!       |");
	puts("===========================================================");
	puts("===========================================================");
	puts("please enter the number(1-4) to comment:");
	
	int comment;
	scanf("%d", &comment);
	struct msgform msg;					
    	int msgqid;
	msgqid = msgget(MSGKEY, 0777);
	msg.node.flag = 5;
	msg.mtype = 1;	
	msg.node.comment = comment;
	msgsnd(msgqid, &msg, sizeof(msg.node), 0);	
}

int main(){
MPAGE:	call_MAIN_GUI();
	char str[10];
	scanf("%s",str);
	if(str[0] == '2'){					//requst for help
        	struct msgform msg;
    		int msgqid;
		msgqid = msgget(MSGKEY, 0777);			//creat message queue;
		msg.node.flag = 1;
		strcpy(msg.node.mtext,str);
		msg.mtype = 1;	
		msgsnd(msgqid, &msg, sizeof(msg.node), 0);	//send the message to the server
	
		msgrcv(msgqid, &msg, sizeof(msg.node), 1, 0);  //receive message from the server
		printf("%s\n",msg.node.mtext);

		puts("Enter 3 return to the main page");
		int num = 1;
		while(num != 3){
			scanf("%d",&num);
		}

		goto MPAGE;
	}
				
	else if(str[0] == '1'){					//shopping
CALLSHOP:	callShopping();	
		char op[3][3];
		int number[3];
		int kind = 0;					//the number of juice choose
//		double _price;					
		double clientMoney;				//the consumer pay money
		double sumPrice = 0;     			//the total value of juice
		
		struct msgform msg;					//send shopping information to server
    		int msgqid;
		msgqid = msgget(MSGKEY, 0777);
		msg.node.flag = 2;
		
		msg.node.na = 0;
		msg.node.nb = 0;
		msg.node.nc = 0;
	
		while(1){					//the process of the shopping
			puts("please enter a letter to choose your juice('q' to finish):");
SCANFKIND:			scanf("%s",op[kind]);
			if(op[kind][0] != 'a' && op[kind][0] != 'b' && op[kind][0] != 'c' && op[kind][0] != 'q'){
				puts("no this command,please check carefully and enter again");
				goto SCANFKIND;
			}
			
			if(op[kind][0] == 'q')	break;
			puts("please enter a number you want to buy:");
			scanf("%d",&number[kind]);
			if(op[kind][0] == 'a'){			//coke	
				clientMoney = 2.5;
				msg.node.na = number[kind];
			}
			else if(op[kind][0] == 'b'){		//orange
				clientMoney = 2.5;
				msg.node.nb = number[kind];
			}
			else{					//milk juice
				clientMoney = 4.0;
				msg.node.nc = number[kind];
			}
			sumPrice += clientMoney * number[kind];	//calcuate the total value
			kind++;
		}
		
		int ii;
		puts("this are what you want to buy: ");					//print shopping information
		for(ii = 0; ii <kind; ++ii){
			if(op[ii][0] == 'a')	printf("coke:	");
			else if(op[ii][0] == 'b')	printf("orange:	");
			else printf("milk juice:	");
			printf("%d\n", number[ii]);
		}
		printf("you have to pay %.1lf total\n", sumPrice);
		
		puts("enter 'e' to ensure you deal or any other letter to change your choose:");//ensure your deal		
		char ensure[5];
		scanf("%s", ensure);
		if(ensure[0] == 'e' || ensure[0] == 'E');
		else goto CALLSHOP;
	
		puts("please enter the money you want to offer:");	//afford
		scanf("%lf",&clientMoney);
		while(clientMoney < sumPrice){
			puts("Sorry, you have to insert more money.Please add the money");
			double temp;
			scanf("%lf",&temp);
			clientMoney += temp;
		}

		msg.node.sumPrice = sumPrice;
		msg.node.clientMoney = clientMoney;
		msg.mtype = 1;	
		msgsnd(msgqid, &msg, sizeof(msg.node), 0);

		msgrcv(msgqid, &msg, sizeof(msg.node), 1, 0);		//print the change 	
		numA = msg.node.na;
		numB = msg.node.nb;
		numC = msg.node.nc;
	
		printf("the change moeny is %s\n", msg.node.chg);

		puts("please enter 'y' to make sure you deal");		//wait to ensure
		char last[2] = "a";
		while(1){
			scanf("%s",last);
			if(last[0] == 'y' || last[0] == 'Y'){
				callComment();
				callLastPrint();
				sleep(3);
				goto MPAGE;
			} 
		}
	}
	else if(str[0] == '3'){
		 callLogin();
		 goto MPAGE;
	}
	else goto MPAGE;	
}
