/* Desktop demo: full stack (HTTP + phone UI + daemon + injector + the real hook blob)
 * against a simulated game that pumps the USB event loop. Lets you click through the UI
 * and watch portal traffic without a console.
 *   make demo && build/host_demo [port] [data_dir]
 */
#include "daemon.h"
#include "http_srv.h"
#include "proc_fake.h"
#include "sceusbd_abi.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *const NIDS[] = {
    "TOhg7P6kTH4","L0EHgZZNVas","EkqGLxWC-S0","RRKFcKQ1Ka4","BKMEGvfCPyU","Fq6+0Fm55xU","bhomgbiQgeo",
    "Dkm5qe8j3XE","RLf56F-WjKQ","Y5go+ha6eDs","AE+mHBHneyk","0ktE1PhzGFU","t3J5pXxhJlI","Hvd3S--n25w",
    "8qB9Ar4P5nc","t7WE9mb1TB8","GjlCrU4GcIY","VJ6oMq-Di2U","EQ6SCLMqzkM","MlW6deWfPp0" };

static struct sky_fake_proc fp;
static uintptr_t orig(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e, uintptr_t f, uintptr_t g, uintptr_t h)
{ (void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g;(void)h; return 0; }

static int cur_app(void *c, char *t, size_t cap, int *pid) { (void)c; snprintf(t, cap, "CUSA00019"); *pid = 4242; return 0; }
static int open_proc(void *c, int pid, struct sky_proc *out) {
    (void)c; (void)pid;
    sky_fake_init(&fp);
    for (unsigned i = 0; i < sizeof NIDS / sizeof *NIDS; i++) sky_fake_add_import(&fp, NIDS[i], (void *)orig);
    *out = sky_fake_iface(&fp);
    return 0;
}
static void close_proc(void *c) { (void)c; }
static void sleep_ms(void *c, int ms) { (void)c; usleep((useconds_t)ms * 1000); }
static void lg(void *c, const char *m) { (void)c; fprintf(stderr, "[skyportal] %s\n", m); }

typedef uintptr_t (*fn8)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
static fn8 fn(const char *nid) { return (fn8)(uintptr_t)*sky_fake_slot(&fp, nid); }
static uint8_t last_status[32];

static void cb_in(struct sky_usb_transfer *t);
static void cb_in(struct sky_usb_transfer *t) {
    memcpy(last_status, t->buffer, 32);
    fprintf(stderr, "[game] portal report '%c' %02x %02x %02x %02x\n", last_status[0] >= 32 ? last_status[0] : '?',
            last_status[1], last_status[2], last_status[3], last_status[4]);
    fn("L0EHgZZNVas")((uintptr_t)t, 0, 0, 0, 0, 0, 0, 0);
}

static void *game(void *arg) {
    (void)arg;
    while (!fp.n_imports || !*sky_fake_slot(&fp, "TOhg7P6kTH4")) usleep(10000);
    usleep(200000);
    void **list = NULL, *h = NULL;
    fn("TOhg7P6kTH4")(0, 0, 0, 0, 0, 0, 0, 0);
    fn("8qB9Ar4P5nc")((uintptr_t)&list, 0, 0, 0, 0, 0, 0, 0);
    fn("VJ6oMq-Di2U")((uintptr_t)list[0], (uintptr_t)&h, 0, 0, 0, 0, 0, 0);
    static uint8_t buf[32], cmd[32] = { 'R' };
    struct sky_usb_transfer *tin = (void *)fn("0ktE1PhzGFU")(0, 0, 0, 0, 0, 0, 0, 0);
    fn("t3J5pXxhJlI")((uintptr_t)tin, (uintptr_t)h, 0x81, (uintptr_t)buf, 32, (uintptr_t)cb_in, 0, 0);
    fn("L0EHgZZNVas")((uintptr_t)tin, 0, 0, 0, 0, 0, 0, 0);
    fn("RRKFcKQ1Ka4")((uintptr_t)h, 0x21, 9, 0x200, 0, (uintptr_t)cmd, 32, 0);
    for (;;) fn("EkqGLxWC-S0")(0, 0, 0, 0, 0, 0, 0, 0);
    return NULL;
}

int main(int argc, char **argv) {
    int port = argc > 1 ? atoi(argv[1]) : 8787;
    const char *root = argc > 2 ? argv[2] : "/tmp/skyportal-demo";
    struct sky_config cfg;
    sky_config_parse(&cfg, "title=CUSA00019 ctx=0\nsettle_ms=0\n");
    struct sky_platform pl = { NULL, cur_app, open_proc, close_proc, sleep_ms, lg };
    static struct sky_daemon d;
    sky_daemon_init(&d, &cfg, &pl, root);
    pthread_t th; pthread_create(&th, NULL, game, NULL);
    sky_daemon_step(&d);
    fprintf(stderr, "[skyportal] demo on http://localhost:%d/  data in %s\n", port, root);
    return sky_http_serve(&d.api, port);
}
