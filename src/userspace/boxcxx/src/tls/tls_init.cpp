/*
 * tls_init.cpp — local-exec TLS bootstrap for BoxOS C++ binaries.
 *
 * The kernel loader ignores PT_TLS; this runs as the very first
 * .init_array entry (constructor priority 101 — ahead of every default-
 * priority global constructor, which may itself touch thread_local).
 *
 * Layout contract with the linker (x86-64 TLS variant 2):
 *
 *      block:  [ .tdata copy | pad | .tbss zeros | pad | TCB ]
 *                ^0            ^AlignUp(tdata,64)         ^tp
 *      tp = block + AlignUp(memsz, 64);  fs:0 = tp (self pointer)
 *      var offset = PT_TLS offset − AlignUp(memsz, 64)   (negative)
 *
 * The 64-byte figure is pinned by kAnchor below: it forces PT_TLS
 * p_align == 64 so the linker and this code compute identical layouts.
 * Apps must not exceed alignas(64) on thread_local objects — the
 * cxxtest.elf link rule carries a readelf check for that.
 *
 * One thread of execution per cabin (BoxOS process model) — exactly one
 * TLS block for the process lifetime; freed by the kernel with the heap.
 */

#include <cstdint>
#include <cstddef>

#include "box/cpu.h"
#include "box/string.h"
#include "box/system.h"

extern "C" {
void *_malloc_impl(size_t size);

/* user.ld — .tdata load image and section sizes (symbol-value trick:
 * the "address" of the size symbols IS the size). */
extern const char __tdata_start[];
extern const char __tdata_size[];
extern const char __tbss_size[];
}

namespace boxcxx {
[[noreturn]] void Panic(const char *msg);
}

namespace {

// Pins PT_TLS p_align to 64 for every C++ binary (see header comment).
// Lives in .tbss; one byte + alignment padding per process.
[[gnu::used]] alignas(64) thread_local char kAnchor;

constexpr uint64_t AlignUp(uint64_t v, uint64_t a)
{
    return (v + a - 1) & ~(a - 1);
}

struct TlsControlBlock {
    void *self;       // fs:0 — address-of-thread-local sequences load this
    void *reserved;   // keeps the TCB 16 bytes, matches SysV expectations
};

} // namespace

extern "C" void __boxcxx_tls_bootstrap()
{
    const uint64_t tdata_size = (uint64_t)(uintptr_t)__tdata_size;
    const uint64_t tbss_size  = (uint64_t)(uintptr_t)__tbss_size;
    constexpr uint64_t kAlign = 64;

    const uint64_t tbss_off  = AlignUp(tdata_size, kAlign);
    const uint64_t memsz     = tbss_off + tbss_size;
    const uint64_t tp_off    = AlignUp(memsz, kAlign);
    const uint64_t total     = tp_off + sizeof(TlsControlBlock);

    void *raw = _malloc_impl(total + kAlign);
    if (!raw) boxcxx::Panic("TLS bootstrap: heap exhausted");

    uintptr_t block = AlignUp((uintptr_t)raw, kAlign);

    memcpy((void *)block, __tdata_start, tdata_size);
    memset((void *)(block + tdata_size), 0, tp_off - tdata_size);

    TlsControlBlock *tcb = (TlsControlBlock *)(block + tp_off);
    tcb->self     = tcb;
    tcb->reserved = nullptr;

    if (cpu_has_fsgsbase()) {
        __asm__ volatile("wrfsbase %0" :: "r"(tcb));
    } else {
        if (tls_set_fsbase((uint64_t)(uintptr_t)tcb) != 0)
            boxcxx::Panic("TLS bootstrap: SYSTEM_OP_TLS_FSBASE failed");
        /* The kernel installs the base at the next context restore —
         * force one so thread_local is live when we return. */
        yield();
    }

    /* First fs-relative access — faults here mean the base never landed. */
    kAnchor = 1;
}

/* Highest-priority constructor: runs before all default-priority global
 * ctors so they can safely construct thread_local state. */
[[gnu::constructor(101)]] static void BoxCxxTlsCtor()
{
    __boxcxx_tls_bootstrap();
}
