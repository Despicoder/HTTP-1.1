#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <unistd.h>

#include "server.h"

static void print_usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [-p port] [-d docroot] [-t threads] [-q queue_capacity] [-T timeout_sec]\n"
        "  -p  port to listen on            (default: 8080)\n"
        "  -d  document root to serve       (default: ./www)\n"
        "  -t  worker thread pool size      (default: 16)\n"
        "  -q  bounded task queue capacity  (default: 256)\n"
        "  -T  idle connection timeout, sec (default: 30)\n",
        prog);
}

int main(int argc, char **argv) {
    int port = 8080;
    char docroot[512] = "./www";
    long threads = 16;
    long queue_capacity = 256;
    int timeout_seconds = 30;

    int opt;
    while ((opt = getopt(argc, argv, "p:d:t:q:T:h")) != -1) {
        switch (opt) {
            case 'p': port = atoi(optarg); break;
            case 'd': strncpy(docroot, optarg, sizeof(docroot) - 1); break;
            case 't': threads = strtol(optarg, NULL, 10); break;
            case 'q': queue_capacity = strtol(optarg, NULL, 10); break;
            case 'T': timeout_seconds = atoi(optarg); break;
            case 'h':
            default:
                print_usage(argv[0]);
                return (opt == 'h') ? 0 : 1;
        }
    }

    if (port <= 0 || port > 65535) {
        fprintf(stderr, "invalid port: %d\n", port);
        return 1;
    }
    if (threads <= 0 || threads > 1024) {
        fprintf(stderr, "invalid thread count: %ld\n", threads);
        return 1;
    }

    printf("Starting server on port %d\n", port);
    printf("  docroot:        %s\n", docroot);
    printf("  worker threads: %ld\n", threads);
    printf("  queue capacity: %ld\n", queue_capacity);
    printf("  idle timeout:   %ds\n", timeout_seconds);

    server_t *server = server_create(port, docroot, (size_t)threads,
                                      (size_t)queue_capacity, timeout_seconds);
    if (!server) {
        fprintf(stderr, "failed to start server\n");
        return 1;
    }

    printf("Server ready. Press Ctrl+C to stop.\n");
    server_run(server);
    server_destroy(server);

    printf("Server shut down cleanly.\n");
    return 0;
}
