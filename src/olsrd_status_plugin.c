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
#include <signal.h>
#if defined(__APPLE__) || defined(__linux__)
# include <execinfo.h>
#endif
#ifdef HAVE_LIBCURL
#include <curl/curl.h>
#endif

#include "httpd.h"
#include "util.h"
#include "olsrd_plugin.h"
#include "ubnt_discover.h"

#include <stdarg.h>

#if __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_ATOMICS__)
# include <stdatomic.h>
# define HAVE_C11_ATOMICS 1
#else
# define HAVE_C11_ATOMICS 0
#endif

/* Global configuration/state (single authoritative definitions) */
int g_is_edgerouter = 0;
int g_has_traceroute = 0;
int g_is_linux_container = 0;

static char   g_bind[64] = "0.0.0.0";
static int    g_port = 11080;
static int    g_enable_ipv6 = 0;
static char   g_asset_root[512] = "/usr/share/olsrd-status-plugin/www";
/* Flags to record whether a plugin parameter was supplied via PlParam
 * If set, configuration file values take precedence over environment vars.
 */
static int g_cfg_port_set = 0;
static int g_cfg_nodedb_ttl_set = 0;
static int g_cfg_nodedb_write_disk_set = 0;
static int g_cfg_nodedb_url_set = 0;
static int g_cfg_net_count = 0;
/* track fetch tuning PlParam presence */
static int g_cfg_fetch_queue_set = 0;
static int g_cfg_fetch_retries_set = 0;
static int g_cfg_fetch_backoff_set = 0;

/* Node DB remote auto-update cache */
static char   g_nodedb_url[512] = "https://ff.cybercomm.at/node_db.json"; /* override via plugin param nodedb_url */
static int    g_nodedb_ttl = 300; /* seconds */
static time_t g_nodedb_last_fetch = 0; /* epoch of last successful fetch */
static char  *g_nodedb_cached = NULL; /* malloc'ed JSON blob */
static size_t g_nodedb_cached_len = 0;
static pthread_mutex_t g_nodedb_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_nodedb_worker_running = 0;
/* Serialize/coordinate concurrent fetches so multiple callers don't race or
 * spawn duplicate network activity. Callers will wait up to a short timeout
 * for an in-progress fetch to finish.
 */
static pthread_mutex_t g_nodedb_fetch_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_nodedb_fetch_cv = PTHREAD_COND_INITIALIZER;
static int g_nodedb_fetch_in_progress = 0;
/* If set to 1, write a copy of the node_db to disk (disabled by default to protect flash) */
static int g_nodedb_write_disk = 0;

/* Configurable startup wait (seconds) for initial DNS/network readiness. */
static int g_nodedb_startup_wait = 30;

/* Fetch tuning defaults (can be overridden via PlParam or env) */
static int g_fetch_queue_max = 4; /* MAX_FETCH_QUEUE default */
static int g_fetch_retries = 3; /* MAX_FETCH_RETRIES default */
static int g_fetch_backoff_initial = 1; /* seconds */
/* UI severity thresholds (defaults mirrored in JS: warn=50, crit=200, dropped_warn=10) */
static int g_fetch_queue_warn = 50;
static int g_fetch_queue_crit = 200;
static int g_fetch_dropped_warn = 10;

/* Counters / metrics - storage moved into the non-atomic branch below when C11 atomics are unavailable */
/* Mutex protecting non-atomic counters; always present so endpoints can lock it regardless of atomics availability */
static pthread_mutex_t g_metrics_lock = PTHREAD_MUTEX_INITIALIZER;

/* Periodic reporter interval (seconds). 0 to disable. Configurable via PlParam 'fetch_report_interval' or env OLSRD_STATUS_FETCH_REPORT_INTERVAL */
static int g_fetch_report_interval = 0; /* default: disabled */
static int g_cfg_fetch_report_set = 0;
static int g_cfg_fetch_queue_warn_set = 0;
static int g_cfg_fetch_queue_crit_set = 0;
static int g_cfg_fetch_dropped_warn_set = 0;
static pthread_t g_fetch_report_thread = 0;
/* Auto-refresh interval (milliseconds) suggested for UI. 0 means disabled. Can be set via PlParam 'fetch_auto_refresh_ms' or env OLSRD_STATUS_FETCH_AUTO_REFRESH_MS */
static int g_fetch_auto_refresh_ms = 15000; /* default 15s */
static int g_cfg_fetch_auto_refresh_set = 0;
/* Control whether fetch queue operations are logged to stderr (0=no, 1=yes) */
static int g_fetch_log_queue = 1;
static int g_cfg_fetch_log_queue_set = 0;
/* (moved) fetch_reporter defined after fetch queue structures so it can reference them */

/* Helper macros to update counters using atomics if available, else mutex */
#if HAVE_C11_ATOMICS
static _Atomic unsigned long atom_fetch_dropped = 0;
static _Atomic unsigned long atom_fetch_retries = 0;
static _Atomic unsigned long atom_fetch_successes = 0;
#define METRIC_INC_DROPPED() atomic_fetch_add_explicit(&atom_fetch_dropped, 1UL, memory_order_relaxed)
#define METRIC_INC_RETRIES() atomic_fetch_add_explicit(&atom_fetch_retries, 1UL, memory_order_relaxed)
#define METRIC_INC_SUCCESS() atomic_fetch_add_explicit(&atom_fetch_successes, 1UL, memory_order_relaxed)
#define METRIC_LOAD_ALL(d,r,s) do { \
    d = atomic_load_explicit(&atom_fetch_dropped, memory_order_relaxed); \
    r = atomic_load_explicit(&atom_fetch_retries, memory_order_relaxed); \
    s = atomic_load_explicit(&atom_fetch_successes, memory_order_relaxed); \
    /* mark potentially-unused locals as used to avoid "set but not used" warnings */ \
    (void)(r); (void)(s); \
  } while(0)
#else
static unsigned long g_metric_fetch_dropped = 0; /* requests dropped due to full queue */
static unsigned long g_metric_fetch_retries = 0; /* total retry attempts performed */
static unsigned long g_metric_fetch_successes = 0; /* successful fetches */
#define METRIC_INC_DROPPED() do { pthread_mutex_lock(&g_metrics_lock); g_metric_fetch_dropped++; pthread_mutex_unlock(&g_metrics_lock); } while(0)
#define METRIC_INC_RETRIES() do { pthread_mutex_lock(&g_metrics_lock); g_metric_fetch_retries++; pthread_mutex_unlock(&g_metrics_lock); } while(0)
#define METRIC_INC_SUCCESS() do { pthread_mutex_lock(&g_metrics_lock); g_metric_fetch_successes++; pthread_mutex_unlock(&g_metrics_lock); } while(0)
#define METRIC_LOAD_ALL(d,r,s) do { \
    pthread_mutex_lock(&g_metrics_lock); \
    d = g_metric_fetch_dropped; r = g_metric_fetch_retries; s = g_metric_fetch_successes; \
    /* mark potentially-unused locals as used to avoid "set but not used" warnings */ \
    (void)(r); (void)(s); \
    pthread_mutex_unlock(&g_metrics_lock); \
  } while(0)
#endif

/* Simple fetch queue structures */
/* fetch request types (bitmask) */
#define FETCH_TYPE_NODEDB   0x1
#define FETCH_TYPE_DISCOVER 0x2

struct fetch_req {
  int force; /* bypass TTL */
  int wait;  /* block caller until done */
  int done;
  int type;  /* FETCH_TYPE_* */
  pthread_mutex_t m;
  pthread_cond_t  cv;
  struct fetch_req *next;
};
static struct fetch_req *g_fetch_q_head = NULL;
static struct fetch_req *g_fetch_q_tail = NULL;
static pthread_mutex_t g_fetch_q_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_fetch_q_cv   = PTHREAD_COND_INITIALIZER;
static int g_fetch_worker_running = 0;

/* max seconds to wait when a caller requests wait=1 to avoid indefinite block */
static int g_fetch_wait_timeout = 30;

/* Debug counters for diagnostics */
/* Debug counters for diagnostics: use C11 atomics when available for lock-free updates */
#if HAVE_C11_ATOMICS
static _Atomic unsigned long atom_debug_enqueue_count = 0;
static _Atomic unsigned long atom_debug_enqueue_count_nodedb = 0;
static _Atomic unsigned long atom_debug_enqueue_count_discover = 0;
static _Atomic unsigned long atom_debug_processed_count = 0;
static _Atomic unsigned long atom_debug_processed_count_nodedb = 0;
static _Atomic unsigned long atom_debug_processed_count_discover = 0;
#define DEBUG_INC_ENQUEUED() atomic_fetch_add_explicit(&atom_debug_enqueue_count, 1UL, memory_order_relaxed)
#define DEBUG_INC_ENQUEUED_NODEDB() atomic_fetch_add_explicit(&atom_debug_enqueue_count_nodedb, 1UL, memory_order_relaxed)
#define DEBUG_INC_ENQUEUED_DISCOVER() atomic_fetch_add_explicit(&atom_debug_enqueue_count_discover, 1UL, memory_order_relaxed)
#define DEBUG_INC_PROCESSED() atomic_fetch_add_explicit(&atom_debug_processed_count, 1UL, memory_order_relaxed)
#define DEBUG_INC_PROCESSED_NODEDB() atomic_fetch_add_explicit(&atom_debug_processed_count_nodedb, 1UL, memory_order_relaxed)
#define DEBUG_INC_PROCESSED_DISCOVER() atomic_fetch_add_explicit(&atom_debug_processed_count_discover, 1UL, memory_order_relaxed)
#define DEBUG_LOAD_ALL(e,en,ed,p,pn,pd) do { \
    e = atomic_load_explicit(&atom_debug_enqueue_count, memory_order_relaxed); \
    en = atomic_load_explicit(&atom_debug_enqueue_count_nodedb, memory_order_relaxed); \
    ed = atomic_load_explicit(&atom_debug_enqueue_count_discover, memory_order_relaxed); \
    p = atomic_load_explicit(&atom_debug_processed_count, memory_order_relaxed); \
    pn = atomic_load_explicit(&atom_debug_processed_count_nodedb, memory_order_relaxed); \
    pd = atomic_load_explicit(&atom_debug_processed_count_discover, memory_order_relaxed); \
  } while(0)
#else
static unsigned long g_debug_enqueue_count = 0;
static unsigned long g_debug_enqueue_count_nodedb = 0;
static unsigned long g_debug_enqueue_count_discover = 0;
static unsigned long g_debug_processed_count = 0;
static unsigned long g_debug_processed_count_nodedb = 0;
static unsigned long g_debug_processed_count_discover = 0;
#define DEBUG_INC_ENQUEUED() do { pthread_mutex_lock(&g_debug_lock); g_debug_enqueue_count++; pthread_mutex_unlock(&g_debug_lock); } while(0)
#define DEBUG_INC_ENQUEUED_NODEDB() do { pthread_mutex_lock(&g_debug_lock); g_debug_enqueue_count_nodedb++; pthread_mutex_unlock(&g_debug_lock); } while(0)
#define DEBUG_INC_ENQUEUED_DISCOVER() do { pthread_mutex_lock(&g_debug_lock); g_debug_enqueue_count_discover++; pthread_mutex_unlock(&g_debug_lock); } while(0)
#define DEBUG_INC_PROCESSED() do { pthread_mutex_lock(&g_debug_lock); g_debug_processed_count++; pthread_mutex_unlock(&g_debug_lock); } while(0)
#define DEBUG_INC_PROCESSED_NODEDB() do { pthread_mutex_lock(&g_debug_lock); g_debug_processed_count_nodedb++; pthread_mutex_unlock(&g_debug_lock); } while(0)
#define DEBUG_INC_PROCESSED_DISCOVER() do { pthread_mutex_lock(&g_debug_lock); g_debug_processed_count_discover++; pthread_mutex_unlock(&g_debug_lock); } while(0)
#define DEBUG_LOAD_ALL(e,en,ed,p,pn,pd) do { \
    pthread_mutex_lock(&g_debug_lock); \
    e = g_debug_enqueue_count; en = g_debug_enqueue_count_nodedb; ed = g_debug_enqueue_count_discover; \
    p = g_debug_processed_count; pn = g_debug_processed_count_nodedb; pd = g_debug_processed_count_discover; \
    pthread_mutex_unlock(&g_debug_lock); \
  } while(0)
#endif

/* Mutex protecting debug counters when C11 atomics are unavailable */
static char g_debug_last_fetch_msg[256] = "";

/* Queue / retry tunables */
#define MAX_FETCH_QUEUE_DEFAULT 4
#define MAX_FETCH_RETRIES_DEFAULT 3
#define FETCH_INITIAL_BACKOFF_SEC_DEFAULT 1

static void enqueue_fetch_request(int force, int wait, int type);
static void *fetch_worker_thread(void *arg);

/* Devices cache populated by background worker to avoid blocking HTTP handlers */
static char *g_devices_cache = NULL; /* JSON array string (malloc'd) */
static size_t g_devices_cache_len = 0;
static time_t g_devices_cache_ts = 0;
static pthread_mutex_t g_devices_cache_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_devices_worker_running = 0;

/* Forward declarations for device discovery helpers used by background worker */
static int ubnt_discover_output(char **out, size_t *outlen);
static int normalize_ubnt_devices(const char *ud, char **outbuf, size_t *outlen);
static int transform_devices_to_legacy(const char *devices_json, char **out, size_t *out_len);

/* Forward declarations for helpers used by fetch worker */
static void get_primary_ipv4(char *out, size_t outlen);
static int buffer_has_content(const char *b, size_t n);
static int validate_nodedb_json(const char *buf, size_t len);
/* forward-declare cached helper and mark unused to avoid warning when not referenced */
static void get_primary_ipv4_cached(char *out, size_t outlen) __attribute__((unused));

/* Small helper: append formatted text directly into growing buffer to avoid asprintf churn */
static int json_appendf(char **bufptr, size_t *lenptr, size_t *capptr, const char *fmt, ...) {
  if (!bufptr || !lenptr || !capptr || !fmt) return -1;
  va_list ap;
  va_start(ap, fmt);
  va_list ap2;
  va_copy(ap2, ap);
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
#endif
  int needed = vsnprintf(NULL, 0, fmt, ap2);
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
  va_end(ap2);
  if (needed < 0) { va_end(ap); return -1; }
  size_t need_total = *lenptr + (size_t)needed + 1;
  if (need_total > *capptr) {
    size_t nc = *capptr ? *capptr : 1024;
    while (nc < need_total) nc *= 2;
    char *nb = realloc(*bufptr, nc);
    if (!nb) { va_end(ap); return -1; }
    *bufptr = nb; *capptr = nc;
  }
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
#endif
  vsnprintf((*bufptr) + *lenptr, (size_t)needed + 1, fmt, ap);
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
  *lenptr += (size_t)needed;
  (*bufptr)[*lenptr] = '\0';
  va_end(ap);
  return 0;
}

/* Global short-lived cache for local HTTP calls (thread-safe, small TTL) */
#define LOCAL_CACHE_ENTRIES 64
#define LOCAL_CACHE_TTL_SEC 1
typedef struct { char *url; char *body; size_t len; time_t ts; } local_cache_entry_t;
static local_cache_entry_t g_local_cache[LOCAL_CACHE_ENTRIES];
static pthread_mutex_t g_local_cache_lock = PTHREAD_MUTEX_INITIALIZER;

static int cached_util_http_get_url_local(const char *url, char **out, size_t *outlen, int timeout_sec) {
  if (!url || !out || !outlen) return -1;
  time_t nowt = time(NULL);
  pthread_mutex_lock(&g_local_cache_lock);
  for (int i = 0; i < LOCAL_CACHE_ENTRIES; ++i) {
    if (g_local_cache[i].url && strcmp(g_local_cache[i].url, url) == 0) {
      if (nowt - g_local_cache[i].ts <= LOCAL_CACHE_TTL_SEC) {
        *out = malloc(g_local_cache[i].len + 1);
        if (!*out) { pthread_mutex_unlock(&g_local_cache_lock); return -1; }
        memcpy(*out, g_local_cache[i].body, g_local_cache[i].len + 1);
        *outlen = g_local_cache[i].len;
        pthread_mutex_unlock(&g_local_cache_lock);
        return 0;
      }
      free(g_local_cache[i].url); g_local_cache[i].url = NULL;
      free(g_local_cache[i].body); g_local_cache[i].body = NULL;
      g_local_cache[i].len = 0; g_local_cache[i].ts = 0;
    }
  }
  pthread_mutex_unlock(&g_local_cache_lock);

  char *tmp = NULL; size_t tlen = 0;
  int rc = util_http_get_url_local(url, &tmp, &tlen, timeout_sec);
  if (rc != 0 || !tmp) { if (tmp) { free(tmp); } return rc; }

  pthread_mutex_lock(&g_local_cache_lock);
  int sel = 0; time_t oldest = nowt;
  for (int i = 0; i < LOCAL_CACHE_ENTRIES; ++i) {
    if (!g_local_cache[i].url) { sel = i; break; }
    if (g_local_cache[i].ts < oldest) { oldest = g_local_cache[i].ts; sel = i; }
  }
  if (g_local_cache[sel].url) free(g_local_cache[sel].url);
  if (g_local_cache[sel].body) free(g_local_cache[sel].body);
  g_local_cache[sel].url = strdup(url);
  g_local_cache[sel].body = malloc(tlen + 1);
  if (g_local_cache[sel].body) {
    memcpy(g_local_cache[sel].body, tmp, tlen + 1);
    g_local_cache[sel].len = tlen;
    g_local_cache[sel].ts = nowt;
  } else {
    free(g_local_cache[sel].url); g_local_cache[sel].url = NULL;
    g_local_cache[sel].len = 0; g_local_cache[sel].ts = 0;
  }
  pthread_mutex_unlock(&g_local_cache_lock);

  *out = tmp; *outlen = tlen;
  return 0;
}

/* Use cached wrapper in this compilation unit */
#undef util_http_get_url_local
#define util_http_get_url_local(url,out,outlen,timeout_sec) cached_util_http_get_url_local(url,out,outlen,timeout_sec)

/* Cache primary IPv4 for short TTL to avoid repeated getifaddrs */
static void get_primary_ipv4_cached(char *out, size_t outlen);
static void get_primary_ipv4_cached(char *out, size_t outlen) {
  static char cached[128] = "";
  static time_t ts = 0;
  time_t nowt = time(NULL);
  if (out && outlen) out[0] = 0;
  if (nowt - ts <= LOCAL_CACHE_TTL_SEC && cached[0]) {
    if (out && outlen) snprintf(out, outlen, "%s", cached);
    return;
  }
  struct ifaddrs *ifap = NULL, *ifa = NULL;
  if (getifaddrs(&ifap) == 0) {
    for (ifa = ifap; ifa; ifa = ifa->ifa_next) {
      if (!ifa->ifa_addr) continue;
      if (ifa->ifa_addr->sa_family == AF_INET) {
        struct sockaddr_in sa; memcpy(&sa, ifa->ifa_addr, sizeof(sa));
        char b[INET_ADDRSTRLEN]; if (inet_ntop(AF_INET, &sa.sin_addr, b, sizeof(b))) {
          if (strcmp(b, "127.0.0.1") != 0) {
            snprintf(cached, sizeof(cached), "%s", b);
            ts = nowt;
            break;
          }
        }
      }
    }
    if (ifap) freeifaddrs(ifap);
  }
  if (out && outlen) snprintf(out, outlen, "%s", cached);
}

/* Worker: periodically refresh devices cache using ubnt_discover_output + normalize_ubnt_devices */
static void *devices_cache_worker(void *arg) {
  (void)arg;
  g_devices_worker_running = 1;
  while (g_devices_worker_running) {
    /* Enqueue a discovery request; let centralized fetch worker perform discovery and update
     * the devices cache. Non-blocking enqueue so this thread won't stall.
     */
    enqueue_fetch_request(0, 0, FETCH_TYPE_DISCOVER);
    /* Sleep with short interval; adjust as needed */
    for (int i = 0; i < 10; i++) { sleep(1); if (!g_devices_worker_running) break; }
  }
  return NULL;
}

/* Start devices cache worker (called from plugin init) */
static void start_devices_worker(void) {
  pthread_t th;
  pthread_create(&th, NULL, devices_cache_worker, NULL);
  pthread_detach(th);
}

/* forward declare fetch implementation so workers can call it */
static void fetch_remote_nodedb(void);
/* forward declare discovery helper so enqueue implementation can call it synchronously when needed */
static void fetch_discover_once(void);

/* Nodedb background worker: periodically refresh remote node DB to avoid blocking handlers */
static void *nodedb_cache_worker(void *arg) {
  (void)arg;
  g_nodedb_worker_running = 1;
  while (g_nodedb_worker_running) {
    /* Enqueue a forced node DB refresh; let the fetch worker handle the actual network operations
     * so retries/backoff and metrics apply uniformly.
     */
    enqueue_fetch_request(1, 0, FETCH_TYPE_NODEDB);
    /* Sleep in small increments to allow clean shutdown; total sleep roughly equals TTL or minimum 10s */
    int total = g_nodedb_ttl > 10 ? g_nodedb_ttl : 10;
    for (int i = 0; i < total; ++i) { if (!g_nodedb_worker_running) break; sleep(1); }
  }
  return NULL;
}

static void start_nodedb_worker(void) {
  pthread_t th;
  pthread_create(&th, NULL, nodedb_cache_worker, NULL);
  pthread_detach(th);
  /* start single fetch worker thread */
  if (!g_fetch_worker_running) {
    g_fetch_worker_running = 1;
    pthread_t fth; pthread_create(&fth, NULL, fetch_worker_thread, NULL); pthread_detach(fth);
  }
}

/* Periodic reporter thread: prints fetch metrics to stderr every g_fetch_report_interval seconds */
static void *fetch_reporter(void *arg) {
  (void)arg;
  while (g_fetch_report_interval > 0) {
    sleep(g_fetch_report_interval);
    if (g_fetch_report_interval <= 0) break;
    unsigned long d=0, r=0, s=0; METRIC_LOAD_ALL(d, r, s);
    pthread_mutex_lock(&g_fetch_q_lock);
    int qlen = 0; struct fetch_req *it = g_fetch_q_head; while (it) { qlen++; it = it->next; }
    pthread_mutex_unlock(&g_fetch_q_lock);
  if (g_fetch_log_queue) fprintf(stderr, "[status-plugin] fetch metrics: queued=%d dropped=%lu retries=%lu successes=%lu\n", qlen, d, r, s);
  }
  return NULL;
}

static void enqueue_fetch_request(int force, int wait, int type) {
  struct fetch_req *rq = calloc(1, sizeof(*rq));
  if (!rq) return;
  rq->force = force; rq->wait = wait; rq->done = 0; rq->type = type ? type : FETCH_TYPE_NODEDB; rq->next = NULL;
  pthread_mutex_init(&rq->m, NULL); pthread_cond_init(&rq->cv, NULL);

  pthread_mutex_lock(&g_fetch_q_lock);
  /* Simple dedupe: if a pending request already exists that will satisfy this one,
   * avoid adding a duplicate. A pending force request satisfies non-force requests.
   */
  struct fetch_req *iter = g_fetch_q_head; struct fetch_req *found = NULL; int qlen = 0;
  while (iter) { qlen++; /* only dedupe requests of the same type */
    if (iter->type == rq->type && (iter->force || force == 0)) { found = iter; break; }
    iter = iter->next;
  }
  if (found) {
    pthread_mutex_unlock(&g_fetch_q_lock);
    /* If caller asked to wait, wait on the existing request to complete */
    if (wait) {
      pthread_mutex_lock(&found->m);
      struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_sec += g_fetch_wait_timeout;
      int wrc = 0;
      while (!found->done && wrc == 0) {
        wrc = pthread_cond_timedwait(&found->cv, &found->m, &ts);
      }
      if (!found->done) {
          if (g_fetch_log_queue) fprintf(stderr, "[status-plugin] enqueue: wait timed out after %d seconds for existing request type=%d\n", g_fetch_wait_timeout, found->type);
        }
      pthread_mutex_unlock(&found->m);
    }
  if (g_fetch_log_queue) fprintf(stderr, "[status-plugin] enqueue: deduped request type=%d force=%d wait=%d\n", rq->type, rq->force, rq->wait);
  pthread_mutex_destroy(&rq->m); pthread_cond_destroy(&rq->cv); free(rq);
    return;
  }

  /* Queue size limiting: if full, either perform a synchronous fetch for waiters or drop the request */
  if (qlen >= g_fetch_queue_max) {
  if (wait) {
      /* Caller requested to block; perform a synchronous fetch inline to satisfy them. */
      pthread_mutex_unlock(&g_fetch_q_lock);
  if (g_fetch_log_queue) fprintf(stderr, "[status-plugin] enqueue: queue full, performing synchronous fetch type=%d\n", rq->type);
  if (rq->type & FETCH_TYPE_DISCOVER) fetch_discover_once(); else fetch_remote_nodedb();
      pthread_mutex_destroy(&rq->m); pthread_cond_destroy(&rq->cv); free(rq);
      return;
    }
    /* Drop non-waiting requests when the queue is full */
    METRIC_INC_DROPPED();
    unsigned long td, tr, ts; METRIC_LOAD_ALL(td, tr, ts);
  if (g_fetch_log_queue) fprintf(stderr, "[status-plugin] fetch queue full (%d), dropping request (total_dropped=%lu)\n", qlen, td);
    pthread_mutex_unlock(&g_fetch_q_lock);
  pthread_mutex_destroy(&rq->m); pthread_cond_destroy(&rq->cv); free(rq);
    return;
  }

  /* Accept into queue */
  if (g_fetch_q_tail) g_fetch_q_tail->next = rq; else g_fetch_q_head = rq;
  g_fetch_q_tail = rq;
  /* update enqueue debug counter while holding queue lock */
  DEBUG_INC_ENQUEUED();
  if (rq->type & FETCH_TYPE_NODEDB) DEBUG_INC_ENQUEUED_NODEDB();
  if (rq->type & FETCH_TYPE_DISCOVER) DEBUG_INC_ENQUEUED_DISCOVER();
  pthread_cond_signal(&g_fetch_q_cv);
  if (g_fetch_log_queue) fprintf(stderr, "[status-plugin] enqueue: added request type=%d force=%d wait=%d (qlen now=%d)\n", rq->type, rq->force, rq->wait, qlen+1);
  pthread_mutex_unlock(&g_fetch_q_lock);

  if (wait) {
    pthread_mutex_lock(&rq->m);
    struct timespec ts2; clock_gettime(CLOCK_REALTIME, &ts2); ts2.tv_sec += g_fetch_wait_timeout;
    int wrc2 = 0;
    while (!rq->done && wrc2 == 0) { wrc2 = pthread_cond_timedwait(&rq->cv, &rq->m, &ts2); }
    if (!rq->done) {
      /* Timed out waiting for worker: mark this request as not waited so the worker
       * will free it when done. Do this while holding rq->m to avoid races. */
  if (g_fetch_log_queue) fprintf(stderr, "[status-plugin] enqueue: own request wait timed out after %d seconds type=%d\n", g_fetch_wait_timeout, rq->type);
      rq->wait = 0; /* worker will treat as non-waiting and free */
      pthread_mutex_unlock(&rq->m);
      return;
    }
    /* Completed: safe to destroy our synchronization primitives and free the request */
    pthread_mutex_unlock(&rq->m);
    pthread_mutex_destroy(&rq->m); pthread_cond_destroy(&rq->cv); free(rq);
  }
}

/* Perform a single discovery pass (called from fetch worker context). This mirrors the
 * previous inline logic but runs inside the fetch worker so discovery is serialized
 * with node_db fetches and benefits from backoff/retries if desired.
 */
static void fetch_discover_once(void) {
  char *ud = NULL; size_t udn = 0;
  if (ubnt_discover_output(&ud, &udn) == 0 && ud && udn > 0) {
    char *normalized = NULL; size_t nlen = 0;
    if (normalize_ubnt_devices(ud, &normalized, &nlen) == 0 && normalized) {
      /* transform to legacy to keep h_status_compat expectations */
      char *legacy = NULL; size_t legacy_len = 0;
      if (transform_devices_to_legacy(normalized, &legacy, &legacy_len) == 0 && legacy) {
        pthread_mutex_lock(&g_devices_cache_lock);
        if (g_devices_cache) free(g_devices_cache);
        g_devices_cache = legacy; g_devices_cache_len = legacy_len; g_devices_cache_ts = time(NULL);
        pthread_mutex_unlock(&g_devices_cache_lock);
      } else { if (legacy) free(legacy); }
      free(normalized);
    }
  }
  if (ud) free(ud);
  /* Clear fetch-in-progress so other fetch requests can proceed. Discovery runs
   * inside the centralized fetch worker and borrows the same nodedb fetch lock
   * to serialize network activity; ensure we clear the in-progress flag here
   * exactly as fetch_remote_nodedb does so the worker doesn't deadlock.
   */
  pthread_mutex_lock(&g_nodedb_fetch_lock);
  g_nodedb_fetch_in_progress = 0;
  pthread_cond_broadcast(&g_nodedb_fetch_cv);
  pthread_mutex_unlock(&g_nodedb_fetch_lock);
}

static void *fetch_worker_thread(void *arg) {
  (void)arg;
  while (g_fetch_worker_running) {
    pthread_mutex_lock(&g_fetch_q_lock);
    while (!g_fetch_q_head && g_fetch_worker_running) pthread_cond_wait(&g_fetch_q_cv, &g_fetch_q_lock);
    struct fetch_req *rq = g_fetch_q_head;
    if (rq) {
      g_fetch_q_head = rq->next;
      if (!g_fetch_q_head) g_fetch_q_tail = NULL;
    }
    pthread_mutex_unlock(&g_fetch_q_lock);
    if (!rq) continue;

  /* update processed counters in a thread-safe way */
  /* increment processed counters (use macros that may be atomic) */
  DEBUG_INC_PROCESSED();
  if (rq->type & FETCH_TYPE_NODEDB) DEBUG_INC_PROCESSED_NODEDB();
  if (rq->type & FETCH_TYPE_DISCOVER) DEBUG_INC_PROCESSED_DISCOVER();
  if (g_fetch_log_queue) fprintf(stderr, "[status-plugin] fetch worker: picked request type=%d force=%d wait=%d\n", rq->type, rq->force, rq->wait);
    /* Process the request: dispatch by type. NodeDB fetch and discovery both use the
     * same retry/backoff logic so they benefit from the same robustness.
     */
    int attempt;
    int succeeded = 0;
    for (attempt = 0; attempt < g_fetch_retries; ++attempt) {
      /* Ensure only one fetch-like action runs at a time (protects shared resources) */
      pthread_mutex_lock(&g_nodedb_fetch_lock);
      while (g_nodedb_fetch_in_progress) pthread_cond_wait(&g_nodedb_fetch_cv, &g_nodedb_fetch_lock);
      g_nodedb_fetch_in_progress = 1;
      pthread_mutex_unlock(&g_nodedb_fetch_lock);

      if (rq->type & FETCH_TYPE_DISCOVER) {
        /* discovery action */
        time_t prev = g_devices_cache_ts;
        fetch_discover_once();
        if (g_devices_cache_ts > prev) { succeeded = 1; break; }
      } else {
        /* default: node_db fetch */
        time_t prev = g_nodedb_last_fetch;
        fetch_remote_nodedb();
        if (g_nodedb_last_fetch > prev) { succeeded = 1; break; }
      }

      /* Backoff before next attempt (if any) */
      METRIC_INC_RETRIES();
      if (attempt + 1 < g_fetch_retries) sleep(g_fetch_backoff_initial << attempt);
    }

    if (succeeded) { METRIC_INC_SUCCESS(); }

  /* Notify any waiter(s) attached to this request */
    pthread_mutex_lock(&rq->m);
    rq->done = 1;
    pthread_cond_broadcast(&rq->cv);
    pthread_mutex_unlock(&rq->m);

    /* If caller didn't wait, the worker is responsible for freeing the request */
    if (!rq->wait) {
      pthread_mutex_destroy(&rq->m); pthread_cond_destroy(&rq->cv); free(rq);
    }
    /* otherwise the creator will free after being signaled */
  }
  return NULL;
}

/* Minimal SIGSEGV handler that logs a backtrace to stderr then exits. */
static void sigsegv_handler(int sig) {
#if defined(__APPLE__) || defined(__linux__)
  void *bt[64]; int bt_size = 0;
  bt_size = backtrace(bt, (int)(sizeof(bt)/sizeof(bt[0])));
  if (bt_size > 0) {
    fprintf(stderr, "[status-plugin] caught signal %d (SIGSEGV) - backtrace follows:\n", sig);
    backtrace_symbols_fd(bt, bt_size, STDERR_FILENO);
  } else {
    fprintf(stderr, "[status-plugin] caught signal %d (SIGSEGV) - no backtrace available\n", sig);
  }
#else
  fprintf(stderr, "[status-plugin] caught signal %d (SIGSEGV)\n", sig);
#endif
  /* Restore default and re-raise to produce core/dump behaviour if desired */
  signal(sig, SIG_DFL);
  raise(sig);
}

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
static int h_status_py(http_request_t *r);
static int h_olsr_links(http_request_t *r); static int h_olsr_routes(http_request_t *r); static int h_olsr_raw(http_request_t *r);
static int h_olsr_links_debug(http_request_t *r);
static int h_olsrd_json(http_request_t *r); static int h_capabilities_local(http_request_t *r);
static int h_txtinfo(http_request_t *r); static int h_jsoninfo(http_request_t *r); static int h_olsrd(http_request_t *r);
static int h_discover(http_request_t *r); static int h_embedded_appjs(http_request_t *r); static int h_emb_jquery(http_request_t *r); static int h_emb_bootstrap(http_request_t *r);
static int h_connections(http_request_t *r); static int h_connections_json(http_request_t *r);
static int h_airos(http_request_t *r); static int h_traffic(http_request_t *r); static int h_versions_json(http_request_t *r); static int h_nodedb(http_request_t *r);
static int h_fetch_metrics(http_request_t *r);
static int h_prometheus_metrics(http_request_t *r);
static int h_fetch_debug(http_request_t *r);
static int h_traceroute(http_request_t *r);

/* Forward declarations for device discovery helpers used by background worker */
static int ubnt_discover_output(char **out, size_t *outlen);
static int normalize_ubnt_devices(const char *ud, char **outbuf, size_t *outlen);
static int transform_devices_to_legacy(const char *devices_json, char **out, size_t *out_len);

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
  const size_t MAX_JSON_BUF = 2 * 1024 * 1024; /* 2 MiB */
  if (*lenptr + (size_t)n + 1 > *capptr) {
    while (*capptr < *lenptr + (size_t)n + 1) {
      size_t next = *capptr * 2;
      if (next > MAX_JSON_BUF) next = MAX_JSON_BUF;
      if (next <= *capptr) { free(t); return -1; }
      *capptr = next;
    }
    if (*capptr > MAX_JSON_BUF) { free(t); return -1; }
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

/* Extract the raw JSON value (object/array/string/number) for a given key from a JSON object string.
 * The returned buffer is malloc'ed and must be freed by the caller. This is a minimal, tolerant extractor used
 * only for light-weight compatibility copying of sub-objects from one generated JSON blob to another.
 */
static int extract_json_value(const char *buf, const char *key, char **out, size_t *out_len) {
  if (!buf || !key || !out) return -1;
  *out = NULL; if (out_len) *out_len = 0;
  char pat[256]; snprintf(pat, sizeof(pat), "\"%s\":", key);
  const char *p = strstr(buf, pat);
  if (!p) return -1;
  p += strlen(pat);
  while (*p && isspace((unsigned char)*p)) p++;
  if (!*p) return -1;
  if (*p == '{' || *p == '[') {
    char open = *p; char close = (open == '{') ? '}' : ']';
    const char *start = p; int depth = 0; const char *q = p;
    while (*q) {
      if (*q == open) depth++;
      else if (*q == close) { depth--; if (depth == 0) { q++; break; } }
      q++;
    }
    if (q <= start) return -1;
    size_t L = (size_t)(q - start);
    char *t = malloc(L + 1); if (!t) return -1;
    memcpy(t, start, L); t[L] = '\0'; *out = t; if (out_len) *out_len = L; return 0;
  } else if (*p == '"') {
    const char *start = p; const char *q = p + 1;
    while (*q) {
      if (*q == '\\' && *(q+1)) q += 2;
      else if (*q == '"') { q++; break; }
      else q++;
    }
    size_t L = (size_t)(q - start);
    char *t = malloc(L + 1); if (!t) return -1;
    memcpy(t, start, L); t[L] = '\0'; *out = t; if (out_len) *out_len = L; return 0;
  } else {
    /* number / literal until comma or end */
    const char *start = p; const char *q = p;
    while (*q && *q != ',' && *q != '}' && *q != ']') q++;
    size_t L = (size_t)(q - start);
    while (L > 0 && isspace((unsigned char)start[L-1])) L--;
    if (L == 0) return -1;
    char *t = malloc(L + 1); if (!t) return -1;
    memcpy(t, start, L); t[L] = '\0'; *out = t; if (out_len) *out_len = L; return 0;
  }
}

/* Transform a normalized devices JSON array into the legacy schema expected by bmk-webstatus.py
 * This is a best-effort textual reconstruction using the available normalized fields.
 * Input: devices_json (string containing JSON array of objects)
 * Output: out (malloc'ed JSON array string) and out_len
 */
static int transform_devices_to_legacy(const char *devices_json, char **out, size_t *out_len) {
  if (!devices_json || !out) return -1;
  const char *p = strchr(devices_json, '[');
  if (!p) p = devices_json;
  const char *end = devices_json + strlen(devices_json);
  /* allocate output buffer */
  size_t cap = 4096; size_t len = 0; char *buf = malloc(cap); if(!buf) return -1; buf[0]=0;
  /* start array */
    int first = 1; if (json_buf_append(&buf, &len, &cap, "[") < 0) { free(buf); return -1; }
  const char *q = p;
  while (q && q < end) {
    /* find next object */
    const char *obj = strchr(q, '{'); if (!obj) break;
    const char *cur = obj+1; int depth = 1;
    while (cur < end && depth>0) { if (*cur=='{') depth++; else if (*cur=='}') depth--; cur++; }
    if (depth!=0) break; /* malformed */
    size_t objlen = (size_t)(cur - obj);
    /* create a temporary null-terminated object string */
    char *objbuf = malloc(objlen+1); if(!objbuf) { free(buf); return -1; }
    memcpy(objbuf, obj, objlen); objbuf[objlen]=0;
   /* extract relevant fields. find_json_string_value returns pointers INTO objbuf (not malloc'd),
     so copy each found slice into a strdup'd buffer we own and can free safely. */
   char *hw = NULL; size_t hlen=0; char *ipv4 = NULL; size_t iplen=0; char *firm = NULL; size_t flen=0;
   char *host = NULL; size_t hlo=0; char *prod = NULL; size_t pl=0; char *upt = NULL; size_t uln=0; char *essid = NULL; size_t esn=0;
   char *tmp = NULL; size_t tlen = 0;
   if (find_json_string_value(objbuf, "hwaddr", &tmp, &tlen)) { hw = strndup(tmp, tlen); hlen = tlen; }
   if (find_json_string_value(objbuf, "ipv4", &tmp, &tlen)) { ipv4 = strndup(tmp, tlen); iplen = tlen; }
   /* try both firmware and fwversion */
   if (find_json_string_value(objbuf, "fwversion", &tmp, &tlen)) { firm = strndup(tmp, tlen); flen = tlen; }
   else if (find_json_string_value(objbuf, "firmware", &tmp, &tlen)) { firm = strndup(tmp, tlen); flen = tlen; }
   if (find_json_string_value(objbuf, "hostname", &tmp, &tlen)) { host = strndup(tmp, tlen); hlo = tlen; }
   if (find_json_string_value(objbuf, "product", &tmp, &tlen)) { prod = strndup(tmp, tlen); pl = tlen; }
   if (find_json_string_value(objbuf, "uptime", &tmp, &tlen)) { upt = strndup(tmp, tlen); uln = tlen; }
   if (find_json_string_value(objbuf, "essid", &tmp, &tlen)) { essid = strndup(tmp, tlen); esn = tlen; }

  /* Build one legacy device object */
  if (!first) json_buf_append(&buf, &len, &cap, ",");
  first = 0;
  /* addresses array: include explicit addr/type and hwaddr in a single entry */
  json_buf_append(&buf, &len, &cap, "{");
  /* addresses */
  json_buf_append(&buf, &len, &cap, "\"addresses\":[{");
  if (ipv4 && iplen>0) json_buf_append(&buf, &len, &cap, "\"addr\":\"%.*s\",\"type\":\"ipv4\",", (int)iplen, ipv4);
  else json_buf_append(&buf, &len, &cap, "\"addr\":\"\",\"type\":\"ipv4\",");
  if (hw && hlen>0) json_buf_append(&buf, &len, &cap, "\"hwaddr\":\"%.*s\"", (int)hlen, hw);
  else json_buf_append(&buf, &len, &cap, "\"hwaddr\":\"\"");
  json_buf_append(&buf, &len, &cap, "}],");
    /* copy common shallow fields */
    if (essid && esn>0) json_buf_append(&buf, &len, &cap, "\"essid\":\"%.*s\",", (int)esn, essid); else json_buf_append(&buf, &len, &cap, "\"essid\":\"\",");
    if (firm && flen>0) json_buf_append(&buf, &len, &cap, "\"fwversion\":\"%.*s\",", (int)flen, firm); else json_buf_append(&buf, &len, &cap, "\"fwversion\":\"\",");
    if (host && hlo>0) json_buf_append(&buf, &len, &cap, "\"hostname\":\"%.*s\",", (int)hlo, host); else json_buf_append(&buf, &len, &cap, "\"hostname\":\"\",");
    if (hw && hlen>0) json_buf_append(&buf, &len, &cap, "\"hwaddr\":\"%.*s\",", (int)hlen, hw); else json_buf_append(&buf, &len, &cap, "\"hwaddr\":\"\",");
    if (ipv4 && iplen>0) json_buf_append(&buf, &len, &cap, "\"ipv4\":\"%.*s\",", (int)iplen, ipv4); else json_buf_append(&buf, &len, &cap, "\"ipv4\":\"\",");
    if (prod && pl>0) json_buf_append(&buf, &len, &cap, "\"product\":\"%.*s\",", (int)pl, prod); else json_buf_append(&buf, &len, &cap, "\"product\":\"\",");
    /* uptime numeric fallback */
    int ui = 0; if (upt && uln>0) { ui = atoi(upt); }
    json_buf_append(&buf, &len, &cap, "\"uptime\":%d", ui);
    json_buf_append(&buf, &len, &cap, "}");

  /* cleanup */
  if (objbuf) free(objbuf);
  if (hw) free(hw);
  if (ipv4) free(ipv4);
  if (firm) free(firm);
  if (host) free(host);
  if (prod) free(prod);
  if (upt) free(upt);
  if (essid) free(essid);

    q = cur;
  }
  json_buf_append(&buf, &len, &cap, "]");
  *out = buf; if (out_len) *out_len = len; return 0;
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
  /* Fallback: if no object entries matched, try legacy array-of-strings under "routes" key */
  /* no legacy array-of-strings fallback here */
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
/* TTL-aware wrapper: only fetch if cache is stale or empty */
/* forward-declare actual fetch implementation so wrapper can call it */
static void fetch_remote_nodedb(void);

/* Helper used by libcurl to collect response data into a growing buffer */
#ifdef HAVE_LIBCURL
struct curl_fetch {
  char *buf;
  size_t len;
};

static size_t curl_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
  struct curl_fetch *cf = (struct curl_fetch*)userdata;
  size_t add = size * nmemb;
  char *nb = realloc(cf->buf, cf->len + add + 1);
  if (!nb) return 0;
  cf->buf = nb;
  memcpy(cf->buf + cf->len, ptr, add);
  cf->len += add;
  cf->buf[cf->len] = '\0';
  return add;
}
#endif

/* RFC1123 time formatter for HTTP Last-Modified header */
static void format_rfc1123_time(time_t t, char *out, size_t outlen) {
  if (!out || outlen==0) return;
  struct tm tm;
  if (gmtime_r(&t, &tm) == NULL) { out[0]=0; return; }
  /* Example: Sun, 06 Nov 1994 08:49:37 GMT */
  strftime(out, outlen, "%a, %d %b %Y %H:%M:%S GMT", &tm);
}

static void fetch_remote_nodedb_if_needed(void) {
  time_t now = time(NULL);
  pthread_mutex_lock(&g_nodedb_lock);
  int need = 0;
  if (!g_nodedb_cached || g_nodedb_cached_len == 0) need = 1;
  else if (g_nodedb_last_fetch == 0) need = 1;
  else if ((now - g_nodedb_last_fetch) >= g_nodedb_ttl) need = 1;
  pthread_mutex_unlock(&g_nodedb_lock);
  if (!need) return;
  /* enqueue an asynchronous fetch request (do not block caller) */
  enqueue_fetch_request(0, 0, FETCH_TYPE_NODEDB);
}

static void fetch_remote_nodedb(void) {
  char ipbuf[128]=""; get_primary_ipv4(ipbuf,sizeof(ipbuf)); if(!ipbuf[0]) snprintf(ipbuf,sizeof(ipbuf),"0.0.0.0");
  time_t entry_t = time(NULL);
  fprintf(stderr, "[status-plugin] nodedb fetch: entry (ts=%ld) url=%s\n", (long)entry_t, g_nodedb_url);
  char *fresh=NULL; size_t fn=0;
  /* If this is the very first fetch since plugin start, the container
   * networking (DNS/routes) may not be ready yet. Wait a short time for
   * DNS to resolve the nodedb host before attempting the fetch so
   * transient startup failures are avoided. This waits up to 30 seconds.
   */
  if (g_nodedb_last_fetch == 0 && g_nodedb_url[0]) {
    char hostbuf[256] = "";
    const char *u = g_nodedb_url;
    const char *hstart = strstr(u, "://");
    if (hstart) hstart += 3; else hstart = u;
    const char *hend = strchr(hstart, '/');
    size_t hlen = hend ? (size_t)(hend - hstart) : strlen(hstart);
    /* strip optional :port */
    const char *colon = memchr(hstart, ':', hlen);
    if (colon) hlen = (size_t)(colon - hstart);
    if (hlen > 0 && hlen < sizeof(hostbuf)) {
      memcpy(hostbuf, hstart, hlen);
      hostbuf[hlen] = '\0';
      int waited = 0;
      for (int i = 0; i < 30; ++i) {
        struct addrinfo hints, *res = NULL;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM;
        if (getaddrinfo(hostbuf, NULL, &hints, &res) == 0) {
          if (res) freeaddrinfo(res);
          if (i > 0) fprintf(stderr, "[status-plugin] nodedb fetch: DNS became available after %d seconds\n", i);
          break;
        }
        fprintf(stderr, "[status-plugin] nodedb fetch: waiting for network/DNS to become available (%s) (%d/30)\n", hostbuf, i+1);
        sleep(1);
        waited++;
      }
      if (waited >= 30) fprintf(stderr, "[status-plugin] nodedb fetch: proceeding after timeout waiting for DNS (%s)\n", hostbuf);
    }
  }
  
  /* Prefer internal HTTP fetch for plain http:// URLs to avoid spawning curl. */
  int success = 0;
  if (strncmp(g_nodedb_url, "http://", 7) == 0) {
    fprintf(stderr, "[status-plugin] nodedb fetch: attempting internal HTTP fetch %s\n", g_nodedb_url);
    int rc = util_http_get_url(g_nodedb_url, &fresh, &fn, 5);
    fprintf(stderr, "[status-plugin] nodedb fetch: internal_http rc=%d bytes=%zu\n", rc, fn);
    if (rc == 0 && fresh && buffer_has_content(fresh,fn) && validate_nodedb_json(fresh,fn)) {
      fprintf(stderr, "[status-plugin] nodedb fetch: method=internal_http success, got %zu bytes\n", fn);
      success = 1;
    } else {
      if (fresh) { free(fresh); fresh = NULL; fn = 0; }
      fprintf(stderr, "[status-plugin] nodedb fetch: internal_http failed or invalid JSON\n");
    }
  }
  /* If not successful and URL is https or internal fetch failed, try libcurl first, then fall back to spawning curl if available */
  if (!success) {
#ifdef HAVE_LIBCURL
  /* libcurl attempt (if detected at build time) */
  fprintf(stderr, "[status-plugin] nodedb fetch: attempting libcurl fetch %s\n", g_nodedb_url);
    CURL *c = curl_easy_init();
    if (c) {
      struct curl_fetch cf = { NULL, 0 };
      curl_easy_setopt(c, CURLOPT_URL, g_nodedb_url);
      curl_easy_setopt(c, CURLOPT_TIMEOUT, 5L);
      curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
      curl_easy_setopt(c, CURLOPT_USERAGENT, "status-plugin");
      struct curl_slist *hdr = NULL; hdr = curl_slist_append(hdr, "Accept: application/json"); curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdr);
      curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
      curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
      curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_write_cb);
      curl_easy_setopt(c, CURLOPT_WRITEDATA, &cf);
      CURLcode cres = curl_easy_perform(c);
      curl_slist_free_all(hdr);
      curl_easy_cleanup(c);
      if (cres == CURLE_OK && cf.buf && cf.len > 0 && validate_nodedb_json(cf.buf, cf.len)) {
        fresh = cf.buf; fn = cf.len; success = 1; fprintf(stderr, "[status-plugin] nodedb fetch: method=libcurl success, got %zu bytes\n", fn);
      } else {
        if (cf.buf) free(cf.buf);
        fprintf(stderr, "[status-plugin] nodedb fetch: method=libcurl failed (curl code=%d)\n", (int)cres);
      }
    } else {
      fprintf(stderr, "[status-plugin] nodedb fetch: libcurl init failed\n");
    }
#endif

#ifndef NO_CURL_FALLBACK
    if (!success) {
      const char *curl_paths[] = {"/usr/bin/curl", "/bin/curl", "/usr/local/bin/curl", "curl", NULL};
      for (const char **curl_path = curl_paths; *curl_path && !success; curl_path++) {
        fprintf(stderr, "[status-plugin] nodedb fetch: attempting external curl at %s\n", *curl_path);
        char cmd[1024];
        snprintf(cmd,sizeof(cmd),"%s -s --max-time 5 -H \"User-Agent: status-plugin OriginIP/%s\" -H \"Accept: application/json\" %s", *curl_path, ipbuf, g_nodedb_url);
        if (util_exec(cmd,&fresh,&fn)==0 && fresh && buffer_has_content(fresh,fn) && validate_nodedb_json(fresh,fn)) {
          fprintf(stderr, "[status-plugin] nodedb fetch: method=external_curl success with %s, got %zu bytes\n", *curl_path, fn);
          success = 1; break;
        } else { if (fresh) { free(fresh); fresh = NULL; fn = 0; } }
      }
    }
#else
    if (!success) {
      fprintf(stderr, "[status-plugin] nodedb fetch: external curl fallback is DISABLED at build time\n");
    }
#endif
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
    /* write a copy for external inspection if explicitly enabled (avoid frequent flash writes) */
    if (g_nodedb_write_disk) {
      FILE *wf=fopen("/tmp/node_db.json","w"); if(wf){ fwrite(g_nodedb_cached,1,g_nodedb_cached_len,wf); fclose(wf);} 
    }
    fresh=NULL;
  } else if (fresh) { free(fresh); }
    else { fprintf(stderr,"[status-plugin] nodedb fetch failed or invalid (%s)\n", g_nodedb_url); }
  /* Clear fetch-in-progress and notify any waiters so they can re-check cache. */
  pthread_mutex_lock(&g_nodedb_fetch_lock);
  g_nodedb_fetch_in_progress = 0;
  pthread_cond_broadcast(&g_nodedb_fetch_cv);
  pthread_mutex_unlock(&g_nodedb_fetch_lock);
}

/* Improved unique-destination counting: counts distinct destination nodes reachable via given last hop. */
static int normalize_olsrd_links(const char *raw, char **outbuf, size_t *outlen) {
  if (!raw || !outbuf || !outlen) return -1;
  *outbuf = NULL; *outlen = 0;
  /* Fetch remote node_db only if cache is stale or empty */
  fetch_remote_nodedb_if_needed();
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
        /* use local HTTP helper when endpoint is loopback */
        if (strstr(rt_cmds[ci], "127.0.0.1") || strstr(rt_cmds[ci], "localhost")) {
          if (util_http_get_url_local(rt_cmds[ci], &rt_raw, &rlen, 1) == 0 && rt_raw && rlen>0) break;
          if (rt_raw) { free(rt_raw); rt_raw=NULL; rlen=0; }
        } else {
          if (util_exec(rt_cmds[ci], &rt_raw, &rlen) == 0 && rt_raw && rlen>0) break;
          if (rt_raw) { free(rt_raw); rt_raw=NULL; rlen=0; }
        }
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
  /* Safety limits to avoid pathological searches on large or malicious inputs */
  const size_t MAX_SEARCH = 256 * 1024; /* 256 KiB */
  const size_t MAX_VALUE_LEN = 4096; /* cap returned value length */
  char needle[128]; if ((size_t)snprintf(needle, sizeof(needle), "\"%s\"", key) >= sizeof(needle)) return 0;
  const char *p = start; const char *search_end = start + MAX_SEARCH;
  /* find buffer true end by looking for terminating NUL if present and shorten search_end accordingly */
  const char *nul = memchr(start, '\0', MAX_SEARCH);
  if (nul) search_end = nul;
  while (p < search_end && (p = strstr(p, needle)) != NULL) {
    const char *q = p + strlen(needle);
    /* skip whitespace */ while (q < search_end && (*q==' '||*q=='\t'||*q=='\r'||*q=='\n')) q++;
    if (q >= search_end || *q != ':') { p = q; continue; }
    q++; while (q < search_end && (*q==' '||*q=='\t'||*q=='\r'||*q=='\n')) q++;
    if (q >= search_end) return 0;
    if (*q == '"') {
      q++; const char *vstart = q; const char *r = q;
      while (r < search_end && *r) {
        if (*r == '\\' && (r + 1) < search_end) { r += 2; continue; }
        if (*r == '"') {
          size_t vlen = (size_t)(r - vstart);
          if (vlen > MAX_VALUE_LEN) vlen = MAX_VALUE_LEN;
          *val = (char*)vstart; *val_len = vlen; return 1;
        }
        r++;
      }
      return 0;
    } else {
      /* not a quoted string: capture until comma or closing brace */
      const char *vstart = q; const char *r = q;
      while (r < search_end && *r && *r != ',' && *r != '}' && *r != '\n') r++;
      while (r > vstart && (*(r-1)==' '||*(r-1)=='\t')) r--;
      size_t vlen = (size_t)(r - vstart);
      if (vlen > MAX_VALUE_LEN) vlen = MAX_VALUE_LEN;
      *val = (char*)vstart; *val_len = vlen; return 1;
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
static void detect_olsr_processes(int *out_olsrd, int *out_olsr2);
/* forward decls for stderr capture and log handler implemented later */
static int start_stderr_capture(void);
static void stop_stderr_capture(void);
static int h_log(http_request_t *r);
static void detect_olsr_processes(int *out_olsrd, int *out_olsr2);

/* Generate versions JSON into an allocated buffer (caller frees) */
static int generate_versions_json(char **outbuf, size_t *outlen) {
  if (!outbuf || !outlen) return -1;
  *outbuf = NULL; *outlen = 0;
  /* short-lived cache to avoid expensive regeneration when handlers call this repeatedly */
  static char *versions_cache = NULL; static size_t versions_cache_n = 0; static time_t versions_cache_ts = 0;
  static pthread_mutex_t versions_cache_lock = PTHREAD_MUTEX_INITIALIZER;
  const time_t TTL = 2; /* seconds */
  time_t now = time(NULL);
  /* if cache is fresh, return a duplicated copy to the caller */
  pthread_mutex_lock(&versions_cache_lock);
  if (versions_cache && versions_cache_n > 0 && (now - versions_cache_ts) < TTL) {
    *outbuf = strdup(versions_cache);
    if (*outbuf) *outlen = versions_cache_n;
    pthread_mutex_unlock(&versions_cache_lock);
    return 0;
  }
  pthread_mutex_unlock(&versions_cache_lock);
  char host[256] = ""; gethostname(host, sizeof(host)); host[sizeof(host)-1]=0;
  int olsrd_on=0, olsr2_on=0; detect_olsr_processes(&olsrd_on,&olsr2_on);

  /* autoupdate wizard info */
  const char *au_path = "/etc/cron.daily/autoupdatewizards";
  int auon = path_exists(au_path);
  char *adu_dat = NULL; size_t adu_n = 0;
  util_read_file("/config/user-data/autoupdate.dat", &adu_dat, &adu_n);
  int aa_on = 0, aa1_on = 0, aa2_on = 0, aale_on = 0, aaebt_on = 0, aabp_on = 0;
  if (adu_dat && adu_n>0) {
    if (memmem(adu_dat, adu_n, "wizard-autoupdate=yes", 20)) aa_on = 1;
    if (memmem(adu_dat, adu_n, "wizard-olsrd_v1=yes", 19)) aa1_on = 1;
    if (memmem(adu_dat, adu_n, "wizard-olsrd_v2=yes", 19)) aa2_on = 1;
    if (memmem(adu_dat, adu_n, "wizard-0xffwsle=yes", 18)) aale_on = 1;
    if (memmem(adu_dat, adu_n, "wizard-ebtables=yes", 18)) aaebt_on = 1;
    if (memmem(adu_dat, adu_n, "wizard-blockPrivate=yes", 24)) aabp_on = 1;
  }

  /* homes */
  char *homes_out = NULL; size_t homes_n = 0;
  if (util_exec("/bin/ls -1 /home 2>/dev/null | awk '{printf \"\\\"%s\\\",\", $0}' | sed 's/,$/\\n/'", &homes_out, &homes_n) != 0) {
    if (homes_out) { free(homes_out); homes_out = NULL; homes_n = 0; }
  }
  if (!homes_out) { homes_out = strdup("\n"); homes_n = homes_out ? strlen(homes_out) : 0; }

  /* md5 */
  char *md5_out = NULL; size_t md5_n = 0;
  if (util_exec("/usr/bin/md5sum /dev/mtdblock2 2>/dev/null | cut -f1 -d' '", &md5_out, &md5_n) != 0) { if (md5_out) { free(md5_out); md5_out = NULL; md5_n = 0; } }

  const char *system_type = path_exists("/config/wizard") ? "edge-router" : "linux-container";

  /* bmk-webstatus */
  char *bmk_out = NULL; size_t bmk_n = 0; char bmkwebstatus[128] = "n/a";
  if (util_exec("head -n 12 /config/custom/www/cgi-bin-status*.php 2>/dev/null | grep -m1 version= | cut -d'\"' -f2", &bmk_out, &bmk_n) == 0 && bmk_out && bmk_n>0) {
    char *t = strndup(bmk_out, (size_t)bmk_n); if (t) { char *nl = strchr(t,'\n'); if (nl) *nl = 0; strncpy(bmkwebstatus, t, sizeof(bmkwebstatus)-1); free(t); }
  }

  /* olsrd4watchdog */
  int olsrd4watchdog = 0; char *olsrd4conf = NULL; size_t olsrd4_n = 0;
  if (util_read_file("/config/user-data/olsrd4.conf", &olsrd4conf, &olsrd4_n) != 0) {
    if (util_read_file("/etc/olsrd/olsrd.conf", &olsrd4conf, &olsrd4_n) != 0) { olsrd4conf = NULL; olsrd4_n = 0; }
  }
  if (olsrd4conf && olsrd4_n>0) { if (memmem(olsrd4conf, olsrd4_n, "olsrd_watchdog", 13) || memmem(olsrd4conf, olsrd4_n, "LoadPlugin.*olsrd_watchdog", 22)) olsrd4watchdog = 1; free(olsrd4conf); }

  /* ips */
  char ipv4_addr[64] = "n/a", ipv6_addr[128] = "n/a";
  char *tmp_out = NULL; size_t tmp_n = 0;
  if (util_exec("ip -4 -o addr show scope global | awk '{print $4; exit}' | cut -d/ -f1", &tmp_out, &tmp_n) == 0 && tmp_out && tmp_n>0) { char *t = strndup(tmp_out, tmp_n); if (t) { char *nl = strchr(t,'\n'); if (nl) *nl = 0; strncpy(ipv4_addr, t, sizeof(ipv4_addr)-1); free(t); } free(tmp_out); tmp_out=NULL; tmp_n=0; }
  if (util_exec("ip -6 -o addr show scope global | awk '{print $4; exit}' | cut -d/ -f1", &tmp_out, &tmp_n) == 0 && tmp_out && tmp_n>0) { char *t = strndup(tmp_out, tmp_n); if (t) { char *nl = strchr(t,'\n'); if (nl) *nl = 0; strncpy(ipv6_addr, t, sizeof(ipv6_addr)-1); free(t); } free(tmp_out); tmp_out=NULL; tmp_n=0; }

  /* linkserial */
  char linkserial[128] = "n/a"; char *ll_out = NULL; size_t ll_n = 0;
  if (util_exec("ip -6 link show eth0 2>/dev/null | grep link/ether | awk '{gsub(\":\",\"\", $2); print toupper($2)}'", &ll_out, &ll_n) == 0 && ll_out && ll_n>0) {
    char *t = strndup(ll_out, ll_n);
    if (t) { char *nl = strchr(t,'\n'); if (nl) *nl=0; strncpy(linkserial, t, sizeof(linkserial)-1); free(t); }
    if (ll_out) free(ll_out);
  }

  /* Attempt to extract olsrd binary/version information (best-effort). Keep fields small and safe. */
  char olsrd_ver[256] = "";
  char olsrd_desc[256] = "";
  char olsrd_dev[128] = "";
  char olsrd_date[64] = "";
  char olsrd_rel[64] = "";
  char olsrd_src[256] = "";
  char *ols_out = NULL; size_t ols_n = 0;
  if (util_exec("grep -oaEm1 'olsr.org - .{1,200}' /usr/sbin/olsrd 2>/dev/null", &ols_out, &ols_n) == 0 && ols_out && ols_n>0) {
    char *s = strndup(ols_out, ols_n);
    if (s) {
      for (char *p = s; *p; ++p) { if ((unsigned char)*p < 0x20) *p = ' '; }
      /* trim leading/trailing spaces */
      char *st = s; while (*st && isspace((unsigned char)*st)) st++;
      char *en = s + strlen(s) - 1; while (en > st && isspace((unsigned char)*en)) *en-- = '\0';
      strncpy(olsrd_ver, st, sizeof(olsrd_ver)-1); olsrd_ver[sizeof(olsrd_ver)-1] = '\0';
      /* try to split into version and desc at first ' - ' occurrence */
      char *dash = strstr(olsrd_ver, " - ");
      if (dash) {
        *dash = '\0'; dash += 3;
        strncpy(olsrd_desc, dash, sizeof(olsrd_desc)-1); olsrd_desc[sizeof(olsrd_desc)-1]=0;
      }
      free(s);
    }
    free(ols_out); ols_out = NULL; ols_n = 0;
  }

  /* Build JSON */
  size_t buf_sz = 4096 + (homes_n>0?homes_n:0) + (md5_n>0?md5_n:0);
  char *obuf = malloc(buf_sz);
  if (!obuf) { if (adu_dat) free(adu_dat); if (homes_out) free(homes_out); if (md5_out) free(md5_out); return -1; }
  char homes_json[512] = "[]";
  if (homes_out && homes_n>0) { size_t hn = homes_n; char *tmp = strndup(homes_out, homes_n); if (tmp) { while (hn>0 && (tmp[hn-1]=='\n' || tmp[hn-1]==',')) { tmp[--hn]=0; } snprintf(homes_json,sizeof(homes_json),"[%s]", tmp[0]?tmp:"" ); free(tmp); } }
  char bootimage_md5[128] = "n/a"; if (md5_out && md5_n>0) { char *m = strndup(md5_out, md5_n); if (m) { char *nl = strchr(m,'\n'); if (nl) *nl=0; strncpy(bootimage_md5, m, sizeof(bootimage_md5)-1); free(m); } }
  snprintf(obuf, buf_sz,
    "{\"host\":\"%s\",\"system\":\"%s\",\"olsrd_running\":%s,\"olsr2_running\":%s,\"olsrd4watchdog\":%s,\"autoupdate_wizards_installed\":\"%s\",\"autoupdate_settings\":{\"auto_update_enabled\":%s,\"olsrd_v1\":%s,\"olsrd_v2\":%s,\"wsle\":%s,\"ebtables\":%s,\"blockpriv\":%s},\"homes\":%s,\"bootimage\":{\"md5\":\"%s\"},\"bmk_webstatus\":\"%s\",\"ipv4\":\"%s\",\"ipv6\":\"%s\",\"linkserial\":\"%s\",\"olsrd\":\"%s\",\"olsrd_details\":{\"version\":\"%s\",\"description\":\"%s\",\"device\":\"%s\",\"date\":\"%s\",\"release\":\"%s\",\"source\":\"%s\"}}\n",
    host,
    system_type,
    olsrd_on?"true":"false",
    olsr2_on?"true":"false",
    olsrd4watchdog?"true":"false",
    auon?"yes":"no",
    aa_on?"true":"false",
    aa1_on?"true":"false",
    aa2_on?"true":"false",
    aale_on?"true":"false",
    aaebt_on?"true":"false",
    aabp_on?"true":"false",
    homes_json,
    bootimage_md5,
    bmkwebstatus,
    ipv4_addr,
    ipv6_addr,
    linkserial,
    olsrd_ver[0]?olsrd_ver:"",
    olsrd_ver[0]?olsrd_ver:"",
    olsrd_desc[0]?olsrd_desc:"",
    olsrd_dev[0]?olsrd_dev:"",
    olsrd_date[0]?olsrd_date:"",
    olsrd_rel[0]?olsrd_rel:"",
    olsrd_src[0]?olsrd_src:""
  );

  if (adu_dat) free(adu_dat);
  if (homes_out) free(homes_out);
  if (md5_out) free(md5_out);
  *outbuf = obuf; *outlen = strlen(obuf);
  /* update cache (store a copy) */
  pthread_mutex_lock(&versions_cache_lock);
  if (versions_cache) { free(versions_cache); versions_cache = NULL; versions_cache_n = 0; }
  versions_cache = strdup(obuf);
  if (versions_cache) versions_cache_n = *outlen; else versions_cache_n = 0;
  versions_cache_ts = time(NULL);
  pthread_mutex_unlock(&versions_cache_lock);
  return 0;
}

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
  #define APPEND(fmt,...) do { if (json_appendf(&buf, &len, &cap, fmt, ##__VA_ARGS__) != 0) { free(buf); send_json(r,"{}\n"); return 0; } } while(0)
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

  /* Generate versions JSON early (reusable helper) */
  char *vgen = NULL; size_t vgen_n = 0;
  (void)vgen_n;
  if (generate_versions_json(&vgen, &vgen_n) != 0) { if (vgen) { free(vgen); vgen = NULL; vgen_n = 0; } }

  /* fetch queue and metrics */
  int qlen = 0; struct fetch_req *fit = NULL; unsigned long m_d=0, m_r=0, m_s=0;
  pthread_mutex_lock(&g_fetch_q_lock);
  fit = g_fetch_q_head; while (fit) { qlen++; fit = fit->next; }
  pthread_mutex_unlock(&g_fetch_q_lock);
  METRIC_LOAD_ALL(m_d, m_r, m_s);

  /* default route */
  char def_ip[64] = ""; char def_dev[64] = ""; char *rout=NULL; size_t rn=0;
  if (util_exec("/sbin/ip route show default 2>/dev/null || /usr/sbin/ip route show default 2>/dev/null || ip route show default 2>/dev/null", &rout,&rn)==0 && rout) {
    char *p=strstr(rout,"via "); if(p){ p+=4; char *q=strchr(p,' '); if(q){ size_t L=q-p; if(L<sizeof(def_ip)){ strncpy(def_ip,p,L); def_ip[L]=0; } } }
    p=strstr(rout," dev "); if(p){ p+=5; char *q=strchr(p,' '); if(!q) q=strchr(p,'\n'); if(q){ size_t L=q-p; if(L<sizeof(def_dev)){ strncpy(def_dev,p,L); def_dev[L]=0; } } }
    free(rout);
  }

  {
    unsigned long _de=0,_den=0,_ded=0,_dp=0,_dpn=0,_dpd=0;
    DEBUG_LOAD_ALL(_de,_den,_ded,_dp,_dpn,_dpd);
    APPEND("\"fetch_stats\":{\"queue_length\":%d,\"dropped\":%lu,\"retries\":%lu,\"successes\":%lu,\"enqueued\":%lu,\"enqueued_nodedb\":%lu,\"enqueued_discover\":%lu,\"processed\":%lu,\"processed_nodedb\":%lu,\"processed_discover\":%lu,\"thresholds\":{\"queue_warn\":%d,\"queue_crit\":%d,\"dropped_warn\":%d}},", qlen, m_d, m_r, m_s, _de, _den, _ded, _dp, _dpn, _dpd, g_fetch_queue_warn, g_fetch_queue_crit, g_fetch_dropped_warn);
  }
  /* include suggested UI autos-refresh ms */
  APPEND("\"fetch_auto_refresh_ms\":%d,", g_fetch_auto_refresh_ms);


  int olsr2_on=0, olsrd_on=0; detect_olsr_processes(&olsrd_on,&olsr2_on);
  if(olsr2_on) fprintf(stderr,"[status-plugin] detected olsrd2 (robust)\n");
  if(olsrd_on) fprintf(stderr,"[status-plugin] detected olsrd (robust)\n");
  if(!olsrd_on && !olsr2_on) fprintf(stderr,"[status-plugin] no OLSR process detected (robust path)\n");

  /* fetch links (for either implementation); do not toggle olsr2_on based on HTTP success */
  char *olsr_links_raw=NULL; size_t oln=0; {
    const char *endpoints[]={"http://127.0.0.1:9090/links","http://127.0.0.1:2006/links","http://127.0.0.1:8123/links",NULL};
    for(const char **ep=endpoints; *ep && !olsr_links_raw; ++ep){
      fprintf(stderr,"[status-plugin] trying OLSR endpoint: %s\n", *ep);
      if (strstr(*ep, "127.0.0.1") || strstr(*ep, "localhost")) {
        if (util_http_get_url_local(*ep, &olsr_links_raw, &oln, 1) == 0 && olsr_links_raw && oln > 0) {
          fprintf(stderr,"[status-plugin] fetched OLSR links from %s (%zu bytes)\n", *ep, oln);
          break;
        }
      } else {
        char cmd[256]; snprintf(cmd,sizeof(cmd),"/usr/bin/curl -s --max-time 1 %s", *ep);
        if(util_exec(cmd,&olsr_links_raw,&oln)==0 && olsr_links_raw && oln>0){
          fprintf(stderr,"[status-plugin] fetched OLSR links from %s (%zu bytes)\n", *ep, oln);
          break;
        }
      }
      if(olsr_links_raw){ free(olsr_links_raw); olsr_links_raw=NULL; oln=0; }
    }
  }

  /* (legacy duplicate fetch block removed after refactor) */

  char *olsr_neighbors_raw=NULL; size_t olnn=0; if(util_http_get_url_local("http://127.0.0.1:9090/neighbors", &olsr_neighbors_raw, &olnn, 1) != 0) { if(olsr_neighbors_raw){ free(olsr_neighbors_raw); olsr_neighbors_raw=NULL; } olnn=0; }
  char *olsr_routes_raw=NULL; size_t olr=0; if(util_http_get_url_local("http://127.0.0.1:9090/routes", &olsr_routes_raw, &olr, 1) != 0) { if(olsr_routes_raw){ free(olsr_routes_raw); olsr_routes_raw=NULL; } olr=0; }
  char *olsr_topology_raw=NULL; size_t olt=0; if(util_http_get_url_local("http://127.0.0.1:9090/topology", &olsr_topology_raw, &olt, 1) != 0) { if(olsr_topology_raw){ free(olsr_topology_raw); olsr_topology_raw=NULL; } olt=0; }

  /* Build JSON */
  APPEND("{");
  APPEND("\"hostname\":"); json_append_escaped(&buf,&len,&cap,hostname); APPEND(",");
  APPEND("\"ip\":"); json_append_escaped(&buf,&len,&cap,ipaddr); APPEND(",");
  APPEND("\"uptime\":\"%ld\",", uptime_seconds);

  /* default_route */
  /* attempt reverse DNS for default route IP to provide a hostname for the gateway */
  /* Try EdgeRouter path first */
  /* versions: use internal generator rather than external script (we generated vgen earlier) */
  if (vgen && vgen_n > 0) {
    APPEND("\"versions\":%s,", vgen);
  } else {
    /* fallback: try a quick generation (rare) */
    char *vtmp = NULL; size_t vtmp_n = 0;
    if (generate_versions_json(&vtmp, &vtmp_n) == 0 && vtmp && vtmp_n>0) {
      APPEND("\"versions\":%s,", vtmp);
      free(vtmp); vtmp = NULL;
    } else {
      /* Provide basic fallback versions for Linux container */
      APPEND("\"versions\":{\"olsrd\":\"unknown\",\"system\":\"linux-container\"},");
    }
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
  /* Devices: prefer cached devices populated by background worker to avoid blocking */
  {
    int used_cache = 0;
    pthread_mutex_lock(&g_devices_cache_lock);
    if (g_devices_cache && g_devices_cache_len > 0) {
      APPEND("\"devices\":%s,", g_devices_cache);
      used_cache = 1;
    }
    pthread_mutex_unlock(&g_devices_cache_lock);
    if (!used_cache) {
      /* fallback to inline discovery if cache not ready */
      char *ud = NULL; size_t udn = 0;
      if (ubnt_discover_output(&ud, &udn) == 0 && ud && udn > 0) {
        fprintf(stderr, "[status-plugin] got device data from ubnt-discover (inline %zu bytes)\n", udn);
        char *normalized = NULL; size_t nlen = 0;
        if (normalize_ubnt_devices(ud, &normalized, &nlen) == 0 && normalized) {
          APPEND("\"devices\":%s,", normalized);
          free(normalized);
        } else {
          APPEND("\"devices\":[],");
        }
        free(ud);
      } else {
        APPEND("\"devices\":[],");
      }
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
  /* legacy compatibility: expose olsrd4watchdog object with state on/off to match bmk-webstatus style */
  APPEND(",\"olsrd4watchdog\":{\"state\":\"%s\"}", olsrd_on?"on":"off");

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
      char *tout = NULL; size_t tlen = 0; int ok = 0;
      if (strstr(*ep, "127.0.0.1") || strstr(*ep, "localhost")) {
        if (util_http_get_url_local(*ep, &tout, &tlen, 1) == 0 && tout && tlen>0) ok = 1;
      } else {
        char cmd[256]; snprintf(cmd, sizeof(cmd), "/usr/bin/curl -s --max-time 1 %s", *ep);
        if (util_exec(cmd, &tout, &tlen) == 0 && tout && tlen>0) ok = 1;
      }
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

  /* Compatibility: add some legacy top-level keys expected by bmk-webstatus.py output
   * We try to copy relevant sub-objects from the previously generated versions JSON or other local buffers.
   */
  {
    /* airosdata: use airos_raw if present, otherwise empty object */
    if (airos_raw && airos_n>0) {
      APPEND(",\"airosdata\":%s", airos_raw);
    } else {
      APPEND(",\"airosdata\":{}" );
    }

    /* autoupdate: attempt to extract autoupdate_settings from versions JSON above (we already appended versions)
     * For simplicity, re-generate versions JSON into vtmp and extract keys. generate_versions_json already freed its buffer
     * earlier, so we re-run it here to fetch the structured object. This is slightly redundant but safe.
     */
    char *vtmp = NULL; size_t vtmp_n = 0; int vtmp_owned = 0;
    if (vgen && vgen_n > 0) {
      vtmp = vgen; vtmp_n = vgen_n; vtmp_owned = 0;
    } else if (generate_versions_json(&vtmp, &vtmp_n) == 0 && vtmp && vtmp_n>0) {
      vtmp_owned = 1;
    }
    if (vtmp && vtmp_n > 0) {
      char *autoup = NULL; size_t alen = 0;
      if (extract_json_value(vtmp, "autoupdate_settings", &autoup, &alen) == 0 && autoup) {
        APPEND(",\"autoupdate\":%s", autoup);
        free(autoup);
      } else {
        APPEND(",\"autoupdate\":{}" );
      }
      /* wizards boolean copied as 'wizards' top-level key (legacy) */
      char *wiz = NULL; if (extract_json_value(vtmp, "autoupdate_wizards_installed", &wiz, NULL)==0 && wiz) {
        /* value in versions is a string; keep as string to avoid surprises */
        APPEND(",\"wizards\":%s", wiz);
        free(wiz);
      } else {
        APPEND(",\"wizards\":\"no\"");
      }
      if (vtmp_owned) { free(vtmp); vtmp = NULL; }
    } else {
      APPEND(",\"autoupdate\":{}" );
      APPEND(",\"wizards\":\"no\"");
    }

  /* homes and bootimage: emit simple defaults (generate_versions_json has its own detailed output elsewhere) */
  APPEND(",\"homes\":[]");
  APPEND(",\"bootimage\":{\"md5\":\"n/a\"}");

  /* linklocals and local_ips: emit empty placeholders for compatibility */
  APPEND(",\"local_ips\":[]");
  APPEND(",\"linklocals\":[]");
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

/* Emit reduced, bmk-webstatus.py-compatible JSON payload for remote collectors
 * Contains a small set of top-level keys expected by the legacy Python script.
 */
static int h_status_compat(http_request_t *r) {
  char *airos_raw = NULL; size_t airos_n = 0; util_read_file("/tmp/10-all.json", &airos_raw, &airos_n);
  /* versions/autoupdate/wizards */
  char *vgen = NULL; size_t vgen_n = 0; if (generate_versions_json(&vgen, &vgen_n) != 0) { if (vgen) { free(vgen); vgen = NULL; vgen_n = 0; } }

  char *out = NULL; size_t outcap = 4096, outlen = 0; out = malloc(outcap); if(!out){ send_json(r, "{}\n"); if(airos_raw) free(airos_raw); if(vgen) free(vgen); return 0; } out[0]=0;
  /* helper to append safely */
  #define CAPPEND(fmt,...) do { if (json_appendf(&out, &outlen, &outcap, fmt, ##__VA_ARGS__) != 0) { free(out); send_json(r,"{}\n"); if(airos_raw) free(airos_raw); if(vgen) free(vgen); return 0; } } while(0)

  CAPPEND("{");
  /* airosdata */
  if (airos_raw && airos_n>0) CAPPEND("\"airosdata\":%s", airos_raw); else CAPPEND("\"airosdata\":{}");

  /* autoupdate + wizards from versions JSON if available */
  if (vgen && vgen_n>0) {
    char *autoup = NULL; size_t alen = 0;
    if (extract_json_value(vgen, "autoupdate_settings", &autoup, &alen) == 0 && autoup) {
      CAPPEND(",\"autoupdate\":%s", autoup);
      free(autoup);
    } else {
      CAPPEND(",\"autoupdate\":{}");
    }
    char *wiz = NULL; if (extract_json_value(vgen, "autoupdate_wizards_installed", &wiz, NULL) == 0 && wiz) { CAPPEND(",\"wizards\":%s", wiz); free(wiz); } else { CAPPEND(",\"wizards\":\"no\""); }
  } else {
    CAPPEND(",\"autoupdate\":{}"); CAPPEND(",\"wizards\":\"no\"");
  }

  /* bootimage minimal: try to extract md5 from generated versions JSON */
  if (vgen && vgen_n>0) {
    char *bm = NULL; size_t bml = 0;
    if (extract_json_value(vgen, "bootimage", &bm, &bml) == 0 && bm) {
      /* bm is an object like {\"md5\":\"...\"} – reuse it directly */
      CAPPEND(",\"bootimage\":%s", bm);
      free(bm);
    } else {
      CAPPEND(",\"bootimage\":{\"md5\":\"n/a\"}");
    }
  } else {
    CAPPEND(",\"bootimage\":{\"md5\":\"n/a\"}");
  }

  /* devices: prefer a lightweight ARP-derived list during remote collection to avoid blocking discovery */
  {
    char *devs = NULL; size_t devn = 0;
    if (devices_from_arp_json(&devs, &devn) == 0 && devs && devn>0) {
      char *legacy_dev = NULL; size_t legacy_len = 0;
      if (transform_devices_to_legacy(devs, &legacy_dev, &legacy_len) == 0 && legacy_dev) {
        CAPPEND(",\"devices\":%s", legacy_dev);
        free(legacy_dev);
      } else {
        CAPPEND(",\"devices\":%s", devs);
      }
      free(devs);
    } else {
      CAPPEND(",\"devices\":[]");
    }
  }

  CAPPEND(",\"homes\":[]");
  CAPPEND(",\"linklocals\":[]");
  CAPPEND(",\"local_ips\":[]");
  /* olsrd4watchdog state: detect olsrd process presence */
  int olsr2_on=0, olsrd_on=0; detect_olsr_processes(&olsrd_on,&olsr2_on);
  CAPPEND(",\"olsrd4watchdog\":{\"state\":\"%s\"}", olsrd_on?"on":"off");
  CAPPEND("}\n");

  http_send_status(r,200,"OK"); http_printf(r,"Content-Type: application/json; charset=utf-8\r\n\r\n"); http_write(r, out, outlen);
  if (out) free(out);
  if (airos_raw) free(airos_raw);
  if (vgen) free(vgen);
  return 0;
}


/* --- Lightweight /status/lite (omit OLSR link/neighbor discovery for faster initial load) --- */
static int h_status_lite(http_request_t *r) {
  char *buf = NULL; size_t cap = 4096, len = 0; buf = malloc(cap); if(!buf){ send_json(r,"{}\n"); return 0; } buf[0]=0;
  #define APP_L(fmt,...) do { if (json_appendf(&buf, &len, &cap, fmt, ##__VA_ARGS__) != 0) { free(buf); send_json(r,"{}\n"); return 0; } } while(0)
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
  /* fetch queue and metrics: include lightweight counters so UI can show queue state during initial load */
  {
    int qlen = 0; struct fetch_req *fit = NULL; unsigned long m_d=0, m_r=0, m_s=0;
    pthread_mutex_lock(&g_fetch_q_lock);
    fit = g_fetch_q_head; while (fit) { qlen++; fit = fit->next; }
    pthread_mutex_unlock(&g_fetch_q_lock);
    METRIC_LOAD_ALL(m_d, m_r, m_s);
    {
      unsigned long _de=0,_den=0,_ded=0,_dp=0,_dpn=0,_dpd=0;
      DEBUG_LOAD_ALL(_de,_den,_ded,_dp,_dpn,_dpd);
      APP_L("\"fetch_stats\":{\"queue_length\":%d,\"dropped\":%lu,\"retries\":%lu,\"successes\":%lu,\"enqueued\":%lu,\"enqueued_nodedb\":%lu,\"enqueued_discover\":%lu,\"processed\":%lu,\"processed_nodedb\":%lu,\"processed_discover\":%lu,\"thresholds\":{\"queue_warn\":%d,\"queue_crit\":%d,\"dropped_warn\":%d}},", qlen, m_d, m_r, m_s, _de, _den, _ded, _dp, _dpn, _dpd, g_fetch_queue_warn, g_fetch_queue_crit, g_fetch_dropped_warn);
    }
    /* also include a suggested UI autos-refresh ms value */
    APP_L("\"fetch_auto_refresh_ms\":%d,", g_fetch_auto_refresh_ms);
  }
  /* httpd runtime stats: connection pool and task queue */
  {
    int _cp_len = 0, _task_count = 0, _pool_enabled = 0, _pool_size = 0;
    extern void httpd_get_runtime_stats(int*,int*,int*,int*);
    httpd_get_runtime_stats(&_cp_len, &_task_count, &_pool_enabled, &_pool_size);
    APP_L("\"httpd_stats\":{\"conn_pool_len\":%d,\"task_count\":%d,\"pool_enabled\":%d,\"pool_size\":%d},", _cp_len, _task_count, _pool_enabled, _pool_size);
  }
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
  {
    char *vout=NULL; size_t vn=0;
    if (generate_versions_json(&vout, &vn) == 0 && vout && vn>0) {
      APP_L("\"versions\":%s,", vout);
      free(vout);
    } else {
      APP_L("\"versions\":{\"olsrd\":\"unknown\"},");
    }
  }
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
  for(const char **ep=eps; *ep && !raw; ++ep){ if(util_http_get_url_local(*ep, &raw, &rn, 1)==0 && raw && rn>0) break; if(raw){ free(raw); raw=NULL; rn=0; } }
  if(!raw){ send_json(r, "{\"via\":\"\",\"routes\":[]}\n"); return 0; }
  char *out=NULL; size_t cap=4096,len=0; out=malloc(cap); if(!out){ free(raw); send_json(r,"{\"via\":\"\",\"routes\":[]}\n"); return 0;} out[0]=0;
  #define APP_R(fmt,...) do { if (json_appendf(&out, &len, &cap, fmt, ##__VA_ARGS__) != 0) { free(out); free(raw); send_json(r,"{\"via\":\"\",\"routes\":[]}\n"); return 0; } } while(0)
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
  if(find_json_string_value(obj,"metric",&v,&vlen) ||
     find_json_string_value(obj,"rtpMetricCost",&v,&vlen) ||
     find_json_string_value(obj,"pathCost",&v,&vlen) ||
     find_json_string_value(obj,"pathcost",&v,&vlen) ||
     find_json_string_value(obj,"tcEdgeCost",&v,&vlen) ||
     find_json_string_value(obj,"cost",&v,&vlen) ||
     find_json_string_value(obj,"metricCost",&v,&vlen) ||
     find_json_string_value(obj,"metrics",&v,&vlen)) snprintf(metric,sizeof(metric),"%.*s",(int)vlen,v);
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
    for(const char **ep=eps; *ep && !links_raw; ++ep){ if(util_http_get_url_local(*ep, &links_raw, &ln, 1)==0 && links_raw && ln>0) break; if(links_raw){ free(links_raw); links_raw=NULL; ln=0; } }
  }
  char *neighbors_raw=NULL; size_t nnr=0; util_http_get_url_local("http://127.0.0.1:9090/neighbors", &neighbors_raw,&nnr, 1);
  char *routes_raw=NULL; size_t rr=0; util_http_get_url_local("http://127.0.0.1:9090/routes", &routes_raw,&rr, 1);
  char *topology_raw=NULL; size_t tr=0; util_http_get_url_local("http://127.0.0.1:9090/topology", &topology_raw,&tr, 1);
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
  #define APP_O(fmt,...) do { if (json_appendf(&buf, &len, &cap, fmt, ##__VA_ARGS__) != 0) { if(buf){ free(buf);} send_json(r,"{}\n"); goto done; } } while(0)
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
  for(const char **ep=eps_links; *ep && !links_raw; ++ep){ if(util_http_get_url_local(*ep, &links_raw, &ln, 1)==0 && links_raw && ln>0) break; if(links_raw){ free(links_raw); links_raw=NULL; ln=0; } }
  char *routes_raw=NULL; size_t rr=0; const char *eps_routes[]={"http://127.0.0.1:9090/routes","http://127.0.0.1:2006/routes","http://127.0.0.1:8123/routes",NULL};
  for(const char **ep=eps_routes; *ep && !routes_raw; ++ep){ if(util_http_get_url_local(*ep, &routes_raw, &rr, 1)==0 && routes_raw && rr>0) break; if(routes_raw){ free(routes_raw); routes_raw=NULL; rr=0; } }
  char *topology_raw=NULL; size_t trn=0; const char *eps_topo[]={"http://127.0.0.1:9090/topology","http://127.0.0.1:2006/topology","http://127.0.0.1:8123/topology",NULL};
  for(const char **ep=eps_topo; *ep && !topology_raw; ++ep){ if(util_http_get_url_local(*ep, &topology_raw, &trn, 1)==0 && topology_raw && trn>0) break; if(topology_raw){ free(topology_raw); topology_raw=NULL; trn=0; } }
  char *buf=NULL; size_t cap=8192,len=0; buf=malloc(cap); if(!buf){ send_json(r,"{\"err\":\"oom\"}\n"); goto done; } buf[0]=0;
  #define APP_RAW(fmt,...) do { if (json_appendf(&buf, &len, &cap, fmt, ##__VA_ARGS__) != 0) { if(buf){ free(buf);} send_json(r,"{\"err\":\"oom\"}\n"); goto done; } } while(0)
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
  #define APP2(fmt,...) do { char *_t=NULL; int _n=asprintf(&_t,fmt,##__VA_ARGS__); if(_n<0||!_t){ if(_t) free(_t); free(buf); send_json(r,"{}\n"); return 0;} if(len+(size_t)_n+1>cap){ while(cap<len+(size_t)_n+1) cap*=2; char *nb=realloc(buf,cap); if(!nb){ free(_t); free(buf); send_json(r,"{}\n"); return 0;} buf=nb;} memcpy(buf+len,_t,(size_t)_n); len += (size_t)_n; buf[len]=0; free(_t);}while(0)
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
  for(const char **ep=eps; *ep && !olsr_links_raw; ++ep){ if(util_http_get_url_local(*ep, &olsr_links_raw, &oln, 1)==0 && olsr_links_raw && oln>0) break; if(olsr_links_raw){ free(olsr_links_raw); olsr_links_raw=NULL; oln=0; } }
  /* also get routes & topology to enrich route/node counts like /olsr/links */
  char *routes_raw=NULL; size_t rr=0; util_http_get_url_local("http://127.0.0.1:9090/routes", &routes_raw,&rr, 1);
  char *topology_raw=NULL; size_t tr=0; util_http_get_url_local("http://127.0.0.1:9090/topology", &topology_raw,&tr, 1);
  APP2("\"olsr2_on\":%s,", olsr2_on?"true":"false");
  APP2("\"olsrd_on\":%s,", olsrd_on?"true":"false");
  if(olsr_links_raw && oln>0){
    size_t l1=strlen(olsr_links_raw); size_t l2=routes_raw?strlen(routes_raw):0; size_t l3=topology_raw?strlen(topology_raw):0;
    char *combined_raw=malloc(l1+l2+l3+8); if(combined_raw){ size_t off=0; memcpy(combined_raw+off,olsr_links_raw,l1); off+=l1; combined_raw[off++]='\n'; if(l2){ memcpy(combined_raw+off,routes_raw,l2); off+=l2; combined_raw[off++]='\n'; } if(l3){ memcpy(combined_raw+off,topology_raw,l3); off+=l3; } combined_raw[off]=0; char *norm=NULL; size_t nn=0; if(normalize_olsrd_links(combined_raw,&norm,&nn)==0 && norm){ APP2("\"links\":%s", norm); free(norm);} else { APP2("\"links\":[]"); } free(combined_raw);} else { APP2("\"links\":[]"); }
  } else { APP2("\"links\":[]"); }
  APP2("}\n");
  http_send_status(r,200,"OK"); http_printf(r,"Content-Type: application/json; charset=utf-8\r\n\r\n"); http_write(r,buf,len); free(buf); if(olsr_links_raw) free(olsr_links_raw); if(routes_raw) free(routes_raw); if(topology_raw) free(topology_raw); return 0; }

static int h_nodedb(http_request_t *r) {
  /* Only fetch if needed (respect TTL) */
  fetch_remote_nodedb_if_needed();
  pthread_mutex_lock(&g_nodedb_lock);
  if (g_nodedb_cached && g_nodedb_cached_len>0) {
  /* Add basic caching headers to reduce client revalidation frequency */
  http_send_status(r,200,"OK");
  http_printf(r,"Content-Type: application/json; charset=utf-8\r\n");
  /* Cache-Control: client-side TTL aligns with server-side TTL */
  http_printf(r,"Cache-Control: public, max-age=%d\r\n", g_nodedb_ttl);
  /* Last-Modified: use last fetch time */
    if (g_nodedb_last_fetch) {
      char tbuf[64]; format_rfc1123_time(g_nodedb_last_fetch, tbuf, sizeof(tbuf)); http_printf(r, "Last-Modified: %s\r\n", tbuf);
    }
    /* ETag: weak tag based on length + last_fetch to allow conditional GET */
    http_printf(r,"ETag: \"%zx-%ld\"\r\n\r\n", g_nodedb_cached_len, g_nodedb_last_fetch);
  http_write(r,g_nodedb_cached,g_nodedb_cached_len); pthread_mutex_unlock(&g_nodedb_lock); return 0; }
  pthread_mutex_unlock(&g_nodedb_lock);
  /* Debug: return error info instead of empty JSON */
  char debug_json[1024];
  char url_copy[256];
  strncpy(url_copy, g_nodedb_url, sizeof(url_copy) - 1);
  url_copy[sizeof(url_copy) - 1] = '\0';
  snprintf(debug_json, sizeof(debug_json), "{\"error\":\"No remote node_db data available\",\"url\":\"%s\",\"last_fetch\":%ld,\"cached_len\":%zu}", url_copy, g_nodedb_last_fetch, g_nodedb_cached_len);
  send_json(r, debug_json); return 0;
}

/* Force a refresh of the remote node_db (bypass TTL). Returns JSON status. */
static int h_nodedb_refresh(http_request_t *r) {
  /* Make refresh non-blocking by default to avoid tying up the HTTP thread.
   * If caller explicitly requests blocking behaviour via ?wait=1, preserve
   * the previous semantics (enqueue and wait). Non-blocking calls will
   * immediately return a queued status and current queue length.
   */
  char wbuf[8] = "0";
  int do_wait = 0;
  if (get_query_param(r, "wait", wbuf, sizeof(wbuf))) {
    if (strcmp(wbuf, "1") == 0) do_wait = 1;
  }

  if (do_wait) {
    /* perform forced fetch: enqueue and wait for completion (legacy behaviour) */
    enqueue_fetch_request(1, 1, FETCH_TYPE_NODEDB);
    pthread_mutex_lock(&g_nodedb_lock);
    if (g_nodedb_cached && g_nodedb_cached_len>0) {
      /* return a small success JSON including last_fetch */
      char resp[256]; snprintf(resp, sizeof(resp), "{\"status\":\"ok\",\"last_fetch\":%ld,\"len\":%zu}", g_nodedb_last_fetch, g_nodedb_cached_len);
      http_send_status(r,200,"OK"); http_printf(r,"Content-Type: application/json; charset=utf-8\r\n\r\n"); http_write(r,resp,strlen(resp)); pthread_mutex_unlock(&g_nodedb_lock); return 0;
    }
    pthread_mutex_unlock(&g_nodedb_lock);
    send_json(r, "{\"status\":\"error\",\"message\":\"fetch failed\"}");
    return 0;
  }

  /* Non-blocking: enqueue and return queued status immediately */
  enqueue_fetch_request(1, 0, FETCH_TYPE_NODEDB);
  /* compute current queue length */
  pthread_mutex_lock(&g_fetch_q_lock);
  int qlen = 0; struct fetch_req *it = g_fetch_q_head; while (it) { qlen++; it = it->next; }
  pthread_mutex_unlock(&g_fetch_q_lock);
  pthread_mutex_lock(&g_nodedb_lock);
  long last = g_nodedb_last_fetch; size_t len = g_nodedb_cached_len;
  pthread_mutex_unlock(&g_nodedb_lock);
  char resp2[256]; snprintf(resp2, sizeof(resp2), "{\"status\":\"queued\",\"last_fetch\":%ld,\"len\":%zu,\"queue_len\":%d}", last, len, qlen);
  http_send_status(r,200,"OK"); http_printf(r,"Content-Type: application/json; charset=utf-8\r\n\r\n"); http_write(r,resp2,strlen(resp2));
  return 0;
}

  /* Simple metrics endpoint for fetch-related counters */
  static int h_fetch_metrics(http_request_t *r) {
    char buf[256];
    pthread_mutex_lock(&g_metrics_lock);
    unsigned long dropped = 0, retries = 0, successes = 0;
    METRIC_LOAD_ALL(dropped, retries, successes);
    snprintf(buf, sizeof(buf), "{\"fetch_dropped\":%lu,\"fetch_retries\":%lu,\"fetch_successes\":%lu}", dropped, retries, successes);
    http_send_status(r,200,"OK"); http_printf(r, "Content-Type: application/json; charset=utf-8\r\n\r\n"); http_write(r, buf, strlen(buf));
    return 0;
  }

/* /status.py dispatch endpoint: map ?get=<name> to existing handlers without reimplementing them */
static int h_status_py(http_request_t *r) {
  char v[128] = "";
  /* Support both forms used by bmk-webstatus.py:
   *  - /status.py?get=status
   *  - /status.py?status         (bare key, no value)
   *  - /status.py?status=        (key with empty value)
   * Check explicit get= first, then fall back to checking known bare keys.
   */
  if (get_query_param(r, "get", v, sizeof(v))) {
    /* use provided get= value */
  } else {
    /* check for bare keys that the Python script maps to get=<name> */
    char t[32];
  if (get_query_param(r, "status", t, sizeof(t))) return h_status_compat(r);
    if (get_query_param(r, "connections", t, sizeof(t))) return h_connections(r);
    if (get_query_param(r, "discover", t, sizeof(t))) return h_discover(r);
    if (get_query_param(r, "airos", t, sizeof(t))) return h_airos(r);
    if (get_query_param(r, "ipv6", t, sizeof(t))) return h_ipv6(r);
    if (get_query_param(r, "ipv4", t, sizeof(t))) return h_ipv4(r);
    if (get_query_param(r, "olsrd", t, sizeof(t))) return h_olsrd(r);
    if (get_query_param(r, "jsoninfo", t, sizeof(t))) return h_jsoninfo(r);
    if (get_query_param(r, "txtinfo", t, sizeof(t))) return h_txtinfo(r);
    if (get_query_param(r, "traffic", t, sizeof(t))) return h_traffic(r);
    if (get_query_param(r, "test", t, sizeof(t))) {
      http_send_status(r,200,"OK"); http_printf(r,"Content-Type: text/plain; charset=utf-8\r\n\r\n"); http_printf(r,"test\n"); return 0;
    }
    /* no known param found -> default to full status */
    return h_status(r);
  }

  /* Map known values (match bmk-webstatus.py supported ?get values) */
  if (strcmp(v, "status") == 0) return h_status_compat(r);
  if (strcmp(v, "connections") == 0) return h_connections(r);
  if (strcmp(v, "discover") == 0) return h_discover(r);
  if (strcmp(v, "airos") == 0) return h_airos(r);
  if (strcmp(v, "ipv6") == 0) return h_ipv6(r);
  if (strcmp(v, "ipv4") == 0) return h_ipv4(r);
  if (strcmp(v, "olsrd") == 0) return h_olsrd(r);
  if (strcmp(v, "jsoninfo") == 0) return h_jsoninfo(r);
  if (strcmp(v, "txtinfo") == 0) return h_txtinfo(r);
  if (strcmp(v, "traffic") == 0) return h_traffic(r);
  if (strcmp(v, "test") == 0) {
    /* no equivalent h_test handler in plugin; emulate small test output */
    http_send_status(r,200,"OK"); http_printf(r,"Content-Type: text/plain; charset=utf-8\r\n\r\n"); http_printf(r,"test\n"); return 0;
  }
  /* unknown -> default to full status to preserve backward compatibility */
  return h_status(r);
}

/* Prometheus-compatible metrics endpoint (simple, non-exhaustive) */
static int h_prometheus_metrics(http_request_t *r) {
  char buf[1024]; size_t off = 0;
  /* Safe append helper: calculate remaining space and update offset safely. */
#define SAFE_APPEND(fmt, ...) do { \
    size_t _rem = (sizeof(buf) > off) ? (sizeof(buf) - off) : 0; \
    /* require room for at least one printable char plus NUL to avoid fortify warnings */ \
    if (_rem > 1) { int _n = snprintf(buf + off, _rem, (fmt), ##__VA_ARGS__); \
      if (_n > 0) { if ((size_t)_n >= _rem) { off = sizeof(buf) - 1; buf[off] = '\0'; } else { off += (size_t)_n; } } \
    } \
  } while(0)
  unsigned long d=0, rts=0, s=0; METRIC_LOAD_ALL(d, rts, s);
  unsigned long de=0, den=0, ded=0, dp=0, dpn=0, dpd=0; DEBUG_LOAD_ALL(de, den, ded, dp, dpn, dpd);
  pthread_mutex_lock(&g_fetch_q_lock);
  int qlen = 0; struct fetch_req *it = g_fetch_q_head; while (it) { qlen++; it = it->next; }
  pthread_mutex_unlock(&g_fetch_q_lock);
  SAFE_APPEND("# HELP olsrd_status_fetch_queue_length Number of pending fetch requests\n");
  SAFE_APPEND("# TYPE olsrd_status_fetch_queue_length gauge\n");
  SAFE_APPEND("olsrd_status_fetch_queue_length %d\n", qlen);
  SAFE_APPEND("# HELP olsrd_status_fetch_dropped_total Total dropped fetch requests\n");
  SAFE_APPEND("# TYPE olsrd_status_fetch_dropped_total counter\n");
  SAFE_APPEND("olsrd_status_fetch_dropped_total %lu\n", d);
  SAFE_APPEND("# HELP olsrd_status_fetch_retries_total Total fetch retry attempts\n");
  SAFE_APPEND("# TYPE olsrd_status_fetch_retries_total counter\n");
  SAFE_APPEND("olsrd_status_fetch_retries_total %lu\n", rts);
  SAFE_APPEND("# HELP olsrd_status_fetch_successes_total Total successful fetches\n");
  SAFE_APPEND("# TYPE olsrd_status_fetch_successes_total counter\n");
  SAFE_APPEND("olsrd_status_fetch_successes_total %lu\n", s);
  SAFE_APPEND("# HELP olsrd_status_fetch_enqueued_total Total enqueue operations\n");
  SAFE_APPEND("# TYPE olsrd_status_fetch_enqueued_total counter\n");
  SAFE_APPEND("olsrd_status_fetch_enqueued_total %lu\n", de);
  SAFE_APPEND("# HELP olsrd_status_fetch_processed_total Total processed operations\n");
  SAFE_APPEND("# TYPE olsrd_status_fetch_processed_total counter\n");
  SAFE_APPEND("olsrd_status_fetch_processed_total %lu\n", dp);
  SAFE_APPEND("# HELP olsrd_status_fetch_enqueued_nodedb_total Enqueued NodeDB fetches\n");
  SAFE_APPEND("# TYPE olsrd_status_fetch_enqueued_nodedb_total counter\n");
  SAFE_APPEND("olsrd_status_fetch_enqueued_nodedb_total %lu\n", den);
  SAFE_APPEND("# HELP olsrd_status_fetch_enqueued_discover_total Enqueued discover ops\n");
  SAFE_APPEND("# TYPE olsrd_status_fetch_enqueued_discover_total counter\n");
  SAFE_APPEND("olsrd_status_fetch_enqueued_discover_total %lu\n", ded);
  SAFE_APPEND("# HELP olsrd_status_fetch_processed_nodedb_total Processed NodeDB ops\n");
  SAFE_APPEND("# TYPE olsrd_status_fetch_processed_nodedb_total counter\n");
  SAFE_APPEND("olsrd_status_fetch_processed_nodedb_total %lu\n", dpn);
  SAFE_APPEND("# HELP olsrd_status_fetch_processed_discover_total Processed discover ops\n");
  SAFE_APPEND("# TYPE olsrd_status_fetch_processed_discover_total counter\n");
  SAFE_APPEND("olsrd_status_fetch_processed_discover_total %lu\n", dpd);

  http_send_status(r,200,"OK"); http_printf(r, "Content-Type: text/plain; charset=utf-8\r\n\r\n"); http_write(r, buf, off);
  /* cleanup macro */
#undef SAFE_APPEND
  return 0;
}

/* Debug endpoint: current queue and queued request metadata */
static int h_fetch_debug(http_request_t *r) {
  pthread_mutex_lock(&g_fetch_q_lock);
  int qlen = 0; struct fetch_req *it = g_fetch_q_head;
  while (it) { qlen++; it = it->next; }
  /* Build JSON array of simple objects: {"force":0|1,"wait":0|1,"type":N} */
  char *buf = NULL; size_t cap = 1024; size_t len = 0; buf = malloc(cap); if(!buf){ send_json(r, "{}\n"); pthread_mutex_unlock(&g_fetch_q_lock); return 0; } buf[0]=0;
  len += snprintf(buf+len, cap-len, "{\"queue_length\":%d,\"requests\":[", qlen);
  it = g_fetch_q_head; int first=1; while (it) {
    if (!first) {
      len += snprintf(buf+len, cap-len, ",");
    }
    first = 0;
    len += snprintf(buf+len, cap-len, "{\"force\":%d,\"wait\":%d,\"type\":%d}", it->force?1:0, it->wait?1:0, it->type);
    it = it->next;
  }
  unsigned long _de=0,_den=0,_ded=0,_dp=0,_dpn=0,_dpd=0;
  DEBUG_LOAD_ALL(_de,_den,_ded,_dp,_dpn,_dpd);
  /* include httpd runtime stats */
  {
    int _cp_len = 0, _task_count = 0, _pool_enabled = 0, _pool_size = 0;
    extern void httpd_get_runtime_stats(int*,int*,int*,int*);
    httpd_get_runtime_stats(&_cp_len, &_task_count, &_pool_enabled, &_pool_size);
    len += snprintf(buf+len, cap-len, "],\"debug\":{\"enqueued\":%lu,\"enqueued_nodedb\":%lu,\"enqueued_discover\":%lu,\"processed\":%lu,\"processed_nodedb\":%lu,\"processed_discover\":%lu,\"last_fetch_msg\":\"%s\",\"httpd_stats\":{\"conn_pool_len\":%d,\"task_count\":%d,\"pool_enabled\":%d,\"pool_size\":%d}}}", _de, _den, _ded, _dp, _dpn, _dpd, g_debug_last_fetch_msg, _cp_len, _task_count, _pool_enabled, _pool_size);
  }
  pthread_mutex_unlock(&g_fetch_q_lock);
  send_json(r, buf);
  free(buf);
  return 0;
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
  #define APP_TR(fmt,...) do { if (json_appendf(&json, &len2, &cap, fmt, ##__VA_ARGS__) != 0) { free(json); free(dup); free(out); free(cmd); send_json(r,"{\"error\":\"oom\"}\n"); return 0; } } while(0)
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
  char url[512]; snprintf(url, sizeof(url), "http://127.0.0.1:9090/%s", q);
  char *out = NULL; size_t n = 0;
  if (util_http_get_url_local(url, &out, &n, 1) == 0 && out && n>0) {
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
/* mutex protecting both small in-process caches */
static pthread_mutex_t g_kv_cache_lock = PTHREAD_MUTEX_INITIALIZER;

static int olsr_cache_get(const char *key, char *out, size_t outlen) {
  if (!key || !out) return 0;
  unsigned long h = 1469598103934665603UL;
  for (const unsigned char *p = (const unsigned char*)key; *p; ++p) h = (h ^ *p) * 1099511628211UL;
  int idx = (int)(h % CACHE_SIZE);
  pthread_mutex_lock(&g_kv_cache_lock);
  if (g_olsr_cache[idx].key[0] == 0) { pthread_mutex_unlock(&g_kv_cache_lock); return 0; }
  if (strcmp(g_olsr_cache[idx].key, key) != 0) { pthread_mutex_unlock(&g_kv_cache_lock); return 0; }
  if (difftime(time(NULL), g_olsr_cache[idx].ts) > CACHE_TTL) { pthread_mutex_unlock(&g_kv_cache_lock); return 0; }
  /* copy up to outlen-1 chars */
  snprintf(out, outlen, "%s", g_olsr_cache[idx].val);
  pthread_mutex_unlock(&g_kv_cache_lock);
  return 1;
}

static void olsr_cache_set(const char *key, const char *val) {
  if (!key || !val) return;
  unsigned long h = 1469598103934665603UL;
  for (const unsigned char *p = (const unsigned char*)key; *p; ++p) h = (h ^ *p) * 1099511628211UL;
  int idx = (int)(h % CACHE_SIZE);
  pthread_mutex_lock(&g_kv_cache_lock);
  snprintf(g_olsr_cache[idx].key, sizeof(g_olsr_cache[idx].key), "%s", key);
  /* store truncated value (safe) */
  snprintf(g_olsr_cache[idx].val, sizeof(g_olsr_cache[idx].val), "%s", val);
  g_olsr_cache[idx].ts = time(NULL);
  pthread_mutex_unlock(&g_kv_cache_lock);
}

static void cache_set(struct kv_cache_entry *cache, const char *key, const char *val) {
  if (!key || !val) return;
  unsigned long h = 1469598103934665603UL;
  for (const unsigned char *p = (const unsigned char*)key; *p; ++p) h = (h ^ *p) * 1099511628211UL;
  int idx = (int)(h % CACHE_SIZE);
  pthread_mutex_lock(&g_kv_cache_lock);
  snprintf(cache[idx].key, sizeof(cache[idx].key), "%s", key);
  snprintf(cache[idx].val, sizeof(cache[idx].val), "%s", val);
  cache[idx].ts = time(NULL);
  pthread_mutex_unlock(&g_kv_cache_lock);
}

static int cache_get(struct kv_cache_entry *cache, const char *key, char *out, size_t outlen) {
  if (!key || !out) return 0;
  unsigned long h = 1469598103934665603UL;
  for (const unsigned char *p = (const unsigned char*)key; *p; ++p) h = (h ^ *p) * 1099511628211UL;
  int idx = (int)(h % CACHE_SIZE);
  pthread_mutex_lock(&g_kv_cache_lock);
  if (cache[idx].key[0] == 0) { pthread_mutex_unlock(&g_kv_cache_lock); return 0; }
  if (strcmp(cache[idx].key, key) != 0) { pthread_mutex_unlock(&g_kv_cache_lock); return 0; }
  if (difftime(time(NULL), cache[idx].ts) > CACHE_TTL) { pthread_mutex_unlock(&g_kv_cache_lock); return 0; }
  snprintf(out, outlen, "%s", cache[idx].val);
  pthread_mutex_unlock(&g_kv_cache_lock);
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
  fetch_remote_nodedb_if_needed();
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
  /* If the caller provided nodedb_url via PlParam, mark it as set */
  if (data == g_nodedb_url) g_cfg_nodedb_url_set = 1;
  return 0;
}
static int set_int_param(const char *value, void *data, set_plugin_parameter_addon addon __attribute__((unused))) {
  if (!value || !data) return 1;
  *(int*)data = atoi(value);
  /* Track which integer config fields are explicitly set via PlParam */
  if (data == &g_port) g_cfg_port_set = 1;
  if (data == &g_nodedb_ttl) g_cfg_nodedb_ttl_set = 1;
  if (data == &g_nodedb_write_disk) g_cfg_nodedb_write_disk_set = 1;
  /* new fetch tuning params via PlParam */
  if (data == &g_fetch_queue_max) g_cfg_fetch_queue_set = 1;
  if (data == &g_fetch_retries) g_cfg_fetch_retries_set = 1;
  if (data == &g_fetch_backoff_initial) g_cfg_fetch_backoff_set = 1;
  if (data == &g_fetch_report_interval) g_cfg_fetch_report_set = 1;
  if (data == &g_fetch_auto_refresh_ms) g_cfg_fetch_auto_refresh_set = 1;
  if (data == &g_fetch_queue_warn) g_cfg_fetch_queue_warn_set = 1;
  if (data == &g_fetch_queue_crit) g_cfg_fetch_queue_crit_set = 1;
  if (data == &g_fetch_dropped_warn) g_cfg_fetch_dropped_warn_set = 1;
  return 0;
}

/* accept multiple PlParam "Net" entries; each value can be CIDR (a/b) or
 * "addr mask" pairs like "193.238.156.0 255.255.252.0" or single address.
 */
static int set_net_param(const char *value, void *data __attribute__((unused)), set_plugin_parameter_addon addon __attribute__((unused))) {
  if (!value) return 1;
  /* forward to httpd allow-list */
  if (http_allow_cidr(value) != 0) {
    fprintf(stderr, "[status-plugin] invalid Net parameter: %s\n", value);
    return 1;
  }
  /* count that at least one Net was supplied in config */
  g_cfg_net_count++;
  return 0;
}

static const struct olsrd_plugin_parameters g_params[] = {
  { .name = "bind",       .set_plugin_parameter = &set_str_param, .data = g_bind,        .addon = {0} },
  { .name = "port",       .set_plugin_parameter = &set_int_param, .data = &g_port,       .addon = {0} },
  { .name = "enableipv6", .set_plugin_parameter = &set_int_param, .data = &g_enable_ipv6,.addon = {0} },
  { .name = "Net",        .set_plugin_parameter = &set_net_param, .data = NULL,          .addon = {0} },
  { .name = "assetroot",  .set_plugin_parameter = &set_str_param, .data = g_asset_root,  .addon = {0} },
  { .name = "nodedb_url", .set_plugin_parameter = &set_str_param, .data = g_nodedb_url,  .addon = {0} },
  { .name = "nodedb_ttl", .set_plugin_parameter = &set_int_param, .data = &g_nodedb_ttl, .addon = {0} },
  { .name = "nodedb_write_disk", .set_plugin_parameter = &set_int_param, .data = &g_nodedb_write_disk, .addon = {0} },
  /* fetch tuning PlParams: override defaults (PlParam wins over env) */
  { .name = "fetch_queue_max", .set_plugin_parameter = &set_int_param, .data = &g_fetch_queue_max, .addon = {0} },
  { .name = "fetch_retries", .set_plugin_parameter = &set_int_param, .data = &g_fetch_retries, .addon = {0} },
  { .name = "fetch_backoff_initial", .set_plugin_parameter = &set_int_param, .data = &g_fetch_backoff_initial, .addon = {0} },
  { .name = "fetch_report_interval", .set_plugin_parameter = &set_int_param, .data = &g_fetch_report_interval, .addon = {0} },
  { .name = "fetch_auto_refresh_ms", .set_plugin_parameter = &set_int_param, .data = &g_fetch_auto_refresh_ms, .addon = {0} },
  { .name = "fetch_log_queue", .set_plugin_parameter = &set_int_param, .data = &g_fetch_log_queue, .addon = {0} },
  /* UI thresholds exported for front-end convenience */
  { .name = "fetch_queue_warn", .set_plugin_parameter = &set_int_param, .data = &g_fetch_queue_warn, .addon = {0} },
  { .name = "fetch_queue_crit", .set_plugin_parameter = &set_int_param, .data = &g_fetch_queue_crit, .addon = {0} },
  { .name = "fetch_dropped_warn", .set_plugin_parameter = &set_int_param, .data = &g_fetch_dropped_warn, .addon = {0} },
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

  /* Allow override of port via environment variable for quick testing/deployment:
   * If OLSRD_STATUS_PLUGIN_PORT is set and contains a valid port number (1-65535),
   * it will override the configured/plugin parameter value in g_port.
   */
  /* Apply environment overrides only when the corresponding plugin parameter was not set.
   * This lets olsrd.conf PlParam explicitly win over env vars while still allowing
   * env vars to provide defaults when no PlParam exists.
   */
  {
    if (!g_cfg_port_set) {
      const char *env_port = getenv("OLSRD_STATUS_PLUGIN_PORT");
      if (env_port && env_port[0]) {
        char *endptr = NULL; long p = strtol(env_port, &endptr, 10);
        if (endptr && *endptr == '\0' && p > 0 && p <= 65535) {
          g_port = (int)p;
          fprintf(stderr, "[status-plugin] setting port from environment: OLSRD_STATUS_PLUGIN_PORT=%s -> %d\n", env_port, g_port);
        } else {
          fprintf(stderr, "[status-plugin] invalid OLSRD_STATUS_PLUGIN_PORT value: %s (ignored)\n", env_port);
        }
      }
    }
  }

  /* Environment-driven overrides for additional plugin parameters.
   * - OLSRD_STATUS_PLUGIN_NET: comma/semicolon/whitespace-separated list of CIDR/mask entries.
   * - OLSRD_STATUS_PLUGIN_NODEDB_URL: URL string for node DB.
   * - OLSRD_STATUS_PLUGIN_NODEDB_TTL: integer seconds TTL.
   * - OLSRD_STATUS_PLUGIN_NODEDB_WRITE_DISK: integer (0/1) to enable writing node DB to disk.
   */
  {
  const char *env_net = getenv("OLSRD_STATUS_PLUGIN_NET");
  if (env_net && env_net[0]) {
    /* If environment-specified networks are present we treat them as authoritative
     * and replace any PlParam-provided Net entries. */
    http_clear_allowlist();
    fprintf(stderr, "[status-plugin] OLSRD_STATUS_PLUGIN_NET set in environment; replacing configured Net entries\n");
      char *buf = strdup(env_net);
      if (buf) {
        char *save = NULL; char *tok = strtok_r(buf, ",; \t\n", &save);
        while (tok) {
          /* trim leading/trailing whitespace */
          char *s = tok; while (*s && isspace((unsigned char)*s)) s++;
          char *e = s + strlen(s); while (e > s && isspace((unsigned char)*(e-1))) { e--; }
          *e = '\0';
          if (*s) {
            if (http_allow_cidr(s) != 0) {
              fprintf(stderr, "[status-plugin] invalid Net value from OLSRD_STATUS_PLUGIN_NET: '%s'\n", s);
            } else {
              fprintf(stderr, "[status-plugin] added allow-list Net from env: %s\n", s);
            }
          }
          tok = strtok_r(NULL, ",; \t\n", &save);
        }
        free(buf);
      }
    /* Log the allow-list we ended up with */
    http_log_allowlist();
  }

  const char *env_nodedb = getenv("OLSRD_STATUS_PLUGIN_NODEDB_URL");
  if (env_nodedb && env_nodedb[0] && !g_cfg_nodedb_url_set) {
      /* copy into fixed-size buffer, truncating if necessary */
      snprintf(g_nodedb_url, sizeof(g_nodedb_url), "%s", env_nodedb);
      fprintf(stderr, "[status-plugin] overriding nodedb_url from environment: %s\n", g_nodedb_url);
    }

  /* Fetch tuning env overrides (only if not set via PlParam) */
  if (!g_cfg_fetch_queue_set) {
    const char *env_q = getenv("OLSRD_STATUS_FETCH_QUEUE_MAX");
    if (env_q && env_q[0]) {
      char *endptr = NULL; long v = strtol(env_q, &endptr, 10);
      if (endptr && *endptr == '\0' && v > 0 && v <= 256) {
        g_fetch_queue_max = (int)v;
        fprintf(stderr, "[status-plugin] overriding fetch_queue_max from env: %d\n", g_fetch_queue_max);
      } else fprintf(stderr, "[status-plugin] invalid OLSRD_STATUS_FETCH_QUEUE_MAX value: %s (ignored)\n", env_q);
    }
  }
  if (!g_cfg_fetch_retries_set) {
    const char *env_r = getenv("OLSRD_STATUS_FETCH_RETRIES");
    if (env_r && env_r[0]) {
      char *endptr = NULL; long v = strtol(env_r, &endptr, 10);
      if (endptr && *endptr == '\0' && v >= 0 && v <= 10) {
        g_fetch_retries = (int)v;
        fprintf(stderr, "[status-plugin] overriding fetch_retries from env: %d\n", g_fetch_retries);
      } else fprintf(stderr, "[status-plugin] invalid OLSRD_STATUS_FETCH_RETRIES value: %s (ignored)\n", env_r);
    }
  }
  if (!g_cfg_fetch_backoff_set) {
    const char *env_b = getenv("OLSRD_STATUS_FETCH_BACKOFF_INITIAL");
    if (env_b && env_b[0]) {
      char *endptr = NULL; long v = strtol(env_b, &endptr, 10);
      if (endptr && *endptr == '\0' && v >= 0 && v <= 60) {
        g_fetch_backoff_initial = (int)v;
        fprintf(stderr, "[status-plugin] overriding fetch_backoff_initial from env: %d\n", g_fetch_backoff_initial);
      } else fprintf(stderr, "[status-plugin] invalid OLSRD_STATUS_FETCH_BACKOFF_INITIAL value: %s (ignored)\n", env_b);
    }
  }

  /* Threshold env overrides for UI hints */
  if (!g_cfg_fetch_queue_warn_set) {
    const char *env_qw = getenv("OLSRD_STATUS_FETCH_QUEUE_WARN");
    if (env_qw && env_qw[0]) { char *endptr=NULL; long v=strtol(env_qw,&endptr,10); if (endptr && *endptr=='\0' && v>=0 && v<=100000) { g_fetch_queue_warn = (int)v; fprintf(stderr, "[status-plugin] overriding fetch_queue_warn from env: %d\n", g_fetch_queue_warn); } else fprintf(stderr, "[status-plugin] invalid OLSRD_STATUS_FETCH_QUEUE_WARN value: %s (ignored)\n", env_qw); }
  }
  if (!g_cfg_fetch_queue_crit_set) {
    const char *env_qc = getenv("OLSRD_STATUS_FETCH_QUEUE_CRIT");
    if (env_qc && env_qc[0]) { char *endptr=NULL; long v=strtol(env_qc,&endptr,10); if (endptr && *endptr=='\0' && v>=0 && v<=100000) { g_fetch_queue_crit = (int)v; fprintf(stderr, "[status-plugin] overriding fetch_queue_crit from env: %d\n", g_fetch_queue_crit); } else fprintf(stderr, "[status-plugin] invalid OLSRD_STATUS_FETCH_QUEUE_CRIT value: %s (ignored)\n", env_qc); }
  }
  if (!g_cfg_fetch_dropped_warn_set) {
    const char *env_dw = getenv("OLSRD_STATUS_FETCH_DROPPED_WARN");
    if (env_dw && env_dw[0]) { char *endptr=NULL; long v=strtol(env_dw,&endptr,10); if (endptr && *endptr=='\0' && v>=0 && v<=100000) { g_fetch_dropped_warn = (int)v; fprintf(stderr, "[status-plugin] overriding fetch_dropped_warn from env: %d\n", g_fetch_dropped_warn); } else fprintf(stderr, "[status-plugin] invalid OLSRD_STATUS_FETCH_DROPPED_WARN value: %s (ignored)\n", env_dw); }
  }

  /* Fetch reporter interval: optional periodic stderr summary */
  if (!g_cfg_fetch_report_set) {
    const char *env_i = getenv("OLSRD_STATUS_FETCH_REPORT_INTERVAL");
    if (env_i && env_i[0]) {
      char *endptr = NULL; long v = strtol(env_i, &endptr, 10);
      if (endptr && *endptr == '\0' && v >= 0 && v <= 3600) {
        g_fetch_report_interval = (int)v;
        fprintf(stderr, "[status-plugin] setting fetch_report_interval from env: %d\n", g_fetch_report_interval);
      } else fprintf(stderr, "[status-plugin] invalid OLSRD_STATUS_FETCH_REPORT_INTERVAL value: %s (ignored)\n", env_i);
    }
  }

  /* Auto-refresh (ms) env override for UI suggested interval (only if not set via PlParam) */
  if (!g_cfg_fetch_auto_refresh_set) {
    const char *env_af = getenv("OLSRD_STATUS_FETCH_AUTO_REFRESH_MS");
    if (env_af && env_af[0]) {
      char *endptr = NULL; long v = strtol(env_af, &endptr, 10);
      if (endptr && *endptr == '\0' && v >= 0 && v <= 600000) {
        g_fetch_auto_refresh_ms = (int)v;
        fprintf(stderr, "[status-plugin] overriding fetch_auto_refresh_ms from env: %d\n", g_fetch_auto_refresh_ms);
      } else fprintf(stderr, "[status-plugin] invalid OLSRD_STATUS_FETCH_AUTO_REFRESH_MS value: %s (ignored)\n", env_af);
    }
  }

  /* fetch queue logging toggle via env (0=off,1=on) */
  if (!g_cfg_fetch_log_queue_set) {
    const char *env_lq = getenv("OLSRD_STATUS_FETCH_LOG_QUEUE");
    if (env_lq && env_lq[0]) {
      char *endptr = NULL; long v = strtol(env_lq, &endptr, 10);
      if (endptr && *endptr == '\0' && (v == 0 || v == 1)) {
        g_fetch_log_queue = (int)v;
        fprintf(stderr, "[status-plugin] setting fetch_log_queue from env: %d\n", g_fetch_log_queue);
      } else fprintf(stderr, "[status-plugin] invalid OLSRD_STATUS_FETCH_LOG_QUEUE value: %s (ignored)\n", env_lq);
    }
  }

  /* Start periodic reporter if requested */
  if (g_fetch_report_interval > 0) {
    pthread_create(&g_fetch_report_thread, NULL, fetch_reporter, NULL);
    pthread_detach(g_fetch_report_thread);
  }

  const char *env_ttl = getenv("OLSRD_STATUS_PLUGIN_NODEDB_TTL");
  if (env_ttl && env_ttl[0] && !g_cfg_nodedb_ttl_set) {
      char *endptr = NULL; long t = strtol(env_ttl, &endptr, 10);
      if (endptr && *endptr == '\0' && t >= 0) {
        g_nodedb_ttl = (int)t;
        fprintf(stderr, "[status-plugin] overriding nodedb_ttl from environment: %ld\n", t);
      } else {
        fprintf(stderr, "[status-plugin] invalid OLSRD_STATUS_PLUGIN_NODEDB_TTL value: %s (ignored)\n", env_ttl);
      }
    }

  const char *env_wd = getenv("OLSRD_STATUS_PLUGIN_NODEDB_WRITE_DISK");
  if (env_wd && env_wd[0] && !g_cfg_nodedb_write_disk_set) {
      char *endptr = NULL; long w = strtol(env_wd, &endptr, 10);
      if (endptr && *endptr == '\0' && w >= 0) {
        g_nodedb_write_disk = (int)w;
        fprintf(stderr, "[status-plugin] overriding nodedb_write_disk from environment: %d\n", g_nodedb_write_disk);
      } else {
        fprintf(stderr, "[status-plugin] invalid OLSRD_STATUS_PLUGIN_NODEDB_WRITE_DISK value: %s (ignored)\n", env_wd);
      }
    }
  }

  /* Optional: allow overriding initial DNS/network wait (seconds) */
  const char *env_wait = getenv("OLSRD_STATUS_FETCH_STARTUP_WAIT");
  if (env_wait && env_wait[0]) {
    char *endptr = NULL; long w = strtol(env_wait, &endptr, 10);
    if (endptr && *endptr == '\0' && w >= 0 && w <= 300) {
      g_nodedb_startup_wait = (int)w;
      fprintf(stderr, "[status-plugin] overriding startup DNS wait: %d seconds\n", g_nodedb_startup_wait);
    } else {
      fprintf(stderr, "[status-plugin] invalid OLSRD_STATUS_FETCH_STARTUP_WAIT value: %s (ignored)\n", env_wait);
    }
  }

  if (http_server_start(g_bind, g_port, g_asset_root) != 0) {
    fprintf(stderr, "[status-plugin] failed to start http server on %s:%d\n", g_bind, g_port);
    return 1;
  }
  /* capture plugin stderr into an in-process ring buffer for /log */
  start_stderr_capture();
  http_server_register_handler("/",         &h_root);
  http_server_register_handler("/index.html", &h_root);
  http_server_register_handler("/ipv4",     &h_ipv4);
  http_server_register_handler("/ipv6",     &h_ipv6);
  http_server_register_handler("/status",   &h_status);
  http_server_register_handler("/status/summary", &h_status_summary);
  http_server_register_handler("/status/olsr", &h_status_olsr);
  http_server_register_handler("/status/lite", &h_status_lite);
  http_server_register_handler("/status.py", &h_status_py);
  http_server_register_handler("/olsr/links", &h_olsr_links);
  http_server_register_handler("/olsr/links_debug", &h_olsr_links_debug);
  http_server_register_handler("/olsr/routes", &h_olsr_routes);
  http_server_register_handler("/olsr/raw", &h_olsr_raw); /* debug */
  http_server_register_handler("/olsrd.json", &h_olsrd_json);
  http_server_register_handler("/capabilities", &h_capabilities_local);
  http_server_register_handler("/nodedb/refresh", &h_nodedb_refresh);
  http_server_register_handler("/metrics", &h_prometheus_metrics);
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
  http_server_register_handler("/fetch_metrics", &h_fetch_metrics);
  http_server_register_handler("/fetch_debug", &h_fetch_debug);
  http_server_register_handler("/log", &h_log);
  http_server_register_handler("/traceroute", &h_traceroute);
  fprintf(stderr, "[status-plugin] listening on %s:%d (assets: %s)\n", g_bind, g_port, g_asset_root);
  /* start background workers */
  start_devices_worker();
  /* start node DB background worker */
  start_nodedb_worker();
  /* install SIGSEGV handler for diagnostic backtraces */
  signal(SIGSEGV, sigsegv_handler);
  return 0;
}

void olsrd_plugin_exit(void) {
  http_server_stop();
  /* stop devices worker and free cache */
  g_devices_worker_running = 0;
  pthread_mutex_lock(&g_devices_cache_lock);
  if (g_devices_cache) { free(g_devices_cache); g_devices_cache = NULL; g_devices_cache_len = 0; }
  pthread_mutex_unlock(&g_devices_cache_lock);
  /* stop nodedb worker and free cache */
  g_nodedb_worker_running = 0;
  pthread_mutex_lock(&g_nodedb_lock);
  if (g_nodedb_cached) { free(g_nodedb_cached); g_nodedb_cached = NULL; g_nodedb_cached_len = 0; }
  pthread_mutex_unlock(&g_nodedb_lock);
  /* stop stderr capture */
  stop_stderr_capture();
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

/* In-process stderr capture: pipe stderr into a reader thread and store recent lines
 * in a circular buffer so the /log HTTP endpoint can return recent plugin logs.
 */
#define LOG_BUF_LINES 8192
#define LOG_LINE_MAX 512
static char g_log_buf[LOG_BUF_LINES][LOG_LINE_MAX];
static int g_log_head = 0; /* next write index */
static int g_log_count = 0; /* number of stored lines */
static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_stderr_pipe_rd = -1;
static int g_orig_stderr_fd = -1;
static pthread_t g_stderr_thread = 0;
static int g_stderr_thread_running = 0;

static void ringbuf_push(const char *s) {
  pthread_mutex_lock(&g_log_lock);
  snprintf(g_log_buf[g_log_head], LOG_LINE_MAX, "%s", s);
  g_log_head = (g_log_head + 1) % LOG_BUF_LINES;
  if (g_log_count < LOG_BUF_LINES) g_log_count++;
  pthread_mutex_unlock(&g_log_lock);
}

static void *stderr_reader_thread(void *arg) {
  (void)arg;
  int fd = g_stderr_pipe_rd;
  char inbuf[1024]; char line[LOG_LINE_MAX]; size_t lp = 0;
  g_stderr_thread_running = 1;
  while (g_stderr_thread_running) {
    ssize_t n = read(fd, inbuf, sizeof(inbuf));
    if (n <= 0) break;
    for (ssize_t i = 0; i < n; i++) {
      char c = inbuf[i];
      if (c == '\r') continue;
      if (c == '\n' || lp+1 >= sizeof(line)) {
        line[lp] = '\0';
        /* write-through to original stderr so system logs still see output */
        if (g_orig_stderr_fd >= 0) {
          dprintf(g_orig_stderr_fd, "%s\n", line);
        }
        ringbuf_push(line);
        lp = 0;
      } else {
        line[lp++] = c;
      }
    }
  }
  /* flush any partial line */
  if (lp > 0) {
    line[lp] = '\0'; if (g_orig_stderr_fd >= 0) dprintf(g_orig_stderr_fd, "%s\n", line); ringbuf_push(line);
  }
  return NULL;
}

static int start_stderr_capture(void) {
  int pipefd[2];
  if (pipe(pipefd) != 0) return -1;
  /* duplicate current stderr so we can forward writes to it */
  g_orig_stderr_fd = dup(STDERR_FILENO);
  if (g_orig_stderr_fd < 0) { close(pipefd[0]); close(pipefd[1]); return -1; }
  /* replace stderr with pipe writer end */
  if (dup2(pipefd[1], STDERR_FILENO) < 0) { close(pipefd[0]); close(pipefd[1]); return -1; }
  close(pipefd[1]);
  g_stderr_pipe_rd = pipefd[0];
  /* start reader thread */
  if (pthread_create(&g_stderr_thread, NULL, stderr_reader_thread, NULL) != 0) {
    close(g_stderr_pipe_rd); g_stderr_pipe_rd = -1; return -1;
  }
  pthread_detach(g_stderr_thread);
  return 0;
}

static void stop_stderr_capture(void) {
  g_stderr_thread_running = 0;
  if (g_stderr_pipe_rd >= 0) { close(g_stderr_pipe_rd); g_stderr_pipe_rd = -1; }
  if (g_orig_stderr_fd >= 0) {
    /* restore original stderr */
    dup2(g_orig_stderr_fd, STDERR_FILENO);
    close(g_orig_stderr_fd); g_orig_stderr_fd = -1;
  }
}

/* HTTP handler for /log - supports ?lines=N or ?minutes=M (approximate) */
static int h_log(http_request_t *r) {
  char qv[64]; int lines = 2000;
  if (get_query_param(r, "lines", qv, sizeof(qv))) { lines = atoi(qv); if (lines <= 0) lines = 2000; if (lines > LOG_BUF_LINES) lines = LOG_BUF_LINES; }
  if (get_query_param(r, "minutes", qv, sizeof(qv))) {
    int mins = atoi(qv); if (mins > 0) {
      int approx = mins * 30; if (approx < lines) lines = approx; if (lines > LOG_BUF_LINES) lines = LOG_BUF_LINES;
    }
  }
  pthread_mutex_lock(&g_log_lock);
  int avail = g_log_count;
  if (lines < avail) avail = lines;
  int start = (g_log_head - avail + LOG_BUF_LINES) % LOG_BUF_LINES;
  /* build JSON in heap buffer */
  size_t est = (size_t)avail * 256 + 256; char *buf = malloc(est); if (!buf) { pthread_mutex_unlock(&g_log_lock); send_json(r, "{\"err\":\"oom\"}\n"); return 0; }
  size_t off = 0; off += snprintf(buf+off, est-off, "{\"lines\":[");
  for (int i = 0; i < avail; i++) {
    int idx = (start + i) % LOG_BUF_LINES;
    /* escape double quotes and backslashes */
    char esc[LOG_LINE_MAX*2]; size_t eo = 0; const char *s = g_log_buf[idx];
    for (size_t j = 0; s[j] && eo+3 < sizeof(esc); j++) {
      if (s[j] == '"' || s[j] == '\\') { esc[eo++] = '\\'; esc[eo++] = s[j]; }
      else if ((unsigned char)s[j] < 32) { esc[eo++] = '?'; }
      else esc[eo++] = s[j];
    }
    esc[eo] = '\0';
    off += snprintf(buf+off, est-off, "%s\"%s\"", i?",":"", esc);
    if (off + 256 > est) { est *= 2; char *nb = realloc(buf, est); if (!nb) break; buf = nb; }
  }
  off += snprintf(buf+off, est-off, "]}\n");
  pthread_mutex_unlock(&g_log_lock);
  send_json(r, buf);
  free(buf);
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
  char url[256];
  snprintf(url, sizeof(url), "http://127.0.0.1:2006/%s", q);
  char *out=NULL; size_t n=0;
  if (util_http_get_url_local(url, &out, &n, 1)==0 && out) {
    http_send_status(r, 200, "OK");
    http_printf(r, "Content-Type: text/plain; charset=utf-8\r\n\r\n");
    http_write(r, out, n); free(out);
  } else send_text(r, "error\n");
  return 0;
}
static int h_jsoninfo(http_request_t *r) {
  char q[64]="version";
  (void)get_query_param(r, "q", q, sizeof(q));
  char url[256]; snprintf(url, sizeof(url), "http://127.0.0.1:9090/%s", q);
  char *out=NULL; size_t n=0;
  if (util_http_get_url_local(url, &out, &n, 1)==0 && out) {
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
  // External scripts removed: rely on internal renderer only

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
  /* Use internal generator rather than an external shell script */
  char *out = NULL; size_t n = 0;
  if (generate_versions_json(&out, &n) == 0 && out && n>0) {
    http_send_status(r,200,"OK");
    http_printf(r,"Content-Type: application/json; charset=utf-8\r\n\r\n");
    http_write(r,out,n); free(out); return 0;
  }
  /* fallback: synthesize a useful versions JSON inline (no external script)
   * Provide info useful for both EdgeRouter and container setups.
   */
  char host[256]=""; gethostname(host,sizeof(host)); host[sizeof(host)-1]=0;
  int olsrd_on=0, olsr2_on=0; detect_olsr_processes(&olsrd_on,&olsr2_on);

  /* autoupdate wizard info */
  const char *au_path = "/etc/cron.daily/autoupdatewizards";
  int auon = path_exists(au_path);
  char *adu_dat = NULL; size_t adu_n = 0;
  util_read_file("/config/user-data/autoupdate.dat", &adu_dat, &adu_n);
  int aa_on = 0, aa1_on = 0, aa2_on = 0, aale_on = 0, aaebt_on = 0, aabp_on = 0;
  if (adu_dat && adu_n>0) {
    if (memmem(adu_dat, adu_n, "wizard-autoupdate=yes", 20)) aa_on = 1;
    if (memmem(adu_dat, adu_n, "wizard-olsrd_v1=yes", 19)) aa1_on = 1;
    if (memmem(adu_dat, adu_n, "wizard-olsrd_v2=yes", 19)) aa2_on = 1;
    if (memmem(adu_dat, adu_n, "wizard-0xffwsle=yes", 18)) aale_on = 1;
    if (memmem(adu_dat, adu_n, "wizard-ebtables=yes", 18)) aaebt_on = 1;
    if (memmem(adu_dat, adu_n, "wizard-blockPrivate=yes", 24)) aabp_on = 1;
  }

  // Gather wizard versions (if present under /config/wizard/feature/*/wizard-run)
  // We'll execute a small shell loop and parse lines like key=version to extract values.
  char *wiz_out = NULL; size_t wiz_n = 0;
  char olsrv1_ver[64] = "n/a", olsrv2_ver[64] = "n/a", wsle_ver[64] = "n/a", ebtables_ver[64] = "n/a", blockpriv_ver[64] = "n/a", autoupdate_ver[64] = "n/a";
  if (path_exists("/config/wizard")) {
  const char *wiz_cmd = "for i in /config/wizard/feature/*/wizard-run 2>/dev/null; do vers=$(head -n 10 \"$i\" | grep -ioE -m 1 'version.*' | awk -F' ' '{print $2;}' | tr -d '[]() '); if head -n 10 \"$i\" | grep -q 'OLSRd_V1'; then echo olsrv1=$vers; fi; if head -n 10 \"$i\" | grep -q 'OLSRd_V2'; then echo olsrv2=$vers; fi; if head -n 10 \"$i\" | grep -q '0xFF-BMK-Webstatus-LetsEncrypt'; then echo wsle=$vers; fi; if head -n 10 \"$i\" | grep -q 'ER-wizard-ebtables'; then echo ebtables=$vers; fi; if head -n 10 \"$i\" | grep -q 'ER-wizard-blockPrivate'; then echo blockpriv=$vers; fi; if head -n 10 \"$i\" | grep -q 'ER-wizard-AutoUpdate'; then echo autoupdate=$vers; fi; done";
    if (util_exec(wiz_cmd, &wiz_out, &wiz_n) == 0 && wiz_out && wiz_n>0) {
      /* parse lines */
      char *p = wiz_out; char *line = NULL;
      while (p && *p) {
        line = p; char *nl = strchr(line,'\n'); if (nl) *nl = '\0';
        if (strncmp(line, "olsrv1=", 7) == 0) strncpy(olsrv1_ver, line+7, sizeof(olsrv1_ver)-1);
        else if (strncmp(line, "olsrv2=", 7) == 0) strncpy(olsrv2_ver, line+7, sizeof(olsrv2_ver)-1);
        else if (strncmp(line, "wsle=", 5) == 0) strncpy(wsle_ver, line+5, sizeof(wsle_ver)-1);
        else if (strncmp(line, "ebtables=", 9) == 0) strncpy(ebtables_ver, line+9, sizeof(ebtables_ver)-1);
        else if (strncmp(line, "blockpriv=", 10) == 0) strncpy(blockpriv_ver, line+10, sizeof(blockpriv_ver)-1);
        else if (strncmp(line, "autoupdate=", 11) == 0) strncpy(autoupdate_ver, line+11, sizeof(autoupdate_ver)-1);
        if (!nl) {
          break;
        }
        p = nl + 1;
      }
    }
  }

  /* homes (users) - simple listing */
  char *homes_out = NULL; size_t homes_n = 0;
  if (util_exec("/bin/ls -1 /home 2>/dev/null | awk '{printf \"\\\"%s\\\",\", $0}' | sed 's/,$/\\n/'", &homes_out, &homes_n) != 0) {
    if (homes_out) { free(homes_out); homes_out = NULL; homes_n = 0; }
  }
  if (!homes_out) {
    /* fallback to empty array */
    homes_out = strdup("\n"); homes_n = homes_out ? strlen(homes_out) : 0;
  }

  /* boot image md5 */
  char *md5_out = NULL; size_t md5_n = 0;
  if (util_exec("/usr/bin/md5sum /dev/mtdblock2 2>/dev/null | cut -f1 -d' '", &md5_out, &md5_n) != 0) {
    if (md5_out) { free(md5_out); md5_out = NULL; md5_n = 0; }
  }

  /* Determine system type heuristically */
  const char *system_type = path_exists("/config/wizard") ? "edge-router" : "linux-container";

  /* bmk-webstatus version (if present) */
  char *bmk_out = NULL; size_t bmk_n = 0; char bmkwebstatus[128] = "n/a";
  if (util_exec("head -n 12 /config/custom/www/cgi-bin-status*.php 2>/dev/null | grep -m1 version= | cut -d'\"' -f2", &bmk_out, &bmk_n) == 0 && bmk_out && bmk_n>0) {
    char *t = strndup(bmk_out, (size_t)bmk_n);
    if (t) { char *nl = strchr(t,'\n'); if (nl) *nl = 0; strncpy(bmkwebstatus, t, sizeof(bmkwebstatus)-1); free(t); }
  }

  /* olsrd4 watchdog flag: check EdgeRouter path first, then container path */
  int olsrd4watchdog = 0;
  char *olsrd4conf = NULL; size_t olsrd4_n = 0;
  if (util_read_file("/config/user-data/olsrd4.conf", &olsrd4conf, &olsrd4_n) != 0) {
    /* fallback to common linux container path */
    if (util_read_file("/etc/olsrd/olsrd.conf", &olsrd4conf, &olsrd4_n) != 0) {
      olsrd4conf = NULL; olsrd4_n = 0;
    }
  }
  if (olsrd4conf && olsrd4_n>0) {
    if (memmem(olsrd4conf, olsrd4_n, "olsrd_watchdog", 13) || memmem(olsrd4conf, olsrd4_n, "LoadPlugin.*olsrd_watchdog", 22)) olsrd4watchdog = 1;
    free(olsrd4conf); olsrd4conf = NULL; olsrd4_n = 0;
  }

  /* local IPs: try to get a reasonable IPv4 and IPv6; prefer non-loopback addresses */
  char ipv4_addr[64] = "n/a"; char ipv6_addr[128] = "n/a"; char originator[128] = "n/a";
  char *tmp_out = NULL; size_t tmp_n = 0;
  if (util_exec("ip -4 -o addr show scope global | awk '{print $4; exit}' | cut -d/ -f1", &tmp_out, &tmp_n) == 0 && tmp_out && tmp_n>0) {
    char *t = strndup(tmp_out, tmp_n); if (t) { char *nl = strchr(t,'\n'); if (nl) *nl = 0; strncpy(ipv4_addr, t, sizeof(ipv4_addr)-1); free(t); }
    free(tmp_out); tmp_out = NULL; tmp_n = 0;
  }
  if (util_exec("ip -6 -o addr show scope global | awk '{print $4; exit}' | cut -d/ -f1", &tmp_out, &tmp_n) == 0 && tmp_out && tmp_n>0) {
    char *t = strndup(tmp_out, tmp_n); if (t) { char *nl = strchr(t,'\n'); if (nl) *nl = 0; strncpy(ipv6_addr, t, sizeof(ipv6_addr)-1); free(t); }
    free(tmp_out); tmp_out = NULL; tmp_n = 0;
  }
  /* originator: if olsr2 running, try local telnet endpoint */
  if (olsr2_on) {
    char *orig_raw = NULL; size_t orig_n = 0;
    if (util_http_get_url_local("http://127.0.0.1:8000/telnet/olsrv2info%20originator", &orig_raw, &orig_n, 1) == 0 && orig_raw && orig_n>0) {
      /* take first line that contains ':' */
      char *nl = strchr(orig_raw,'\n'); if (nl) *nl = 0; if (strchr(orig_raw,':')) strncpy(originator, orig_raw, sizeof(originator)-1);
      free(orig_raw);
    }
  }

  /* linklocals: capture eth0 MAC as serial-like identifier (best-effort) */
  char linkserial[128] = "n/a";
  char *ll_out = NULL; size_t ll_n = 0;
  if (util_exec("ip -6 link show eth0 2>/dev/null | grep link/ether | awk '{gsub(\":\",\"\", $2); print toupper($2)}'", &ll_out, &ll_n) == 0 && ll_out && ll_n>0) {
    char *t = strndup(ll_out, ll_n); if (t) { char *nl = strchr(t,'\n'); if (nl) *nl = 0; strncpy(linkserial, t, sizeof(linkserial)-1); free(t); }
  }


  /* Build JSON into dynamic buffer */
  size_t buf_sz = 4096 + (homes_n>0?homes_n:0) + (md5_n>0?md5_n:0);
  char *obuf = malloc(buf_sz);
  if (!obuf) {
    /* out of memory: fallback minimal */
    char buf2[512]; snprintf(buf2,sizeof(buf2),"{\"olsrd_status_plugin\":\"%s\",\"host\":\"%s\"}\n","1.0",host);
    send_json(r, buf2);
    if (adu_dat) free(adu_dat);
    return 0;
  }
  /* sanitize homes_out (it should contain quoted comma separated list or newline) */
  char homes_json[512] = "[]";
  if (homes_out && homes_n>0) {
    /* homes_out already formatted by the ls command above ("user","user2",) */
    size_t hn = homes_n;
    /* remove trailing comma/newline and ensure brackets */
    char *tmp = strndup(homes_out, homes_n);
    if (tmp) {
      /* strip trailing comma or newline */
      while (hn>0 && (tmp[hn-1]=='\n' || tmp[hn-1]==',')) { tmp[--hn]=0; }
      snprintf(homes_json, sizeof(homes_json), "[%s]", tmp[0] ? tmp : "");
      free(tmp);
    }
  }

  /* md5 cleanup */
  char bootimage_md5[128] = "n/a";
  if (md5_out && md5_n>0) {
    /* trim newline */
    char *m = strndup(md5_out, md5_n);
    if (m) {
      char *nl = strchr(m,'\n'); if (nl) *nl = 0; strncpy(bootimage_md5, m, sizeof(bootimage_md5)-1); bootimage_md5[sizeof(bootimage_md5)-1]=0; free(m);
    }
  }

  snprintf(obuf, buf_sz,
    "{\"host\":\"%s\",\"system\":\"%s\",\"olsrd_running\":%s,\"olsr2_running\":%s,\"olsrd4watchdog\":%s,\"autoupdate_wizards_installed\":\"%s\",\"autoupdate_settings\":{\"auto_update_enabled\":%s,\"olsrd_v1\":%s,\"olsrd_v2\":%s,\"wsle\":%s,\"ebtables\":%s,\"blockpriv\":%s},\"homes\":%s,\"bootimage\":{\"md5\":\"%s\"}}\n",
    host,
    system_type,
    olsrd_on?"true":"false",
    olsr2_on?"true":"false",
    olsrd4watchdog?"true":"false",
    auon?"yes":"no",
    aa_on?"true":"false",
    aa1_on?"true":"false",
    aa2_on?"true":"false",
    aale_on?"true":"false",
    aaebt_on?"true":"false",
    aabp_on?"true":"false",
    homes_json,
    bootimage_md5
  );

  http_send_status(r,200,"OK");
  http_printf(r,"Content-Type: application/json; charset=utf-8\r\n\r\n");
  http_write(r, obuf, strlen(obuf));
  free(obuf);
  if (adu_dat) free(adu_dat);
  if (homes_out) free(homes_out);
  if (md5_out) free(md5_out);
  return 0;
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
