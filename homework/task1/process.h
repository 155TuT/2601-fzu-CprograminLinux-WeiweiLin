#ifndef PROCESS_H
#define PROCESS_H

#include <sys/types.h>

/* 父、子进程分别执行若干步任务，用来观察并发输出。 */
void do_work(const char *role, int steps);

/* 子进程通知父进程后调用 exec；仅在出错时返回退出码。 */
int run_child(pid_t parent_pid, char *const command[]);

#endif
