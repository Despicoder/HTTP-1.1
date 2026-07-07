#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <pthread.h>
#include <stddef.h>

/*
 * Generic bounded-queue thread pool.
 *
 * Tasks are opaque (void *) so the same pool implementation can be reused
 * for any workload. In this project, each task is a `connection_t *`
 * (see server.h) that is ready to be read from / written to.
 *
 * The queue is a fixed-size ring buffer. Producers (the epoll thread) block
 * on threadpool_submit() if the queue is full, which provides natural
 * backpressure instead of unbounded memory growth under load.
 */
typedef struct {
    void **items;              /* ring buffer of task pointers          */
    size_t capacity;
    size_t head;                /* index to pop from                     */
    size_t tail;                /* index to push to                      */
    size_t count;

    pthread_mutex_t lock;
    pthread_cond_t  not_empty;
    pthread_cond_t  not_full;

    pthread_t *workers;
    size_t     num_workers;

    void (*handler)(void *task, void *ctx);
    void  *ctx;

    volatile int shutdown;
} threadpool_t;

/* Create a pool with `num_workers` threads and a bounded queue of
 * `queue_capacity` slots. `handler` is invoked by a worker thread for
 * every submitted task, with `ctx` passed through unchanged. */
threadpool_t *threadpool_create(size_t num_workers, size_t queue_capacity,
                                 void (*handler)(void *task, void *ctx),
                                 void *ctx);

/* Blocks if the queue is full until space is available. Returns 0 on
 * success, -1 if the pool is shutting down. */
int threadpool_submit(threadpool_t *pool, void *task);

/* Signals shutdown, wakes all workers, joins them, and frees the pool.
 * Any tasks still sitting in the queue at shutdown time are dropped
 * (caller is responsible for policy on in-flight tasks). */
void threadpool_destroy(threadpool_t *pool);

#endif /* THREADPOOL_H */
