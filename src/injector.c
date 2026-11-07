#include "injector.h"
#include "hook_blob.h"
#include "hook_state.h"
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <unistd.h>

static const char *const NIDS[SKY_HOOK_COUNT] = {
#define X(n, nid, req) nid,
    SKY_HOOK_LIST(X)
#undef X
};
static const uint32_t REQUIRED[SKY_HOOK_COUNT] = {
#define X(n, nid, req) req,
    SKY_HOOK_LIST(X)
#undef X
};
static const uint32_t HOOK_OFF[SKY_HOOK_COUNT] = {
#define X(n, nid, req) SKY_BLOB_OFF_##n,
    SKY_HOOK_LIST(X)
#undef X
};

#include <stdio.h>
static void ilog(const struct sky_inject_cfg *cfg, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void ilog(const struct sky_inject_cfg *cfg, const char *fmt, ...) {
    if (!cfg || !cfg->log) return;
    char m[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(m, sizeof m, fmt, ap);
    va_end(ap);
    cfg->log(cfg->log_ctx, m);
}
#define STAGE_DONE(n) (cfg && cfg->stage > 0 && cfg->stage <= (n))

#define SH(inj, field) ((inj)->shared + offsetof(struct sky_shared, field))

int sky_inject(const struct sky_proc *p, const struct sky_inject_cfg *cfg, struct sky_injection *out) {
    memset(out, 0, sizeof(*out));

    /* 1. Resolve every import slot first. Nothing is touched if the game is unsuitable. */
    uint64_t slot[SKY_HOOK_COUNT] = {0};
    for (int i = 0; i < SKY_HOOK_COUNT; i++) {
        uint64_t va = 0;
        if (p->find_import_slot(p->ctx, NIDS[i], &va) != 0) va = 0;
        slot[i] = va;
        if (REQUIRED[i] && !va) return SKY_INJ_ERR_MISSING_REQUIRED;
    }
    if (!slot[SKY_H_HandleEvents] && !slot[SKY_H_HandleEventsTimeout]) return SKY_INJ_ERR_NO_EVENT_PUMP;
    { int found = 0; for (int i = 0; i < SKY_HOOK_COUNT; i++) found += slot[i] != 0;
      ilog(cfg, "step: resolved %d/%d import slots", found, (int)SKY_HOOK_COUNT); }

    /* 2. Allocate the two regions: executable code image, and writable state. */
    size_t code_sz  = (SKY_BLOB_FILE_SIZE + 15u) & ~(size_t)15u;
    size_t state_sz = (sizeof(struct sky_hook_state) + 15u) & ~(size_t)15u;
    uint64_t code = 0, state = 0;
    if (p->alloc_code(p->ctx, code_sz, &code) != 0 || !code || (code & 15)) return SKY_INJ_ERR_ALLOC;
    if (p->alloc_data(p->ctx, state_sz, &state) != 0 || !state || (state & 15)) return SKY_INJ_ERR_ALLOC_DATA;
    ilog(cfg, "step: regions ok (code 0x%llx, state 0x%llx)", (unsigned long long)code, (unsigned long long)state);
    if (STAGE_DONE(1)) return SKY_INJ_STOPPED;

    struct sky_hook_state *st = calloc(1, state_sz);
    uint8_t *img = calloc(1, code_sz);
    if (!st || !img) { free(st); free(img); return SKY_INJ_ERR_ALLOC; }
    memcpy(img, sky_blob_bytes, SKY_BLOB_FILE_SIZE);
    memcpy(img + SKY_BLOB_STATEPTR_OFF, &state, sizeof state);     /* code -> state pointer */

    struct sky_shared *sh = &st->shared;
    st->canary_a = SKY_CANARY_A; st->canary_b = SKY_CANARY_B; st->canary_c = SKY_CANARY_C;
    memset(st->guard, SKY_GUARD_FILL, sizeof st->guard);
    sh->magic = SKY_SHARED_MAGIC;
    sh->version = SKY_SHARED_VERSION;
    sh->enabled = 0;                                  /* flipped on after patching */
    sh->abi_has_ctx = cfg && cfg->abi_has_ctx ? 1u : 0u;
    sh->check_connected_ret = cfg ? cfg->check_connected_ret : 0;
    sh->idle_status_ms = cfg ? cfg->idle_status_ms : 0;
    uint64_t us = 0;
    if (p->resolve_symbol && p->resolve_symbol(p->ctx, "usleep", &us) == 0) sh->usleep_fn = us;
    sky_portal_init(&sh->portal);

    /* 3. Record the original slot contents (needed for pass-through) before patching. */
    for (int i = 0; i < SKY_HOOK_COUNT; i++) {
        if (!slot[i]) continue;
        uint64_t orig = 0;
        if (p->read(p->ctx, slot[i], &orig, sizeof orig) != 0) { free(st); free(img); return SKY_INJ_ERR_SLOT; }
        sh->orig[i] = orig;
    }

    /* State first, then code: nothing can reach the code until a slot is patched below. */
    ilog(cfg, "step: writing state (%zu bytes)", state_sz);
    int rc = p->write(p->ctx, state, st, state_sz);
    if (rc == 0) { ilog(cfg, "step: writing code (%zu bytes)", code_sz); rc = p->write(p->ctx, code, img, code_sz); }
    free(st);
    free(img);
    if (rc != 0) return SKY_INJ_ERR_WRITE;
    ilog(cfg, "step: state and code written");

    out->base = code;
    out->state = state;
    out->shared = state;
    if (STAGE_DONE(2)) return SKY_INJ_STOPPED;

    /* 4. Patch while the game is stopped, then enable. */
    ilog(cfg, "step: suspending game");
    if (p->suspend) p->suspend(p->ctx);
    ilog(cfg, "step: game suspended, patching slots");
    int err = 0;
    for (int i = 0; i < SKY_HOOK_COUNT && !err; i++) {
        if (!slot[i]) continue;
        uint64_t target = code + HOOK_OFF[i];
        if (p->write(p->ctx, slot[i], &target, sizeof target) != 0) { err = SKY_INJ_ERR_SLOT; break; }
        out->slot[i] = slot[i];
        out->patched++;
    }
    if (!err) ilog(cfg, "step: patched %u slots", out->patched);
    if (!err && !STAGE_DONE(3)) {
        uint32_t one = 1;
        if (p->write(p->ctx, SH(out, enabled), &one, sizeof one) != 0) err = SKY_INJ_ERR_WRITE;
        else ilog(cfg, "step: hooks enabled");
    }
    if (err) {                                        /* roll back whatever was patched */
        for (int i = 0; i < SKY_HOOK_COUNT; i++) {
            if (!out->slot[i]) continue;
            uint64_t orig = 0;
            if (p->read(p->ctx, SH(out, orig) + (uint64_t)i * 8, &orig, sizeof orig) == 0)
                p->write(p->ctx, out->slot[i], &orig, sizeof orig);
        }
    }
    ilog(cfg, "step: resuming game");
    if (p->resume) p->resume(p->ctx);
    ilog(cfg, "step: game resumed");
    if (!err && STAGE_DONE(3)) return SKY_INJ_STOPPED;
    return err;
}

int sky_unhook(const struct sky_proc *p, const struct sky_injection *inj) {
    int rc = 0;
    uint32_t zero = 0;
    if (p->suspend) p->suspend(p->ctx);
    p->write(p->ctx, SH(inj, enabled), &zero, sizeof zero);   /* in-flight calls fall through */
    for (int i = 0; i < SKY_HOOK_COUNT; i++) {
        if (!inj->slot[i]) continue;
        uint64_t orig = 0;
        if (p->read(p->ctx, SH(inj, orig) + (uint64_t)i * 8, &orig, sizeof orig) != 0 ||
            p->write(p->ctx, inj->slot[i], &orig, sizeof orig) != 0) rc = SKY_INJ_ERR_SLOT;
    }
    if (p->resume) p->resume(p->ctx);
    return rc;
}

#define WAIT_MS 2000

static int wait_consumed(const struct sky_proc *p, const struct sky_injection *inj, unsigned slot) {
    for (int waited = 0; waited < WAIT_MS; waited += 5) {
        uint32_t r = 1;
        if (p->read(p->ctx, SH(inj, req) + 4u * slot, &r, sizeof r) != 0) return -1;
        if (r == 0) return 0;
        usleep(5000);
    }
    return -2;       /* game never pumped events (not running, paused, or portal not in use yet) */
}

int sky_remote_load(const struct sky_proc *p, const struct sky_injection *inj, unsigned slot, const uint8_t *fig) {
    if (slot >= SKY_SLOTS || !fig) return -1;
    /* one staging buffer, so serialise: wait for any earlier request on any slot */
    for (unsigned s = 0; s < SKY_SLOTS; s++) if (wait_consumed(p, inj, s) != 0) return -3;
    if (p->write(p->ctx, SH(inj, stage), fig, SKY_FIG_SIZE) != 0) return -1;
    uint32_t r = SKY_REQ_LOAD;
    if (p->write(p->ctx, SH(inj, req) + 4u * slot, &r, sizeof r) != 0) return -1;
    return wait_consumed(p, inj, slot);
}

int sky_remote_remove(const struct sky_proc *p, const struct sky_injection *inj, unsigned slot) {
    if (slot >= SKY_SLOTS) return -1;
    uint32_t r = SKY_REQ_REMOVE;
    if (p->write(p->ctx, SH(inj, req) + 4u * slot, &r, sizeof r) != 0) return -1;
    return wait_consumed(p, inj, slot);
}

int sky_remote_read_figure(const struct sky_proc *p, const struct sky_injection *inj, unsigned slot,
                           uint8_t *out, int *dirty) {
    if (slot >= SKY_SLOTS || !out) return -1;
    uint64_t fa = inj->shared + offsetof(struct sky_shared, portal) +
                  offsetof(struct sky_portal, fig) + (uint64_t)slot * sizeof(struct sky_figure);
    for (int attempt = 0; attempt < 50; attempt++) {
        uint32_t s1 = 0, s2 = 0; uint8_t d = 0;
        if (p->read(p->ctx, fa + offsetof(struct sky_figure, seq), &s1, 4) != 0) return -1;
        if (p->read(p->ctx, fa + offsetof(struct sky_figure, data), out, SKY_FIG_SIZE) != 0) return -1;
        if (p->read(p->ctx, fa + offsetof(struct sky_figure, dirty), &d, 1) != 0) return -1;
        if (p->read(p->ctx, fa + offsetof(struct sky_figure, seq), &s2, 4) != 0) return -1;
        if (s1 == s2 && !(s1 & 1)) {
            if (dirty) *dirty = d;
            return 0;
        }
        usleep(1000);
    }
    return -2;
}

int sky_remote_check_state(const struct sky_proc *p, const struct sky_injection *inj, uint64_t *where) {
    uint32_t magic = 0, ca = 0, cb = 0, cc = 0;
    uint8_t guard[SKY_GUARD_BYTES];
    const uint64_t b = inj->state;
    if (where) *where = 0;
    if (p->read(p->ctx, b + offsetof(struct sky_hook_state, shared) + offsetof(struct sky_shared, magic), &magic, 4) != 0 ||
        p->read(p->ctx, b + offsetof(struct sky_hook_state, canary_a), &ca, 4) != 0 ||
        p->read(p->ctx, b + offsetof(struct sky_hook_state, canary_b), &cb, 4) != 0 ||
        p->read(p->ctx, b + offsetof(struct sky_hook_state, canary_c), &cc, 4) != 0 ||
        p->read(p->ctx, b + offsetof(struct sky_hook_state, guard), guard, sizeof guard) != 0) return -1;
    int bad = 0;
    uint64_t first = 0;
#define NOTE(bit, addr) do { bad |= (bit); if (!first) first = (addr); } while (0)
    if (magic != SKY_SHARED_MAGIC) NOTE(1, b);
    if (ca != SKY_CANARY_A) NOTE(2, b + offsetof(struct sky_hook_state, canary_a));
    if (cb != SKY_CANARY_B) NOTE(4, b + offsetof(struct sky_hook_state, canary_b));
    if (cc != SKY_CANARY_C) NOTE(8, b + offsetof(struct sky_hook_state, canary_c));
    for (size_t i = 0; i < sizeof guard; i++)
        if (guard[i] != SKY_GUARD_FILL) { NOTE(16, b + offsetof(struct sky_hook_state, guard) + i); break; }
#undef NOTE
    if (where) *where = first;
    return bad;
}

int sky_remote_get_stats(const struct sky_proc *p, const struct sky_injection *inj, struct sky_shared *out) {
    return p->read(p->ctx, inj->shared, out, sizeof *out);
}
