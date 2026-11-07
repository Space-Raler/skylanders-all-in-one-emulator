#include "proc_fake.h"
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

void sky_fake_init(struct sky_fake_proc *f) {
    memset(f, 0, sizeof *f);
    f->usleep_addr = (uint64_t)(uintptr_t)&usleep;
}

void sky_fake_add_import(struct sky_fake_proc *f, const char *nid, void *orig) {
    uint64_t *slot = &f->slot_mem[f->n_imports];
    *slot = (uint64_t)(uintptr_t)orig;
    f->imports[f->n_imports].nid = nid;
    f->imports[f->n_imports].slot = (uint64_t)(uintptr_t)slot;
    f->n_imports++;
}

uint64_t *sky_fake_slot(struct sky_fake_proc *f, const char *nid) {
    for (int i = 0; i < f->n_imports; i++)
        if (!strcmp(f->imports[i].nid, nid)) return (uint64_t *)(uintptr_t)f->imports[i].slot;
    return NULL;
}

static size_t page_up(size_t n) { size_t pg = (size_t)sysconf(_SC_PAGESIZE); return (n + pg - 1) & ~(pg - 1); }

static int alloc_region(struct sky_fake_proc *f, size_t size, int is_code, uint64_t *addr) {
    if (f->n_regions >= 8) return -1;
    size_t len = page_up(size);
    void *m = mmap(NULL, len, is_code ? (PROT_READ | PROT_EXEC) : (PROT_READ | PROT_WRITE),
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m == MAP_FAILED) return -1;
    f->regions[f->n_regions].addr = (uint64_t)(uintptr_t)m;
    f->regions[f->n_regions].len = len;
    f->regions[f->n_regions].is_code = is_code;
    f->n_regions++;
    *addr = (uint64_t)(uintptr_t)m;
    return 0;
}
static int p_alloc_code(void *c, size_t size, uint64_t *addr) { return alloc_region(c, size, 1, addr); }
static int p_alloc_data(void *c, size_t size, uint64_t *addr) { return alloc_region(c, size, 0, addr); }

static int p_write(void *c, uint64_t a, const void *s, size_t n) {
    struct sky_fake_proc *f = c;
    for (int i = 0; i < f->n_regions; i++) {
        if (a >= f->regions[i].addr && a < f->regions[i].addr + f->regions[i].len && f->regions[i].is_code) {
            /* the debugger can write through page protections; the game cannot */
            if (mprotect((void *)(uintptr_t)f->regions[i].addr, f->regions[i].len, PROT_READ | PROT_WRITE)) return -1;
            memcpy((void *)(uintptr_t)a, s, n);
            return mprotect((void *)(uintptr_t)f->regions[i].addr, f->regions[i].len, PROT_READ | PROT_EXEC) ? -1 : 0;
        }
    }
    memcpy((void *)(uintptr_t)a, s, n);
    return 0;
}
static int p_read(void *c, uint64_t a, void *d, size_t n) { (void)c; memcpy(d, (void *)(uintptr_t)a, n); return 0; }
static int p_find(void *c, const char *nid, uint64_t *va) {
    uint64_t *s = sky_fake_slot(c, nid);
    if (!s) return -1;
    *va = (uint64_t)(uintptr_t)s;
    return 0;
}
static int p_sym(void *c, const char *name, uint64_t *addr) {
    struct sky_fake_proc *f = c;
    if (strcmp(name, "usleep") || !f->usleep_addr) return -1;
    *addr = f->usleep_addr;
    return 0;
}
static int p_susp(void *c) { ((struct sky_fake_proc *)c)->suspended = 1; return 0; }
static int p_res(void *c) { ((struct sky_fake_proc *)c)->suspended = 0; return 0; }

struct sky_proc sky_fake_iface(struct sky_fake_proc *f) {
    struct sky_proc p = { f, p_alloc_code, p_alloc_data, p_write, p_read, p_find, p_sym, p_susp, p_res };
    return p;
}
