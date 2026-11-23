#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <stdlib.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <sys/mman.h>
#include "shm_stats.h"

#define UNIX_SOCKET "/tmp/vswitch_khadija.sock"

static volatile sig_atomic_t watching = 1;

static void stop_watching(int signal_number)
{
    (void)signal_number;
    watching = 0;
}

static void usage(void)
{
    fprintf(stderr,
        "Usage: ./swctl <command>\n"
        "  show mac\n"
        "  show ports\n"
        "  shutdown <port>\n"
        "  up <port>\n"
        "  vlan <port> <vlan>\n"
        "  mirror <port> <sniffer_port>\n"
        "  clear mac\n"
        "  clear stats\n"
        "  stats [--watch]\n");
}

static int send_command(const char *command)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", UNIX_SOCKET);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "switch is not running\n");
        close(fd);
        return -1;
    }

    size_t length = strlen(command);
    size_t sent = 0;
    while (sent < length) {
        ssize_t n = write(fd, command + sent, length - sent);
        if (n > 0) {
            sent += (size_t)n;
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            perror("write");
            close(fd);
            return -1;
        }
    }

    char buffer[1024];
    ssize_t n;
    int read_error = 0;
    for (;;) {
        n = read(fd, buffer, sizeof(buffer) - 1);
        if (n > 0) {
            buffer[n] = '\0';
            printf("%s", buffer);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) {
            perror("read");
            read_error = 1;
        }
        break;
    }

    close(fd);
    return read_error ? -1 : 0;
}

static int parse_port(const char *text)
{
    char *end;
    long value = strtol(text, &end, 10);
    return *text != '\0' && *end == '\0' && value >= 0 && value < MAX_PORTS;
}

static int parse_vlan(const char *text)
{
    char *end;
    long value = strtol(text, &end, 10);
    return *text != '\0' && *end == '\0' && value >= 1 && value <= 4094;
}

static int valid_command(int argc, char **argv)
{
    if (argc == 1) return 1;
    if (strcmp(argv[1], "show") == 0)
        return argc == 3 && (strcmp(argv[2], "mac") == 0 || strcmp(argv[2], "ports") == 0);
    if (strcmp(argv[1], "shutdown") == 0 || strcmp(argv[1], "up") == 0)
        return argc == 3 && parse_port(argv[2]);
    if (strcmp(argv[1], "vlan") == 0)
        return argc == 4 && parse_port(argv[2]) && parse_vlan(argv[3]);
    if (strcmp(argv[1], "mirror") == 0)
        return argc == 4 && parse_port(argv[2]) && parse_port(argv[3]);
    if (strcmp(argv[1], "clear") == 0)
        return argc == 3 && (strcmp(argv[2], "mac") == 0 || strcmp(argv[2], "stats") == 0);
    if (strcmp(argv[1], "stats") == 0)
        return argc == 2 || (argc == 3 && strcmp(argv[2], "--watch") == 0);
    return 0;
}

static void print_stats(shm_stats_t *stats)
{
    port_stats_t ports[MAX_PORTS];
    pthread_mutex_lock(&stats->lock);
    int count = stats->num_ports < MAX_PORTS ? stats->num_ports : MAX_PORTS;
    memcpy(ports, stats->ports, (size_t)count * sizeof(*ports));
    pthread_mutex_unlock(&stats->lock);
    for (int i = 0; i < count; i++) {
        printf("port %d in %llu out %llu dropped %llu mirrored %llu\n", i,
            (unsigned long long)ports[i].frames_in,
            (unsigned long long)ports[i].frames_out,
            (unsigned long long)ports[i].frames_dropped,
            (unsigned long long)ports[i].frames_mirrored);
    }
}

int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);
    if (!valid_command(argc, argv)) {
        usage();
        return 1;
    }

    char command[256] = "stats\n";
    if (argc > 1) {
        command[0] = '\0';
        for (int i = 1; i < argc; i++) {
            if (i > 1) strncat(command, " ", sizeof(command) - strlen(command) - 1);
            strncat(command, argv[i], sizeof(command) - strlen(command) - 1);
        }
        strncat(command, "\n", sizeof(command) - strlen(command) - 1);
    }

    if (argc >= 2 && strcmp(argv[1], "stats") == 0) {
        if (argc == 3 && strcmp(argv[2], "--watch") == 0) {
            shm_stats_t *stats = shm_stats_attach();
            if (!stats) {
                fprintf(stderr, "switch stats are not available\n");
                return 1;
            }
            struct sigaction action;
            memset(&action, 0, sizeof(action));
            action.sa_handler = stop_watching;
            sigemptyset(&action.sa_mask);
            sigaction(SIGINT, &action, NULL);
            sigaction(SIGTERM, &action, NULL);
            while (watching) {
                print_stats(stats);
                fflush(stdout);
                sleep(1);
                if (watching) printf("\n");
            }
            munmap(stats, sizeof(shm_stats_t));
            return 0;
        }

        shm_stats_t *stats = shm_stats_attach();
        if (stats) {
            print_stats(stats);
            munmap(stats, sizeof(shm_stats_t));
            return 0;
        }
    }

    return send_command(command) == 0 ? 0 : 1;
}
