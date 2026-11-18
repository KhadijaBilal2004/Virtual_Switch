#include <stdio.h>
#include <assert.h>
#include "frame.h"
#include "mac_table.h"

int main(void)
{
    frame_header_t original = {
        .length = 100,
        .src = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55},
        .dst = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF},
        .vlan = 10,
        .priority = 1
    };
    frame_header_t wire;
    frame_header_t received;

    serialize_header(&original, &wire);
    deserialize_header(&wire, &received);

    assert(received.length == 100);
    assert(received.vlan == 10);
    assert(received.priority == 1);
    printf("Frame header serialization test passed!\n");

    mac_table_t table;
    mac_table_init(&table);

    uint8_t mac1[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
    uint8_t mac2[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};

    assert(mac_table_lookup(&table, mac1) == -1);

    mac_table_insert(&table, mac1, 1);
    assert(mac_table_lookup(&table, mac1) == 1);

    mac_table_insert(&table, mac2, 2);
    assert(mac_table_lookup(&table, mac2) == 2);

    mac_table_insert(&table, mac1, 3);
    assert(mac_table_lookup(&table, mac1) == 3);

    printf("MAC table test passed!\n");

    return 0;
}