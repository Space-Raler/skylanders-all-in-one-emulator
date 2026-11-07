// Plain PS5 payload entry point (send the ELF to elfldr, e.g. port 9021, or put it in
// /data/OnionHEN/payloads). Everything of substance lives in the host-tested C core
// (daemon.c / injector.c / api.c / http_srv.c); this file only supplies the console glue.
#ifdef SKY_HAVE_PS5_SDK

#include <pthread.h>
#include <signal.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <ps5/payload.h>     // payload_get_args(): kdata_base_addr, needed by libhijacker

extern "C" {
#include "daemon.h"
#include "http_srv.h"
int  sky_hijack_open(int pid, struct sky_proc *out);
void sky_hijack_close(void);

int sceSystemServiceGetAppIdOfRunningBigApp();
int sceSystemServiceGetAppTitleId(int app_id, char *title_id);
int _sceApplicationGetAppId(int pid, int *appId);
int sceKernelSendNotificationRequest(int device, void *req, size_t size, int blocking);
}
#include "sky_log.h"

#define ROOT_DIR  "/data/skylanders"
#define LOG_PATH  ROOT_DIR "/skyportal.log"
#define CFG_PATH  ROOT_DIR "/skyportal.ini"

// libhijacker (include/kernel/proc.hpp) reads this global; the SDK's sample plugins define it exactly so.
uintptr_t kernel_base = 0;

extern "C" void sky_log(const char *fmt, ...) {
    char msg[560];
    // seconds since the payload started, so log lines can be lined up with what you did on screen
    static timespec t0 = {0, 0};
    timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
    if (t0.tv_sec == 0 && t0.tv_nsec == 0) t0 = now;
    long ms = (now.tv_sec - t0.tv_sec) * 1000L + (now.tv_nsec - t0.tv_nsec) / 1000000L;
    int off = snprintf(msg, 24, "[+%ld.%03lds] ", ms / 1000, ms % 1000);
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(msg + off, sizeof msg - 1 - (size_t)off, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof msg - 2 - off) n = (int)sizeof msg - 2 - off;
    n += off;
    msg[n++] = '\n';
    msg[n] = 0;
    fputs(msg, stdout);
    int fd = open(LOG_PATH, O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd >= 0) { (void)write(fd, msg, (size_t)n); fsync(fd); close(fd); }   // fsync: the line must survive a console crash
}

// The usual on-screen toast used by payload-SDK samples (layout: 45 unknown bytes, then the text).
static void notify(const char *fmt, ...) {
    struct { char unk[45]; char message[3075]; } req;
    memset(&req, 0, sizeof req);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(req.message, sizeof req.message, fmt, ap);
    va_end(ap);
    sceKernelSendNotificationRequest(0, &req, sizeof req, 0);
}

// Our LAN address: a UDP connect() sends nothing, it only picks the route, so getsockname() gives the IP.
static void ip_string(char *out, size_t cap) {
    snprintf(out, cap, "<ps5-ip>");
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return;
    sockaddr_in to; memset(&to, 0, sizeof to);
    to.sin_family = AF_INET; to.sin_port = htons(9); to.sin_addr.s_addr = htonl(0x08080808);
    sockaddr_in me; memset(&me, 0, sizeof me);
    socklen_t ml = sizeof me;
    if (connect(s, (sockaddr *)&to, sizeof to) == 0 && getsockname(s, (sockaddr *)&me, &ml) == 0)
        snprintf(out, cap, "%s", inet_ntoa(me.sin_addr));
    close(s);
}

static sky_config g_cfg;

static bool title_configured(const char *t) {
    for (int i = 0; i < g_cfg.n_titles; i++)
        if (!strcmp(g_cfg.titles[i].id, t)) return true;
    return false;
}

// Title of the running "big app" (the foreground game) and its pid.
//  * returns -1 when no game is running, or a configured game's pid cannot be found yet;
//  * returns 0 with *pid = 0 for games we are not configured for (the daemon ignores them);
//  * pid discovery scans pids and asks which app each belongs to (10k calls), so it only runs for
//    configured titles and the result is cached until the app changes.
static int current_app(void *, char *title, size_t cap, int *pid) {
    int appid = sceSystemServiceGetAppIdOfRunningBigApp();
    if (appid <= 0) return -1;
    char tid[256] = {0};
    if (sceSystemServiceGetAppTitleId(appid, tid) != 0) return -1;
    snprintf(title, cap, "%s", tid);
    *pid = 0;
    if (!title_configured(title)) return 0;

    static int c_app = 0, c_pid = 0;
    if (c_app == appid && c_pid > 0) {
        int chk = 0;
        if (_sceApplicationGetAppId(c_pid, &chk) >= 0 && chk == appid) { *pid = c_pid; return 0; }
    }
    for (int p = 1; p <= 9999; p++) {
        int a = 0;
        if (_sceApplicationGetAppId(p, &a) < 0) continue;
        if (a == appid) { c_app = appid; c_pid = p; *pid = p; sky_log("%s: appid 0x%X pid %d", title, appid, p); return 0; }
    }
    return -1;
}
static int  open_proc(void *, int pid, struct sky_proc *out) { return sky_hijack_open(pid, out); }
static void close_proc(void *) { sky_hijack_close(); }
static void sleep_ms(void *, int ms) { usleep((useconds_t)ms * 1000); }
static void logmsg(void *, const char *m) {
    sky_log("[SkyPortal] %s", m);
    if (!strncmp(m, "hooked ", 7)) notify("SkyPortal: game hooked");   // everything else only goes to the log file
}

static sky_daemon g_daemon;

static void *http_thread(void *) {
    int rc = sky_http_serve(&g_daemon.api, g_daemon.cfg.port);
    sky_log("http server stopped (rc=%d)", rc);
    notify("SkyPortal: web server on port %d stopped (rc=%d)", g_daemon.cfg.port, rc);
    return nullptr;
}

int main() {
    signal(SIGPIPE, SIG_IGN);                 // a phone dropping a connection mid-reply must not kill the payload
    mkdir(ROOT_DIR, 0777);
    sky_log("SkyPortal payload 0.1.0 starting");

    // libhijacker needs the kernel data base the payload loader hands us. Without it every kernel read
    // would fault, so stop here with a message instead of crashing later.
    payload_args_t *args = payload_get_args();
    if (!args || !args->kdata_base_addr) {
        sky_log("payload_get_args() gave no kdata_base_addr; was this ELF started by a payload loader?");
        notify("SkyPortal: no kernel base from the loader; not starting");
        return 1;
    }
    kernel_base = (uintptr_t)args->kdata_base_addr;
    sky_log("kernel_base=0x%llx", (unsigned long long)kernel_base);

    static char cfgtext[2048];
    FILE *f = fopen(CFG_PATH, "r");
    size_t n = f ? fread(cfgtext, 1, sizeof cfgtext - 1, f) : 0;
    if (f) fclose(f);
    cfgtext[n] = 0;
    sky_config_parse(&g_cfg, cfgtext);
    if (g_cfg.n_titles == 0) {
        logmsg(nullptr, "no title= lines in " CFG_PATH "; nothing to do");
        return 1;
    }

    sky_platform pl = { nullptr, current_app, open_proc, close_proc, sleep_ms, logmsg };
    sky_daemon_init(&g_daemon, &g_cfg, &pl, ROOT_DIR);
    pthread_t th;
    pthread_create(&th, nullptr, http_thread, nullptr);
    char ip[32];
    ip_string(ip, sizeof ip);
    notify("SkyPortal ready: http://%s:%d", ip, g_cfg.port);
    for (unsigned tick = 0;; tick++) {                   // 50 ms tick
        if (tick % 20 == 0) sky_daemon_step(&g_daemon);  // watch for the game once a second
        if (tick % 5 == 0) sky_daemon_autosave(&g_daemon);   // progress autosave, 4 times a second
        sky_daemon_poll_trace(&g_daemon);                // keep the log within ~50 ms of the game's last command
        sleep_ms(nullptr, 50);
    }
}

#endif  // SKY_HAVE_PS5_SDK
