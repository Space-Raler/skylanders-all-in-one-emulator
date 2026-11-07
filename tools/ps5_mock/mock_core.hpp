// MOCK of the etaHEN-SDK API surface proc_etahen.cpp uses. Signatures are copied from
// etaHEN-SDK/include/{hijacker/hijacker.hpp,hijacker/allocator.hpp,kernel/rtld.hpp,nid.hpp,util.hpp}.
// It exists only so `tools/syntax_check_ps5.sh` can type-check our code with a host g++ (the real headers
// need clang + the FreeBSD/PS5 payload SDK). It proves nothing about linking or runtime behaviour.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <elf.h>
#include <string.h>
static constexpr size_t NID_LENGTH = 11;
union Nid {
    char str[NID_LENGTH + 1];
    Nid() noexcept = default;
    explicit constexpr Nid(const char *n) noexcept : str{} { for (size_t i = 0; i < NID_LENGTH; i++) str[i] = n[i]; }
};
namespace nid { static inline constexpr Nid usleep{"QcteRwbsnV0"}; }
template <typename T> class UniquePtr {
    T *ptr;
public:
    explicit UniquePtr() : ptr(nullptr) {}
    UniquePtr(decltype(nullptr)) : ptr(nullptr) {}
    UniquePtr(T *p) : ptr(p) {}
    UniquePtr(const UniquePtr &) = delete;
    UniquePtr &operator=(const UniquePtr &) = delete;
    UniquePtr(UniquePtr &&r) noexcept : ptr(r.ptr) { r.ptr = nullptr; }
    UniquePtr &operator=(UniquePtr &&r) noexcept { delete ptr; ptr = r.ptr; r.ptr = nullptr; return *this; }
    ~UniquePtr() { delete ptr; }
    explicit operator bool() const { return ptr != nullptr; }
    T *operator->() const { return ptr; }
    T *get() const { return ptr; }
};
template <typename T> class UniquePtr<T[]> {
    T *ptr;
public:
    UniquePtr(decltype(nullptr)) : ptr(nullptr) {}
    UniquePtr(T *p) : ptr(p) {}
    UniquePtr(UniquePtr &&r) noexcept : ptr(r.ptr) { r.ptr = nullptr; }
    ~UniquePtr() { delete[] ptr; }
    explicit operator bool() const { return ptr; }
    T &operator[](size_t i) const { return ptr[i]; }
};
template <typename T> struct Array {
    T *b = nullptr; size_t n = 0;
    T *begin() const { return b; }
    T *end() const { return b + n; }
};
class SharedLibSection {
public:
    uintptr_t start() const; uintptr_t end() const; size_t sectionLength() const;
};
class RtldMeta;
namespace rtld { class ElfSymbolTable { public: ssize_t getSymbolIndex(const Nid &nid) const; }; }
class RtldMeta {
public:
    const rtld::ElfSymbolTable &getSymbolTable() const;
    const Array<Elf64_Rela> &getPltTable() const;
};
class SharedLib {
public:
    const SharedLibSection *getTextSection() const;
    const SharedLibSection *getDataSection() const;
    RtldMeta *getMetaData() const;
    uintptr_t imagebase() const;
};
class StringView;
class Hijacker {
public:
    static UniquePtr<Hijacker> getHijacker(int pid);
    SharedLib *getEboot() const;
    uintptr_t getLibKernelAddress(const Nid &fname) const { return getFunctionAddress(nullptr, fname); }
    uintptr_t getFunctionAddress(const SharedLib *lib, const Nid &fname) const noexcept;
    UniquePtr<uint8_t[]> read(uintptr_t src, size_t size);
    bool read(uintptr_t src, void *dst, size_t size);
    template <typename T> T read(uintptr_t vaddr);
    bool write(uintptr_t vaddr, const void *src, size_t size);
    template <typename T> bool write(uintptr_t vaddr, const T &value);
};
