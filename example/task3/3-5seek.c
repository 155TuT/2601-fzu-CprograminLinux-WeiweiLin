/*流文件指针位置的定位操作*/
/*程序中创建出的文件非纯文本文件，可使用“od -td1 -tc -Ad 文件名”方式查看其内容 */
/*-td1选项表示将文件中的字节以十进制的形式列出来，每组一个字节。
-tc选项表示将文件中的ASCII码以字符形式列出来。
输出结果最左边的一列是文件中的地址，-Ad选项要求以十进制显示文件中的地址。*/

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>

struct stu
{
	char name[10];
	int age;	
};

int main(void)
{
   struct stu mystu[3]={{"Jim",14},{"Jam",15},{"Lily",19}};
   struct stu mystuout;
   FILE *fp;
   extern int errno; 
   char file[]="record.txt";
   int i,j;
   long k;
   fpos_t pos1,pos2;
  
   fp=fopen(file,"w");
   if(fp==NULL)
   {
   	  printf("cant't open file %s.\n",file);
   	  printf("errno:%d\n",errno);
   	  printf("ERR  :%s\n",strerror(errno));
   	  return(1);
   	}
   	else
   	{
   		printf("%s was opened.\n",file);   		
	}
 
    i=fwrite(mystu,sizeof(struct stu),3,fp);//创建的文件内容为二进制文件，非纯文本文件.
    printf("%d students was written.\n",i);
    fclose(fp);

/*以下为按指定要求读出记录*/

   fp=fopen(file,"r");
   if(fp==NULL)
   {
   	  printf("cant't open file %s.\n",file);
   	  printf("errno:%d\n",errno);
   	  printf("ERR  :%s\n",strerror(errno));
   	  return(1);
   }


   k=ftell(fp);//ftell函数可得到当前文件指针位置
   printf("当前指针位置为%ld .\n",k);

   fseek(fp,1*sizeof(struct stu),SEEK_SET);//从文件开始（SEEK_SET）移动指针至1个结构体的偏移量
   
   fgetpos(fp,&pos1);//另外一种得到当前文件指针位置的方法
   printf("移动指针后的当前指针位置为%f .\n",(float)pos1.__pos);
  
   j=fread(&mystuout,sizeof(struct stu),1,fp);//从文件流中读1个结构体的长度的内容至mystuout
   printf("%d students was read.\n",j);
   printf("NAME:%s\tAGE:%d\n",mystuout.name,mystuout.age);

   k=ftell(fp);//得到当前文件指针位置
   printf("读出记录后的当前指针位置为%ld .\n",k);

   j=fread(&mystuout,sizeof(struct stu),1,fp);
   printf("%d students was read.\n",j);
   printf("NAME:%s\tAGE:%d\n",mystuout.name,mystuout.age);

   pos2.__pos=(long)(1*sizeof(struct stu));//设置移动量为一个结构体
   fsetpos(fp,&pos2);//另外一种移动文件指针位置的方法
  
   k=ftell(fp);
   printf("再次移动指针后的当前指针位置为%ld .\n",k);

   j=fread(&mystuout,sizeof(struct stu),1,fp);
   printf("%d students was read.\n",j);
   printf("NAME:%s\tAGE:%d\n",mystuout.name,mystuout.age);

   k=ftell(fp);
   printf("再次读记录后的当前指针位置为%ld .\n",k);

   fclose(fp);

}

