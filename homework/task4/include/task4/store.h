#ifndef TASK4_STORE_H
#define TASK4_STORE_H

#include <limits.h>
#include <stdint.h>
#include <stddef.h>
#include "task4/backend.h"
#include "task4/files.h"

struct t4_session {
    char token[33];
    char directory[PATH_MAX];
    char context[PATH_MAX];
    int lock_fd;
    int log_fd;
};

/* 同一会话只允许一个连接持有锁；空 token 创建独立会话。 */
int t4_session_open(const char *root, const char *token, struct t4_session *s);
void t4_session_close(struct t4_session *s);
int t4_event(struct t4_session *s, const char *event, uint32_t request, uint32_t offset);
/* 检查顺序及重试 prompt，返回持久回复长度、完成标志和本轮目录。 */
int t4_turn_open(struct t4_session *s, uint32_t request,
                 const unsigned char *prompt, uint32_t length,
                 char directory[PATH_MAX], uint32_t *cached, int *done,
                 const char *corpus, const struct t4_backend *backend);
int t4_turn_done(const char *directory);

#endif
