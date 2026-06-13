/*
 * cxa_runtime.cpp — Itanium C++ ABI runtime core for BoxOS userspace.
 *
 *   - __cxa_atexit / __cxa_finalize   (static destructor registry)
 *   - __cxa_guard_acquire/release/abort (function-local static init)
 *   - __cxa_pure_virtual / __cxa_deleted_virtual
 *   - std::terminate machinery + key functions of <exception>/<new> classes
 *
 * Single thread of execution per cabin (BoxOS process model) — the guard
 * protocol still detects recursive initialization, which is the only
 * failure mode possible without threads.
 */

#include <exception>
#include <new>
#include <cstddef>
#include <cstdint>

#include "box/print.h"
#include "box/system.h"

extern "C" {
void *_malloc_impl(size_t size);
void *realloc(void *ptr, size_t size);
void  free(void *ptr);
int  *__boxcxx_uncaught_count();   // cxa_exception.cpp
}

namespace boxcxx {

[[noreturn]] void Panic(const char *msg)
{
    printf("[boxcxx] FATAL: %s\n", msg);
    exit(134u);   // 128 + SIGABRT by convention; BoxOS treats it as plain code
    __builtin_unreachable();
}

} // namespace boxcxx

// ── std::terminate ──────────────────────────────────────────────────────

namespace {

std::terminate_handler g_terminate_handler = nullptr;

} // namespace

namespace std {

terminate_handler set_terminate(terminate_handler handler) noexcept
{
    terminate_handler old = g_terminate_handler;
    g_terminate_handler   = handler;
    return old;
}

terminate_handler get_terminate() noexcept
{
    return g_terminate_handler;
}

void terminate() noexcept
{
    terminate_handler handler = g_terminate_handler;
    if (handler) {
        handler();
        // A terminate handler must not return — enforce it.
        boxcxx::Panic("terminate handler returned");
    }
    boxcxx::Panic("std::terminate() called");
}

int uncaught_exceptions() noexcept
{
    return *__boxcxx_uncaught_count();
}

// ── Key functions: vtables + what() for the base hierarchy ─────────────

exception::~exception() = default;

const char *exception::what() const noexcept
{
    return "std::exception";
}

bad_exception::~bad_exception() = default;

const char *bad_exception::what() const noexcept
{
    return "std::bad_exception";
}

bad_alloc::~bad_alloc() = default;

const char *bad_alloc::what() const noexcept
{
    return "std::bad_alloc";
}

bad_array_new_length::~bad_array_new_length() = default;

const char *bad_array_new_length::what() const noexcept
{
    return "std::bad_array_new_length";
}

const nothrow_t nothrow{};

} // namespace std

// Itanium ABI: emitted by the compiler at array-new sites whose element
// count could overflow the allocation size (e.g. make_unique<T[]>(n)).
extern "C" [[noreturn]] void __cxa_throw_bad_array_new_length()
{
    throw std::bad_array_new_length{};
}

// ── __cxa_atexit / __cxa_finalize ───────────────────────────────────────

// Normally supplied by crtbegin.o; BoxOS binaries are fully static single
// images, so the canonical "main program" handle is NULL. Referenced by
// compiler-emitted __cxa_atexit registration calls (hidden visibility —
// must live inside every link that contains C++ static dtors, i.e. here).
extern "C" {
__attribute__((visibility("hidden"))) void *__dso_handle = nullptr;
}

namespace {

struct AtExitEntry {
    void (*fn)(void *);
    void *arg;
};

AtExitEntry *g_atexit_entries = nullptr;
size_t       g_atexit_count   = 0;
size_t       g_atexit_cap     = 0;

} // namespace

extern "C" int __cxa_atexit(void (*fn)(void *), void *arg, void *dso)
{
    (void)dso;   // static linking — single image, no DSO handles
    if (!fn) return -1;

    if (g_atexit_count == g_atexit_cap) {
        size_t new_cap = g_atexit_cap ? g_atexit_cap * 2 : 32;
        void *grown = realloc(g_atexit_entries, new_cap * sizeof(AtExitEntry));
        if (!grown) return -1;
        g_atexit_entries = static_cast<AtExitEntry *>(grown);
        g_atexit_cap     = new_cap;
    }

    g_atexit_entries[g_atexit_count].fn  = fn;
    g_atexit_entries[g_atexit_count].arg = arg;
    g_atexit_count++;
    return 0;
}

// thread_local destructors — Itanium __cxa_thread_atexit. One thread per
// cabin, so this is a second registry drained BEFORE the static one
// ([basic.start.term]: thread storage duration ends first).
namespace {
AtExitEntry *g_thread_entries = nullptr;
size_t       g_thread_count   = 0;
size_t       g_thread_cap     = 0;
} // namespace

extern "C" int __cxa_thread_atexit(void (*fn)(void *), void *arg, void *dso)
{
    (void)dso;
    if (!fn) return -1;

    if (g_thread_count == g_thread_cap) {
        size_t new_cap = g_thread_cap ? g_thread_cap * 2 : 16;
        void *grown = realloc(g_thread_entries, new_cap * sizeof(AtExitEntry));
        if (!grown) return -1;
        g_thread_entries = static_cast<AtExitEntry *>(grown);
        g_thread_cap     = new_cap;
    }

    g_thread_entries[g_thread_count].fn  = fn;
    g_thread_entries[g_thread_count].arg = arg;
    g_thread_count++;
    return 0;
}

extern "C" void __cxa_finalize(void *dso)
{
    (void)dso;   // NULL → run everything (only mode for static binaries)

    // thread_local destructors first, then statics — both in reverse
    // registration order. Handlers registered DURING finalize (a dtor
    // constructing another static) land at the tail and are picked up
    // because the loops re-read the counts.
    while (g_thread_count > 0) {
        AtExitEntry entry = g_thread_entries[--g_thread_count];
        entry.fn(entry.arg);
    }
    free(g_thread_entries);
    g_thread_entries = nullptr;
    g_thread_cap     = 0;

    while (g_atexit_count > 0) {
        AtExitEntry entry = g_atexit_entries[--g_atexit_count];
        entry.fn(entry.arg);
    }

    free(g_atexit_entries);
    g_atexit_entries = nullptr;
    g_atexit_cap     = 0;
}

// ── Function-local static guards ────────────────────────────────────────
//
// Itanium ABI: 64-bit guard object; byte 0 = "initialized", the rest is
// implementation-defined. Byte 1 = boxcxx "initialization in progress"
// marker for recursion detection.

extern "C" int __cxa_guard_acquire(uint64_t *guard)
{
    unsigned char *bytes = reinterpret_cast<unsigned char *>(guard);
    if (bytes[0]) return 0;          // already initialized
    if (bytes[1]) {
        // C++ [stmt.dcl]: recursive re-entry during initialization is UB —
        // diagnose loudly instead of looping or corrupting.
        boxcxx::Panic("recursive initialization of function-local static");
    }
    bytes[1] = 1;
    return 1;
}

extern "C" void __cxa_guard_release(uint64_t *guard)
{
    unsigned char *bytes = reinterpret_cast<unsigned char *>(guard);
    bytes[0] = 1;
    bytes[1] = 0;
}

extern "C" void __cxa_guard_abort(uint64_t *guard)
{
    unsigned char *bytes = reinterpret_cast<unsigned char *>(guard);
    bytes[1] = 0;
}

// ── Vtable trap entries ─────────────────────────────────────────────────

extern "C" [[noreturn]] void __cxa_pure_virtual()
{
    boxcxx::Panic("pure virtual function call");
}

extern "C" [[noreturn]] void __cxa_deleted_virtual()
{
    boxcxx::Panic("deleted virtual function call");
}
