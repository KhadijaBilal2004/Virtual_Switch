#include "signals.h"
#include <stdio.h>
#include <sys/socket.h>

int block_switch_signals(sigset_t *set)
{
    sigemptyset(set);
    sigaddset(set, SIGINT);
    sigaddset(set, SIGHUP);
    sigaddset(set, SIGUSR1);
    return pthread_sigmask(SIG_BLOCK, set, NULL);
}

static void dump_mac_file(mac_table_t *table)
{
    FILE *fp = fopen("khadija_mac_dump.txt", "w");
    if (!fp) return;
    for (int b = 0; b < NUM_BUCKETS; b++) {
        pthread_mutex_lock(&table->buckets[b].lock);
        for (mac_node_t *node = table->buckets[b].head; node; node = node->next)
            fprintf(fp, "%02x:%02x:%02x:%02x:%02x:%02x -> port %d vlan %d\n",
                node->mac[0], node->mac[1], node->mac[2], node->mac[3], node->mac[4], node->mac[5],
                node->port, node->vlan);
        pthread_mutex_unlock(&table->buckets[b].lock);
    }
    fclose(fp);
}

void *sig_watcher_loop(void *arg)
{
    sig_ctx_t *ctx = arg;
    int caught;
    while (atomic_load(ctx->keep_alive)) {
        if (sigwait(&ctx->mask, &caught) != 0) continue;
        if (caught == SIGUSR1) dump_mac_file(ctx->table);
        else if (caught == SIGHUP && ctx->reload) (void)ctx->reload(ctx->callback_arg);
        else if (caught == SIGINT) {
            atomic_store(ctx->keep_alive, 0);
            if (ctx->listen_fd >= 0) shutdown(ctx->listen_fd, SHUT_RDWR);
            if (ctx->shutdown) ctx->shutdown(ctx->callback_arg);
            break;
        }
    }
    return NULL;
}
