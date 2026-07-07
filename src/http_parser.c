#define _GNU_SOURCE
#include "http_parser.h"

#include <string.h>
#include <strings.h>
#include <stdio.h>

const char *http_get_header(const http_request_t *req, const char *name) {
    for (int i = 0; i < req->num_headers; i++) {
        if (strcasecmp(req->headers[i].name, name) == 0) {
            return req->headers[i].value;
        }
    }
    return NULL;
}

/* Trim leading/trailing spaces in-place, return pointer to trimmed start. */
static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t' ||
                       s[len - 1] == '\r' || s[len - 1] == '\n')) {
        s[--len] = '\0';
    }
    return s;
}

parse_status_t http_parse_request(const char *buf, size_t len,
                                   http_request_t *req, size_t *consumed) {
    /* Look for the blank line that terminates the header section. */
    const char *header_end = memmem(buf, len, "\r\n\r\n", 4);
    if (!header_end) {
        if (len >= MAX_REQUEST_SIZE) return PARSE_TOO_LARGE;
        return PARSE_INCOMPLETE;
    }

    size_t total_header_len = (size_t)(header_end - buf) + 4;
    if (total_header_len > MAX_REQUEST_SIZE) return PARSE_TOO_LARGE;

    memset(req, 0, sizeof(*req));

    /* --- Request line: "METHOD SP request-target SP HTTP-version CRLF" --- */
    const char *line_end = memmem(buf, total_header_len, "\r\n", 2);
    if (!line_end) return PARSE_ERROR;

    size_t line_len = (size_t)(line_end - buf);
    if (line_len == 0 || line_len >= 1024) return PARSE_ERROR;

    char line[1024];
    memcpy(line, buf, line_len);
    line[line_len] = '\0';

    char method[MAX_METHOD_LEN];
    char path[MAX_PATH_LEN];
    char version[MAX_VERSION_LEN];

    if (sscanf(line, "%15s %1023s %15s", method, path, version) != 3) {
        return PARSE_ERROR;
    }

    /* Only GET and HEAD are supported -- this is a static file server, not
     * a general application server, so there is no request body handling. */
    if (strcmp(method, "GET") != 0 && strcmp(method, "HEAD") != 0) {
        return PARSE_ERROR;
    }
    if (strncmp(version, "HTTP/", 5) != 0) return PARSE_ERROR;

    snprintf(req->method, MAX_METHOD_LEN, "%s", method);
    snprintf(req->path, MAX_PATH_LEN, "%s", path);
    snprintf(req->version, MAX_VERSION_LEN, "%s", version);

    /* --- Headers --- */
    const char *cursor = line_end + 2; /* skip CRLF of request line */
    const char *headers_stop = header_end + 2; /* the blank line's CRLF */

    while (cursor < headers_stop) {
        const char *next_crlf = memmem(cursor, (size_t)(headers_stop - cursor), "\r\n", 2);
        if (!next_crlf) break;

        size_t hlen = (size_t)(next_crlf - cursor);
        if (hlen == 0) break; /* shouldn't happen, headers_stop already excludes blank line */

        if (hlen < 2048 && req->num_headers < MAX_HEADERS) {
            char hline[2048];
            memcpy(hline, cursor, hlen);
            hline[hlen] = '\0';

            char *colon = strchr(hline, ':');
            if (colon) {
                *colon = '\0';
                char *name = trim(hline);
                char *value = trim(colon + 1);

                http_header_t *h = &req->headers[req->num_headers];
                strncpy(h->name, name, MAX_HEADER_NAME - 1);
                strncpy(h->value, value, MAX_HEADER_VALUE - 1);
                req->num_headers++;
            }
        }

        cursor = next_crlf + 2;
    }

    /* --- Resolve keep-alive per RFC 7230 semantics --- */
    int is_http_1_1 = (strcmp(req->version, "HTTP/1.1") == 0);
    req->keep_alive = is_http_1_1 ? 1 : 0;

    const char *conn_hdr = http_get_header(req, "Connection");
    if (conn_hdr) {
        if (strcasestr(conn_hdr, "close")) {
            req->keep_alive = 0;
        } else if (strcasestr(conn_hdr, "keep-alive")) {
            req->keep_alive = 1;
        }
    }

    *consumed = total_header_len;
    return PARSE_OK;
}
