// libhijacker backend (headers + libhijacker.a come from an etaHEN-SDK checkout) for struct sky_proc.
//
// Written against the real headers in etaHEN-SDK (include/hijacker/*.hpp, include/kernel/rtld.hpp,
// include/dbg/dbg.hpp) and modelled on Plugin_samples/Game_Plugin_Loader. It has been syntax-checked against
// those headers with stubs for the payload SDK (see tools/syntax_check_ps5.sh) but NOT linked, and NOT run on
// a console.
//
// Memory model (this is why struct sky_proc has two allocators):
//   libhijacker has no RWX allocator. It offers only ProcessMemoryAllocator on the eboot's TEXT section
//   (executable, not writable by the game) and DATA section (writable, not executable); both carve from the
//   *end of the section*, which the SDK documents as "likely unused". We follow that convention for both the
//   hook code (text tail) and the hook state (data tail) but verify the carved range is all zero before
//   handing it out, and refuse otherwise instead of overwriting something live. The debugger-side write
//   (dbg::write) bypasses page protection, which is how code gets into the text tail at all.
#ifdef SKY_HAVE_PS5_SDK

#include <cstdint>
#include <cstring>
#include <unistd.h>

#include "dbg.hpp"
#include "dbg/dbg.hpp"
#include "elf/elf.hpp"
#include "hijacker/hijacker.hpp"
#include "nid.hpp"
#include <ps5/kernel.h>
#include <ps5/mdbg.h>
#include "offsets.hpp"

extern "C" {
#include "proc_iface.h"
}
#include "sky_log.h"

extern "C" {
int32_t sceKernelPrepareToSuspendProcess(pid_t pid);
int32_t sceKernelSuspendProcess(pid_t pid);
int32_t sceKernelPrepareToResumeProcess(pid_t pid);
int32_t sceKernelResumeProcess(pid_t pid);
}

namespace {

struct EtaProc {
    UniquePtr<Hijacker> hj{nullptr};
    int pid = 0;
    bool suspended = false;
    bool live = false;       // hooks on, game running: the hook may clear request words at any moment
    size_t taken_code = 0;   // bytes already carved from the text tail in this attach
    size_t taken_data = 0;   // ... and from the data tail
};
EtaProc g;

// Carve `size` bytes from the end of `sec`, SDK-style, but only if they are currently all zero.
int tail_alloc(const SharedLibSection *sec, size_t &taken, size_t size, const char *what, uint64_t *out) {
    if (!sec) { sky_log("%s: section not found", what); return -1; }
    const uintptr_t start = sec->start(), end = sec->end();
    size = (size + 15u) & ~(size_t)15u;
    const uintptr_t addr = (end - taken - size) & ~(uintptr_t)15u;
    if (addr <= start || end - addr > (end - start) / 2) {       // never eat into more than half a section
        sky_log("%s: section 0x%llx..0x%llx too small for 0x%zx bytes", what,
                (unsigned long long)start, (unsigned long long)end, size);
        return -1;
    }
    auto buf = g.hj->read(addr, size);                           // UniquePtr<uint8_t[]>, null on failure
    if (!buf) { sky_log("%s: cannot read target 0x%llx (+0x%zx)", what, (unsigned long long)addr, size); return -1; }
    for (size_t i = 0; i < size; i++) {
        if (buf[i] != 0) {
            sky_log("%s: tail 0x%llx is not zero at +0x%zx; refusing to overwrite live memory", what,
                    (unsigned long long)addr, i);
            return -1;
        }
    }
    taken = end - addr;
    *out = addr;
    sky_log("%s: carved 0x%zx bytes at 0x%llx (section 0x%llx..0x%llx)", what, size,
            (unsigned long long)addr, (unsigned long long)start, (unsigned long long)end);
    return 0;
}

int p_alloc_code(void *, size_t size, uint64_t *addr) {
    auto *eboot = g.hj->getEboot();
    return tail_alloc(eboot ? eboot->getTextSection() : nullptr, g.taken_code, size, "code(text tail)", addr);
}
// Data-section tail is live on this game; search the top of the data segment downwards for a zero run
// (state + guard bands), then re-read after a delay and require it to still be zero.
int cave_alloc(const SharedLibSection *sec, size_t size, const char *what, uint64_t *out) {
    if (!sec) { sky_log("%s: section not found", what); return -1; }
    const uintptr_t start = sec->start(), end = sec->end();
    const size_t GUARD = 0x100, CH = 0x10000, LIMIT = 0x800000;
    size = (size + 15u) & ~(size_t)15u;
    const size_t need = size + 2 * GUARD + 16;
    const uintptr_t floor_ = (end - start > LIMIT) ? end - LIMIT : start;
    size_t run = 0, unreadable = 0;
    uintptr_t run_top = 0;
    for (uintptr_t c = end; c > floor_;) {
        const size_t n = (c - floor_ < CH) ? (size_t)(c - floor_) : CH;
        c -= n;
        auto buf = g.hj->read(c, n);
        if (!buf) { unreadable++; run = 0; continue; }
        for (size_t i = n; i-- > 0;) {
            if (buf[i] != 0) { run = 0; continue; }
            if (run == 0) run_top = c + i + 1;
            if (++run < need) continue;
            const uintptr_t lo = run_top - run;
            const uintptr_t addr = (lo + GUARD + 15u) & ~(uintptr_t)15u;
            usleep(200000);
            auto chk = g.hj->read(lo, need);
            bool ok = (bool)chk;
            for (size_t k = 0; ok && k < need; k++) ok = chk[k] == 0;
            if (!ok) { sky_log("%s: cave at 0x%llx changed on recheck; continuing", what, (unsigned long long)lo);
                       run = 0; continue; }
            *out = addr;
            sky_log("%s: zero cave 0x%llx..0x%llx, using 0x%llx (+0x%zx), %zu unreadable chunks skipped", what,
                    (unsigned long long)lo, (unsigned long long)(lo + need), (unsigned long long)addr, size, unreadable);
            return 0;
        }
    }
    sky_log("%s: no zero run of 0x%zx bytes in 0x%llx..0x%llx (%zu unreadable chunks)", what, need,
            (unsigned long long)floor_, (unsigned long long)end, unreadable);
    return -1;
}

int p_alloc_data(void *, size_t size, uint64_t *addr) {
    auto *eboot = g.hj->getEboot();
    return cave_alloc(eboot ? eboot->getDataSection() : nullptr, size, "state(data cave)", addr);
}

// libhijacker's write (dbg::write) fails at the data cave even for 8 bytes (log: "chunk size 0x8 failed at +0x0"),
// while reads work. Fall back through the payload SDK's own cross-process writers, logging each return code and
// the page protection, and read back to prove the bytes landed. Protection is only changed as a last resort.
int p_write(void *, uint64_t addr, const void *src, size_t n) {
    const uint8_t *s = static_cast<const uint8_t *>(src);
    const char *how = nullptr;
    if (g.hj->write(addr, s, n)) {
        how = "libhijacker";
    } else {
        sky_log("write: libhijacker FAILED at 0x%llx (+0x%zx); vmem prot=%d", (unsigned long long)addr, n,
                (int)kernel_get_vmem_protection(g.pid, (intptr_t)addr, n));
        int r = mdbg_copyin(g.pid, s, (intptr_t)addr, n);
        sky_log("write: mdbg_copyin rc=%d", r);
        if (r == 0) {
            how = "mdbg_copyin";
        } else {
            r = kernel_proc_copyin(g.pid, s, (intptr_t)addr, n);
            sky_log("write: kernel_proc_copyin rc=%d", r);
            if (r == 0) {
                how = "kernel_proc_copyin";
            } else {
                const uintptr_t pg = addr & ~(uintptr_t)0xfff;
                const size_t len = (size_t)(((addr + n + 0xfff) & ~(uintptr_t)0xfff) - pg);
                r = kernel_set_vmem_protection(g.pid, (intptr_t)pg, len, 3 /* RW */);
                sky_log("write: set_vmem_protection RW 0x%llx+0x%zx rc=%d", (unsigned long long)pg, len, r);
                r = kernel_proc_copyin(g.pid, s, (intptr_t)addr, n);
                sky_log("write: kernel_proc_copyin after RW rc=%d", r);
                if (r == 0) how = "kernel_proc_copyin(after RW)";
            }
        }
    }
    if (!how) { sky_log("write: ALL write paths failed at 0x%llx", (unsigned long long)addr); return -1; }
    auto back = g.hj->read(addr, n);
    if (!back) { sky_log("write: readback read failed"); return -1; }
    if (g.live && n == 4) {                              // a request word the hook already consumed reads back as 0
        const uint8_t *b = back.get();
        if (!(b[0] | b[1] | b[2] | b[3])) return 0;
    }
    if (std::memcmp(back.get(), s, n) != 0) { sky_log("write: READBACK MISMATCH at 0x%llx (via %s)", (unsigned long long)addr, how); return -1; }
    sky_log("write: 0x%zx bytes at 0x%llx ok via %s", n, (unsigned long long)addr, how);
    return 0;
}

int p_read(void *, uint64_t addr, void *dst, size_t n) { return g.hj->read(addr, dst, n) ? 0 : -1; }

// VA of the game's PLT/GOT slot for the import with this NID: symbol-table index by NID, then the JMPREL
// entry that references that index (same walk as the SDK sample's HookGame()).
int p_find(void *, const char *nid_str, uint64_t *va) {
    if (!nid_str || std::strlen(nid_str) < NID_LENGTH) return -1;
    const Nid nid{nid_str};
    SharedLib *eboot = g.hj->getEboot();
    if (!eboot) return -1;
    RtldMeta *meta = eboot->getMetaData();
    if (!meta) return -1;
    const ssize_t idx = meta->getSymbolTable().getSymbolIndex(nid);
    if (idx < 0) return -1;                                       // game does not import it
    for (const Elf64_Rela &r : meta->getPltTable()) {
        if (static_cast<ssize_t>(ELF64_R_SYM(r.r_info)) == idx) {
            *va = eboot->imagebase() + r.r_offset;
            return 0;
        }
    }
    return -1;                                                    // symbol present but not via a PLT slot
}

// Only "usleep" is ever asked for; it is resolved in the *game's* libkernel.
int p_sym(void *, const char *name, uint64_t *addr) {
    if (!name || std::strcmp(name, "usleep") != 0) return -1;
    const uintptr_t a = g.hj->getLibKernelAddress(nid::usleep);
    if (!a) return -1;
    *addr = a;
    return 0;
}

// Same suspend/resume dance as Game_Plugin_Loader (its comment: resuming without the pause can panic the
// kernel when the user later quits the game).
int p_susp(void *) {
    if (g.suspended) return 0;
    sceKernelPrepareToSuspendProcess(g.pid);
    if (sceKernelSuspendProcess(g.pid) < 0) return -1;
    g.suspended = true;
    return 0;
}
int p_res(void *) {
    if (!g.suspended) return 0;
    usleep(1000);
    sceKernelPrepareToResumeProcess(g.pid);
    sceKernelResumeProcess(g.pid);
    g.suspended = false;
    g.live = true;
    return 0;
}

}  // namespace

// defined in libhijacker/source/offsets.cpp (global, C++ linkage)
uint32_t getSystemSwVersion();

extern "C" int sky_hijack_open(int pid, struct sky_proc *out) {
    sky_log("probe2: libhijacker kern.sdk_version=0x%x", (unsigned)getSystemSwVersion());
    {
        const unsigned long long db = (unsigned long long)KERNEL_ADDRESS_DATA_BASE;
        sky_log("probe2: sdk allproc-data_base      =0x%llx", (unsigned long long)KERNEL_ADDRESS_ALLPROC - db);
        sky_log("probe2: sdk rootvnode-data_base    =0x%llx", (unsigned long long)KERNEL_ADDRESS_ROOTVNODE - db);
        sky_log("probe2: sdk security_flags-data_base=0x%llx", (unsigned long long)KERNEL_ADDRESS_SECURITY_FLAGS - db);
        sky_log("probe2: sdk qa_flags-data_base     =0x%llx", (unsigned long long)KERNEL_ADDRESS_QA_FLAGS - db);
        sky_log("probe2: sdk utoken_flags-data_base =0x%llx", (unsigned long long)KERNEL_ADDRESS_UTOKEN_FLAGS - db);
        sky_log("probe2: sdk proc offsets p_ucred=0x%llx p_fd=0x%llx p_pid=0x%llx p_vmspace=0x%llx",
                (unsigned long long)KERNEL_OFFSET_PROC_P_UCRED, (unsigned long long)KERNEL_OFFSET_PROC_P_FD,
                (unsigned long long)KERNEL_OFFSET_PROC_P_PID, (unsigned long long)KERNEL_OFFSET_PROC_P_VMSPACE);
        sky_log("probe2: sdk ucred offsets uid=0x%llx authid=0x%llx caps=0x%llx attrs=0x%llx",
                (unsigned long long)KERNEL_OFFSET_UCRED_CR_UID, (unsigned long long)KERNEL_OFFSET_UCRED_CR_SCEAUTHID,
                (unsigned long long)KERNEL_OFFSET_UCRED_CR_SCECAPS, (unsigned long long)KERNEL_OFFSET_UCRED_CR_SCEATTRS);
        sky_log("probe2: sdk filedesc rdir=0x%llx jdir=0x%llx vmspace.pmap=0x%llx",
                (unsigned long long)KERNEL_OFFSET_FILEDESC_FD_RDIR, (unsigned long long)KERNEL_OFFSET_FILEDESC_FD_JDIR,
                (unsigned long long)KERNEL_OFFSET_VMSPACE_VM_PMAP);
    }
    sky_log("probe: sdk fw_version=0x%x", (unsigned)kernel_get_fw_version());
    sky_log("probe: sdk data_base=0x%llx allproc=0x%llx",
            (unsigned long long)KERNEL_ADDRESS_DATA_BASE, (unsigned long long)KERNEL_ADDRESS_ALLPROC);
    {
        // Read-only layout check of the proc struct that libhijacker would walk, via the payload SDK.
        const intptr_t kp = kernel_get_proc(pid);
        sky_log("probe3: kernel_get_proc(%d)=0x%llx", pid, (unsigned long long)kp);
        if (kp) {
            sky_log("probe3: pid at +0xbc = %d (expect %d)", (int)kernel_getint(kp + 0xbc), pid);
            sky_log("probe3: ucred at +0x40 = 0x%llx, sdk ucred = 0x%llx",
                    (unsigned long long)kernel_getlong(kp + 0x40), (unsigned long long)kernel_get_proc_ucred(pid));
            sky_log("probe3: fd at +0x48 = 0x%llx, sdk filedesc = 0x%llx",
                    (unsigned long long)kernel_getlong(kp + 0x48), (unsigned long long)kernel_get_proc_filedesc(pid));
            char tid[24] = {0};
            kernel_copyout(kp + 0x470, tid, sizeof tid - 1);
            sky_log("probe3: titleid at +0x470 = '%.16s' (expect CUSA00697)", tid);
            sky_log("probe3: word at +0x3e8 (shared object ptr) = 0x%llx", (unsigned long long)kernel_getlong(kp + 0x3e8));
        }
    }
    const size_t lh_allproc = offsets::allproc();
    sky_log("probe: libhijacker allproc offset=0x%llx (all f's = unsupported firmware)", (unsigned long long)lh_allproc);
    if (lh_allproc == (size_t)-1) {
        sky_log("probe: this libhijacker has no offsets for this firmware; not calling getHijacker (it would panic the console)");
        return -1;
    }
    sky_log("open: Hijacker::getHijacker(%d) ...", pid);
    g.hj = Hijacker::getHijacker(pid);
    if (!g.hj) { sky_log("open: getHijacker(%d) returned null", pid); return -1; }
    sky_log("open: getHijacker ok");
    SharedLib *eboot = g.hj->getEboot();
    if (!eboot) { sky_log("open: getEboot returned null"); g.hj = nullptr; return -1; }
    const SharedLibSection *txt = eboot->getTextSection();
    const SharedLibSection *dat = eboot->getDataSection();
    if (!txt || !dat || !eboot->getMetaData()) { sky_log("open: eboot text/data/metadata missing"); g.hj = nullptr; return -1; }
    sky_log("open: text 0x%llx..0x%llx data 0x%llx..0x%llx", (unsigned long long)txt->start(),
            (unsigned long long)txt->end(), (unsigned long long)dat->start(), (unsigned long long)dat->end());
    g.pid = pid;
    g.suspended = false;
    g.live = false;
    g.taken_code = g.taken_data = 0;
    *out = sky_proc{nullptr, p_alloc_code, p_alloc_data, p_write, p_read, p_find, p_sym, p_susp, p_res};
    return 0;
}

extern "C" void sky_hijack_close(void) {
    if (g.hj && g.suspended) p_res(nullptr);                      // never leave the game stopped
    g.hj = nullptr;
    g.live = false;
    g.pid = 0;
}

#endif  // SKY_HAVE_PS5_SDK
