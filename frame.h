#ifndef FRAME_H
#define FRAME_H

#include <stdint.h>
#include <netinet/in.h>

#define FRAME_HEADER_SIZE 17u

typedef struct __attribute__((packed)) {
    uint16_t length;
    uint8_t src[6];
    uint8_t dst[6];
    uint16_t vlan;
    uint8_t priority;
} frame_header_t;


int serialize_header(const frame_header_t *in, frame_header_t *out);
int deserialize_header(const frame_header_t *in, frame_header_t *out);

#endif
