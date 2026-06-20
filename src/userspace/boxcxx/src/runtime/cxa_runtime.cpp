/*
 * cxa_runtime.cpp — Itanium C++ ABI runtime core for BoxOS userspace.
 *
 *   - __cxa_atexit / __cxa_finalize   (static destructor registry)
 *   - __cxa_guard_acquire/release/abort (function-local static init)
 *   - __cxa_pure_virtual / __cxa_deleted_virtual
 *   - std::terminate machinery + key functions of <exception>/<new> classes
 *
 * Multiple strands may share a cabin (strand_spawn). The static-dtor registry
 * (__cxa_atexit) stays process-global — statics have process duration and are
 * drained once at exit. thread_local destructors (__cxa_thread_atexit) are
 * PER-STRAND: their registry head lives in each strand's own neg-TLS, so every
 * strand drains exactly its own thread-storage objects with no lock. The
 * function-local-static guards use an atomic in-progress byte so concurrent
 * first-touch by two strands still runs the initializer exactly once.
 */

#include <exception>
#include <new>
#include <cstddef>
#include <cstdint>

#include "box/print.h"
#include "box/system.h"
#include "box/sync.h"               // yield() — guard loser back-off
#include "box/cxx/tls_strand.h"     // __boxcxx_thread_storage_exit decl
#include "box/core/strand_self.h"   // strand_self() — guard owner token

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

// thread_local destructors — Itanium __cxa_thread_atexit. PER-STRAND: the list
// head lives in this strand's own neg-TLS (zeroed by the kernel for a spawned
// strand, by tls_init for main), so each strand owns an independent LIFO list
// with zero locking. Nodes are heap-allocated (malloc is strand-safe). Drained
// by __boxcxx_thread_storage_exit when the strand ends — for main strand, that
// call sits in __cxa_finalize BEFORE the static dtors ([basic.start.term]:
// thread storage duration ends before static storage duration).
namespace {
struct ThreadExitNode {
    void (*fn)(void *);
    void           *arg;
    ThreadExitNode *next;
};
} // namespace

static thread_local ThreadExitNode *g_thread_exit_head = nullptr;

extern "C" int __cxa_thread_atexit(void (*fn)(void *), void *arg, void *dso)
{
    (void)dso;
    if (!fn) return -1;

    auto *node = static_cast<ThreadExitNode *>(_malloc_impl(sizeof(ThreadExitNode)));
    if (!node) return -1;

    node->fn   = fn;
    node->arg  = arg;
    node->next = g_thread_exit_head;   // LIFO push onto this strand's head
    g_thread_exit_head = node;
    return 0;
}

extern "C" void __boxcxx_thread_storage_exit(void)
{
    // Pop LIFO — reverse of registration order. A dtor that registers another
    // thread_local re-pushes onto the head, so the loop re-reads it and the
    // late registration is honoured before the list drains.
    while (g_thread_exit_head) {
        ThreadExitNode *node = g_thread_exit_head;
        g_thread_exit_head   = node->next;
        node->fn(node->arg);
        free(node);
    }
}

extern "C" void __cxa_finalize(void *dso)
{
    (void)dso;   // NULL → run everything (only mode for static binaries)

    // Main strand's thread_local destructors run first (thread storage duration
    // ends before static), then the process-global statics in reverse
    // registration order. Handlers registered DURING the static drain (a dtor
    // constructing another static) land at the tail and are picked up because
    // the loop re-reads the count.
    __boxcxx_thread_storage_exit();

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
// implementation-defined. The whole word is operated on atomically so a
// concurrent first-touch by two strands resolves to exactly one initializer:
//
//   bit  0     (byte 0)    GUARD_DONE        — initialized (ABI-mandated byte)
//   bit  8     (byte 1)    GUARD_BUSY        — an initializer is running
//   bits 32-63 (bytes 4-7) owner strand id   — who is running it (recursion)
//
// The winner CASes BUSY|owner in one atomic op (no torn owner window). A loser
// whose own strand id matches the owner is a genuine recursive re-entry (UB per
// [stmt.dcl]) and panics; any other loser spins until DONE, or — if the
// initializer threw (abort clears BUSY without setting DONE) — retries to become
// the initializer itself. Single-thread behaviour is unchanged: the very first
// acquire always wins the CAS, and self-recursion is still detected.

namespace {
constexpr uint64_t GUARD_DONE = 0x1ull;        // byte 0
constexpr uint64_t GUARD_BUSY = 0x100ull;      // byte 1
constexpr int      GUARD_OWNER_SHIFT = 32;

inline uint64_t guard_owner_bits(uint32_t self)
{
    return static_cast<uint64_t>(self) << GUARD_OWNER_SHIFT;
}
} // namespace

extern "C" int __cxa_guard_acquire(uint64_t *guard)
{
    const uint32_t self = strand_self();   // distinct per strand; cabin pid for main

    for (;;) {
        uint64_t cur = __atomic_load_n(guard, __ATOMIC_ACQUIRE);
        if (cur & GUARD_DONE)
            return 0;                       // already initialized

        if (cur & GUARD_BUSY) {
            uint32_t owner = static_cast<uint32_t>(cur >> GUARD_OWNER_SHIFT);
            if (owner == self) {
                // Same strand re-entered its own in-progress init — real UB.
                boxcxx::Panic("recursive initialization of function-local static");
            }
            // Another strand is initializing: back off and re-poll.
            yield();
            continue;
        }

        // Claim it: publish BUSY + owner in a single atomic step.
        uint64_t want = GUARD_BUSY | guard_owner_bits(self);
        if (__atomic_compare_exchange_n(guard, &cur, want, false,
                                        __ATOMIC_ACQUIRE, __ATOMIC_ACQUIRE)) {
            return 1;                       // we are the initializer
        }
        // Lost the race (cur reloaded) — loop and re-evaluate.
    }
}

extern "C" void __cxa_guard_release(uint64_t *guard)
{
    // Initialized: clear BUSY/owner, set DONE. Release so the constructed object
    // is visible to any strand that observes DONE.
    __atomic_store_n(guard, GUARD_DONE, __ATOMIC_RELEASE);
}

extern "C" void __cxa_guard_abort(uint64_t *guard)
{
    // Initializer threw: clear BUSY/owner, leave DONE unset so a waiting strand
    // re-attempts the initialization.
    __atomic_store_n(guard, 0ull, __ATOMIC_RELEASE);
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
