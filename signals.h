#ifndef SIGNALS_H
#define SIGNALS_H

#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include "mac_table.h"
#include "shm_stats.h"

typedef int (*signal_reload_callback_t)(void *arg);
typedef void (*signal_shutdown_callback_t)(void *arg);

typedef struct {
    sigset_t mask;
    mac_table_t *table;
    shm_stats_t *stats;
    int listen_fd;
    atomic_int *keep_alive;
    signal_reload_callback_t reload;
    signal_shutdown_callback_t shutdown;
    void *callback_arg;
} sig_ctx_t;

int block_switch_signals(sigset_t *set);
void *sig_watcher_loop(void *arg);

#endif
