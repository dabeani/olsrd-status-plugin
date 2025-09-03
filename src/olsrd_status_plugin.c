#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <unistd.h>
#include <ifaddrs.h>
#if defined(__linux__)
# include <sys/sysinfo.h>
#endif

#include "httpd.h"
#include "util.h"
#include "olsrd_plugin.h"

#include <stdarg.h>

/* helper: return 1 if buffer contains any non-whitespace byte */
static int buffer_has_content(const char *b, size_t n) {
  if (!b || n == 0) return 0;
  for (size_t i = 0; i < n; i++) if (!isspace((unsigned char)b[i])) return 1;
  return 0;
}

/* JSON buffer helpers used by h_status and other responders */
static int json_buf_append(char **bufptr, size_t *lenptr, size_t *capptr, const char *fmt, ...) {
  va_list ap; char *t = NULL; int n;
  va_start(ap, fmt);
#if defined(_GNU_SOURCE) || defined(__GNU__)
#if defined(__GNUC__)
  _Pragma("GCC diagnostic push");
  _Pragma("GCC diagnostic ignored \"-Wformat-nonliteral\"");
#endif
  n = vasprintf(&t, fmt, ap);
#if defined(__GNUC__)
  _Pragma("GCC diagnostic pop");
#endif
#else
  /* fallback: estimate with a stack buffer */
  char _tmpbuf[1024];
  n = vsnprintf(_tmpbuf, sizeof(_tmpbuf), fmt, ap);
  if (n >= 0) {
    t = malloc((size_t)n + 1);
    if (t) memcpy(t, _tmpbuf, (size_t)n + 1);
  }
#endif
  va_end(ap);
  if (n < 0 || !t) return -1;
  if (*lenptr + (size_t)n + 1 > *capptr) {
    while (*capptr < *lenptr + (size_t)n + 1) *capptr *= 2;
    char *nb = (char*)realloc(*bufptr, *capptr);
    if (!nb) { free(t); return -1; }
    *bufptr = nb;
  }
  memcpy(*bufptr + *lenptr, t, (size_t)n);
  *lenptr += (size_t)n; (*bufptr)[*lenptr] = 0;
  free(t);
  return 0;
}

static int json_append_escaped(char **bufptr, size_t *lenptr, size_t *capptr, const char *s) {
  if (!s) { return json_buf_append(bufptr, lenptr, capptr, "\"\""); }
  if (json_buf_append(bufptr, lenptr, capptr, "\"") < 0) return -1;
  const unsigned char *p = (const unsigned char*)s;
  for (; *p; ++p) {
    unsigned char c = *p;
    switch (c) {
      case '"': if (json_buf_append(bufptr, lenptr, capptr, "\\\"") < 0) return -1; break;
      case '\\': if (json_buf_append(bufptr, lenptr, capptr, "\\\\") < 0) return -1; break;
      case '\b': if (json_buf_append(bufptr, lenptr, capptr, "\\b") < 0) return -1; break;
      case '\f': if (json_buf_append(bufptr, lenptr, capptr, "\\f") < 0) return -1; break;
      case '\n': if (json_buf_append(bufptr, lenptr, capptr, "\\n") < 0) return -1; break;
      case '\r': if (json_buf_append(bufptr, lenptr, capptr, "\\r") < 0) return -1; break;
      case '\t': if (json_buf_append(bufptr, lenptr, capptr, "\\t") < 0) return -1; break;
      default:
        if (c < 0x20) {
          if (json_buf_append(bufptr, lenptr, capptr, "\\u%04x", c) < 0) return -1;
        } else {
          char t[2] = { (char)c, 0 };
          if (json_buf_append(bufptr, lenptr, capptr, "%s", t) < 0) return -1;
        }
    }
  }
  if (json_buf_append(bufptr, lenptr, capptr, "\"") < 0) return -1;
  return 0;
}

/* global flags set at init */
extern int g_is_edgerouter;
extern int g_has_traceroute;
extern char g_traceroute_path[];


/* Return first existing path from candidates or NULL */
static const char *find_first_existing(const char **candidates) {
  for (const char **p = candidates; p && *p; ++p) if (path_exists(*p)) return *p;
  return NULL;
}

/* Build devices JSON from /proc/net/arp as a fallback when ubnt-discover is not available */
static int devices_from_arp_json(char **out, size_t *outlen) {
  char line[512]; FILE *f = fopen("/proc/net/arp", "r"); if (!f) return -1;
  /* skip header */ if (!fgets(line, sizeof(line), f)) { fclose(f); return -1; }
  size_t cap = 4096; size_t len = 0; char *buf = malloc(cap); if (!buf) { fclose(f); return -1; }
  buf[0]=0; json_buf_append(&buf, &len, &cap, "["); int first = 1;
  while (fgets(line, sizeof(line), f)) {
    char ip[64], hw[64], dev[64]; ip[0]=hw[0]=dev[0]=0;
    if (sscanf(line, "%63s %*s %*s %63s %*s %63s", ip, hw, dev) >= 1) {
      if (!first) json_buf_append(&buf, &len, &cap, ",");
      first = 0;
      /* attempt reverse lookup */
      char namebuf[256] = "";
      struct in_addr ina; if (inet_aton(ip, &ina)) {
        struct hostent *he = gethostbyaddr(&ina, sizeof(ina), AF_INET);
        if (he && he->h_name) snprintf(namebuf, sizeof(namebuf), "%s", he->h_name);
      }
  json_buf_append(&buf, &len, &cap, "{\"ipv4\":"); json_append_escaped(&buf,&len,&cap, ip);
  json_buf_append(&buf, &len, &cap, ",\"hwaddr\":"); json_append_escaped(&buf,&len,&cap, hw);
  json_buf_append(&buf, &len, &cap, ",\"hostname\":"); json_append_escaped(&buf,&len,&cap, namebuf);
  json_buf_append(&buf, &len, &cap, ",\"product\":\"\",\"uptime\":\"\",\"mode\":\"\",\"essid\":\"\",\"firmware\":\"\"}");
    }
  }
  fclose(f);
  json_buf_append(&buf, &len, &cap, "]\n"); *out = buf; *outlen = len; return 0;
}

/* Try to obtain ubnt-discover output: prefer binary if present, else /tmp/10-all.json, else arp fallback */
static int ubnt_discover_output(char **out, size_t *outlen) {
  const char *ubnt_candidates[] = { "/usr/sbin/ubnt-discover", "/sbin/ubnt-discover", "/usr/bin/ubnt-discover", "/usr/local/sbin/ubnt-discover", "/usr/local/bin/ubnt-discover", "/opt/ubnt/ubnt-discover", NULL };
  const char *ub = find_first_existing(ubnt_candidates);
  if (ub) {
    char cmd[512]; snprintf(cmd, sizeof(cmd), "%s -d 150 -V -i none -j", ub);
    if (util_exec(cmd, out, outlen) == 0 && *out && *outlen>0) return 0;
  }
  /* try preexisting dump */
  if (path_exists("/tmp/10-all.json")) {
    if (util_read_file("/tmp/10-all.json", out, outlen) == 0 && *out && *outlen>0) return 0;
  }
  /* fallback to arp-based device list */
  if (devices_from_arp_json(out, outlen) == 0) return 0;
  return -1;
}

/* Extract a quoted JSON string value for key from a JSON object region.
 * Searches for '"key"' after 'start' and returns the pointer to the value (not allocated) and length in val_len.
 * Returns 1 on success, 0 otherwise.
 */
static int find_json_string_value(const char *start, const char *key, const char **val, size_t *val_len) {
  if (!start || !key || !val || !val_len) return 0;
  char needle[128]; snprintf(needle, sizeof(needle), "\"%s\"", key);
  const char *p = start;
  while ((p = strstr(p, needle)) != NULL) {
    const char *q = p + strlen(needle);
    /* skip whitespace */ while (*q && (*q==' '||*q=='\t'||*q=='\r'||*q=='\n')) q++;
    if (*q != ':') { p = q; continue; }
    q++; while (*q && (*q==' '||*q=='\t'||*q=='\r'||*q=='\n')) q++;
    if (*q == '"') {
      q++; const char *vstart = q; const char *r = q;
      while (*r) {
        if (*r == '\\' && r[1]) { r += 2; continue; }
        if (*r == '"') { *val = vstart; *val_len = (size_t)(r - vstart); return 1; }
        r++;
      }
      return 0;
    } else {
      /* not a quoted string: capture until comma or closing brace */
      const char *vstart = q; const char *r = q;
      while (*r && *r != ',' && *r != '}' && *r != '\n') r++;
      while (r > vstart && (*(r-1)==' '||*(r-1)=='\t')) r--;
      *val = vstart; *val_len = (size_t)(r - vstart); return 1;
    }
  }
  return 0;
}

/* Normalize devices array from ubnt-discover JSON string `ud` into a new allocated JSON array in *outbuf (caller must free). */
static int normalize_ubnt_devices(const char *ud, char **outbuf, size_t *outlen) {
  if (!ud || !outbuf || !outlen) return -1;
  *outbuf = NULL; *outlen = 0;
  size_t cap = 4096; size_t len = 0; char *buf = malloc(cap); if (!buf) return -1; buf[0]=0;
  /* simple search for "devices" : [ */
  const char *p = strstr(ud, "\"devices\"" );
  if (!p) { /* no devices key */ json_buf_append(&buf, &len, &cap, "[]"); *outbuf=buf; *outlen=len; return 0; }
  const char *arr = strchr(p, '[');
  if (!arr) { json_buf_append(&buf,&len,&cap,"[]"); *outbuf=buf; *outlen=len; return 0; }
  /* iterate objects inside array by scanning braces */
  const char *q = arr; int depth = 0;
  while (*q) {
    if (*q == '[') { depth++; q++; continue; }
  if (*q == ']') { depth--; if (depth==0) { break; } q++; continue; }
    if (*q == '{') {
      /* parse object from q to matching } */
      const char *obj_start = q; int od = 0; const char *r = q;
      while (*r) {
        if (*r == '{') od++; else if (*r == '}') { od--; if (od==0) { r++; break; } }
        r++;
      }
      if (!r || r<=obj_start) break;
      /* within obj_start..r extract fields */
      const char *v; size_t vlen;
      /* fields to extract: ipv4 (or ip), mac or hwaddr, name/hostname, product, uptime, mode, essid, firmware */
      char ipv4[64] = ""; char hwaddr[64] = ""; char hostname[256] = ""; char product[128] = ""; char uptime[64] = ""; char mode[64] = ""; char essid[128] = ""; char firmware[128] = "";
      if (find_json_string_value(obj_start, "ipv4", &v, &vlen) || find_json_string_value(obj_start, "ip", &v, &vlen)) { snprintf(ipv4, sizeof(ipv4), "%.*s", (int)vlen, v); }
      if (find_json_string_value(obj_start, "mac", &v, &vlen) || find_json_string_value(obj_start, "hwaddr", &v, &vlen)) { snprintf(hwaddr, sizeof(hwaddr), "%.*s", (int)vlen, v); }
      if (find_json_string_value(obj_start, "name", &v, &vlen) || find_json_string_value(obj_start, "hostname", &v, &vlen)) { snprintf(hostname, sizeof(hostname), "%.*s", (int)vlen, v); }
      if (find_json_string_value(obj_start, "product", &v, &vlen)) { snprintf(product, sizeof(product), "%.*s", (int)vlen, v); }
      if (find_json_string_value(obj_start, "uptime", &v, &vlen)) { snprintf(uptime, sizeof(uptime), "%.*s", (int)vlen, v); }
      if (find_json_string_value(obj_start, "mode", &v, &vlen)) { snprintf(mode, sizeof(mode), "%.*s", (int)vlen, v); }
      if (find_json_string_value(obj_start, "essid", &v, &vlen)) { snprintf(essid, sizeof(essid), "%.*s", (int)vlen, v); }
      if (find_json_string_value(obj_start, "firmware", &v, &vlen)) { snprintf(firmware, sizeof(firmware), "%.*s", (int)vlen, v); }

      /* append comma if not first */
      if (len > 2) json_buf_append(&buf, &len, &cap, ",");
      /* append normalized object */
      json_buf_append(&buf, &len, &cap, "{\"ipv4\":"); json_append_escaped(&buf, &len, &cap, ipv4); json_buf_append(&buf,&len,&cap, ",\"hwaddr\":"); json_append_escaped(&buf,&len,&cap, hwaddr);
      json_buf_append(&buf,&len,&cap, ",\"hostname\":"); json_append_escaped(&buf,&len,&cap, hostname);
      json_buf_append(&buf,&len,&cap, ",\"product\":"); json_append_escaped(&buf,&len,&cap, product);
      json_buf_append(&buf,&len,&cap, ",\"uptime\":"); json_append_escaped(&buf,&len,&cap, uptime);
      json_buf_append(&buf,&len,&cap, ",\"mode\":"); json_append_escaped(&buf,&len,&cap, mode);
      json_buf_append(&buf,&len,&cap, ",\"essid\":"); json_append_escaped(&buf,&len,&cap, essid);
      json_buf_append(&buf,&len,&cap, ",\"firmware\":"); json_append_escaped(&buf,&len,&cap, firmware);
      json_buf_append(&buf,&len,&cap, "}");

      q = r; continue;
    }
    q++;
  }
  /* wrap with brackets */
  char *full = malloc(len + 4);
  if (!full) { free(buf); return -1; }
  full[0] = '['; if (len>0) memcpy(full+1, buf, len); full[1+len] = ']'; full[2+len] = '\n'; full[3+len]=0;
  free(buf);
  *outbuf = full; *outlen = 3 + len; return 0;
}

/* Normalize olsrd API JSON links into simple array expected by UI
 * For each link object, produce {intf, local, remote, remote_host, lq, nlq, cost, routes, nodes}
 */
static int normalize_olsrd_links(const char *raw, char **outbuf, size_t *outlen) {
  if (!raw || !outbuf || !outlen) return -1;
  *outbuf = NULL; *outlen = 0;
  const char *p = strstr(raw, "\"links\"");
  if (!p) return -1;
  const char *arr = strchr(p, '[');
  if (!arr) return -1;
  const char *q = arr; int depth = 0;
  size_t cap = 4096; size_t len = 0; char *buf = malloc(cap); if (!buf) return -1; buf[0]=0;
  json_buf_append(&buf, &len, &cap, "["); int first = 1;
  while (*q) {
    if (*q == '[') { depth++; q++; continue; }
    if (*q == ']') { depth--; if (depth==0) break; q++; continue; }
    if (*q == '{') {
      const char *obj = q; int od = 0; const char *r = q;
      while (*r) { if (*r=='{') od++; else if (*r=='}') { od--; if (od==0) { r++; break; } } r++; }
      if (!r || r<=obj) break;
      /* extract fields */
      const char *v; size_t vlen;
      char intf[64] = ""; char local[64] = ""; char remote[64] = ""; char remote_host[256] = ""; char lq[32] = ""; char nlq[32] = ""; char cost[32] = "";
      if (find_json_string_value(obj, "olsrInterface", &v, &vlen) || find_json_string_value(obj, "ifName", &v, &vlen)) snprintf(intf, sizeof(intf), "%.*s", (int)vlen, v);
      if (find_json_string_value(obj, "localIP", &v, &vlen)) snprintf(local, sizeof(local), "%.*s", (int)vlen, v);
      if (find_json_string_value(obj, "remoteIP", &v, &vlen)) snprintf(remote, sizeof(remote), "%.*s", (int)vlen, v);
      if (find_json_string_value(obj, "linkQuality", &v, &vlen)) snprintf(lq, sizeof(lq), "%.*s", (int)vlen, v);
      if (find_json_string_value(obj, "neighborLinkQuality", &v, &vlen)) snprintf(nlq, sizeof(nlq), "%.*s", (int)vlen, v);
      if (find_json_string_value(obj, "linkCost", &v, &vlen)) snprintf(cost, sizeof(cost), "%.*s", (int)vlen, v);
      if (!first) json_buf_append(&buf, &len, &cap, ","); first = 0;
      json_buf_append(&buf, &len, &cap, "{\"intf\":"); json_append_escaped(&buf,&len,&cap,intf);
      json_buf_append(&buf, &len, &cap, ",\"local\":"); json_append_escaped(&buf,&len,&cap,local);
      json_buf_append(&buf, &len, &cap, ",\"remote\":"); json_append_escaped(&buf,&len,&cap,remote);
      json_buf_append(&buf, &len, &cap, ",\"remote_host\":"); json_append_escaped(&buf,&len,&cap,remote_host);
      json_buf_append(&buf, &len, &cap, ",\"lq\":"); json_append_escaped(&buf,&len,&cap,lq);
      json_buf_append(&buf, &len, &cap, ",\"nlq\":"); json_append_escaped(&buf,&len,&cap,nlq);
      json_buf_append(&buf, &len, &cap, ",\"cost\":"); json_append_escaped(&buf,&len,&cap,cost);
      json_buf_append(&buf, &len, &cap, ",\"routes\":\"\",\"nodes\":\"\"}");
      q = r; continue;
    }
    q++; 
  }
  json_buf_append(&buf, &len, &cap, "]"); *outbuf = buf; *outlen = len; return 0;
}

/* forward decls for local helpers used before their definitions */
static void send_text(http_request_t *r, const char *text);
static void send_json(http_request_t *r, const char *json);
static int get_query_param(http_request_t *r, const char *key, char *out, size_t outlen);

static int h_airos(http_request_t *r);
static int h_traffic(http_request_t *r);

/* index.html moved to external asset at www/index.html; served from g_asset_root */
static int h_status(http_request_t *r) {
  /* Build a JSON status object used by the frontend (app.js).
   * Fields provided: hostname, ip, uptime (seconds), devices (empty/default), airosdata (from /tmp/10-all.json),
   * default_route (ip, dev), links (empty array), olsr2_on (bool).
   * Behavior adapts to EdgeRouter detection (g_is_edgerouter) and available tools.
   */
  char *buf = NULL; size_t cap = 8192, len = 0;
  buf = (char*)malloc(cap);
  if (!buf) { send_json(r, "{}\n"); return 0; }
  #define APPEND(fmt,...) do{ \
    char _t[1024]; int _n = snprintf(_t, sizeof(_t), fmt, ##__VA_ARGS__); \
    if (_n >= (int)sizeof(_t)) _n = (int)sizeof(_t) - 1; \
    if (_n > 0) { \
      if (len + _n + 1 > cap) { while (cap < len + _n + 1) cap *= 2; char *nb = (char*)realloc(buf, cap); if(!nb){ free(buf); send_json(r, "{}\n"); return 0; } buf = nb; } \
      memcpy(buf + len, _t, (size_t)_n); len += _n; buf[len]=0; } \
  } while(0)

  /* use json_append_escaped(...) helper defined above */

  /* hostname */
  char hostname[256] = ""; if (gethostname(hostname, sizeof(hostname))==0) hostname[sizeof(hostname)-1]=0;

  /* primary IPv4: pick first non-loopback IPv4 */
  char ipaddr[128] = "";
  struct ifaddrs *ifap = NULL, *ifa = NULL;
  if (getifaddrs(&ifap) == 0) {
    for (ifa = ifap; ifa; ifa = ifa->ifa_next) {
      if (!ifa->ifa_addr) continue;
      if (ifa->ifa_addr->sa_family == AF_INET) {
        char bufip[INET_ADDRSTRLEN] = "";
        struct sockaddr_in sa_local;
        memset(&sa_local, 0, sizeof(sa_local));
        memcpy(&sa_local, ifa->ifa_addr, sizeof(sa_local));
        inet_ntop(AF_INET, &sa_local.sin_addr, bufip, sizeof(bufip));
        if (bufip[0] && strcmp(bufip, "127.0.0.1") != 0) { snprintf(ipaddr, sizeof(ipaddr), "%s", bufip); break; }
      }
    }
    freeifaddrs(ifap);
  }

  /* uptime in seconds (first token of /proc/uptime) */
  char *upt = NULL; size_t un = 0; long uptime_seconds = 0;
  if (util_read_file("/proc/uptime", &upt, &un) == 0 && upt && un>0) {
    uptime_seconds = (long)atof(upt);
    free(upt); upt = NULL;
  }
  /* fallback: if /proc/uptime didn't yield a value, try sysinfo() on Linux */
#if defined(__linux__)
  if (uptime_seconds == 0) {
    struct sysinfo si;
    if (sysinfo(&si) == 0) uptime_seconds = (long)si.uptime;
  }
#endif

  /* airosdata: include raw JSON from /tmp/10-all.json if present */
  char *airos_raw = NULL; size_t airos_n = 0;
  int have_airos = 0;
  if (util_read_file("/tmp/10-all.json", &airos_raw, &airos_n) == 0 && airos_raw && airos_n > 0) have_airos = 1;

  /* default route: parse `ip route show default` */
  char def_ip[64] = ""; char def_dev[64] = "";
  char *rout = NULL; size_t rn = 0;
  if (util_exec("/sbin/ip route show default 2>/dev/null || ip route show default 2>/dev/null", &rout, &rn) == 0 && rout && rn>0) {
    /* sample: default via 78.41.115.1 dev eth0 proto static
       find 'via' and 'dev' tokens */
    char *p = strstr(rout, "via "); if (p) { p += 4; char *q = strchr(p, ' '); if (q) { size_t L = q - p; if (L < sizeof(def_ip)) { strncpy(def_ip, p, L); def_ip[L]=0; } } }
    p = strstr(rout, " dev "); if (p) { p += 5; char *q = strchr(p, ' '); if (!q) q = strchr(p, '\n'); if (q) { size_t L = q - p; if (L < sizeof(def_dev)) { strncpy(def_dev, p, L); def_dev[L]=0; } else snprintf(def_dev, sizeof(def_dev), "%s", p); } else snprintf(def_dev, sizeof(def_dev), "%s", p); }
    free(rout); rout = NULL;
  }

  /* detect olsr2 presence by pidof */
  char *pout = NULL; size_t pn = 0; int olsr2_on = 0;
  if (util_exec("pidof olsrd2 2>/dev/null || pidof olsrd 2>/dev/null", &pout, &pn) == 0 && pout && pn>0) {
    olsr2_on = 1; /* keep pout for diagnostics, free after JSON emitted */
  }

  /* try to fetch olsrd links from local JSON API endpoints if pidof didn't detect process */
  char *olsr_links_raw = NULL; size_t oln = 0;
  if (!olsr2_on) {
    const char *endpoints[] = { "http://127.0.0.1:9090/links", "http://127.0.0.1:2006/links", "http://127.0.0.1:8123/links", NULL };
    for (const char **ep = endpoints; *ep && !olsr_links_raw; ++ep) {
      char cmd[256]; snprintf(cmd, sizeof(cmd), "/usr/bin/curl -s --max-time 1 %s", *ep);
      if (util_exec(cmd, &olsr_links_raw, &oln) == 0 && olsr_links_raw && oln>0) {
        olsr2_on = 1; break;
      }
      if (olsr_links_raw) { free(olsr_links_raw); olsr_links_raw = NULL; oln = 0; }
    }
  }

  /* Build JSON */
  APPEND("{");
  APPEND("\"hostname\":"); json_append_escaped(&buf, &len, &cap, hostname); APPEND(",");
  APPEND("\"ip\":"); json_append_escaped(&buf, &len, &cap, ipaddr); APPEND(",");
  APPEND("\"uptime\":\"%ld\",", uptime_seconds);

  /* default_route */
  /* attempt reverse DNS for default route IP to provide a hostname for the gateway */
  char def_hostname[256] = "";
  if (def_ip[0]) {
    struct in_addr ina;
    if (inet_aton(def_ip, &ina)) {
      struct hostent *he = gethostbyaddr(&ina, sizeof(ina), AF_INET);
      if (he && he->h_name) snprintf(def_hostname, sizeof(def_hostname), "%s", he->h_name);
    }
  }
  APPEND("\"default_route\":{");
  APPEND("\"ip\":"); json_append_escaped(&buf, &len, &cap, def_ip); APPEND(",");
  APPEND("\"dev\":"); json_append_escaped(&buf, &len, &cap, def_dev); APPEND(",");
  APPEND("\"hostname\":"); json_append_escaped(&buf, &len, &cap, def_hostname);
  APPEND("},");

  /* devices will be populated below from ubnt-discover or arp fallback */

  /* airosdata: raw JSON if present */
  if (have_airos) {
    APPEND("\"airosdata\":");
    /* ensure airos_raw is valid JSON; insert raw bytes directly */
    APPEND("%s", airos_raw);
    APPEND(",");
  } else {
    APPEND("\"airosdata\":{} ,");
  }

  /* include versions.sh output under "versions" if available */
  char *vout = NULL; size_t vn = 0;
  if (path_exists("/config/custom/versions.sh")) {
    if (util_exec("/config/custom/versions.sh", &vout, &vn) == 0 && vout && vn>0) {
      APPEND("\"versions\":%s,", vout); free(vout); vout = NULL;
    }
  } else if (path_exists("/usr/share/olsrd-status-plugin/versions.sh")) {
    if (util_exec("/usr/share/olsrd-status-plugin/versions.sh", &vout, &vn) == 0 && vout && vn>0) {
      APPEND("\"versions\":%s,", vout); free(vout); vout = NULL;
    }
  } else if (path_exists("./versions.sh")) {
    if (util_exec("./versions.sh", &vout, &vn) == 0 && vout && vn>0) {
      APPEND("\"versions\":%s,", vout); free(vout); vout = NULL;
    }
  }
  /* attempt to read traceroute_to from settings.inc (same path as python reference) */
  char traceroute_to[256] = "";
  {
    char *s=NULL; size_t sn=0;
    if (util_read_file("/config/custom/www/settings.inc", &s, &sn) == 0 && s && sn>0) {
      /* simple parse: look for traceroute_to= value */
      char *line = s; char *end = s + sn;
      while (line && line < end) {
        char *nl = memchr(line, '\n', (size_t)(end - line));
        size_t linelen = nl ? (size_t)(nl - line) : (size_t)(end - line);
        if (linelen > 0 && memmem(line, linelen, "traceroute_to", 12)) {
          /* find '=' */
          char *eq = memchr(line, '=', linelen);
          if (eq) {
            char *v = eq + 1; size_t vlen = (size_t)(end - v);
            /* trim semicolons, quotes and whitespace */
            while (vlen && (v[vlen-1]=='\n' || v[vlen-1]=='\r' || v[vlen-1]==' ' || v[vlen-1]=='\'' || v[vlen-1]=='\"' || v[vlen-1]==';')) vlen--;
            while (vlen && (*v==' ' || *v=='\'' || *v=='\"')) { v++; vlen--; }
            if (vlen > 0) {
              size_t copy = vlen < sizeof(traceroute_to)-1 ? vlen : sizeof(traceroute_to)-1;
              memcpy(traceroute_to, v, copy); traceroute_to[copy]=0;
            }
          }
        }
        if (!nl) {
          break;
        }
        line = nl + 1;
      }
      free(s);
    }
  }
  /* Try to populate devices from ubnt-discover (or fallback) using helper */
  {
    char *ud = NULL; size_t udn = 0;
    if (ubnt_discover_output(&ud, &udn) == 0 && ud && udn > 0) {
      char *normalized = NULL; size_t nlen = 0;
      if (normalize_ubnt_devices(ud, &normalized, &nlen) == 0 && normalized) {
        APPEND("\"devices\":");
        json_buf_append(&buf, &len, &cap, "%s", normalized);
        APPEND(",");
        free(normalized);
      } else {
        APPEND("\"devices\":[] ,");
      }
      free(ud);
    } else {
      APPEND("\"devices\":[] ,");
    }
  }

  /* links: either from olsrd API or empty array */
  if (olsr_links_raw && oln>0) {
    APPEND("\"links\":"); json_buf_append(&buf, &len, &cap, "%s", olsr_links_raw); APPEND(",");
  } else {
    APPEND("\"links\":[],");
  }
  APPEND("\"olsr2_on\":%s", olsr2_on?"true":"false");

  /* include pidof output for diagnostics when present */
  if (pout && pn>0) {
    APPEND(",\"olsrd_pidof\":"); json_append_escaped(&buf, &len, &cap, pout);
    free(pout); pout = NULL;
  }
  if (olsr_links_raw) { free(olsr_links_raw); olsr_links_raw = NULL; }

  /* diagnostics: report which local olsrd endpoints were probed and traceroute info */
  {
    APPEND(",\"diagnostics\":{");
    /* endpoints probed earlier (try same list) */
    const char *eps[] = { "http://127.0.0.1:9090/links", "http://127.0.0.1:2006/links", "http://127.0.0.1:8123/links", NULL };
    APPEND("\"olsrd_endpoints\":[");
    int first_ep = 1;
    for (const char **ep = eps; *ep; ++ep) {
      char cmd[256]; char *tout = NULL; size_t tlen = 0; snprintf(cmd, sizeof(cmd), "/usr/bin/curl -s --max-time 1 %s", *ep);
      int ok = (util_exec(cmd, &tout, &tlen) == 0 && tout && tlen>0) ? 1 : 0;
      if (!first_ep) APPEND(","); first_ep = 0;
      APPEND("{\"url\":"); json_append_escaped(&buf,&len,&cap,*ep);
      APPEND(",\"ok\":%s,\"len\":%zu,\"sample\":", ok?"true":"false", tlen);
      if (tout && tlen>0) {
        /* include a short sample */
        char sample[256]; size_t copy = tlen < sizeof(sample)-1 ? tlen : sizeof(sample)-1; memcpy(sample, tout, copy); sample[copy]=0; json_append_escaped(&buf,&len,&cap,sample);
      } else json_append_escaped(&buf,&len,&cap,"");
      APPEND("}"); if (tout) free(tout);
    }
    APPEND("]");
    APPEND(",\"traceroute\":{\"available\":%s,\"path\":", g_has_traceroute?"true":"false"); json_append_escaped(&buf,&len,&cap, g_traceroute_path);
    APPEND("}");
    APPEND("}");
  }

  /* include fixed traceroute-to-uplink output (mimic python behavior) */
  if (traceroute_to[0] && g_has_traceroute) {
    const char *trpath = (g_traceroute_path[0]) ? g_traceroute_path : "traceroute";
    size_t cmdlen = strlen(trpath) + strlen(traceroute_to) + 64;
    char *cmd = (char*)malloc(cmdlen);
    if (cmd) {
      snprintf(cmd, cmdlen, "%s -4 -w 1 -q 1 %s", trpath, traceroute_to);
      char *tout = NULL; size_t t_n = 0;
      if (util_exec(cmd, &tout, &t_n) == 0 && tout && t_n>0) {
        /* parse lines into simple objects */
        APPEND(",\"trace_target\":"); json_append_escaped(&buf,&len,&cap,traceroute_to);
        APPEND(",\"trace_to_uplink\":[");
        char *p = tout; char *line; int first = 1;
        while ((line = strsep(&p, "\n")) != NULL) {
          if (!line || !*line) continue;
          /* skip header line starting with 'traceroute to' */
          if (strstr(line, "traceroute to") == line) continue;
          /* tokenize */
          char *copy = strdup(line);
          char *tok = NULL; char *save = NULL; int idx = 0;
          char hop[16] = ""; char ip[64] = ""; char host[256] = ""; char ping[64] = "";
          tok = strtok_r(copy, " \t", &save);
          while (tok) {
            if (idx == 0) { snprintf(hop, sizeof(hop), "%s", tok); }
            else if (idx == 1) {
              /* could be hostname or ip */
              if (tok[0] == '(') {
                /* unexpected, skip */
              } else {
                /* store as ip for now */
                snprintf(ip, sizeof(ip), "%s", tok);
              }
            } else if (idx == 2) {
              /* maybe ip in parentheses or first ping */
              if (tok[0] == '(') {
                /* strip parentheses */
                char *endp = strchr(tok, ')');
                if (endp) { *endp = 0; snprintf(host, sizeof(host), "%s", tok+1); }
              } else if (!ping[0]) {
                snprintf(ping, sizeof(ping), "%s", tok);
              }
            } else {
              if (!ping[0] && strchr(tok, 'm')) { snprintf(ping, sizeof(ping), "%s", tok); }
            }
            idx++; tok = strtok_r(NULL, " \t", &save);
          }
          free(copy);
          if (!first) APPEND(","); first = 0;
          APPEND("{\"hop\":%s,\"ip\":", hop); json_append_escaped(&buf,&len,&cap, ip);
          APPEND(",\"host\":"); json_append_escaped(&buf,&len,&cap, host);
          APPEND(",\"ping\":"); json_append_escaped(&buf,&len,&cap, ping);
          APPEND("}");
        }
        APPEND("]");
        free(tout);
      }
      free(cmd);
    }
  }

  /* optionally include admin_url when running on EdgeRouter */
  if (g_is_edgerouter) {
    int admin_port = 443;
    char *cfg = NULL; size_t cn = 0;
    if (util_read_file("/config/config.boot", &cfg, &cn) == 0 && cfg && cn > 0) {
      const char *tok = strstr(cfg, "https-port");
      if (tok) {
        /* move past token and find first integer sequence */
        tok += strlen("https-port");
        while (*tok && !isdigit((unsigned char)*tok)) tok++;
        if (isdigit((unsigned char)*tok)) {
          char *endptr = NULL; long v = strtol(tok, &endptr, 10);
          if (v > 0 && v < 65536) admin_port = (int)v;
        }
      }
      free(cfg);
    }
    /* prefer default route ip if available, else hostname */
    if (def_ip[0] || hostname[0]) {
      const char *host_for_admin = def_ip[0] ? def_ip : hostname;
      size_t needed = strlen("https://") + strlen(host_for_admin) + 16;
      char *admin_url_buf = (char*)malloc(needed);
      if (admin_url_buf) {
        if (admin_port == 443) snprintf(admin_url_buf, needed, "https://%s/", host_for_admin);
        else snprintf(admin_url_buf, needed, "https://%s:%d/", host_for_admin, admin_port);
        APPEND(",\"admin_url\":"); json_append_escaped(&buf, &len, &cap, admin_url_buf);
        free(admin_url_buf);
      }
    }
  }
  APPEND("\n}\n");

  /* send and cleanup */
  http_send_status(r, 200, "OK");
  http_printf(r, "Content-Type: application/json; charset=utf-8\r\n\r\n");
  http_write(r, buf, len);
  free(buf);
  if (airos_raw) free(airos_raw);
  return 0;
}

static int h_nodedb(http_request_t *r) {
  /* Try local custom node db first, then /tmp, then remote fetch. */
  char *out = NULL; size_t n = 0;
  const char *local_custom = "/config/custom/node_db.json";
  const char *tmp_local = "/tmp/node_db.json";
  if (path_exists(local_custom)) {
    if (util_read_file(local_custom, &out, &n) == 0 && out && buffer_has_content(out, n)) {
      http_send_status(r, 200, "OK"); http_printf(r, "Content-Type: application/json; charset=utf-8\r\n\r\n"); http_write(r, out, n); free(out); return 0;
    }
    if (out) { free(out); out = NULL; n = 0; }
  }
  if (path_exists(tmp_local)) {
    if (util_read_file(tmp_local, &out, &n) == 0 && out && buffer_has_content(out, n)) {
      http_send_status(r, 200, "OK"); http_printf(r, "Content-Type: application/json; charset=utf-8\r\n\r\n"); http_write(r, out, n); free(out); return 0;
    }
    if (out) { free(out); out = NULL; n = 0; }
  }
  /* attempt remote fetch via curl */
  if (util_exec("/usr/bin/curl -s --max-time 1 https://ff.cybercomm.at/node_db.json", &out, &n) == 0 && out && buffer_has_content(out, n)) {
    http_send_status(r, 200, "OK"); http_printf(r, "Content-Type: application/json; charset=utf-8\r\n\r\n"); http_write(r, out, n); free(out); return 0;
  }
  if (out) { free(out); out = NULL; n = 0; }
  /* try arp-based fallback */
  if (devices_from_arp_json(&out, &n) == 0 && out && n>0) {
    http_send_status(r, 200, "OK"); http_printf(r, "Content-Type: application/json; charset=utf-8\r\n\r\n"); http_write(r, out, n); free(out); return 0;
  }
  send_json(r, "{}\n");
  return 0;
}

/* capabilities endpoint */
/* forward-declare globals used by capabilities endpoint (defined later) */
extern int g_is_edgerouter;
extern int g_has_ubnt_discover;
extern int g_has_traceroute;

/* capabilities endpoint */
static int h_capabilities_local(http_request_t *r) {
  int airos = path_exists("/tmp/10-all.json");
  int discover = g_has_ubnt_discover ? 1 : 0;
  int tracer = g_has_traceroute ? 1 : 0;
  /* also expose show_link_to_adminlogin if set in settings.inc */
  int show_admin = 0;
  {
    char *s = NULL; size_t sn = 0;
    if (util_read_file("/config/custom/www/settings.inc", &s, &sn) == 0 && s && sn>0) {
      /* find the line containing show_link_to_adminlogin and parse its value */
      const char *p = s; const char *end = s + sn;
      while (p && p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t linelen = nl ? (size_t)(nl - p) : (size_t)(end - p);
        if (linelen > 0 && memmem(p, linelen, "show_link_to_adminlogin", strlen("show_link_to_adminlogin"))) {
          const char *eq = memchr(p, '=', linelen);
          if (eq) {
            const char *v = eq + 1; size_t vlen = (size_t)(p + linelen - v);
            /* trim whitespace and quotes/semicolons */
            while (vlen && (v[vlen-1]=='\n' || v[vlen-1]=='\r' || v[vlen-1]==' ' || v[vlen-1]=='\'' || v[vlen-1]=='"' || v[vlen-1]==';')) vlen--;
            while (vlen && (*v==' ' || *v=='\'' || *v=='"')) { v++; vlen--; }
            if (vlen > 0) {
              char tmp[32]={0}; size_t copy = vlen < sizeof(tmp)-1 ? vlen : sizeof(tmp)-1; memcpy(tmp, v, copy); tmp[copy]=0;
              if (atoi(tmp) != 0) show_admin = 1;
            }
          }
        }
  if (!nl) break;
  p = nl + 1;
      }
      free(s);
    }
  }
  char buf[320]; snprintf(buf, sizeof(buf), "{\"is_edgerouter\":%s,\"discover\":%s,\"airos\":%s,\"connections\":true,\"traffic\":%s,\"txtinfo\":true,\"jsoninfo\":true,\"traceroute\":%s,\"show_admin_link\":%s}",
    g_is_edgerouter?"true":"false", discover?"true":"false", airos?"true":"false", path_exists("/tmp")?"true":"false", tracer?"true":"false", show_admin?"true":"false");
  send_json(r, buf);
  return 0;
}
#include "httpd.h"
int g_is_edgerouter = 0;
int g_has_ubnt_discover = 0;
int g_has_traceroute = 0;
/* PATHLEN not provided by host headers in this build context; use 512 as a safe default. */
#ifndef PATHLEN
#define PATHLEN 512
#endif
char g_ubnt_discover_path[PATHLEN] = "";
char g_traceroute_path[PATHLEN] = "";
char g_olsrd_path[PATHLEN] = "";
#include "util.h"
#include "olsrd_plugin.h"
int env_is_edgerouter(void);

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

static char   g_bind[64] = "0.0.0.0";
static int    g_port = 11080;
static int    g_enable_ipv6 = 0;
static char   g_asset_root[512] = "/usr/share/olsrd-status-plugin/www";

static int h_root(http_request_t *r);
static int h_ipv4(http_request_t *r);
static int h_ipv6(http_request_t *r);
static int h_txtinfo(http_request_t *r);
static int h_jsoninfo(http_request_t *r);
static int h_olsrd(http_request_t *r);
static int h_discover(http_request_t *r);
static int h_connections(http_request_t *r);
static int h_airos(http_request_t *r);
static int h_traffic(http_request_t *r);

/* embedded index.html removed; assets are served from g_asset_root/www (see www/index.html) */

/* Stubs for JSON/async handlers referenced during http server registration. */
/* connections.json: return JSON produced by render_connections_json */
static int h_connections_json(http_request_t *r) {
  char *out = NULL; size_t n = 0;
  if (render_connections_json(&out, &n) == 0 && out) {
    http_send_status(r, 200, "OK");
    http_printf(r, "Content-Type: application/json; charset=utf-8\r\n\r\n");
    http_write(r, out, n);
    free(out);
    return 0;
  }
  send_json(r, "{}\n");
  return 0;
}

/* versions.json: run versions.sh (custom or packaged) and return JSON output */
static int h_versions_json(http_request_t *r) {
  char *out = NULL; size_t n = 0;
  const char *custom = "/config/custom/versions.sh";
  const char *packaged = "/usr/share/olsrd-status-plugin/versions.sh";
  if (path_exists(custom)) {
    char cmd[512]; snprintf(cmd, sizeof(cmd), "%s", custom);
  if (util_exec(cmd, &out, &n) == 0 && out && buffer_has_content(out, n)) {
      http_send_status(r, 200, "OK");
      http_printf(r, "Content-Type: application/json; charset=utf-8\r\n\r\n");
      http_write(r, out, n); free(out); return 0;
    }
  if (out) { free(out); out = NULL; n = 0; }
  }
  if (path_exists(packaged)) {
    char cmd[512]; snprintf(cmd, sizeof(cmd), "%s", packaged);
  if (util_exec(cmd, &out, &n) == 0 && out && buffer_has_content(out, n)) {
      http_send_status(r, 200, "OK");
      http_printf(r, "Content-Type: application/json; charset=utf-8\r\n\r\n");
      http_write(r, out, n); free(out); return 0;
    }
  if (out) { free(out); out = NULL; n = 0; }
  }
  /* fallback: run versions.sh from repo root if present (development) */
  if (path_exists("./versions.sh")) {
  if (util_exec("./versions.sh", &out, &n) == 0 && out && buffer_has_content(out, n)) {
      http_send_status(r, 200, "OK");
      http_printf(r, "Content-Type: application/json; charset=utf-8\r\n\r\n");
      http_write(r, out, n); free(out); return 0;
    }
  if (out) { free(out); out = NULL; n = 0; }
  }
  send_json(r, "{}\n");
  return 0;
}

/* traceroute: run traceroute binary if available and return stdout as plain text */
static int h_traceroute(http_request_t *r) {
  char target[256] = "";
  (void)get_query_param(r, "target", target, sizeof(target));
  if (!target[0]) { send_text(r, "No target provided\n"); return 0; }
  if (!g_has_traceroute || !g_traceroute_path[0]) { send_text(r, "traceroute not available\n"); return 0; }
  /* build command dynamically to avoid compile-time truncation warnings */
  size_t cmdlen = strlen(g_traceroute_path) + 4 + strlen(target) + 32;
  char *cmd = (char*)malloc(cmdlen);
  if (!cmd) { send_text(r, "error allocating memory\n"); return 0; }
  snprintf(cmd, cmdlen, "%s -n %s 2>&1", g_traceroute_path, target);
  char *out = NULL; size_t n = 0;
  if (util_exec(cmd, &out, &n) == 0 && out) {
    http_send_status(r, 200, "OK");
    http_printf(r, "Content-Type: text/plain; charset=utf-8\r\n\r\n");
    http_write(r, out, n);
    free(out);
    free(cmd);
    return 0;
  }
  free(cmd);
  /* try ICMP-based traceroute as a fallback */
  {
    size_t cmdlen2 = strlen(g_traceroute_path) + 8 + strlen(target) + 32;
    char *cmd2 = malloc(cmdlen2);
    if (cmd2) {
      snprintf(cmd2, cmdlen2, "%s -I -n %s 2>&1", g_traceroute_path, target);
      char *out2 = NULL; size_t n2 = 0;
      if (util_exec(cmd2, &out2, &n2) == 0 && out2) {
        http_send_status(r, 200, "OK");
        http_printf(r, "Content-Type: text/plain; charset=utf-8\r\n\r\n");
        http_write(r, out2, n2);
        free(out2);
        free(cmd2);
        return 0;
      }
      free(cmd2);
    }
  }
  send_text(r, "error running traceroute\n");
  return 0;
}



static int h_embedded_appjs(http_request_t *r) {
  return http_send_file(r, g_asset_root, "js/app.js", NULL);
}

static int h_emb_jquery(http_request_t *r) {
  return http_send_file(r, g_asset_root, "js/jquery.min.js", NULL);
}

static int h_emb_bootstrap(http_request_t *r) {
  return http_send_file(r, g_asset_root, "js/bootstrap.min.js", NULL);
}

/* check asset files under g_asset_root and log their existence and permissions */
static void log_asset_permissions(void) {
  const char *rel_files[] = { };
  char path[1024];
  struct stat st;
  /* check root */
  if (stat(g_asset_root, &st) == 0 && S_ISDIR(st.st_mode)) {
    fprintf(stderr, "[status-plugin] asset root: %s (mode %o, uid=%d, gid=%d)\n", g_asset_root, (int)(st.st_mode & 07777), (int)st.st_uid, (int)st.st_gid);
  } else {
    fprintf(stderr, "[status-plugin] asset root missing or not a directory: %s\n", g_asset_root);
  }
  size_t num_files = sizeof(rel_files)/sizeof(rel_files[0]);
  for (size_t i = 0; i < num_files; i++) {
    snprintf(path, sizeof(path), "%s/%s", g_asset_root, rel_files[i]);
    if (stat(path, &st) == 0) {
      int ok_r = access(path, R_OK) == 0;
      int ok_x = access(path, X_OK) == 0;
      fprintf(stderr, "[status-plugin] asset: %s exists (mode %o, uid=%d, gid=%d) readable=%s executable=%s\n",
        path, (int)(st.st_mode & 07777), (int)st.st_uid, (int)st.st_gid, ok_r?"yes":"no", ok_x?"yes":"no");
    } else {
      fprintf(stderr, "[status-plugin] asset: %s MISSING\n", path);
    }
  }
}

/* plugin lifecycle prototype to match olsrd expectations */
void olsrd_plugin_exit(void);

static void send_text(http_request_t *r, const char *text) {
  http_send_status(r, 200, "OK");
  http_printf(r, "Content-Type: text/plain; charset=utf-8\r\n\r\n");
  http_write(r, text, strlen(text));
}
static void send_json(http_request_t *r, const char *json) {
  http_send_status(r, 200, "OK");
  http_printf(r, "Content-Type: application/json; charset=utf-8\r\n\r\n");
  http_write(r, json, strlen(json));
}

int olsrd_plugin_interface_version(void) {
  return 5;
}

static int set_str_param(const char *value, void *data, set_plugin_parameter_addon addon __attribute__((unused))) {
  if (!value || !data) return 1;
  snprintf((char*)data, 511, "%s", value);
  return 0;
}
static int set_int_param(const char *value, void *data, set_plugin_parameter_addon addon __attribute__((unused))) {
  if (!value || !data) return 1;
  *(int*)data = atoi(value);
  return 0;
}

static const struct olsrd_plugin_parameters g_params[] = {
  { .name = "bind",       .set_plugin_parameter = &set_str_param, .data = g_bind,        .addon = {0} },
  { .name = "port",       .set_plugin_parameter = &set_int_param, .data = &g_port,       .addon = {0} },
  { .name = "enableipv6", .set_plugin_parameter = &set_int_param, .data = &g_enable_ipv6,.addon = {0} },
  { .name = "assetroot",  .set_plugin_parameter = &set_str_param, .data = g_asset_root,  .addon = {0} },
};

void olsrd_get_plugin_parameters(const struct olsrd_plugin_parameters **params, int *size) {
  *params = g_params;
  *size = (int)(sizeof(g_params)/sizeof(g_params[0]));
}

int olsrd_plugin_init(void) {
  log_asset_permissions();
  /* detect availability of optional external tools without failing startup */
  const char *ubnt_candidates[] = { "/usr/sbin/ubnt-discover", "/sbin/ubnt-discover", "/usr/bin/ubnt-discover", "/usr/local/sbin/ubnt-discover", "/usr/local/bin/ubnt-discover", "/opt/ubnt/ubnt-discover", NULL };
  const char *tracer_candidates[] = { "/usr/sbin/traceroute", "/bin/traceroute", "/usr/bin/traceroute", "/usr/local/bin/traceroute", NULL };
  const char *olsrd_candidates[] = { "/usr/sbin/olsrd", "/usr/bin/olsrd", "/sbin/olsrd", NULL };
  for (const char **p = ubnt_candidates; *p; ++p) { if (path_exists(*p)) { g_has_ubnt_discover = 1; snprintf(g_ubnt_discover_path, sizeof(g_ubnt_discover_path), "%s", *p); break; } }
  for (const char **p = tracer_candidates; *p; ++p) { if (path_exists(*p)) { g_has_traceroute = 1; snprintf(g_traceroute_path, sizeof(g_traceroute_path), "%s", *p); break; } }
  for (const char **p = olsrd_candidates; *p; ++p) { if (path_exists(*p)) { snprintf(g_olsrd_path, sizeof(g_olsrd_path), "%s", *p); break; } }
  g_is_edgerouter = env_is_edgerouter();

  if (http_server_start(g_bind, g_port, g_asset_root) != 0) {
    fprintf(stderr, "[status-plugin] failed to start http server on %s:%d\n", g_bind, g_port);
    return 1;
  }
  http_server_register_handler("/",         &h_root);
  http_server_register_handler("/ipv4",     &h_ipv4);
  http_server_register_handler("/ipv6",     &h_ipv6);
  http_server_register_handler("/status",   &h_status);
  http_server_register_handler("/capabilities", &h_capabilities_local);
  http_server_register_handler("/txtinfo",  &h_txtinfo);
  http_server_register_handler("/jsoninfo", &h_jsoninfo);
  http_server_register_handler("/olsrd",    &h_olsrd);
  http_server_register_handler("/olsr2",    &h_olsrd);
  http_server_register_handler("/discover", &h_discover);
  http_server_register_handler("/js/app.js", &h_embedded_appjs);
  http_server_register_handler("/js/jquery.min.js", &h_emb_jquery);
  http_server_register_handler("/js/bootstrap.min.js", &h_emb_bootstrap);
  http_server_register_handler("/connections",&h_connections);
  http_server_register_handler("/connections.json", &h_connections_json);
  http_server_register_handler("/airos",    &h_airos);
  http_server_register_handler("/traffic",  &h_traffic);
  http_server_register_handler("/versions.json", &h_versions_json);
  http_server_register_handler("/nodedb.json", &h_nodedb);
  http_server_register_handler("/traceroute", &h_traceroute);
  fprintf(stderr, "[status-plugin] listening on %s:%d (assets: %s)\n", g_bind, g_port, g_asset_root);
  return 0;
}

void olsrd_plugin_exit(void) {
  http_server_stop();
}

static int get_query_param(http_request_t *r, const char *key, char *out, size_t outlen) {
  if (!r->query[0]) return 0;
  char q[512]; snprintf(q, sizeof(q), "%s", r->query);
  char *tok = strtok(q, "&");
  while (tok) {
    char *eq = strchr(tok, '=');
    if (eq) {
      *eq = 0;
      if (strcmp(tok, key) == 0) {
        snprintf(out, outlen, "%s", eq+1);
        return 1;
      }
    } else {
      if (strcmp(tok, key) == 0) { out[0]=0; return 1; }
    }
    tok = strtok(NULL, "&");
  }
  return 0;
}

static int h_embedded_index(http_request_t *r) {
  return http_send_file(r, g_asset_root, "index.html", NULL);
}

static int h_root(http_request_t *r) {
  /* Serve index.html from asset root (www/index.html) */
  return h_embedded_index(r);
}

static int h_ipv4(http_request_t *r) {
  char *out=NULL; size_t n=0;
  const char *cmd = "/sbin/ip -4 a && echo && /sbin/ip -4 neigh && echo && /usr/sbin/brctl show";
  if (util_exec(cmd, &out, &n)==0 && out) {
    http_send_status(r, 200, "OK");
    http_printf(r, "Content-Type: text/plain; charset=utf-8\r\n\r\n");
    http_write(r, out, n);
    free(out);
  } else send_text(r, "error\n");
  return 0;
}
static int h_ipv6(http_request_t *r) {
  char *out=NULL; size_t n=0;
  const char *cmd = "/sbin/ip -6 a && echo && /sbin/ip -6 neigh && echo && /usr/sbin/brctl show";
  if (util_exec(cmd, &out, &n)==0 && out) {
    http_send_status(r, 200, "OK");
    http_printf(r, "Content-Type: text/plain; charset=utf-8\r\n\r\n");
    http_write(r, out, n);
    free(out);
  } else send_text(r, "error\n");
  return 0;
}

static int h_txtinfo(http_request_t *r) {
  char q[64]="ver";
  (void)get_query_param(r, "q", q, sizeof(q));
  char cmd[256];
  snprintf(cmd, sizeof(cmd), "/usr/bin/curl -s --max-time 1 http://127.0.0.1:2006/%s", q);
  char *out=NULL; size_t n=0;
  if (util_exec(cmd, &out, &n)==0 && out) {
    http_send_status(r, 200, "OK");
    http_printf(r, "Content-Type: text/plain; charset=utf-8\r\n\r\n");
    http_write(r, out, n); free(out);
  } else send_text(r, "error\n");
  return 0;
}
static int h_jsoninfo(http_request_t *r) {
  char q[64]="version";
  (void)get_query_param(r, "q", q, sizeof(q));
  char cmd[256];
  snprintf(cmd, sizeof(cmd), "/usr/bin/curl -s --max-time 1 http://127.0.0.1:9090/%s", q);
  char *out=NULL; size_t n=0;
  if (util_exec(cmd, &out, &n)==0 && out) {
    send_json(r, (n?out:"{}")); free(out);
  } else send_json(r, "{\"error\":\"curl failed\"}");
  return 0;
}

static int h_olsrd(http_request_t *r) {
  char outbuf[2048]; size_t off = 0;
  /* hostname */
  char hn[256] = "unknown"; if (gethostname(hn, sizeof(hn))==0) hn[sizeof(hn)-1]=0;
  off += snprintf(outbuf+off, sizeof(outbuf)-off, "%s\n", hn);

  /* olsrd pid file change time: stat /proc/<pid>/stat ctime if pid exists */
  char *pout = NULL; size_t pn = 0;
  if (util_exec("pidof olsrd", &pout, &pn)==0 && pout && pn>0) {
    /* take first token as pid */
    char pid[32] = {0}; size_t i=0; while (i<pn && i<sizeof(pid)-1 && pout[i] && pout[i]!=' ' && pout[i]!='\n') { pid[i]=pout[i]; i++; }
    if (pid[0]) {
      char statpath[64]; snprintf(statpath, sizeof(statpath), "/proc/%s/stat", pid);
      struct stat st; if (stat(statpath, &st)==0) {
        time_t s = st.st_mtime; time_t d = time(NULL);
        off += snprintf(outbuf+off, sizeof(outbuf)-off, "%ld\n%ld\n%ld\n", (long)s, (long)(d - s), (long)d);
      } else {
        off += snprintf(outbuf+off, sizeof(outbuf)-off, "\n\n\n");
      }
    } else {
      off += snprintf(outbuf+off, sizeof(outbuf)-off, "\n\n\n");
    }
    free(pout); pout = NULL; pn = 0;
  } else {
    off += snprintf(outbuf+off, sizeof(outbuf)-off, "\n\n\n");
  }

  /* uptime first-token and difference */
  char *ub = NULL; size_t un=0;
  if (util_read_file("/proc/uptime", &ub, &un)==0 && ub && un>0) {
    double up = atof(ub); long su = (long)up; long d = time(NULL);
    off += snprintf(outbuf+off, sizeof(outbuf)-off, "%ld\n%ld\n", (long)(d - su), su);
    free(ub); ub = NULL;
  } else {
    off += snprintf(outbuf+off, sizeof(outbuf)-off, "\n\n");
  }

  /* binary/version parsing: run grep to extract a chunk, replace control chars with '~' and split by '~' to mimic original awk parsing */
  char *sout = NULL; size_t sn=0;
  if (util_exec("grep -oaEm1 'olsr.org - .{185}' /usr/sbin/olsrd 2>/dev/null", &sout, &sn)==0 && sout && sn>0) {
    /* replace selected control chars with '~' to create separators similar to the original sed call */
    for (size_t i=0;i<sn;i++) {
      unsigned char c = (unsigned char)sout[i];
    /* c is unsigned char; check non-printable control ranges explicitly */
    if (c <= 0x08 || c == 0x0B || c == 0x0C || (c >= 0x0E && c <= 0x1F)) sout[i] = '~';
      if (sout[i]=='\n' || sout[i]=='\r') sout[i]='~';
    }
    /* split by '~' and pick fields matching original awk -F~ '{print $1,$3,$5,$6,$9,$12}' */
    char *parts[16]; int pc = 0;
    char *p = sout; char *tok;
    while (pc < 16 && (tok = strsep(&p, "~")) != NULL) {
      parts[pc++] = tok;
    }
    const char *ver = (pc>0 && parts[0] && parts[0][0]) ? parts[0] : "";
    const char *dsc = (pc>2 && parts[2] && parts[2][0]) ? parts[2] : "";
    const char *dev = (pc>4 && parts[4] && parts[4][0]) ? parts[4] : "";
    const char *dat = (pc>5 && parts[5] && parts[5][0]) ? parts[5] : "";
    const char *rel = (pc>8 && parts[8] && parts[8][0]) ? parts[8] : "";
    const char *src = (pc>11 && parts[11] && parts[11][0]) ? parts[11] : "";
    /* trim leading/trailing spaces from these fields */
    char tver[256]={0}, tdsc[256]={0}, tdev[256]={0}, tdat[256]={0}, trel[256]={0}, tsrc[256]={0};
    if (ver) { snprintf(tver,sizeof(tver),"%s", ver); }
    if (dsc) { snprintf(tdsc,sizeof(tdsc),"%s", dsc); }
    if (dev) { snprintf(tdev,sizeof(tdev),"%s", dev); }
    if (dat) { snprintf(tdat,sizeof(tdat),"%s", dat); }
    if (rel) { snprintf(trel,sizeof(trel),"%s", rel); }
    if (src) { snprintf(tsrc,sizeof(tsrc),"%s", src); }
    /* remove surrounding spaces/newlines */
    for(char *s=tver;*s;s++) if(*s=='\n' || *s=='\r') *s=' ';
    for(char *s=tdsc;*s;s++) if(*s=='\n' || *s=='\r') *s=' ';
    for(char *s=tdev;*s;s++) if(*s=='\n' || *s=='\r') *s=' ';
    for(char *s=tdat;*s;s++) if(*s=='\n' || *s=='\r') *s=' ';
    for(char *s=trel;*s;s++) if(*s=='\n' || *s=='\r') *s=' ';
    for(char *s=tsrc;*s;s++) if(*s=='\n' || *s=='\r') *s=' ';
    off += snprintf(outbuf+off, sizeof(outbuf)-off, "ver:%s\ndsc:%s\ndev:%s\ndat:%s\nrel:%s\nsrc:%s\n", tver, tdsc, tdev, tdat, trel, tsrc);
    free(sout); sout = NULL;
  } else {
    off += snprintf(outbuf+off, sizeof(outbuf)-off, "ver:\ndsc:\ndev:\ndat:\nrel:\nsrc:\n");
  }

  /* append /root.dev/version second field if available */
  char *rv = NULL; size_t rvn=0;
  if (util_read_file("/root.dev/version", &rv, &rvn)==0 && rv && rvn>0) {
    /* extract second whitespace token from file */
    char tmpv[256] = {0}; size_t L = rvn < sizeof(tmpv)-1 ? rvn : sizeof(tmpv)-1; memcpy(tmpv, rv, L); tmpv[L]=0;
    char *t1 = strtok(tmpv, " \t\n");
    char *t2 = t1 ? strtok(NULL, " \t\n") : NULL;
    if (t2) off += snprintf(outbuf+off, sizeof(outbuf)-off, "%s\n", t2);
    free(rv);
  }

  http_send_status(r, 200, "OK");
  http_printf(r, "Content-Type: text/plain; charset=utf-8\r\n\r\n");
  http_write(r, outbuf, off);
  return 0;
}

static int h_discover(http_request_t *r) {
  char ifs[256]="";
  (void)get_query_param(r, "q", ifs, sizeof(ifs));
  char cmd[1024];
  if (!g_has_ubnt_discover || !g_ubnt_discover_path[0]) { send_json(r, "{}"); return 0; }
  if (ifs[0]) snprintf(cmd, sizeof(cmd), "%s -d 900 -V -i %s -j", g_ubnt_discover_path, ifs);
  else snprintf(cmd, sizeof(cmd), "%s -d 150 -V -j", g_ubnt_discover_path);
  char *out=NULL; size_t n=0;
  if (util_exec(cmd, &out, &n)==0 && out && n>0) { send_json(r, out); free(out); }
  else { send_json(r, "{}"); }
  return 0;
}

static int h_connections(http_request_t *r) {
  char *out = NULL; size_t n = 0;
  // Prefer the native renderer implemented in connections.c
  if (render_connections_plain(&out, &n) == 0 && out && n > 0) {
    http_send_status(r, 200, "OK");
    http_printf(r, "Content-Type: text/plain; charset=utf-8\r\n\r\n");
    http_write(r, out, n);
    free(out);
    return 0;
  }

  // Fallback to custom external script for compatibility
  if (util_exec("/config/custom/connections.sh", &out, &n) == 0 && out && n > 0) {
    http_send_status(r, 200, "OK");
    http_printf(r, "Content-Type: text/plain; charset=utf-8\r\n\r\n");
    http_write(r, out, n);
    free(out);
    return 0;
  }

  // Nothing available
  send_text(r, "n/a\n");
  return 0;
}

static int h_airos(http_request_t *r) {
  char *out=NULL; size_t n=0;
  if (util_read_file("/tmp/10-all.json", &out, &n)==0 && out) {
    http_send_status(r, 200, "OK");
    http_printf(r, "Content-Type: application/json; charset=utf-8\r\n\r\n");
    http_write(r, out, n); free(out);
  } else send_json(r, "{}");
  return 0;
}

static int h_traffic(http_request_t *r) {
  char *out=NULL; size_t n=0;
  if (util_exec("ls -1 /tmp/traffic-*.dat 2>/dev/null | xargs -r -I{} sh -c 'echo ### {}; cat {}'", &out, &n)==0 && out) {
    http_send_status(r, 200, "OK");
    http_printf(r, "Content-Type: text/plain; charset=utf-8\r\n\r\n");
    http_write(r, out, n); free(out);
  } else send_text(r, "[]\n");
  return 0;
}

/* forward declarations used before including headers later in this file */
int path_exists(const char *p);
extern int g_is_edgerouter;
