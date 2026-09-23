#include "net.h"

#include <errno.h>
#include <sys/socket.h>
#include <unistd.h>

int main(int argc, char *argv[])
{
    const char *host = argc > 1 ? argv[1] : "127.0.0.1";
    const char *service = argc > 2 ? argv[2] : DEFAULT_PORT;
    const char *input_path = argc > 3 ? argv[3] : "data/messages.txt";
    const char *output_path = argc > 4 ? argv[4] : "output/replies.txt";
    uint16_t port;
    FILE *input = NULL, *output = NULL;
    int fd = -1, result = 1;
    size_t line = 0, sequence = 0, skipped = 0;
    char request[MAX_TEXT + 1], reply[MAX_TEXT + 1];

    if (argc > 5 || parse_port(service, 0, &port) == -1) {
        fprintf(stderr, "Usage: %s [host [port:1..65535 [input-file [new-output-file]]]]\n",
                argv[0]);
        return 2;
    }
    if (setup_output() == -1) {
        return 1;
    }
    input = fopen(input_path, "r");
    if (input == NULL) {
        perror("fopen (input)");
        goto cleanup;
    }
    /* C11 的 x 模式独占创建，避免误覆盖输入文件或已有结果。 */
    output = fopen(output_path, "wx");
    if (output == NULL) {
        perror("fopen (new output; choose a non-existing file)");
        goto cleanup;
    }
    fd = connect_server(host, service);
    if (fd == -1) {
        goto cleanup;
    }
    printf("[client] pid=%ld, connected to %s:%u\n", (long)getpid(), host, (unsigned)port);
    printf("[client] input=%s; output=%s\n", input_path, output_path);
    for (;;) {
        size_t length, reply_length;
        int read_result = read_text_line(input, request, &length);
        if (read_result == 0) {
            break;
        }
        ++line;
        if (read_result == -1) {
            perror("read input file");
            goto cleanup;
        }
        if (read_result == -2) {
            fprintf(stderr, "Input line #%zu invalid (max %d bytes, no NUL); skipped.\n",
                    line, MAX_TEXT);
            ++skipped;
            continue;
        }
        ++sequence;
        printf("[client] send #%zu (%zu bytes): %s\n", sequence, length, request);
        if (send_frame(fd, request, length) == -1) {
            perror("send request");
            goto cleanup;
        }
        int received = receive_frame(fd, reply, &reply_length);
        if (received != 1) {
            if (received == 0) {
                errno = EPROTO; /* 当前请求尚未收到回复，EOF 属于异常。 */
            }
            perror("receive reply");
            goto cleanup;
        }
        /* fwrite 按实际长度写入；每条回复在结果文件中占一行。 */
        if (fwrite(reply, 1, reply_length, output) != reply_length ||
            fputc('\n', output) == EOF || fflush(output) == EOF) {
            perror("save reply file");
            goto cleanup;
        }
        printf("[client] recv #%zu (%zu bytes): %s\n", sequence, reply_length, reply);
    }
    /* 半关闭发送方向，服务器在消息边界读到 EOF 后关闭连接。 */
    if (shutdown(fd, SHUT_WR) == -1) {
        perror("shutdown");
        goto cleanup;
    }
    size_t extra_length;
    int received = receive_frame(fd, reply, &extra_length);
    if (received != 0) {
        if (received == 1) {
            errno = EPROTO;
        }
        perror("wait for server EOF");
        goto cleanup;
    }
    printf("[client] input EOF; saved %zu replies, skipped %zu lines.\n", sequence, skipped);
    result = 0;

cleanup:
    if (fd != -1) {
        close(fd);
    }
    if (input != NULL && fclose(input) == EOF) {
        perror("fclose (input)");
        result = 1;
    }
    if (output != NULL && fclose(output) == EOF) {
        perror("fclose (output)");
        result = 1;
    }
    return result;
}
