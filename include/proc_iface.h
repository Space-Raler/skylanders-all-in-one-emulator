/* Everything the injector needs from "a game process". Two backends:
 *   plugin/proc_fake.c    - host: same-address-space simulation, blob really executes; code region is
 *                           mapped R+X and data region R+W, so a hook that writes its own code faults
 *   plugin/proc_hijacker.cpp - PS5: libhijacker (needs an etaHEN-SDK checkout for headers + libhijacker.a)
 */
#ifndef SKY_PROC_IFACE_H
#define SKY_PROC_IFACE_H
#include <stddef.h>
#include <stdint.h>

struct sky_proc {
    void *ctx;
    /* Two regions, because the PS5 backend has no RWX allocator (text tail = executable but not writable
     * by the game; data tail = writable but not executable):
     *   alloc_code: executable region for the hook code image; the target never writes it.
     *   alloc_data: writable region for all mutable hook state; the target never executes it.
     * Both return 16-byte-aligned, zero-filled memory that is not in use by the target, or fail. */
    int (*alloc_code)(void *ctx, size_t size, uint64_t *addr);
    int (*alloc_data)(void *ctx, size_t size, uint64_t *addr);
    int (*write)(void *ctx, uint64_t addr, const void *src, size_t n);
    int (*read)(void *ctx, uint64_t addr, void *dst, size_t n);
    /* address of the game's PLT/GOT slot that imports libSceUsbd symbol <nid>; 0 if not imported */
    int (*find_import_slot)(void *ctx, const char *nid, uint64_t *slot_va);
    /* address of a function in the target (e.g. usleep in libkernel); 0 if unavailable */
    int (*resolve_symbol)(void *ctx, const char *name, uint64_t *addr);
    int (*suspend)(void *ctx);
    int (*resume)(void *ctx);
};
#endif
