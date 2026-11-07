#include "daemon.h"
#include "proc_fake.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const SWAP[] = {
    "TOhg7P6kTH4","L0EHgZZNVas","EkqGLxWC-S0","RRKFcKQ1Ka4","BKMEGvfCPyU","Fq6+0Fm55xU","bhomgbiQgeo",
    "Dkm5qe8j3XE","RLf56F-WjKQ","Y5go+ha6eDs","AE+mHBHneyk","0ktE1PhzGFU","t3J5pXxhJlI","Hvd3S--n25w",
    "8qB9Ar4P5nc","t7WE9mb1TB8","GjlCrU4GcIY","VJ6oMq-Di2U","EQ6SCLMqzkM","MlW6deWfPp0" };

static struct sky_fake_proc fp;
static char cur_title[16]; static int cur_pid, opens, closes, logs, omit_submit;
static uintptr_t orig(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e, uintptr_t f, uintptr_t g, uintptr_t h)
{ (void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g;(void)h; return 0; }

static int cur_app(void *c, char *t, size_t cap, int *pid) { (void)c; if (!cur_pid) return -1; snprintf(t, cap, "%s", cur_title); *pid = cur_pid; return 0; }
static int open_proc(void *c, int pid, struct sky_proc *out) {
    (void)c; (void)pid; opens++;
    sky_fake_init(&fp);
    for (unsigned i = 0; i < sizeof SWAP / sizeof *SWAP; i++)
        if (!(omit_submit && !strcmp(SWAP[i], "L0EHgZZNVas"))) sky_fake_add_import(&fp, SWAP[i], (void *)orig);
    *out = sky_fake_iface(&fp);
    return 0;
}
static void close_proc(void *c) { (void)c; closes++; }
static void sleep_ms(void *c, int ms) { (void)c; (void)ms; }
static void lg(void *c, const char *m) { (void)c; logs++; fprintf(stderr, "    [log] %s\n", m); }

int main(void) {
    struct sky_config cfg;
    int n = sky_config_parse(&cfg, "# comment\nport=9000\nsettle_ms=0\ntitle=CUSA00019 ctx=0\ntitle=CUSA99999 ctx=1 check=1\n\nbogus=1\n");
    assert(n == 2 && cfg.port == 9000 && cfg.settle_ms == 0);
    assert(!strcmp(cfg.titles[0].id, "CUSA00019") && !cfg.titles[0].abi_has_ctx);
    assert(cfg.titles[1].abi_has_ctx == 1 && cfg.titles[1].check_connected_ret == 1);

    char dir[] = "/tmp/skydmnXXXXXX"; assert(mkdtemp(dir));
    struct sky_platform pl = { NULL, cur_app, open_proc, close_proc, sleep_ms, lg };
    static struct sky_daemon d;
    sky_daemon_init(&d, &cfg, &pl, dir);

    assert(sky_daemon_step(&d) == 0 && opens == 0);                 /* nothing running */
    strcpy(cur_title, "CUSA12345"); cur_pid = 100;
    assert(sky_daemon_step(&d) == 0 && opens == 0);                 /* unrelated game: untouched */
    strcpy(cur_title, "CUSA00019"); cur_pid = 200;
    assert(sky_daemon_step(&d) == 1 && opens == 1 && d.injected);   /* supported game: hooked */
    assert(sky_daemon_step(&d) == 0 && opens == 1);                 /* steady state */
    assert(d.api.link.connected(d.api.link.ctx) == 1);

    cur_pid = 0;                                                    /* game quits */
    assert(sky_daemon_step(&d) == 1 && !d.injected && closes == 1);
    cur_pid = 201;                                                  /* relaunch -> hooked again */
    assert(sky_daemon_step(&d) == 1 && d.injected && opens == 2);
    cur_pid = 202;                                                  /* crashed + restarted without a gap */
    assert(sky_daemon_step(&d) == 1 && d.injected && d.pid == 202 && opens == 3);

    /* a build missing a required import: refused once, game untouched, not retried every tick */
    cur_pid = 0; sky_daemon_step(&d);
    omit_submit = 1; strcpy(cur_title, "CUSA00019"); cur_pid = 300;
    int o = opens, c0 = closes;
    assert(sky_daemon_step(&d) == 0 && !d.injected && opens == o + 1 && closes == c0 + 1);
    for (int i = 0; i < fp.n_imports; i++) assert(*(uint64_t *)(uintptr_t)fp.imports[i].slot == (uint64_t)(uintptr_t)orig);
    assert(sky_daemon_step(&d) == 0 && opens == o + 1);              /* no retry storm */
    cur_pid = 301; omit_submit = 0;                                  /* new process: tried again */
    assert(sky_daemon_step(&d) == 1 && d.injected);
    char cmd[300]; snprintf(cmd, sizeof cmd, "rm -rf %s", dir); (void)!system(cmd);
    puts("daemon tests passed");
    return 0;
}
