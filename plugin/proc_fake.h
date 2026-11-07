/* Host simulation of a game process, for tests. The "game" is this same process:
 * addresses are real pointers, the PLT is an array of u64, and the two allocators hand out real mmap
 * regions with the PS5 rules: code = R+X (daemon writes flip it to RW briefly), data = R+W (no exec).
 * So the compiled blob genuinely executes, and a hook that writes its own code region crashes the test. */
#ifndef SKY_PROC_FAKE_H
#define SKY_PROC_FAKE_H
#include <stddef.h>
#include "proc_iface.h"
#include "sky_hooks.h"

#define FAKE_MAX_IMPORTS 32

struct fake_import { const char *nid; uint64_t slot; };

struct sky_fake_proc {
    struct fake_import imports[FAKE_MAX_IMPORTS];
    int n_imports;
    uint64_t slot_mem[FAKE_MAX_IMPORTS];   /* the simulated PLT/GOT */
    int suspended;
    uint64_t usleep_addr;
    struct { uint64_t addr; size_t len; int is_code; } regions[8];
    int n_regions;
};

void sky_fake_init(struct sky_fake_proc *f);
void sky_fake_add_import(struct sky_fake_proc *f, const char *nid, void *original_fn);  /* creates a slot */
uint64_t *sky_fake_slot(struct sky_fake_proc *f, const char *nid);
struct sky_proc sky_fake_iface(struct sky_fake_proc *f);
#endif
