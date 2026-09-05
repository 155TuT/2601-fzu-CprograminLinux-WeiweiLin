/*room.h*/
#ifndef __ROOM_H
#define __ROOM_H
#include "globalapi.h"

#define ROOM_NUM 6 //教学楼总数

/*教室的一个简单描述，room_id表示楼号，room_num表示剩余教室数*/
typedef struct room_struct_t {
	int room_id;
	int room_num;
	
	pthread_mutex_t room_mutex;
} room_struct;
room_struct room_list[ROOM_NUM];

/*init_room_list:初始化room_list数组*/
void init_room_list()
{
	FILE *fp;
	fp=fopen("room.txt","r");
	int id,num;
	int i;
	for(i=0; i<ROOM_NUM;i++) {
		fscanf(fp,"%d %d\n",&id,&num);
		room_list[i].room_id=id;
		room_list[i].room_num=num;
		
	}
	fclose(fp);
}


#endif
