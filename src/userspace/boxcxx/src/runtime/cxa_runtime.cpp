
#include <exception>
#include <new>
#include <cstddef>
#include <cstdint>
#include <__bits/c_terminate>

#include "box/print.h"
#include "box/system.h"
#include "box/sync.h"
#include "box/cxx/tls_strand.h"
#include "box/core/strand_self.h"

extern "C" {
void *malloc(size_t size);
void *realloc(void *ptr, size_t size);
void  free(void *ptr);
int  *__boxcxx_uncaught_count();
}

namespace boxcxx {

[[noreturn]] void Panic(const char *msg)
{
    printf("[boxcxx] FATAL: %s\n", msg);
    std::abort();
}

}


namespace {

std::terminate_handler g_terminate_handler = nullptr;

}

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
        boxcxx::Panic("terminate handler returned");
    }
    boxcxx::Panic("std::terminate() called");
}

int uncaught_exceptions() noexcept
{
    return *__boxcxx_uncaught_count();
}


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

}

extern "C" [[noreturn]] void __cxa_throw_bad_array_new_length()
{
    throw std::bad_array_new_length{};
}


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

}

extern "C" int __cxa_atexit(void (*fn)(void *), void *arg, void *dso)
{
    (void)dso;
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

namespace {
struct ThreadExitNode {
    void (*fn)(void *);
    void           *arg;
    ThreadExitNode *next;
};
}

static thread_local ThreadExitNode *g_thread_exit_head = nullptr;

extern "C" int __cxa_thread_atexit(void (*fn)(void *), void *arg, void *dso)
{
    (void)dso;
    if (!fn) return -1;

    auto *node = static_cast<ThreadExitNode *>(malloc(sizeof(ThreadExitNode)));
    if (!node) return -1;

    node->fn   = fn;
    node->arg  = arg;
    node->next = g_thread_exit_head;
    g_thread_exit_head = node;
    return 0;
}

extern "C" void __boxcxx_thread_storage_exit(void)
{
    while (g_thread_exit_head) {
        ThreadExitNode *node = g_thread_exit_head;
        g_thread_exit_head   = node->next;
        node->fn(node->arg);
        free(node);
    }
}

extern "C" void __cxa_finalize(void *dso)
{
    (void)dso;

    __boxcxx_thread_storage_exit();

    while (g_atexit_count > 0) {
        AtExitEntry entry = g_atexit_entries[--g_atexit_count];
        entry.fn(entry.arg);
    }

    free(g_atexit_entries);
    g_atexit_entries = nullptr;
    g_atexit_cap     = 0;
}


namespace {
constexpr uint64_t GUARD_DONE = 0x1ull;
constexpr uint64_t GUARD_BUSY = 0x100ull;
constexpr int      GUARD_OWNER_SHIFT = 32;

inline uint64_t guard_owner_bits(uint32_t self)
{
    return static_cast<uint64_t>(self) << GUARD_OWNER_SHIFT;
}
}

extern "C" int __cxa_guard_acquire(uint64_t *guard)
{
    const uint32_t self = strand_self();

    for (;;) {
        uint64_t cur = __atomic_load_n(guard, __ATOMIC_ACQUIRE);
        if (cur & GUARD_DONE)
            return 0;

        if (cur & GUARD_BUSY) {
            uint32_t owner = static_cast<uint32_t>(cur >> GUARD_OWNER_SHIFT);
            if (owner == self) {
                boxcxx::Panic("recursive initialization of function-local static");
            }
            yield();
            continue;
        }

        uint64_t want = GUARD_BUSY | guard_owner_bits(self);
        if (__atomic_compare_exchange_n(guard, &cur, want, false,
                                        __ATOMIC_ACQUIRE, __ATOMIC_ACQUIRE)) {
            return 1;
        }
    }
}

extern "C" void __cxa_guard_release(uint64_t *guard)
{
    __atomic_store_n(guard, GUARD_DONE, __ATOMIC_RELEASE);
}

extern "C" void __cxa_guard_abort(uint64_t *guard)
{
    __atomic_store_n(guard, 0ull, __ATOMIC_RELEASE);
}


extern "C" [[noreturn]] void __cxa_pure_virtual()
{
    boxcxx::Panic("pure virtual function call");
}

extern "C" [[noreturn]] void __cxa_deleted_virtual()
{
    boxcxx::Panic("deleted virtual function call");
}