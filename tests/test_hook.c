/* End-to-end host test: inject the real freestanding blob into a simulated game
 * (RWX mmap, fake PLT), then drive it exactly like a libusb-style game would:
 * enumerate, claim, async interrupt transfers, HandleEvents* pump. A second thread
 * plays the daemon (load/remove/read figures through the shared block). */
#include "api.h"
#include "injector.h"
#include "hook_state.h"
#include "proc_fake.h"
#include "sceusbd_abi.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#define N_SWAP 20
#define N_TRAP 21
static const char *const SWAP[N_SWAP] = {
    "TOhg7P6kTH4","L0EHgZZNVas","EkqGLxWC-S0","RRKFcKQ1Ka4","BKMEGvfCPyU","Fq6+0Fm55xU","bhomgbiQgeo",
    "Dkm5qe8j3XE","RLf56F-WjKQ","Y5go+ha6eDs","AE+mHBHneyk","0ktE1PhzGFU","t3J5pXxhJlI","Hvd3S--n25w",
    "8qB9Ar4P5nc","t7WE9mb1TB8","GjlCrU4GcIY","VJ6oMq-Di2U","EQ6SCLMqzkM","MlW6deWfPp0" };
static const char *const TRAP[N_TRAP] = {
    "TOhg7P6kTH4","Fq6+0Fm55xU","MlW6deWfPp0","RLf56F-WjKQ","BKMEGvfCPyU","-sgi7EeLSO8","8qB9Ar4P5nc",
    "bhomgbiQgeo","t7WE9mb1TB8","GjlCrU4GcIY","VJ6oMq-Di2U","EQ6SCLMqzkM","L0EHgZZNVas","+wU6CGuZcWk",
    "Dkm5qe8j3XE","Y5go+ha6eDs","AE+mHBHneyk","0ktE1PhzGFU","t3J5pXxhJlI","Hvd3S--n25w","RRKFcKQ1Ka4" };

#define NID_INIT "TOhg7P6kTH4"
#define NID_GDL "8qB9Ar4P5nc"
#define NID_GDD "bhomgbiQgeo"
#define NID_GCD "Dkm5qe8j3XE"
#define NID_OPEN "VJ6oMq-Di2U"
#define NID_KDA "RLf56F-WjKQ"
#define NID_DET "Y5go+ha6eDs"
#define NID_CLAIM "AE+mHBHneyk"
#define NID_ALLOC "0ktE1PhzGFU"
#define NID_FREE "-sgi7EeLSO8"
#define NID_FILL "t3J5pXxhJlI"
#define NID_SUBMIT "L0EHgZZNVas"
#define NID_HE "EkqGLxWC-S0"
#define NID_HET "+wU6CGuZcWk"
#define NID_CTRL "RRKFcKQ1Ka4"

static struct sky_fake_proc fp;
static struct sky_proc proc;
static struct sky_injection inj;
static int has_ctx, use_timeout;
static int orig_calls;

static uintptr_t orig_any(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e, uintptr_t f, uintptr_t g, uintptr_t h) {
    (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g; (void)h;
    orig_calls++;
    return 777;
}

typedef uintptr_t (*fn8)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
static uintptr_t gcall(const char *nid, uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3,
                       uintptr_t a4, uintptr_t a5, uintptr_t a6, uintptr_t a7) {
    fn8 f = (fn8)(uintptr_t)*sky_fake_slot(&fp, nid);
    return f(a0, a1, a2, a3, a4, a5, a6, a7);
}
#define G(nid, ...) gcall(nid, __VA_ARGS__)

static void pump(void) {
    static char ctx[8];
    struct { int64_t s, u; } tv = { 0, 2000 };
    if (use_timeout) {
        if (has_ctx) G(NID_HET, (uintptr_t)ctx, (uintptr_t)&tv, 0, 0, 0, 0, 0, 0);
        else G(NID_HET, (uintptr_t)&tv, 0, 0, 0, 0, 0, 0, 0);
    } else {
        G(NID_HE, has_ctx ? (uintptr_t)ctx : 0, 0, 0, 0, 0, 0, 0, 0);
    }
}

static uint8_t last_in[32];
static int in_count, out_count;
static void cb_in(struct sky_usb_transfer *t) {
    assert(t->status == 0 && t->actual_length == 32);
    memcpy(last_in, t->buffer, 32);
    in_count++;
    assert(G(NID_SUBMIT, (uintptr_t)t, 0, 0, 0, 0, 0, 0, 0) == 0);   /* re-arm like a real game */
}
static void cb_out(struct sky_usb_transfer *t) { (void)t; out_count++; }

static void send_out(struct sky_usb_transfer *t, const uint8_t *cmd, size_t n, uint8_t *buf) {
    memset(buf, 0, 32); memcpy(buf, cmd, n);
    G(NID_FILL, (uintptr_t)t, 0xDEAD, 0x02, (uintptr_t)buf, 32, (uintptr_t)cb_out, 0, 0);
    assert(G(NID_SUBMIT, (uintptr_t)t, 0, 0, 0, 0, 0, 0, 0) == 0);
}

struct dthread { unsigned slot; uint8_t fig[SKY_FIG_SIZE]; int rc; int done; int remove; };
static int rd(void *c, unsigned s, uint8_t *o, int *dr) { (void)c; return sky_remote_read_figure(&proc, &inj, s, o, dr); }
static int cn(void *c) { (void)c; return 1; }

static void *daemon_thread(void *arg) {
    struct dthread *d = arg;
    d->rc = d->remove ? sky_remote_remove(&proc, &inj, d->slot)
                      : sky_remote_load(&proc, &inj, d->slot, d->fig);
    d->done = 1;
    return NULL;
}
static void run_daemon(struct dthread *d) {
    pthread_t th; d->done = 0;
    pthread_create(&th, NULL, daemon_thread, d);
    for (int i = 0; i < 4000 && !d->done; i++) pump();           /* game keeps pumping meanwhile */
    pthread_join(th, NULL);
    assert(d->rc == 0);
    pump(); pump();
}

static void session(const char *const *nids, int n, int ctx_mode, int timeout_mode) {
    has_ctx = ctx_mode; use_timeout = timeout_mode;
    sky_fake_init(&fp);
    for (int i = 0; i < n; i++) sky_fake_add_import(&fp, nids[i], (void *)orig_any);
    proc = sky_fake_iface(&fp);
    struct sky_inject_cfg cfg = { ctx_mode, 0, 0, NULL, NULL, 0 };
    assert(sky_inject(&proc, &cfg, &inj) == SKY_INJ_OK);
    assert(inj.patched == (unsigned)n);
    for (int i = 0; i < n; i++) assert(*sky_fake_slot(&fp, nids[i]) != (uint64_t)(uintptr_t)orig_any);

    /* --- enumerate --- */
    static char ctx[8]; void *ctxp = ctx;
    assert((int)G(NID_INIT, ctx_mode ? (uintptr_t)&ctxp : 0, 0, 0, 0, 0, 0, 0, 0) == 0);
    void **list = NULL;
    intptr_t cnt = (intptr_t)(ctx_mode ? G(NID_GDL, (uintptr_t)ctxp, (uintptr_t)&list, 0, 0, 0, 0, 0, 0)
                                       : G(NID_GDL, (uintptr_t)&list, 0, 0, 0, 0, 0, 0, 0));
    assert(cnt == 1 && list && list[0] && !list[1]);
    struct sky_usb_device_descriptor dd;
    assert((int)G(NID_GDD, (uintptr_t)list[0], (uintptr_t)&dd, 0, 0, 0, 0, 0, 0) == 0);
    assert(dd.idVendor == 0x1430 && dd.idProduct == 0x0150 && dd.bNumConfigurations == 1);
    struct sky_usb_config_descriptor *cfgd = NULL;
    assert((int)G(NID_GCD, (uintptr_t)list[0], 0, (uintptr_t)&cfgd, 0, 0, 0, 0, 0) == 0);
    assert(cfgd->bNumInterfaces == 1 && cfgd->interface[0].num_altsetting == 1);
    const struct sky_usb_interface_descriptor *id = &cfgd->interface[0].altsetting[0];
    assert(id->bNumEndpoints == 2 && id->bInterfaceClass == 3 && id->extra_length == 9 && id->extra[1] == 0x21);
    assert(id->endpoint[0].bEndpointAddress == 0x81 && id->endpoint[1].bEndpointAddress == 0x02);
    assert(id->endpoint[0].wMaxPacketSize == 32);
    void *handle = NULL;
    assert((int)G(NID_OPEN, (uintptr_t)list[0], (uintptr_t)&handle, 0, 0, 0, 0, 0, 0) == 0 && handle);
    assert((int)G(NID_KDA, (uintptr_t)handle, 0, 0, 0, 0, 0, 0, 0) == 0);
    assert((int)G(NID_DET, (uintptr_t)handle, 0, 0, 0, 0, 0, 0, 0) == 0);
    assert((int)G(NID_CLAIM, (uintptr_t)handle, 0, 0, 0, 0, 0, 0, 0) == 0);

    /* synchronous control: device + report descriptors */
    uint8_t cb[64];
    assert((int)G(NID_CTRL, (uintptr_t)handle, 0x81, 6, 0x2200, 0, (uintptr_t)cb, 64, 0) == 29 && cb[0] == 0x06);

    /* descriptors pinned to the values embedded in SkyPortal Helper 0.4.0 */
    {
        static const uint8_t want_dev[18] = { 0x12,1,0x10,1,0,0,0,0x40,0x30,0x14,0x50,1,0,1,1,2,0,1 };
        uint8_t dd[64];
        assert((int)G(NID_CTRL, (uintptr_t)handle, 0x80, 6, 0x0100, 0, (uintptr_t)dd, 64, 0) == 18 && !memcmp(dd, want_dev, 18));
        assert((int)G(NID_CTRL, (uintptr_t)handle, 0x80, 6, 0x0200, 0, (uintptr_t)dd, 64, 0) == 41 &&
               dd[2] == 0x29 && dd[33] == 0x16 && dd[40] == 0x01);                 /* IN bInterval 0x16, OUT 1 */
        static const uint8_t want_prod[24] = { 0x18,3,'S',0,'p',0,'y',0,'r',0,'o',0,' ',0,'P',0,'o',0,'r',0,'t',0,'a',0 };
        assert((int)G(NID_CTRL, (uintptr_t)handle, 0x80, 6, 0x0302, 0x0409, (uintptr_t)dd, 64, 0) == 24 && !memcmp(dd, want_prod, 24));
    }

    /* --- async transfers --- */
    struct sky_usb_transfer *tin = (void *)G(NID_ALLOC, 0, 0, 0, 0, 0, 0, 0, 0);
    struct sky_usb_transfer *tout = (void *)G(NID_ALLOC, 0, 0, 0, 0, 0, 0, 0, 0);
    assert(tin && tout && tin != tout);
    uint8_t inbuf[32], outbuf[32];
    in_count = out_count = 0;
    G(NID_FILL, (uintptr_t)tin, (uintptr_t)handle, 0x81, (uintptr_t)inbuf, 32, (uintptr_t)cb_in, 0, 0);
    assert(tin->endpoint == 0x81 && tin->type == LIBUSB_TRANSFER_TYPE_INTERRUPT && tin->length == 32 && tin->buffer == inbuf);
    assert(G(NID_SUBMIT, (uintptr_t)tin, 0, 0, 0, 0, 0, 0, 0) == 0);
    pump(); pump();
    assert(in_count == 0);                                       /* nothing to say yet */

    send_out(tout, (const uint8_t *)"R", 1, outbuf); pump();
    assert(out_count == 1 && in_count == 1 && last_in[0] == 'R' && last_in[1] == 0x02 && last_in[2] == 0x1b);

    /* SET_REPORT through the synchronous control path also reaches the portal */
    uint8_t act[32] = { 'A', 1 };
    assert((int)G(NID_CTRL, (uintptr_t)handle, 0x21, 9, 0x0200, 0, (uintptr_t)act, 32, 0) == 32);
    pump();
    assert(in_count == 2 && !memcmp(last_in, "A\x01\xff\x77", 4));

    /* --- daemon loads a figure while the game pumps --- */
    struct dthread d; memset(&d, 0, sizeof d);
    d.slot = 1;
    for (int i = 0; i < SKY_FIG_SIZE; i++) d.fig[i] = (uint8_t)(i * 13 + 5);
    int before = in_count;
    run_daemon(&d);
    assert(in_count == before + 1 && last_in[0] == 'S' && last_in[1] == (3u << 2));   /* spontaneous "figure added" */

    uint8_t q[3] = { 'Q', 0x11, 5 };
    send_out(tout, q, 3, outbuf); pump();
    assert(last_in[0] == 'Q' && last_in[1] == 0x11 && last_in[2] == 5 && !memcmp(&last_in[3], d.fig + 5 * 16, 16));

    /* game writes a block; daemon sees it, seqlock-consistent */
    uint8_t w[19] = { 'W', 0x11, 9 };
    for (int i = 0; i < 16; i++) w[3 + i] = (uint8_t)(0xC0 + i);
    send_out(tout, w, 19, outbuf); pump();
    assert(last_in[0] == 'W' && last_in[1] == 0x11);
    uint8_t back[SKY_FIG_SIZE]; int dirty = 0;
    assert(sky_remote_read_figure(&proc, &inj, 1, back, &dirty) == 0 && dirty == 1);
    assert(!memcmp(back + 9 * 16, w + 3, 16) && !memcmp(back, d.fig, 9 * 16));

    /* autosave end-to-end: the block the game just wrote reaches the figure file through the real hook */
    {
        char dir[] = "/tmp/skyhookXXXXXX"; assert(mkdtemp(dir));
        struct sky_api_link lk = { NULL, cn, NULL, NULL, rd };
        static struct sky_api api; sky_api_init(&api, dir, &lk);
        char fp_[200]; snprintf(fp_, sizeof fp_, "%s/figures/e2e.bin", dir);
        FILE *ff = fopen(fp_, "wb"); assert(ff && fwrite(d.fig, 1, SKY_FIG_SIZE, ff) == SKY_FIG_SIZE); fclose(ff);
        snprintf(api.slot_name[1], sizeof api.slot_name[1], "e2e.bin");
        memcpy(api.track[1].shadow, d.fig, SKY_FIG_SIZE); api.track[1].have = 1;
        assert(sky_api_autosave(&api) == 0 && sky_api_autosave(&api) == 1);
        uint8_t disk[SKY_FIG_SIZE];
        ff = fopen(fp_, "rb"); assert(ff && fread(disk, 1, SKY_FIG_SIZE, ff) == SKY_FIG_SIZE); fclose(ff);
        assert(!memcmp(disk + 9 * 16, w + 3, 16) && !memcmp(disk, d.fig, 9 * 16) && !memcmp(disk + 10 * 16, d.fig + 10 * 16, SKY_FIG_SIZE - 10 * 16));
        char cmd[300]; snprintf(cmd, sizeof cmd, "rm -rf %s", dir); (void)!system(cmd);
    }

    /* remove */
    struct dthread r; memset(&r, 0, sizeof r); r.slot = 1; r.remove = 1;
    run_daemon(&r);
    assert(last_in[0] == 'S' && last_in[1] == (2u << 2));

    /* stats + cleanup */
    struct sky_shared st;
    assert(sky_remote_get_stats(&proc, &inj, &st) == 0);
    assert(st.magic == SKY_SHARED_MAGIC && st.transfers_done > 5 && st.cmds_seen >= 4);
    if (sky_fake_slot(&fp, NID_FREE)) {
        assert(G(NID_FREE, (uintptr_t)tin, 0, 0, 0, 0, 0, 0, 0) == 0);
        assert(G(NID_FREE, (uintptr_t)tout, 0, 0, 0, 0, 0, 0, 0) == 0);
    }

    /* --- unhook: slots restored, calls pass through to the game's originals --- */
    assert(sky_unhook(&proc, &inj) == 0);
    for (int i = 0; i < n; i++) assert(*sky_fake_slot(&fp, nids[i]) == (uint64_t)(uintptr_t)orig_any);
    int oc = orig_calls;
    assert(G(NID_OPEN, 0, 0, 0, 0, 0, 0, 0, 0) == 777 && orig_calls == oc + 1);
}

static void test_refusals(void) {
    /* game that does not import a required function: nothing may be patched */
    sky_fake_init(&fp);
    for (int i = 0; i < N_SWAP; i++)
        if (strcmp(SWAP[i], NID_SUBMIT)) sky_fake_add_import(&fp, SWAP[i], (void *)orig_any);
    proc = sky_fake_iface(&fp);
    struct sky_inject_cfg cfg = { 0, 0, 0, NULL, NULL, 0 };
    assert(sky_inject(&proc, &cfg, &inj) == SKY_INJ_ERR_MISSING_REQUIRED);
    for (int i = 0; i < fp.n_imports; i++) assert(*(uint64_t *)(uintptr_t)fp.imports[i].slot == (uint64_t)(uintptr_t)orig_any);

    /* no event pump import at all */
    sky_fake_init(&fp);
    for (int i = 0; i < N_SWAP; i++)
        if (strcmp(SWAP[i], NID_HE)) sky_fake_add_import(&fp, SWAP[i], (void *)orig_any);
    proc = sky_fake_iface(&fp);
    assert(sky_inject(&proc, &cfg, &inj) == SKY_INJ_ERR_NO_EVENT_PUMP);
}

/* Diagnostic stages: 1 touches nothing, 2 writes only (no slot patched), 3 patches with hooks disabled, 4 = full. */
static void test_stages(void) {
    for (int stage = 1; stage <= 4; stage++) {
        struct sky_fake_proc fp; struct sky_proc proc; struct sky_injection inj;
        sky_fake_init(&fp);
        for (int i = 0; i < N_SWAP; i++) sky_fake_add_import(&fp, SWAP[i], (void *)orig_any);
        proc = sky_fake_iface(&fp);
        struct sky_inject_cfg cfg = { 0, 0, stage, NULL, NULL, 0 };
        int rc = sky_inject(&proc, &cfg, &inj);
        assert(rc == (stage == 4 ? SKY_INJ_OK : SKY_INJ_STOPPED));
        unsigned changed = 0;
        for (int i = 0; i < fp.n_imports; i++)
            changed += *(uint64_t *)(uintptr_t)fp.imports[i].slot != (uint64_t)(uintptr_t)orig_any;
        if (stage <= 2) assert(changed == 0);                 /* no PLT slot touched before stage 3 */
        else assert(changed == (unsigned)N_SWAP);              /* stage 3 and 4 patched every import */
        if (stage == 3) {                                      /* patched but disabled: hooks must pass through */
            uint32_t enabled = 1;
            assert(proc.read(proc.ctx, inj.shared + offsetof(struct sky_shared, enabled), &enabled, 4) == 0);
            assert(enabled == 0);
        }
    }
    printf("  stage limits 1..4 behave\n");
}

/* Idle status: with idle_status_ms set, a pending IN poll is answered with a status report now and then, but only after
 * the game has sent a command, and not on every pump. */
static void test_idle_status(void) {
    has_ctx = 0; use_timeout = 0;
    sky_fake_init(&fp);
    for (int i = 0; i < N_SWAP; i++) sky_fake_add_import(&fp, SWAP[i], (void *)orig_any);
    proc = sky_fake_iface(&fp);
    struct sky_inject_cfg cfg = { 0, 0, 0, NULL, NULL, 5 };
    assert(sky_inject(&proc, &cfg, &inj) == SKY_INJ_OK);
    void **list = NULL; void *handle = NULL;
    assert((int)G(NID_INIT, 0, 0, 0, 0, 0, 0, 0, 0) == 0);
    assert((int)G(NID_GDL, (uintptr_t)&list, 0, 0, 0, 0, 0, 0, 0) == 1);
    assert((int)G(NID_OPEN, (uintptr_t)list[0], (uintptr_t)&handle, 0, 0, 0, 0, 0, 0) == 0);
    struct sky_usb_transfer *tin = (void *)G(NID_ALLOC, 0, 0, 0, 0, 0, 0, 0, 0);
    struct sky_usb_transfer *tout = (void *)G(NID_ALLOC, 0, 0, 0, 0, 0, 0, 0, 0);
    uint8_t inbuf[32], outbuf[32];
    in_count = out_count = 0;
    G(NID_FILL, (uintptr_t)tin, (uintptr_t)handle, 0x81, (uintptr_t)inbuf, 32, (uintptr_t)cb_in, 0, 0);
    assert(G(NID_SUBMIT, (uintptr_t)tin, 0, 0, 0, 0, 0, 0, 0) == 0);
    for (int i = 0; i < 30; i++) pump();
    assert(in_count == 0);                                       /* game has not spoken yet: stay quiet */
    send_out(tout, (const uint8_t *)"R", 1, outbuf); pump();
    assert(in_count == 1 && last_in[0] == 'R');
    int base = in_count;
    for (int i = 0; i < 100; i++) pump();                        /* ~1 ms of sleeping per empty pump */
    int idle = in_count - base;
    assert(idle >= 3 && idle <= 30);                             /* periodic, not on every pump */
    assert(last_in[0] == 'S' && last_in[6] == 1);
    printf("  idle status: %d reports in 100 idle pumps\n", idle);

    /* trace ring: the LAST events must survive (a crash log needs them), the first KEEP stay put */
    for (int i = 0; i < 150; i++) {
        uint8_t qq[32] = { 'Q', 0x01, (uint8_t)(i % 64), (uint8_t)i };
        assert((int)G(NID_CTRL, (uintptr_t)handle, 0x21, 9, 0x0200, 0, (uintptr_t)qq, 3 + (i & 1), 0) >= 0);
    }
    uint32_t head = 0; uint8_t e[SKY_TRACE_LEN];
    assert(proc.read(proc.ctx, inj.shared + offsetof(struct sky_shared, trace_head), &head, 4) == 0 && head > SKY_TRACE_N);
    assert(proc.read(proc.ctx, inj.shared + offsetof(struct sky_shared, trace) + (uint64_t)SKY_TRACE_IDX(head - 1) * SKY_TRACE_LEN, e, sizeof e) == 0);
    assert(e[0] == 'O' && e[8] == 'Q' && e[10] == 149 % 64);                       /* newest entry = newest command */
    assert(proc.read(proc.ctx, inj.shared + offsetof(struct sky_shared, trace), e, sizeof e) == 0 && e[0] != 0);   /* first kept */

    /* state guards: intact, then each kind of damage is reported */
    uint64_t where = 1;
    assert(sky_remote_check_state(&proc, &inj, &where) == 0 && where == 0);
    uint32_t junk = 0x12345678;
    assert(proc.write(proc.ctx, inj.state + offsetof(struct sky_hook_state, canary_b), &junk, 4) == 0);
    assert(sky_remote_check_state(&proc, &inj, &where) == 4 && where == inj.state + offsetof(struct sky_hook_state, canary_b));
    uint32_t good = SKY_CANARY_B;
    assert(proc.write(proc.ctx, inj.state + offsetof(struct sky_hook_state, canary_b), &good, 4) == 0);
    uint8_t z = 0;
    assert(proc.write(proc.ctx, inj.state + offsetof(struct sky_hook_state, guard) + 77, &z, 1) == 0);
    assert(sky_remote_check_state(&proc, &inj, &where) == 16 && where == inj.state + offsetof(struct sky_hook_state, guard) + 77);
    printf("  trace ring keeps the newest events; state guards detect damage\n");
}

int main(void) {
    test_stages();
    test_idle_status();
    for (int ctx = 0; ctx < 2; ctx++) {
        session(SWAP, N_SWAP, ctx, 0);          /* Swap Force: HandleEvents */
        session(TRAP, N_TRAP, ctx, 1);          /* Trap Team: HandleEventsTimeout + FreeTransfer */
        printf("  abi_has_ctx=%d: Swap Force + Trap Team sessions ok\n", ctx);
    }
    test_refusals();
    puts("hook/injector end-to-end tests passed");
    return 0;
}
