#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

int util_exec(const char *cmd, char **out, size_t *outlen) {
  *out = NULL; *outlen = 0;
  FILE *fp = popen(cmd, "r");
  if (!fp) return -1;
  size_t cap = 8192;
  char *buf = (char*)malloc(cap);
  if (!buf) { pclose(fp); return -1; }
  size_t len = 0;
  size_t n;
  while (!feof(fp)) {
    if (cap - len < 4096) {
      cap *= 2;
      char *nb = (char*)realloc(buf, cap);
      if (!nb) { free(buf); pclose(fp); return -1; }
      buf = nb;
    }
    n = fread(buf + len, 1, 4096, fp);
    len += n;
  }
  pclose(fp);
  *out = buf;
  *outlen = len;
  return 0;
}

int util_read_file(const char *path, char **out, size_t *outlen) {
  *out = NULL; *outlen = 0;
  FILE *f = fopen(path, "rb");
  if (!f) return -1;
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (sz < 0) { fclose(f); return -1; }
  char *buf = (char*)malloc((size_t)sz);
  if (!buf) { fclose(f); return -1; }
  if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) { free(buf); fclose(f); return -1; }
  fclose(f);
  *out = buf;
  *outlen = (size_t)sz;
  return 0;
}

int util_file_exists(const char *path) {
  struct stat st;
  return stat(path, &st) == 0;
}

int util_is_container(void) {
  if (util_file_exists("/.dockerenv")) return 1;
  char *c=NULL; size_t n=0;
  if (util_read_file("/proc/1/cgroup", &c, &n)==0 && c) {
    if (memmem(c, n, "docker", 6) || memmem(c, n, "kubepods", 8)) { free(c); return 1; }
    free(c);
  }
  return 0;
}


#include <sys/stat.h>
int path_exists(const char *p){ struct stat st; return stat(p,&st)==0; }
static int _x_ok(const char *p){ return access(p,X_OK)==0; }
int env_is_edgerouter(void){
  if (path_exists("/etc/edgeos-release")) return 1;
  if (path_exists("/etc/version") && path_exists("/etc/ubnt")) return 1;
  if (_x_ok("/usr/sbin/ubnt-discover") || _x_ok("/sbin/ubnt-discover")) return 1;
  if (path_exists("/tmp/10-all.json")) return 1;
  return 0;
}
