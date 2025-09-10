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
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/select.h>
#include <sys/time.h>

#include "ubnt_discover.h"

int main(void) {
    int sock = ubnt_open_broadcast_socket(0);
    if (sock < 0) {
        perror("socket");
        return 1;
    }

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons(10001);
    inet_pton(AF_INET, "255.255.255.255", &dst.sin_addr);

    if (ubnt_discover_send(sock, &dst) != 0) {
        perror("sendto");
        return 2;
    }

    fprintf(stderr, "Sent UBNT v1 probe to 255.255.255.255:10001; listening for 3 seconds...\n");

    struct timeval tv_end;
    gettimeofday((struct timeval*)&tv_end, NULL);
    time_t start = time(NULL);
    time_t timeout_sec = 3;

    while (time(NULL) - start < timeout_sec) {
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
