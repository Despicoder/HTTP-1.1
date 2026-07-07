# tinyhttpd — Multithreaded epoll HTTP/1.1 Server

A static-file HTTP/1.1 server in C, built around a single-threaded `epoll`
event loop feeding a bounded-queue thread pool. Written as a from-scratch
systems project — no libevent, no libuv, just BSD sockets and the raw
Linux `epoll` API.

## What it actually does

- Raw BSD sockets, non-blocking, `epoll` edge-triggered (`EPOLLET`) event loop
- `EPOLLONESHOT` + a 16-worker thread pool with a bounded task queue, so a
  connection is only ever handled by one thread at a time (no locking
  needed around a single connection's buffer)
- Hand-rolled HTTP/1.1 request-line + header parser (GET/HEAD only)
- MIME-type routing by file extension, RFC1123 `Date` headers, correct
  `Content-Length`/`Connection` semantics for keep-alive and HTTP/1.0
  vs 1.1
- Idle-connection reaper thread (mitigates Slowloris-style hold-open attacks)
- Bounded per-connection header buffer (8KB cap → `431` instead of unbounded
  memory growth on adversarial clients)
- Path-traversal rejection, `SO_REUSEADDR`, `SIGPIPE` ignored, graceful
  `SIGINT`/`SIGTERM` shutdown that drains in-flight requests before exiting

This has been compiled with `-Wall -Wextra` (zero warnings) and exercised
with curl and raw-socket Python scripts covering: basic GET/HEAD, MIME
type resolution, 404s, path traversal (403), oversized headers (431),
malformed requests (400), keep-alive pipelining, 100 concurrent requests,
idle-connection timeout, and graceful shutdown.

---

## 1. Prerequisites — what you need to know before building this

You don't need all of this mastered up front, but you'll hit every one of
these topics while building it, so it helps to at least skim them first.

**C language / tooling**
- Pointers, structs, manual memory management (`malloc`/`free`), and the
  discipline of checking every allocation and every syscall return value
- Reading `man` pages fluently (`man 2 epoll_wait`, `man 3 recv`, etc. —
  section 2 is syscalls, section 3 is library functions)
- Makefiles and `gcc` flags well enough to know what `-D_GNU_SOURCE`,
  `-pthread`, and `-Wall -Wextra` are doing for you

**Networking fundamentals**
- The BSD sockets API: `socket()`, `bind()`, `listen()`, `accept()`,
  `connect()`, `recv()`/`send()`
- TCP basics: what a connection actually is (a 4-tuple + kernel buffers),
  why `SO_REUSEADDR` matters, what `TCP_NODELAY` (Nagle's algorithm) does
- The client-server request/response model and, ideally, having watched
  raw HTTP traffic in Wireshark or via `nc`/`telnet` at least once

**I/O multiplexing / the C10K problem**
- Blocking vs non-blocking I/O, and why one-thread-per-connection doesn't
  scale to thousands of clients
- `select()`/`poll()` and *why* they don't scale, then `epoll()` and why it
  does (kernel-maintained interest list vs userspace re-scanning every fd)
- Level-triggered vs edge-triggered epoll — this is the single most
  common source of subtle bugs in a project like this (miss one `EAGAIN`
  drain-loop and you'll silently stop getting notified on a busy socket)

**Concurrency**
- `pthread_create`/`join`, mutexes, condition variables
- Producer-consumer queues, and why a *bounded* queue (backpressure) beats
  an unbounded one under load
- Race conditions specific to this design: what happens if a connection is
  being read by a worker thread at the exact moment the timeout-sweeper
  thread decides to close it (this project solves it with an `in_flight`
  flag — see `handle_connection_task` / `sweeper_thread_fn` in `server.c`)

**HTTP/1.1 itself**
- Request line + header grammar, status codes, `Content-Length`, and the
  actually-kind-of-fiddly keep-alive rules (HTTP/1.1 defaults to
  keep-alive, HTTP/1.0 defaults to close, and either can be overridden by
  an explicit `Connection` header)

## 2. Where to actually learn each piece

These are the sources people in this space cite most:

- **Beej's Guide to Network Programming** — the standard first stop for
  BSD sockets in C. Free, short, and it's how most systems programmers
  learned `socket()`/`bind()`/`accept()` in the first place.
  https://beej.us/guide/bgnet/
- **The Linux Programming Interface** by Michael Kerrisk — the
  authoritative reference for everything below the libc line: sockets,
  signals, threads, and a full chapter specifically on `epoll`. If you
  only buy one systems programming book, this is the one people recommend.
- **UNIX Network Programming, Vol. 1** by W. Richard Stevens — older but
  still the canonical text on socket-level network programming and the
  various I/O models (blocking, non-blocking, multiplexed, signal-driven,
  async).
- **The C10K Problem** by Dan Kegel — a (now historical, but still very
  relevant) survey of exactly the problem this project solves: how do you
  serve 10,000+ concurrent connections without one thread per connection.
  http://www.kegel.com/c10k.html
- **`man 7 epoll`** — genuinely worth reading start to finish. It directly
  documents the edge-triggered vs level-triggered distinction and the
  "you must drain until EAGAIN" requirement that this codebase depends on.
- **RFC 7230** (HTTP/1.1 Message Syntax and Routing) and **RFC 7231**
  (Semantics and Content) — the actual spec for what this server
  implements. You don't need to read them cover to cover, but §6.3 of
  7230 (persistence/keep-alive) is directly relevant.
  https://datatracker.ietf.org/doc/html/rfc7230
- **nginx's own architecture docs** — nginx uses the same
  epoll-event-loop-plus-worker-pool family of ideas at a much larger scale;
  reading how a production server thinks about this is a good sanity check
  against this project's design.
  https://nginx.org/en/docs/dev/development_guide.html
- **"Advanced Programming in the UNIX Environment"** by Stevens & Rago —
  the standard reference for the pthreads material (mutexes, condition
  variables) once you're past the basics.

If you want a second worked example of a small C web server to compare
design decisions against (there are many small teaching servers on
GitHub, of varying quality) — search for "tinyhttpd" or "simple epoll
http server C" on GitHub for others' takes on the same problem; comparing
a few different implementations of the same idea is a good way to see
which design decisions are load-bearing and which are just style.

## 3. Architecture

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
        worker thread 1        worker thread 2   ...   worker thread 16
        drain socket until EAGAIN → parse request → route → build
        response → write_all() → keep-alive? re-arm epoll : close fd
```

A second, lightweight thread (the "sweeper") wakes up once a second,
walks the list of live connections, and force-closes any connection that
has been idle longer than the configured timeout — as long as it isn't
currently `in_flight` (owned by a worker).

**Why `EPOLLONESHOT`?** Without it, if a client sends a burst of data,
multiple worker threads could get woken up for the *same* fd
simultaneously and race on the same connection buffer. `EPOLLONESHOT`
guarantees a connection is delivered to exactly one thread at a time; the
worker explicitly re-arms it (`EPOLL_CTL_MOD`) when it's done, which also
naturally implements "only fetch more work once you've finished the last
unit of work."

**Why buffer per-connection instead of parsing straight off the socket?**
Edge-triggered epoll can deliver a read event when only part of a request
has arrived (e.g., a slow client, or a request split across TCP
segments). The server accumulates bytes into `connection_t.buf` across
possibly-many events until `http_parse_request()` sees the terminating
`\r\n\r\n`, at which point it processes exactly one request and slides any
leftover pipelined bytes to the front of the buffer for the next parse.

## 4. Building and running

```bash
make                       # builds ./httpserver
./httpserver -p 8080 -d ./www -t 16 -q 256 -T 30
```

Flags: `-p` port, `-d` docroot, `-t` worker thread count, `-q` queue
capacity, `-T` idle-connection timeout in seconds. `Ctrl+C` triggers a
graceful shutdown (finishes in-flight requests, then exits).

Quick manual test:
```bash
curl http://127.0.0.1:8080/
curl -I http://127.0.0.1:8080/style.css   # check MIME type / headers
curl http://127.0.0.1:8080/does-not-exist # 404
```

## 5. Load testing (to reproduce throughput/latency numbers)

[`wrk`](https://github.com/wg/wrk) is the standard tool for this kind of
benchmark claim:

```bash
# build wrk from source (not in apt on most distros)
git clone https://github.com/wg/wrk.git && cd wrk && make

./wrk -t4 -c200 -d30s http://127.0.0.1:8080/
```

`-t4`: 4 threads (match to core count), `-c200`: 200 concurrent
connections, `-d30s`: run for 30 seconds. wrk reports requests/sec and
latency percentiles (p50/p90/p99) directly — that's where the "12,000+
req/sec, <2ms p99" style of CV bullet comes from in practice. Your actual
numbers will depend on the CPU you run this on, so benchmark on your own
hardware rather than quoting someone else's numbers.

## 6. Known limitations (be ready to talk about these in an interview)

- No HTTPS/TLS (would need OpenSSL and a TLS handshake state machine per
  connection)
- No request body support (fine for a static file server; a real app
  server needs `Content-Length`/chunked body parsing)
- Single-process (no `SO_REUSEPORT` + multi-process fan-out the way nginx
  does for multi-core scaling beyond one thread pool)
- Whole-file reads into memory rather than `sendfile()`/`mmap()` — fine
  for small static assets, not ideal for large files
- The idle-timeout sweeper does a full linked-list scan every second; at
  very high connection counts a timer wheel would scale better
