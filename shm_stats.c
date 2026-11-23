#include "shm_stats.h"
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

shm_stats_t *shm_stats_create(int num_ports)
{
    if (num_ports < 1 || num_ports > MAX_PORTS) { errno = EINVAL; return NULL; }
    int fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0600);
    if (fd < 0) return NULL;
    if (ftruncate(fd, sizeof(shm_stats_t)) < 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return NULL;
    }
    shm_stats_t *stats = mmap(NULL, sizeof(*stats), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    int saved = errno;
    close(fd);
    if (stats == MAP_FAILED) { errno = saved; return NULL; }
    memset(stats, 0, sizeof(*stats));
    stats->num_ports = num_ports;
    pthread_mutexattr_t attr;
    if (pthread_mutexattr_init(&attr) != 0) goto fail;
    if (pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED) != 0) {
        pthread_mutexattr_destroy(&attr);
        goto fail;
    }
    if (pthread_mutex_init(&stats->lock, &attr) != 0) {
        pthread_mutexattr_destroy(&attr);
        goto fail;
    }
    pthread_mutexattr_destroy(&attr);
    return stats;

fail:
    munmap(stats, sizeof(*stats));
    shm_unlink(SHM_NAME);
    errno = EINVAL;
    return NULL;
}

shm_stats_t *shm_stats_attach(void)
{
    int fd = shm_open(SHM_NAME, O_RDWR, 0600);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_size < (off_t)sizeof(shm_stats_t)) {
        int saved = errno ? errno : EINVAL;
        close(fd);
        errno = saved;
        return NULL;
    }
    shm_stats_t *stats = mmap(NULL, sizeof(*stats), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    int saved = errno;
    close(fd);
    if (stats == MAP_FAILED) { errno = saved; return NULL; }
    if (stats->num_ports < 1 || stats->num_ports > MAX_PORTS) {
        munmap(stats, sizeof(*stats));
        errno = EINVAL;
        return NULL;
    }
    return stats;
}

void shm_stats_destroy(shm_stats_t *stats)
{
    if (stats) munmap(stats, sizeof(*stats));
    shm_unlink(SHM_NAME);
}
