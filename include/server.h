#ifndef SERVER_H
#define SERVER_H

#include <pthread.h>
#include <time.h>
#include <signal.h>
#include "threadpool.h"

/* Per-connection state. Kept alive across multiple epoll events for the
 * same fd (we use EPOLLONESHOT, so a connection is only ever "owned" by
 * one worker thread at a time -- either the epoll thread waiting for the
 * next event, or a worker thread actively processing it). */
typedef struct connection {
    int fd;

    char   *buf;       /* accumulated request bytes (grows up to MAX_REQUEST_SIZE) */
    size_t  buf_len;
    size_t  buf_cap;

    time_t  last_active;
    volatile int in_flight; /* 1 while a worker owns this connection */

    struct connection *prev, *next; /* intrusive list for the idle-timeout sweeper */
} connection_t;

typedef struct {
    int listen_fd;
    int epoll_fd;

    threadpool_t *pool;

    char docroot[512];
    int  timeout_seconds;

    pthread_mutex_t conn_lock;
    connection_t   *conn_head;   /* protected by conn_lock */

    pthread_t sweeper_thread;
    int       sweeper_running;
} server_t;

/* Creates the listening socket, epoll instance, and thread pool.
 * Returns NULL on failure (and prints an error to stderr). */
server_t *server_create(int port, const char *docroot, size_t num_workers,
                         size_t queue_capacity, int timeout_seconds);

/* Runs the epoll event loop. Blocks until a shutdown is requested via
 * SIGINT/SIGTERM (or server_stop() is called from another thread).
 * Returns 0 on clean shutdown. */
int server_run(server_t *server);

/* Requests a graceful shutdown. Safe to call from a signal handler. */
void server_request_shutdown(void);

/* Frees all resources associated with the server. Call after server_run()
 * returns. */
void server_destroy(server_t *server);

#endif /* SERVER_H */
