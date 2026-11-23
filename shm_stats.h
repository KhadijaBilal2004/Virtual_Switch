#ifndef SHM_STATS_H
#define SHM_STATS_H

#include <stdint.h>
#include <pthread.h>

#define SHM_NAME "/vswitch_khadija"
#define MAX_PORTS 32

typedef struct {
    uint64_t frames_in;
    uint64_t frames_out;
    uint64_t frames_dropped;
    uint64_t frames_mirrored;
} port_stats_t;

typedef struct {
    pthread_mutex_t lock;
    int num_ports;
    port_stats_t ports[MAX_PORTS];
} shm_stats_t;

shm_stats_t *shm_stats_create(int num_ports);
shm_stats_t *shm_stats_attach(void);
void shm_stats_destroy(shm_stats_t *stats);

#endif