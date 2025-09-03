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

/* forward decls for local helpers used before their definitions */
static void send_text(http_request_t *r, const char *text);
static void send_json(http_request_t *r, const char *json);
static int get_query_param(http_request_t *r, const char *key, char *out, size_t outlen);

extern int render_connections_plain(char **buf_out, size_t *len_out);
/* plugin-global flags (declared here for use in early functions) */
extern int g_is_edgerouter;
extern int g_has_ubnt_discover;
extern int g_has_traceroute;
#define PATHLEN 256
extern char g_ubnt_discover_path[PATHLEN];
extern char g_traceroute_path[PATHLEN];
extern char g_olsrd_path[PATHLEN];

static int h_connections_json(http_request_t *r) {
  char *buf = NULL; size_t len = 0;
  if (render_connections_plain(&buf, &len) != 0 || !buf) {
    send_json(r, "{\"connections\":[]}");
    return 0;
  }
  fprintf(stderr, "[h_connections_json] render_connections_plain returned len=%zu buf=%p\n", len, (void*)buf);

  // parse plain output
  typedef struct {
    char port[64];
    char bridge[64];
    char macs[8][64]; int macs_n;
    char ips[8][64]; int ips_n;
  } port_t;
  const int ports_cap = 512;
  port_t *ports = (port_t*)malloc(sizeof(port_t) * ports_cap);
  if (!ports) { if (buf) free(buf); send_json(r, "{\"connections\":[]}"); return 0; }
  int ports_n = 0;

  char *line;
  // duplicate buffer for strtok
  if (len == 0) {
    free(buf);
    send_json(r, "{\"connections\":[]}");
    return 0;
  }
  char *dup = (char*)malloc(len+1);
  if (!dup) { fprintf(stderr, "[h_connections_json] malloc(dup) failed\n"); free(buf); send_json(r, "{\"connections\":[]}"); return 0; }
  memcpy(dup, buf, len); dup[len]=0;
  line = strtok(dup, "\n");
  int line_no = 0;
  while (line) {
    line_no++;
    while (*line==' ' || *line=='\t') line++;
    fprintf(stderr, "[h_connections_json] parsing line %d: '%s'\n", line_no, line[0] ? line : "<empty>");
    if (line[0]=='#') { line = strtok(NULL, "\n"); continue; }
    if (strncmp(line, ". ", 2)==0) {
      // . bridge\tport\tmac
  char br[64]={0}, pt[64]={0}, mac[64]={0};
      // tokenize by whitespace after prefix
      char *p = line+2;
      // read bridge
  int rc = sscanf(p, "%63s %63s %63s", br, pt, mac);
  if (rc < 2) { fprintf(stderr, "[h_connections_json] malformed . line: '%s'\n", p); line = strtok(NULL, "\n"); continue; }
      if (rc >= 2) {
        // find or add port entry for pt
        int idx=-1;
        for(int i=0;i<ports_n;i++) if (strcmp(ports[i].port, pt)==0) { idx=i; break; }
  if (idx==-1 && ports_n < ports_cap) { idx = ports_n++; snprintf(ports[idx].port, sizeof(ports[idx].port), "%s", pt); ports[idx].macs_n=0; ports[idx].ips_n=0; ports[idx].bridge[0]=0; }
        if (idx!=-1) {
          if (br[0]) snprintf(ports[idx].bridge, sizeof(ports[idx].bridge), "%s", br);
          if (mac[0] && ports[idx].macs_n < 8) snprintf(ports[idx].macs[ports[idx].macs_n++], sizeof(ports[idx].macs[0]), "%s", mac);
        }
      }
    } else if (strncmp(line, "~ ", 2)==0) {
      // ~ port\tmac\tip
  char pt[64]={0}, mac[64]={0}, ip[64]={0};
      char *p = line+2;
  int rc = sscanf(p, "%63s %63s %63s", pt, mac, ip);
  if (rc < 2) { fprintf(stderr, "[h_connections_json] malformed ~ line: '%s'\n", p); line = strtok(NULL, "\n"); continue; }
  if (rc >= 2) {
        int idx=-1;
        for(int i=0;i<ports_n;i++) if (strcmp(ports[i].port, pt)==0) { idx=i; break; }
  if (idx==-1 && ports_n < ports_cap) { idx = ports_n++; snprintf(ports[idx].port, sizeof(ports[idx].port), "%s", pt); ports[idx].macs_n=0; ports[idx].ips_n=0; ports[idx].bridge[0]=0; }
        if (idx!=-1) {
          if (mac[0] && ports[idx].macs_n < 8) snprintf(ports[idx].macs[ports[idx].macs_n++], sizeof(ports[idx].macs[0]), "%s", mac);
          if (ip[0] && ports[idx].ips_n < 8) snprintf(ports[idx].ips[ports[idx].ips_n++], sizeof(ports[idx].ips[0]), "%s", ip);
        }
      }
    }
    line = strtok(NULL, "\n");
  }

  // build JSON
  size_t cap = 4096; char *out = (char*)malloc(cap); size_t outlen=0;
  if (!out) { fprintf(stderr, "[h_connections_json] malloc(out) failed\n"); free(dup); if (buf) free(buf); send_json(r, "{\"connections\":[]}"); return 0; }
  #define APPEND_OUT(fmt,...) do{ char t[1024]; int n = snprintf(t,sizeof(t),fmt,##__VA_ARGS__); if(n<0) n=0; if(n>=(int)sizeof(t)) n = (int)sizeof(t)-1; if(n>0){ if(outlen + n + 1 > cap){ while (cap < outlen + n + 1) cap *= 2; char *tmp = (char*)realloc(out, cap); if(!tmp){ fprintf(stderr,"[h_connections_json] realloc failed\n"); free(out); free(dup); if(buf) free(buf); send_json(r,"{\"connections\":[]}"); return 0; } out = tmp; } memcpy(out+outlen, t, n); outlen += n; out[outlen]=0; } } while(0)

  APPEND_OUT("{\"ports\":[");
  for(int i=0;i<ports_n;i++){
    if (i) APPEND_OUT(",");
    APPEND_OUT("{\"port\":\"%s\"", ports[i].port);
    if (ports[i].bridge[0]) APPEND_OUT(",\"bridge\":\"%s\"", ports[i].bridge);
    if (ports[i].macs_n>0){ APPEND_OUT(",\"macs\":["); for(int j=0;j<ports[i].macs_n;j++){ if(j) APPEND_OUT(","); APPEND_OUT("\"%s\"", ports[i].macs[j]); } APPEND_OUT("]"); }
    if (ports[i].ips_n>0){ APPEND_OUT(",\"ips\":["); for(int j=0;j<ports[i].ips_n;j++){ if(j) APPEND_OUT(","); APPEND_OUT("\"%s\"", ports[i].ips[j]); } APPEND_OUT("]"); }
    APPEND_OUT("}");
  }
  APPEND_OUT("]}");

  free(dup);
  if (buf) free(buf);
  if (ports) free(ports);
  http_send_status(r, 200, "OK");
  http_printf(r, "Content-Type: application/json; charset=utf-8\r\n\r\n");
  http_write(r, out, outlen);
  free(out);
  return 0;
}

static int h_versions_json(http_request_t *r) {
  // Native implementation: build JSON object
  char *json = NULL; size_t cap = 8192; json = (char*)malloc(cap); size_t len = 0;
  #define APPEND_JSON(fmt,...) do{ int _aj_n = snprintf(NULL,0,fmt,##__VA_ARGS__); if(len + _aj_n + 2 > cap){ cap = (len + _aj_n + 2) * 2; json = (char*)realloc(json, cap); } len += snprintf(json+len, cap-len, fmt, ##__VA_ARGS__); } while(0)

  APPEND_JSON("{");
  // wizards
  APPEND_JSON("\"wizards\":{");
  int firstw = 1;
  const char *wkdir = "/config/wizard/feature";
  DIR *d = opendir(wkdir);
  if (d) {
    struct dirent *de;
    while ((de = readdir(d))) {
      if (de->d_name[0]=='.') continue;
      char path[512]; snprintf(path, sizeof(path), "%s/%s/wizard-run", wkdir, de->d_name);
      FILE *f = fopen(path, "r");
      if (!f) continue;
      char line[256]; int lines = 0; char vers[128] = "";
      while (fgets(line, sizeof(line), f) && lines < 20) {
        lines++;
        char *p = strstr(line, "version");
        if (p) {
          // grab token after 'version'
          char *tok = strchr(line, ' ');
          if (tok) {
            while(*tok && isspace((unsigned char)*tok)) tok++;
            strncpy(vers, tok, sizeof(vers)-1); vers[sizeof(vers)-1]=0;
            // trim
            char *nl = strchr(vers, '\n'); if (nl) *nl=0;
            break;
          }
        }
      }
      fclose(f);
      if (vers[0]) {
        if (!firstw) APPEND_JSON(",");
        APPEND_JSON("\"%s\":\"%s\"", de->d_name, vers);
        firstw = 0;
      }
    }
    closedir(d);
  }
  if (firstw) { APPEND_JSON("\"olsrv1\":\"n/a\""); }
  APPEND_JSON("},");

  // local_ips (ipv4)
  char *out=NULL; size_t n=0;
  char ipv4[128] = "n/a";
  if (util_exec("/sbin/ip -4 -o addr show", &out, &n)==0 && out) {
    // parse output like: "<idx>: <if>    <family> <addr>/<mask> ..."
    char *p = out;
    char *ln = strtok(p, "\n");
    while (ln) {
      char addrfield[128] = "";
      if (sscanf(ln, "%*s %*s %*s %127s", addrfield) == 1) {
        char *slash = strchr(addrfield, '/'); if (slash) *slash = '\0';
        if (addrfield[0] && strncmp(addrfield, "127.", 4) != 0) { snprintf(ipv4, sizeof(ipv4), "%s", addrfield); break; }
      }
      ln = strtok(NULL, "\n");
    }
    free(out); out=NULL; n=0;
  }
  APPEND_JSON("\"local_ips\":{\"ipv4\":\"%s\",\"ipv6\":\"n/a\",\"originator\":\"n/a\"},", ipv4);

  // autoupdate: check files and autoupdate.dat
  int autoinstalled = 0;
  struct stat st; if (stat("/etc/cron.daily/autoupdatewizards", &st)==0) autoinstalled = 1;
  int aa_on=0, aa1_on=0, aa2_on=0, aale_on=0, aaebt_on=0, aabp_on=0;
  FILE *af = fopen("/config/user-data/autoupdate.dat","r");
  if (af) {
    char L[256]; while (fgets(L,sizeof(L),af)){
      if (strstr(L,"wizard-autoupdate=yes")) aa_on=1;
      if (strstr(L,"wizard-olsrd_v1=yes")) aa1_on=1;
      if (strstr(L,"wizard-olsrd_v2=yes")) aa2_on=1;
      if (strstr(L,"wizard-0xffwsle=yes")) aale_on=1;
      if (strstr(L,"wizard-ebtables=yes")) aaebt_on=1;
      if (strstr(L,"wizard-blockPrivate=yes")) aabp_on=1;
    } fclose(af);
  }
  APPEND_JSON("\"autoupdate\":{\"installed\":\"%s\",\"enabled\":\"%s\",\"aa\":\"%s\",\"olsrv1\":\"%s\",\"olsrv2\":\"%s\",\"0xffwsle\":\"%s\",\"ebtables\":\"%s\",\"blockpriv\":\"%s\"},",
    autoinstalled?"yes":"no", aa_on?"on":"off", aa_on?"on":"off", aa1_on?"on":"off", aa2_on?"on":"off", aale_on?"on":"off", aaebt_on?"on":"off", aabp_on?"on":"off");

  // olsrd4watchdog
  int watchdog_on = 0;
  FILE *cf = fopen("/config/user-data/olsrd4.conf","r");
  if (cf) {
    char L[256]; while (fgets(L,sizeof(L),cf)){
      if (strstr(L,"LoadPlugin") && strstr(L,"olsrd_watchdog") && L[0] != '#') { watchdog_on = 1; break; }
    } fclose(cf);
  }
  APPEND_JSON("\"olsrd4watchdog\":{\"state\":\"%s\"},", watchdog_on?"on":"off");

  // linklocals: attempt to build map by reading ip -6 link
  char *llbuf=NULL; if (util_exec("ip -6 link show", &llbuf, &n)==0 && llbuf) {
    APPEND_JSON("\"linklocals\":{");
    char *p = llbuf; char *ln = strtok(p, "\n"); int first=1;
    while (ln) {
      if (strstr(ln, "link/ether")) {
        char ifn[64] = "", mac[64] = "";
        if (sscanf(ln, "%*d: %63[^:]: %*s link/ether %63s", ifn, mac) >= 1) {
          if (mac[0]) {
            for(char *c=mac;*c;c++) *c = toupper((unsigned char)*c);
            if (!first) APPEND_JSON(","); APPEND_JSON("\"%s\":\"%s\"", ifn, mac); first=0;
          }
        }
      }
      ln = strtok(NULL, "\n");
    }
    APPEND_JSON("},"); free(llbuf); llbuf=NULL; n=0;
  } else {
    APPEND_JSON("\"linklocals\":{},");
  }

  // homes and bootimage placeholder
  APPEND_JSON("\"homes\":[\"funkfeuer\"],");
  APPEND_JSON("\"bootimage\":{\"md5\":\"n/a\"}");

  APPEND_JSON("}");

  http_send_status(r, 200, "OK");
  http_printf(r, "Content-Type: application/json; charset=utf-8\r\n\r\n");
  http_write(r, json, len);
  free(json);
  return 0;
}

static int h_nodedb(http_request_t *r) {
  char *out = NULL; size_t n = 0;
  const char *curl_candidates[] = { "/usr/bin/curl", "/usr/sbin/curl", "/bin/curl", "/usr/local/bin/curl", NULL };
  const char *url_candidates[] = { "https://ff.cybercomm.at/node_db.json", "http://ff.cybercomm.at/node_db.json", NULL };
  int success = 0;
  char cmd[1024];
  for (const char **cp = curl_candidates; !success && *cp; ++cp) {
    if (!path_exists(*cp)) continue;
    for (const char **up = url_candidates; !success && *up; ++up) {
      snprintf(cmd, sizeof(cmd), "/bin/sh -c '%s -s --connect-timeout 3 %s 2>&1'", *cp, *up);
      if (util_exec(cmd, &out, &n) == 0 && out && n>0) {
        /* validate output looks like JSON (object or array) */
        size_t i=0; while (i < n && isspace((unsigned char)out[i])) i++;
        if (i < n && (out[i] == '{' || out[i] == '[')) {
          send_json(r, out);
          free(out);
          success = 1;
          break;
        } else {
          /* not JSON: log and continue trying */
          fprintf(stderr, "[h_nodedb] candidate %s %s returned non-json output (len=%zu)\n", *cp, *up, n);
          free(out); out = NULL; n = 0;
        }
      } else {
        /* no output, log and continue */
        fprintf(stderr, "[h_nodedb] candidate %s failed to fetch %s\n", *cp, *up);
        if (out) { free(out); out = NULL; n = 0; }
      }
    }
  }
  if (!success) {
    /* fallback: attempt to build a small nodedb from /etc/hosts to help local mapping */
    FILE *fh = fopen("/etc/hosts", "r");
    if (fh) {
      char line[1024]; char buf[8192]; size_t bl = 0; buf[0]=0;
      bl += snprintf(buf+bl, sizeof(buf)-bl, "{");
      int first = 1;
      while (fgets(line, sizeof(line), fh)) {
        if (line[0]=='#') continue;
        char ip[128]={0}, name[256]={0};
        if (sscanf(line, "%127s %255s", ip, name) == 2) {
          if (!first) bl += snprintf(buf+bl, sizeof(buf)-bl, ",");
          bl += snprintf(buf+bl, sizeof(buf)-bl, "\"%s\":{\"name\":\"%s\"}", ip, name);
          first = 0;
        }
      }
      bl += snprintf(buf+bl, sizeof(buf)-bl, "}");
      fclose(fh);
      fprintf(stderr, "[h_nodedb] using /etc/hosts fallback (entries=%d)\n", first?0:1);
      send_json(r, buf);
    } else {
      /* fallback: empty object if remote fetch fails */
      fprintf(stderr, "[h_nodedb] remote fetch failed, /etc/hosts missing; returning {}\n");
      send_json(r, "{}");
    }
  }
  return 0;
}

static int h_traceroute(http_request_t *r) {
  char target[256] = "78.41.115.36"; // default
  (void)get_query_param(r, "target", target, sizeof(target));
  
  // basic sanitization: allow only alnum, dot, dash, underscore, colon
  char clean[256]; size_t ci=0;
  for(size_t i=0; target[i] && ci+1<sizeof(clean); i++){
    char c = target[i];
    if ((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='.'||c=='-'||c=='_'||c==':') { clean[ci++]=c; }
    else { /* reject on unexpected char */ }
  }
  clean[ci]=0;
  if (ci==0) { send_text(r, "invalid target\n"); return 0; }
  char cmd[1024];
  if (!g_has_traceroute || !g_traceroute_path[0]) { send_text(r, "traceroute not available on this system\n"); return 0; }
  if (strchr(clean, ':')) snprintf(cmd, sizeof(cmd), "%s -n -w 1 -q 1 -6 %s", g_traceroute_path, clean);
  else snprintf(cmd, sizeof(cmd), "%s -n -w 1 -q 1 %s", g_traceroute_path, clean);
  char *out = NULL; size_t n = 0;
  if (util_exec(cmd, &out, &n)==0 && out) {
    http_send_status(r, 200, "OK");
    http_printf(r, "Content-Type: text/plain; charset=utf-8\r\n\r\n");
    http_write(r, out, n); free(out);
  } else send_text(r, "traceroute failed\n");
  return 0;
}

/* status endpoint for frontend: lightweight JSON */
static int h_status(http_request_t *r) {
  char hostname[256] = "";
  if (gethostname(hostname, sizeof(hostname)) != 0) snprintf(hostname, sizeof(hostname), "unknown");
  char *out = NULL; size_t n = 0;
  char ipbuf[128] = "";
    if (util_exec("/sbin/ip -4 -o addr show", &out, &n)==0 && out) {
      char *p = out; char *ln = strtok(p, "\n");
      while (ln) {
        char addrfield[128] = "";
        if (sscanf(ln, "%*s %*s %*s %127s", addrfield) == 1) {
          char *slash = strchr(addrfield, '/'); if (slash) *slash='\0';
          if (addrfield[0] && strncmp(addrfield, "127.", 4) != 0) { snprintf(ipbuf, sizeof(ipbuf), "%s", addrfield); break; }
        }
        ln = strtok(NULL, "\n");
      }
      free(out); out=NULL; n=0;
    } else strcpy(ipbuf, "");
  char uptimebuf[128] = "";
  if (util_read_file("/proc/uptime", &out, &n)==0 && out && n>0) {
    /* parse first token (seconds) */
    char tmp[128] = {0}; size_t L = n < sizeof(tmp)-1 ? n : sizeof(tmp)-1; memcpy(tmp, out, L); tmp[L]=0;
    char *tok = strtok(tmp, " \t\n");
    if (tok) {
      double sec = atof(tok); int days = (int)(sec/86400); int hrs = ((int)sec%86400)/3600; snprintf(uptimebuf,sizeof(uptimebuf), "%dd %dh", days, hrs);
    }
    free(out); out=NULL; n=0;
  }
  if (uptimebuf[0] == '\0') {
#if defined(__linux__)
    struct sysinfo si;
    if (sysinfo(&si) == 0) {
      long sec = si.uptime;
      int days = (int)(sec/86400); int hrs = ((int)sec%86400)/3600;
      snprintf(uptimebuf, sizeof(uptimebuf), "%dd %dh", days, hrs);
    } else {
      snprintf(uptimebuf, sizeof(uptimebuf), "n/a");
    }
#else
    snprintf(uptimebuf, sizeof(uptimebuf), "n/a");
#endif
  }
  // collect OLSR IPv4 links (if available)
  char *linksbuf = NULL; size_t linksn = 0;
  int links_count = 0;
  char links_json[4096]; size_t lpos = 0; links_json[0]=0;
  /* build gatewaylist/nodelist from routing table (similar to Python reference)
   * We'll count unique destinations per gateway so the UI can show a routes count.
   */
  typedef struct gw_entry_s { char gw[64]; char **dests; int dest_n; int dest_cap; int nodes_n; } gw_entry_t;
  const int GW_CAP = 128;
  gw_entry_t *gw = (gw_entry_t*)calloc(GW_CAP, sizeof(gw_entry_t));
  int gw_n = 0;
  char *rout_out = NULL; size_t rout_n = 0;
  if (util_exec("/sbin/ip -4 r | grep -vE 'scope|default' | awk '{print $3,$1,$5}'", &rout_out, &rout_n) == 0 && rout_out && rout_n>0) {
    char *rp = rout_out; char *rln = strtok(rp, "\n");
    while (rln) {
      char gwip[64] = "", dest[64] = "";
      if (sscanf(rln, "%63s %63s", gwip, dest) >= 1) {
        if (gwip[0] && dest[0]) {
          int idx = -1;
          for (int i=0;i<gw_n;i++) if (strcmp(gw[i].gw, gwip)==0) { idx = i; break; }
          if (idx == -1 && gw_n < GW_CAP) {
            idx = gw_n++;
            snprintf(gw[idx].gw, sizeof(gw[idx].gw), "%s", gwip);
            gw[idx].dests = NULL; gw[idx].dest_n = 0; gw[idx].dest_cap = 0; gw[idx].nodes_n = 0;
          }
          if (idx != -1) {
            /* ensure unique dests */
            int found = 0;
            for (int j=0;j<gw[idx].dest_n;j++) if (strcmp(gw[idx].dests[j], dest)==0) { found = 1; break; }
            if (!found) {
              if (gw[idx].dest_n + 1 > gw[idx].dest_cap) {
                int newcap = gw[idx].dest_cap ? gw[idx].dest_cap * 2 : 4;
                char **tmp = (char**)realloc(gw[idx].dests, sizeof(char*) * newcap);
                if (tmp) { gw[idx].dests = tmp; gw[idx].dest_cap = newcap; }
              }
              if (gw[idx].dests && gw[idx].dest_n < gw[idx].dest_cap) {
                gw[idx].dests[gw[idx].dest_n] = strdup(dest);
                if (gw[idx].dests[gw[idx].dest_n]) gw[idx].dest_n++;
              }
            }
          }
        }
      }
      rln = strtok(NULL, "\n");
    }
    free(rout_out); rout_out = NULL; rout_n = 0;
  }
  /* try multiple endpoints for the olsrd control http interface; some systems bind to the host IP */
  const char *link_candidates[] = { "http://127.0.0.1:2006/lin", "http://localhost:2006/lin", NULL };
  char dyn_url[256] = "";
  if (ipbuf[0]) snprintf(dyn_url, sizeof(dyn_url), "http://%s:2006/lin", ipbuf);
  int found_links = 0;
  char curlcmd[512];
  for (const char **p = link_candidates; !found_links && *p; ++p) {
    snprintf(curlcmd, sizeof(curlcmd), "/usr/bin/curl -s --connect-timeout 1 %s", *p);
    if (util_exec(curlcmd, &linksbuf, &linksn) == 0 && linksbuf && linksn>0) { found_links = 1; break; }
    if (linksbuf) { free(linksbuf); linksbuf = NULL; linksn = 0; }
  }
  if (!found_links && dyn_url[0]) {
    snprintf(curlcmd, sizeof(curlcmd), "/usr/bin/curl -s --connect-timeout 1 %s", dyn_url);
    if (util_exec(curlcmd, &linksbuf, &linksn) == 0 && linksbuf && linksn>0) found_links = 1;
  }
  if (found_links) {
    // parse lines, each line expected to be: Intf LocalIP RemoteIP RemoteHostname LQ NLQ Cost [routes nodes]
    char *p = linksbuf; char *ln = strtok(p, "\n");
    lpos += snprintf(links_json + lpos, sizeof(links_json) - lpos, "\"links\":[");
    while (ln) {
      // trim leading spaces
      while (*ln && isspace((unsigned char)*ln)) ln++;
      if (*ln) {
        char *tokens[16]; int pc=0;
        char *p2 = ln; char *tok2;
        while (pc < 16 && (tok2 = strsep(&p2, " \t")) != NULL) {
          if (tok2[0]==0) continue;
          tokens[pc++] = tok2;
        }
          if (pc >= 6) {
          /* detect whether first token is IP (LocalIP) or an interface name */
          struct in_addr addrcheck;
          char intf[64] = ""; char lip[128] = ""; char rip[128] = ""; char rhost[256] = ""; char lq[32] = ""; char nlq[32] = ""; char cost[32] = ""; char routes[64] = ""; char nodes[64] = "";
          if (inet_pton(AF_INET, tokens[0], &addrcheck) == 1) {
            /* format: LocalIP RemoteIP Hyst. LQ NLQ Cost [..] */
            snprintf(lip, sizeof(lip), "%s", tokens[0]);
            snprintf(rip, sizeof(rip), "%s", tokens[1]);
            /* Hyst is tokens[2], LQ tokens[3], NLQ tokens[4], Cost tokens[5] */
            snprintf(lq, sizeof(lq), "%s", tokens[3] ? tokens[3] : "");
            snprintf(nlq, sizeof(nlq), "%s", tokens[4] ? tokens[4] : "");
            snprintf(cost, sizeof(cost), "%s", tokens[5] ? tokens[5] : "");
          } else if (pc >= 7 && inet_pton(AF_INET, tokens[1], &addrcheck) == 1) {
            /* format variant: Intf LocalIP RemoteIP [RemoteHostname] LQ NLQ Cost [routes nodes]
             * Be tolerant: strip masks (/32) from IP tokens and trailing ':' from intf.
             */
            /* clean intf (strip trailing ':') */
            snprintf(intf, sizeof(intf), "%s", tokens[0]);
            size_t ilen = strlen(intf); if (ilen > 0 && intf[ilen-1] == ':') intf[ilen-1] = '\0';
            /* helper to clean ip-like tokens (remove /mask or trailing comma) */
            char t1[128] = "", t2[128] = "", t3[128] = "";
            snprintf(t1, sizeof(t1), "%s", tokens[1]); char *s = strchr(t1, '/'); if (s) *s = '\0';
            snprintf(t2, sizeof(t2), "%s", tokens[2]); s = strchr(t2, '/'); if (s) *s = '\0';
            snprintf(lip, sizeof(lip), "%s", t1);
            snprintf(rip, sizeof(rip), "%s", t2);
            /* decide whether tokens[3] is a hostname or the LQ value */
            int cur = 3;
            if (pc > 3) {
              snprintf(t3, sizeof(t3), "%s", tokens[3]); char t3c[128]; snprintf(t3c, sizeof(t3c), "%s", t3); s = strchr(t3c, '/'); if (s) *s='\0';
              /* if tokens[3] is not an IP numeric value, consider it a remote hostname */
              if (inet_pton(AF_INET, t3c, &addrcheck) != 1) {
                snprintf(rhost, sizeof(rhost), "%s", tokens[3]); cur = 4;
              }
            }
            /* now map LQ/NLQ/Cost/Routes/Nodes relative to cur */
            if (pc > cur) snprintf(lq, sizeof(lq), "%s", tokens[cur]);
            if (pc > cur+1) snprintf(nlq, sizeof(nlq), "%s", tokens[cur+1]);
            if (pc > cur+2) snprintf(cost, sizeof(cost), "%s", tokens[cur+2]);
            if (pc > cur+3) snprintf(routes, sizeof(routes), "%s", tokens[cur+3]);
            if (pc > cur+4) snprintf(nodes, sizeof(nodes), "%s", tokens[cur+4]);
          } else {
            ln = strtok(NULL, "\n");
            continue;
          }
          /* fallback: some olsrd builds or configurations append routes/nodes as key=value or inside
           * bracketed fields at the end of the line. Try to extract them from the original line if
           * tokens did not yield values. Run this for both token formats.
           */
          if (!routes[0] || !nodes[0]) {
            char *rp = strstr(ln, "routes=");
            if (!rp) rp = strstr(ln, "routes:");
            if (rp) {
              char *sep = strchr(rp, '='); if (!sep) sep = strchr(rp, ':');
              if (sep) {
                char *valp = sep + 1;
                while (*valp && isspace((unsigned char)*valp)) valp++;
                char valbuf[64] = {0}; int vi = 0;
                while (*valp && !isspace((unsigned char)*valp) && vi < (int)sizeof(valbuf)-1) valbuf[vi++] = *valp++;
                if (vi) snprintf(routes, sizeof(routes), "%s", valbuf);
              }
            }
            char *np = strstr(ln, "nodes=");
            if (!np) np = strstr(ln, "nodes:");
            if (np) {
              char *sepn = strchr(np, '='); if (!sepn) sepn = strchr(np, ':');
              if (sepn) {
                char *valp = sepn + 1;
                while (*valp && isspace((unsigned char)*valp)) valp++;
                char valbuf[64] = {0}; int vi = 0;
                while (*valp && !isspace((unsigned char)*valp) && vi < (int)sizeof(valbuf)-1) valbuf[vi++] = *valp++;
                if (vi) snprintf(nodes, sizeof(nodes), "%s", valbuf);
              }
            }
            if ((!routes[0] || !nodes[0]) && (strchr(ln, '[') || strchr(ln, '('))) {
              char *lb = strchr(ln, '[');
              char *rb = lb ? strchr(lb, ']') : NULL;
              if (!lb) { lb = strchr(ln, '('); rb = lb ? strchr(lb, ')') : NULL; }
              if (lb && rb && rb > lb) {
                size_t il = (size_t)(rb - lb - 1);
                if (il > 0 && il < 512) {
                  char inner[512]; memcpy(inner, lb+1, il); inner[il]=0;
                  char *pinner = inner; char *tk;
                  while ((tk = strsep(&pinner, " ,;:\t")) != NULL) {
                    if (!tk[0]) continue;
                    /* skip tokens that look like IPs (contain a dot) */
                    if (strchr(tk, '.')) continue;
                    if (!routes[0]) {
                      if (strpbrk(tk, "0123456789")) { snprintf(routes, sizeof(routes), "%s", tk); continue; }
                    }
                    if (!nodes[0]) {
                      if (strpbrk(tk, "0123456789")) { snprintf(nodes, sizeof(nodes), "%s", tk); continue; }
                    }
                  }
                }
              }
            }

            /* additional heuristics: look for explicit 'routes='/'routes:' and 'nodes='/'nodes:' with numeric values */
            if (!routes[0]) {
              char *rpp = ln;
              while ((rpp = strstr(rpp, "routes")) != NULL) {
                char *sep = strchr(rpp, '='); if (!sep) sep = strchr(rpp, ':');
                if (sep) {
                  char *v = sep + 1; while (*v && isspace((unsigned char)*v)) v++;
                  char nb[64] = {0}; int ni = 0; while (*v && (isdigit((unsigned char)*v) || *v==',' ) && ni < (int)sizeof(nb)-1) nb[ni++] = *v++;
                  if (ni) { snprintf(routes, sizeof(routes), "%s", nb); break; }
                }
                rpp += 6; /* skip past 'routes' */
              }
            }
            if (!nodes[0]) {
              char *npp = ln;
              while ((npp = strstr(npp, "nodes")) != NULL) {
                char *sep = strchr(npp, '='); if (!sep) sep = strchr(npp, ':');
                if (sep) {
                  char *v = sep + 1; while (*v && isspace((unsigned char)*v)) v++;
                  char nb[64] = {0}; int ni = 0; while (*v && (isdigit((unsigned char)*v) || *v==',' ) && ni < (int)sizeof(nb)-1) nb[ni++] = *v++;
                  if (ni) { snprintf(nodes, sizeof(nodes), "%s", nb); break; }
                }
                npp += 5; /* skip past 'nodes' */
              }
            }

          }
          /* if remote hostname missing or placeholder '-', attempt a reverse DNS lookup for the remote IP
           * and if that fails fall back to the remote IP string so the UI shows something useful.
           */
          if (( !rhost[0] || strcmp(rhost, "-") == 0 ) && rip[0]) {
            struct sockaddr_in raddr; memset(&raddr,0,sizeof(raddr)); raddr.sin_family = AF_INET;
            if (inet_pton(AF_INET, rip, &raddr.sin_addr) == 1) {
              char rname[256] = "";
              if (getnameinfo((struct sockaddr*)&raddr, sizeof(raddr), rname, sizeof(rname), NULL, 0, 0) == 0) {
                snprintf(rhost, sizeof(rhost), "%s", rname);
              } else {
                /* fallback to IP string */
                snprintf(rhost, sizeof(rhost), "%s", rip);
              }
            } else {
              snprintf(rhost, sizeof(rhost), "%s", rip);
            }
          }
          /* ensure lip and rip are valid IPs */
          if (inet_pton(AF_INET, lip, &addrcheck) != 1) { ln = strtok(NULL, "\n"); continue; }
          if (inet_pton(AF_INET, rip, &addrcheck) != 1) { ln = strtok(NULL, "\n"); continue; }
          /* final fallback: ensure remote_host is never empty in output - use rip if no name resolved */
          if (!rhost[0]) snprintf(rhost, sizeof(rhost), "%s", rip);
          /* if interface name missing, attempt to map local IP to interface via getifaddrs */
          if (!intf[0] && lip[0]) {
            struct ifaddrs *ifap = NULL, *ifa = NULL;
            if (getifaddrs(&ifap) == 0 && ifap) {
              for (ifa = ifap; ifa; ifa = ifa->ifa_next) {
                if (!ifa->ifa_addr) continue;
                if (ifa->ifa_addr->sa_family == AF_INET) {
                  struct sockaddr_in sa;
                  memcpy(&sa, ifa->ifa_addr, sizeof(struct sockaddr_in));
                  char addrbuf[128] = ""; inet_ntop(AF_INET, &sa.sin_addr, addrbuf, sizeof(addrbuf));
                  if (addrbuf[0] && strcmp(addrbuf, lip) == 0) { snprintf(intf, sizeof(intf), "%s", ifa->ifa_name); break; }
                }
              }
              freeifaddrs(ifap);
            }
            if (intf[0]) fprintf(stderr, "[h_status] mapped local %s -> intf=%s\n", lip, intf);
          }
          if (lpos + 512 > sizeof(links_json)) break;
          /* debug log parsed link fields to aid investigation when remote_host missing */
          fprintf(stderr, "[h_status] parsed link: intf='%s' local='%s' remote='%s' remote_host='%s' lq='%s' nlq='%s' cost='%s' routes='%s' nodes='%s'\n",
            intf, lip, rip, rhost, lq, nlq, cost, routes, nodes);
          if (links_count) lpos += snprintf(links_json + lpos, sizeof(links_json) - lpos, ",");
          /* determine routes/nodes counts from gateway table if not already present */
          int routes_count = 0; int nodes_count = 0;
          if (!routes[0] || !nodes[0]) {
            for (int gi=0; gi<gw_n; gi++) {
              if (strcmp(gw[gi].gw, rip) == 0) {
                routes_count = gw[gi].dest_n;
                nodes_count = gw[gi].nodes_n;
                break;
              }
            }
          }
          lpos += snprintf(links_json + lpos, sizeof(links_json) - lpos,
            "{\"intf\":\"%s\",\"local\":\"%s\",\"remote\":\"%s\",\"remote_host\":\"%s\",\"lq\":\"%s\",\"nlq\":\"%s\",\"cost\":\"%s\"",
            intf, lip, rip, rhost, lq, nlq, cost);
          if (routes[0]) lpos += snprintf(links_json + lpos, sizeof(links_json) - lpos, ",\"routes\":\"%s\"", routes);
          else if (routes_count > 0) lpos += snprintf(links_json + lpos, sizeof(links_json) - lpos, ",\"routes\":\"%d\"", routes_count);
          if (nodes[0]) lpos += snprintf(links_json + lpos, sizeof(links_json) - lpos, ",\"nodes\":\"%s\"", nodes);
          else if (nodes_count > 0) lpos += snprintf(links_json + lpos, sizeof(links_json) - lpos, ",\"nodes\":\"%d\"", nodes_count);
          lpos += snprintf(links_json + lpos, sizeof(links_json) - lpos, "}");
          links_count++;
        }
      }
      ln = strtok(NULL, "\n");
    }
    lpos += snprintf(links_json + lpos, sizeof(links_json) - lpos, "]");
    free(linksbuf); linksbuf = NULL; linksn = 0;
      /* free gateway list */
      for (int i=0;i<gw_n;i++) {
        if (gw[i].dests) {
          for (int j=0;j<gw[i].dest_n;j++) if (gw[i].dests[j]) free(gw[i].dests[j]);
          free(gw[i].dests);
        }
      }
      free(gw);
  } else {
    snprintf(links_json, sizeof(links_json), "\"links\":[]");
  }

  /* default route: try to get via `ip -4 r get 8.8.8.8` and parse 'via' and 'dev' */
  char def_ip[64] = ""; char def_dev[64] = ""; char def_host[256] = "";
  if (util_exec("/sbin/ip -4 r get 8.8.8.8", &out, &n) == 0 && out && n>0) {
    char *ln = strtok(out, "\n");
    if (ln) {
      char *pv = strstr(ln, " via "); if (pv) { pv += 5; sscanf(pv, "%63s", def_ip); }
      char *pd = strstr(ln, " dev "); if (pd) { pd += 5; sscanf(pd, "%63s", def_dev); }
    }
    free(out); out = NULL; n = 0;
  }
  if (def_ip[0]) {
    struct sockaddr_in sa; memset(&sa,0,sizeof(sa)); sa.sin_family = AF_INET; inet_pton(AF_INET, def_ip, &sa.sin_addr);
    /* write name directly into def_host to avoid an intermediate buffer that
     * could be larger than def_host and trigger truncation warnings.
     */
    if (getnameinfo((struct sockaddr*)&sa, sizeof(sa), def_host, sizeof(def_host), NULL, 0, 0) != 0) def_host[0]=0;
  }

  /* default_route JSON */
  char def_json[512];
  if (def_ip[0]) snprintf(def_json, sizeof(def_json), "\"default_route\":{\"hostname\":\"%s\",\"ip\":\"%s\",\"dev\":\"%s\"}", def_host[0]?def_host:def_ip, def_ip, def_dev);
  else snprintf(def_json, sizeof(def_json), "\"default_route\":{}");

  // build minimal JSON (include default_route) dynamically to avoid truncation warnings
  size_t jcap = 4096; char *jbuf = (char*)malloc(jcap);
  if (!jbuf) { send_json(r, "{}"); return 0; }
  int needed = snprintf(NULL,0,"{\"hostname\":\"%s\",\"ip\":\"%s\",\"uptime\":\"%s\",\"devices\":[],\"airosdata\":{},\"olsr2_on\":false,%s,%s}", hostname, ipbuf, uptimebuf, links_json, def_json);
  if (needed < 0) { free(jbuf); send_json(r, "{}"); return 0; }
  if ((size_t)needed + 1 > jcap) { jcap = (size_t)needed + 1; char *tmp = (char*)realloc(jbuf, jcap); if(!tmp){ free(jbuf); send_json(r, "{}"); return 0; } jbuf = tmp; }
  snprintf(jbuf, jcap, "{\"hostname\":\"%s\",\"ip\":\"%s\",\"uptime\":\"%s\",\"devices\":[],\"airosdata\":{},\"olsr2_on\":false,%s,%s}",
    hostname, ipbuf, uptimebuf, links_json, def_json);
  send_json(r, jbuf);
  free(jbuf);
  return 0;
}

/* capabilities endpoint */
static int h_capabilities_local(http_request_t *r) {
  int airos = path_exists("/tmp/10-all.json");
  int discover = g_has_ubnt_discover ? 1 : 0;
  int tracer = g_has_traceroute ? 1 : 0;
  char buf[256]; snprintf(buf, sizeof(buf), "{\"is_edgerouter\":%s,\"discover\":%s,\"airos\":%s,\"connections\":true,\"traffic\":%s,\"txtinfo\":true,\"jsoninfo\":true,\"traceroute\":%s}",
    g_is_edgerouter?"true":"false", discover?"true":"false", airos?"true":"false", path_exists("/tmp")?"true":"false", tracer?"true":"false");
  send_json(r, buf);
  return 0;
}
#include "httpd.h"
int g_is_edgerouter = 0;
int g_has_ubnt_discover = 0;
int g_has_traceroute = 0;
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

/* Embedded assets for environments where file access fails */
static const char embedded_index_html[] =
"<!doctype html>\n"
"<html lang=\"de\">\n"
"<head>\n"
"  <meta charset=\"utf-8\">\n"
"  <meta http-equiv=\"x-ua-compatible\" content=\"IE=edge\">\n"
"  <meta name=\"viewport\" content=\"width=1000, initial-scale=0.5\">\n"
"  <meta name=\"description\" content=\"\">\n"
"  <meta name=\"author\" content=\"\">\n"
"  <title>olsrd status</title>\n"
"  <style>\n"
"    body { padding: 20px; }\n"
"    .badge-env { font-size: 12px; margin-left: 8px; }\n"
"    pre { max-height: 400px; overflow:auto; background:#111; color:#0f0; padding:12px; }\n"
"    .tab-content { margin-top: 15px; }\n"
"    .table { width: 100%; margin-bottom: 20px; }\n"
"    .table th, .table td { padding: 8px; text-align: left; border: 1px solid #ddd; }\n"
"    .table th { background-color: #f5f5f5; }\n"
"    .btn { display: inline-block; padding: 6px 12px; margin-bottom: 0; font-size: 14px; font-weight: normal; line-height: 1.42857143; text-align: center; white-space: nowrap; vertical-align: middle; cursor: pointer; border: 1px solid transparent; border-radius: 4px; }\n"
"    .btn-primary { color: #fff; background-color: #337ab7; border-color: #2e6da4; }\n"
"    .btn-default { color: #333; background-color: #fff; border-color: #ccc; }\n"
"    .form-control { display: block; width: 100%; height: 34px; padding: 6px 12px; font-size: 14px; line-height: 1.42857143; color: #555; background-color: #fff; border: 1px solid #ccc; border-radius: 4px; }\n"
"    .input-group { position: relative; display: table; border-collapse: separate; }\n"
"    .input-group-btn { position: relative; font-size: 0; white-space: nowrap; }\n"
"    .input-group-btn .btn { position: relative; border-radius: 0; }\n"
"    .input-group .form-control { position: relative; z-index: 2; float: left; width: 100%; margin-bottom: 0; }\n"
"    .input-group .form-control:focus { z-index: 3; }\n"
"    .input-group-btn .btn { border-left-width: 0; }\n"
"    .input-group-btn:first-child .btn { border-right-width: 0; }\n"
"    .nav-tabs { border-bottom: 1px solid #ddd; }\n"
"    .nav-tabs > li { float: left; margin-bottom: -1px; }\n"
"    .nav-tabs > li > a { margin-right: 2px; line-height: 1.42857143; border: 1px solid transparent; border-radius: 4px 4px 0 0; }\n"
"    .nav-tabs > li.active > a, .nav-tabs > li.active > a:hover, .nav-tabs > li.active > a:focus { color: #555; background-color: #fff; border: 1px solid #ddd; border-bottom-color: transparent; cursor: default; }\n"
"    .tab-content > .tab-pane { display: none; }\n"
"    .tab-content > .active { display: block; }\n"
"    .dl-horizontal dt { float: left; width: 160px; clear: left; text-align: right; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }\n"
"    .dl-horizontal dd { margin-left: 180px; }\n"
"    .panel { margin-bottom: 20px; background-color: #fff; border: 1px solid #ddd; border-radius: 4px; }\n"
"    .panel-body { padding: 15px; }\n"
"    .panel-footer { padding: 10px 15px; background-color: #f5f5f5; border-top: 1px solid #ddd; border-bottom-right-radius: 3px; border-bottom-left-radius: 3px; }\n"
"    .alert { padding: 15px; margin-bottom: 20px; border: 1px solid transparent; border-radius: 4px; }\n"
"    .alert-warning { color: #8a6d3b; background-color: #fcf8e3; border-color: #faebcc; }\n"
"  </style>\n"
"</head>\n"
"<body>\n"
"  <div class=\"container\">\n"
"    <div class=\"row\">\n"
"      <div class=\"card\">\n"
"          <ul class=\"nav nav-tabs\" role=\"tablist\" id=\"mainTabs\">\n"
"          <li role=\"presentation\"><a href=\"#main\" aria-controls=\"main\" role=\"tab\" data-toggle=\"tab\"><span class=\"glyphicon glyphicon-info-sign\" aria-hidden=\"true\"></span> Übersicht</a></li>\n"
"          <li role=\"presentation\" class=\"active\"><a href=\"#status\" aria-controls=\"status\" role=\"tab\" data-toggle=\"tab\"><span class=\"glyphicon glyphicon-dashboard\" aria-hidden=\"true\"></span> Status</a></li>\n"
"          <li role=\"presentation\" id=\"tab-olsr2\" style=\"display:none\"><a href=\"#olsr2\" aria-controls=\"olsr2\" role=\"tab\" data-toggle=\"tab\"><span class=\"glyphicon glyphicon-dashboard\" aria-hidden=\"true\"></span> OLSRv2</a></li>\n"
"          <li role=\"presentation\"><a href=\"#contact\" aria-controls=\"contact\" role=\"tab\" data-toggle=\"tab\"><span class=\"glyphicon glyphicon-user\" aria-hidden=\"true\"></span> Kontakt</a></li>\n"
"          <li role=\"presentation\" id=\"tab-admin\" style=\"display:none\"><a href=\"#admin\" aria-controls=\"admin\" role=\"tab\" data-toggle=\"tab\"><span class=\"glyphicon glyphicon-user\" aria-hidden=\"true\"></span> Admin Login</a></li>\n"
"        </ul>\n"
"        <br>\n"
"        <div class=\"tab-content\">\n"
"          <!-- Main TAB -->\n"
"          <div role=\"tabpanel\" class=\"tab-pane\" id=\"main\">\n"
"            <div class=\"page-header\">\n"
"              <h1 id=\"hostname\">Loading… <small id=\"ip\"></small></h1>\n"
"            </div>\n"
"            <div class=\"panel panel-default\">\n"
"              <div class=\"panel-body\"><b>WAS?</b></div>\n"
"              <div class=\"panel-footer\">FunkFeuer ist ein freies, experimentelles Netzwerk in Wien, Graz, der Weststeiermark, in Teilen des Weinviertels (NÖ) und in Bad Ischl. Es wird aufgebaut und betrieben von computerbegeisterten Menschen. Das Projekt verfolgt keine kommerziellen Interessen.</div>\n"
"            </div>\n"
"            <div class=\"panel panel-default\">\n"
"              <div class=\"panel-body\"><b>FREI?</b></div>\n"
"              <div class=\"panel-footer\">FunkFeuer ist offen für jeden und jede, der/die Interesse hat und bereit ist mitzuarbeiten. Es soll dabei ein nicht reguliertes Netzwerk entstehen, welches das Potential hat, den digitalen Graben zwischen den sozialen Schichten zu Überbrücken und so Infrastruktur und Wissen zur Verfügung zu stellen. Zur Teilnahme an FunkFeuer braucht man einen WLAN Router (gibt\'s ab 60 Euro) oder einen PC, das OLSR Programm, eine IP Adresse von FunkFeuer, etwas Geduld und Motivation. Auf unserer Karte ist eingezeichnet, wo man FunkFeuer schon überall (ungefähr) empfangen kann (bitte beachte, dass manchmal Häuser oder Ähnliches im Weg sind, dann geht\'s nur über Umwege).</div>\n"
"            </div>\n"
"          </div>\n"
"          <!-- Status TAB -->\n"
"          <div role=\"tabpanel\" class=\"tab-pane active\" id=\"status\">\n"
"            <dl class=\"dl-horizontal\">\n"
"              <dt>System Uptime <span class=\"glyphicon glyphicon-time\"></span></dt><dd id=\"uptime\">Loading…</dd>\n"
"              <dt>IPv4 Default-Route <span class=\"glyphicon glyphicon-road\"></span></dt><dd id=\"default-route\">Loading…</dd>\n"
"              <dt>mgmt Devices <span class=\"glyphicon glyphicon-signal\"></span></dt><dd>\n"
"                <table class=\"table table-hover table-bordered table-condensed\" id=\"devicesTable\">\n"
"                  <thead>\n"
"                    <tr>\n"
"                      <th>Hostname</th>\n"
"                      <th>Product</th>\n"
"                      <th>Uptime</th>\n"
"                      <th>Mode</th>\n"
"                      <th>ESSID</th>\n"
"                      <th>Firmware</th>\n"
"                      <th>Wireless</th>\n"
"                    </tr>\n"
"                  </thead>\n"
"                  <tbody></tbody>\n"
"                </table>\n"
"              </dd>\n"
"              <dt>IPv4 OLSR-Links <span class=\"glyphicon glyphicon-link\"></span></dt>\n"
"              <dd>\n"
"                <div id=\"olsr-links-wrap\">\n"
"                  <table class=\"table table-condensed table-striped table-bordered\" id=\"olsrLinksTable\">\n"
"                    <thead><tr><th>Intf</th><th>Local IP</th><th>Remote IP</th><th>Remote Hostname</th><th>LQ</th><th>NLQ</th><th>Cost</th><th>routes</th><th>nodes</th></tr></thead>\n"
"                    <tbody></tbody>\n"
"                  </table>\n"
"                </div>\n"
"              </dd>\n"
"            </dl>\n"
"          </div>\n"
"          <!-- OLSRv2 TAB (hidden unless detected) -->\n"
"          <div role=\"tabpanel\" class=\"tab-pane\" id=\"olsr2\">\n"
"            <pre id=\"olsr2info\">Loading…</pre>\n"
"          </div>\n"
"          <!-- Contact TAB -->\n"
"          <div role=\"tabpanel\" class=\"tab-pane\" id=\"contact\">\n"
"            <div class=\"panel panel-default\">\n"
"              <div class=\"panel-body\">Kontakt: <a href=\"mailto:office@funkfeuer.at\">office@funkfeuer.at</a></div>\n"
"            </div>\n"
"          </div>\n"
"          <!-- Connections TAB -->\n"
"          <div role=\"tabpanel\" class=\"tab-pane\" id=\"connections\">\n"
"            <div class=\"row\" style=\"margin-bottom:8px\">\n"
"              <div class=\"col-xs-8\">\n"
"                <button id=\"refresh-connections\" class=\"btn btn-default btn-sm\">Refresh</button>\n"
"                <span id=\"connections-status\" style=\"margin-left:12px\"></span>\n"
"              </div>\n"
"              <div class=\"col-xs-4 text-right\">\n"
"                <div class=\"input-group input-group-sm\" style=\"width:260px; display:inline-block\">\n"
"                  <input id=\"tr-host\" type=\"text\" class=\"form-control\" placeholder=\"Traceroute target (ip or host)\">\n"
"                  <span class=\"input-group-btn\"><button id=\"tr-run\" class=\"btn btn-primary\">Trace</button></span>\n"
"                </div>\n"
"              </div>\n"
"            </div>\n"
"            <div id=\"connections-wrap\">\n"
"              <table class=\"table table-hover table-bordered table-condensed\" id=\"connectionsTable\">\n"
"                <thead>\n"
"                  <tr>\n"
"                    <th data-key=\"port\">Port</th>\n"
"                    <th data-key=\"bridge\">Bridge</th>\n"
"                    <th data-key=\"macs\">MACs</th>\n"
"                    <th data-key=\"ips\">IPs</th>\n"
"                    <th data-key=\"hostname\">Hostname</th>\n"
"                    <th data-key=\"notes\">Notes</th>\n"
"                  </tr>\n"
"                </thead>\n"
"                <tbody></tbody>\n"
"              </table>\n"
"            </div>\n"
"            <pre id=\"p-traceroute\" style=\"display:none; background:#111; color:#0f0; padding:12px; max-height:360px; overflow:auto\"></pre>\n"
"          </div>\n"
"          <!-- Versions TAB -->\n"
"          <div role=\"tabpanel\" class=\"tab-pane\" id=\"versions\">\n"
"            <div class=\"row\" style=\"margin-bottom:8px\">\n"
"              <div class=\"col-xs-6\">\n"
"                <button id=\"refresh-versions\" class=\"btn btn-default btn-sm\">Refresh</button>\n"
"                <span id=\"versions-status\" style=\"margin-left:12px\"></span>\n"
"              </div>\n"
"            </div>\n"
"            <div id=\"versions-wrap\">\n"
"              <!-- friendly panel rendered by JS -->\n"
"            </div>\n"
"          </div>\n"
"          <!-- Admin TAB (hidden unless detected) -->\n"
"          <div role=\"tabpanel\" class=\"tab-pane\" id=\"admin\">\n"
"            <div class=\"panel panel-default\">\n"
"              <div class=\"panel-body\">Admin Login: <a id=\"adminLink\" href=\"#\">Loading…</a></div>\n"
"            </div>\n"
"          </div>\n"
"        </div>\n"
"      </div>\n"
"    </div>\n"
"  </div>\n"
"  <script src=\"/js/jquery.min.js\"></script>\n"
"  <script src=\"/js/bootstrap.min.js\"></script>\n"
"  <script src=\"/js/app.js\"></script>\n"
"</body>\n"
"</html>\n";

static const char embedded_app_js[] =
"window.refreshTab = function(id, url) {\n"
"  var el = document.getElementById(id);\n"
"  if (el) el.textContent = 'Loading…';\n"
"  if (id === 'p-json') {\n"
"    fetch(url, {cache:\"no-store\"}).then(r=>r.text()).then(t=>{\n"
"      try{ el.textContent = JSON.stringify(JSON.parse(t), null, 2); }\n"
"      catch(e){ el.textContent = t; }\n"
"    }).catch(e=>{ el.textContent = \"ERR: \"+e; });\n"
"    return;\n"
"  }\n"
"  fetch(url, {cache:\"no-store\"}).then(r=>{\n"
"    if(!r.ok) return r.text().then(t=>{ el.textContent=\"HTTP \"+r.status+\"\\n\"+t; });\n"
"    return r.text().then(t=>{ el.textContent = t; });\n"
"  }).catch(e=>{ el.textContent = \"ERR: \"+e; });\n"
"};\n"
"\n"
"function setText(id, text) {\n"
"  var el = document.getElementById(id);\n"
"  if (el) el.textContent = text;\n"
"}\n"
"\n"
"function setHTML(id, html) {\n"
"  var el = document.getElementById(id);\n"
"  if (el) el.innerHTML = html;\n"
"}\n"
"\n"
"function showTab(tabId, show) {\n"
"  var el = document.getElementById(tabId);\n"
"  if (el) el.style.display = show ? '' : 'none';\n"
"}\n"
"\n"
"function populateDevicesTable(devices, airos) {\n"
"  var tbody = document.querySelector('#devicesTable tbody');\n"
"  tbody.innerHTML = '';\n"
"  if (!devices || !Array.isArray(devices)) return;\n"
"  var warn_frequency = 0;\n"
"  devices.forEach(function(device) {\n"
"    var tr = document.createElement('tr');\n"
"    function td(val) { var td = document.createElement('td'); td.innerHTML = val || ''; return td; }\n"
"    tr.appendChild(td(device.ipv4));\n"
"    tr.appendChild(td(device.hostname));\n"
"    tr.appendChild(td(device.product));\n"
"    tr.appendChild(td(device.uptime));\n"
"    tr.appendChild(td(device.mode));\n"
"    tr.appendChild(td(device.essid));\n"
"    tr.appendChild(td(device.firmware));\n"
"    var wireless = '';\n"
"    var freq_start = null, freq_end = null, frequency = null, chanbw = null;\n"
"    if (airos && airos[device.ipv4] && airos[device.ipv4].wireless) {\n"
"      var w = airos[device.ipv4].wireless;\n"
"      frequency = parseInt((w.frequency || '').replace('MHz','').trim());\n"
"      chanbw = parseInt(w.chanbw || 0);\n"
"      if (frequency && chanbw) {\n"
"        freq_start = frequency - (chanbw/2);\n"
"        freq_end = frequency + (chanbw/2);\n"
"        if (freq_start < 5490 && freq_end > 5710) warn_frequency = 1;\n"
"      }\n"
"      wireless = (w.frequency ? w.frequency + 'MHz ' : '') + (w.mode || '');\n"
"    }\n"
"    tr.appendChild(td(wireless));\n"
"    tbody.appendChild(tr);\n"
"  });\n"
"  var table = document.getElementById('devicesTable');\n"
"  if (warn_frequency && table) {\n"
"    var warn = document.createElement('div');\n"
"    warn.className = 'alert alert-warning';\n"
"    warn.innerHTML = 'Warnung: Frequenzüberlappung erkannt!';\n"
"    table.parentNode.insertBefore(warn, table);\n"
"  }\n"
"}\n"
"\n"
"function populateOlsrLinksTable(links) {\n"
"  var tbody = document.querySelector('#olsrLinksTable tbody');\n"
"  if (!tbody) return; tbody.innerHTML = '';\n"
"  if (!links || !Array.isArray(links)) return;\n"
"  links.forEach(function(l){\n"
"    var tr = document.createElement('tr');\n"
"    function td(val){ var td = document.createElement('td'); td.innerHTML = val || ''; return td; }\n"
"    tr.appendChild(td(l.intf));\n"
"    tr.appendChild(td(l.local));\n"
"    tr.appendChild(td(l.remote));\n"
"    tr.appendChild(td(l.remote_host));\n"
"    tr.appendChild(td(l.lq));\n"
"    tr.appendChild(td(l.nlq));\n"
"    tr.appendChild(td(l.cost));\n"
"    tr.appendChild(td(l.routes || ''));\n"
"    tr.appendChild(td(l.nodes || ''));\n"
"    tbody.appendChild(tr);\n"
"  });\n"
"}\n"
"\n"
"function updateUI(data) {\n"
"  setText('hostname', data.hostname || 'Unknown');\n"
"  setText('ip', data.ip || '');\n"
"  setText('uptime', data.uptime || '');\n"
"  try { if (data.hostname) document.title = data.hostname; } catch(e) {}\n"
"  // render default route if available (hostname link + ip link + device)\n"
"  try {\n"
"    if (data.default_route && (data.default_route.ip || data.default_route.dev || data.default_route.hostname)) {\n"
"      var host = data.default_route.hostname || '';\n"
"      var ip = data.default_route.ip || '';\n"
"      var dev = data.default_route.dev || '';\n"
"      var parts = [];\n"
"      if (host) parts.push('<a target=\"_blank\" href=\"https://' + host + '\">' + host + '</a>');\n"
"      if (ip) parts.push('(<a target=\"_blank\" href=\"https://' + ip + '\">' + ip + '</a>)');\n"
"      var html = parts.join(' ');\n"
"      if (dev) html += ' via ' + dev;\n"
"      setHTML('default-route', html);\n"
"    } else {\n"
"      setText('default-route', 'n/a');\n"
"    }\n"
"  } catch(e) { setText('default-route', 'n/a'); }\n"
"  populateDevicesTable(data.devices, data.airos);\n"
"  if (data.olsr2_on) {\n"
"    showTab('tab-olsr2', true);\n"
"    setText('olsr2info', data.olsr2info || '');\n"
"  } else {\n"
"    showTab('tab-olsr2', false);\n"
"  }\n"
"  if (data.admin && data.admin.url) {\n"
"    showTab('tab-admin', true);\n"
"    var adminLink = document.getElementById('adminLink');\n"
"    if (adminLink) { adminLink.href = data.admin.url; adminLink.textContent = 'Login'; }\n"
"  } else {\n"
"    showTab('tab-admin', false);\n"
"  }\n"
"}\n"
"\n"
"function detectPlatformAndLoad() {\n"
"  fetch('/capabilities', {cache: 'no-store'})\n"
"    .then(r => r.json())\n"
"    .then(caps => {\n"
"      var data = { hostname: '', ip: '', uptime: '', devices: [], airos: {}, olsr2_on: false, olsr2info: '', admin: null };\n"
"      fetch('/status', {cache: 'no-store'})\n"
"        .then(r => r.json())\n"
"        .then(status => {\n"
"          data.hostname = status.hostname || '';\n"
"          data.ip = status.ip || '';\n"
"          data.uptime = status.uptime || '';\n"
"          data.devices = status.devices || [];\n"
"          data.airos = status.airosdata || {};\n"
"          // ensure defaults so updateUI can safely use them\n"
"          data.default_route = status.default_route || {};\n"
"          data.links = status.links || [];\n"
"          if (status.olsr2_on) {\n"
"            data.olsr2_on = true;\n"
"            fetch('/olsr2', {cache: 'no-store'})\n"
"              .then(r => r.text())\n"
"              .then(t => { data.olsr2info = t; updateUI(data); try { if (data.links && data.links.length) populateOlsrLinksTable(data.links); } catch(e){} });\n"
"          } else {\n"
"            updateUI(data);\n"
"            try { if (data.links && data.links.length) populateOlsrLinksTable(data.links); } catch(e){}\n"
"          }\n"
"          if (status.admin_url) {\n"
"            data.admin = { url: status.admin_url };\n"
"            updateUI(data);\n"
"          }\n"
"          var nodedb = {};\n"
"          fetch('/nodedb.json',{cache:'no-store'}).then(r=>r.json()).then(nb=>{ nodedb = nb || {}; }).catch(()=>{ nodedb = {}; });\n"
"          function loadConnections() {\n"
"            var statusEl = document.getElementById('connections-status'); if(statusEl) statusEl.textContent = 'Loading...';\n"
"            fetch('/connections.json',{cache:'no-store'}).then(r=>r.json()).then(c=>{\n"
"              renderConnectionsTable(c, nodedb);\n"
"              if(statusEl) statusEl.textContent = '';\n"
"            }).catch(e=>{ var el=document.getElementById('connections-status'); if(el) el.textContent='ERR: '+e; });\n"
"          }\n"
"          loadConnections();\n"
"          function loadVersions() {\n"
"            var statusEl = document.getElementById('versions-status'); if(statusEl) statusEl.textContent = 'Loading...';\n"
"            fetch('/versions.json',{cache:'no-store'}).then(r=>r.json()).then(v=>{\n"
"              renderVersionsPanel(v);\n"
"              if(statusEl) statusEl.textContent = '';\n"
"            }).catch(e=>{ var el=document.getElementById('versions-status'); if(el) el.textContent='ERR: '+e; });\n"
"          }\n"
"          loadVersions();\n"
"          document.getElementById('tr-run').addEventListener('click', function(){ runTraceroute(); });\n"
"          document.getElementById('refresh-connections').addEventListener('click', loadConnections);\n"
"          document.getElementById('refresh-versions').addEventListener('click', loadVersions);\n"
"        });\n"
"    });\n"
"}\n"
"\n"
"document.addEventListener('DOMContentLoaded', function() {\n"
"  // Initialize tab functionality\n"
"  var tabLinks = document.querySelectorAll('#mainTabs a');\n"
"  var tabPanes = document.querySelectorAll('.tab-pane');\n"
"  \n"
"  function switchTab(targetId) {\n"
"    // Hide all tab panes\n"
"    tabPanes.forEach(function(pane) {\n"
"      pane.classList.remove('active');\n"
"    });\n"
"    \n"
"    // Remove active class from all tab links\n"
"    tabLinks.forEach(function(link) {\n"
"      link.parentElement.classList.remove('active');\n"
"    });\n"
"    \n"
"    // Show target tab pane\n"
"    var targetPane = document.querySelector(targetId);\n"
"    if (targetPane) {\n"
"      targetPane.classList.add('active');\n"
"    }\n"
"    \n"
"    // Add active class to clicked tab link\n"
"    var activeLink = document.querySelector('#mainTabs a[href=\"' + targetId + '\"]');\n"
"    if (activeLink) {\n"
"      activeLink.parentElement.classList.add('active');\n"
"    }\n"
"  }\n"
"  \n"
"  // Add click handlers to tab links\n"
"  tabLinks.forEach(function(link) {\n"
"    link.addEventListener('click', function(e) {\n"
"      e.preventDefault();\n"
"      var targetId = this.getAttribute('href');\n"
"      switchTab(targetId);\n"
"    });\n"
"  });\n"
"  \n"
"  detectPlatformAndLoad();\n"
"});\n"
"\n"
"function renderConnectionsTable(c, nodedb) {\n"
"  var tbody = document.querySelector('#connectionsTable tbody');\n"
"  tbody.innerHTML = '';\n"
"  if (!c || !c.ports) return;\n"
"  c.ports.forEach(function(p){\n"
"    var tr = document.createElement('tr');\n"
"    function td(val){ var td=document.createElement('td'); td.innerHTML = val || ''; return td; }\n"
"    tr.appendChild(td(p.port));\n"
"    tr.appendChild(td(p.bridge || ''));\n"
"    tr.appendChild(td((p.macs || []).join('<br>')));\n"
"    tr.appendChild(td((p.ips || []).join('<br>')));\n"
"    var hostnames = [];\n"
"    (p.ips || []).forEach(function(ip){ if(nodedb[ip] && nodedb[ip].name) hostnames.push(nodedb[ip].name); });\n"
"    tr.appendChild(td(hostnames.join('<br>')));\n"
"    tr.appendChild(td(p.notes || ''));\n"
"    tbody.appendChild(tr);\n"
"  });\n"
"  var headers = document.querySelectorAll('#connectionsTable th');\n"
"  headers.forEach(function(h){ h.style.cursor='pointer'; h.onclick = function(){ sortTableByColumn(h.getAttribute('data-key')); }; });\n"
"}\n"
"\n"
"function renderVersionsPanel(v) {\n"
"  var wrap = document.getElementById('versions-wrap'); if(!wrap) return; wrap.innerHTML='';\n"
"  if(!v) { wrap.textContent = 'No versions data'; return; }\n"
"  var dl = document.createElement('dl'); dl.className='dl-horizontal';\n"
"  function add(k,label){ var dt=document.createElement('dt'); dt.textContent=label||k; var dd=document.createElement('dd'); dd.textContent=(v[k]!==undefined?v[k]:'-'); dl.appendChild(dt); dl.appendChild(dd); }\n"
"  add('hostname','Hostname');\n"
"  add('firmware','Firmware');\n"
"  add('kernel','Kernel');\n"
"  add('model','Model');\n"
"  add('autoupdate','AutoUpdate');\n"
"  var pre = document.createElement('pre'); pre.style.maxHeight='240px'; pre.style.overflow='auto'; pre.textContent = JSON.stringify(v,null,2);\n"
"  wrap.appendChild(dl); wrap.appendChild(pre);\n"
"}\n"
"\n"
"function sortTableByColumn(key) {\n"
"  var tbody = document.querySelector('#connectionsTable tbody');\n"
"  if(!tbody) return;\n"
"  var rows = Array.prototype.slice.call(tbody.querySelectorAll('tr'));\n"
"  rows.sort(function(a,b){\n"
"    var idx = getColumnIndexByKey(key);\n"
"    var va = a.cells[idx] ? a.cells[idx].textContent.trim() : '';\n"
"    var vb = b.cells[idx] ? b.cells[idx].textContent.trim() : '';\n"
"    return va.localeCompare(vb, undefined, {numeric:true});\n"
"  });\n"
"  rows.forEach(function(r){ tbody.appendChild(r); });\n"
"}\n"
"\n"
"function getColumnIndexByKey(key){\n"
"  var ths = document.querySelectorAll('#connectionsTable th');\n"
"  for(var i=0;i<ths.length;i++){ if(ths[i].getAttribute('data-key')===key) return i; }\n"
"  return 0;\n"
"}\n"
"\n"
"function runTraceroute(){\n"
"  var target = document.getElementById('tr-host').value.trim();\n"
"  if(!target) return alert('Enter target for traceroute');\n"
"  var pre = document.getElementById('p-traceroute'); pre.style.display='block'; pre.textContent='Running traceroute...';\n"
"  fetch('/traceroute?target='+encodeURIComponent(target),{cache:'no-store'}).then(r=>r.text()).then(t=>{ pre.textContent = t; }).catch(e=>{ pre.textContent = 'ERR: '+e; });\n"
"}\n"
"\n";

static int h_embedded_index(http_request_t *r) {
  http_send_status(r, 200, "OK");
  http_printf(r, "Content-Type: text/html; charset=utf-8\r\n\r\n");
  http_write(r, embedded_index_html, sizeof(embedded_index_html)-1);
  return 0;
}

static int h_embedded_appjs(http_request_t *r) {
  http_send_status(r, 200, "OK");
  http_printf(r, "Content-Type: application/javascript; charset=utf-8\r\n\r\n");
  http_write(r, embedded_app_js, sizeof(embedded_app_js)-1);
  return 0;
}

static const char embedded_jquery_min[] = "/* minimal jquery stub for offline */ window.$ = window.jQuery = { ready: function(f){ if(typeof f==='function') f(); }, ajax: function(){}, get: function(){}, post: function(){} };\n";
static const char embedded_bootstrap_min[] = "/* minimal bootstrap stub */ (function(){ var bs = {}; bs.tab = { show: function(){} }; window.bootstrap = bs; })();\n";

static int h_emb_jquery(http_request_t *r) { http_send_status(r,200,"OK"); http_printf(r,"Content-Type: application/javascript; charset=utf-8\r\n\r\n"); http_write(r, embedded_jquery_min, sizeof(embedded_jquery_min)-1); return 0; }
static int h_emb_bootstrap(http_request_t *r) { http_send_status(r,200,"OK"); http_printf(r,"Content-Type: application/javascript; charset=utf-8\r\n\r\n"); http_write(r, embedded_bootstrap_min, sizeof(embedded_bootstrap_min)-1); return 0; }

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
  const char *ubnt_candidates[] = { "/usr/sbin/ubnt-discover", "/sbin/ubnt-discover", "/usr/bin/ubnt-discover", NULL };
  const char *tracer_candidates[] = { "/usr/sbin/traceroute", "/bin/traceroute", "/usr/bin/traceroute", NULL };
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

static int h_root(http_request_t *r) {
  /* Always serve embedded index.html so "/" matches the packaged index exactly */
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
