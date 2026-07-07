#include "threadpool.h"

#include <stdio.h>
#include <stdlib.h>

static void *worker_loop(void *arg) {
    threadpool_t *pool = arg;

    for (;;) {
        pthread_mutex_lock(&pool->lock);

        while (pool->count == 0 && !pool->shutdown) {
            pthread_cond_wait(&pool->not_empty, &pool->lock);
        }

        if (pool->shutdown && pool->count == 0) {
            pthread_mutex_unlock(&pool->lock);
            break;
        }

        void *task = pool->items[pool->head];
        pool->head = (pool->head + 1) % pool->capacity;
        pool->count--;

        /* Wake up any producer blocked in threadpool_submit() waiting for
         * a free slot. */
        pthread_cond_signal(&pool->not_full);
        pthread_mutex_unlock(&pool->lock);

        pool->handler(task, pool->ctx);
    }

    return NULL;
}

threadpool_t *threadpool_create(size_t num_workers, size_t queue_capacity,
                                 void (*handler)(void *task, void *ctx),
                                 void *ctx) {
    if (num_workers == 0 || queue_capacity == 0 || handler == NULL) {
        return NULL;
    }

    threadpool_t *pool = calloc(1, sizeof(threadpool_t));
    if (!pool) return NULL;

    pool->items = calloc(queue_capacity, sizeof(void *));
    pool->workers = calloc(num_workers, sizeof(pthread_t));
    if (!pool->items || !pool->workers) {
        free(pool->items);
        free(pool->workers);
        free(pool);
        return NULL;
    }

    pool->capacity = queue_capacity;
    pool->num_workers = num_workers;
    pool->handler = handler;
    pool->ctx = ctx;
    pool->shutdown = 0;

    pthread_mutex_init(&pool->lock, NULL);
    pthread_cond_init(&pool->not_empty, NULL);
    pthread_cond_init(&pool->not_full, NULL);

    for (size_t i = 0; i < num_workers; i++) {
        if (pthread_create(&pool->workers[i], NULL, worker_loop, pool) != 0) {
            fprintf(stderr, "threadpool: failed to create worker %zu\n", i);
            pool->num_workers = i; /* only join the ones that started */
            threadpool_destroy(pool);
            return NULL;
        }
    }

    return pool;
}

int threadpool_submit(threadpool_t *pool, void *task) {
    pthread_mutex_lock(&pool->lock);

    while (pool->count == pool->capacity && !pool->shutdown) {
        pthread_cond_wait(&pool->not_full, &pool->lock);
    }

    if (pool->shutdown) {
        pthread_mutex_unlock(&pool->lock);
        return -1;
    }

    pool->items[pool->tail] = task;
    pool->tail = (pool->tail + 1) % pool->capacity;
    pool->count++;

    pthread_cond_signal(&pool->not_empty);
    pthread_mutex_unlock(&pool->lock);
    return 0;
}

void threadpool_destroy(threadpool_t *pool) {
    if (!pool) return;

    pthread_mutex_lock(&pool->lock);
    pool->shutdown = 1;
    pthread_mutex_unlock(&pool->lock);

    pthread_cond_broadcast(&pool->not_empty);
    pthread_cond_broadcast(&pool->not_full);

    for (size_t i = 0; i < pool->num_workers; i++) {
        pthread_join(pool->workers[i], NULL);
    }

    pthread_mutex_destroy(&pool->lock);
    pthread_cond_destroy(&pool->not_empty);
    pthread_cond_destroy(&pool->not_full);

    free(pool->items);
    free(pool->workers);
    free(pool);
}
