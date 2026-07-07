# HTTP/1.1 Server

A multithreaded HTTP/1.1 static file server written from scratch in C, using raw BSD sockets and Linux's `epoll` API. No external HTTP or networking libraries — just the POSIX socket API, `pthreads`, and a hand-rolled request parser.

## Features

- **Non-blocking, edge-triggered `epoll` event loop** for handling thousands of concurrent connections on a single thread
- **Bounded thread pool** (configurable worker count and queue capacity) for processing requests off the event loop
- **`EPOLLONESHOT`-based connection ownership**, ensuring a single connection is only ever handled by one thread at a time
- **Hand-rolled HTTP/1.1 parser** for request lines and headers (GET/HEAD)
- **MIME-type resolution** by file extension for common web asset types
- **Correct keep-alive semantics** per RFC 7230 (HTTP/1.1 defaults to keep-alive, HTTP/1.0 defaults to close, either overridable via the `Connection` header)
- **RFC 1123-formatted `Date` headers** and proper `Content-Length` handling
- **Idle-connection reaper**: a background thread that closes connections that have been inactive past a configurable timeout, protecting against Slowloris-style hold-open attacks
- **Bounded per-connection header buffer** (returns `431 Request Header Fields Too Large` instead of growing memory unboundedly on adversarial input)
- **Path-traversal protection** on requested file paths
- **Graceful shutdown** on `SIGINT`/`SIGTERM` — in-flight requests are allowed to finish before the process exits

## Architecture

```
                     ┌─────────────────────────────────────┐
                     │           epoll event loop          │
                     │            (main thread)            │
                     │                                     │
   new connection ──>│  accept4() in a loop until EAGAIN   |
                     │  → set EPOLLONESHOT, add to epoll   │
                     │                                     │
   fd readable    ──>│  events[i].data.ptr = connection_t* │
                     │  → mark in_flight,push to task queue│
                     └───────────────┬─────────────────────┘
                                     │
                          bounded task queue (mutex + condvars)
                                     │
              ┌──────────────────────┼──────────────────────┐
              ▼                      ▼                      ▼
        worker thread 1        worker thread 2   ...   worker thread N
        drain socket until EAGAIN → parse request → route → build
        response → write_all() → keep-alive? re-arm epoll : close fd
```

A separate "sweeper" thread wakes up periodically, checks all open connections for inactivity, and closes any that have exceeded the configured idle timeout — skipping any connection currently owned by a worker thread.

## Project layout

```
http-server/
├── include/          # Header files
├── src/
│   ├── main.c         # CLI argument parsing, entry point
│   ├── server.c       # epoll event loop, connection lifecycle, sweeper thread
│   ├── threadpool.c   # Worker thread pool and bounded task queue
│   ├── http_parser.c  # HTTP/1.1 request line + header parsing
│   └── response.c     # Response building, MIME type resolution
├── www/               # Default document root (static files served)
├── Makefile
└── README.md
```

## Requirements

- Linux (uses `epoll`, which is Linux-specific)
- `gcc` with C11 support
- `pthreads`
- [`wrk`](https://github.com/wg/wrk), for load testing (optional, see below)

## Building

```bash
make
```

This produces a `httpserver` binary in the project root.

To clean build artifacts:
```bash
make clean
```

## Running

```bash
./httpserver -p 8080 -d ./www -t 16 -q 256 -T 30
```

| Flag | Description | Default |
|------|-------------|---------|
| `-p` | Port to listen on | `8080` |
| `-d` | Document root to serve files from | `./www` |
| `-t` | Number of worker threads | `16` |
| `-q` | Bounded task queue capacity | `256` |
| `-T` | Idle connection timeout, in seconds | `30` |

Press `Ctrl+C` to trigger a graceful shutdown — the server finishes any in-flight requests before exiting.

### Quick manual test

With the server running, in another terminal:

```bash
curl http://127.0.0.1:8080/
curl -I http://127.0.0.1:8080/style.css   # inspect MIME type / headers
curl http://127.0.0.1:8080/does-not-exist  # 404
```

## Load testing

[`wrk`](https://github.com/wg/wrk) is used to measure throughput and latency under concurrent load.

**Install `wrk`** (not typically packaged in standard repos, so build from source):
```bash
git clone https://github.com/wg/wrk.git
cd wrk
make
```

**Run a load test** against a running instance of the server:
```bash
./wrk -t4 -c200 -d30s --latency http://127.0.0.1:8080/
```

- `-t4` — number of `wrk` threads (match to your CPU core count)
- `-c200` — number of concurrent connections
- `-d30s` — test duration
- `--latency` — reports full latency percentile breakdown (p50/p75/p90/p99), not just the average

Results will vary by hardware, network stack, and environment (e.g. bare-metal Linux vs. a VM vs. WSL2), so we need to benchmark on our own target environment.

## Known limitations

- No TLS/HTTPS support
- No request body support (GET/HEAD only — sufficient for static file serving, not a general-purpose app server)
- Single-process (no multi-process fan-out via `SO_REUSEPORT`)
- Files are read fully into memory per request rather than served via `sendfile()`/`mmap()`, which is less efficient for large files
- The idle-connection sweeper scans the full connection list on each pass; a timer-wheel design would scale better at very high connection counts

