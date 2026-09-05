/*基于流的I/O操作--打开关闭文件流的操作*/
/*基于流的I/O操作比基于文件描述符的I/O操作更简单方便一些*/
/*参阅《LinuxC编程一站式学习》第25章C标准库第2节标准IO库函数的内容*/

#include <stdio.h>
#include <fcntl.h>
#include <stdlib.h>

int main(void)
{
    FILE *fp;
    int fd;
    
    if( (fp = fopen("hello.txt", "w")) == NULL)
    {   /*以只写方式打开文件，并清空文件。若没有此文件，则创建它。路径为当前目录下，也可使用绝对路径。打开成功返回文件指针。--基于流的方式*/
        printf ("fail to open 1!\n");
        exit(1);          /*出错退出*/
    }
    fprintf(fp, "Hello! I like Linux C program!\n"); 
        /*向该流输出一段信息，这段信息会保存到打开的文件上，形成文件文件*/
    fclose(fp);               /*操作完毕，关闭流*/
    
    if( (fd = open("hello.txt", O_RDWR)) == -1)
      {  /*以读写的方式打开文件--基于文件描述符的方式*/
         printf ("fail to open!\n");
         exit(1);         /*出错退出*/
      }
    
    if((fp = fdopen(fd, "a+")) == NULL)
      {  /*在打开的文件上打开一个流，基于已存在的文件描述符打开流，并从文件尾开始读写。*/
        /*其中w代表write，r代表read，a代表append，+代表更新*/
         printf ("fail to open stream!\n");
         exit(1);         /*出错退出*/
      }
    fprintf(fp, "I am doing Linux C programs!\n");
         /*向该流输出一段信息，这段信息会保存到打开的文件上*/
    fclose(fp);             /*关闭流，文件也被关闭*/
    system("cat hello.txt");//使用cat命令显示文件内容
    return 0;
}
