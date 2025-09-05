#ifndef OLSRD_STATUS_UTIL_H
#define OLSRD_STATUS_UTIL_H
#include <stddef.h>
int util_exec(const char *cmd, char **out, size_t *outlen);
int util_read_file(const char *path, char **out, size_t *outlen);
/* Perform a simple HTTP GET for local loopback URLs (http://127.0.0.1:PORT/path).
 * Returns 0 on success and fills *out with the response body (headers stripped).
 * Timeout is in seconds for connect+read operations.
 */
int util_http_get_url_local(const char *url, char **out, size_t *outlen, int timeout_sec);
int util_is_container(void);
int util_file_exists(const char *path);
int path_exists(const char *p);
int env_is_edgerouter(void);
int env_is_linux_container(void);
int render_connections_plain(char **buf_out, size_t *len_out);
int render_connections_json(char **buf_out, size_t *len_out);
#endif
