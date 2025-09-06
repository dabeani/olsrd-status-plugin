#include "httpd.h"
#include <pthread.h>
#include <sys/stat.h>
#include <stdlib.h>
int path_exists(const char *p);
extern int g_is_edgerouter;
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netdb.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <stdarg.h>
#include <ctype.h>

typedef struct http_handler_node {
  char route[64];
  http_handler_fn fn;
  struct http_handler_node *next;
} http_handler_node_t;

static int g_srv_fd = -1;
static pthread_t g_srv_th;
static int g_run = 0;
static char g_asset_root[512] = {0};
static http_handler_node_t *g_handlers = NULL;
/* simple allow-list storage */
typedef struct cidr_entry {
  struct in6_addr addr;
  int prefix; /* 0-128 */
  struct cidr_entry *next;
} cidr_entry_t;
static cidr_entry_t *g_allowed = NULL;

static int parse_cidr(const char *s, struct in6_addr *out_addr, int *out_prefix) {
  if (!s || !out_addr || !out_prefix) return -1;
  char tmp[128]; snprintf(tmp, sizeof(tmp), "%s", s);
  char *sp = strchr(tmp, ' ');
  char *slash = strchr(tmp, '/');
  if (sp) {
    /* format: "addr mask" */
    *sp = 0; char *addr = tmp; char *mask = sp+1; /* mask like 255.255.252.0 */
    struct in_addr a, m;
    if (inet_pton(AF_INET, addr, &a) == 1 && inet_pton(AF_INET, mask, &m) == 1) {
      /* convert to in6 mapped */
      memset(out_addr, 0, sizeof(*out_addr));
      out_addr->s6_addr[10] = 0xff; out_addr->s6_addr[11] = 0xff;
      memcpy(&out_addr->s6_addr[12], &a, 4);
      uint32_t maskbe; memcpy(&maskbe, &m, 4);
      maskbe = ntohl(maskbe);
      int p = 0; for (int i=31;i>=0;--i) if (maskbe & (1u<<i)) p++; else break;
  /* IPv4 occupies the low 32 bits of the IPv6 mapped address; convert to IPv6 prefix space */
  *out_prefix = p + 96;
      return 0;
    }
    return -1;
  } else if (slash) {
    /* cidr like 193.238.156.0/22 or IPv6*/
    if (strchr(tmp, ':')) {
      /* IPv6 */
      char *cp = strchr(tmp,'/'); if (!cp) return -1; *cp=0; char *pfx = cp+1;
      if (inet_pton(AF_INET6, tmp, out_addr) != 1) return -1;
      *out_prefix = atoi(pfx);
      return 0;
    } else {
      char *cp = strchr(tmp,'/'); *cp=0; char *pfx = cp+1; struct in_addr a;
      if (inet_pton(AF_INET, tmp, &a) != 1) return -1;
      memset(out_addr,0,sizeof(*out_addr)); out_addr->s6_addr[10]=0xff; out_addr->s6_addr[11]=0xff; memcpy(&out_addr->s6_addr[12], &a,4);
  /* IPv4 CIDR prefix is relative to the 32-bit IPv4 portion; convert to IPv6 prefix */
  *out_prefix = atoi(pfx) + 96;
      return 0;
    }
  } else {
    /* single address */
    struct in_addr a;
    if (inet_pton(AF_INET, tmp, &a) == 1) {
      memset(out_addr,0,sizeof(*out_addr)); out_addr->s6_addr[10]=0xff; out_addr->s6_addr[11]=0xff; memcpy(&out_addr->s6_addr[12], &a,4);
  /* single IPv4 address -> full 32-bit match inside IPv6 mapped space */
  *out_prefix = 32 + 96; return 0;
    }
    if (inet_pton(AF_INET6, tmp, out_addr) == 1) { *out_prefix = 128; return 0; }
    return -1;
  }
}

int http_allow_cidr(const char *cidr_or_addr_mask) {
  struct in6_addr a; int p;
  if (parse_cidr(cidr_or_addr_mask, &a, &p) != 0) return -1;
  int was_empty = (g_allowed == NULL);
  cidr_entry_t *e = calloc(1, sizeof(*e)); if (!e) return -1;
  e->addr = a; e->prefix = p; e->next = g_allowed; g_allowed = e;
  if (was_empty) {
    fprintf(stderr, "[httpd] access restricted: Net parameter(s) found, only allowed networks will have access\n");
  }
  /* Optional debug: print each added entry in readable form when requested */
  const char *dbg = getenv("OLSR_DEBUG_ALLOWLIST");
  if (dbg && dbg[0]=='1') {
    char buf[128];
    /* show IPv4-mapped addresses as IPv4/prefix when appropriate */
    if (e->addr.s6_addr[10] == 0xff && e->addr.s6_addr[11] == 0xff) {
      char v4[64]; snprintf(v4, sizeof(v4), "%u.%u.%u.%u", (unsigned char)e->addr.s6_addr[12], (unsigned char)e->addr.s6_addr[13], (unsigned char)e->addr.s6_addr[14], (unsigned char)e->addr.s6_addr[15]);
      snprintf(buf, sizeof(buf), "%s/%d", v4, e->prefix - 96);
    } else {
      if (inet_ntop(AF_INET6, &e->addr, buf, sizeof(buf)) == NULL) snprintf(buf, sizeof(buf), "<inet_ntop_fail>");
    }
    fprintf(stderr, "[httpd][debug] added allow-list entry: %s (prefix=%d)\n", buf, e->prefix);
  }
  return 0;
}

static int addr_prefix_match(const struct in6_addr *a, const struct in6_addr *b, int prefix) {
  if (prefix <= 0) return 1;
  int full_bytes = prefix / 8;
  int rem_bits = prefix % 8;
  if (full_bytes > 16) full_bytes = 16;
  if (memcmp(a, b, full_bytes) != 0) return 0;
  if (rem_bits == 0) return 1;
  unsigned char mask = (unsigned char)(0xff << (8 - rem_bits));
  return ( (a->s6_addr[full_bytes] & mask) == (b->s6_addr[full_bytes] & mask) );
}

int http_is_client_allowed(const char *client_ip) {
  if (!client_ip) return 0;
  if (!g_allowed) return 1; /* no restrictions */
  struct in6_addr a; if (strchr(client_ip,':')) {
    if (inet_pton(AF_INET6, client_ip, &a) != 1) return 0;
  } else {
    struct in_addr v4; if (inet_pton(AF_INET, client_ip, &v4) != 1) return 0;
    memset(&a,0,sizeof(a)); a.s6_addr[10]=0xff; a.s6_addr[11]=0xff; memcpy(&a.s6_addr[12], &v4,4);
  }
  const char *dbg = getenv("OLSR_DEBUG_ALLOWLIST");
  int debug = (dbg && dbg[0]=='1') ? 1 : 0;
  if (debug) {
    char cip[64]; snprintf(cip, sizeof(cip), "%s", client_ip);
    fprintf(stderr, "[httpd][debug] checking client IP %s against %s allow-list entries\n", cip, g_allowed?"current":"none");
  }
  for (cidr_entry_t *e = g_allowed; e; e = e->next) {
    if (debug) {
      char buf[128];
      if (e->addr.s6_addr[10] == 0xff && e->addr.s6_addr[11] == 0xff) {
        snprintf(buf, sizeof(buf), "%u.%u.%u.%u/%d", (unsigned char)e->addr.s6_addr[12], (unsigned char)e->addr.s6_addr[13], (unsigned char)e->addr.s6_addr[14], (unsigned char)e->addr.s6_addr[15], e->prefix-96);
      } else {
        if (inet_ntop(AF_INET6, &e->addr, buf, sizeof(buf)) == NULL) snprintf(buf, sizeof(buf), "<inet_ntop_fail>");
      }
      fprintf(stderr, "[httpd][debug] testing entry %s (prefix=%d)\n", buf, e->prefix);
    }
    if (addr_prefix_match(&a, &e->addr, e->prefix)) return 1;
  }
  return 0;
}

void http_clear_allowlist(void) {
  cidr_entry_t *e = g_allowed;
  while (e) { cidr_entry_t *nx = e->next; free(e); e = nx; }
  g_allowed = NULL;
}

void http_log_allowlist(void) {
  if (!g_allowed) { fprintf(stderr, "[httpd] allow-list is empty (no restrictions)\n"); return; }
  fprintf(stderr, "[httpd] configured allow-list entries:\n");
  for (cidr_entry_t *e = g_allowed; e; e = e->next) {
    char buf[128];
    if (e->addr.s6_addr[10] == 0xff && e->addr.s6_addr[11] == 0xff) {
      snprintf(buf, sizeof(buf), "%u.%u.%u.%u/%d", (unsigned char)e->addr.s6_addr[12], (unsigned char)e->addr.s6_addr[13], (unsigned char)e->addr.s6_addr[14], (unsigned char)e->addr.s6_addr[15], e->prefix-96);
    } else {
      if (inet_ntop(AF_INET6, &e->addr, buf, sizeof(buf)) == NULL) snprintf(buf, sizeof(buf), "<inet_ntop_fail>");
    }
    fprintf(stderr, "  - %s (prefix=%d)\n", buf, e->prefix);
  }
}

static int starts_with(const char *s, const char *p) {
  size_t ls = strlen(s), lp = strlen(p);
  return (ls >= lp && strncmp(s, p, lp) == 0) ? 1 : 0;
}

int http_server_register_handler(const char *route, http_handler_fn fn) {
  http_handler_node_t *n = (http_handler_node_t*)calloc(1, sizeof(*n));
  if (!n) return -1;
  snprintf(n->route, sizeof(n->route), "%s", route);
  n->fn = fn;
  n->next = g_handlers;
  g_handlers = n;
  return 0;
}

void http_send_status(http_request_t *r, int code, const char *status) {
  char hdr[256];
  int n = snprintf(hdr, sizeof(hdr),
                   "HTTP/1.1 %d %s\r\n"
                   "Connection: close\r\n"
                   "Server: olsrd-status-plugin\r\n",
                   code, status ? status : "");
  ssize_t _rv = write(r->fd, hdr, n); (void)_rv;
}

void http_write(http_request_t *r, const char *buf, size_t len) {
  ssize_t _rv2 = write(r->fd, buf, len); (void)_rv2;
}

int http_printf(http_request_t *r, const char *fmt, ...) {
  char b[8192];
  va_list ap;
  va_start(ap, fmt);
  /* fmt can be non-literal but we intentionally forward it; suppress the
   * -Wformat-nonliteral warning locally to keep the build clean.
   */
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
#endif
  int n = vsnprintf(b, sizeof(b), fmt, ap);
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
  va_end(ap);
  if (n > 0) { ssize_t _rv3 = write(r->fd, b, (size_t)n); (void)_rv3; }
  return n;
}

static const char *guess_mime(const char *p) {
  const char *ext = strrchr(p, '.');
  if (!ext) return "application/octet-stream";
  ext++;
  if (!strcmp(ext,"css")) return "text/css";
  if (!strcmp(ext,"js")) return "application/javascript";
  if (!strcmp(ext,"png")) return "image/png";
  if (!strcmp(ext,"ico")) return "image/x-icon";
  if (!strcmp(ext,"map")) return "application/json";
  if (!strcmp(ext,"woff")) return "font/woff";
  if (!strcmp(ext,"woff2")) return "font/woff2";
  if (!strcmp(ext,"ttf")) return "font/ttf";
  if (!strcmp(ext,"eot")) return "application/vnd.ms-fontobject";
  if (!strcmp(ext,"svg")) return "image/svg+xml";
  if (!strcmp(ext,"html")) return "text/html; charset=utf-8";
  return "application/octet-stream";
}

int http_send_file(http_request_t *r, const char *asset_root, const char *relpath, const char *mime) {
  char path[1024];
  snprintf(path, sizeof(path), "%s/%s", asset_root, relpath);
  int fd = open(path, O_RDONLY);
  if (fd < 0) {
  fprintf(stderr, "[status-plugin] http_send_file: open('%s') failed: %s (errno=%d)\n", path, strerror(errno), errno);
  http_send_status(r, 404, "Not Found");
  http_printf(r, "Content-Type: text/plain\r\n\r\nnot found\n");
  return -1;
  }
  const char *m = mime ? mime : guess_mime(relpath);
  fprintf(stderr, "[status-plugin] http_send_file: serving '%s' (mime=%s)\n", path, m);
  struct stat st;
  if (fstat(fd, &st) == 0) {
    /* Generate Last-Modified header */
    char lm[128];
    struct tm *gmt = gmtime(&st.st_mtime);
    if (gmt) strftime(lm, sizeof(lm), "%a, %d %b %Y %H:%M:%S GMT", gmt); else lm[0]=0;
    /* Cache static assets for 1 day by default */
    http_send_status(r, 200, "OK");
    if (lm[0]) {
      http_printf(r, "Content-Type: %s\r\nCache-Control: public, max-age=86400\r\nLast-Modified: %s\r\n\r\n", m, lm);
    } else {
      http_printf(r, "Content-Type: %s\r\nCache-Control: public, max-age=86400\r\n\r\n", m);
    }
  } else {
    http_send_status(r, 200, "OK");
    http_printf(r, "Content-Type: %s\r\n\r\n", m);
  }
  char buf[16384];
  ssize_t n;
  while ((n = read(fd, buf, sizeof(buf))) > 0) {
    ssize_t _wret = write(r->fd, buf, (size_t)n); (void)_wret;
  }
  close(fd);
  return 0;
}

static void urldecode(char *s) {
  char *o = s;
  while (*s) {
    if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
      char hx[3] = { s[1], s[2], 0 };
      *o++ = (char)strtol(hx, NULL, 16);
      s += 3;
    } else if (*s == '+') {
      *o++ = ' ';
      s++;
    } else {
      *o++ = *s++;
    }
  }
  *o = 0;
}

static void *server_thread(void *arg) {
  (void)arg;
  while (g_run) {
    struct sockaddr_storage ss;
    socklen_t sl = sizeof(ss);
    int cfd = accept(g_srv_fd, (struct sockaddr *)&ss, &sl);
    if (cfd < 0) {
      if (errno == EINTR) continue;
      if (!g_run) break;
      continue;
    }
    char buf[8192];
    ssize_t n = read(cfd, buf, sizeof(buf)-1);
    if (n <= 0) { close(cfd); continue; }
    /* if we filled the buffer completely, the request may be too large -> reject */
    if ((size_t)n >= sizeof(buf)-1) {
      /* respond with 413 Payload Too Large and close */
      http_request_t r = {0}; r.fd = cfd; snprintf(r.client_ip, sizeof(r.client_ip), "unknown");
      http_send_status(&r, 413, "Payload Too Large");
      http_printf(&r, "Content-Type: text/plain\r\n\r\nRequest too large\n");
      close(cfd);
      continue;
    }
    buf[n] = 0;
    http_request_t r = {0};
    r.fd = cfd;
    snprintf(r.client_ip, sizeof(r.client_ip), "unknown");
    if (ss.ss_family == AF_INET) {
      struct sockaddr_in *in = (struct sockaddr_in*)&ss;
      inet_ntop(AF_INET, &in->sin_addr, r.client_ip, sizeof(r.client_ip));
    } else if (ss.ss_family == AF_INET6) {
      struct sockaddr_in6 *in6 = (struct sockaddr_in6*)&ss;
      inet_ntop(AF_INET6, &in6->sin6_addr, r.client_ip, sizeof(r.client_ip));
    }
    char *sp1 = strchr(buf, ' ');
    if (!sp1) { close(cfd); continue; }
    *sp1 = 0;
    /* accept only safe methods to reduce attack surface */
    snprintf(r.method, sizeof(r.method), "%.7s", buf);
    if (!(strcmp(r.method, "GET") == 0 || strcmp(r.method, "HEAD") == 0)) {
      http_send_status(&r, 405, "Method Not Allowed");
      http_printf(&r, "Content-Type: text/plain\r\n\r\nMethod not allowed\n");
      close(cfd);
      continue;
    }
    char *sp2 = strchr(sp1+1, ' ');
    if (!sp2) { close(cfd); continue; }
    *sp2 = 0;
    char *path = sp1+1;
    char *q = strchr(path, '?');
    if (q) {
      *q = 0;
      /* copy query safely */
      size_t qlen = strnlen(q+1, sizeof(r.query)-1);
      if (qlen >= sizeof(r.query)-1) qlen = sizeof(r.query)-1;
      memcpy(r.query, q+1, qlen); r.query[qlen]=0; urldecode(r.query);
    }
    /* copy path safely */
    size_t plen = strnlen(path, sizeof(r.path)-1);
    if (plen >= sizeof(r.path)-1) plen = sizeof(r.path)-1;
    memcpy(r.path, path, plen); r.path[plen]=0;
    char *hosth = strcasestr(sp2+1, "\nHost:");
    if (hosth) {
      hosth += 6;
      while (*hosth==' ' || *hosth=='\t') hosth++;
      char *e = strpbrk(hosth, "\r\n");
      if (e) *e = 0;
      /* copy host safely */
      size_t hlen = strnlen(hosth, sizeof(r.host)-1);
      if (hlen >= sizeof(r.host)-1) hlen = sizeof(r.host)-1;
      memcpy(r.host, hosth, hlen); r.host[hlen]=0;
    }
    /* Log incoming request */
    fprintf(stderr, "[httpd] request: %s %s from %s query='%s' host='%s'\n", r.method, r.path, r.client_ip, r.query, r.host);
    /* Enforce allow-list even for static assets */
    if (!http_is_client_allowed(r.client_ip)) {
      fprintf(stderr, "[httpd] client %s not allowed to access %s\n", r.client_ip, r.path);
      http_send_status(&r, 403, "Forbidden");
      http_printf(&r, "Content-Type: text/plain\r\n\r\nforbidden\n");
      close(cfd);
      continue;
    }
    if (starts_with(r.path, "/css/") || starts_with(r.path, "/js/") || starts_with(r.path, "/fonts/")) {
      fprintf(stderr, "[httpd] static asset request: %s (serve from %s)\n", r.path, g_asset_root);
      http_send_file(&r, g_asset_root, r.path+1, NULL);
      close(cfd);
      continue;
    }
    http_handler_node_t *nptr = g_handlers;
    int handled = 0;
    while (nptr) {
      if (strcmp(nptr->route, r.path) == 0) {
        handled = 1;
        fprintf(stderr, "[httpd] dispatch to handler: %s\n", nptr->route);
        /* enforce access control */
        if (!http_is_client_allowed(r.client_ip)) {
          fprintf(stderr, "[httpd] client %s not allowed to access %s\n", r.client_ip, nptr->route);
          http_send_status(&r, 403, "Forbidden");
          http_printf(&r, "Content-Type: text/plain\r\n\r\nforbidden\n");
        } else {
          nptr->fn(&r);
        }
        break;
      }
      nptr = nptr->next;
    }
    if (!handled) {
      fprintf(stderr, "[httpd] 404 Not Found: %s\n", r.path);
      http_send_status(&r, 404, "Not Found");
      http_printf(&r, "Content-Type: text/plain\r\n\r\nnot found: %s\n", r.path);
    }
    close(cfd);
  }
  return NULL;
}

int http_server_start(const char *bind_ip, int port, const char *asset_root) {
  if (asset_root) snprintf(g_asset_root, sizeof(g_asset_root), "%s", asset_root);
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  struct sockaddr_in a;
  memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  a.sin_port = htons((uint16_t)port);
  a.sin_addr.s_addr = inet_addr(bind_ip ? bind_ip : "0.0.0.0");
  if (bind(fd, (struct sockaddr*)&a, sizeof(a)) < 0) { close(fd); return -1; }
  if (listen(fd, 64) < 0) { close(fd); return -1; }
  g_srv_fd = fd;
  g_run = 1;
  if (pthread_create(&g_srv_th, NULL, server_thread, NULL) != 0) {
    close(fd); g_srv_fd = -1; g_run = 0; return -1;
  }
  return 0;
}

void http_server_stop(void) {
  if (!g_run) return;
  g_run = 0;
  if (g_srv_fd >= 0) { shutdown(g_srv_fd, SHUT_RDWR); close(g_srv_fd); g_srv_fd = -1; }
  pthread_join(g_srv_th, NULL);
  http_handler_node_t *n = g_handlers;
  while (n) { http_handler_node_t *nx = n->next; free(n); n = nx; }
  g_handlers = NULL;
}


/* --- built-in connections (forward decl from connections.c) --- */
int render_connections_plain(char **buf_out, size_t *len_out);
static void h_connections_builtin(http_request_t *r) __attribute__((unused));
static void h_connections_builtin(http_request_t *r){
  char *buf=NULL; size_t len=0;
  if (render_connections_plain(&buf,&len)==0 && buf && len>0){
    http_send_status(r, 200, "OK");
    http_write(r, buf, len);
    free(buf);
  } else {
    http_send_status(r, 204, "No Content");
  }
}

/* --- capabilities JSON --- */
static void h_capabilities(http_request_t *r) __attribute__((unused));
static void h_capabilities(http_request_t *r){
  http_send_status(r, 200, "OK");
  http_printf(r,
    "{\"is_edgerouter\":%s,"
     "\"discover\":%s,"
     "\"airos\":%s,"
     "\"connections\":true,"
     "\"traffic\":%s,"
     "\"txtinfo\":true,"
     "\"jsoninfo\":true}",
    g_is_edgerouter ? "true":"false",
    g_is_edgerouter ? "true":"false",
    g_is_edgerouter ? "true":"false",
    path_exists("/tmp") ? "true":"false"

  );
}

/* Serve index.html at "/" from assetroot; fallback to text summary */
static void h_root_fallback(http_request_t *r) __attribute__((unused));
static void h_root_fallback(http_request_t *r) {
  http_send_status(r, 200, "OK");
  http_printf(r, "olsrd-status-plugin\n(no index.html found at assetroot: %s)\n", g_asset_root);
}

/* Serve index.html at "/" using the existing asset server */
static void h_root(http_request_t *r) __attribute__((unused));
static void h_root(http_request_t *r){
  if (http_send_file(r, g_asset_root, "index.html", "text/html; charset=utf-8") != 0){
    /* Fallback to embedded minimal HTML */
    http_send_status(r, 200, "OK");
    http_printf(r, "Content-Type: text/html; charset=utf-8\r\n\r\n");
    http_printf(r, "<!DOCTYPE html><html><head><title>OLSR Status</title></head><body>\n");
    http_printf(r, "<h1>OLSR Status Plugin</h1>\n");
    http_printf(r, "<p>Asset files not found at: %s</p>\n", g_asset_root);
    http_printf(r, "<p>Check plugin installation and assetroot parameter.</p>\n");
    http_printf(r, "<p><a href='/status'>Status JSON</a> | <a href='/capabilities'>Capabilities</a></p>\n");
    http_printf(r, "</body></html>\n");
  }
}
