#include "net.h"

#include <arpa/inet.h>
#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int main(int argc, char *argv[])
{
    const char *service = argc > 1 ? argv[1] : DEFAULT_PORT;
    const char *log_path = argc > 2 ? argv[2] : "output/server-records.txt";
    const char *bind_ip = argc > 3 ? argv[3] : "127.0.0.1";
    uint16_t port;
    struct sockaddr_in address = {0}, peer;
    socklen_t address_size = sizeof(address), peer_size = sizeof(peer);
    int listener = -1, client = -1, result = 1, reuse = 1;
    FILE *records = NULL;
    size_t sequence = 0;
    char request[MAX_TEXT + 1], reply[MAX_TEXT + 1];

    if (argc > 4 || parse_port(service, 1, &port) == -1) {
        fprintf(stderr, "Usage: %s [port:0..65535 [log-file [bind-ipv4]]]\n", argv[0]);
        return 2;
    }
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (inet_pton(AF_INET, bind_ip, &address.sin_addr) != 1) {
        fprintf(stderr, "Invalid bind IPv4 address: %s\n", bind_ip);
        return 2;
    }
    if (setup_output() == -1) {
        return 1;
    }
    listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener == -1 ||
        setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) == -1 ||
        bind(listener, (struct sockaddr *)&address, sizeof(address)) == -1 ||
        listen(listener, 5) == -1 ||
        getsockname(listener, (struct sockaddr *)&address, &address_size) == -1) {
        perror("create/listen socket");
        goto cleanup;
    }
    /* 追加保存已收到的请求与已发送的回复，不覆盖上次运行的记录。 */
    records = fopen(log_path, "a");
    if (records == NULL) {
        perror("fopen (server log)");
        goto cleanup;
    }
    if (fprintf(records, "session: pid=%ld, listen=%s:%u\n", (long)getpid(),
                bind_ip, (unsigned)ntohs(address.sin_port)) < 0 ||
        fflush(records) == EOF) {
        perror("write server log");
        goto cleanup;
    }
    printf("[server] pid=%ld, listening on %s:%u\n", (long)getpid(),
           bind_ip, (unsigned)ntohs(address.sin_port));
    printf("[server] log=%s; waiting for one client session.\n", log_path);
    do {
        client = accept(listener, (struct sockaddr *)&peer, &peer_size);
    } while (client == -1 && errno == EINTR);
    if (client == -1) {
        perror("accept");
        goto cleanup;
    }
    /* 本实验一次运行服务一个连接，监听队列长度不代表并发服务数量。 */
    close(listener);
    listener = -1;
    char peer_ip[INET_ADDRSTRLEN];
    if (inet_ntop(AF_INET, &peer.sin_addr, peer_ip, sizeof(peer_ip)) == NULL) {
        perror("inet_ntop");
        goto cleanup;
    }
    printf("[server] connected: %s:%u\n", peer_ip, (unsigned)ntohs(peer.sin_port));

    for (;;) {
        size_t length;
        int received = receive_frame(client, request, &length);
        if (received == 0) {
            printf("[server] client EOF; processed %zu messages.\n", sequence);
            result = 0;
            break;
        }
        if (received == -1) {
            perror("receive request");
            goto cleanup;
        }
        ++sequence;
        for (size_t i = 0; i < length; ++i) {
            unsigned char c = (unsigned char)request[i];
            reply[i] = (char)(c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c);
        }
        reply[length] = '\0';
        printf("[server] recv #%zu (%zu bytes): %s\n", sequence, length, request);
        if (fprintf(records, "recv #%zu (%zu bytes): %s\n", sequence, length, request) < 0 ||
            fflush(records) == EOF) {
            perror("save request");
            goto cleanup;
        }
        if (send_frame(client, reply, length) == -1) {
            perror("send reply");
            goto cleanup;
        }
        printf("[server] send #%zu (%zu bytes): %s\n", sequence, length, reply);
        if (fprintf(records, "send #%zu (%zu bytes): %s\n", sequence, length, reply) < 0 ||
            fflush(records) == EOF) {
            perror("save reply");
            goto cleanup;
        }
    }

cleanup:
    if (client != -1) {
        close(client);
    }
    if (listener != -1) {
        close(listener);
    }
    if (records != NULL && fclose(records) == EOF) {
        perror("fclose (server log)");
        result = 1;
    }
    if (result == 0) {
        printf("[server] records saved; sockets closed.\n");
    }
    return result;
}
