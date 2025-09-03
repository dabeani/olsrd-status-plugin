#ifndef OLSRD_STATUS_UTIL_H
#define OLSRD_STATUS_UTIL_H
#include <stddef.h>
int util_exec(const char *cmd, char **out, size_t *outlen);
int util_read_file(const char *path, char **out, size_t *outlen);
int util_is_container(void);
int util_file_exists(const char *path);
int path_exists(const char *p);
int env_is_edgerouter(void);
int render_connections_plain(char **buf_out, size_t *len_out);
int render_connections_json(char **buf_out, size_t *len_out);
#endif
