#ifndef RESPONSE_H
#define RESPONSE_H

#include <sys/types.h>
#include <stddef.h>

/* Looks up a MIME type by file extension. Always returns a valid
 * non-NULL string ("application/octet-stream" if unknown). */
const char *mime_type_for_path(const char *path);

/* Human-readable reason phrase for a status code, e.g. "Not Found". */
const char *status_text(int code);

/* Writes a full HTTP response (status line + headers + body) into `buf`.
 * Returns the number of bytes written, or -1 if `buf_size` was too small.
 * `body` may be NULL if body_len is 0 (used for HEAD responses). */
ssize_t build_response(char *buf, size_t buf_size, int status_code,
                        const char *mime_type, const char *body,
                        size_t body_len, int keep_alive, int is_head);

/* Convenience wrapper: builds a small HTML error body for `status_code`
 * and writes the full response into `buf`. */
ssize_t build_error_response(char *buf, size_t buf_size, int status_code,
                              int keep_alive, int is_head);

#endif /* RESPONSE_H */
