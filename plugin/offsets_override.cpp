// Replaces libhijacker's offsets.o (allproc, root_vnode, security/qa/utoken flags, getSystemSwVersion).
// libhijacker's own table stops at firmware 10.60 and answers -1 beyond that, which makes
// Hijacker::getHijacker() read kernel memory at (kernel_base - 1) and panic the console.
// Here every offset is derived from the payload SDK, which knows the running firmware:
//     offset = SDK kernel address - SDK kernel data base
// If the SDK has no value (0) or something implausible, we return -1, exactly like the original
// "unsupported firmware" answer, and the caller in proc_hijacker.cpp refuses to attach.
//
// All six symbols that offsets.o defines are defined here, so the linker never pulls offsets.o from
// libhijacker.a. If one were missing and referenced, the link would fail with a duplicate-symbol error
// (loud), not silently use the old table.
#ifdef SKY_HAVE_PS5_SDK

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/sysctl.h>
#include <ps5/kernel.h>

#include "offsets.hpp"
#include "sky_log.h"

uint32_t getSystemSwVersion() {
    static uint32_t version;
    if (version != 0) return version;
    uint32_t v = 0;
    size_t size = sizeof v;
    sysctlbyname("kern.sdk_version", &v, &size, nullptr, 0);
    version = v;
    return version;
}

namespace {

size_t from_sdk(intptr_t addr, const char *what) {
    const intptr_t base = KERNEL_ADDRESS_DATA_BASE;
    if (!base || !addr || addr < base || (addr - base) > 0x10000000) {
        sky_log("offsets: SDK has no usable %s address for this firmware (base=0x%llx addr=0x%llx)", what,
                (unsigned long long)base, (unsigned long long)addr);
        return (size_t)-1;
    }
    return (size_t)(addr - base);
}

}  // namespace

namespace offsets {

size_t allproc() {
    static size_t v;
    if (!v) v = from_sdk(KERNEL_ADDRESS_ALLPROC, "allproc");
    return v;
}
size_t root_vnode() {
    static size_t v;
    if (!v) v = from_sdk(KERNEL_ADDRESS_ROOTVNODE, "rootvnode");
    return v;
}
size_t security_flags() {
    static size_t v;
    if (!v) v = from_sdk(KERNEL_ADDRESS_SECURITY_FLAGS, "security_flags");
    return v;
}
size_t qa_flags() {
    static size_t v;
    if (!v) v = from_sdk(KERNEL_ADDRESS_QA_FLAGS, "qa_flags");
    return v;
}
size_t utoken_flags() {
    static size_t v;
    if (!v) v = from_sdk(KERNEL_ADDRESS_UTOKEN_FLAGS, "utoken_flags");
    return v;
}

}  // namespace offsets

#endif  // SKY_HAVE_PS5_SDK
