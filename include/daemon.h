/* Title watcher + glue between the phone API and the injected hook. Platform-independent:
 * everything OS/SDK specific arrives through struct sky_platform. */
#ifndef SKY_DAEMON_H
#define SKY_DAEMON_H
#include "api.h"
#include "injector.h"

#define SKY_MAX_TITLES 8

struct sky_title_cfg { char id[16]; int abi_has_ctx; uint32_t check_connected_ret; };
struct sky_config {
    struct sky_title_cfg titles[SKY_MAX_TITLES];
    int n_titles;
    int port;
    int settle_ms;                 /* wait after the title appears before patching (imports must be bound) */
    int stage;                     /* diagnostic stage limit 0 (full) or 1..4, see injector.h */
    int idle_status_ms;            /* answer idle IN polls with a status report this often (default 22, 0 = off) */
    int autosave;                  /* 1 (default): write game progress back to the figure files; 0: only the Save button */
};

struct sky_platform {
    void *ctx;
    /* foreground app: fills title (e.g. "CUSA00019") and pid; returns 0 if a game is running */
    int  (*current_app)(void *ctx, char *title, size_t cap, int *pid);
    /* attach to the process. The returned iface stays valid until close_proc. */
    int  (*open_proc)(void *ctx, int pid, struct sky_proc *out);
    void (*close_proc)(void *ctx);
    void (*sleep_ms)(void *ctx, int ms);
    void (*log)(void *ctx, const char *msg);
};

struct sky_daemon {
    struct sky_config cfg;
    struct sky_platform plat;
    struct sky_api api;
    struct sky_proc proc;
    struct sky_injection inj;
    int injected;
    int pid;
    int failed_pid;                /* process we refused/failed to hook: do not retry every tick */
    char title[16];
    uint32_t trace_seen;           /* trace entries already logged */
    uint32_t calls_sum;            /* last logged sum of hook call counters */
    uint32_t lights_seq;           /* last logged light-change counter */
    int      state_bad;            /* last logged state-damage mask (so each change is logged once) */
};

/* "title=CUSA00019 ctx=0" lines, "port=8787", "settle_ms=3000", '#' comments. Returns number of titles. */
int  sky_config_parse(struct sky_config *c, const char *text);
void sky_daemon_init(struct sky_daemon *d, const struct sky_config *cfg, const struct sky_platform *plat, const char *data_root);
/* Progress autosave tick; cheap when nothing is hooked. Call a few times per second. Returns files written. */
int  sky_daemon_autosave(struct sky_daemon *d);
/* Copy new hook trace entries / light changes / call counters into the log. Cheap when nothing changed; call it often
 * (every ~50 ms) so the last events before a game crash are not lost with the process. */
void sky_daemon_poll_trace(struct sky_daemon *d);
/* One iteration of the watch loop. Returns 1 if a hook was installed or removed this step. */
int  sky_daemon_step(struct sky_daemon *d);
#endif
