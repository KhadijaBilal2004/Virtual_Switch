#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include "frame.h"
#include "mac_table.h"
#include "shm_stats.h"
#include "signals.h"

#define QUEUE_SIZE 64
#define UNIX_SOCKET "/tmp/vswitch_khadija.sock"
#define MAX_PORTS 32
#define TOKEN_RATE 100
#define MAX_FRAME_PAYLOAD 1500

typedef struct frame_packet {
    frame_header_t header;
    uint8_t payload[MAX_FRAME_PAYLOAD];
    int ref;
    pthread_mutex_t ref_lock;
} frame_packet_t;

typedef struct queue_node {
    frame_packet_t *frame;
    int mirrored;
    struct queue_node *next;
} queue_node_t;

typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    queue_node_t *head;
    queue_node_t *tail;
    int count;
    int stopped;
} frame_queue_t;

typedef struct {
    int fd;
    int active;
    int enabled;
    int slot_reserved;
    int receive_started;
    int sender_started;
    int vlan;
    int trunk;
    int mirror_from;
    pthread_t receive_thread;
    pthread_t sender_thread;
    frame_queue_t queues[3];
    int tokens;
    pthread_mutex_t token_lock;
    pthread_cond_t token_cond;
    pthread_mutex_t work_lock;
    pthread_cond_t work_cond;
} port_t;

typedef struct log_node {
    char text[256];
    struct log_node *next;
} log_node_t;

typedef struct {
    int listen_fd;
    int unix_fd;
    int num_ports;
    port_t ports[MAX_PORTS];
    sem_t free_ports;
    mac_table_t table;
    shm_stats_t *stats;
    atomic_int keep_alive;
    pthread_mutex_t port_lock;
    pthread_mutex_t lifecycle_lock;
    pthread_t signal_thread;
    pthread_t admin_thread;
    pthread_t maintenance_thread;
    pthread_t logger_thread;
    pthread_mutex_t log_lock;
    pthread_cond_t log_cond;
    log_node_t *log_head;
    log_node_t *log_tail;
} switch_t;

static switch_t sw;
static void stats_drop(int port_no);

static void frame_release(frame_packet_t *frame)
{
    int release;
    pthread_mutex_lock(&frame->ref_lock);
    release = --frame->ref == 0;
    pthread_mutex_unlock(&frame->ref_lock);
    if (release) {
        pthread_mutex_destroy(&frame->ref_lock);
        free(frame);
    }
}

static void frame_add_ref(frame_packet_t *frame)
{
    pthread_mutex_lock(&frame->ref_lock);
    frame->ref++;
    pthread_mutex_unlock(&frame->ref_lock);
}

static void queue_init(frame_queue_t *queue)
{
    pthread_mutex_init(&queue->lock, NULL);
    pthread_cond_init(&queue->cond, NULL);
    queue->head = queue->tail = NULL;
    queue->count = 0;
    queue->stopped = 0;
}

static int queue_push(frame_queue_t *queue, frame_packet_t *frame, int mirrored)
{
    queue_node_t *node = malloc(sizeof(*node));
    if (!node) return -1;
    node->frame = frame;
    node->mirrored = mirrored;
    node->next = NULL;
    pthread_mutex_lock(&queue->lock);
    if (queue->stopped || queue->count >= QUEUE_SIZE) {
        pthread_mutex_unlock(&queue->lock);
        free(node);
        return -1;
    }
    if (queue->tail) queue->tail->next = node;
    else queue->head = node;
    queue->tail = node;
    queue->count++;
    pthread_cond_signal(&queue->cond);
    pthread_mutex_unlock(&queue->lock);
    return 0;
}

static frame_packet_t *queue_try_pop(frame_queue_t *queue, int *mirrored)
{
    pthread_mutex_lock(&queue->lock);
    queue_node_t *node = queue->head;
    if (node) {
        queue->head = node->next;
        if (!queue->head) queue->tail = NULL;
        queue->count--;
    }
    pthread_mutex_unlock(&queue->lock);
    if (!node) return NULL;
    frame_packet_t *frame = node->frame;
    *mirrored = node->mirrored;
    free(node);
    return frame;
}

static int queues_have_frames(port_t *port)
{
    for (int i = 0; i < 3; i++) {
        pthread_mutex_lock(&port->queues[i].lock);
        int count = port->queues[i].count;
        pthread_mutex_unlock(&port->queues[i].lock);
        if (count) return 1;
    }
    return 0;
}

static void queue_stop(frame_queue_t *queue)
{
    pthread_mutex_lock(&queue->lock);
    queue->stopped = 1;
    pthread_cond_broadcast(&queue->cond);
    pthread_mutex_unlock(&queue->lock);
}

static void queue_drain(frame_queue_t *queue, int port_no)
{
    pthread_mutex_lock(&queue->lock);
    queue_node_t *node = queue->head;
    queue->head = queue->tail = NULL;
    queue->count = 0;
    pthread_mutex_unlock(&queue->lock);
    while (node) {
        queue_node_t *next = node->next;
        frame_release(node->frame);
        stats_drop(port_no);
        free(node);
        node = next;
    }
}

static void log_enqueue(const char *text)
{
    log_node_t *node = malloc(sizeof(*node));
    if (!node) return;
    snprintf(node->text, sizeof(node->text), "%s", text);
    node->next = NULL;
    pthread_mutex_lock(&sw.log_lock);
    if (sw.log_tail) sw.log_tail->next = node;
    else sw.log_head = node;
    sw.log_tail = node;
    pthread_cond_signal(&sw.log_cond);
    pthread_mutex_unlock(&sw.log_lock);
}

static void *logger_loop(void *arg)
{
    (void)arg;
    FILE *file = fopen("khadija_switch.log", "a");
    for (;;) {
        pthread_mutex_lock(&sw.log_lock);
        while (!sw.log_head && atomic_load(&sw.keep_alive))
            pthread_cond_wait(&sw.log_cond, &sw.log_lock);
        log_node_t *node = sw.log_head;
        if (node) {
            sw.log_head = node->next;
            if (!sw.log_head) sw.log_tail = NULL;
        }
        int done = !node && !atomic_load(&sw.keep_alive);
        pthread_mutex_unlock(&sw.log_lock);
        if (done) break;
        if (node) {
            time_t now = time(NULL);
            struct tm tm_now;
            char timestamp[32] = "unknown-time";
            if (localtime_r(&now, &tm_now)) strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &tm_now);
            if (file) fprintf(file, "%s %s\n", timestamp, node->text);
            free(node);
        }
    }
    if (file) fclose(file);
    return NULL;
}

static void stats_lock(void)
{
    if (sw.stats) pthread_mutex_lock(&sw.stats->lock);
}

static void stats_unlock(void)
{
    if (sw.stats) pthread_mutex_unlock(&sw.stats->lock);
}

static void stats_drop(int port_no)
{
    stats_lock();
    if (sw.stats && port_no >= 0 && port_no < sw.stats->num_ports)
        sw.stats->ports[port_no].frames_dropped++;
    stats_unlock();
}

static int port_is_active(int port_no)
{
    pthread_mutex_lock(&sw.port_lock);
    int active = sw.ports[port_no].active;
    pthread_mutex_unlock(&sw.port_lock);
    return active;
}

static int port_snapshot(int port_no, int *vlan, int *trunk, int *mirror_from)
{
    pthread_mutex_lock(&sw.port_lock);
    port_t *port = &sw.ports[port_no];
    int active = port->active;
    if (vlan) *vlan = port->vlan;
    if (trunk) *trunk = port->trunk;
    if (mirror_from) *mirror_from = port->mirror_from;
    pthread_mutex_unlock(&sw.port_lock);
    return active;
}

static void queue_add(int port_no, frame_packet_t *frame, int priority, int mirrored)
{
    if (priority < 0 || priority > 2) priority = 1;
    port_t *port = &sw.ports[port_no];
    frame_add_ref(frame);
    if (queue_push(&port->queues[priority], frame, mirrored) != 0) {
        frame_release(frame);
        stats_drop(port_no);
        return;
    }
    pthread_mutex_lock(&port->work_lock);
    pthread_cond_signal(&port->work_cond);
    pthread_mutex_unlock(&port->work_lock);
}

static void forward_frame(int from, frame_packet_t *frame)
{
    int broadcast = 1;
    for (int i = 0; i < 6; i++) if (frame->header.dst[i] != 0xff) broadcast = 0;
    int flood = broadcast;
    if (!broadcast) {
        int target = mac_table_lookup_vlan(&sw.table, frame->header.dst, frame->header.vlan);
        int vlan = 0, trunk = 0;
        if (target == from) flood = 0;
        else if (target >= 0 && target < sw.num_ports &&
                 port_snapshot(target, &vlan, &trunk, NULL) && (trunk || vlan == frame->header.vlan))
            queue_add(target, frame, frame->header.priority, 0);
        else flood = 1;
    }
    if (flood) {
        for (int i = 0; i < sw.num_ports; i++) {
            int vlan, trunk;
            if (i != from && port_snapshot(i, &vlan, &trunk, NULL) &&
                (trunk || vlan == frame->header.vlan)) queue_add(i, frame, frame->header.priority, 0);
        }
    }
    for (int i = 0; i < sw.num_ports; i++) {
        int mirror_from;
        if (i != from && port_snapshot(i, NULL, NULL, &mirror_from) && mirror_from == from)
            queue_add(i, frame, frame->header.priority, 1);
    }
    stats_lock();
    if (sw.stats && from < sw.stats->num_ports) sw.stats->ports[from].frames_in++;
    stats_unlock();
    frame_release(frame);
}

static void request_port_stop(int port_no)
{
    pthread_mutex_lock(&sw.port_lock);
    port_t *port = &sw.ports[port_no];
    port->active = 0;
    int fd = port->fd;
    pthread_mutex_unlock(&sw.port_lock);
    for (int i = 0; i < 3; i++) queue_stop(&port->queues[i]);
    pthread_mutex_lock(&port->work_lock);
    pthread_cond_broadcast(&port->work_cond);
    pthread_mutex_unlock(&port->work_lock);
    pthread_mutex_lock(&port->token_lock);
    pthread_cond_broadcast(&port->token_cond);
    pthread_mutex_unlock(&port->token_lock);
    if (fd >= 0) shutdown(fd, SHUT_RDWR);
}

static int wait_for_token(port_t *port)
{
    pthread_mutex_lock(&port->token_lock);
    while (port->tokens <= 0 && port_is_active((int)(port - sw.ports)) && atomic_load(&sw.keep_alive))
        pthread_cond_wait(&port->token_cond, &port->token_lock);
    int allowed = port->tokens > 0 && port_is_active((int)(port - sw.ports)) && atomic_load(&sw.keep_alive);
    if (allowed) port->tokens--;
    pthread_mutex_unlock(&port->token_lock);
    return allowed;
}

static void *sender_loop(void *arg)
{
    int port_no = (int)(intptr_t)arg;
    port_t *port = &sw.ports[port_no];
    static const int schedule[9] = {0, 0, 0, 0, 0, 1, 1, 1, 2};
    int cursor = 0;
    while (atomic_load(&sw.keep_alive) && port_is_active(port_no)) {
        frame_packet_t *frame = NULL;
        int mirrored = 0;
        for (int i = 0; i < 9 && !frame; i++) {
            frame = queue_try_pop(&port->queues[schedule[cursor]], &mirrored);
            cursor = (cursor + 1) % 9;
        }
        if (!frame) {
            pthread_mutex_lock(&port->work_lock);
            while (atomic_load(&sw.keep_alive) && port_is_active(port_no) && !queues_have_frames(port))
                pthread_cond_wait(&port->work_cond, &port->work_lock);
            pthread_mutex_unlock(&port->work_lock);
            continue;
        }
        if (!wait_for_token(port)) {
            frame_release(frame);
            continue;
        }
        frame_header_t wire;
        serialize_header(&frame->header, &wire);
        int fd = port->fd;
        int ok = fd >= 0 && write_full(fd, &wire, FRAME_HEADER_SIZE) == FRAME_HEADER_SIZE &&
                 write_full(fd, frame->payload, frame->header.length) == frame->header.length;
        if (ok) {
            stats_lock();
            if (sw.stats && port_no < sw.stats->num_ports) {
                sw.stats->ports[port_no].frames_out++;
                if (mirrored) sw.stats->ports[port_no].frames_mirrored++;
            }
            stats_unlock();
        } else {
            stats_drop(port_no);
            request_port_stop(port_no);
        }
        frame_release(frame);
    }
    return NULL;
}

static void *receiver_loop(void *arg)
{
    int port_no = (int)(intptr_t)arg;
    port_t *port = &sw.ports[port_no];
    int fd = port->fd;
    while (atomic_load(&sw.keep_alive) && port_is_active(port_no)) {
        frame_header_t wire;
        int n = read_full(fd, &wire, FRAME_HEADER_SIZE);
        if (n != FRAME_HEADER_SIZE) break;
        frame_header_t header;
        deserialize_header(&wire, &header);
        if (header.length > MAX_FRAME_PAYLOAD || header.vlan < 1 || header.vlan > 4094) break;
        int vlan, trunk;
        port_snapshot(port_no, &vlan, &trunk, NULL);
        if (!trunk) header.vlan = (uint16_t)vlan;
        frame_packet_t *frame = calloc(1, sizeof(*frame));
        if (!frame) break;
        frame->header = header;
        if (pthread_mutex_init(&frame->ref_lock, NULL) != 0) {
            free(frame);
            break;
        }
        frame->ref = 1;
        if (header.length && read_full(fd, frame->payload, header.length) != header.length) {
            frame_release(frame);
            break;
        }
        if (mac_table_insert_vlan(&sw.table, header.src, port_no, header.vlan))
            log_enqueue("MAC moved");
        if (mac_table_count_port(&sw.table, port_no) > MAC_PORT_LIMIT) {
            log_enqueue("port shut down: MAC limit exceeded");
            frame_release(frame);
            break;
        }
        forward_frame(port_no, frame);
    }
    request_port_stop(port_no);
    return NULL;
}

static void wake_all(void)
{
    for (int i = 0; i < sw.num_ports; i++) request_port_stop(i);
}

static void reply_line(int fd, const char *text)
{
    if (write_full(fd, text, (int)strlen(text)) < 0) return;
}

static void admin_show_mac(int fd)
{
    struct entry { uint8_t mac[6]; int port; int vlan; } entries[MAC_TABLE_MAX];
    size_t count = 0;
    for (int b = 0; b < NUM_BUCKETS; b++) {
        pthread_mutex_lock(&sw.table.buckets[b].lock);
        for (mac_node_t *node = sw.table.buckets[b].head; node && count < MAC_TABLE_MAX; node = node->next) {
            memcpy(entries[count].mac, node->mac, 6);
            entries[count].port = node->port;
            entries[count].vlan = node->vlan;
            count++;
        }
        pthread_mutex_unlock(&sw.table.buckets[b].lock);
    }
    char line[128];
    for (size_t i = 0; i < count; i++) {
        snprintf(line, sizeof(line), "%02x:%02x:%02x:%02x:%02x:%02x port %d vlan %d\n",
            entries[i].mac[0], entries[i].mac[1], entries[i].mac[2], entries[i].mac[3],
            entries[i].mac[4], entries[i].mac[5], entries[i].port, entries[i].vlan);
        reply_line(fd, line);
    }
}

static void admin_show_ports(int fd)
{
    struct state { int active, enabled, vlan, trunk, mirror_from; } ports[MAX_PORTS];
    pthread_mutex_lock(&sw.port_lock);
    for (int i = 0; i < sw.num_ports; i++) {
        ports[i].active = sw.ports[i].active;
        ports[i].enabled = sw.ports[i].enabled;
        ports[i].vlan = sw.ports[i].vlan;
        ports[i].trunk = sw.ports[i].trunk;
        ports[i].mirror_from = sw.ports[i].mirror_from;
    }
    pthread_mutex_unlock(&sw.port_lock);
    char line[128];
    for (int i = 0; i < sw.num_ports; i++) {
        snprintf(line, sizeof(line), "port %d active %d enabled %d vlan %d trunk %d mirror_from %d\n",
            i, ports[i].active, ports[i].enabled, ports[i].vlan, ports[i].trunk, ports[i].mirror_from);
        reply_line(fd, line);
    }
}

static void admin_stats(int fd)
{
    port_stats_t ports[MAX_PORTS];
    int count = 0;
    stats_lock();
    if (sw.stats) {
        count = sw.stats->num_ports;
        memcpy(ports, sw.stats->ports, (size_t)count * sizeof(*ports));
    }
    stats_unlock();
    char line[192];
    for (int i = 0; i < count; i++) {
        snprintf(line, sizeof(line), "port %d in %llu out %llu dropped %llu mirrored %llu\n", i,
            (unsigned long long)ports[i].frames_in, (unsigned long long)ports[i].frames_out,
            (unsigned long long)ports[i].frames_dropped, (unsigned long long)ports[i].frames_mirrored);
        reply_line(fd, line);
    }
}

static void cleanup_port_locked(int i)
{
    port_t *port = &sw.ports[i];
    request_port_stop(i);
    if (port->receive_started) pthread_join(port->receive_thread, NULL);
    if (port->sender_started) pthread_join(port->sender_thread, NULL);
    for (int q = 0; q < 3; q++) queue_drain(&port->queues[q], (int)(port - sw.ports));
    pthread_mutex_lock(&sw.port_lock);
    if (port->fd >= 0) close(port->fd);
    port->fd = -1;
    port->active = 0;
    port->receive_started = 0;
    port->sender_started = 0;
    int release_slot = port->slot_reserved;
    port->slot_reserved = 0;
    pthread_mutex_unlock(&sw.port_lock);
    if (release_slot) sem_post(&sw.free_ports);
}

static void reap_ports(void)
{
    pthread_mutex_lock(&sw.lifecycle_lock);
    for (int i = 0; i < sw.num_ports; i++) {
        pthread_mutex_lock(&sw.port_lock);
        int reap = sw.ports[i].fd >= 0 && !sw.ports[i].active;
        pthread_mutex_unlock(&sw.port_lock);
        if (reap) cleanup_port_locked(i);
    }
    pthread_mutex_unlock(&sw.lifecycle_lock);
}

static int read_command(int fd, char *line, size_t capacity)
{
    size_t used = 0;
    while (used + 1 < capacity) {
        char c;
        ssize_t n = read(fd, &c, 1);
        if (n == 1) {
            line[used++] = c;
            if (c == '\n') break;
        } else if (n < 0 && errno == EINTR) continue;
        else if (n == 0) break;
        else return -1;
    }
    line[used] = '\0';
    return (int)used;
}

static void admin_command(int fd, char *line)
{
    char cmd[32] = "", arg[32] = "";
    int p1 = -1, p2 = -1;
    if (sscanf(line, "%31s %31s", cmd, arg) < 1) return;
    if (strcmp(cmd, "show") == 0 && strcmp(arg, "mac") == 0) { admin_show_mac(fd); return; }
    if (strcmp(cmd, "show") == 0 && strcmp(arg, "ports") == 0) { admin_show_ports(fd); return; }
    if (strcmp(cmd, "stats") == 0) { admin_stats(fd); return; }
    if (strcmp(cmd, "clear") == 0 && strcmp(arg, "mac") == 0) { mac_table_clear(&sw.table); reply_line(fd, "ok\n"); return; }
    if (strcmp(cmd, "clear") == 0 && strcmp(arg, "stats") == 0) {
        stats_lock();
        if (sw.stats) memset(sw.stats->ports, 0, sizeof(sw.stats->ports));
        stats_unlock();
        reply_line(fd, "ok\n");
        return;
    }
    if (strcmp(cmd, "shutdown") == 0 && sscanf(line, "%*s %d", &p1) == 1 && p1 >= 0 && p1 < sw.num_ports) {
        pthread_mutex_lock(&sw.lifecycle_lock);
        pthread_mutex_lock(&sw.port_lock);
        sw.ports[p1].enabled = 0;
        int connected = sw.ports[p1].fd >= 0;
        pthread_mutex_unlock(&sw.port_lock);
        if (connected) cleanup_port_locked(p1);
        pthread_mutex_unlock(&sw.lifecycle_lock);
        reply_line(fd, "ok\n");
        return;
    }
    if (strcmp(cmd, "up") == 0 && sscanf(line, "%*s %d", &p1) == 1 && p1 >= 0 && p1 < sw.num_ports) {
        pthread_mutex_lock(&sw.port_lock);
        sw.ports[p1].enabled = 1;
        pthread_mutex_unlock(&sw.port_lock);
        reply_line(fd, "ok\n");
        return;
    }
    if (strcmp(cmd, "vlan") == 0 && sscanf(line, "%*s %d %d", &p1, &p2) == 2 &&
        p1 >= 0 && p1 < sw.num_ports && p2 >= 1 && p2 <= 4094) {
        pthread_mutex_lock(&sw.port_lock);
        sw.ports[p1].vlan = p2;
        pthread_mutex_unlock(&sw.port_lock);
        reply_line(fd, "ok\n");
        return;
    }
    if (strcmp(cmd, "mirror") == 0 && sscanf(line, "%*s %d %d", &p1, &p2) == 2 &&
        p1 >= 0 && p1 < sw.num_ports && p2 >= 0 && p2 < sw.num_ports && p1 != p2) {
        pthread_mutex_lock(&sw.port_lock);
        sw.ports[p2].mirror_from = p1;
        pthread_mutex_unlock(&sw.port_lock);
        reply_line(fd, "ok\n");
        return;
    }
    reply_line(fd, "error: invalid command or arguments\n");
}

static void *admin_loop(void *arg)
{
    (void)arg;
    while (atomic_load(&sw.keep_alive)) {
        int fd = accept(sw.unix_fd, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR) continue;
            if (!atomic_load(&sw.keep_alive)) break;
            continue;
        }
        char line[256];
        int n = read_command(fd, line, sizeof(line));
        if (n > 0) admin_command(fd, line);
        close(fd);
    }
    return NULL;
}

static int init_unix_socket(void)
{
    sw.unix_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sw.unix_fd < 0) return -1;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", UNIX_SOCKET);
    unlink(UNIX_SOCKET);
    if (bind(sw.unix_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(sw.unix_fd, 16) < 0) {
        close(sw.unix_fd);
        sw.unix_fd = -1;
        unlink(UNIX_SOCKET);
        return -1;
    }
    return 0;
}

static int reload_config(void)
{
    FILE *fp = fopen("vswitch.conf", "r");
    if (!fp) return errno == ENOENT ? 0 : -1;
    int vlans[MAX_PORTS], trunks[MAX_PORTS];
    for (int i = 0; i < sw.num_ports; i++) { vlans[i] = 1; trunks[i] = 0; }
    char line[256];
    int valid = 1;
    while (fgets(line, sizeof(line), fp)) {
        char kind[32], extra[32];
        int p = -1, value = 0;
        if (line[0] == '#' || line[0] == '\n') continue;
        char *content = line;
                while (*content == ' ' || *content == '\t' || *content == '\r' || *content == '\n') content++;
                if (*content == '#' || *content == '\0') continue;
                int fields = sscanf(content, "%31s %d %d %31s", kind, &p, &value, extra);
        if (fields == 3 && strcmp(kind, "vlan") == 0 && p >= 0 && p < sw.num_ports && value >= 1 && value <= 4094)
            vlans[p] = value;
        else if (fields == 2 && sscanf(content, "%31s %d", kind, &p) == 2 && strcmp(kind, "trunk") == 0 && p >= 0 && p < sw.num_ports)
            trunks[p] = 1;
        else { valid = 0; break; }
    }
    if (ferror(fp)) valid = 0;
    fclose(fp);
    if (!valid) return -1;
    pthread_mutex_lock(&sw.port_lock);
    for (int i = 0; i < sw.num_ports; i++) {
        sw.ports[i].vlan = vlans[i];
        sw.ports[i].trunk = trunks[i];
    }
    pthread_mutex_unlock(&sw.port_lock);
    return 1;
}

static int signal_reload_callback(void *arg)
{
    (void)arg;
    int result = reload_config();
    if (result < 0) log_enqueue("config reload failed: invalid configuration");
    else if (result > 0) log_enqueue("configuration reloaded");
    return result;
}

static void signal_shutdown_callback(void *arg)
{
    (void)arg;
    if (sw.unix_fd >= 0) shutdown(sw.unix_fd, SHUT_RDWR);
    wake_all();
    pthread_mutex_lock(&sw.log_lock);
    pthread_cond_broadcast(&sw.log_cond);
    pthread_mutex_unlock(&sw.log_lock);
}

static void initialize_port_threads(int i)
{
    port_t *port = &sw.ports[i];
    pthread_mutex_lock(&port->work_lock);
    pthread_cond_broadcast(&port->work_cond);
    pthread_mutex_unlock(&port->work_lock);
}

static void *maintenance_loop(void *arg);

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "Usage: ./switch <tcp_port> <num_ports>\n");
        return 1;
    }
    char *end;
    long tcp_port = strtol(argv[1], &end, 10);
    if (*argv[1] == '\0' || *end != '\0' || tcp_port < 1 || tcp_port > 65535) return 1;
    long nports = strtol(argv[2], &end, 10);
    if (*argv[2] == '\0' || *end != '\0' || nports < 1 || nports > MAX_PORTS) return 1;
    sw.num_ports = (int)nports;
    sw.listen_fd = sw.unix_fd = -1;
    atomic_init(&sw.keep_alive, 1);
    pthread_mutex_init(&sw.port_lock, NULL);
    pthread_mutex_init(&sw.lifecycle_lock, NULL);
    pthread_mutex_init(&sw.log_lock, NULL);
    pthread_cond_init(&sw.log_cond, NULL);
    sem_init(&sw.free_ports, 0, (unsigned)sw.num_ports);
    mac_table_init(&sw.table);
    sw.stats = shm_stats_create(sw.num_ports);
    if (!sw.stats) { fprintf(stderr, "cannot create shared statistics\n"); return 1; }

    for (int i = 0; i < sw.num_ports; i++) {
        port_t *port = &sw.ports[i];
        port->fd = -1;
        port->active = 0;
        port->enabled = 1;
        port->slot_reserved = 0;
        port->receive_started = port->sender_started = 0;
        port->vlan = 1;
        port->trunk = 0;
        port->mirror_from = -1;
        port->tokens = TOKEN_RATE;
        pthread_mutex_init(&port->token_lock, NULL);
        pthread_cond_init(&port->token_cond, NULL);
        pthread_mutex_init(&port->work_lock, NULL);
        pthread_cond_init(&port->work_cond, NULL);
        for (int q = 0; q < 3; q++) queue_init(&port->queues[q]);
    }

    sigset_t signals;
    if (block_switch_signals(&signals) != 0) {
        fprintf(stderr, "cannot block switch signals\n");
        return 1;
    }
    signal(SIGPIPE, SIG_IGN);

    if (init_unix_socket() < 0) { perror("UNIX socket"); return 1; }
    sw.listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    int reuse = 1;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)tcp_port);
    if (sw.listen_fd < 0 || setsockopt(sw.listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0 ||
        bind(sw.listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(sw.listen_fd, 16) < 0) {
        perror("listen socket");
        return 1;
    }
    sig_ctx_t signal_context = {
        .mask = signals, .table = &sw.table, .stats = sw.stats, .listen_fd = sw.listen_fd,
        .keep_alive = &sw.keep_alive, .reload = signal_reload_callback,
        .shutdown = signal_shutdown_callback, .callback_arg = &sw
    };
    if (pthread_create(&sw.signal_thread, NULL, sig_watcher_loop, &signal_context) != 0 ||
        pthread_create(&sw.admin_thread, NULL, admin_loop, NULL) != 0 ||
        pthread_create(&sw.maintenance_thread, NULL, maintenance_loop, NULL) != 0 ||
        pthread_create(&sw.logger_thread, NULL, logger_loop, NULL) != 0) {
        fprintf(stderr, "cannot start switch threads\n");
        return 1;
    }
    if (reload_config() < 0) log_enqueue("config reload failed: invalid configuration");
    log_enqueue("switch started");

    while (atomic_load(&sw.keep_alive)) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(sw.listen_fd, &readfds);
        struct timeval timeout = {.tv_sec = 0, .tv_usec = 200000};
        int ready = select(sw.listen_fd + 1, &readfds, NULL, NULL, &timeout);
        if (!atomic_load(&sw.keep_alive)) break;
        if (ready < 0) { if (errno == EINTR) continue; break; }
        reap_ports();
        if (!ready) continue;
        struct sockaddr_in client;
        socklen_t len = sizeof(client);
        int fd = accept(sw.listen_fd, (struct sockaddr *)&client, &len);
        if (fd < 0) { if (errno == EINTR || errno == EAGAIN) continue; if (!atomic_load(&sw.keep_alive)) break; continue; }
        struct timespec until;
        clock_gettime(CLOCK_REALTIME, &until);
        until.tv_sec++;
        if (sem_timedwait(&sw.free_ports, &until) != 0) {
            write_full(fd, "switch full", 11);
            close(fd);
            continue;
        }
        pthread_mutex_lock(&sw.lifecycle_lock);
        int port_no = -1;
        pthread_mutex_lock(&sw.port_lock);
        for (int i = 0; i < sw.num_ports; i++)
            if (sw.ports[i].enabled && sw.ports[i].fd < 0) { port_no = i; break; }
        if (port_no >= 0) {
            port_t *port = &sw.ports[port_no];
            port->fd = fd;
            port->active = 1;
            port->slot_reserved = 1;
            port->tokens = TOKEN_RATE;
        }
        pthread_mutex_unlock(&sw.port_lock);
        if (port_no < 0) {
            pthread_mutex_unlock(&sw.lifecycle_lock);
            write_full(fd, "switch full", 11);
            close(fd);
            sem_post(&sw.free_ports);
            continue;
        }
        port_t *port = &sw.ports[port_no];
        for (int q = 0; q < 3; q++) {
            pthread_mutex_lock(&port->queues[q].lock);
            port->queues[q].stopped = 0;
            pthread_mutex_unlock(&port->queues[q].lock);
        }
        if (pthread_create(&port->sender_thread, NULL, sender_loop, (void *)(intptr_t)port_no) == 0)
            port->sender_started = 1;
        if (pthread_create(&port->receive_thread, NULL, receiver_loop, (void *)(intptr_t)port_no) == 0)
            port->receive_started = 1;
        if (!port->sender_started || !port->receive_started) cleanup_port_locked(port_no);
        initialize_port_threads(port_no);
        pthread_mutex_unlock(&sw.lifecycle_lock);
    }

    atomic_store(&sw.keep_alive, 0);
    if (sw.listen_fd >= 0) shutdown(sw.listen_fd, SHUT_RDWR);
    if (sw.unix_fd >= 0) shutdown(sw.unix_fd, SHUT_RDWR);
    wake_all();
    pthread_mutex_lock(&sw.log_lock);
    pthread_cond_broadcast(&sw.log_cond);
    pthread_mutex_unlock(&sw.log_lock);
    pthread_kill(sw.signal_thread, SIGINT);
    pthread_join(sw.signal_thread, NULL);
    pthread_join(sw.admin_thread, NULL);
    pthread_join(sw.maintenance_thread, NULL);
    pthread_mutex_lock(&sw.lifecycle_lock);
    for (int i = 0; i < sw.num_ports; i++) {
        pthread_mutex_lock(&sw.port_lock);
        int connected = sw.ports[i].fd >= 0;
        pthread_mutex_unlock(&sw.port_lock);
        if (connected) cleanup_port_locked(i);
        for (int q = 0; q < 3; q++) queue_drain(&sw.ports[i].queues[q], i);
    }
    pthread_mutex_unlock(&sw.lifecycle_lock);
    pthread_join(sw.logger_thread, NULL);
    if (sw.listen_fd >= 0) close(sw.listen_fd);
    if (sw.unix_fd >= 0) close(sw.unix_fd);
    unlink(UNIX_SOCKET);
    mac_table_destroy(&sw.table);
    shm_stats_destroy(sw.stats);
    sem_destroy(&sw.free_ports);
    for (int i = 0; i < sw.num_ports; i++) {
        for (int q = 0; q < 3; q++) {
            pthread_cond_destroy(&sw.ports[i].queues[q].cond);
            pthread_mutex_destroy(&sw.ports[i].queues[q].lock);
        }
        pthread_cond_destroy(&sw.ports[i].token_cond);
        pthread_mutex_destroy(&sw.ports[i].token_lock);
        pthread_cond_destroy(&sw.ports[i].work_cond);
        pthread_mutex_destroy(&sw.ports[i].work_lock);
    }
    pthread_cond_destroy(&sw.log_cond);
    pthread_mutex_destroy(&sw.log_lock);
    pthread_mutex_destroy(&sw.lifecycle_lock);
    pthread_mutex_destroy(&sw.port_lock);
    return 0;
}

static void *maintenance_loop(void *arg)
{
    (void)arg;
    time_t last_age = time(NULL);
    while (atomic_load(&sw.keep_alive)) {
        struct timespec delay = {.tv_sec = 0, .tv_nsec = 10000000};
        nanosleep(&delay, NULL);
        for (int i = 0; i < sw.num_ports; i++) {
            port_t *port = &sw.ports[i];
            pthread_mutex_lock(&port->token_lock);
            if (port->tokens < TOKEN_RATE) port->tokens++;
            pthread_cond_broadcast(&port->token_cond);
            pthread_mutex_unlock(&port->token_lock);
        }
        time_t now = time(NULL);
        if (now - last_age >= 5) { mac_table_age(&sw.table, 300); last_age = now; }
    }
    return NULL;
}
