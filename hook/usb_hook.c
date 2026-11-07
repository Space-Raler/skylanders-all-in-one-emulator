/* In-game virtual Skylanders portal: replacement bodies for the sceUsbd* PLT slots.
 *
 * Built two ways from this one file:
 *   - host tests: normal object, called directly / through a fake PLT.
 *   - in-game blob: -ffreestanding -fPIC, no libc, no imports, no absolute
 *     relocations (tools/gen_blob.py checks), copied into the game process by
 *     the injector and wired in by overwriting the game's PLT/GOT slots.
 *
 * All hooks have the same 8-register-argument signature so one macro can
 * tail-call the original when the plugin is disabled, whatever the real
 * prototype is. Hooks that need fewer arguments simply ignore the rest.
 */
#include "sky_hooks.h"
#include "sky_mem.h"
#include "sceusbd_abi.h"
#include "hook_state.h"

#define HOOK_ARGS uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3, \
                  uintptr_t a4, uintptr_t a5, uintptr_t a6, uintptr_t a7
#define HOOK_ANY_ARGS a0, a1, a2, a3, a4, a5, a6, a7
#define HOOK(name) uintptr_t skyhook_##name(HOOK_ARGS)

#ifdef SKY_FREESTANDING
/* freestanding builds still get compiler-generated memcpy/memset calls */
void *memcpy(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);
void *memcpy(void *d, const void *s, size_t n) {
    uint8_t *dd = d; const uint8_t *ss = s; while (n--) *dd++ = *ss++; return d;
}
void *memset(void *d, int c, size_t n) {
    uint8_t *dd = d; while (n--) *dd++ = (uint8_t)c; return d;
}
#endif

/* The only writable global of the blob: patched by the injector (it lives in the blob's .data, which the
 * game never writes). Everything else the hook mutates is reached through it. */
struct sky_hook_state *sky_state_ptr __attribute__((section(".data.sky_state_ptr"))) = 0;
#define ST (sky_state_ptr)
#define S  (ST->shared)
#define g_xfer        (ST->g_xfer)
#define g_xfer_used   (ST->g_xfer_used)
#define g_queue       (ST->g_queue)
#define g_dev_obj     (ST->g_dev_obj)
#define g_handle_obj  (ST->g_handle_obj)
#define g_ctx_obj     (ST->g_ctx_obj)
#define g_devlist     (ST->g_devlist)
#define g_cfg         (ST->g_cfg)
#define g_iface       (ST->g_iface)
#define g_ifdesc      (ST->g_ifdesc)
#define g_eps         (ST->g_eps)
#define g_config_value (ST->g_config_value)
#define g_strbuf      (ST->g_strbuf)

#define MAX_COMPLETE_PER_PUMP 64

/* Descriptors: cross-checked byte-for-byte against the descriptor tables embedded in SkyPortal Helper 0.4.0
 * (device, config incl. HID + both endpoints, report descriptor, string descriptors). Not checked against a
 * real portal capture. */
static const uint8_t DEV_DESC[18] = {
    0x12, 0x01, 0x10, 0x01, 0x00, 0x00, 0x00, 0x40,   /* bcdUSB 1.10, as in Helper 0.4.0 */
    0x30, 0x14, 0x50, 0x01, 0x00, 0x01, 0x01, 0x02, 0x00, 0x01 };
static const uint8_t CFG_DESC[41] = {
    9, 2, 0x29, 0x00, 1, 1, 0, 0x80, 0xFA,
    9, 4, 0, 0, 2, 3, 0, 0, 0,
    9, 0x21, 0x11, 0x01, 0, 1, 0x22, 0x1D, 0x00,
    7, 5, 0x81, 3, 0x20, 0x00, 0x16,                 /* IN bInterval 0x16, as in Helper 0.4.0 */
    7, 5, 0x02, 3, 0x20, 0x00, 1 };
static const uint8_t REPORT_DESC[29] = {
    0x06, 0x00, 0xFF, 0x09, 0x01, 0xA1, 0x01, 0x19, 0x01, 0x29, 0x40,
    0x15, 0x00, 0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, 0x20, 0x81, 0x00,
    0x19, 0x01, 0x29, 0x40, 0x91, 0x00, 0xC0 };

static void trace(uint8_t tag, uint8_t a, uint8_t b, uint16_t v, uint16_t w, const uint8_t *d, unsigned n) {
    if (tag == 'O' || tag == 'o') {
        uint32_t k = ((uint32_t)tag << 24) | ((d ? d[0] : 0u) << 16) | ((n & 0xFFu) << 8) | (d && n > 1 ? d[1] : 0u);
        if (k == S.dd_key) { if (++S.dd_rep > 3) return; }
        else { S.dd_key = k; S.dd_rep = 0; }
    }
    if (d && n && d[0] == 'S' && (tag == 'I' || tag == 'G' || tag == 'O' || tag == 'o')) {
        if (S.trace_status >= 8) return;                  /* status polls would flood the ring */
        S.trace_status++;
    }
    if (n > 32) n = 32;
    uint8_t *e = S.trace[SKY_TRACE_IDX(S.trace_head)];
    sky_zero(e, SKY_TRACE_LEN);
    e[0] = tag; e[1] = (uint8_t)n; e[2] = (uint8_t)v; e[3] = (uint8_t)(v >> 8);
    e[4] = (uint8_t)w; e[5] = (uint8_t)(w >> 8); e[6] = a; e[7] = b;
    if (d && n) sky_copy(e + 8, d, n);
    S.trace_head++;
}

static uintptr_t call_orig(unsigned id, HOOK_ARGS) {
    uint64_t f = S.orig[id];
    if (!f) return 0;
    return ((uintptr_t (*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t,
                           uintptr_t, uintptr_t, uintptr_t, uintptr_t))f)(HOOK_ANY_ARGS);
}

#define ENTER(id)                                              \
    do {                                                       \
        S.calls[SKY_H_##id]++;                                 \
        if (!S.enabled) return call_orig(SKY_H_##id, HOOK_ANY_ARGS); \
    } while (0)

static void apply_requests(void) {
    for (unsigned i = 0; i < SKY_SLOTS; i++) {
        uint32_t r = S.req[i];
        if (r == SKY_REQ_LOAD) { sky_portal_load(&S.portal, i, S.stage); S.req[i] = 0; }
        else if (r == SKY_REQ_REMOVE) { sky_portal_remove(&S.portal, i); S.req[i] = 0; }
    }
}

static void do_sleep(unsigned us) {
    if (S.usleep_fn && us) {
        ((int (*)(unsigned))(uintptr_t)S.usleep_fn)(us);
        S.idle_ms += (us + 999u) / 1000u;                 /* the only clock the blob has: time spent asleep here */
    }
}

static void build_config(void) {
    sky_zero(&g_cfg, sizeof g_cfg); sky_zero(&g_iface, sizeof g_iface);
    sky_zero(&g_ifdesc, sizeof g_ifdesc); sky_zero(g_eps, sizeof g_eps);
    for (int e = 0; e < 2; e++) {
        const uint8_t *d = &CFG_DESC[27 + 7 * e];
        g_eps[e].bLength = d[0]; g_eps[e].bDescriptorType = d[1];
        g_eps[e].bEndpointAddress = d[2]; g_eps[e].bmAttributes = d[3];
        g_eps[e].wMaxPacketSize = (uint16_t)(d[4] | (d[5] << 8)); g_eps[e].bInterval = d[6];
    }
    const uint8_t *i = &CFG_DESC[9];
    g_ifdesc.bLength = i[0]; g_ifdesc.bDescriptorType = i[1]; g_ifdesc.bInterfaceNumber = i[2];
    g_ifdesc.bAlternateSetting = i[3]; g_ifdesc.bNumEndpoints = i[4]; g_ifdesc.bInterfaceClass = i[5];
    g_ifdesc.bInterfaceSubClass = i[6]; g_ifdesc.bInterfaceProtocol = i[7]; g_ifdesc.iInterface = i[8];
    g_ifdesc.endpoint = g_eps;
    g_ifdesc.extra = &CFG_DESC[18]; g_ifdesc.extra_length = 9;      /* HID descriptor */
    g_iface.altsetting = &g_ifdesc; g_iface.num_altsetting = 1;
    g_cfg.bLength = 9; g_cfg.bDescriptorType = 2; g_cfg.wTotalLength = 0x29;
    g_cfg.bNumInterfaces = 1; g_cfg.bConfigurationValue = 1; g_cfg.bmAttributes = 0x80; g_cfg.MaxPower = 0xFA;
    g_cfg.interface = &g_iface;
}

static int copy_out(const uint8_t *src, size_t n, uint8_t *dst, size_t want) {
    if (!dst) return 0;
    if (n > want) n = want;
    sky_copy(dst, src, n);
    return (int)n;
}

/* Returns bytes transferred (IN) / accepted (OUT), or a negative libusb error. */
static int do_control(uint8_t rt, uint8_t req, uint16_t value, uint16_t index,
                      uint8_t *data, uint16_t len) {
    (void)index;
    if (!(rt == 0xA1 && req == 0x01) && !(rt == 0x21 && req == 0x09)) { uint8_t wl[2] = {(uint8_t)len, (uint8_t)(len >> 8)}; trace('C', rt, req, value, index, wl, 2); }
    if (rt == 0x80 && req == 6) {                       /* GET_DESCRIPTOR (device) */
        uint8_t type = (uint8_t)(value >> 8), id = (uint8_t)value;
        if (type == 1) return copy_out(DEV_DESC, sizeof DEV_DESC, data, len);
        if (type == 2) return copy_out(CFG_DESC, sizeof CFG_DESC, data, len);
        if (type == 3) {
            if (id == 0) { g_strbuf[0] = 4; g_strbuf[1] = 3; g_strbuf[2] = 9; g_strbuf[3] = 4; return copy_out(g_strbuf, 4, data, len); }
            if (id > 2) return LIBUSB_ERROR_PIPE;
            unsigned n = 0; const char *s = id == 1 ? "Activision" : "Spyro Porta";  /* no pointer table: stays relocation-free */
            while (s[n] && n < 30) { g_strbuf[2 + 2 * n] = (uint8_t)s[n]; g_strbuf[3 + 2 * n] = 0; n++; }
            g_strbuf[0] = (uint8_t)(2 + 2 * n); g_strbuf[1] = 3;
            return copy_out(g_strbuf, 2 + 2 * n, data, len);
        }
        return LIBUSB_ERROR_PIPE;
    }
    if (rt == 0x81 && req == 6) {                       /* GET_DESCRIPTOR (interface): HID / report */
        uint8_t type = (uint8_t)(value >> 8);
        if (type == 0x22) return copy_out(REPORT_DESC, sizeof REPORT_DESC, data, len);
        if (type == 0x21) return copy_out(&CFG_DESC[18], 9, data, len);
        return LIBUSB_ERROR_PIPE;
    }
    if (rt == 0x80 && req == 0) { static const uint8_t z[2] = {0, 0}; return copy_out(z, 2, data, len); }
    if (rt == 0x80 && req == 8) { return copy_out(&g_config_value, 1, data, len); }
    if (rt == 0x00 && req == 9) { g_config_value = (uint8_t)value; return 0; }
    if (rt == 0x00 && req == 5) return 0;               /* SET_ADDRESS */
    if (rt == 0x01 && req == 11) return 0;              /* SET_INTERFACE */
    if (rt == 0x21 && req == 0x09) {                    /* HID SET_REPORT: a portal command */
        if (!data) return LIBUSB_ERROR_INVALID_PARAM;
        S.cmds_seen++; S.last_cmd = data[0];
        trace('O', 0, 0, len, 0, data, len);
        sky_portal_command(&S.portal, data, len);
        return len;
    }
    if (rt == 0xA1 && req == 0x01) {                    /* HID GET_REPORT */
        uint8_t rep[SKY_REPORT];
        if (!sky_portal_next_report(&S.portal, rep)) sky_zero(rep, sizeof rep);
        else trace('G', 0, 0, 32, 0, rep, 32);
        return copy_out(rep, sizeof rep, data, len);
    }
    if (rt == 0x21 && (req == 0x0A || req == 0x0B)) return 0;   /* SET_IDLE / SET_PROTOCOL */
    return LIBUSB_ERROR_PIPE;
}

static void complete(struct sky_usb_transfer *t, int actual) {
    t->status = LIBUSB_TRANSFER_COMPLETED;
    t->actual_length = actual;
    S.transfers_done++;
    if (t->callback) t->callback(t);
}

/* Try to finish one queued transfer; returns 1 if it completed. */
static int try_finish(struct sky_usb_transfer *t) {
    if (t->type == LIBUSB_TRANSFER_TYPE_CONTROL) {
        if (!t->buffer || t->length < 8) { complete(t, 0); return 1; }
        const uint8_t *b = t->buffer;
        uint16_t wlen = (uint16_t)(b[6] | (b[7] << 8));
        if ((unsigned)wlen + 8 > (unsigned)t->length) wlen = (uint16_t)(t->length - 8);
        int r = do_control(b[0], b[1], (uint16_t)(b[2] | (b[3] << 8)), (uint16_t)(b[4] | (b[5] << 8)),
                           t->buffer + 8, wlen);
        if (r < 0) { t->status = 4 /* STALL */; t->actual_length = 0; S.transfers_done++; if (t->callback) t->callback(t); return 1; }
        complete(t, r);
        return 1;
    }
    if (t->endpoint & 0x80) {                           /* interrupt/bulk IN */
        uint8_t rep[SKY_REPORT];
        if (!sky_portal_in_ready(&S.portal) || !sky_portal_next_report(&S.portal, rep)) {
            /* Nothing queued. A real portal still answers each IN poll with a status report; do that, but only once
             * the game has talked to us and no more often than idle_status_ms of the game's own idle sleeping. */
            if (!S.idle_status_ms || !S.cmds_seen || S.idle_ms < S.idle_status_ms) return 0;
            sky_portal_status_report(&S.portal, rep);
        }
        S.idle_ms = 0;
        int n = t->length < SKY_REPORT ? t->length : SKY_REPORT;
        trace('I', t->endpoint, 0, (uint16_t)n, 0, rep, n > 0 ? (unsigned)n : 0);
        if (t->buffer && n > 0) sky_copy(t->buffer, rep, (size_t)n);
        complete(t, n > 0 ? n : 0);
        return 1;
    }
    /* OUT: a command report */
    if (t->buffer && t->length > 0) {
        S.cmds_seen++; S.last_cmd = t->buffer[0];
        trace('o', t->endpoint, 0, (uint16_t)t->length, 0, t->buffer, (unsigned)t->length);
        sky_portal_out_packet(&S.portal, t->buffer, (size_t)t->length);
    }
    complete(t, t->length);
    return 1;
}

static int pump(void) {
    int done = 0;
    apply_requests();
    for (int pass = 0; pass < 2 && done < MAX_COMPLETE_PER_PUMP; pass++) {
        /* two passes: OUT commands first so same-pump IN transfers see the replies */
        for (int i = 0; i < MAX_QUEUE && done < MAX_COMPLETE_PER_PUMP; i++) {
            struct sky_usb_transfer *t = g_queue[i];
            if (!t) continue;
            int is_in = t->type != LIBUSB_TRANSFER_TYPE_CONTROL && (t->endpoint & 0x80);
            if ((pass == 0) == is_in) continue;
            g_queue[i] = 0;                             /* callback may resubmit */
            if (try_finish(t)) done++;
            else { g_queue[i] = t; if (is_in && !S.pend_noted) { S.pend_noted = 1; trace('P', t->endpoint, t->type, (uint16_t)t->length, 0, 0, 0); } }                        /* IN with nothing to say: stay pending */
        }
    }
    return done;
}

/* ---- hooks ------------------------------------------------------------ */

HOOK(Init) {
    ENTER(Init);
    if (S.abi_has_ctx && a0) *(void **)a0 = g_ctx_obj;
    return 0;
}
HOOK(Exit) { ENTER(Exit); return 0; }
HOOK(GetDeviceList) {
    ENTER(GetDeviceList);
    void ***out = (void ***)(S.abi_has_ctx ? a1 : a0);
    if (!out) return (uintptr_t)(intptr_t)LIBUSB_ERROR_INVALID_PARAM;
    g_devlist[0] = g_dev_obj; g_devlist[1] = 0;
    *out = g_devlist;
    return 1;
}
HOOK(FreeDeviceList) { ENTER(FreeDeviceList); return 0; }
HOOK(GetDeviceDescriptor) {
    ENTER(GetDeviceDescriptor);
    if (!a1) return (uintptr_t)(intptr_t)LIBUSB_ERROR_INVALID_PARAM;
    sky_copy((void *)a1, DEV_DESC, sizeof DEV_DESC);
    return 0;
}
HOOK(GetConfigDescriptor) {
    ENTER(GetConfigDescriptor);
    if (!a2 || a1 != 0) return (uintptr_t)(intptr_t)LIBUSB_ERROR_NOT_FOUND;
    build_config();
    *(const struct sky_usb_config_descriptor **)a2 = &g_cfg;
    return 0;
}
HOOK(FreeConfigDescriptor) { ENTER(FreeConfigDescriptor); return 0; }
HOOK(GetBusNumber) { ENTER(GetBusNumber); return 1; }
HOOK(GetDeviceAddress) { ENTER(GetDeviceAddress); return 2; }
HOOK(Open) {
    ENTER(Open);
    if (!a1) return (uintptr_t)(intptr_t)LIBUSB_ERROR_INVALID_PARAM;
    *(void **)a1 = g_handle_obj;
    return 0;
}
HOOK(KernelDriverActive) { ENTER(KernelDriverActive); return 0; }
HOOK(DetachKernelDriver) { ENTER(DetachKernelDriver); return 0; }
HOOK(AttachKernelDriver) { ENTER(AttachKernelDriver); return 0; }
HOOK(ClaimInterface) { ENTER(ClaimInterface); return 0; }
HOOK(CheckConnected) { ENTER(CheckConnected); return S.check_connected_ret; }

HOOK(AllocTransfer) {
    ENTER(AllocTransfer);
    (void)a0;                               /* iso transfers not supported */
    for (int i = 0; i < MAX_XFER; i++) {
        if (!g_xfer_used[i]) {
            g_xfer_used[i] = 1;
            sky_zero(&g_xfer[i], sizeof g_xfer[i]);
            { uint64_t v[3]={a0,a1,(uint64_t)(uintptr_t)&g_xfer[i]}; trace('Y',0,0,0,0,(const uint8_t *)v,24); } return (uintptr_t)&g_xfer[i];
        }
    }
    return 0;
}
HOOK(FreeTransfer) {
    ENTER(FreeTransfer);
    for (int i = 0; i < MAX_QUEUE; i++) if (g_queue[i] == (void *)a0) g_queue[i] = 0;
    for (int i = 0; i < MAX_XFER; i++) if ((uintptr_t)&g_xfer[i] == a0) g_xfer_used[i] = 0;
    return 0;
}
/* libusb_fill_interrupt_transfer(transfer, handle, endpoint, buffer, length, cb, user_data, timeout) */
HOOK(FillInterruptTransfer) {
    ENTER(FillInterruptTransfer);
    struct sky_usb_transfer *t = (struct sky_usb_transfer *)a0;
    if (!t) return 0;
    t->dev_handle = (void *)a1;
    t->endpoint = (uint8_t)a2;
    t->type = LIBUSB_TRANSFER_TYPE_INTERRUPT;
    t->buffer = (uint8_t *)a3;
    t->length = (int)a4;
    t->callback = (sky_usb_transfer_cb)a5;
    t->user_data = (void *)a6;
    t->timeout = (unsigned)a7;
    return 0;
}
HOOK(SubmitTransfer) {
    ENTER(SubmitTransfer);
    struct sky_usb_transfer *t = (struct sky_usb_transfer *)a0;
    if (S.calls[SKY_H_SubmitTransfer] <= 8) {          /* layout dump of the first few submits only */
        { uint64_t v[2]={a0,a1}; trace('X',0,0,0,0,(const uint8_t *)v,16); }
        if (t) {
            trace('T', t->endpoint, t->type, (uint16_t)t->length, t->flags, (const uint8_t *)t, 32);
            trace('U', 0, 0, 0, 0, (const uint8_t *)t + 32, 32);
            if (t->buffer && t->length > 0) trace('B', 0, 0, (uint16_t)t->length, 0, t->buffer, (unsigned)t->length);
        }
    }
    if (!t) return (uintptr_t)(intptr_t)LIBUSB_ERROR_INVALID_PARAM;
    for (int i = 0; i < MAX_QUEUE; i++) {
        if (!g_queue[i]) { t->status = 0; t->actual_length = 0; g_queue[i] = t; return 0; }
    }
    return (uintptr_t)(intptr_t)LIBUSB_ERROR_NO_MEM;
}
HOOK(HandleEvents) {
    ENTER(HandleEvents);
    if (!pump()) { do_sleep(1000); pump(); }
    return 0;
}
HOOK(HandleEventsTimeout) {
    ENTER(HandleEventsTimeout);
    const struct { int64_t sec, usec; } *tv = (const void *)(S.abi_has_ctx ? a1 : a0);
    int64_t us = tv ? tv->sec * 1000000 + tv->usec : 0;
    if (!pump() && us > 0) { do_sleep(us > 1000 ? 1000u : (unsigned)us); pump(); }
    return 0;
}
/* libusb_control_transfer(handle, bmRequestType, bRequest, wValue, wIndex, data, wLength, timeout) */
HOOK(ControlTransfer) {
    ENTER(ControlTransfer);
    apply_requests();
    return (uintptr_t)(intptr_t)do_control((uint8_t)a1, (uint8_t)a2, (uint16_t)a3,
                                           (uint16_t)a4, (uint8_t *)a5, (uint16_t)a6);
}
