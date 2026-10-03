#ifndef TASK4_FILES_H
#define TASK4_FILES_H

#include <limits.h>
#include <stddef.h>

/* 两侧共有的文件工具，不依赖服务端会话或生成后端。 */
int t4_path(char result[PATH_MAX], const char *directory, const char *name);
int t4_write_all(int fd, const void *data, size_t length);
int t4_directory_sync(const char *directory);

#endif
