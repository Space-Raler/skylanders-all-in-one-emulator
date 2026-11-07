/* ALL writable state of the in-game hook lives in one struct, allocated in a *writable* region of the
 * game. The hook code itself lives in a separate *executable* region that the game cannot write (the
 * etaHEN SDK only offers text-section and data-section tail allocators, never one RWX region), so the
 * code reaches this state through a single pointer (sky_state_ptr) that the injector patches in.
 * sky_shared is first so the daemon can address it at the state's base. */
#ifndef SKY_HOOK_STATE_H
#define SKY_HOOK_STATE_H
#include <stddef.h>
#include <stdint.h>
#include "sky_hooks.h"
#include "sceusbd_abi.h"

#define MAX_XFER  32
#define MAX_QUEUE 64

#define SKY_GUARD_BYTES 1024
#define SKY_CANARY_A 0xC0DEA11Eu
#define SKY_CANARY_B 0xC0DEB22Eu
#define SKY_CANARY_C 0xC0DEC33Eu
#define SKY_GUARD_FILL 0xA5

struct sky_hook_state {
    struct sky_shared shared;
    uint32_t canary_a;               /* the hook never writes the canaries or the guard band: if the daemon sees them */
    uint32_t pad_a;                  /* changed, something else in the game is using this memory */
    struct sky_usb_transfer g_xfer[MAX_XFER];
    uint32_t canary_b, pad_b;
    uint8_t g_xfer_used[MAX_XFER];
    struct sky_usb_transfer *g_queue[MAX_QUEUE];
    uint8_t g_dev_obj[16], g_handle_obj[16], g_ctx_obj[16];
    void *g_devlist[2];
    struct sky_usb_config_descriptor g_cfg;
    struct sky_usb_interface g_iface;
    struct sky_usb_interface_descriptor g_ifdesc;
    struct sky_usb_endpoint_descriptor g_eps[2];
    uint8_t g_config_value;
    uint8_t g_strbuf[64];
    uint32_t canary_c, pad_c;
    uint8_t guard[SKY_GUARD_BYTES];
};
_Static_assert(offsetof(struct sky_hook_state, shared) == 0, "shared block must be first");
#endif
