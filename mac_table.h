#ifndef MAC_TABLE_H
#define MAC_TABLE_H

#include <stdint.h>
#include <pthread.h>
#define NUM_BUCKETS 16
typedef struct mac_node 
{
    uint8_t mac[6];
    int port;
    struct mac_node *next;
} mac_node_t;
typedef struct
{
    pthread_mutex_t lock;
    mac_node_t *head;
} mac_bucket_t;
typedef struct 
{
    mac_bucket_t buckets[NUM_BUCKETS];
} mac_table_t;
void mac_table_init(mac_table_t *table);
int mac_table_lookup(mac_table_t *table, const uint8_t *mac);
void mac_table_insert(mac_table_t *table, const uint8_t *mac, int port);

#endif