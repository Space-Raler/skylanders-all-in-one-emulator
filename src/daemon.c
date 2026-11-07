#include "daemon.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int sky_config_parse(struct sky_config *c, const char *text) {
    memset(c, 0, sizeof *c);
    c->port = 8787;
    c->settle_ms = 25000;
    c->autosave = 1;
    c->idle_status_ms = 22;                  /* the portal's IN bInterval */
    const char *p = text;
    while (p && *p) {
        char line[160];
        size_t n = strcspn(p, "\n");
        snprintf(line, sizeof line, "%.*s", (int)(n < sizeof line - 1 ? n : sizeof line - 1), p);
        p += n; if (*p == '\n') p++;
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (*s == '#' || !*s || *s == '\r') continue;
        if (!strncmp(s, "port=", 5)) { c->port = atoi(s + 5); continue; }
        if (!strncmp(s, "settle_ms=", 10)) { c->settle_ms = atoi(s + 10); continue; }
        if (!strncmp(s, "stage=", 6)) { c->stage = atoi(s + 6); continue; }
        if (!strncmp(s, "idle_status=", 12)) { c->idle_status_ms = atoi(s + 12); if (c->idle_status_ms < 0) c->idle_status_ms = 0; continue; }
        if (!strncmp(s, "autosave=", 9)) { c->autosave = atoi(s + 9) != 0; continue; }
        if (!strncmp(s, "title=", 6) && c->n_titles < SKY_MAX_TITLES) {
            struct sky_title_cfg *t = &c->titles[c->n_titles];
            char id[16] = {0};
            int ctx = 0;
            unsigned ccr = 0;
            if (sscanf(s + 6, "%15s", id) != 1) continue;
            const char *k = strstr(s, "ctx=");
            if (k) ctx = atoi(k + 4);
            k = strstr(s, "check=");
            if (k) ccr = (unsigned)strtoul(k + 6, NULL, 0);
            snprintf(t->id, sizeof t->id, "%s", id);
            t->abi_has_ctx = ctx != 0;
            t->check_connected_ret = ccr;
            c->n_titles++;
        }
    }
    return c->n_titles;
}

/* ---- link between phone API and the injected hook ---- */
static int l_connected(void *c) { struct sky_daemon *d = c; return d->injected; }
static int l_load(void *c, unsigned s, const uint8_t *f) { struct sky_daemon *d = c; return d->injected ? sky_remote_load(&d->proc, &d->inj, s, f) : -1; }
static int l_remove(void *c, unsigned s) { struct sky_daemon *d = c; return d->injected ? sky_remote_remove(&d->proc, &d->inj, s) : -1; }
static int l_read(void *c, unsigned s, uint8_t *o, int *dirty) { struct sky_daemon *d = c; return d->injected ? sky_remote_read_figure(&d->proc, &d->inj, s, o, dirty) : -1; }

static void api_log(void *c, const char *m) { struct sky_daemon *d = c; if (d->plat.log) d->plat.log(d->plat.ctx, m); }

void sky_daemon_init(struct sky_daemon *d, const struct sky_config *cfg, const struct sky_platform *plat, const char *root) {
    memset(d, 0, sizeof *d);
    d->cfg = *cfg;
    d->plat = *plat;
    struct sky_api_link lk = { d, l_connected, l_load, l_remove, l_read };
    sky_api_init(&d->api, root, &lk);
    d->api.autosave = cfg->autosave;
    d->api.log = api_log;
    d->api.log_ctx = d;
}

int sky_daemon_autosave(struct sky_daemon *d) {
    return d->injected ? sky_api_autosave(&d->api) : 0;
}

static void inj_log(void *c, const char *m) { struct sky_daemon *d = c; if (d->plat.log) d->plat.log(d->plat.ctx, m); }

static void say(struct sky_daemon *d, const char *fmt, const char *a, int b) {
    char m[200];
    snprintf(m, sizeof m, fmt, a, b);
    if (d->plat.log) d->plat.log(d->plat.ctx, m);
}

/* Copy new hook trace entries and call counters from the game into the log (no-op until something happens). */
static void trace_poll(struct sky_daemon *d) {
    const struct sky_proc *p = &d->proc;
    uint32_t head = 0;
    if (!p->read || p->read(p->ctx, d->inj.shared + offsetof(struct sky_shared, trace_head), &head, 4) != 0) return;
    if (head - d->trace_seen > SKY_TRACE_N - SKY_TRACE_KEEP && head > SKY_TRACE_N) {   /* ring lapped us */
        uint32_t keep_from = head - (SKY_TRACE_N - SKY_TRACE_KEEP);
        if (d->trace_seen < SKY_TRACE_KEEP) d->trace_seen = SKY_TRACE_KEEP;               /* fixed part is never lapped */
        if (keep_from > d->trace_seen) {
            char m[80]; snprintf(m, sizeof m, "trace: %u events lost (ring lapped)", (unsigned)(keep_from - d->trace_seen));
            if (d->plat.log) d->plat.log(d->plat.ctx, m);
            d->trace_seen = keep_from;
        }
    }
    while (d->trace_seen < head) {
        uint8_t e[SKY_TRACE_LEN];
        if (p->read(p->ctx, d->inj.shared + offsetof(struct sky_shared, trace) +
                    (uint64_t)SKY_TRACE_IDX(d->trace_seen) * SKY_TRACE_LEN, e, sizeof e) != 0) return;
        char m[200]; size_t o = (size_t)snprintf(m, sizeof m, "trace[%u] %c a=%02x b=%02x v=%04x w=%04x n=%u:",
            d->trace_seen, e[0] >= 32 && e[0] < 127 ? e[0] : '?', e[6], e[7], e[2] | (e[3] << 8), e[4] | (e[5] << 8), e[1]);
        for (unsigned i = 0; i < e[1] && i < 32 && o + 4 < sizeof m; i++) o += (size_t)snprintf(m + o, sizeof m - o, " %02x", e[8 + i]);
        if (d->plat.log) d->plat.log(d->plat.ctx, m);
        d->trace_seen++;
    }
    struct sky_lights lt;
    if (p->read(p->ctx, d->inj.shared + offsetof(struct sky_shared, portal) + offsetof(struct sky_portal, lights),
                &lt, sizeof lt) == 0 && lt.seq != d->lights_seq) {
        d->lights_seq = lt.seq;
        char m[160];
        snprintf(m, sizeof m, "lights: ring=#%02x%02x%02x fade=%ums backlight=%s%u (change %u)",
                 lt.ring[0], lt.ring[1], lt.ring[2], (unsigned)lt.fade_ms,
                 lt.backlight_set ? "" : "unset/", (unsigned)lt.backlight, (unsigned)lt.seq);
        if (d->plat.log) d->plat.log(d->plat.ctx, m);
    }
    uint32_t calls[SKY_HOOK_COUNT]; uint32_t sum = 0;
    if (p->read(p->ctx, d->inj.shared + offsetof(struct sky_shared, calls), calls, sizeof calls) != 0) return;
    for (int i = 0; i < SKY_HOOK_COUNT; i++) sum += calls[i];
    if (sum == d->calls_sum) return;
    d->calls_sum = sum;
    static const char *const names[] = {
#define X(n, nid, req) #n,
        SKY_HOOK_LIST(X)
#undef X
    };
    char m[400]; size_t o = (size_t)snprintf(m, sizeof m, "calls:");
    for (int i = 0; i < SKY_HOOK_COUNT && o + 24 < sizeof m; i++)
        if (calls[i]) o += (size_t)snprintf(m + o, sizeof m - o, " %s=%u", names[i], calls[i]);
    if (d->plat.log) d->plat.log(d->plat.ctx, m);
}

void sky_daemon_poll_trace(struct sky_daemon *d) {
    if (d->injected) trace_poll(d);
}

int sky_daemon_step(struct sky_daemon *d) {
    char title[16] = {0};
    int pid = 0;
    int running = d->plat.current_app(d->plat.ctx, title, sizeof title, &pid) == 0;

    if (d->injected && (!running || pid != d->pid)) {      /* game exited or relaunched */
        say(d, "game %s (pid %d) gone; dropping hook state", d->title, d->pid);
        d->plat.close_proc(d->plat.ctx);
        d->injected = 0;
        d->pid = 0;
        sky_api_reset(&d->api);                                /* new process has an empty portal */
        if (!running) return 1;
    }
    if (!running) { d->failed_pid = 0; return 0; }
    if (d->injected) {
        uint64_t where = 0;
        int bad = sky_remote_check_state(&d->proc, &d->inj, &where);
        if (bad != d->state_bad) {
            d->state_bad = bad;
            char m[200];
            if (bad < 0) snprintf(m, sizeof m, "state at 0x%llx is unreadable now", (unsigned long long)d->inj.state);
            else if (bad) snprintf(m, sizeof m, "STATE DAMAGED by the game: mask 0x%x (1 magic, 2/4/8 canaries, 16 guard band), first at 0x%llx, state at 0x%llx",
                                   (unsigned)bad, (unsigned long long)where, (unsigned long long)d->inj.state);
            else snprintf(m, sizeof m, "state intact again");
            if (d->plat.log) d->plat.log(d->plat.ctx, m);
        }
        trace_poll(d);
        return 0;
    }
    if (pid == d->failed_pid) return 0;

    const struct sky_title_cfg *tc = NULL;
    for (int i = 0; i < d->cfg.n_titles; i++)
        if (!strcmp(d->cfg.titles[i].id, title)) { tc = &d->cfg.titles[i]; break; }
    if (!tc) return 0;

    if (d->cfg.settle_ms > 0) {
        d->plat.sleep_ms(d->plat.ctx, d->cfg.settle_ms);
        char t2[16] = {0}; int p2 = 0;                       /* game quit or relaunched while we waited? */
        if (d->plat.current_app(d->plat.ctx, t2, sizeof t2, &p2) != 0 || p2 != pid) return 0;
    }
    say(d, "attaching to %s (pid %d)", title, pid);
    if (d->plat.open_proc(d->plat.ctx, pid, &d->proc) != 0) { say(d, "cannot attach to %s (pid %d)", title, pid); d->failed_pid = pid; return 0; }
    say(d, "attached to %s; stage limit %d (0 = none)", title, d->cfg.stage);
    struct sky_inject_cfg ic = { tc->abi_has_ctx, tc->check_connected_ret, d->cfg.stage, d, inj_log, (unsigned)d->cfg.idle_status_ms };
    int rc = sky_inject(&d->proc, &ic, &d->inj);
    if (rc == SKY_INJ_STOPPED) {
        say(d, "stage limit reached for %s (stage=%d in the ini); stopping here on purpose", title, d->cfg.stage);
        d->plat.close_proc(d->plat.ctx);
        d->failed_pid = pid;
        return 0;
    }
    if (rc != SKY_INJ_OK) {
        say(d, "injection into %s refused/failed (code %d); game left untouched", title, rc);
        d->plat.close_proc(d->plat.ctx);
        d->failed_pid = pid;                               /* no retry loop on the same process */
        return 0;
    }
    snprintf(d->title, sizeof d->title, "%s", title);
    d->pid = pid;
    d->injected = 1;
    d->trace_seen = 0; d->calls_sum = 0; d->lights_seq = 0; d->state_bad = 0;    /* new process = fresh trace ring and counters */
    say(d, "hooked %s (%d slots patched)", title, (int)d->inj.patched);
    return 1;
}
