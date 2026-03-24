/*
 * shared_msg.c — Anonymous shared memory IPC.
 *
 * Layout: [atomic_int ready] [uint32_t len] [char data[1024]]
 * Total: 1032 bytes, page-aligned by mmap.
 */

#include "shared_msg.h"

#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

#define MSG_MAX_LEN 1024

struct shared_msg {
    atomic_int ready;       /* 0 = empty, 1 = message available */
    uint32_t   len;         /* message length in bytes */
    char       data[MSG_MAX_LEN];
};

shared_msg_t *shared_msg_create(void)
{
    void *mem = mmap(NULL, sizeof(shared_msg_t),
                     PROT_READ | PROT_WRITE,
                     MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED)
        return NULL;

    shared_msg_t *msg = mem;
    atomic_store(&msg->ready, 0);
    msg->len = 0;
    memset(msg->data, 0, MSG_MAX_LEN);
    return msg;
}

void shared_msg_send(shared_msg_t *msg, const void *data, size_t len)
{
    if (!msg || !data) return;
    if (len > MSG_MAX_LEN) len = MSG_MAX_LEN;

    memcpy(msg->data, data, len);
    msg->len = (uint32_t)len;
    /* Release: ensure data and len are visible before ready flag. */
    atomic_store_explicit(&msg->ready, 1, memory_order_release);
}

int shared_msg_recv(shared_msg_t *msg, void *buf, size_t bufsz, int timeout_ms)
{
    if (!msg || !buf || bufsz == 0) return -1;

    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);

    for (;;) {
        /* Acquire: if ready is 1, we see the data the writer stored. */
        if (atomic_load_explicit(&msg->ready, memory_order_acquire)) {
            size_t copy = msg->len;
            if (copy > bufsz) copy = bufsz;
            memcpy(buf, msg->data, copy);
            return (int)copy;
        }

        /* Check timeout */
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long elapsed_ms = (now.tv_sec - start.tv_sec) * 1000 +
                          (now.tv_nsec - start.tv_nsec) / 1000000;
        if (elapsed_ms >= timeout_ms)
            return 0; /* timeout */

        /* Yield — avoid busy spin. 1ms sleep. */
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 1000000 };
        nanosleep(&ts, NULL);
    }
}

void shared_msg_destroy(shared_msg_t *msg)
{
    if (!msg) return;
    munmap(msg, sizeof(shared_msg_t));
}
