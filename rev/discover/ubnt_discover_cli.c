/*
 * Simple CLI example:
 *   - broadcasts a v1 probe to 255.255.255.255:10001
 *   - waits for replies for a few seconds
 *   - prints parsed fields (best-effort)
 *
 * Build:
 *   gcc -O2 -Wall -o ubnt_discover examples/ubnt_discover_cli.c src/ubnt_discover.c
 *
 * Run (may require sudo depending on your network setup):
 *   ./ubnt_discover
 */
#define _POSIX_C_SOURCE 200112L
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/select.h>
#include <getopt.h>

#include "ubnt_discover.h"

/* Minimal stub used when building CLI standalone to satisfy plugin_log_trace calls */
#include <stdarg.h>
/* Prototype matching src/status_log.h to satisfy -Wmissing-prototypes */
void plugin_log_trace(const char *fmt, ...) __attribute__((format(printf,1,2)));

/* Implementation for standalone CLI */
void plugin_log_trace(const char *fmt, ...) {
    /* Format into a local buffer first. The compiler warns about non-literal
     * format strings; silence that warning locally.
     */
    char buf[1024];
    va_list ap; va_start(ap, fmt);
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wformat-nonliteral"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
#endif
    vsnprintf(buf, sizeof(buf), fmt, ap);
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
    va_end(ap);
    buf[sizeof(buf)-1] = '\0';
    fprintf(stderr, "%s\n", buf);
}

static void usage(const char *prog) {
    fprintf(stderr, "Usage: %s [-b bind_ip] [-p port] [-t timeout_seconds] [-d] [-h]\n", prog);
    fprintf(stderr, "  -b bind_ip        bind local address (default: any)\n");
    fprintf(stderr, "  -p port           discovery port to send to and optionally bind (default: 10001)\n");
    fprintf(stderr, "  -t timeout        listen timeout in seconds (default: 3)\n");
    fprintf(stderr, "  -d                enable runtime debug (sets OLSRD_STATUS_UBNT_DEBUG=1)\n");
    fprintf(stderr, "  -h                show this help\n");
}

int main(int argc, char **argv) {
    const char *bind_ip = NULL;
    uint16_t port = 10001;
    unsigned int timeout_sec = 3;
    int runtime_debug = 0;
    int opt;
    while ((opt = getopt(argc, argv, "b:p:t:dh")) != -1) {
        switch (opt) {
        case 'b': bind_ip = optarg; break;
        case 'p': port = (uint16_t)atoi(optarg); if (port == 0) { fprintf(stderr, "invalid port: %s\n", optarg); return 2; } break;
        case 't': timeout_sec = (unsigned int)atoi(optarg); if (timeout_sec == 0) timeout_sec = 1; break;
        case 'd': runtime_debug = 1; break;
        case 'h': default: usage(argv[0]); return 0;
        }
    }

    if (runtime_debug) {
        /* ubnt_discover.c checks the OLSRD_STATUS_UBNT_DEBUG env var at runtime */
        setenv("OLSRD_STATUS_UBNT_DEBUG", "1", 1);
    }
    /* Bind to the known UBNT discovery port so devices that reply to 10001 reach us.
     * Some devices send responses to port 10001 rather than the probe's ephemeral
     * source port. Binding to 10001 increases discovery yield on many networks.
     */
    int sock;
    if (bind_ip && bind_ip[0]) {
        sock = ubnt_open_broadcast_socket_bound(bind_ip, port);
    } else {
        sock = ubnt_open_broadcast_socket(port);
    }
    if (sock < 0) {
        perror("socket");
        return 1;
    }

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons(port);
    inet_pton(AF_INET, "255.255.255.255", &dst.sin_addr);

    if (ubnt_discover_send(sock, &dst) != 0) {
        perror("sendto");
        return 2;
    }

    if (bind_ip && bind_ip[0])
        fprintf(stderr, "Sent UBNT v1 probe to 255.255.255.255:%u (bound to %s); listening for %u seconds...\n", (unsigned)port, bind_ip, timeout_sec);
    else
        fprintf(stderr, "Sent UBNT v1 probe to 255.255.255.255:%u; listening for %u seconds...\n", (unsigned)port, timeout_sec);

    struct timeval tv_end;
    gettimeofday((struct timeval*)&tv_end, NULL);
    time_t start = time(NULL);
    /* timeout_sec from CLI */

    while (time(NULL) - start < (time_t)timeout_sec) {
        fd_set rfds; FD_ZERO(&rfds); FD_SET(sock, &rfds);
        struct timeval tv; tv.tv_sec = 0; tv.tv_usec = 300000; // 300ms
        int rc = select(sock+1, &rfds, NULL, NULL, &tv);
        if (rc > 0 && FD_ISSET(sock, &rfds)) {
            char ip[64];
            struct ubnt_kv kv[16]; size_t kvn = 16;
            int n = ubnt_discover_recv(sock, ip, sizeof(ip), kv, &kvn);
            if (n > 0) {
                printf("Device from %s:\n", ip);
                for (size_t i=0;i<kvn;i++) {
                    printf("  %-14s %s\n", kv[i].key, kv[i].value);
                }
                printf("\n");
            }
        }
    }

    close(sock);
    return 0;
}
