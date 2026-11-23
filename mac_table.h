#ifndef MAC_TABLE_H
#define MAC_TABLE_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <time.h>

#define NUM_BUCKETS 16
#define MAC_TABLE_MAX 1024
#define MAC_PORT_LIMIT 4
#define MAC_TABLE_PORTS 32

typedef struct mac_node {
    uint8_t mac[6];
    int port;
    int vlan;
    time_t last_seen;
    uint64_t last_used;
    struct mac_node *next;
} mac_node_t;

typedef struct {
    pthread_mutex_t lock;
    mac_node_t *head;
} mac_bucket_t;

typedef struct {
    mac_bucket_t buckets[NUM_BUCKETS];
    pthread_mutex_t count_lock;
    int count;
    int port_counts[MAC_TABLE_PORTS];
    atomic_uint_fast64_t use_clock;
} mac_table_t;

void mac_table_init(mac_table_t *table);
void mac_table_destroy(mac_table_t *table);
int mac_table_lookup(mac_table_t *table, const uint8_t *mac);
void mac_table_insert(mac_table_t *table, const uint8_t *mac, int port);
int mac_table_lookup_vlan(mac_table_t *table, const uint8_t *mac, int vlan);
int mac_table_insert_vlan(mac_table_t *table, const uint8_t *mac, int port, int vlan);
void mac_table_age(mac_table_t *table, time_t seconds);
void mac_table_clear(mac_table_t *table);
int mac_table_count_port(mac_table_t *table, int port);

#endif
