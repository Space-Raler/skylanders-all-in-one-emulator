/* Hook table + the memory block shared between the injected blob and the daemon. */
#ifndef SKY_HOOKS_H
#define SKY_HOOKS_H
#include <stdint.h>
#include "sky_portal.h"

/* X(name, nid, required)
 * NIDs verified in your findings (SHA-1 scheme, 22/22 observed). "required" means
 * injection is refused if the game does not import it. At least one of
 * HandleEvents / HandleEventsTimeout is also required (checked in injector.c). */
#define SKY_HOOK_LIST(X) \
    X(Init,                  "TOhg7P6kTH4", 1) \
    X(Exit,                  "Fq6+0Fm55xU", 0) \
    X(GetDeviceList,         "8qB9Ar4P5nc", 1) \
    X(FreeDeviceList,        "EQ6SCLMqzkM", 0) \
    X(GetDeviceDescriptor,   "bhomgbiQgeo", 0) \
    X(GetConfigDescriptor,   "Dkm5qe8j3XE", 0) \
    X(FreeConfigDescriptor,  "Hvd3S--n25w", 0) \
    X(GetBusNumber,          "t7WE9mb1TB8", 0) \
    X(GetDeviceAddress,      "GjlCrU4GcIY", 0) \
    X(Open,                  "VJ6oMq-Di2U", 1) \
    X(KernelDriverActive,    "RLf56F-WjKQ", 0) \
    X(DetachKernelDriver,    "Y5go+ha6eDs", 0) \
    X(AttachKernelDriver,    "BKMEGvfCPyU", 0) \
    X(ClaimInterface,        "AE+mHBHneyk", 0) \
    X(AllocTransfer,         "0ktE1PhzGFU", 1) \
    X(FreeTransfer,          "-sgi7EeLSO8", 0) \
    X(FillInterruptTransfer, "t3J5pXxhJlI", 0) \
    X(SubmitTransfer,        "L0EHgZZNVas", 1) \
    X(HandleEvents,          "EkqGLxWC-S0", 0) \
    X(HandleEventsTimeout,   "+wU6CGuZcWk", 0) \
    X(ControlTransfer,       "RRKFcKQ1Ka4", 0) \
    X(CheckConnected,        "MlW6deWfPp0", 0)

enum sky_hook_id {
#define X(n, nid, req) SKY_H_##n,
    SKY_HOOK_LIST(X)
#undef X
    SKY_HOOK_COUNT
};

#define SKY_SHARED_MAGIC   0x504F4B53u   /* "SKOP" */
#define SKY_SHARED_VERSION 1u

enum { SKY_REQ_NONE = 0, SKY_REQ_LOAD = 1, SKY_REQ_REMOVE = 2 };

#define SKY_TRACE_N   96
#define SKY_TRACE_KEEP 32                 /* first events stay (handshake); the rest rotate, so a crash shows the LAST ones */
#define SKY_TRACE_IDX(n) ((n) < SKY_TRACE_KEEP ? (n) : SKY_TRACE_KEEP + ((n) - SKY_TRACE_KEEP) % (SKY_TRACE_N - SKY_TRACE_KEEP))
#define SKY_TRACE_LEN 40

struct sky_shared {
    uint32_t magic, version;
    volatile uint32_t enabled;        /* 0 = every hook tail-calls the original */
    uint32_t abi_has_ctx;             /* 1: Init/Exit/GetDeviceList/HandleEvents* take a leading ctx arg */
    uint32_t check_connected_ret;     /* what sceUsbdCheckConnected returns (semantics unverified) */
    uint32_t pend_noted;
    uint32_t dd_key, dd_rep;
    uint32_t idle_status_ms;          /* 0 = only answer commands / figure changes; else an idle status report this often */
    uint32_t idle_ms;                 /* ms the game has slept in HandleEvents since an IN poll last completed */
    uint64_t usleep_fn;               /* int (*)(unsigned us), resolved by the injector; may be 0 */
    uint64_t orig[SKY_HOOK_COUNT];    /* original PLT slot contents, for pass-through */
    uint32_t calls[SKY_HOOK_COUNT];   /* diagnostics */
    uint32_t transfers_done;
    uint32_t cmds_seen;
    uint32_t last_cmd;
    uint32_t stage_slot;
    volatile uint32_t req[SKY_SLOTS]; /* daemon -> game: LOAD/REMOVE, game clears when consumed */
    uint8_t  stage[SKY_FIG_SIZE];     /* figure image the daemon wants loaded */
    struct sky_portal portal;
    /* diagnostic trace: the FIRST SKY_TRACE_N events after the hook is enabled. Entry layout (see trace() in
     * usb_hook.c): [0]=tag [1]=n data bytes [2..3]=v [4..5]=w [6]=a [7]=b [8..39]=data. 'S' reports capped. */
    volatile uint32_t trace_head;
    uint32_t trace_status;
    uint8_t  trace[SKY_TRACE_N][SKY_TRACE_LEN];
};

/* Hook entry points are named skyhook_<name>; all take 8 pointer-sized args. */
#endif
