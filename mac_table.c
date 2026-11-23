#include "mac_table.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned hash_mac(const uint8_t *mac)
{
    unsigned sum = 'k' + 'h' + 'a' + 'd' + 'i' + 'j' + 'a';
    for (int i = 0; i < 6; i++) sum += mac[i];
    return sum % NUM_BUCKETS;
}

static uint64_t next_use(mac_table_t *table)
{
    return atomic_fetch_add(&table->use_clock, 1) + 1;
}

void mac_table_init(mac_table_t *table)
{
    pthread_mutex_init(&table->count_lock, NULL);
    table->count = 0;
    memset(table->port_counts, 0, sizeof(table->port_counts));
    atomic_init(&table->use_clock, 0);
    for (int i = 0; i < NUM_BUCKETS; i++) {
        pthread_mutex_init(&table->buckets[i].lock, NULL);
        table->buckets[i].head = NULL;
    }
}

void mac_table_clear(mac_table_t *table)
{
    for (int i = 0; i < NUM_BUCKETS; i++) pthread_mutex_lock(&table->buckets[i].lock);
    pthread_mutex_lock(&table->count_lock);
    for (int i = 0; i < NUM_BUCKETS; i++) {
        mac_node_t *node = table->buckets[i].head;
        while (node) {
            mac_node_t *next = node->next;
            free(node);
            node = next;
        }
        table->buckets[i].head = NULL;
    }
    table->count = 0;
    memset(table->port_counts, 0, sizeof(table->port_counts));
    pthread_mutex_unlock(&table->count_lock);
    for (int i = NUM_BUCKETS - 1; i >= 0; i--) pthread_mutex_unlock(&table->buckets[i].lock);
}

void mac_table_destroy(mac_table_t *table)
{
    mac_table_clear(table);
    for (int i = 0; i < NUM_BUCKETS; i++) pthread_mutex_destroy(&table->buckets[i].lock);
    pthread_mutex_destroy(&table->count_lock);
}

static int lookup(mac_table_t *table, const uint8_t *mac, int vlan, int match_vlan)
{
    unsigned idx = hash_mac(mac);
    pthread_mutex_lock(&table->buckets[idx].lock);
    for (mac_node_t *node = table->buckets[idx].head; node; node = node->next) {
        if (memcmp(node->mac, mac, 6) == 0 && (!match_vlan || node->vlan == vlan)) {
            int port = node->port;
            node->last_seen = time(NULL);
            node->last_used = next_use(table);
            pthread_mutex_unlock(&table->buckets[idx].lock);
            return port;
        }
    }
    pthread_mutex_unlock(&table->buckets[idx].lock);
    return -1;
}

int mac_table_lookup(mac_table_t *table, const uint8_t *mac)
{
    return lookup(table, mac, 0, 0);
}

int mac_table_lookup_vlan(mac_table_t *table, const uint8_t *mac, int vlan)
{
    return lookup(table, mac, vlan, 1);
}

static int evict_lru(mac_table_t *table)
{
    mac_node_t **oldest_link = NULL;
    int evicted = 0;
    uint64_t oldest = UINT64_MAX;
    for (int i = 0; i < NUM_BUCKETS; i++) pthread_mutex_lock(&table->buckets[i].lock);
    pthread_mutex_lock(&table->count_lock);
    for (int i = 0; i < NUM_BUCKETS; i++) {
        for (mac_node_t **link = &table->buckets[i].head; *link; link = &(*link)->next) {
            if ((*link)->last_used < oldest) {
                oldest = (*link)->last_used;
                oldest_link = link;
            }
        }
    }
    if (oldest_link) {
        mac_node_t *node = *oldest_link;
        *oldest_link = node->next;
        if (node->port >= 0 && node->port < MAC_TABLE_PORTS) table->port_counts[node->port]--;
        free(node);
        table->count--;
        evicted = 1;
    }
    pthread_mutex_unlock(&table->count_lock);
    for (int i = NUM_BUCKETS - 1; i >= 0; i--) pthread_mutex_unlock(&table->buckets[i].lock);
    return evicted;
}

int mac_table_insert_vlan(mac_table_t *table, const uint8_t *mac, int port, int vlan)
{
    mac_node_t *fresh = malloc(sizeof(*fresh));
    if (!fresh) return 0;
    memcpy(fresh->mac, mac, 6);
    fresh->port = port;
    fresh->vlan = vlan;
    fresh->last_seen = time(NULL);
    fresh->next = NULL;
    unsigned idx = hash_mac(mac);

    for (;;) {
        pthread_mutex_lock(&table->buckets[idx].lock);
        for (mac_node_t *node = table->buckets[idx].head; node; node = node->next) {
            if (memcmp(node->mac, mac, 6) == 0 && node->vlan == vlan) {
                int moved = node->port != port;
                pthread_mutex_lock(&table->count_lock);
                if (moved) {
                    if (node->port >= 0 && node->port < MAC_TABLE_PORTS) table->port_counts[node->port]--;
                    if (port >= 0 && port < MAC_TABLE_PORTS) table->port_counts[port]++;
                    node->port = port;
                }
                pthread_mutex_unlock(&table->count_lock);
                node->last_seen = fresh->last_seen;
                node->last_used = next_use(table);
                pthread_mutex_unlock(&table->buckets[idx].lock);
                free(fresh);
                return moved;
            }
        }
        pthread_mutex_lock(&table->count_lock);
        if (table->count < MAC_TABLE_MAX) {
            fresh->last_used = next_use(table);
            fresh->next = table->buckets[idx].head;
            table->buckets[idx].head = fresh;
            table->count++;
            if (port >= 0 && port < MAC_TABLE_PORTS) table->port_counts[port]++;
            pthread_mutex_unlock(&table->count_lock);
            pthread_mutex_unlock(&table->buckets[idx].lock);
            return 0;
        }
        pthread_mutex_unlock(&table->count_lock);
        pthread_mutex_unlock(&table->buckets[idx].lock);
        if (!evict_lru(table)) { free(fresh); return 0; }
    }
}

void mac_table_insert(mac_table_t *table, const uint8_t *mac, int port)
{
    (void)mac_table_insert_vlan(table, mac, port, 1);
}

void mac_table_age(mac_table_t *table, time_t seconds)
{
    time_t now = time(NULL);
    for (int i = 0; i < NUM_BUCKETS; i++) {
        pthread_mutex_lock(&table->buckets[i].lock);
        mac_node_t **link = &table->buckets[i].head;
        while (*link) {
            if (now - (*link)->last_seen >= seconds) {
                mac_node_t *old = *link;
                *link = old->next;
                int old_port = old->port;
                free(old);
                pthread_mutex_lock(&table->count_lock);
                table->count--;
                if (old_port >= 0 && old_port < MAC_TABLE_PORTS) table->port_counts[old_port]--;
                pthread_mutex_unlock(&table->count_lock);
            } else link = &(*link)->next;
        }
        pthread_mutex_unlock(&table->buckets[i].lock);
    }
}

int mac_table_count_port(mac_table_t *table, int port)
{
    if (port < 0 || port >= MAC_TABLE_PORTS) return 0;
    pthread_mutex_lock(&table->count_lock);
    int count = table->port_counts[port];
    pthread_mutex_unlock(&table->count_lock);
    return count;
}
