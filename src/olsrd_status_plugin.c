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
#include <time.h>
#include <sys/time.h>
#include <pthread.h>

#include "httpd.h"
#include "util.h"
#include "olsrd_plugin.h"
#include "ubnt_discover.h"

#include <stdarg.h>

/* Global configuration/state (single authoritative definitions) */
int g_is_edgerouter = 0;
int g_has_traceroute = 0;
int g_is_linux_container = 0;

static char   g_bind[64] = "0.0.0.0";
static int    g_port = 11080;
static int    g_enable_ipv6 = 0;
static char   g_asset_root[512] = "/usr/share/olsrd-status-plugin/www";

/* Node DB remote auto-update cache */
static char   g_nodedb_url[512] = "https://ff.cybercomm.at/node_db.json"; /* override via plugin param nodedb_url */
static int    g_nodedb_ttl = 300; /* seconds */
static time_t g_nodedb_last_fetch = 0; /* epoch of last successful fetch */
static char  *g_nodedb_cached = NULL; /* malloc'ed JSON blob */
static size_t g_nodedb_cached_len = 0;
static pthread_mutex_t g_nodedb_lock = PTHREAD_MUTEX_INITIALIZER;

/* Paths to optional external tools (detected at init) */
#ifndef PATHLEN
#define PATHLEN 512
#endif
char g_traceroute_path[PATHLEN] = "";
char g_olsrd_path[PATHLEN] = "";

/* Forward declarations for HTTP handlers */
static int h_root(http_request_t *r);
static int h_ipv4(http_request_t *r); static int h_ipv6(http_request_t *r);
static int h_status(http_request_t *r); static int h_status_summary(http_request_t *r); static int h_status_olsr(http_request_t *r); static int h_status_lite(http_request_t *r);
static int h_olsr_links(http_request_t *r); static int h_olsr_routes(http_request_t *r); static int h_olsr_raw(http_request_t *r);
static int h_olsr_links_debug(http_request_t *r);
static int h_olsrd_json(http_request_t *r); static int h_capabilities_local(http_request_t *r);
static int h_txtinfo(http_request_t *r); static int h_jsoninfo(http_request_t *r); static int h_olsrd(http_request_t *r);
static int h_discover(http_request_t *r); static int h_embedded_appjs(http_request_t *r); static int h_emb_jquery(http_request_t *r); static int h_emb_bootstrap(http_request_t *r);
static int h_connections(http_request_t *r); static int h_connections_json(http_request_t *r);
static int h_airos(http_request_t *r); static int h_traffic(http_request_t *r); static int h_versions_json(http_request_t *r); static int h_nodedb(http_request_t *r);
static int h_traceroute(http_request_t *r);

/* helper: return 1 if buffer contains any non-whitespace byte */
static int buffer_has_content(const char *b, size_t n) {
  if (!b || n == 0) return 0;
  for (size_t i = 0; i < n; i++) if (!isspace((unsigned char)b[i])) return 1;
  return 0;
}

/* Format uptime in a human-friendly short form to match bmk-webstatus.py semantics.
 * Examples: "30sek", "5min", "3h", "2d"
 */
static void format_duration(long s, char *out, size_t outlen) {
  if (!out || outlen == 0) return;
  if (s < 0) s = 0;
  /* Preserve short form (legacy) when very small, else humanize similar to uptime's DDd HH:MM */
  if (s < 60) { snprintf(out, outlen, "%lds", s); return; }
  long days = s / 86400; long rem = s % 86400; long hrs = rem / 3600; rem %= 3600; long mins = rem / 60;
  if (days > 0) snprintf(out, outlen, "%ldd %02ld:%02ldh", days, hrs, mins);
  else if (hrs > 0) snprintf(out, outlen, "%ld:%02ldh", hrs, mins);
  else snprintf(out, outlen, "%ldmin", mins);
}

/* Produce a Linux uptime(1)-like line with load averages: "up 2 days, 03:14, load average: 0.15, 0.08, 0.01" */
static void format_uptime_linux(long seconds, char *out, size_t outlen) {
  if (!out || outlen==0) return;
  if (seconds < 0) seconds = 0;
  long days = seconds / 86400;
  long hrs  = (seconds / 3600) % 24;
  long mins = (seconds / 60) % 60;
  char dur[128]; dur[0]=0;
  if (days > 0) {
    /* match classic uptime style: up X days, HH:MM */
    snprintf(dur, sizeof(dur), "up %ld day%s, %02ld:%02ld", days, days==1?"":"s", hrs, mins);
  } else if (hrs > 0) {
    snprintf(dur, sizeof(dur), "up %ld:%02ld", hrs, mins);
  } else {
    snprintf(dur, sizeof(dur), "up %ld min", mins);
  }
  double loads[3] = {0,0,0};
#if defined(__linux__)
  FILE *lf = fopen("/proc/loadavg", "r");
  if (lf) {
    if (fscanf(lf, "%lf %lf %lf", &loads[0], &loads[1], &loads[2]) != 3) { loads[0]=loads[1]=loads[2]=0; }
    fclose(lf);
  }
#endif
  snprintf(out, outlen, "%s, load average: %.2f, %.2f, %.2f", dur, loads[0], loads[1], loads[2]);
}

/* Robust uptime seconds helper: /proc/uptime -> sysinfo() -> /proc/stat btime */
static long get_uptime_seconds(void) {
  long up = 0;
  char *upt = NULL; size_t un = 0;
  if (util_read_file("/proc/uptime", &upt, &un) == 0 && upt && un>0) {
    /* first token */
    up = (long)atof(upt);
  }
  if (upt) { free(upt); upt=NULL; }
#if defined(__linux__)
  if (up <= 0) {
    struct sysinfo si; if (sysinfo(&si) == 0) up = (long)si.uptime;
  }
#endif
  /* Fallback: parse btime from /proc/stat */
  if (up <= 0) {
    char *statc=NULL; size_t sn=0; if (util_read_file("/proc/stat", &statc, &sn)==0 && statc){
      char *line = statc; char *end = statc + sn;
      while (line < end) {
        char *nl = memchr(line,'\n',(size_t)(end-line));
        size_t ll = nl ? (size_t)(nl-line) : (size_t)(end-line);
        if (ll > 6 && memcmp(line,"btime ",6)==0) {
          long btime = atol(line+6);
          if (btime > 0) {
            time_t now = time(NULL);
            if (now > btime) up = (long)(now - btime);
          }
          break;
        }
        if (!nl) break;
        line = nl + 1;
      }
      free(statc);
    }
  }
  if (up < 0) up = 0;
  return up;
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

/* --- Helper counters for OLSR link enrichment --- */
static int find_json_string_value(const char *start, const char *key, char **val, size_t *val_len); /* forward */
static int find_best_nodename_in_nodedb(const char *buf, size_t len, const char *dest_ip, char *out_name, size_t out_len); /* forward */
static int count_routes_for_ip(const char *section, const char *ip) {
  if (!section || !ip || !ip[0]) return 0;
  const char *arr = strchr(section,'[');
  if (!arr) return 0;
  const char *p = arr;
  int depth = 0;
  int cnt = 0;
  while (*p) {
    if (*p == '[') { depth++; p++; continue; }
    if (*p == ']') { depth--; if (depth==0) break; p++; continue; }
    if (*p == '{') {
      const char *obj = p; int od = 1; p++;
      while (*p && od>0) { if (*p=='{') od++; else if (*p=='}') od--; p++; }
      const char *end = p;
      if (end>obj) {
        char *v; size_t vlen; char gw[64] = "";
        if (find_json_string_value(obj,"gateway",&v,&vlen) ||
            find_json_string_value(obj,"via",&v,&vlen) ||
            find_json_string_value(obj,"gatewayIP",&v,&vlen) ||
            find_json_string_value(obj,"nextHop",&v,&vlen) ||
            find_json_string_value(obj,"nexthop",&v,&vlen)) {
          snprintf(gw,sizeof(gw),"%.*s",(int)vlen,v);
        }
        /* strip /mask if present */
        if (gw[0]) { char *slash=strchr(gw,'/'); if(slash) *slash=0; }
        if (gw[0] && strcmp(gw,ip)==0) cnt++;
      }
      continue;
    }
    p++;
  }
  /* Legacy fallback: routes represented as array of plain strings without gateway field.
     Format examples: "193.238.158.38  1" or "78.41.112.141  5".
     We approximate "routes via ip" by counting how many destination strings START with the neighbor IP.
     This is heuristic (destination != gateway) but better than always zero. */
  if (cnt == 0) {
    const char *routes_key = strstr(section, "\"routes\"");
    if (routes_key) {
      const char *sa = strchr(routes_key,'[');
      if (sa) {
        const char *q = sa; int d = 0; int in_str = 0; const char *str_start = NULL;
        while (*q) {
          char c = *q;
          if (!in_str) {
            if (c == '[') { d++; }
            else if (c == ']') { d--; if (d==0) break; }
            else if (c == '"') { in_str = 1; str_start = q+1; }
          } else { /* inside string */
            if (c == '"') {
              /* end of string */
              size_t slen = (size_t)(q - str_start);
              if (slen >= strlen(ip)) {
                if (strncmp(str_start, ip, strlen(ip)) == 0) {
                  /* ensure next char is space or end => begins with ip */
                  if (slen == strlen(ip) || str_start[strlen(ip)]==' ' || str_start[strlen(ip)]=='\t') cnt++;
                }
              }
              in_str = 0; str_start = NULL;
            }
          }
          q++;
        }
      }
    }
  }
  /* Last-resort fallback: simple pattern scan for '"gateway":"<ip>' inside the provided section.
     This catches cases where JSON structure variations prevented earlier logic from matching (e.g. nested objects,
     slight field name changes, or concatenated JSON documents without separators). */
  if (cnt == 0) {
    char pattern[96]; snprintf(pattern,sizeof(pattern),"\"gateway\":\"%s", ip);
    const char *scan = section; int safety = 0;
    while ((scan = strstr(scan, pattern)) && safety < 100000) { cnt++; scan += strlen(pattern); safety++; }
    if (cnt == 0) {
      /* also try common alt key nextHop */
      snprintf(pattern,sizeof(pattern),"\"nextHop\":\"%s", ip);
      scan = section; safety = 0;
      while ((scan = strstr(scan, pattern)) && safety < 100000) { cnt++; scan += strlen(pattern); safety++; }
    }
  }
  /* Optional debug: enable by exporting OLSR_DEBUG_LINK_COUNTS=1 in environment. */
  if (cnt == 0) {
    const char *dbg = getenv("OLSR_DEBUG_LINK_COUNTS");
    if (dbg && *dbg=='1') {
      fprintf(stderr, "[status-plugin][debug] route count fallback still zero for ip=%s (section head=%.40s)\n", ip, section);
    }
  }
  return cnt;
}
static int count_nodes_for_ip(const char *section, const char *ip) {
  if (!section || !ip || !ip[0]) return 0;
  const char *arr = strchr(section,'[');
  if (!arr) return 0;
  const char *p = arr;
  int depth = 0;
  int cnt = 0;
  while (*p) {
    if (*p == '[') { depth++; p++; continue; }
    if (*p == ']') { depth--; if (depth==0) break; p++; continue; }
    if (*p == '{') {
      const char *obj = p; int od = 1; p++;
      while (*p && od>0) { if (*p=='{') od++; else if (*p=='}') od--; p++; }
      const char *end = p;
      if (end>obj) {
        char *v; size_t vlen; char lh[128] = "";
        /* Accept several possible key spellings – different olsrd/olsrd2 builds expose different field names */
    if (find_json_string_value(obj,"lastHopIP",&v,&vlen) ||
      find_json_string_value(obj,"lastHopIp",&v,&vlen) ||
      find_json_string_value(obj,"lastHopIpAddress",&v,&vlen) ||
      find_json_string_value(obj,"lastHop",&v,&vlen) ||
      find_json_string_value(obj,"via",&v,&vlen) ||
      find_json_string_value(obj,"gateway",&v,&vlen) ||
      find_json_string_value(obj,"gatewayIP",&v,&vlen) ||
      find_json_string_value(obj,"nextHop",&v,&vlen)) {
          snprintf(lh,sizeof(lh),"%.*s",(int)vlen,v);
        }
        if (lh[0]) {
          /* Some APIs include netmask (1.2.3.4/32) – trim at slash for comparison */
          char cmp[128]; snprintf(cmp,sizeof(cmp),"%s",lh); char *slash=strchr(cmp,'/'); if(slash) *slash='\0';
          if (strcmp(cmp,ip)==0) cnt++;
        }
      }
      continue;
    }
    p++;
  }
  if (cnt == 0) {
    /* Fallback pattern scan for lastHopIP / lastHop / gateway occurrences. Unlike routes, topology objects
       may differ; we count occurrences of any matching key directly referencing ip. */
    const char *keys[] = { "\"lastHopIP\":\"", "\"lastHopIp\":\"", "\"lastHop\":\"", "\"gateway\":\"", "\"via\":\"", NULL };
    for (int ki=0; keys[ki]; ++ki) {
      char pattern[96]; snprintf(pattern,sizeof(pattern),"%s%s", keys[ki], ip);
      const char *scan = section; int safety=0; while ((scan = strstr(scan, pattern)) && safety < 100000) { cnt++; scan += strlen(pattern); safety++; }
      if (cnt) break; /* stop on first key that yields hits */
    }
    if (cnt == 0) {
      const char *dbg = getenv("OLSR_DEBUG_LINK_COUNTS");
      if (dbg && *dbg=='1') fprintf(stderr, "[status-plugin][debug] node count fallback still zero for ip=%s (section head=%.40s)\n", ip, section);
    }
  }
  return cnt;
}

/* Improved unique node counting: for topology-style sections (array of objects with lastHop*/
static int count_unique_nodes_for_ip(const char *section, const char *ip) {
  if (!section || !ip || !ip[0]) return 0;
  const char *arr = strchr(section,'[');
  if (!arr) return 0;
  const char *p = arr;
  int depth = 0;
  /* store up to N unique destinations (cap to avoid excessive memory) */
  const int MAX_UNIQUE = 2048;
  char **uniq = NULL; int ucnt = 0; int rc = 0;
  while (*p) {
    if (*p=='[') { depth++; p++; continue; }
    if (*p==']') { depth--; if(depth==0) break; p++; continue; }
    if (*p=='{') {
      const char *obj = p; int od=1; p++;
      while (*p && od>0) { if (*p=='{') od++; else if (*p=='}') od--; p++; }
      const char *end = p; if (end<=obj) continue;
      char *v; size_t vlen; char lastHop[128]=""; char dest[128]="";
      /* Extract lastHop variants */
      if (find_json_string_value(obj,"lastHopIP",&v,&vlen) ||
          find_json_string_value(obj,"lastHopIp",&v,&vlen) ||
          find_json_string_value(obj,"lastHopIpAddress",&v,&vlen) ||
          find_json_string_value(obj,"lastHop",&v,&vlen) ||
          find_json_string_value(obj,"via",&v,&vlen) ||
          find_json_string_value(obj,"gateway",&v,&vlen) ||
          find_json_string_value(obj,"gatewayIP",&v,&vlen) ||
          find_json_string_value(obj,"nextHop",&v,&vlen)) {
        snprintf(lastHop,sizeof(lastHop),"%.*s",(int)vlen,v);
      }
      if (!lastHop[0]) continue;
      /* normalize (trim possible /mask) */
      char lastHopTrim[128]; snprintf(lastHopTrim,sizeof(lastHopTrim),"%s",lastHop); char *slash = strchr(lastHopTrim,'/'); if (slash) *slash='\0';
      if (strcmp(lastHopTrim, ip)!=0) continue; /* only entries for this neighbor */
      /* Extract destination variants */
      if (find_json_string_value(obj,"destinationIP",&v,&vlen) ||
          find_json_string_value(obj,"destinationIp",&v,&vlen) ||
          find_json_string_value(obj,"destination",&v,&vlen) ||
          find_json_string_value(obj,"destIpAddress",&v,&vlen) ||
          find_json_string_value(obj,"dest",&v,&vlen) ||
          find_json_string_value(obj,"to",&v,&vlen) ||
          find_json_string_value(obj,"target",&v,&vlen) ||
          find_json_string_value(obj,"originator",&v,&vlen)) {
        snprintf(dest,sizeof(dest),"%.*s",(int)vlen,v);
      }
      if (!dest[0]) continue;
      char destTrim[128]; snprintf(destTrim,sizeof(destTrim),"%s",dest); slash = strchr(destTrim,'/'); if (slash) *slash='\0';
      if (strcmp(destTrim, ip)==0) continue; /* don't count neighbor itself */
      if (destTrim[0]==0) continue;
      /* Try to resolve the destination to a node name from node_db; fall back to dest IP if unavailable. */
      char nodename[128] = "";
      if (g_nodedb_cached && g_nodedb_cached_len > 0) {
        pthread_mutex_lock(&g_nodedb_lock);
        /* Use CIDR-aware best-match lookup */
        find_best_nodename_in_nodedb(g_nodedb_cached, g_nodedb_cached_len, destTrim, nodename, sizeof(nodename));
        pthread_mutex_unlock(&g_nodedb_lock);
      }
      if (!nodename[0]) snprintf(nodename, sizeof(nodename), "%s", destTrim);
      /* linear de-dupe by node name */
      int dup = 0; for (int i=0;i<ucnt;i++) { if (strcmp(uniq[i], nodename) == 0) { dup = 1; break; } }
      if (!dup) {
        if (!uniq) uniq = (char**)calloc(MAX_UNIQUE, sizeof(char*));
        if (uniq && ucnt < MAX_UNIQUE) {
          uniq[ucnt] = strdup(nodename);
          if (uniq[ucnt]) ucnt++;
        }
      }
      continue;
    }
    p++;
  }
  rc = ucnt;
  if (uniq) { for (int i=0;i<ucnt;i++) free(uniq[i]); free(uniq); }
  return rc;
}

/* Extract twoHopNeighborCount (or linkcount as last resort) for a given neighbor IP from neighbors section */
static int neighbor_twohop_for_ip(const char *section, const char *ip) {
  if(!section||!ip||!ip[0]) return 0;
  const char *arr = strchr(section,'[');
  if(!arr) return 0;
  const char *p = arr; int depth=0; int best=0;
  while(*p){
    if(*p=='['){ depth++; p++; continue; }
    if(*p==']'){ depth--; if(depth==0) break; p++; continue; }
    if(*p=='{'){
      const char *obj=p; int od=1; p++;
      while(*p && od>0){ if(*p=='{') od++; else if(*p=='}') od--; p++; }
      const char *end=p; if(end>obj){
        char *v; size_t vlen; char addr[128]=""; char twohop_s[32]=""; char linkcount_s[32]="";
        if(find_json_string_value(obj,"ipAddress",&v,&vlen) || find_json_string_value(obj,"ip",&v,&vlen)) snprintf(addr,sizeof(addr),"%.*s",(int)vlen,v);
        if(addr[0]){ char cmp[128]; snprintf(cmp,sizeof(cmp),"%s",addr); char *slash=strchr(cmp,'/'); if(slash) *slash='\0';
          if(strcmp(cmp,ip)==0){
            if(find_json_string_value(obj,"twoHopNeighborCount",&v,&vlen)) snprintf(twohop_s,sizeof(twohop_s),"%.*s",(int)vlen,v);
            if(find_json_string_value(obj,"linkcount",&v,&vlen)) snprintf(linkcount_s,sizeof(linkcount_s),"%.*s",(int)vlen,v);
            int val=0; if(twohop_s[0]) val=atoi(twohop_s); else if(linkcount_s[0]) val=atoi(linkcount_s);
            if(val>best) best=val; /* keep largest in case of duplicates */
          }
        }
      }
      continue; }
    p++; }
  return best;
}

/* Minimal ARP enrichment: look up MAC and reverse DNS for IPv4 */
static void arp_enrich_ip(const char *ip, char *mac_out, size_t mac_len, char *host_out, size_t host_len) {
  if (mac_out && mac_len) mac_out[0]=0;
  if (host_out && host_len) host_out[0]=0;
  if(!ip||!*ip) return;
  FILE *f = fopen("/proc/net/arp", "r");
  if (f) {
    char line[512];
    if(!fgets(line,sizeof(line),f)) { fclose(f); f=NULL; }
    while (fgets(line,sizeof(line),f)) {
      char ipf[128], hw[128];
      if (sscanf(line, "%127s %*s %*s %127s", ipf, hw) == 2) {
        if (strcmp(ipf, ip)==0) {
          if (mac_out && mac_len) {
            size_t hwlen=strlen(hw); if(hwlen>=mac_len) hwlen=mac_len-1; memcpy(mac_out,hw,hwlen); mac_out[hwlen]=0;
          }
          break;
        }
      }
    }
    fclose(f);
  }
  if (host_out && host_len) {
    struct in_addr ina; if (inet_aton(ip,&ina)) { struct hostent *he=gethostbyaddr(&ina,sizeof(ina),AF_INET); if(he&&he->h_name) snprintf(host_out,host_len,"%s",he->h_name); }
  }
}

/* Basic ARP table to JSON device list */
static int devices_from_arp_json(char **out, size_t *outlen) {
  if(!out||!outlen) return -1;
  *out=NULL; *outlen=0;
  FILE *f = fopen("/proc/net/arp","r"); if(!f) return -1; char *buf=NULL; size_t cap=2048,len=0; buf=malloc(cap); if(!buf){ fclose(f); return -1;} buf[0]=0; json_buf_append(&buf,&len,&cap,"["); int first=1; char line[512];
  if(!fgets(line,sizeof(line),f)) { fclose(f); free(buf); return -1; }
  while(fgets(line,sizeof(line),f)){
    char ip[128], hw[128], dev[64];
    if (sscanf(line,"%127s %*s %*s %127s %*s %63s", ip, hw, dev) >=2) {
      if(!first) json_buf_append(&buf,&len,&cap,",");
      first=0;
      json_buf_append(&buf,&len,&cap,"{\"ipv4\":"); json_append_escaped(&buf,&len,&cap,ip);
      json_buf_append(&buf,&len,&cap,",\"hwaddr\":"); json_append_escaped(&buf,&len,&cap,hw);
      json_buf_append(&buf,&len,&cap,",\"hostname\":\"\",\"product\":\"\",\"uptime\":\"\",\"mode\":\"\",\"essid\":\"\",\"firmware\":\"\",\"signal\":\"\",\"tx_rate\":\"\",\"rx_rate\":\"\"}");
    }
  }
  fclose(f); json_buf_append(&buf,&len,&cap,"]"); *out=buf; *outlen=len; return 0;
}

/* Obtain primary non-loopback IPv4 (best effort). */
static void get_primary_ipv4(char *out, size_t outlen){ if(out&&outlen) out[0]=0; struct ifaddrs *ifap=NULL,*ifa=NULL; if(getifaddrs(&ifap)==0){ for(ifa=ifap; ifa; ifa=ifa->ifa_next){ if(ifa->ifa_addr && ifa->ifa_addr->sa_family==AF_INET){ struct sockaddr_in sa; memcpy(&sa,ifa->ifa_addr,sizeof(sa)); char b[INET_ADDRSTRLEN]; if(inet_ntop(AF_INET,&sa.sin_addr,b,sizeof(b))){ if(strcmp(b,"127.0.0.1")!=0){ snprintf(out,outlen,"%s",b); break; } } } } freeifaddrs(ifap);} }

/* Very lightweight validation that node_db json has object form & expected keys */
static int validate_nodedb_json(const char *buf, size_t len){ if(!buf||len==0) return 0; /* skip leading ws */ size_t i=0; while(i<len && (buf[i]==' '||buf[i]=='\n'||buf[i]=='\r'||buf[i]=='\t')) i++; if(i>=len) return 0; if(buf[i] != '{') return 0; /* look for some indicative keys */ if(strstr(buf,"\"v6-to-v4\"")||strstr(buf,"\"v6-to-id\"")||strstr(buf,"\"v6-hna-at\"")) return 1; if(strstr(buf,"\"n\"")) return 1; return 1; }

/* Fetch remote node_db and update cache */
static void fetch_remote_nodedb(void) {
  char ipbuf[128]=""; get_primary_ipv4(ipbuf,sizeof(ipbuf)); if(!ipbuf[0]) snprintf(ipbuf,sizeof(ipbuf),"0.0.0.0");
  char *fresh=NULL; size_t fn=0;
  
  /* Try multiple curl paths and protocols */
  const char *curl_paths[] = {"curl", "/usr/bin/curl", "/bin/curl", "/usr/local/bin/curl", NULL};
  const char *protocols[] = {"https", "http", NULL};
  int success = 0;
  
  for (const char **curl_path = curl_paths; *curl_path && !success; curl_path++) {
    for (const char **protocol = protocols; *protocol && !success; protocol++) {
      char url[512];
      if (strcmp(*protocol, "https") == 0) {
        snprintf(url, sizeof(url), "%s", g_nodedb_url);
      } else {
        // Convert HTTPS to HTTP
        if (strncmp(g_nodedb_url, "https://", 8) == 0) {
          snprintf(url, sizeof(url), "http://%s", g_nodedb_url + 8);
        } else {
          continue; // Skip if not HTTPS
        }
      }
      
      char cmd[1024]; 
      snprintf(cmd,sizeof(cmd),"%s -s --max-time 5 -H \"User-Agent: status-plugin OriginIP/%s\" -H \"Accept: application/json\" %s", *curl_path, ipbuf, url);
      
      if (util_exec(cmd,&fresh,&fn)==0 && fresh && buffer_has_content(fresh,fn) && validate_nodedb_json(fresh,fn)) {
        fprintf(stderr, "[status-plugin] nodedb fetch succeeded with %s, got %zu bytes\n", *curl_path, fn);
        success = 1;
        break;
      } else {
        if (fresh) { 
          free(fresh); 
          fresh = NULL; 
          fn = 0; 
        }
      }
    }
  }
  
  if (success) {
    /* augment: if remote JSON is an object mapping IP -> { n:.. } ensure each has hostname/name keys */
    int is_object_mapping = 0; /* heuristic: starts with '{' and contains '"n"' and an IPv4 pattern */
    if (fresh[0]=='{' && strstr(fresh,"\"n\"") && strstr(fresh,".\"")) is_object_mapping=1;
    if (is_object_mapping) {
      /* naive single-pass insertion: for each occurrence of "n":"VALUE" inside an object that lacks hostname add "hostname":"VALUE","name":"VALUE" */
      char *aug = malloc(fn*2 + 32); /* generous */
      if (aug) {
        size_t o=0; const char *p=fresh; int in_obj=0; int pending_insert=0; char last_n_val[256]; last_n_val[0]=0; int inserted_for_obj=0;
        while (*p) {
          if (*p=='{') { in_obj++; inserted_for_obj=0; last_n_val[0]=0; pending_insert=0; }
          if (*p=='}') { if (pending_insert && last_n_val[0] && !inserted_for_obj) {
              o += (size_t)snprintf(aug+o, fn*2+32 - o, ",\"hostname\":\"%s\",\"name\":\"%s\"", last_n_val, last_n_val);
              pending_insert=0; inserted_for_obj=1; }
            in_obj--; if (in_obj<0) in_obj=0; }
          if (strncmp(p,"\"n\":\"",5)==0) {
            const char *vstart = p+5; const char *q=vstart; while(*q && *q!='"') q++; size_t L=(size_t)(q-vstart); if(L>=sizeof(last_n_val)) L=sizeof(last_n_val)-1; memcpy(last_n_val,vstart,L); last_n_val[L]=0; pending_insert=1; inserted_for_obj=0;
          }
          if (pending_insert && strncmp(p,"\"hostname\"",10)==0) { pending_insert=0; inserted_for_obj=1; }
          if (pending_insert && strncmp(p,"\"name\"",6)==0) { /* still add hostname later if only name appears */ }
          aug[o++]=*p; p++; if(o>fn*2) break; }
        aug[o]=0;
        if (o>0) { free(fresh); fresh=aug; fn=strlen(fresh); }
        else free(aug);
      }
    }
    pthread_mutex_lock(&g_nodedb_lock);
    if (g_nodedb_cached) free(g_nodedb_cached);
    g_nodedb_cached=fresh; g_nodedb_cached_len=fn; g_nodedb_last_fetch=time(NULL);
    pthread_mutex_unlock(&g_nodedb_lock);
    /* write a copy for external inspection */
    FILE *wf=fopen("/tmp/node_db.json","w"); if(wf){ fwrite(g_nodedb_cached,1,g_nodedb_cached_len,wf); fclose(wf);} fresh=NULL;
  } else if (fresh) { free(fresh); }
    else { fprintf(stderr,"[status-plugin] nodedb fetch failed or invalid (%s)\n", g_nodedb_url); }
}

/* Improved unique-destination counting: counts distinct destination nodes reachable via given last hop. */
static int normalize_olsrd_links(const char *raw, char **outbuf, size_t *outlen) {
  if (!raw || !outbuf || !outlen) return -1;
  *outbuf = NULL; *outlen = 0;
  /* Always fetch remote node_db for fresh data */
  fetch_remote_nodedb();
  /* --- Route & node name fan-out (Python legacy parity) ------------------
   * The original bmk-webstatus.py derives per-neighbor route counts and node
   * counts exclusively from the Linux IPv4 routing table plus node_db names:
   *   /sbin/ip -4 r | grep -vE 'scope|default' | awk '{print $3,$1,$5}'
   * It builds:
   *   gatewaylist[gateway_ip] -> list of destination prefixes (count = routes)
   *   nodelist[gateway_ip]   -> unique node names (from node_db[dest]['n'])
   * We replicate that logic here before parsing OLSR link JSON so we can
   * prefer these authoritative counts. Only if unavailable / zero do we
   * fall back to topology / neighbors heuristic logic.
   */
  struct gw_stat { char gw[64]; int routes; int nodes; int name_count; char names[256][64]; };
  #define MAX_GW_STATS 512
  static struct gw_stat *gw_stats = NULL; static int gw_stats_count = 0; /* not cached between calls */
  do {
    char *rt_raw=NULL; size_t rlen=0;
    const char *rt_cmds[] = {
      "/sbin/ip -4 r 2>/dev/null",
      "/usr/sbin/ip -4 r 2>/dev/null",
      "ip -4 r 2>/dev/null",
      NULL
    };
    for (int ci=0; rt_cmds[ci] && !rt_raw; ++ci) {
      if (util_exec(rt_cmds[ci], &rt_raw, &rlen) == 0 && rt_raw && rlen>0) break;
      if (rt_raw) { free(rt_raw); rt_raw=NULL; rlen=0; }
    }
    if (!rt_raw) break; /* no routing table */
    gw_stats = calloc(MAX_GW_STATS, sizeof(struct gw_stat));
    if (!gw_stats) { free(rt_raw); break; }
    char *saveptr=NULL; char *line = strtok_r(rt_raw, "\n", &saveptr);
    while(line) {
      if (strstr(line, "default") || strstr(line, "scope")) { line=strtok_r(NULL,"\n",&saveptr); continue; }
      /* Expect host routes of form: <dest> via <gateway> dev <if> ... */
      char dest[64]="", via[64]="";
      if (sscanf(line, "%63s via %63s", dest, via) == 2) {
        /* strip trailing characters like ',' if any */
        for (int i=0; via[i]; ++i) if (via[i]==','||via[i]==' ') { via[i]='\0'; break; }
        if (dest[0] && via[0]) {
          /* locate / create gw_stat */
          int gi=-1; for(int i=0;i<gw_stats_count;i++){ if(strcmp(gw_stats[i].gw,via)==0){ gi=i; break; } }
          if (gi==-1 && gw_stats_count < MAX_GW_STATS) { gi=gw_stats_count++; snprintf(gw_stats[gi].gw,sizeof(gw_stats[gi].gw),"%s",via); }
          if (gi>=0) {
            gw_stats[gi].routes++;
            /* node name lookup: use CIDR-aware best-match from node_db */
            char nodename[128] = "";
            int have_name = 0;
            if (g_nodedb_cached && g_nodedb_cached_len > 0) {
              pthread_mutex_lock(&g_nodedb_lock);
              if (find_best_nodename_in_nodedb(g_nodedb_cached, g_nodedb_cached_len, dest, nodename, sizeof(nodename))) have_name = 1;
              pthread_mutex_unlock(&g_nodedb_lock);
            }
            if (have_name) {
              /* ensure unique per gateway */
              int dup=0; for (int ni=0; ni<gw_stats[gi].name_count; ++ni) if(strcmp(gw_stats[gi].names[ni],nodename)==0){ dup=1; break; }
              if(!dup && gw_stats[gi].name_count < 256) {
                /* Cap and copy safely */
                nodename[63]='\0';
                size_t nlen = strnlen(nodename, 63);
                memcpy(gw_stats[gi].names[gw_stats[gi].name_count], nodename, nlen);
                gw_stats[gi].names[gw_stats[gi].name_count][nlen] = '\0';
                gw_stats[gi].name_count++;
                gw_stats[gi].nodes = gw_stats[gi].name_count;
              }
            }
          }
        }
      }
      line = strtok_r(NULL, "\n", &saveptr);
    }
    free(rt_raw);
  } while(0);

  const char *p = strstr(raw, "\"links\"");
  const char *arr = NULL;
  if (p) arr = strchr(p, '[');
  if (!arr) {
    /* fallback: first array in document */
    arr = strchr(raw, '[');
    if (!arr) return -1;
  }
  const char *q = arr; int depth = 0;
  size_t cap = 4096; size_t len = 0; char *buf = malloc(cap); if (!buf) return -1; buf[0]=0;
  json_buf_append(&buf, &len, &cap, "["); int first = 1; int parsed = 0;
  /* Detect legacy (olsrd) or v2 (olsr2 json embedded) route/topology sections. We first look for plain
   * "routes" / "topology" keys; if not found, fall back to the wrapper keys we emit in /status (olsr_routes_raw / olsr_topology_raw).
   */
  const char *routes_section = strstr(raw, "\"routes\"");
  const char *topology_section = strstr(raw, "\"topology\"");
  if (!routes_section) {
    const char *alt = strstr(raw, "\"olsr_routes_raw\"");
    if (alt) {
      /* Skip to first '[' after this key so counting helpers work */
      const char *arrp = strchr(alt, '[');
      if (arrp) routes_section = arrp - 10 > alt ? alt : arrp; /* provide pointer inside block */
    }
  }
  if (!topology_section) {
    const char *alt = strstr(raw, "\"olsr_topology_raw\"");
    if (alt) {
      const char *arrp = strchr(alt, '[');
      if (arrp) topology_section = arrp - 10 > alt ? alt : arrp;
    }
  }
  const char *neighbors_section = strstr(raw, "\"neighbors\"");
  while (*q) {
    if (*q == '[') { depth++; q++; continue; }
    if (*q == ']') { depth--; if (depth==0) break; q++; continue; }
    if (*q == '{') {
      const char *obj = q; int od = 0; const char *r = q;
      while (*r) { if (*r=='{') od++; else if (*r=='}') { od--; if (od==0) { r++; break; } } r++; }
      if (!r || r<=obj) break;
      char *v; size_t vlen; char intf[64]=""; char local[64]=""; char remote[64]=""; char remote_host[256]=""; char lq[32]=""; char nlq[32]=""; char cost[32]="";
      if (find_json_string_value(obj, "olsrInterface", &v, &vlen) || find_json_string_value(obj, "ifName", &v, &vlen)) snprintf(intf,sizeof(intf),"%.*s",(int)vlen,v);
      if (find_json_string_value(obj, "localIP", &v, &vlen) || find_json_string_value(obj, "localIp", &v, &vlen) || find_json_string_value(obj, "local", &v, &vlen)) snprintf(local,sizeof(local),"%.*s",(int)vlen,v);
      if (find_json_string_value(obj, "remoteIP", &v, &vlen) || find_json_string_value(obj, "remoteIp", &v, &vlen) || find_json_string_value(obj, "remote", &v, &vlen) || find_json_string_value(obj, "neighborIP", &v, &vlen)) snprintf(remote,sizeof(remote),"%.*s",(int)vlen,v);
      if (!remote[0]) { q = r; continue; }
      if (remote[0]) { struct in_addr ina_r; if (inet_aton(remote,&ina_r)) { struct hostent *hre=gethostbyaddr(&ina_r,sizeof(ina_r),AF_INET); if(hre&&hre->h_name) snprintf(remote_host,sizeof(remote_host),"%s",hre->h_name); } }
      if (find_json_string_value(obj, "linkQuality", &v, &vlen)) snprintf(lq,sizeof(lq),"%.*s",(int)vlen,v);
      if (find_json_string_value(obj, "neighborLinkQuality", &v, &vlen)) snprintf(nlq,sizeof(nlq),"%.*s",(int)vlen,v);
      if (find_json_string_value(obj, "linkCost", &v, &vlen)) snprintf(cost,sizeof(cost),"%.*s",(int)vlen,v);
      int routes_cnt = routes_section ? count_routes_for_ip(routes_section, remote) : 0;
      int nodes_cnt = 0;
      char node_names_concat[1024]; node_names_concat[0]='\0';
      /* Prefer Python-style route table fan-out first (exact parity) */
      if (gw_stats_count > 0) {
        for (int gi=0; gi<gw_stats_count; ++gi) {
          if (strcmp(gw_stats[gi].gw, remote)==0) {
            if (gw_stats[gi].routes > 0) routes_cnt = gw_stats[gi].routes;
            if (gw_stats[gi].nodes > 0) nodes_cnt  = gw_stats[gi].nodes;
            if (gw_stats[gi].name_count > 0) {
              size_t off=0;
              for (int ni=0; ni<gw_stats[gi].name_count; ++ni) {
                const char *nm = gw_stats[gi].names[ni];
                if(!nm||!*nm) continue;
                size_t nlen = strnlen(nm, 63);
                size_t need = nlen + (off?1:0) + 1; /* comma + name + nul */
                if (off + need >= sizeof(node_names_concat)) break;
                if (off) {
                  node_names_concat[off++] = ',';
                }
                memcpy(&node_names_concat[off], nm, nlen);
                off += nlen;
                node_names_concat[off] = '\0';
              }
            }
            break;
          }
        }
      }
      if (topology_section) {
        if (nodes_cnt == 0) {
          nodes_cnt = count_unique_nodes_for_ip(topology_section, remote);
          if (nodes_cnt == 0) nodes_cnt = count_nodes_for_ip(topology_section, remote);
        }
      }
      /* Fallback: try neighbors section two-hop counts if topology yielded nothing */
      if (nodes_cnt == 0 && neighbors_section) {
        int twohop = neighbor_twohop_for_ip(neighbors_section, remote);
        if (twohop > 0) nodes_cnt = twohop;
        if (routes_cnt == 0 && twohop > 0) routes_cnt = twohop; /* approximate */
      }
      char routes_s[16]; snprintf(routes_s,sizeof(routes_s),"%d",routes_cnt);
      char nodes_s[16]; snprintf(nodes_s,sizeof(nodes_s),"%d",nodes_cnt);
      static char def_ip_cached[64];
      if (!def_ip_cached[0]) { char *rout_link=NULL; size_t rnl=0; if(util_exec("/sbin/ip route show default 2>/dev/null || /usr/sbin/ip route show default 2>/dev/null || ip route show default 2>/dev/null", &rout_link,&rnl)==0 && rout_link){ char *pdef=strstr(rout_link,"via "); if(pdef){ pdef+=4; char *q2=strchr(pdef,' '); if(q2){ size_t L=q2-pdef; if(L<sizeof(def_ip_cached)){ strncpy(def_ip_cached,pdef,L); def_ip_cached[L]=0; } } } free(rout_link);} }
      int is_default = (def_ip_cached[0] && strcmp(def_ip_cached, remote)==0)?1:0;
  if (!first) json_buf_append(&buf,&len,&cap,",");
  first=0;
      json_buf_append(&buf,&len,&cap,"{\"intf\":"); json_append_escaped(&buf,&len,&cap,intf);
      json_buf_append(&buf,&len,&cap,",\"local\":"); json_append_escaped(&buf,&len,&cap,local);
      json_buf_append(&buf,&len,&cap,",\"remote\":"); json_append_escaped(&buf,&len,&cap,remote);
      json_buf_append(&buf,&len,&cap,",\"remote_host\":"); json_append_escaped(&buf,&len,&cap,remote_host);
      json_buf_append(&buf,&len,&cap,",\"lq\":"); json_append_escaped(&buf,&len,&cap,lq);
      json_buf_append(&buf,&len,&cap,",\"nlq\":"); json_append_escaped(&buf,&len,&cap,nlq);
      json_buf_append(&buf,&len,&cap,",\"cost\":"); json_append_escaped(&buf,&len,&cap,cost);
      json_buf_append(&buf,&len,&cap,",\"routes\":"); json_append_escaped(&buf,&len,&cap,routes_s);
      json_buf_append(&buf,&len,&cap,",\"nodes\":"); json_append_escaped(&buf,&len,&cap,nodes_s);
  if (node_names_concat[0]) { json_buf_append(&buf,&len,&cap,",\"node_names\":"); json_append_escaped(&buf,&len,&cap,node_names_concat); }
      json_buf_append(&buf,&len,&cap,",\"is_default\":%s", is_default?"true":"false");
      json_buf_append(&buf,&len,&cap,"}");
      parsed++;
      q = r; continue;
    }
    q++;
  }
  if (parsed == 0) {
    /* broad fallback: scan objects manually */
    free(buf); buf=NULL; cap=4096; len=0; buf=malloc(cap); if(!buf) return -1; buf[0]=0; json_buf_append(&buf,&len,&cap,"["); first=1;
    const char *scan = raw; int safety=0;
    while((scan=strchr(scan,'{')) && safety<500) {
      safety++; const char *obj=scan; int od=0; const char *r=obj; while(*r){ if(*r=='{') od++; else if(*r=='}'){ od--; if(od==0){ r++; break; } } r++; }
  if(!r) break;
  size_t ol=(size_t)(r-obj);
      if(!memmem(obj,ol,"remote",6) || !memmem(obj,ol,"local",5)) { scan=scan+1; continue; }
      char *v; size_t vlen; char local[64]=""; char remote[64]=""; char remote_host[128]="";
      if(find_json_string_value(obj,"localIP",&v,&vlen) || find_json_string_value(obj,"local",&v,&vlen)) snprintf(local,sizeof(local),"%.*s",(int)vlen,v);
      if(find_json_string_value(obj,"remoteIP",&v,&vlen) || find_json_string_value(obj,"remote",&v,&vlen) || find_json_string_value(obj,"neighborIP",&v,&vlen)) snprintf(remote,sizeof(remote),"%.*s",(int)vlen,v);
      if(!remote[0]) { scan=r; continue; }
      if(remote[0]){ struct in_addr ina_r; if(inet_aton(remote,&ina_r)){ struct hostent *hre=gethostbyaddr(&ina_r,sizeof(ina_r),AF_INET); if(hre&&hre->h_name) snprintf(remote_host,sizeof(remote_host),"%s",hre->h_name); } }
  if(!first) json_buf_append(&buf,&len,&cap,",");
  first=0;
      json_buf_append(&buf,&len,&cap,"{\"intf\":\"\",\"local\":"); json_append_escaped(&buf,&len,&cap,local);
      json_buf_append(&buf,&len,&cap,",\"remote\":"); json_append_escaped(&buf,&len,&cap,remote);
      json_buf_append(&buf,&len,&cap,",\"remote_host\":"); json_append_escaped(&buf,&len,&cap,remote_host);
      json_buf_append(&buf,&len,&cap,",\"lq\":\"\",\"nlq\":\"\",\"cost\":\"\",\"routes\":\"0\",\"nodes\":\"0\",\"is_default\":false}");
      scan=r;
    }
  json_buf_append(&buf,&len,&cap,"]"); *outbuf=buf; *outlen=len;
  if (gw_stats) { free(gw_stats); gw_stats = NULL; gw_stats_count = 0; }
  return 0;
  }
  json_buf_append(&buf,&len,&cap,"]"); *outbuf=buf; *outlen=len; return 0;
  if (gw_stats) { free(gw_stats); gw_stats = NULL; gw_stats_count = 0; }
}

/* UBNT discover output acquisition using internal discovery only */
static int ubnt_discover_output(char **out, size_t *outlen) {
  if (!out || !outlen) return -1;
  *out = NULL; *outlen = 0;
  static char *cache_buf = NULL; static size_t cache_len = 0; static time_t cache_time = 0; const int CACHE_TTL = 20; /* seconds */
  time_t nowt = time(NULL);
  if (cache_buf && cache_len > 0 && nowt - cache_time < CACHE_TTL) {
    *out = malloc(cache_len+1); if(!*out) return -1; memcpy(*out, cache_buf, cache_len+1); *outlen = cache_len; return 0;
  }
  /* Skip external tool - use internal broadcast discovery only */
  /* Internal broadcast discovery */
  int s = ubnt_open_broadcast_socket(0);
  if (s >= 0) {
    struct sockaddr_in dst; memset(&dst,0,sizeof(dst)); dst.sin_family=AF_INET; dst.sin_port=htons(10001); dst.sin_addr.s_addr=inet_addr("255.255.255.255");
    if (ubnt_discover_send(s,&dst)==0) {
      struct ubnt_kv kv[64];
      struct timeval start, now; gettimeofday(&start,NULL);
      struct agg_dev { char ip[64]; char hostname[256]; char hw[64]; char product[128]; char uptime[64]; char mode[64]; char essid[128]; char firmware[128]; int have_hostname,have_hw,have_product,have_uptime,have_mode,have_essid,have_firmware,have_fwversion; char fwversion_val[128]; } devices[64];
      int dev_count = 0;
      for(;;) {
        size_t kvn = sizeof(kv)/sizeof(kv[0]); char ip[64]=""; int n = ubnt_discover_recv(s, ip, sizeof(ip), kv, &kvn);
        if (n > 0 && ip[0]) {
          int idx=-1; for(int di=0; di<dev_count; ++di){ if(strcmp(devices[di].ip, ip)==0){ idx=di; break; } }
            if(idx<0 && dev_count < (int)(sizeof(devices)/sizeof(devices[0]))){ idx = dev_count++; memset(&devices[idx],0,sizeof(devices[idx])); snprintf(devices[idx].ip,sizeof(devices[idx].ip),"%s",ip); }
            if(idx>=0){
              for(size_t i=0;i<kvn;i++){
                if(strcmp(kv[i].key,"hostname")==0 && !devices[idx].have_hostname){ snprintf(devices[idx].hostname,sizeof(devices[idx].hostname),"%s",kv[i].value); devices[idx].have_hostname=1; }
                else if(strcmp(kv[i].key,"hwaddr")==0 && !devices[idx].have_hw){ snprintf(devices[idx].hw,sizeof(devices[idx].hw),"%s",kv[i].value); devices[idx].have_hw=1; }
                else if(strcmp(kv[i].key,"product")==0 && !devices[idx].have_product){ snprintf(devices[idx].product,sizeof(devices[idx].product),"%s",kv[i].value); devices[idx].have_product=1; }
                else if(strcmp(kv[i].key,"uptime")==0 && !devices[idx].have_uptime){ snprintf(devices[idx].uptime,sizeof(devices[idx].uptime),"%s",kv[i].value); devices[idx].have_uptime=1; }
                else if(strcmp(kv[i].key,"mode")==0 && !devices[idx].have_mode){ snprintf(devices[idx].mode,sizeof(devices[idx].mode),"%s",kv[i].value); devices[idx].have_mode=1; }
                else if(strcmp(kv[i].key,"essid")==0 && !devices[idx].have_essid){ snprintf(devices[idx].essid,sizeof(devices[idx].essid),"%s",kv[i].value); devices[idx].have_essid=1; }
                else if(strcmp(kv[i].key,"firmware")==0 && !devices[idx].have_firmware){ snprintf(devices[idx].firmware,sizeof(devices[idx].firmware),"%s",kv[i].value); devices[idx].have_firmware=1; }
                else if(strcmp(kv[i].key,"fwversion")==0 && !devices[idx].have_firmware){ snprintf(devices[idx].firmware,sizeof(devices[idx].firmware),"%s",kv[i].value); devices[idx].have_firmware=1; devices[idx].have_fwversion=1; }
              }
            }
        }
        gettimeofday(&now,NULL); long ms = (now.tv_sec - start.tv_sec)*1000 + (now.tv_usec - start.tv_usec)/1000;
  if (ms > 300) { ubnt_discover_send(s,&dst); }
  if (ms > 800) break;
  usleep(20000);
      }
      close(s);
      for(int di=0; di<dev_count; ++di){ if((!devices[di].have_hostname || !devices[di].have_hw) && devices[di].ip[0]){ char arp_mac[64]=""; char arp_host[256]=""; arp_enrich_ip(devices[di].ip, arp_mac, sizeof(arp_mac), arp_host, sizeof(arp_host)); if(!devices[di].have_hw && arp_mac[0]){ snprintf(devices[di].hw,sizeof(devices[di].hw),"%s",arp_mac); devices[di].have_hw=1; } if(!devices[di].have_hostname && arp_host[0]){ snprintf(devices[di].hostname,sizeof(devices[di].hostname),"%s",arp_host); devices[di].have_hostname=1; } } }
      size_t cap=4096; size_t len=0; char *b = malloc(cap); if(!b) goto broadcast_fail; b[0]=0; json_buf_append(&b,&len,&cap,"[");
      for(int di=0; di<dev_count; ++di){ if(di) json_buf_append(&b,&len,&cap,","); json_buf_append(&b,&len,&cap,"{\"ipv4\":"); json_append_escaped(&b,&len,&cap,devices[di].ip); json_buf_append(&b,&len,&cap,",\"hwaddr\":"); json_append_escaped(&b,&len,&cap,devices[di].hw); json_buf_append(&b,&len,&cap,",\"hostname\":"); json_append_escaped(&b,&len,&cap,devices[di].hostname); json_buf_append(&b,&len,&cap,",\"product\":"); json_append_escaped(&b,&len,&cap,devices[di].product); json_buf_append(&b,&len,&cap,",\"uptime\":"); json_append_escaped(&b,&len,&cap,devices[di].uptime); json_buf_append(&b,&len,&cap,",\"mode\":"); json_append_escaped(&b,&len,&cap,devices[di].mode); json_buf_append(&b,&len,&cap,",\"essid\":"); json_append_escaped(&b,&len,&cap,devices[di].essid); json_buf_append(&b,&len,&cap,",\"firmware\":"); json_append_escaped(&b,&len,&cap,devices[di].firmware); json_buf_append(&b,&len,&cap,",\"signal\":\"\",\"tx_rate\":\"\",\"rx_rate\":\"\"}"); }
      json_buf_append(&b,&len,&cap,"]\n"); *out=b; *outlen=len; free(cache_buf); cache_buf=malloc(len+1); if(cache_buf){ memcpy(cache_buf,b,len); cache_buf[len]=0; cache_len=len; cache_time=nowt; } fprintf(stderr,"[status-plugin] internal broadcast discovery merged %d devices (%zu bytes)\n", dev_count, len); return 0;
    }
    close(s);
  }
broadcast_fail:
  /* Use internal broadcast discovery */
  if (devices_from_arp_json(out, outlen)==0 && *out && *outlen>0) { free(cache_buf); cache_buf=malloc(*outlen+1); if(cache_buf){ memcpy(cache_buf,*out,*outlen); cache_buf[*outlen]=0; cache_len=*outlen; cache_time=nowt; } return 0; }
  return -1;
}

/* Extract a quoted JSON string value for key from a JSON object region.
 * Searches for '"key"' after 'start' and returns the pointer to the value (not allocated) and length in val_len.
 * Returns 1 on success, 0 otherwise.
 */
static int find_json_string_value(const char *start, const char *key, char **val, size_t *val_len) {
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
        if (*r == '"') { *val = (char*)vstart; *val_len = (size_t)(r - vstart); return 1; }
        r++;
      }
      return 0;
    } else {
      /* not a quoted string: capture until comma or closing brace */
      const char *vstart = q; const char *r = q;
      while (*r && *r != ',' && *r != '}' && *r != '\n') r++;
      while (r > vstart && (*(r-1)==' '||*(r-1)=='\t')) r--;
      *val = (char*)vstart; *val_len = (size_t)(r - vstart); return 1;
    }
  }
  return 0;
}

/* Find best matching node name for dest_ip in a node_db JSON mapping.
 * Supports keys that are exact IPv4 or CIDR (e.g. "1.2.3.0/24"). Chooses
 * the longest-prefix match (highest mask) when multiple entries match.
 * Returns 1 on success (out_name populated), 0 otherwise.
 */
static int find_best_nodename_in_nodedb(const char *buf, size_t len, const char *dest_ip, char *out_name, size_t out_len) {
  (void)len; /* parameter present for future use; silence unused parameter warning */
  if (!buf || !dest_ip || !out_name || out_len == 0) return 0;
  out_name[0] = '\0';
  struct in_addr ina; if (!inet_aton(dest_ip, &ina)) return 0;
  uint32_t dest = ntohl(ina.s_addr);
  const char *p = buf; int best_mask = -1; char best_name[256] = "";
  while ((p = strchr(p, '"')) != NULL) {
    const char *kstart = p + 1;
    const char *kend = strchr(kstart, '"');
    if (!kend) break;
    size_t keylen = (size_t)(kend - kstart);
    if (keylen == 0 || keylen >= 64) { p = kend + 1; continue; }
    char keybuf[64]; memcpy(keybuf, kstart, keylen); keybuf[keylen] = '\0';
    /* Move to ':' and then to object '{' */
    const char *after = kend + 1;
    while (*after && (*after == ' ' || *after == '\t' || *after == '\r' || *after == '\n')) after++;
    if (*after != ':') { p = kend + 1; continue; }
    after++;
    while (*after && (*after == ' ' || *after == '\t' || *after == '\r' || *after == '\n')) after++;
    if (*after != '{') { p = kend + 1; continue; }
    /* find end of this object to limit search */
    const char *objstart = after; const char *objend = objstart; int od = 0;
    while (*objend) {
      if (*objend == '{') od++; else if (*objend == '}') { od--; if (od == 0) { objend++; break; } }
      objend++;
    }
    if (!objend || objend <= objstart) break;
    /* Only consider keys that start with digit (ipv4) */
    if (!(keybuf[0] >= '0' && keybuf[0] <= '9')) { p = objend; continue; }
    /* parse key as ip[/mask] */
    char addrpart[64]; int maskbits = 32;
    char *s = strchr(keybuf, '/'); if (s) {
      size_t L = (size_t)(s - keybuf); if (L >= sizeof(addrpart)) { p = objend; continue; }
      memcpy(addrpart, keybuf, L); addrpart[L] = '\0'; maskbits = atoi(s + 1);
    } else { snprintf(addrpart, sizeof(addrpart), "%s", keybuf); }
    struct in_addr ina_k; if (!inet_aton(addrpart, &ina_k)) { p = objend; continue; }
    uint32_t net = ntohl(ina_k.s_addr);
  if (maskbits < 0) maskbits = 0;
  if (maskbits > 32) maskbits = 32;
    uint32_t mask = (maskbits == 0) ? 0 : ((maskbits == 32) ? 0xFFFFFFFFu : (~((1u << (32 - maskbits)) - 1u)));
    if ((dest & mask) != (net & mask)) { p = objend; continue; }
    /* matched; extract "n" value inside object */
    char *v = NULL; size_t vlen = 0;
    if (find_json_string_value(objstart, "n", &v, &vlen)) {
      size_t L = vlen; if (L >= sizeof(best_name)) L = sizeof(best_name) - 1;
      memcpy(best_name, v, L); best_name[L] = '\0';
      if (maskbits > best_mask) best_mask = maskbits;
    }
    p = objend;
  }
  if (best_mask >= 0 && best_name[0]) {
    size_t L = strnlen(best_name, sizeof(best_name)); if (L >= out_len) L = out_len - 1; memcpy(out_name, best_name, L); out_name[L] = '\0'; return 1;
  }
  return 0;
}

/* forward declaration for cached hostname lookup (defined later) */
static void lookup_hostname_cached(const char *ipv4, char *out, size_t outlen);
/* forward declarations for OLSRd proxy cache helpers */
static int olsr_cache_get(const char *key, char *out, size_t outlen);
static void olsr_cache_set(const char *key, const char *val);

/* Normalize devices array from ubnt-discover JSON string `ud` into a new allocated JSON array in *outbuf (caller must free). */
static int normalize_ubnt_devices(const char *ud, char **outbuf, size_t *outlen) {
  if (!ud || !outbuf || !outlen) return -1;
  *outbuf = NULL; *outlen = 0;
  size_t cap = 4096; size_t len = 0; char *buf = malloc(cap); if (!buf) return -1; buf[0]=0;
  /* simple search for "devices" : [ */
  const char *p = strstr(ud, "\"devices\"" );
  if (!p) {
    /* No explicit "devices" key. If the payload itself is a JSON array (internal broadcast output), passthrough. */
    const char *s = ud; while (*s && isspace((unsigned char)*s)) s++;
    if (*s == '[') {
      size_t l = strlen(s);
      char *copy = malloc(l + 1);
      if (!copy) { free(buf); return -1; }
      memcpy(copy, s, l + 1);
      free(buf);
      *outbuf = copy; *outlen = l; return 0;
    }
    /* Otherwise return empty array */
    json_buf_append(&buf, &len, &cap, "[]"); *outbuf=buf; *outlen=len; return 0;
  }
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
  char *v; size_t vlen;
      /* fields to extract: ipv4 (or ip), mac or hwaddr, name/hostname, product, uptime, mode, essid, firmware */
  char ipv4[64] = ""; char hwaddr[64] = ""; char hostname[256] = ""; char product[128] = ""; char uptime[64] = ""; char mode[64] = ""; char essid[128] = ""; char firmware[128] = ""; char signal[32]=""; char tx_rate[32]=""; char rx_rate[32]="";
      if (find_json_string_value(obj_start, "ipv4", &v, &vlen) || find_json_string_value(obj_start, "ip", &v, &vlen)) { snprintf(ipv4, sizeof(ipv4), "%.*s", (int)vlen, v); }
      if (find_json_string_value(obj_start, "mac", &v, &vlen) || find_json_string_value(obj_start, "hwaddr", &v, &vlen)) { snprintf(hwaddr, sizeof(hwaddr), "%.*s", (int)vlen, v); }
      if (find_json_string_value(obj_start, "name", &v, &vlen) || find_json_string_value(obj_start, "hostname", &v, &vlen)) { snprintf(hostname, sizeof(hostname), "%.*s", (int)vlen, v); }
      if (find_json_string_value(obj_start, "product", &v, &vlen)) { snprintf(product, sizeof(product), "%.*s", (int)vlen, v); }
      if (find_json_string_value(obj_start, "uptime", &v, &vlen)) {
        /* Try to parse uptime as seconds and format it; fallback to raw string if not numeric */
        long ut = 0; char *endptr = NULL;
        if (vlen > 0) {
          char tmp[64] = ""; size_t copy = vlen < sizeof(tmp)-1 ? vlen : sizeof(tmp)-1; memcpy(tmp, v, copy); tmp[copy]=0;
          ut = strtol(tmp, &endptr, 10);
          if (endptr && *endptr == 0 && ut > 0) {
            char formatted[32] = ""; format_duration(ut, formatted, sizeof(formatted));
            snprintf(uptime, sizeof(uptime), "%s", formatted);
          } else {
            snprintf(uptime, sizeof(uptime), "%.*s", (int)vlen, v);
          }
        }
      }
      if (find_json_string_value(obj_start, "mode", &v, &vlen)) { snprintf(mode, sizeof(mode), "%.*s", (int)vlen, v); }
      if (find_json_string_value(obj_start, "essid", &v, &vlen)) { snprintf(essid, sizeof(essid), "%.*s", (int)vlen, v); }
      if (find_json_string_value(obj_start, "firmware", &v, &vlen)) { snprintf(firmware, sizeof(firmware), "%.*s", (int)vlen, v); }
  /* optional enrichment fields (best-effort) */
  if (find_json_string_value(obj_start, "signal", &v, &vlen) || find_json_string_value(obj_start, "signal_dbm", &v, &vlen)) { snprintf(signal, sizeof(signal), "%.*s", (int)vlen, v); }
  if (find_json_string_value(obj_start, "tx_rate", &v, &vlen) || find_json_string_value(obj_start, "txrate", &v, &vlen) || find_json_string_value(obj_start, "txSpeed", &v, &vlen)) { snprintf(tx_rate, sizeof(tx_rate), "%.*s", (int)vlen, v); }
  if (find_json_string_value(obj_start, "rx_rate", &v, &vlen) || find_json_string_value(obj_start, "rxrate", &v, &vlen) || find_json_string_value(obj_start, "rxSpeed", &v, &vlen)) { snprintf(rx_rate, sizeof(rx_rate), "%.*s", (int)vlen, v); }

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
  json_buf_append(&buf,&len,&cap, ",\"signal\":"); json_append_escaped(&buf,&len,&cap, signal);
  json_buf_append(&buf,&len,&cap, ",\"tx_rate\":"); json_append_escaped(&buf,&len,&cap, tx_rate);
  json_buf_append(&buf,&len,&cap, ",\"rx_rate\":"); json_append_escaped(&buf,&len,&cap, rx_rate);
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

/* Normalize olsrd neighbors JSON into array expected by UI
 * For each neighbor object produce { originator, bindto, lq, nlq, cost, metric, hostname }
 */
static int normalize_olsrd_neighbors(const char *raw, char **outbuf, size_t *outlen) {
  if (!raw || !outbuf || !outlen) return -1;
  *outbuf=NULL; *outlen=0;
  const char *p = strstr(raw, "\"neighbors\"");
  if (!p) p = strstr(raw, "\"link\""); /* some variants */
  const char *arr = p ? strchr(p,'[') : NULL;
  if (!arr) { arr = strchr(raw,'['); if(!arr) return -1; }
  const char *q = arr; int depth=0; size_t cap=4096,len=0; char *buf=malloc(cap); if(!buf) return -1; buf[0]=0;
  json_buf_append(&buf,&len,&cap,"["); int first=1;
  while(*q){
    if(*q=='['){ depth++; q++; continue; }
    if(*q==']'){ depth--; if(depth==0) break; q++; continue; }
    if(*q=='{'){
      const char *obj=q; int od=0; const char *r=q; while(*r){ if(*r=='{') od++; else if(*r=='}'){ od--; if(od==0){ r++; break; } } r++; }
      if(!r||r<=obj) break;
      char *v; size_t vlen; char originator[128]=""; char bindto[64]=""; char lq[32]=""; char nlq[32]=""; char cost[32]=""; char metric[32]=""; char hostname[256]="";
      if(find_json_string_value(obj,"neighbor_originator",&v,&vlen) || find_json_string_value(obj,"originator",&v,&vlen) || find_json_string_value(obj,"ipAddress",&v,&vlen)) snprintf(originator,sizeof(originator),"%.*s",(int)vlen,v);
      if(find_json_string_value(obj,"link_bindto",&v,&vlen)) snprintf(bindto,sizeof(bindto),"%.*s",(int)vlen,v);
      if(find_json_string_value(obj,"linkQuality",&v,&vlen) || find_json_string_value(obj,"lq",&v,&vlen)) snprintf(lq,sizeof(lq),"%.*s",(int)vlen,v);
      if(find_json_string_value(obj,"neighborLinkQuality",&v,&vlen) || find_json_string_value(obj,"nlq",&v,&vlen)) snprintf(nlq,sizeof(nlq),"%.*s",(int)vlen,v);
      if(find_json_string_value(obj,"linkCost",&v,&vlen) || find_json_string_value(obj,"cost",&v,&vlen)) snprintf(cost,sizeof(cost),"%.*s",(int)vlen,v);
      if(find_json_string_value(obj,"metric",&v,&vlen)) snprintf(metric,sizeof(metric),"%.*s",(int)vlen,v);
      if(originator[0]) lookup_hostname_cached(originator, hostname, sizeof(hostname));
  if(!first) json_buf_append(&buf,&len,&cap,",");
  first=0;
      json_buf_append(&buf,&len,&cap,"{\"originator\":"); json_append_escaped(&buf,&len,&cap,originator);
      json_buf_append(&buf,&len,&cap,",\"bindto\":"); json_append_escaped(&buf,&len,&cap,bindto);
      json_buf_append(&buf,&len,&cap,",\"lq\":"); json_append_escaped(&buf,&len,&cap,lq);
      json_buf_append(&buf,&len,&cap,",\"nlq\":"); json_append_escaped(&buf,&len,&cap,nlq);
      json_buf_append(&buf,&len,&cap,",\"cost\":"); json_append_escaped(&buf,&len,&cap,cost);
      json_buf_append(&buf,&len,&cap,",\"metric\":"); json_append_escaped(&buf,&len,&cap,metric);
      json_buf_append(&buf,&len,&cap,",\"hostname\":"); json_append_escaped(&buf,&len,&cap,hostname);
      json_buf_append(&buf,&len,&cap,"}");
  q=r; continue;
    }
    q++;
  }
  json_buf_append(&buf,&len,&cap,"]"); *outbuf=buf; *outlen=len; return 0;
}

/* forward decls for local helpers used before their definitions */
static void send_text(http_request_t *r, const char *text);
static void send_json(http_request_t *r, const char *json);
static int get_query_param(http_request_t *r, const char *key, char *out, size_t outlen);

/* Robust detection of olsrd / olsrd2 processes for diverse environments (EdgeRouter, containers, musl) */
static void detect_olsr_processes(int *out_olsrd, int *out_olsr2) {
  if(out_olsrd) {
    *out_olsrd = 0;
  }
  if(out_olsr2) {
    *out_olsr2 = 0;
  }
  char *out=NULL; size_t on=0;
  if(out_olsr2 && util_exec("pidof olsrd2 2>/dev/null", &out,&on)==0 && out && on>0){ *out_olsr2=1; }
  if(out){ free(out); out=NULL; on=0; }
  if(out_olsrd && util_exec("pidof olsrd 2>/dev/null", &out,&on)==0 && out && on>0){ *out_olsrd=1; }
  if(out){ free(out); out=NULL; on=0; }
  if( (out_olsrd && *out_olsrd) || (out_olsr2 && *out_olsr2) ) return;
  /* Fallback: parse ps output (works even when pidof missing or wrapper used) */
  if(util_exec("ps -o pid= -o comm= -o args= 2>/dev/null", &out,&on)!=0 || !out){
    if(out){ free(out); out=NULL; on=0; }
    util_exec("ps 2>/dev/null", &out,&on); /* busybox minimal */
  }
  if(out){
    const char *needle2="olsrd2"; const char *needle1="olsrd"; /* order: check olsrd2 first to avoid substring confusion */
    const char *p=out;
    while(p && *p){
      const char *line_end=strchr(p,'\n'); if(!line_end) line_end=p+strlen(p);
      if(line_end>p){
        if(out_olsr2 && !*out_olsr2){ if(strstr(p,needle2)) *out_olsr2=1; }
        if(out_olsrd && !*out_olsrd){ if(strstr(p,needle1) && !strstr(p,needle2)) *out_olsrd=1; }
        if( (out_olsrd && *out_olsrd) && (out_olsr2 && *out_olsr2) ) break;
      }
      if(*line_end==0) break; else p=line_end+1;
    }
    free(out);
  }
}

static int h_airos(http_request_t *r);

/* Full /status endpoint */
static int h_status(http_request_t *r) {
  char *buf = NULL; size_t cap = 16384, len = 0; buf = malloc(cap); if(!buf){ send_json(r, "{}\n"); return 0; } buf[0]=0;
  #define APPEND(fmt,...) do { char *_tmp=NULL; int _n=asprintf(&_tmp,fmt,##__VA_ARGS__); if(_n<0||!_tmp){ if(_tmp) free(_tmp); free(buf); send_json(r,"{}\n"); return 0;} if(len+(size_t)_n+1>cap){ while(cap<len+(size_t)_n+1) cap*=2; char *nb=realloc(buf,cap); if(!nb){ free(_tmp); free(buf); send_json(r,"{}\n"); return 0;} buf=nb;} memcpy(buf+len,_tmp,(size_t)_n); len+=(size_t)_n; buf[len]=0; free(_tmp);}while(0)
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
        struct sockaddr_in sa_local; memset(&sa_local, 0, sizeof(sa_local)); memcpy(&sa_local, ifa->ifa_addr, sizeof(sa_local));
        inet_ntop(AF_INET, &sa_local.sin_addr, bufip, sizeof(bufip));
        snprintf(ipaddr, sizeof(ipaddr), "%s", bufip);
        break;
      }
    }
    freeifaddrs(ifap);
  }

  /* uptime in seconds */
  long uptime_seconds = get_uptime_seconds();

  /* airosdata */
  char *airos_raw = NULL; size_t airos_n = 0;
  (void)airos_n; /* kept for symmetry with util_read_file signature */
  util_read_file("/tmp/10-all.json", &airos_raw, &airos_n); /* ignore result; airos_raw may be NULL */

  /* default route */
  char def_ip[64] = ""; char def_dev[64] = ""; char *rout=NULL; size_t rn=0;
  if (util_exec("/sbin/ip route show default 2>/dev/null || /usr/sbin/ip route show default 2>/dev/null || ip route show default 2>/dev/null", &rout,&rn)==0 && rout) {
    char *p=strstr(rout,"via "); if(p){ p+=4; char *q=strchr(p,' '); if(q){ size_t L=q-p; if(L<sizeof(def_ip)){ strncpy(def_ip,p,L); def_ip[L]=0; } } }
    p=strstr(rout," dev "); if(p){ p+=5; char *q=strchr(p,' '); if(!q) q=strchr(p,'\n'); if(q){ size_t L=q-p; if(L<sizeof(def_dev)){ strncpy(def_dev,p,L); def_dev[L]=0; } } }
    free(rout);
  }


  int olsr2_on=0, olsrd_on=0; detect_olsr_processes(&olsrd_on,&olsr2_on);
  if(olsr2_on) fprintf(stderr,"[status-plugin] detected olsrd2 (robust)\n");
  if(olsrd_on) fprintf(stderr,"[status-plugin] detected olsrd (robust)\n");
  if(!olsrd_on && !olsr2_on) fprintf(stderr,"[status-plugin] no OLSR process detected (robust path)\n");

  /* fetch links (for either implementation); do not toggle olsr2_on based on HTTP success */
  char *olsr_links_raw=NULL; size_t oln=0; {
    const char *endpoints[]={"http://127.0.0.1:9090/links","http://127.0.0.1:2006/links","http://127.0.0.1:8123/links",NULL};
    for(const char **ep=endpoints; *ep && !olsr_links_raw; ++ep){
      char cmd[256]; snprintf(cmd,sizeof(cmd),"/usr/bin/curl -s --max-time 1 %s", *ep);
      fprintf(stderr,"[status-plugin] trying OLSR endpoint: %s\n", *ep);
      if(util_exec(cmd,&olsr_links_raw,&oln)==0 && olsr_links_raw && oln>0){
        fprintf(stderr,"[status-plugin] fetched OLSR links from %s (%zu bytes)\n", *ep, oln);
        break;
      }
      if(olsr_links_raw){ free(olsr_links_raw); olsr_links_raw=NULL; oln=0; }
    }
  }

  /* (legacy duplicate fetch block removed after refactor) */

  char *olsr_neighbors_raw=NULL; size_t olnn=0; if(util_exec("/usr/bin/curl -s --max-time 1 http://127.0.0.1:9090/neighbors", &olsr_neighbors_raw,&olnn)!=0 && olsr_neighbors_raw){ free(olsr_neighbors_raw); olsr_neighbors_raw=NULL; olnn=0; }
  char *olsr_routes_raw=NULL; size_t olr=0; if(util_exec("/usr/bin/curl -s --max-time 1 http://127.0.0.1:9090/routes", &olsr_routes_raw,&olr)!=0 && olsr_routes_raw){ free(olsr_routes_raw); olsr_routes_raw=NULL; olr=0; }
  char *olsr_topology_raw=NULL; size_t olt=0; if(util_exec("/usr/bin/curl -s --max-time 1 http://127.0.0.1:9090/topology", &olsr_topology_raw,&olt)!=0 && olsr_topology_raw){ free(olsr_topology_raw); olsr_topology_raw=NULL; olt=0; }

  /* Build JSON */
  APPEND("{");
  APPEND("\"hostname\":"); json_append_escaped(&buf,&len,&cap,hostname); APPEND(",");
  APPEND("\"ip\":"); json_append_escaped(&buf,&len,&cap,ipaddr); APPEND(",");
  APPEND("\"uptime\":\"%ld\",", uptime_seconds);

  /* default_route */
  /* attempt reverse DNS for default route IP to provide a hostname for the gateway */
  /* Try EdgeRouter path first */
  char *vout=NULL; size_t vn=0; int versions_loaded=0;
  if (path_exists("/config/custom/versions.sh")) {
    if (util_exec("/config/custom/versions.sh", &vout, &vn) == 0 && vout && vn>0) {
      APPEND("\"versions\":%s,", vout); free(vout); vout = NULL; versions_loaded = 1;
    }
  } else if (path_exists("/usr/share/olsrd-status-plugin/versions.sh")) {
    if (util_exec("/usr/share/olsrd-status-plugin/versions.sh", &vout, &vn) == 0 && vout && vn>0) {
      APPEND("\"versions\":%s,", vout); free(vout); vout = NULL; versions_loaded = 1;
    }
  } else if (path_exists("./versions.sh")) {
    if (util_exec("./versions.sh", &vout, &vn) == 0 && vout && vn>0) {
      APPEND("\"versions\":%s,", vout); free(vout); vout = NULL; versions_loaded = 1;
    }
  }
  
  if (!versions_loaded) {
    /* Provide basic fallback versions for Linux container */
    APPEND("\"versions\":{\"olsrd\":\"unknown\",\"system\":\"linux-container\"},");
  }
  /* attempt to read traceroute_to from settings.inc (same path as python reference) */
    /* attempt to read traceroute_to from settings.inc (same path as python reference)
     * Default to the Python script's traceroute target if not configured.
     */
    char traceroute_to[256] = "78.41.115.36";
  /* human readable uptime string, prefer python-like format */
  char uptime_str[64] = ""; format_duration(uptime_seconds, uptime_str, sizeof(uptime_str));
  char uptime_linux[160] = ""; format_uptime_linux(uptime_seconds, uptime_linux, sizeof(uptime_linux));
  APPEND("\"uptime_str\":"); json_append_escaped(&buf, &len, &cap, uptime_str); APPEND(",");
  APPEND("\"uptime_linux\":"); json_append_escaped(&buf, &len, &cap, uptime_linux); APPEND(",");
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
  /* fallback: if traceroute_to not set, use default route IP (filled later) - placeholder handled below */
  /* Try to populate devices from ubnt-discover using helper */
  {
    char *ud = NULL; size_t udn = 0;
    if (ubnt_discover_output(&ud, &udn) == 0 && ud && udn > 0) {
      fprintf(stderr, "[status-plugin] got device data from ubnt-discover (%zu bytes)\n", udn);
      char *normalized = NULL; size_t nlen = 0;
      if (normalize_ubnt_devices(ud, &normalized, &nlen) == 0 && normalized) {
        APPEND("\"devices\":");
        json_buf_append(&buf, &len, &cap, "%s", normalized);
        APPEND(",");
        free(normalized);
      } else {
        fprintf(stderr, "[status-plugin] failed to normalize device data\n");
        APPEND("\"devices\":[] ,");
      }
      free(ud);
    } else {
      fprintf(stderr, "[status-plugin] no device data available\n");
      APPEND("\"devices\":[] ,");
    }
  }

  /* links: either from olsrd API or empty array */
  if (olsr_links_raw && oln>0) {
    /* try to normalize olsrd raw links into UI-friendly format */
    char *norm = NULL; size_t nn = 0;
    /* combine links + routes raw so route counting inside normalizer can see routes section */
    char *combined_raw=NULL; size_t combined_len=0;
    if (olsr_links_raw) {
      size_t l1=strlen(olsr_links_raw); size_t l2= (olsr_routes_raw? strlen(olsr_routes_raw):0);
      combined_len = l1 + l2 + 8;
      combined_raw = malloc(combined_len+1);
      if (combined_raw) {
        combined_raw[0]=0;
        memcpy(combined_raw, olsr_links_raw, l1); combined_raw[l1]='\n';
        if (l2) memcpy(combined_raw+l1+1, olsr_routes_raw, l2);
        combined_raw[l1+1+l2]=0;
      }
    }
    if (normalize_olsrd_links(combined_raw?combined_raw:olsr_links_raw, &norm, &nn) == 0 && norm && nn>0) {
      APPEND("\"links\":"); json_buf_append(&buf, &len, &cap, "%s", norm); APPEND(",");
      /* also attempt to normalize neighbors from neighbors payload */
      char *nne = NULL; size_t nne_n = 0;
      if (olsr_neighbors_raw && olnn > 0 && normalize_olsrd_neighbors(olsr_neighbors_raw, &nne, &nne_n) == 0 && nne && nne_n>0) {
        APPEND("\"neighbors\":"); json_buf_append(&buf, &len, &cap, "%s", nne); APPEND(",");
        free(nne);
      } else {
        APPEND("\"neighbors\":[],");
      }
      free(norm);
      if (combined_raw) { free(combined_raw); combined_raw=NULL; }
    } else {
      APPEND("\"links\":"); json_buf_append(&buf, &len, &cap, "%s", olsr_links_raw); APPEND(",");
      /* neighbors fallback: try neighbors data first, then links */
      char *nne2 = NULL; size_t nne2_n = 0;
      if ((olsr_neighbors_raw && olnn > 0 && normalize_olsrd_neighbors(olsr_neighbors_raw, &nne2, &nne2_n) == 0 && nne2 && nne2_n>0) ||
          (normalize_olsrd_neighbors(olsr_links_raw, &nne2, &nne2_n) == 0 && nne2 && nne2_n>0)) {
        APPEND("\"neighbors\":"); json_buf_append(&buf, &len, &cap, "%s", nne2); APPEND(","); free(nne2);
      } else {
        APPEND("\"neighbors\":[],");
      }
      if (combined_raw) { free(combined_raw); combined_raw=NULL; }
    }
  } else {
    APPEND("\"links\":[],");
    /* try to get neighbors even if no links */
    char *nne3 = NULL; size_t nne3_n = 0;
    if (olsr_neighbors_raw && olnn > 0 && normalize_olsrd_neighbors(olsr_neighbors_raw, &nne3, &nne3_n) == 0 && nne3 && nne3_n>0) {
      APPEND("\"neighbors\":"); json_buf_append(&buf, &len, &cap, "%s", nne3); APPEND(",");
      free(nne3);
    } else {
      APPEND("\"neighbors\":[],");
    }
  }
  APPEND("\"olsr2_on\":%s,", olsr2_on?"true":"false");
  APPEND("\"olsrd_on\":%s", olsrd_on?"true":"false");

  /* include raw olsr JSON for neighbors/routes/topology when available to mimic python script */
  if (olsr_neighbors_raw && olnn>0) { APPEND(",\"olsr_neighbors_raw\":%s", olsr_neighbors_raw); }
  if (olsr_routes_raw && olr>0) { APPEND(",\"olsr_routes_raw\":%s", olsr_routes_raw); }
  if (olsr_topology_raw && olt>0) { APPEND(",\"olsr_topology_raw\":%s", olsr_topology_raw); }

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
  if (!traceroute_to[0]) {
    /* if not explicitly configured, use default route IP as traceroute target */
    if (def_ip[0]) snprintf(traceroute_to, sizeof(traceroute_to), "%s", def_ip);
  }
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
          if (strstr(line, "traceroute to") == line) continue;
          /* Normalize multiple spaces -> single to simplify splitting */
          char *norm = strdup(line); if(!norm) continue;
          for(char *q=norm; *q; ++q){ if(*q=='\t') *q=' '; }
          /* collapse spaces */
          char *w=norm, *rdr=norm; int sp=0; while(*rdr){ if(*rdr==' '){ if(!sp){ *w++=' '; sp=1; } } else { *w++=*rdr; sp=0; } rdr++; } *w=0;
          /* ping buffer enlarged to 64 to avoid truncation when copying parsed token */
          char hop[16]=""; char ip[64]=""; char host[256]=""; char ping[64]="";
          /* '*' hop */
          if (strchr(norm,'*') && strstr(norm," * ")==norm+ (strchr(norm,' ')? (strchr(norm,' ')-norm)+1:0)) {
            /* leave as '*' no latency */
          }
          /* Tokenize manually */
          char *save=NULL; char *tok=strtok_r(norm," ",&save); int idx=0; char seen_paren_ip=0; char raw_ip_paren[64]=""; char raw_host[256]=""; 
          char prev_tok[64]="";
          while(tok){
            if(idx==0){ snprintf(hop,sizeof(hop),"%s",tok); }
            else if(idx==1){
              if(tok[0]=='('){ /* rare ordering, will handle later */ }
              else if(strcmp(tok,"*")==0){ snprintf(ip,sizeof(ip),"*"); }
              else { snprintf(raw_host,sizeof(raw_host),"%s",tok); }
            } else {
              if(tok[0]=='('){
                char *endp=strchr(tok,')'); if(endp){ *endp=0; snprintf(raw_ip_paren,sizeof(raw_ip_paren),"%s",tok+1); seen_paren_ip=1; }
              }
              /* latency extraction: accept forms '12.3ms' OR '12.3' followed by token 'ms' */
              if(!ping[0]) {
                size_t L = strlen(tok);
                if(L>2 && tok[L-2]=='m' && tok[L-1]=='s') {
                  char num[32]; size_t cpy = (L-2) < sizeof(num)-1 ? (L-2) : sizeof(num)-1; memcpy(num,tok,cpy); num[cpy]=0;
                  int ok=1; for(size_t xi=0; xi<cpy; ++xi){ if(!(isdigit((unsigned char)num[xi]) || num[xi]=='.')) { ok=0; break; } }
                  if(ok && cpy>0) snprintf(ping,sizeof(ping),"%s",num);
                } else if(strcmp(tok,"ms")==0 && prev_tok[0]) {
                  int ok=1; for(size_t xi=0; prev_tok[xi]; ++xi){ if(!(isdigit((unsigned char)prev_tok[xi]) || prev_tok[xi]=='.')) { ok=0; break; } }
                  if(ok) snprintf(ping,sizeof(ping),"%s",prev_tok);
                }
              }
            }
            /* remember token for next iteration */
            snprintf(prev_tok,sizeof(prev_tok),"%s",tok);
            tok=strtok_r(NULL," ",&save); idx++;
          }
          /* If ping captured originally with trailing ms (legacy), ensure we didn't store literal 'ms' */
          if(strcmp(ping,"ms")==0) ping[0]=0;
          if(seen_paren_ip){
            snprintf(ip,sizeof(ip),"%s",raw_ip_paren);
            snprintf(host,sizeof(host),"%s",raw_host);
          } else {
            if(raw_host[0]) {
              int is_ip=1; for(char *c=raw_host; *c; ++c){ if(!isdigit((unsigned char)*c) && *c!='.') { is_ip=0; break; } }
              if(is_ip) {
                /* limit copy explicitly to avoid warning (raw_host len already bounded) */
                snprintf(ip,sizeof(ip),"%.*s", (int)sizeof(ip)-1, raw_host);
              } else {
                snprintf(host,sizeof(host),"%.*s", (int)sizeof(host)-1, raw_host);
              }
            }
          }
          free(norm);
          if(!first) APPEND(","); first=0;
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

/* --- Lightweight /status/lite (omit OLSR link/neighbor discovery for faster initial load) --- */
static int h_status_lite(http_request_t *r) {
  char *buf = NULL; size_t cap = 4096, len = 0; buf = malloc(cap); if(!buf){ send_json(r,"{}\n"); return 0; } buf[0]=0;
  #define APP_L(fmt,...) do { char *_tmp=NULL; int _n=asprintf(&_tmp,fmt,##__VA_ARGS__); if(_n<0||!_tmp){ if(_tmp) free(_tmp); free(buf); send_json(r,"{}\n"); return 0;} if(len+(size_t)_n+1>cap){ while(cap<len+(size_t)_n+1) cap*=2; char *nb=realloc(buf,cap); if(!nb){ free(_tmp); free(buf); send_json(r,"{}\n"); return 0;} buf=nb;} memcpy(buf+len,_tmp,(size_t)_n); len+=(size_t)_n; buf[len]=0; free(_tmp);}while(0)
  APP_L("{");
  char hostname[256]=""; if(gethostname(hostname,sizeof(hostname))==0) hostname[sizeof(hostname)-1]=0; APP_L("\"hostname\":"); json_append_escaped(&buf,&len,&cap,hostname); APP_L(",");
  /* primary IPv4 */
  char ipaddr[128]=""; struct ifaddrs *ifap=NULL,*ifa=NULL; if(getifaddrs(&ifap)==0){ for(ifa=ifap;ifa;ifa=ifa->ifa_next){ if(!ifa->ifa_addr) continue; if(ifa->ifa_addr->sa_family==AF_INET){ struct sockaddr_in sa; memcpy(&sa,ifa->ifa_addr,sizeof(sa)); char b[INET_ADDRSTRLEN]; if(inet_ntop(AF_INET,&sa.sin_addr,b,sizeof(b)) && strcmp(b,"127.0.0.1")!=0){ snprintf(ipaddr,sizeof(ipaddr),"%s",b); break; } } } if(ifap) freeifaddrs(ifap);} APP_L("\"ip\":"); json_append_escaped(&buf,&len,&cap,ipaddr); APP_L(",");
  /* uptime */
  long uptime_seconds = get_uptime_seconds();
  char uptime_str[64]=""; format_duration(uptime_seconds, uptime_str, sizeof(uptime_str));
  char uptime_linux[160]=""; format_uptime_linux(uptime_seconds, uptime_linux, sizeof(uptime_linux));
  APP_L("\"uptime_str\":"); json_append_escaped(&buf,&len,&cap,uptime_str); APP_L(",");
  APP_L("\"uptime_linux\":"); json_append_escaped(&buf,&len,&cap,uptime_linux); APP_L(",");
  /* default route */
  char def_ip[64]="", def_dev[64]="", def_hostname[256]=""; char *rout=NULL; size_t rn=0; if(util_exec("/sbin/ip route show default 2>/dev/null || /usr/sbin/ip route show default 2>/dev/null || ip route show default 2>/dev/null", &rout,&rn)==0 && rout){ char *p=strstr(rout,"via "); if(p){ p+=4; char *q=strchr(p,' '); if(q){ size_t L=q-p; if(L<sizeof(def_ip)){ strncpy(def_ip,p,L); def_ip[L]=0; } } } p=strstr(rout," dev "); if(p){ p+=5; char *q=strchr(p,' '); if(!q) q=strchr(p,'\n'); if(q){ size_t L=q-p; if(L<sizeof(def_dev)){ strncpy(def_dev,p,L); def_dev[L]=0; } } } free(rout);} if(def_ip[0]){ struct in_addr ina; if(inet_aton(def_ip,&ina)){ struct hostent *he=gethostbyaddr(&ina,sizeof(ina),AF_INET); if(he && he->h_name) snprintf(def_hostname,sizeof(def_hostname),"%s",he->h_name); }}
  APP_L("\"default_route\":{");
  APP_L("\"ip\":"); json_append_escaped(&buf,&len,&cap,def_ip);
  APP_L(",\"dev\":"); json_append_escaped(&buf,&len,&cap,def_dev);
  APP_L(",\"hostname\":"); json_append_escaped(&buf,&len,&cap,def_hostname);
  APP_L("},");
  /* devices */
  {
    char *ud=NULL; size_t udn=0; if(ubnt_discover_output(&ud,&udn)==0 && ud){ char *normalized=NULL; size_t nlen=0; if(normalize_ubnt_devices(ud,&normalized,&nlen)==0 && normalized){ APP_L("\"devices\":%s,", normalized); free(normalized);} else APP_L("\"devices\":[],"); free(ud);} else APP_L("\"devices\":[],");
  }
  /* airos data minimal */
  if(path_exists("/tmp/10-all.json")){ char *ar=NULL; size_t an=0; if(util_read_file("/tmp/10-all.json",&ar,&an)==0 && ar){ APP_L("\"airosdata\":%s,", ar); free(ar);} else APP_L("\"airosdata\":{},"); } else APP_L("\"airosdata\":{},");
  /* versions (fast attempt) */
  char *vout=NULL; size_t vn=0; int versions_loaded=0; if(path_exists("/config/custom/versions.sh")){ if(util_exec("/config/custom/versions.sh",&vout,&vn)==0 && vout){ APP_L("\"versions\":%s,", vout); free(vout); versions_loaded=1; }} else if(path_exists("/usr/share/olsrd-status-plugin/versions.sh")){ if(util_exec("/usr/share/olsrd-status-plugin/versions.sh",&vout,&vn)==0 && vout){ APP_L("\"versions\":%s,", vout); free(vout); versions_loaded=1; }} if(!versions_loaded) APP_L("\"versions\":{\"olsrd\":\"unknown\"},");
  /* detect olsrd / olsrd2 (previously skipped in lite) */
  int lite_olsr2_on=0, lite_olsrd_on=0; detect_olsr_processes(&lite_olsrd_on,&lite_olsr2_on);
  APP_L("\"olsr2_on\":%s,\"olsrd_on\":%s", lite_olsr2_on?"true":"false", lite_olsrd_on?"true":"false");
  APP_L("}\n");
  http_send_status(r,200,"OK"); http_printf(r,"Content-Type: application/json; charset=utf-8\r\n\r\n"); http_write(r,buf,len); free(buf); return 0;
}

/* --- Per-neighbor routes endpoint: /olsr/routes?via=1.2.3.4 --- */
static int h_olsr_routes(http_request_t *r) {
  char via_ip[64]=""; get_query_param(r,"via", via_ip, sizeof(via_ip));
  int filter = via_ip[0] ? 1 : 0;
  /* fetch routes JSON from possible endpoints */
  char *raw=NULL; size_t rn=0; const char *eps[]={"http://127.0.0.1:9090/routes","http://127.0.0.1:2006/routes","http://127.0.0.1:8123/routes",NULL};
  for(const char **ep=eps; *ep && !raw; ++ep){ char cmd[256]; snprintf(cmd,sizeof(cmd),"/usr/bin/curl -s --max-time 1 %s", *ep); if(util_exec(cmd,&raw,&rn)==0 && raw && rn>0) break; if(raw){ free(raw); raw=NULL; rn=0; } }
  if(!raw){ send_json(r, "{\"via\":\"\",\"routes\":[]}\n"); return 0; }
  char *out=NULL; size_t cap=4096,len=0; out=malloc(cap); if(!out){ free(raw); send_json(r,"{\"via\":\"\",\"routes\":[]}\n"); return 0;} out[0]=0;
  #define APP_R(fmt,...) do { char *_t=NULL; int _n=asprintf(&_t,fmt,##__VA_ARGS__); if(_n<0||!_t){ if(_t) free(_t); free(out); free(raw); send_json(r,"{\"via\":\"\",\"routes\":[]}\n"); return 0;} if(len+(size_t)_n+1>cap){ while(cap<len+(size_t)_n+1) cap*=2; char *nb=realloc(out,cap); if(!nb){ free(_t); free(out); free(raw); send_json(r,"{\"via\":\"\",\"routes\":[]}\n"); return 0;} out=nb;} memcpy(out+len,_t,(size_t)_n); len+=(size_t)_n; out[len]=0; free(_t);}while(0)
  APP_R("{\"via\":"); json_append_escaped(&out,&len,&cap, via_ip); APP_R(",\"routes\":["); int first=1; int count=0;
  const char *p=strchr(raw,'['); if(!p) p=raw;
  while(*p){
    if(*p=='{'){
      const char *obj=p; int od=1; p++; while(*p && od>0){ if(*p=='{') od++; else if(*p=='}') od--; p++; } const char *end=p;
      if(end>obj){
        char *v; size_t vlen; char gw[128]=""; char dst[128]=""; char dev[64]=""; char metric[32]="";
        if(find_json_string_value(obj,"via",&v,&vlen) || find_json_string_value(obj,"gateway",&v,&vlen) || find_json_string_value(obj,"gatewayIP",&v,&vlen) || find_json_string_value(obj,"nextHop",&v,&vlen)) snprintf(gw,sizeof(gw),"%.*s",(int)vlen,v);
        if(find_json_string_value(obj,"destination",&v,&vlen) || find_json_string_value(obj,"destinationIPNet",&v,&vlen) || find_json_string_value(obj,"dst",&v,&vlen)) snprintf(dst,sizeof(dst),"%.*s",(int)vlen,v);
        if(find_json_string_value(obj,"device",&v,&vlen) || find_json_string_value(obj,"dev",&v,&vlen) || find_json_string_value(obj,"interface",&v,&vlen)) snprintf(dev,sizeof(dev),"%.*s",(int)vlen,v);
        if(find_json_string_value(obj,"metric",&v,&vlen) || find_json_string_value(obj,"rtpMetricCost",&v,&vlen)) snprintf(metric,sizeof(metric),"%.*s",(int)vlen,v);
        int match=1; if(filter){ if(!gw[0]) match=0; else { char gw_ip[128]; snprintf(gw_ip,sizeof(gw_ip),"%s",gw); char *slash=strchr(gw_ip,'/'); if(slash) *slash=0; if(strcmp(gw_ip,via_ip)!=0) match=0; } }
        if(match && dst[0]){
          if(!first) APP_R(","); first=0; count++;
          char line[320]; if(metric[0]) snprintf(line,sizeof(line),"%s %s %s", dst, dev, metric); else snprintf(line,sizeof(line),"%s %s", dst, dev);
          json_append_escaped(&out,&len,&cap,line);
        }
      }
      continue; }
    p++; }
  APP_R("],\"count\":%d}\n", count);
  http_send_status(r,200,"OK"); http_printf(r,"Content-Type: application/json; charset=utf-8\r\n\r\n"); http_write(r,out,len); free(out); free(raw); return 0; }

/* --- OLSR links endpoint with minimal neighbors --- */
static int h_olsr_links(http_request_t *r) {
  int olsr2_on=0, olsrd_on=0; detect_olsr_processes(&olsrd_on,&olsr2_on);
  /* fetch links regardless of legacy vs v2 */
  char *links_raw=NULL; size_t ln=0; {
    const char *eps[]={"http://127.0.0.1:9090/links","http://127.0.0.1:2006/links","http://127.0.0.1:8123/links",NULL};
    for(const char **ep=eps; *ep && !links_raw; ++ep){ char cmd[256]; snprintf(cmd,sizeof(cmd),"/usr/bin/curl -s --max-time 1 %s", *ep); if(util_exec(cmd,&links_raw,&ln)==0 && links_raw && ln>0) break; if(links_raw){ free(links_raw); links_raw=NULL; ln=0; } }
  }
  char *neighbors_raw=NULL; size_t nnr=0; util_exec("/usr/bin/curl -s --max-time 1 http://127.0.0.1:9090/neighbors", &neighbors_raw,&nnr);
  char *routes_raw=NULL; size_t rr=0; util_exec("/usr/bin/curl -s --max-time 1 http://127.0.0.1:9090/routes", &routes_raw,&rr);
  char *topology_raw=NULL; size_t tr=0; util_exec("/usr/bin/curl -s --max-time 1 http://127.0.0.1:9090/topology", &topology_raw,&tr);
  char *norm_links=NULL; size_t nlinks=0; {
    size_t l1 = links_raw?strlen(links_raw):0;
    size_t l2 = routes_raw?strlen(routes_raw):0;
    size_t l3 = topology_raw?strlen(topology_raw):0;
    if (l1||l2||l3) {
      char *combined_raw = malloc(l1+l2+l3+16);
      if (combined_raw) {
        size_t off=0;
        if (l1){ memcpy(combined_raw+off,links_raw,l1); off+=l1; combined_raw[off++]='\n'; }
        if (l2){ memcpy(combined_raw+off,routes_raw,l2); off+=l2; combined_raw[off++]='\n'; }
        if (l3){ memcpy(combined_raw+off,topology_raw,l3); off+=l3; }
        combined_raw[off]=0;
        if(normalize_olsrd_links(combined_raw,&norm_links,&nlinks)!=0){ norm_links=NULL; }
        free(combined_raw);
      }
    }
  }
  char *norm_neighbors=NULL; size_t nneigh=0; if(neighbors_raw && normalize_olsrd_neighbors(neighbors_raw,&norm_neighbors,&nneigh)!=0){ norm_neighbors=NULL; }
  /* Build JSON */
  char *buf=NULL; size_t cap=8192,len=0; buf=malloc(cap); if(!buf){ send_json(r,"{}\n"); goto done; } buf[0]=0;
  #define APP_O(fmt,...) do { char *_t=NULL; int _n=asprintf(&_t,fmt,##__VA_ARGS__); if(_n<0||!_t){ if(_t) free(_t); if(buf){ free(buf);} send_json(r,"{}\n"); goto done; } if(len+(size_t)_n+1>cap){ while(cap<len+(size_t)_n+1) cap*=2; char *nb=realloc(buf,cap); if(!nb){ free(_t); free(buf); send_json(r,"{}\n"); goto done;} buf=nb;} memcpy(buf+len,_t,(size_t)_n); len+=(size_t)_n; buf[len]=0; free(_t);}while(0)
  APP_O("{");
  APP_O("\"olsr2_on\":%s,", olsr2_on?"true":"false");
  APP_O("\"olsrd_on\":%s,", olsrd_on?"true":"false");
  if(norm_links) APP_O("\"links\":%s,", norm_links); else APP_O("\"links\":[],");
  if(norm_neighbors) APP_O("\"neighbors\":%s", norm_neighbors); else APP_O("\"neighbors\":[]");
  APP_O("}\n");
  http_send_status(r,200,"OK"); http_printf(r,"Content-Type: application/json; charset=utf-8\r\n\r\n"); http_write(r,buf,len);
  free(buf);
done:
  if (links_raw) free(links_raw);
  if (neighbors_raw) free(neighbors_raw);
  if (routes_raw) free(routes_raw);
  if (topology_raw) free(topology_raw);
  if (norm_links) free(norm_links);
  if (norm_neighbors) free(norm_neighbors);
  return 0;
}

/* Debug endpoint: expose per-neighbor unique destination list to verify node counting */
static int h_olsr_links_debug(http_request_t *r) {
  send_json(r, "{\"error\":\"debug disabled pending fix\"}\n");
  return 0;
}

/* --- Debug raw OLSR data: /olsr/raw (NOT for production; helps diagnose node counting) --- */
static int h_olsr_raw(http_request_t *r) {
  char *links_raw=NULL; size_t ln=0; const char *eps_links[]={"http://127.0.0.1:9090/links","http://127.0.0.1:2006/links","http://127.0.0.1:8123/links",NULL};
  for(const char **ep=eps_links; *ep && !links_raw; ++ep){ char cmd[256]; snprintf(cmd,sizeof(cmd),"/usr/bin/curl -s --max-time 1 %s", *ep); if(util_exec(cmd,&links_raw,&ln)==0 && links_raw && ln>0) break; if(links_raw){ free(links_raw); links_raw=NULL; ln=0; } }
  char *routes_raw=NULL; size_t rr=0; const char *eps_routes[]={"http://127.0.0.1:9090/routes","http://127.0.0.1:2006/routes","http://127.0.0.1:8123/routes",NULL};
  for(const char **ep=eps_routes; *ep && !routes_raw; ++ep){ char cmd[256]; snprintf(cmd,sizeof(cmd),"/usr/bin/curl -s --max-time 1 %s", *ep); if(util_exec(cmd,&routes_raw,&rr)==0 && routes_raw && rr>0) break; if(routes_raw){ free(routes_raw); routes_raw=NULL; rr=0; } }
  char *topology_raw=NULL; size_t trn=0; const char *eps_topo[]={"http://127.0.0.1:9090/topology","http://127.0.0.1:2006/topology","http://127.0.0.1:8123/topology",NULL};
  for(const char **ep=eps_topo; *ep && !topology_raw; ++ep){ char cmd[256]; snprintf(cmd,sizeof(cmd),"/usr/bin/curl -s --max-time 1 %s", *ep); if(util_exec(cmd,&topology_raw,&trn)==0 && topology_raw && trn>0) break; if(topology_raw){ free(topology_raw); topology_raw=NULL; trn=0; } }
  char *buf=NULL; size_t cap=8192,len=0; buf=malloc(cap); if(!buf){ send_json(r,"{\"err\":\"oom\"}\n"); goto done; } buf[0]=0;
  #define APP_RAW(fmt,...) do { char *_t=NULL; int _n=asprintf(&_t,fmt,##__VA_ARGS__); if(_n<0||!_t){ if(_t) free(_t); if(buf){ free(buf);} send_json(r,"{\"err\":\"oom\"}\n"); goto done;} if(len+(size_t)_n+1>cap){ while(cap<len+(size_t)_n+1) cap*=2; char *nb=realloc(buf,cap); if(!nb){ free(_t); free(buf); send_json(r,"{\"err\":\"oom\"}\n"); goto done;} buf=nb;} memcpy(buf+len,_t,(size_t)_n); len+=(size_t)_n; buf[len]=0; free(_t);}while(0)
  APP_RAW("{");
  APP_RAW("\"links_raw\":"); if(links_raw) json_append_escaped(&buf,&len,&cap, links_raw); else APP_RAW("\"\""); APP_RAW(",");
  APP_RAW("\"routes_raw\":"); if(routes_raw) json_append_escaped(&buf,&len,&cap, routes_raw); else APP_RAW("\"\""); APP_RAW(",");
  APP_RAW("\"topology_raw\":"); if(topology_raw) json_append_escaped(&buf,&len,&cap, topology_raw); else APP_RAW("\"\"");
  APP_RAW("}\n");
  http_send_status(r,200,"OK"); http_printf(r,"Content-Type: application/json; charset=utf-8\r\n\r\n"); http_write(r,buf,len);
  free(buf);
done:
  if(links_raw) free(links_raw);
  if(routes_raw) free(routes_raw);
  if(topology_raw) free(topology_raw);
  return 0;
}

/* Lightweight summary: only essentials for initial paint */
static int h_status_summary(http_request_t *r) {
  char hostname[256]=""; if (gethostname(hostname,sizeof(hostname))==0) hostname[sizeof(hostname)-1]=0; else hostname[0]=0;
  char ipaddr[128]=""; struct ifaddrs *ifap=NULL,*ifa=NULL; if (getifaddrs(&ifap)==0){ for(ifa=ifap;ifa;ifa=ifa->ifa_next){ if(ifa->ifa_addr && ifa->ifa_addr->sa_family==AF_INET){ struct sockaddr_in sa; memcpy(&sa,ifa->ifa_addr,sizeof(sa)); char b[INET_ADDRSTRLEN]; if(inet_ntop(AF_INET,&sa.sin_addr,b,sizeof(b)) && strcmp(b,"127.0.0.1")!=0){ snprintf(ipaddr,sizeof(ipaddr),"%s",b); break;} } } if(ifap) freeifaddrs(ifap);} 
  long uptime_seconds = get_uptime_seconds();
  char uptime_h[160]=""; format_uptime_linux(uptime_seconds, uptime_h, sizeof(uptime_h));
  char buf[1024]; snprintf(buf,sizeof(buf),"{\"hostname\":\"%s\",\"ip\":\"%s\",\"uptime_linux\":\"%s\"}\n", hostname, ipaddr, uptime_h);
  send_json(r, buf); return 0; }

/* OLSR specific subset: links + neighbors + default_route only */
static int h_status_olsr(http_request_t *r) {
  /* Reuse full builder but skip heavy pieces. For simplicity call h_status then prune would be costly.
   * Instead minimally reproduce needed fields.
   */
  char *buf=NULL; size_t cap=4096,len=0; buf=malloc(cap); if(!buf){ send_json(r,"{}\n"); return 0; } buf[0]=0;
  #define APP2(fmt,...) do { char *_t=NULL; int _n=asprintf(&_t,fmt,##__VA_ARGS__); if(_n<0||!_t){ if(_t) free(_t); free(buf); send_json(r,"{}\n"); return 0;} if(len+_n+1>cap){ while(cap<len+_n+1) cap*=2; char *nb=realloc(buf,cap); if(!nb){ free(_t); free(buf); send_json(r,"{}\n"); return 0;} buf=nb;} memcpy(buf+len,_t,(size_t)_n); len+=_n; buf[len]=0; free(_t);}while(0)
  APP2("{");
  /* hostname/ip */
  char hostname[256]=""; if(gethostname(hostname,sizeof(hostname))==0) hostname[sizeof(hostname)-1]=0; APP2("\"hostname\":"); json_append_escaped(&buf,&len,&cap,hostname); APP2(",");
  char ipaddr[128]=""; struct ifaddrs *ifap=NULL,*ifa=NULL; if(getifaddrs(&ifap)==0){ for(ifa=ifap;ifa;ifa=ifa->ifa_next){ if(ifa->ifa_addr && ifa->ifa_addr->sa_family==AF_INET){ struct sockaddr_in sa; memcpy(&sa,ifa->ifa_addr,sizeof(sa)); char b[INET_ADDRSTRLEN]; if(inet_ntop(AF_INET,&sa.sin_addr,b,sizeof(b)) && strcmp(b,"127.0.0.1")!=0){ snprintf(ipaddr,sizeof(ipaddr),"%s",b); break;} } } if(ifap) freeifaddrs(ifap);} APP2("\"ip\":"); json_append_escaped(&buf,&len,&cap,ipaddr); APP2(",");
  /* default route */
  char def_ip[64]="", def_dev[64]=""; char *rout=NULL; size_t rn=0; if(util_exec("/sbin/ip route show default 2>/dev/null || /usr/sbin/ip route show default 2>/dev/null || ip route show default 2>/dev/null", &rout,&rn)==0 && rout){ char *p=strstr(rout,"via "); if(p){ p+=4; char *q=strchr(p,' '); if(q){ size_t L=q-p; if(L<sizeof(def_ip)){ strncpy(def_ip,p,L); def_ip[L]=0; } } } p=strstr(rout," dev "); if(p){ p+=5; char *q=strchr(p,' '); if(!q) q=strchr(p,'\n'); if(q){ size_t L=q-p; if(L<sizeof(def_dev)){ strncpy(def_dev,p,L); def_dev[L]=0; } } }
    free(rout); }
  APP2("\"default_route\":{"); APP2("\"ip\":"); json_append_escaped(&buf,&len,&cap,def_ip); APP2(",\"dev\":"); json_append_escaped(&buf,&len,&cap,def_dev); APP2("},");
  /* attempt OLSR links minimal (separate flags) */
  int olsr2_on=0, olsrd_on=0; detect_olsr_processes(&olsrd_on,&olsr2_on);
  char *olsr_links_raw=NULL; size_t oln=0; const char *eps[]={"http://127.0.0.1:9090/links","http://127.0.0.1:2006/links","http://127.0.0.1:8123/links",NULL};
  for(const char **ep=eps; *ep && !olsr_links_raw; ++ep){ char cmd[256]; snprintf(cmd,sizeof(cmd),"/usr/bin/curl -s --max-time 1 %s", *ep); if(util_exec(cmd,&olsr_links_raw,&oln)==0 && olsr_links_raw && oln>0) break; if(olsr_links_raw){ free(olsr_links_raw); olsr_links_raw=NULL; oln=0; } }
  /* also get routes & topology to enrich route/node counts like /olsr/links */
  char *routes_raw=NULL; size_t rr=0; util_exec("/usr/bin/curl -s --max-time 1 http://127.0.0.1:9090/routes", &routes_raw,&rr);
  char *topology_raw=NULL; size_t tr=0; util_exec("/usr/bin/curl -s --max-time 1 http://127.0.0.1:9090/topology", &topology_raw,&tr);
  APP2("\"olsr2_on\":%s,", olsr2_on?"true":"false");
  APP2("\"olsrd_on\":%s,", olsrd_on?"true":"false");
  if(olsr_links_raw && oln>0){
    size_t l1=strlen(olsr_links_raw); size_t l2=routes_raw?strlen(routes_raw):0; size_t l3=topology_raw?strlen(topology_raw):0;
    char *combined_raw=malloc(l1+l2+l3+8); if(combined_raw){ size_t off=0; memcpy(combined_raw+off,olsr_links_raw,l1); off+=l1; combined_raw[off++]='\n'; if(l2){ memcpy(combined_raw+off,routes_raw,l2); off+=l2; combined_raw[off++]='\n'; } if(l3){ memcpy(combined_raw+off,topology_raw,l3); off+=l3; } combined_raw[off]=0; char *norm=NULL; size_t nn=0; if(normalize_olsrd_links(combined_raw,&norm,&nn)==0 && norm){ APP2("\"links\":%s", norm); free(norm);} else { APP2("\"links\":[]"); } free(combined_raw);} else { APP2("\"links\":[]"); }
  } else { APP2("\"links\":[]"); }
  APP2("}\n");
  http_send_status(r,200,"OK"); http_printf(r,"Content-Type: application/json; charset=utf-8\r\n\r\n"); http_write(r,buf,len); free(buf); if(olsr_links_raw) free(olsr_links_raw); if(routes_raw) free(routes_raw); if(topology_raw) free(topology_raw); return 0; }

static int h_nodedb(http_request_t *r) {
  /* Always fetch remote node_db */
  fetch_remote_nodedb();
  pthread_mutex_lock(&g_nodedb_lock);
  if (g_nodedb_cached && g_nodedb_cached_len>0) {
    http_send_status(r,200,"OK"); http_printf(r,"Content-Type: application/json; charset=utf-8\r\n\r\n"); http_write(r,g_nodedb_cached,g_nodedb_cached_len); pthread_mutex_unlock(&g_nodedb_lock); return 0; }
  pthread_mutex_unlock(&g_nodedb_lock);
  /* Debug: return error info instead of empty JSON */
  char debug_json[1024];
  char url_copy[256];
  strncpy(url_copy, g_nodedb_url, sizeof(url_copy) - 1);
  url_copy[sizeof(url_copy) - 1] = '\0';
  snprintf(debug_json, sizeof(debug_json), "{\"error\":\"No remote node_db data available\",\"url\":\"%s\",\"last_fetch\":%ld,\"cached_len\":%zu}", url_copy, g_nodedb_last_fetch, g_nodedb_cached_len);
  send_json(r, debug_json); return 0;
}

/* capabilities endpoint */
/* forward-declare globals used by capabilities endpoint (defined later) */
extern int g_is_edgerouter;
extern int g_has_traceroute;

/* capabilities endpoint */
static int h_capabilities_local(http_request_t *r) {
  int airos = path_exists("/tmp/10-all.json");
  int discover = 1; /* Internal discovery always available */
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
  char buf[320]; snprintf(buf, sizeof(buf), "{\"is_edgerouter\":%s,\"is_linux_container\":%s,\"discover\":%s,\"airos\":%s,\"connections\":true,\"traffic\":%s,\"txtinfo\":true,\"jsoninfo\":true,\"traceroute\":%s,\"show_admin_link\":%s}",
    g_is_edgerouter?"true":"false", g_is_linux_container?"true":"false", discover?"true":"false", airos?"true":"false", path_exists("/tmp")?"true":"false", tracer?"true":"false", show_admin?"true":"false");
  send_json(r, buf);
  return 0;
}
/* duplicate include/global block removed */

/* traceroute: run traceroute binary if available and return stdout as plain text */
static int h_traceroute(http_request_t *r) {
  char target[256] = "";
  (void)get_query_param(r, "target", target, sizeof(target));
  char want_json[8] = ""; (void)get_query_param(r, "format", want_json, sizeof(want_json));
  if (!target[0]) { send_text(r, "No target provided\n"); return 0; }
  if (!g_has_traceroute || !g_traceroute_path[0]) { send_text(r, "traceroute not available\n"); return 0; }
  /* build command dynamically to avoid compile-time truncation warnings */
  size_t cmdlen = strlen(g_traceroute_path) + 4 + strlen(target) + 32;
  char *cmd = (char*)malloc(cmdlen);
  if (!cmd) { send_text(r, "error allocating memory\n"); return 0; }
  /* conservative flags: IPv4, numeric, wait 2s, 1 probe per hop, max 8 hops */
  snprintf(cmd, cmdlen, "%s -4 -n -w 2 -q 1 -m 8 %s 2>&1", g_traceroute_path, target);
  char *out = NULL; size_t n = 0;
  if (util_exec(cmd, &out, &n) == 0 && out) {
    if (want_json[0] && (want_json[0]=='j' || want_json[0]=='J')) {
      /* parse traceroute plain text into JSON hops */
      char *dup = strndup(out, n);
      if (!dup) { free(out); free(cmd); send_json(r, "{\"error\":\"oom\"}\n"); return 0; }
      char *saveptr=NULL; char *line=strtok_r(dup, "\n", &saveptr);
      size_t cap=2048,len2=0; char *json=malloc(cap); if(!json){ free(dup); free(out); free(cmd); send_json(r,"{\"error\":\"oom\"}\n"); return 0; } json[0]=0;
      #define APP_TR(fmt,...) do { char *_t=NULL; int _n=asprintf(&_t,fmt,##__VA_ARGS__); if(_n<0||!_t){ if(_t) free(_t); free(json); free(dup); free(out); free(cmd); send_json(r,"{\"error\":\"oom\"}\n"); return 0;} if(len2+(size_t)_n+1>cap){ while(cap<len2+(size_t)_n+1) cap*=2; char *nb=realloc(json,cap); if(!nb){ free(_t); free(json); free(dup); free(out); free(cmd); send_json(r,"{\"error\":\"oom\"}\n"); return 0;} json=nb;} memcpy(json+len2,_t,(size_t)_n); len2+=(size_t)_n; json[len2]=0; free(_t);}while(0)
      APP_TR("{\"target\":"); json_append_escaped(&json,&len2,&cap,target); APP_TR(",\"hops\":["); int first=1;
      while(line){
        /* skip header */
        if (strstr(line, "traceroute to") == line) { line=strtok_r(NULL,"\n",&saveptr); continue; }
        char *trim=line; while(*trim==' '||*trim=='\t') trim++;
        if(!*trim){ line=strtok_r(NULL,"\n",&saveptr); continue; }
        /* capture hop number */
        char *sp = trim; while(*sp && *sp!=' ' && *sp!='\t') sp++; char hopbuf[16]=""; size_t hlen=(size_t)(sp-trim); if(hlen && hlen<sizeof(hopbuf)){ memcpy(hopbuf,trim,hlen); hopbuf[hlen]=0; }
        if(!hopbuf[0] || !isdigit((unsigned char)hopbuf[0])) { line=strtok_r(NULL,"\n",&saveptr); continue; }
        /* extract IP/host and latency numbers */
        char ip[64]=""; char host[128]=""; char *p2=sp; /* rest of line */
        /* attempt parentheses ip */
        char *paren = strchr(p2,'(');
        if(paren){ char *close=strchr(paren,')'); if(close){ size_t ilen=(size_t)(close-(paren+1)); if(ilen && ilen<sizeof(ip)){ memcpy(ip,paren+1,ilen); ip[ilen]=0; } } }
        /* host: token after hop that is not '(' and not numeric ip */
        {
          char tmp[256]; snprintf(tmp,sizeof(tmp),"%s",p2);
          char *toksave=NULL; char *tok=strtok_r(tmp," \t",&toksave); while(tok){ if(tok[0]=='('){ tok=strtok_r(NULL," \t",&toksave); continue; } if(strcmp(tok,"*")==0){ tok=strtok_r(NULL," \t",&toksave); continue; } if(!host[0]){ snprintf(host,sizeof(host),"%s",tok); } tok=strtok_r(NULL," \t",&toksave); }
          /* if host looks like ip and ip empty -> ip=host */
          if(!ip[0] && host[0]){
            int is_ip=1; for(char *c=host; *c; ++c){ if(!isdigit((unsigned char)*c) && *c!='.') { is_ip=0; break; } }
            if(is_ip){
              /* safe bounded copy host(<=128) -> ip(64) */
              size_t host_len_copy = strnlen(host, sizeof(ip)-1);
              memcpy(ip, host, host_len_copy); ip[host_len_copy]=0;
              host[0]=0;
            }
          }
        }
        /* collect all latency samples (numbers followed by ms) */
        double samples[8]; int sc=0; char *scan=p2; while(*scan && sc<8){ while(*scan && !isdigit((unsigned char)*scan) && *scan!='*') scan++; if(*scan=='*'){ scan++; continue; } char *endp=NULL; double val=strtod(scan,&endp); if(endp && val>=0){ while(*endp==' ') endp++; if(strncasecmp(endp,"ms",2)==0){ samples[sc++]=val; scan=endp+2; continue; } } if(endp==scan){ scan++; } else scan=endp; }
        /* build latency string: if multiple, join with '/' */
        char latency[128]=""; if(sc==1) snprintf(latency,sizeof(latency),"%.3gms",samples[0]); else if(sc>1){ size_t off=0; for(int i=0;i<sc;i++){ int w=snprintf(latency+off,sizeof(latency)-off,"%s%.3gms", i?"/":"", samples[i]); if(w<0|| (size_t)w>=sizeof(latency)-off) break; off+=(size_t)w; } }
        if(!first) APP_TR(","); first=0;
        APP_TR("{\"hop\":"); json_append_escaped(&json,&len2,&cap,hopbuf);
        APP_TR(",\"ip\":"); json_append_escaped(&json,&len2,&cap,ip);
        APP_TR(",\"host\":"); json_append_escaped(&json,&len2,&cap,host);
        APP_TR(",\"ping\":"); json_append_escaped(&json,&len2,&cap,latency);
        APP_TR("}");
        line=strtok_r(NULL,"\n",&saveptr);
      }
      APP_TR("]}\n");
      http_send_status(r,200,"OK"); http_printf(r,"Content-Type: application/json; charset=utf-8\r\n\r\n"); http_write(r,json,len2);
      free(json); free(dup); free(out); free(cmd); return 0;
    } else {
      http_send_status(r, 200, "OK");
      http_printf(r, "Content-Type: text/plain; charset=utf-8\r\n\r\n");
      http_write(r, out, n);
      free(out);
      free(cmd);
      return 0;
    }
  }
  free(cmd);
  /* try ICMP-based traceroute as a fallback */
  {
    size_t cmdlen2 = strlen(g_traceroute_path) + 8 + strlen(target) + 32;
    char *cmd2 = malloc(cmdlen2);
    if (cmd2) {
  snprintf(cmd2, cmdlen2, "%s -I -n -w 2 -q 1 -m 8 %s 2>&1", g_traceroute_path, target);
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
  if (http_send_file(r, g_asset_root, "js/app.js", NULL) != 0) {
    /* Fallback: serve minimal JavaScript for debugging */
    http_send_status(r, 200, "OK");
    http_printf(r, "Content-Type: application/javascript; charset=utf-8\r\n\r\n");
    http_printf(r, "console.log('OLSR Status Plugin - app.js fallback loaded');\n");
    http_printf(r, "window.addEventListener('load', function() {\n");
    http_printf(r, "  console.log('Page loaded, fetching status...');\n");
    http_printf(r, "  fetch('/status').then(r => r.json()).then(data => {\n");
    http_printf(r, "    console.log('Status data:', data);\n");
    http_printf(r, "    document.body.innerHTML += '<pre>' + JSON.stringify(data, null, 2) + '</pre>';\n");
    http_printf(r, "  }).catch(e => console.error('Error fetching status:', e));\n");
    http_printf(r, "});\n");
  }
  return 0;
}

/* Simple proxy for selected OLSRd JSON queries (whitelist q values). */
static int h_olsrd_json(http_request_t *r) {
  char q[64] = "links";
  (void)get_query_param(r, "q", q, sizeof(q));
  /* whitelist */
  const char *allowed[] = { "version","all","runtime","startup","neighbors","links","routes","hna","mid","topology","interfaces","2hop","sgw","pudposition","plugins","neighbours","gateways", NULL };
  int ok = 0;
  for (const char **p = allowed; *p; ++p) if (strcmp(*p, q) == 0) { ok = 1; break; }
  if (!ok) { send_json(r, "{}\n"); return 0; }
  /* consult in-memory cache first */
  char cached[256] = "";
  if (olsr_cache_get(q, cached, sizeof(cached))) { send_json(r, cached); return 0; }
  char cmd[512]; snprintf(cmd, sizeof(cmd), "/usr/bin/curl -s --max-time 1 http://127.0.0.1:9090/%s", q);
  char *out = NULL; size_t n = 0;
  if (util_exec(cmd, &out, &n) == 0 && out && n>0) {
    /* cache truncated to OLSR_CACHE_VAL-1 */
    olsr_cache_set(q, out);
    send_json(r, out); free(out); return 0;
  }
  send_json(r, "{}\n"); return 0;
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

/* Simple TTL cache for hostname lookups & nodedb lookups to avoid repeated blocking I/O
 * Very small fixed-size cache with linear probing; entries expire after CACHE_TTL seconds.
 */
#define CACHE_SIZE 128
#define CACHE_TTL 60
struct kv_cache_entry { char key[128]; char val[256]; time_t ts; };
static struct kv_cache_entry g_host_cache[CACHE_SIZE];
/* separate cache for OLSRd proxy responses; uses same slot size as kv_cache_entry */
static struct kv_cache_entry g_olsr_cache[CACHE_SIZE];

static int olsr_cache_get(const char *key, char *out, size_t outlen) {
  if (!key || !out) return 0;
  unsigned long h = 1469598103934665603UL;
  for (const unsigned char *p = (const unsigned char*)key; *p; ++p) h = (h ^ *p) * 1099511628211UL;
  int idx = (int)(h % CACHE_SIZE);
  if (g_olsr_cache[idx].key[0] == 0) return 0;
  if (strcmp(g_olsr_cache[idx].key, key) != 0) return 0;
  if (difftime(time(NULL), g_olsr_cache[idx].ts) > CACHE_TTL) return 0;
  /* copy up to outlen-1 chars */
  snprintf(out, outlen, "%s", g_olsr_cache[idx].val);
  return 1;
}

static void olsr_cache_set(const char *key, const char *val) {
  if (!key || !val) return;
  unsigned long h = 1469598103934665603UL;
  for (const unsigned char *p = (const unsigned char*)key; *p; ++p) h = (h ^ *p) * 1099511628211UL;
  int idx = (int)(h % CACHE_SIZE);
  snprintf(g_olsr_cache[idx].key, sizeof(g_olsr_cache[idx].key), "%s", key);
  /* store truncated value (safe) */
  snprintf(g_olsr_cache[idx].val, sizeof(g_olsr_cache[idx].val), "%s", val);
  g_olsr_cache[idx].ts = time(NULL);
}

static void cache_set(struct kv_cache_entry *cache, const char *key, const char *val) {
  if (!key || !val) return;
  unsigned long h = 1469598103934665603UL;
  for (const unsigned char *p = (const unsigned char*)key; *p; ++p) h = (h ^ *p) * 1099511628211UL;
  int idx = (int)(h % CACHE_SIZE);
  snprintf(cache[idx].key, sizeof(cache[idx].key), "%s", key);
  snprintf(cache[idx].val, sizeof(cache[idx].val), "%s", val);
  cache[idx].ts = time(NULL);
}

static int cache_get(struct kv_cache_entry *cache, const char *key, char *out, size_t outlen) {
  if (!key || !out) return 0;
  unsigned long h = 1469598103934665603UL;
  for (const unsigned char *p = (const unsigned char*)key; *p; ++p) h = (h ^ *p) * 1099511628211UL;
  int idx = (int)(h % CACHE_SIZE);
  if (cache[idx].key[0] == 0) return 0;
  if (strcmp(cache[idx].key, key) != 0) return 0;
  if (difftime(time(NULL), cache[idx].ts) > CACHE_TTL) return 0;
  snprintf(out, outlen, "%s", cache[idx].val);
  return 1;
}

/* lookup hostname for an ipv4 string using cache, gethostbyaddr and nodedb files/remote as fallback */
static void lookup_hostname_cached(const char *ipv4, char *out, size_t outlen) {
  if (!ipv4 || !out) return;
  out[0]=0;
  if (cache_get(g_host_cache, ipv4, out, outlen)) return;
  /* try reverse DNS */
  struct in_addr ina; if (inet_aton(ipv4, &ina)) {
    struct hostent *he = gethostbyaddr(&ina, sizeof(ina), AF_INET);
    if (he && he->h_name) {
      snprintf(out, outlen, "%s", he->h_name);
      cache_set(g_host_cache, ipv4, out);
      return;
    }
  }
  /* try cached remote node_db first */
  fetch_remote_nodedb();
  if (g_nodedb_cached && g_nodedb_cached_len > 0) {
    char needle[256]; 
    if (snprintf(needle, sizeof(needle), "\"%s\":", ipv4) >= (int)sizeof(needle)) {
      /* IP address too long, skip */
      goto nothing_found;
    }
    char *pos = strstr(g_nodedb_cached, needle);
    if (pos) {
      char *hpos = strstr(pos, "\"hostname\":");
      if (hpos && find_json_string_value(hpos, "hostname", &pos, &g_nodedb_cached_len)) {
        size_t copy = g_nodedb_cached_len < outlen-1 ? g_nodedb_cached_len : outlen-1; memcpy(out, pos, copy); out[copy]=0; cache_set(g_host_cache, ipv4, out); return;
      }
      /* fallback to "n" */
      char *npos = strstr(pos, "\"n\":");
      if (npos && find_json_string_value(npos, "n", &pos, &g_nodedb_cached_len)) {
        size_t copy = g_nodedb_cached_len < outlen-1 ? g_nodedb_cached_len : outlen-1; memcpy(out, pos, copy); out[copy]=0; cache_set(g_host_cache, ipv4, out); return;
      }
    }
  }
  /* nothing found */
nothing_found:
  out[0]=0;
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
  { .name = "nodedb_url", .set_plugin_parameter = &set_str_param, .data = g_nodedb_url,  .addon = {0} },
  { .name = "nodedb_ttl", .set_plugin_parameter = &set_int_param, .data = &g_nodedb_ttl, .addon = {0} },
};

void olsrd_get_plugin_parameters(const struct olsrd_plugin_parameters **params, int *size) {
  *params = g_params;
  *size = (int)(sizeof(g_params)/sizeof(g_params[0]));
}

int olsrd_plugin_init(void) {
  log_asset_permissions();
  /* detect availability of optional external tools without failing startup */
  const char *tracer_candidates[] = { "/usr/sbin/traceroute", "/bin/traceroute", "/usr/bin/traceroute", "/usr/local/bin/traceroute", NULL };
  const char *olsrd_candidates[] = { "/usr/sbin/olsrd", "/usr/bin/olsrd", "/sbin/olsrd", NULL };
  for (const char **p = tracer_candidates; *p; ++p) { if (path_exists(*p)) { g_has_traceroute = 1; snprintf(g_traceroute_path, sizeof(g_traceroute_path), "%s", *p); break; } }
  for (const char **p = olsrd_candidates; *p; ++p) { if (path_exists(*p)) { snprintf(g_olsrd_path, sizeof(g_olsrd_path), "%s", *p); break; } }
  g_is_edgerouter = env_is_edgerouter();
  g_is_linux_container = env_is_linux_container();
  
  fprintf(stderr, "[status-plugin] environment detection: edgerouter=%s, linux_container=%s\n", 
          g_is_edgerouter ? "yes" : "no", g_is_linux_container ? "yes" : "no");

  /* Try to detect local www directory for development */
  if (!path_exists(g_asset_root) || !path_exists(g_asset_root)) {
    char local_www[512];
    snprintf(local_www, sizeof(local_www), "./www");
    if (path_exists(local_www)) {
      fprintf(stderr, "[status-plugin] using local www directory: %s\n", local_www);
      snprintf(g_asset_root, sizeof(g_asset_root), "%s", local_www);
    } else {
      fprintf(stderr, "[status-plugin] warning: asset root %s not found, web interface may not work\n", g_asset_root);
    }
  }

  if (http_server_start(g_bind, g_port, g_asset_root) != 0) {
    fprintf(stderr, "[status-plugin] failed to start http server on %s:%d\n", g_bind, g_port);
    return 1;
  }
  http_server_register_handler("/",         &h_root);
  http_server_register_handler("/index.html", &h_root);
  http_server_register_handler("/ipv4",     &h_ipv4);
  http_server_register_handler("/ipv6",     &h_ipv6);
  http_server_register_handler("/status",   &h_status);
  http_server_register_handler("/status/summary", &h_status_summary);
  http_server_register_handler("/status/olsr", &h_status_olsr);
  http_server_register_handler("/status/lite", &h_status_lite);
  http_server_register_handler("/olsr/links", &h_olsr_links);
  http_server_register_handler("/olsr/links_debug", &h_olsr_links_debug);
  http_server_register_handler("/olsr/routes", &h_olsr_routes);
  http_server_register_handler("/olsr/raw", &h_olsr_raw); /* debug */
  http_server_register_handler("/olsrd.json", &h_olsrd_json);
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
  const char *cmd = "/sbin/ip -4 a 2>/dev/null || /usr/sbin/ip -4 a 2>/dev/null || ip -4 a 2>/dev/null && echo && /sbin/ip -4 neigh 2>/dev/null || /usr/sbin/ip -4 neigh 2>/dev/null || ip -4 neigh 2>/dev/null && echo && /usr/sbin/brctl show 2>/dev/null || /sbin/brctl show 2>/dev/null || brctl show 2>/dev/null";
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
  const char *cmd = "/sbin/ip -6 a 2>/dev/null || /usr/sbin/ip -6 a 2>/dev/null || ip -6 a 2>/dev/null && echo && /sbin/ip -6 neigh 2>/dev/null || /usr/sbin/ip -6 neigh 2>/dev/null || ip -6 neigh 2>/dev/null && echo && /usr/sbin/brctl show 2>/dev/null || /sbin/brctl show 2>/dev/null || brctl show 2>/dev/null";
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
  char *out=NULL; size_t n=0;
  if (ubnt_discover_output(&out, &n) == 0 && out && n > 0) {
    send_json(r, out);
    free(out);
  } else {
    send_json(r, "{}");
  }
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
  // Try Linux container paths
  if (util_exec("/usr/share/olsrd-status-plugin/connections.sh", &out, &n) == 0 && out && n > 0) {
    http_send_status(r, 200, "OK");
    http_printf(r, "Content-Type: text/plain; charset=utf-8\r\n\r\n");
    http_write(r, out, n);
    free(out);
    return 0;
  }
  if (util_exec("./connections.sh", &out, &n) == 0 && out && n > 0) {
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

static int h_connections_json(http_request_t *r) {
  char *out=NULL; size_t n=0;
  if (render_connections_json(&out,&n)==0 && out && n>0) {
    http_send_status(r,200,"OK");
    http_printf(r,"Content-Type: application/json; charset=utf-8\r\n\r\n");
    http_write(r,out,n);
    free(out); return 0;
  }
  /* fallback: synthesize empty structure */
  send_json(r,"{\"ports\":[]}\n");
  if(out) free(out);
  return 0;
}

static int h_versions_json(http_request_t *r) {
  char *out=NULL; size_t n=0; int ok=0;
  if(path_exists("/config/custom/versions.sh")) {
    if(util_exec("/config/custom/versions.sh", &out,&n)==0 && out && buffer_has_content(out,n)) ok=1;
  }
  if(!ok && out){ free(out); out=NULL; n=0; }
  if(!ok && path_exists("/usr/share/olsrd-status-plugin/versions.sh")) {
    if(util_exec("/usr/share/olsrd-status-plugin/versions.sh", &out,&n)==0 && out && buffer_has_content(out,n)) ok=1;
  }
  if(!ok && out){ free(out); out=NULL; n=0; }
  if(!ok && path_exists("./versions.sh")) {
    if(util_exec("./versions.sh", &out,&n)==0 && out && buffer_has_content(out,n)) ok=1;
  }
  if(ok) {
    http_send_status(r,200,"OK");
    http_printf(r,"Content-Type: application/json; charset=utf-8\r\n\r\n");
    http_write(r,out,n); free(out); return 0;
  }
  /* fallback minimal */
  char host[256]=""; gethostname(host,sizeof(host)); host[sizeof(host)-1]=0;
  char buf[512]; snprintf(buf,sizeof(buf),"{\"olsrd_status_plugin\":\"%s\",\"host\":\"%s\"}\n","1.0",host);
  send_json(r,buf);
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
