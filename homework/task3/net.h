#ifndef TASK3_NET_H
#define TASK3_NET_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define MAX_TEXT 1024
#define DEFAULT_PORT "3339"

int setup_output(void);
int parse_port(const char *text, int allow_zero, uint16_t *port);
int connect_server(const char *host, const char *service);
int send_frame(int fd, const char *text, size_t length);
/* 返回 1 表示完整消息，0 表示消息边界上的 EOF，-1 表示错误。 */
int receive_frame(int fd, char text[MAX_TEXT + 1], size_t *length);
/* 返回 1 表示一行，0 表示 EOF，-1 表示读错误，-2 表示非法行。 */
int read_text_line(FILE *input, char text[MAX_TEXT + 1], size_t *length);

#endif
