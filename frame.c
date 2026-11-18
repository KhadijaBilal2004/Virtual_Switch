#include "frame.h"
#include <string.h>
int serialize_header(const frame_header_t *in, frame_header_t *out)
{
    out->length = htons(in->length);
    memcpy(out->src, in->src, 6);
    memcpy(out->dst, in->dst, 6);
    out->vlan = htons(in->vlan);
    out->priority = in->priority;
    return 0;
}
int deserialize_header(const frame_header_t *in, frame_header_t *out)
{
    out->length = ntohs(in->length);
    memcpy(out->src, in->src, 6);
    memcpy(out->dst, in->dst, 6);
    out->vlan = ntohs(in->vlan);
    out->priority = in->priority;
    return 0;
}