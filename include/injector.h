#ifndef SKY_INJECTOR_H
#define SKY_INJECTOR_H
#include "proc_iface.h"
#include "sky_hooks.h"

/* Diagnostic stage limit (0 = no limit, do everything). Used to find which step kills a console:
 *   1 resolve import slots + carve/check the two regions       (reads only, game untouched)
 *   2 ... + write hook state and code into the game            (writes, nothing references them yet)
 *   3 ... + suspend, patch the PLT slots with the hooks DISABLED (they tail-call the originals), resume
 *   4 ... + enable the hooks                                    (same as 0) */
struct sky_inject_cfg {
    int abi_has_ctx;              /* see sky_shared.abi_has_ctx */
    uint32_t check_connected_ret;
    int stage;                    /* 0 or 1..4, see above */
    void *log_ctx;
    void (*log)(void *ctx, const char *msg);   /* optional step log */
    unsigned idle_status_ms;      /* >0: complete a pending IN poll with a status report this often when idle (0 = off) */
};

struct sky_injection {
    uint64_t base;                /* hook code image address in the game (executable region) */
    uint64_t state;               /* struct sky_hook_state address in the game (writable region) */
    uint64_t shared;              /* == state: struct sky_shared is its first member */
    uint64_t slot[SKY_HOOK_COUNT];/* patched slot VAs (0 = not imported / not patched) */
    unsigned patched;
};

enum {
    SKY_INJ_OK = 0,
    SKY_INJ_ERR_ALLOC = -1,
    SKY_INJ_ERR_WRITE = -2,
    SKY_INJ_ERR_MISSING_REQUIRED = -3,   /* game lacks a required import: nothing was patched */
    SKY_INJ_ERR_NO_EVENT_PUMP = -4,
    SKY_INJ_ERR_SLOT = -5,
    SKY_INJ_ERR_ALLOC_DATA = -6,
    SKY_INJ_STOPPED = 1,                 /* stage limit reached on purpose; not an error */
};

/* Allocate+write the blob, record originals, then patch all slots (suspend/resume around it). */
int sky_inject(const struct sky_proc *p, const struct sky_inject_cfg *cfg, struct sky_injection *out);
/* Restore every patched slot to its original value and disable the hooks. */
int sky_unhook(const struct sky_proc *p, const struct sky_injection *inj);

/* Daemon -> game figure control. Return 0 on success, <0 on failure/timeout. */
int sky_remote_load(const struct sky_proc *p, const struct sky_injection *inj, unsigned slot, const uint8_t *fig1024);
int sky_remote_remove(const struct sky_proc *p, const struct sky_injection *inj, unsigned slot);
/* Consistent snapshot of a figure the game may have written to. *dirty is set if it had. */
int sky_remote_read_figure(const struct sky_proc *p, const struct sky_injection *inj, unsigned slot,
                           uint8_t *out1024, int *dirty);
/* Check that the game has not scribbled over our state: magic, canaries and the guard band. Returns 0 if intact, a bit mask
 * of damaged parts otherwise (1 magic, 2/4/8 canaries a/b/c, 16 guard band), or -1 if the state cannot be read at all.
 * *where gets the game address of the first damaged part. */
int sky_remote_check_state(const struct sky_proc *p, const struct sky_injection *inj, uint64_t *where);
int sky_remote_get_stats(const struct sky_proc *p, const struct sky_injection *inj, struct sky_shared *out);
#endif
