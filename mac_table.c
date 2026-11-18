#include "mac_table.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int hash_mac(const uint8_t *mac)
{
    int sum = 'k' + 'h' + 'a' + 'd' + 'i' + 'j' + 'a';
    for (int i = 0; i < 6; i++) {
        sum += mac[i];
    }
    return sum % NUM_BUCKETS;
}
void mac_table_init(mac_table_t *table)
{
    for (int i = 0; i < NUM_BUCKETS; i++) 
    {
        pthread_mutex_init(&table->buckets[i].lock, NULL);
        table->buckets[i].head = NULL;
    }
}
int mac_table_lookup(mac_table_t *table, const uint8_t *mac)
{
    int idx = hash_mac(mac);
    pthread_mutex_lock(&table->buckets[idx].lock);
    mac_node_t *curr = table->buckets[idx].head;
    while (curr != NULL) 
    {
        if (memcmp(curr->mac, mac, 6) == 0) 
        {
            int port = curr->port;
            pthread_mutex_unlock(&table->buckets[idx].lock);
            return port;
        }
        curr = curr->next;
    }
    pthread_mutex_unlock(&table->buckets[idx].lock);
    return -1;
}
void mac_table_insert(mac_table_t *table, const uint8_t *mac, int port)
{
    int idx = hash_mac(mac);

    pthread_mutex_lock(&table->buckets[idx].lock);

    mac_node_t *curr = table->buckets[idx].head;
    while (curr != NULL) {
        if (memcmp(curr->mac, mac, 6) == 0) {
            if (curr->port != port) {
                printf("MAC moved: port %d -> port %d\n", curr->port, port);
                curr->port = port;
            }
            pthread_mutex_unlock(&table->buckets[idx].lock);
            return;
        }
        curr = curr->next;
    }
    mac_node_t *n = malloc(sizeof(mac_node_t));
    memcpy(n->mac, mac, 6);
    n->port = port;
    n->next = table->buckets[idx].head;
    table->buckets[idx].head = n;
    pthread_mutex_unlock(&table->buckets[idx].lock);
}