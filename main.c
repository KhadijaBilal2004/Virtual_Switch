#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "frame.h"
#include "mac_table.h"

static void test_frame_header(void)
{
    frame_header_t original = {
        .length = 100,
        .src = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55},
        .dst = {0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff},
        .vlan = 10,
        .priority = 1
    };
    frame_header_t wire, received;
    assert(serialize_header(&original, &wire) == 0);
    assert(deserialize_header(&wire, &received) == 0);
    assert(received.length == original.length);
    assert(received.vlan == original.vlan);
    assert(received.priority == original.priority);
    assert(memcmp(received.src, original.src, sizeof(original.src)) == 0);
    assert(memcmp(received.dst, original.dst, sizeof(original.dst)) == 0);
    puts("Frame header serialization test passed!");
}

static void make_mac(unsigned value, uint8_t mac[6])
{
    mac[0] = 0x02;
    mac[1] = (uint8_t)(value >> 24);
    mac[2] = (uint8_t)(value >> 16);
    mac[3] = (uint8_t)(value >> 8);
    mac[4] = (uint8_t)value;
    mac[5] = 0x01;
}

static void test_mac_table(void)
{
    mac_table_t table;
    mac_table_init(&table);
    uint8_t mac1[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
    uint8_t mac2[6] = {0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
    assert(mac_table_lookup(&table, mac1) == -1);
    mac_table_insert(&table, mac1, 1);
    assert(mac_table_lookup(&table, mac1) == 1);
    mac_table_insert(&table, mac2, 2);
    assert(mac_table_lookup(&table, mac2) == 2);
    mac_table_insert(&table, mac1, 3);
    assert(mac_table_lookup(&table, mac1) == 3);
    assert(mac_table_count_port(&table, 1) == 0);
    assert(mac_table_count_port(&table, 3) == 1);

    uint8_t vlan_mac[6] = {2, 1, 2, 3, 4, 5};
    mac_table_insert_vlan(&table, vlan_mac, 4, 10);
    mac_table_insert_vlan(&table, vlan_mac, 5, 20);
    assert(mac_table_lookup_vlan(&table, vlan_mac, 10) == 4);
    assert(mac_table_lookup_vlan(&table, vlan_mac, 20) == 5);

    mac_table_clear(&table);
    uint8_t addresses[MAC_TABLE_MAX + 1][6];
    for (unsigned i = 0; i < MAC_TABLE_MAX; i++) {
        make_mac(i, addresses[i]);
        mac_table_insert_vlan(&table, addresses[i], (int)(i % 8), 1);
    }
    assert(mac_table_lookup_vlan(&table, addresses[0], 1) == 0);
    make_mac(MAC_TABLE_MAX, addresses[MAC_TABLE_MAX]);
    mac_table_insert_vlan(&table, addresses[MAC_TABLE_MAX], 8, 1);
    assert(mac_table_lookup_vlan(&table, addresses[1], 1) == -1);
    assert(mac_table_lookup_vlan(&table, addresses[0], 1) == 0);
    assert(mac_table_lookup_vlan(&table, addresses[MAC_TABLE_MAX], 1) == 8);
    assert(mac_table_count_port(&table, 1) == 127);
    assert(mac_table_count_port(&table, 8) == 1);
    pthread_mutex_lock(&table.count_lock);
    assert(table.count == MAC_TABLE_MAX);
    pthread_mutex_unlock(&table.count_lock);
    mac_table_destroy(&table);
    puts("MAC table, VLAN, and LRU tests passed!");
}

int main(void)
{
    test_frame_header();
    test_mac_table();
    return 0;
}
