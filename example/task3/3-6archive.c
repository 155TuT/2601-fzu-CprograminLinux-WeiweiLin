/*一个简单的员工档案管理系统，可以实现简单的员工资料增加、删除及查询*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARFILE "./usr.ar"//指定档案文件的路径名称

struct arstruct//员工资料结构
{
    char name[10];
    int age;
    char tele[21];
};


/*删除员工函数==================================*/
void removeuser()
{
    char name[10];
    struct arstruct ar;
    FILE *fp;
    FILE *fpn;
    if((fpn = fopen("./tmpfile","w")) == NULL)
    {
        return;
    }
    if((fp = fopen(ARFILE,"r")) == NULL)
    {
        return;
    }
    memset(&ar,0x00,sizeof(ar));//清空结构
    printf("请输入员工姓名:");
    memset(name,0x00,sizeof(name));
    scanf("%s",name);
    while(fread(&ar,sizeof(ar),1,fp) == 1)//循环复制，与输入姓名相匹配的不复制
    {
        if(strcmp(name,ar.name) != 0)
        {
            fwrite(&ar,sizeof(ar),1,fpn);//不相同，则复制
        }
        memset(&ar,0x00,sizeof(ar));
    }
    fclose(fp);
    fclose(fpn);
    remove(ARFILE);//删除原档案文件
    rename("./tmpfile",ARFILE);//复制好的新文件重命名为档案文件
    printf("删除员工资料成功,按任意键继续...\n");

    getchar();//清楚缓冲区残留的\n
    getchar();//等待回车
}

/*查询员工函数==================================*/
void queryuser()
{
    int found;
    char name[10];
    struct arstruct ar;
    FILE *fp;
    if((fp = fopen(ARFILE,"r")) == NULL)
    {
        return;
    }
    memset(&ar,0x00,sizeof(ar));
    printf("请输入员工姓名:");
    memset(name,0x00,sizeof(name));
    scanf("%s",name);
    found=0;
    while(fread(&ar,sizeof(ar),1,fp) == 1)
    {
        if(strcmp(name,ar.name) == 0)
        {
            found=1;
            break;
        }
        memset(&ar,0x00,sizeof(ar));
    }
    fclose(fp);
    if(found)
    {
        printf("姓名=%s\n",ar.name);
        printf("年龄=%d\n",ar.age);
        printf("手机=%s\n",ar.tele);
    }
    else
    {
        printf("没有员工%s的数据\n",name);
    }

    getchar();//清楚缓冲区残留的\n
    getchar();//等待回车
}

/*增加员工函数==================================*/
void insertuser()
{
    struct arstruct ar;
    FILE *fp;
    if((fp = fopen(ARFILE,"a")) == NULL)
    {
        return;
    }
    memset(&ar,0x00,sizeof(ar));
    printf("请输入员工姓名:");
    scanf("%s",ar.name);
    printf("请输入员工年龄:");
    scanf("%d",&(ar.age));
    printf("请输入员工手机号码:");
    scanf("%s",ar.tele);
    if(fwrite(&ar,sizeof(ar),1,fp) < 0)
    {
        perror("fwrite");
        fclose(fp);
        return;
    }
    fclose(fp);
    printf("增加新员工成功,按任意键继续...\n");

    getchar();//清楚缓冲区残留的\n
    getchar();//等待回车
}

/*主程序，输出结果==================================*/
int main(void)
{
    char c;
    while(1)
    {
        printf("\033[2J");//清屏。也可使用system("clear")  
        printf("     *员工档案管理系统*\n");
        printf("---------------------------\n");
        printf("     1.录入新员工档案      \n");
        printf("     2.查看员工档案          \n");
        printf("     3.删除员工档案          \n");
        printf("     0.退出系统            \n");
        printf("---------------------------\n");
        switch((c=getchar()))
        {
            case '1':
                insertuser();
                break;
            case '2':
                queryuser();
                break;
            case '3':
                removeuser();
                break;
            case '0':
                return 0;
            default:
                break;
        }
    }
}

