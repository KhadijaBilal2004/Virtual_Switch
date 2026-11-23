#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <signal.h>
#include <sys/select.h>
#include <errno.h>
#include "frame.h"

static int parse_mac(const char *text, uint8_t *mac)
{
    return sscanf(text, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
        &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5]) == 6;
}

int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);
    if (argc != 4) {
        printf("Usage: ./host <switch_ip> <tcp_port> <mac>\n");
        return 1;
    }
    uint8_t mac[6];
    if (!parse_mac(argv[3], mac)) return 1;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    char *end;
    long port = strtol(argv[2], &end, 10);
    if (*argv[2] == '\0' || *end != '\0' || port < 1 || port > 65535 ||
        inet_pton(AF_INET, argv[1], &addr.sin_addr) != 1 || fd < 0) {
        if (fd >= 0) close(fd);
        return 1;
    }
    addr.sin_port = htons((uint16_t)port);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) { close(fd); return 1; }

    char input[1500];
    fd_set set;
    while (1) {
        FD_ZERO(&set);
        FD_SET(fd, &set);
        FD_SET(STDIN_FILENO, &set);
        struct timeval timeout;
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;
        if (select(fd + 1, &set, NULL, NULL, &timeout) < 0) break;
        if (FD_ISSET(STDIN_FILENO, &set)) {
            if (fgets(input, sizeof(input), stdin) == NULL) break;
            int len = strlen(input);
            frame_header_t header;
            memset(&header, 0, sizeof(header));
            header.length = len;
            memcpy(header.src, mac, 6);
            memset(header.dst, 0xff, 6);
            header.vlan = 1;
            header.priority = 1;
            frame_header_t wire;
            serialize_header(&header, &wire);
            if (write_full(fd, &wire, FRAME_HEADER_SIZE) < 0) break;
            if (write_full(fd, input, len) < 0) break;
        }
        if (FD_ISSET(fd, &set)) {
            frame_header_t wire;
            int n = read_full(fd, &wire, FRAME_HEADER_SIZE);
            if (n <= 0) break;
            frame_header_t header;
            deserialize_header(&wire, &header);
            if (header.length > sizeof(input)) break;
            if (header.length > 0) {
                n = read_full(fd, input, header.length);
                if (n != header.length) break;
            }
            fwrite(input, 1, header.length, stdout);
            fflush(stdout);
        }
    }
    close(fd);
    return 0;
}
