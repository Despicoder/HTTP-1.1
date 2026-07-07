#define _GNU_SOURCE
#include "server.h"
#include "http_parser.h"
#include "response.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#define MAX_EVENTS       1024
#define RECV_CHUNK       8192
#define RESPONSE_BUF_MAX (1024 * 1024) /* 1MB cap on a single response we'll buffer in RAM */
#define WRITE_POLL_TIMEOUT_MS 5000
#define SWEEPER_INTERVAL_SEC 1

/* Set once by the signal handler; server_run()'s epoll_wait has a bounded
 * timeout specifically so it periodically re-checks this flag instead of
 * blocking forever. sig_atomic_t + a plain flag is enough here since we
 * only ever set it to 1 and never rely on it for anything but a boolean
 * "should we stop" check. */
static volatile sig_atomic_t g_shutdown = 0;

static void handle_signal(int sig) {
    (void)sig;
    g_shutdown = 1;
}

void server_request_shutdown(void) {
    g_shutdown = 1;
}

/* ---------------------------------------------------------------------
 * Connection list (for the idle-timeout sweeper)
 * ------------------------------------------------------------------- */

static connection_t *connection_create(int fd) {
    connection_t *c = calloc(1, sizeof(connection_t));
    if (!c) return NULL;
    c->fd = fd;
    c->buf_cap = RECV_CHUNK;
    c->buf = malloc(c->buf_cap);
    if (!c->buf) {
        free(c);
        return NULL;
    }
    c->buf_len = 0;
    c->last_active = time(NULL);
    c->in_flight = 0;
    return c;
}

static void connection_free(connection_t *c) {
    if (!c) return;
    free(c->buf);
    free(c);
}

static void register_connection(server_t *server, connection_t *c) {
    pthread_mutex_lock(&server->conn_lock);
    c->next = server->conn_head;
    c->prev = NULL;
    if (server->conn_head) server->conn_head->prev = c;
    server->conn_head = c;
    pthread_mutex_unlock(&server->conn_lock);
}

static void unregister_connection(server_t *server, connection_t *c) {
    pthread_mutex_lock(&server->conn_lock);
    if (c->prev) c->prev->next = c->next;
    if (c->next) c->next->prev = c->prev;
    if (server->conn_head == c) server->conn_head = c->next;
    pthread_mutex_unlock(&server->conn_lock);
}

/* Closes and fully tears down a connection: removes it from epoll, closes
 * the fd, unregisters it from the sweeper's list, and frees it. */
static void close_connection(server_t *server, connection_t *c) {
    epoll_ctl(server->epoll_fd, EPOLL_CTL_DEL, c->fd, NULL);
    close(c->fd);
    unregister_connection(server, c);
    connection_free(c);
}

/* ---------------------------------------------------------------------
 * Socket helpers
 * ------------------------------------------------------------------- */

/* Writes exactly `len` bytes to `fd`, handling partial writes and EAGAIN
 * by polling for writability. This runs inside a worker thread, so
 * blocking this one thread on a slow client does not stall the rest of
 * the server -- only that client's own connection is affected. */
static int write_all(int fd, const char *buf, size_t len) {
    size_t total_written = 0;
    while (total_written < len) {
        ssize_t n = send(fd, buf + total_written, len - total_written, MSG_NOSIGNAL);
        if (n > 0) {
            total_written += (size_t)n;
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd pfd = { .fd = fd, .events = POLLOUT };
            int pr = poll(&pfd, 1, WRITE_POLL_TIMEOUT_MS);
            if (pr <= 0) return -1; /* timeout or poll error: give up on this client */
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        return -1; /* real error (e.g. ECONNRESET) */
    }
    return 0;
}

static void rearm_epoll(server_t *server, connection_t *c) {
    struct epoll_event ev = {0};
    ev.events = EPOLLIN | EPOLLET | EPOLLONESHOT;
    ev.data.ptr = c;
    /* MOD, not ADD -- the fd is already registered from when it was accepted. */
    epoll_ctl(server->epoll_fd, EPOLL_CTL_MOD, c->fd, &ev);
}

/* ---------------------------------------------------------------------
 * Request routing / static file serving
 * ------------------------------------------------------------------- */

/* Rejects any path containing a ".." segment, which is the classic
 * directory-traversal attack against a static file server. This is a
 * simple, conservative check appropriate for a teaching/portfolio server;
 * a production server would also canonicalize via realpath() and verify
 * the result still lives under docroot. */
static int path_is_safe(const char *path) {
    return strstr(path, "..") == NULL;
}

static void send_error(connection_t *c, int status_code, int keep_alive, int is_head) {
    char resp[512];
    ssize_t n = build_error_response(resp, sizeof(resp), status_code, keep_alive, is_head);
    if (n > 0) {
        write_all(c->fd, resp, (size_t)n);
    }
}

static void route_and_respond(server_t *server, connection_t *c,
                               const http_request_t *req) {
    int is_head = (strcmp(req->method, "HEAD") == 0);

    if (!path_is_safe(req->path)) {
        send_error(c, 403, req->keep_alive, is_head);
        return;
    }

    /* Strip a query string, if any -- we don't support one, but clients
     * are allowed to send "GET /page?x=1 HTTP/1.1". */
    char clean_path[MAX_PATH_LEN];
    strncpy(clean_path, req->path, sizeof(clean_path) - 1);
    clean_path[sizeof(clean_path) - 1] = '\0';
    char *qmark = strchr(clean_path, '?');
    if (qmark) *qmark = '\0';

    char full_path[2200];
    if (strcmp(clean_path, "/") == 0) {
        snprintf(full_path, sizeof(full_path), "%s/index.html", server->docroot);
    } else {
        snprintf(full_path, sizeof(full_path), "%s%s", server->docroot, clean_path);
    }

    struct stat st;
    if (stat(full_path, &st) != 0) {
        send_error(c, 404, req->keep_alive, is_head);
        return;
    }

    if (S_ISDIR(st.st_mode)) {
        char index_path[2048 + 16];
        snprintf(index_path, sizeof(index_path), "%s/index.html", full_path);
        if (stat(index_path, &st) != 0) {
            send_error(c, 404, req->keep_alive, is_head);
            return;
        }
        snprintf(full_path, sizeof(full_path), "%s", index_path);
    }

    if ((size_t)st.st_size > RESPONSE_BUF_MAX - 512) {
        /* Keep this project's response path simple (single buffered
         * write); very large files are out of scope for the demo docroot. */
        send_error(c, 500, req->keep_alive, is_head);
        return;
    }

    FILE *f = fopen(full_path, "rb");
    if (!f) {
        send_error(c, 404, req->keep_alive, is_head);
        return;
    }

    char *file_buf = malloc((size_t)st.st_size);
    if (!file_buf) {
        fclose(f);
        send_error(c, 500, req->keep_alive, is_head);
        return;
    }

    size_t read_bytes = fread(file_buf, 1, (size_t)st.st_size, f);
    fclose(f);

    char *resp_buf = malloc(RESPONSE_BUF_MAX);
    if (!resp_buf) {
        free(file_buf);
        send_error(c, 500, req->keep_alive, is_head);
        return;
    }

    const char *mime = mime_type_for_path(full_path);
    ssize_t resp_len = build_response(resp_buf, RESPONSE_BUF_MAX, 200, mime,
                                       file_buf, read_bytes, req->keep_alive, is_head);

    free(file_buf);

    if (resp_len > 0) {
        write_all(c->fd, resp_buf, (size_t)resp_len);
    }
    free(resp_buf);
}

/* ---------------------------------------------------------------------
 * Worker task: process one "readable" event for a connection
 * ------------------------------------------------------------------- */

static void handle_connection_task(void *task, void *ctx) {
    connection_t *c = task;
    server_t *server = ctx;

    int should_close = 0;
    char tmp[RECV_CHUNK];

    /* Edge-triggered epoll: we must drain the socket until EAGAIN, or we
     * will not get notified again even though data is still sitting in
     * the kernel receive buffer. */
    for (;;) {
        ssize_t n = recv(c->fd, tmp, sizeof(tmp), 0);

        if (n > 0) {
            if (c->buf_len + (size_t)n > MAX_REQUEST_SIZE) {
                send_error(c, 431, 0, 0); /* headers too large */
                should_close = 1;
                break;
            }
            if (c->buf_len + (size_t)n > c->buf_cap) {
                size_t new_cap = c->buf_cap * 2;
                while (new_cap < c->buf_len + (size_t)n) new_cap *= 2;
                char *grown = realloc(c->buf, new_cap);
                if (!grown) {
                    send_error(c, 500, 0, 0);
                    should_close = 1;
                    break;
                }
                c->buf = grown;
                c->buf_cap = new_cap;
            }
            memcpy(c->buf + c->buf_len, tmp, (size_t)n);
            c->buf_len += (size_t)n;
            continue;
        }

        if (n == 0) {
            should_close = 1; /* peer closed the connection */
            break;
        }

        /* n < 0 */
        if (errno == EAGAIN || errno == EWOULDBLOCK) break; /* drained for now */
        if (errno == EINTR) continue;

        should_close = 1; /* real socket error */
        break;
    }

    if (!should_close) {
        c->last_active = time(NULL);

        http_request_t req;
        size_t consumed = 0;
        parse_status_t ps = http_parse_request(c->buf, c->buf_len, &req, &consumed);

        if (ps == PARSE_INCOMPLETE) {
            /* Not enough data yet -- wait for more. */
            c->in_flight = 0;
            rearm_epoll(server, c);
            return;
        } else if (ps == PARSE_TOO_LARGE) {
            send_error(c, 431, 0, 0);
            should_close = 1;
        } else if (ps == PARSE_ERROR) {
            send_error(c, 400, 0, 0);
            should_close = 1;
        } else {
            route_and_respond(server, c, &req);

            /* Slide any pipelined bytes belonging to the *next* request
             * down to the front of the buffer. */
            size_t remaining = c->buf_len - consumed;
            if (remaining > 0) memmove(c->buf, c->buf + consumed, remaining);
            c->buf_len = remaining;

            if (!req.keep_alive) {
                should_close = 1;
            } else {
                c->last_active = time(NULL);
                c->in_flight = 0;
                rearm_epoll(server, c);
                return;
            }
        }
    }

    close_connection(server, c);
}

/* ---------------------------------------------------------------------
 * Idle-connection sweeper
 * ------------------------------------------------------------------- */

static void *sweeper_thread_fn(void *arg) {
    server_t *server = arg;

    while (!g_shutdown) {
        sleep(SWEEPER_INTERVAL_SEC);
        if (g_shutdown) break;

        time_t now = time(NULL);
        connection_t **to_close = NULL;
        int n = 0, cap = 0;

        pthread_mutex_lock(&server->conn_lock);
        connection_t *c = server->conn_head;
        while (c) {
            connection_t *next = c->next;
            if (!c->in_flight && (now - c->last_active) > server->timeout_seconds) {
                if (c->prev) c->prev->next = c->next;
                if (c->next) c->next->prev = c->prev;
                if (server->conn_head == c) server->conn_head = c->next;

                if (n == cap) {
                    cap = cap ? cap * 2 : 16;
                    to_close = realloc(to_close, cap * sizeof(*to_close));
                }
                to_close[n++] = c;
            }
            c = next;
        }
        pthread_mutex_unlock(&server->conn_lock);

        for (int i = 0; i < n; i++) {
            epoll_ctl(server->epoll_fd, EPOLL_CTL_DEL, to_close[i]->fd, NULL);
            close(to_close[i]->fd);
            connection_free(to_close[i]);
        }
        free(to_close);
    }

    return NULL;
}

/* ---------------------------------------------------------------------
 * Server lifecycle
 * ------------------------------------------------------------------- */

server_t *server_create(int port, const char *docroot, size_t num_workers,
                         size_t queue_capacity, int timeout_seconds) {
    server_t *server = calloc(1, sizeof(server_t));
    if (!server) return NULL;

    strncpy(server->docroot, docroot, sizeof(server->docroot) - 1);
    server->timeout_seconds = timeout_seconds;
    pthread_mutex_init(&server->conn_lock, NULL);

    server->listen_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (server->listen_fd < 0) {
        perror("socket");
        free(server);
        return NULL;
    }

    int opt = 1;
    setsockopt(server->listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((uint16_t)port);

    if (bind(server->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(server->listen_fd);
        free(server);
        return NULL;
    }

    if (listen(server->listen_fd, SOMAXCONN) < 0) {
        perror("listen");
        close(server->listen_fd);
        free(server);
        return NULL;
    }

    server->epoll_fd = epoll_create1(0);
    if (server->epoll_fd < 0) {
        perror("epoll_create1");
        close(server->listen_fd);
        free(server);
        return NULL;
    }

    struct epoll_event ev = {0};
    ev.events = EPOLLIN | EPOLLET; /* listen socket: level of ET is fine, we drain via accept loop */
    ev.data.ptr = NULL; /* NULL sentinel == "this is the listening socket" */
    if (epoll_ctl(server->epoll_fd, EPOLL_CTL_ADD, server->listen_fd, &ev) < 0) {
        perror("epoll_ctl(listen_fd)");
        close(server->epoll_fd);
        close(server->listen_fd);
        free(server);
        return NULL;
    }

    server->pool = threadpool_create(num_workers, queue_capacity,
                                      handle_connection_task, server);
    if (!server->pool) {
        fprintf(stderr, "failed to create thread pool\n");
        close(server->epoll_fd);
        close(server->listen_fd);
        free(server);
        return NULL;
    }

    struct sigaction sa = {0};
    sa.sa_handler = handle_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN); /* a client closing mid-write should not kill us */

    server->sweeper_running = 1;
    pthread_create(&server->sweeper_thread, NULL, sweeper_thread_fn, server);

    return server;
}

static void accept_new_connections(server_t *server) {
    for (;;) {
        int client_fd = accept4(server->listen_fd, NULL, NULL, SOCK_NONBLOCK);
        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break; /* drained */
            if (errno == EINTR) continue;
            break; /* other errors: stop trying this round */
        }

        int opt = 1;
        setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

        connection_t *c = connection_create(client_fd);
        if (!c) {
            close(client_fd);
            continue;
        }

        register_connection(server, c);

        struct epoll_event ev = {0};
        ev.events = EPOLLIN | EPOLLET | EPOLLONESHOT;
        ev.data.ptr = c;
        if (epoll_ctl(server->epoll_fd, EPOLL_CTL_ADD, client_fd, &ev) < 0) {
            unregister_connection(server, c);
            close(client_fd);
            connection_free(c);
        }
    }
}

int server_run(server_t *server) {
    struct epoll_event events[MAX_EVENTS];

    while (!g_shutdown) {
        int n = epoll_wait(server->epoll_fd, events, MAX_EVENTS, 1000);

        if (n < 0) {
            if (errno == EINTR) continue;
            perror("epoll_wait");
            break;
        }

        for (int i = 0; i < n; i++) {
            if (events[i].data.ptr == NULL) {
                accept_new_connections(server);
                continue;
            }

            connection_t *c = events[i].data.ptr;

            if (events[i].events & (EPOLLHUP | EPOLLERR)) {
                close_connection(server, c);
                continue;
            }

            c->in_flight = 1;
            if (threadpool_submit(server->pool, c) != 0) {
                /* Pool is shutting down; nothing more to do. */
                c->in_flight = 0;
            }
        }
    }

    /* --- Graceful shutdown sequence --- */
    epoll_ctl(server->epoll_fd, EPOLL_CTL_DEL, server->listen_fd, NULL);
    close(server->listen_fd);

    /* Let in-flight tasks finish, then stop the workers. */
    threadpool_destroy(server->pool);
    server->pool = NULL;

    /* Wake the sweeper (it sleeps up to 1s) and join it. */
    if (server->sweeper_running) {
        pthread_join(server->sweeper_thread, NULL);
        server->sweeper_running = 0;
    }

    /* Close any connections still open. */
    pthread_mutex_lock(&server->conn_lock);
    connection_t *c = server->conn_head;
    while (c) {
        connection_t *next = c->next;
        epoll_ctl(server->epoll_fd, EPOLL_CTL_DEL, c->fd, NULL);
        close(c->fd);
        connection_free(c);
        c = next;
    }
    server->conn_head = NULL;
    pthread_mutex_unlock(&server->conn_lock);

    close(server->epoll_fd);
    return 0;
}

void server_destroy(server_t *server) {
    if (!server) return;
    pthread_mutex_destroy(&server->conn_lock);
    free(server);
}
