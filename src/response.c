#include "response.h"

#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <time.h>

typedef struct {
    const char *ext;
    const char *mime;
} mime_entry_t;

static const mime_entry_t MIME_TABLE[] = {
    {".html", "text/html; charset=utf-8"},
    {".htm",  "text/html; charset=utf-8"},
    {".css",  "text/css; charset=utf-8"},
    {".js",   "application/javascript; charset=utf-8"},
    {".json", "application/json; charset=utf-8"},
    {".txt",  "text/plain; charset=utf-8"},
    {".png",  "image/png"},
    {".jpg",  "image/jpeg"},
    {".jpeg", "image/jpeg"},
    {".gif",  "image/gif"},
    {".svg",  "image/svg+xml"},
    {".ico",  "image/x-icon"},
    {".pdf",  "application/pdf"},
    {".xml",  "application/xml"},
    {".woff", "font/woff"},
    {".woff2","font/woff2"},
};

static const size_t MIME_TABLE_LEN = sizeof(MIME_TABLE) / sizeof(MIME_TABLE[0]);

const char *mime_type_for_path(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";

    for (size_t i = 0; i < MIME_TABLE_LEN; i++) {
        if (strcasecmp(dot, MIME_TABLE[i].ext) == 0) {
            return MIME_TABLE[i].mime;
        }
    }
    return "application/octet-stream";
}

const char *status_text(int code) {
    switch (code) {
        case 200: return "OK";
        case 204: return "No Content";
        case 301: return "Moved Permanently";
        case 304: return "Not Modified";
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 408: return "Request Timeout";
        case 413: return "Payload Too Large";
        case 414: return "URI Too Long";
        case 431: return "Request Header Fields Too Large";
        case 500: return "Internal Server Error";
        case 503: return "Service Unavailable";
        default:  return "Unknown";
    }
}

static void rfc1123_date(char *out, size_t out_size) {
    time_t now = time(NULL);
    struct tm tm_utc;
    gmtime_r(&now, &tm_utc);
    strftime(out, out_size, "%a, %d %b %Y %H:%M:%S GMT", &tm_utc);
}

ssize_t build_response(char *buf, size_t buf_size, int status_code,
                        const char *mime_type, const char *body,
                        size_t body_len, int keep_alive, int is_head) {
    char date[64];
    rfc1123_date(date, sizeof(date));

    int header_len = snprintf(
        buf, buf_size,
        "HTTP/1.1 %d %s\r\n"
        "Server: tinyhttpd/1.0\r\n"
        "Date: %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: %s\r\n"
        "\r\n",
        status_code, status_text(status_code), date,
        mime_type ? mime_type : "application/octet-stream",
        body_len, keep_alive ? "keep-alive" : "close");

    if (header_len < 0 || (size_t)header_len >= buf_size) return -1;

    if (!is_head && body_len > 0) {
        if ((size_t)header_len + body_len > buf_size) return -1;
        memcpy(buf + header_len, body, body_len);
    }

    return header_len + (is_head ? 0 : (ssize_t)body_len);
}

ssize_t build_error_response(char *buf, size_t buf_size, int status_code,
                              int keep_alive, int is_head) {
    char body[256];
    int body_len = snprintf(
        body, sizeof(body),
        "<html><head><title>%d %s</title></head>"
        "<body><h1>%d %s</h1></body></html>",
        status_code, status_text(status_code),
        status_code, status_text(status_code));

    if (body_len < 0) body_len = 0;

    return build_response(buf, buf_size, status_code, "text/html; charset=utf-8",
                           body, (size_t)body_len, keep_alive, is_head);
}
