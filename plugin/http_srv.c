#include "http_srv.h"
#include "web_ui.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define MAX_HEAD 4096

static void send_all(int fd, const void *p, size_t n) {
    const char *c = p;
    while (n) {
        ssize_t w = send(fd, c, n, MSG_NOSIGNAL);
        if (w <= 0) return;
        c += w; n -= (size_t)w;
    }
}

static void respond(int fd, int code, const char *type, const void *body, size_t len) {
    const char *reason = code == 200 ? "OK" : code == 400 ? "Bad Request" : code == 404 ? "Not Found" :
                         code == 413 ? "Payload Too Large" : "Internal Server Error";
    char h[384];
    int n = snprintf(h, sizeof h,
        "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nCache-Control: no-store\r\n"
        "Connection: close\r\nContent-Length: %zu\r\n\r\n", code, reason, type, len);
    send_all(fd, h, (size_t)n);
    send_all(fd, body, len);
}

static void handle(int fd, struct sky_api *api) {
    static char buf[MAX_HEAD + SKY_API_MAX_BODY + 16];
    size_t used = 0, hlen = 0;
    long need = 0;
    while (used < sizeof buf - 1) {
        ssize_t n = recv(fd, buf + used, sizeof buf - 1 - used, 0);
        if (n <= 0) break;
        used += (size_t)n;
        buf[used] = 0;
        if (!hlen) {
            char *sep = strstr(buf, "\r\n\r\n");
            if (!sep) { if (used >= MAX_HEAD) break; continue; }
            hlen = (size_t)(sep + 4 - buf);
            const char *cl = strcasestr(buf, "\r\ncontent-length:");
            need = cl && cl < sep ? strtol(cl + 17, NULL, 10) : 0;
            if (need < 0 || need > SKY_API_MAX_BODY) { respond(fd, 413, "text/plain", "too large", 9); return; }
        }
        if (used >= hlen + (size_t)need) break;
    }
    if (!hlen || used < hlen + (size_t)need) { respond(fd, 400, "text/plain", "bad request", 11); return; }

    char method[8] = {0}, target[160] = {0};
    if (sscanf(buf, "%7s %159s", method, target) != 2) { respond(fd, 400, "text/plain", "bad request", 11); return; }

    if (!strcmp(method, "GET") && (!strcmp(target, "/") || !strcmp(target, "/index.html"))) {
        respond(fd, 200, "text/html; charset=utf-8", SKY_WEB_UI, sizeof(SKY_WEB_UI) - 1);
        return;
    }
    static char out[SKY_FIG_SIZE + 4096];
    size_t olen = 0; const char *ct = "application/json";
    int code = sky_api_handle(api, method, target, (const uint8_t *)buf + hlen, (size_t)need, out, sizeof out, &olen, &ct);
    respond(fd, code, ct, out, olen);
}

int sky_http_serve(struct sky_api *api, int port) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;
    int yes = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons((uint16_t)port); a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, (struct sockaddr *)&a, sizeof a) < 0 || listen(s, 8) < 0) { close(s); return -2; }
    for (;;) {
        int c = accept(s, NULL, NULL);
        if (c < 0) continue;
        struct timeval tv = { 5, 0 };                      /* a stalled client must not wedge the loop */
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        handle(c, api);
        close(c);
    }
}
