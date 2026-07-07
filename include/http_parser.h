#ifndef HTTP_PARSER_H
#define HTTP_PARSER_H

#include <stddef.h>

#define MAX_HEADERS        32
#define MAX_HEADER_NAME    128
#define MAX_HEADER_VALUE   512
#define MAX_PATH_LEN       1024
#define MAX_METHOD_LEN     16
#define MAX_VERSION_LEN    16

/* Hard cap on the size of the request line + headers we will buffer per
 * connection. This bounds memory per connection and is a direct mitigation
 * against slow/oversized-header adversarial clients. Bodies are not
 * supported (server only implements GET/HEAD), so this is effectively the
 * cap on total request size. */
#define MAX_REQUEST_SIZE   8192

typedef struct {
    char name[MAX_HEADER_NAME];
    char value[MAX_HEADER_VALUE];
} http_header_t;

typedef struct {
    char method[MAX_METHOD_LEN];
    char path[MAX_PATH_LEN];
    char version[MAX_VERSION_LEN];
    http_header_t headers[MAX_HEADERS];
    int num_headers;
    int keep_alive;  /* resolved keep-alive decision for this request */
} http_request_t;

typedef enum {
    PARSE_OK,          /* a complete request was parsed                  */
    PARSE_INCOMPLETE,  /* need more bytes before we can parse             */
    PARSE_ERROR,       /* malformed request line/headers                 */
    PARSE_TOO_LARGE    /* header section exceeded MAX_REQUEST_SIZE        */
} parse_status_t;

/* Attempts to parse a full HTTP request (request line + headers,
 * terminated by a blank line) out of `buf` (length `len`).
 *
 * On PARSE_OK, `*consumed` is set to the number of bytes that made up the
 * request (so the caller can slide any pipelined bytes to the front of its
 * buffer). `req` is filled in with the parsed method/path/version/headers
 * and a resolved keep_alive decision. */
parse_status_t http_parse_request(const char *buf, size_t len,
                                   http_request_t *req, size_t *consumed);

/* Case-insensitive header lookup. Returns NULL if not present. */
const char *http_get_header(const http_request_t *req, const char *name);

#endif /* HTTP_PARSER_H */
