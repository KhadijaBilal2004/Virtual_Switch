#include "frame.h"
#include <string.h>
#include <errno.h>
#include <unistd.h>
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

int read_full(int fd, void *data, int size)
{
    int got = 0;
    char *p = (char *)data;
    while (got < size) {
        int n = read(fd, p + got, size - got);
        if (n == 0) return 0;
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        got += n;
    }
    return got;
}

int write_full(int fd, const void *data, int size)
{
    int sent = 0;
    const char *p = (const char *)data;
    while (sent < size) {
        int n = write(fd, p + sent, size - sent);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        sent += n;
    }
    return sent;
}