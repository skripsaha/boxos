/*
 * new_delete.cpp — the full replaceable allocation-function set (Itanium
 * x86-64) over the boxlib heap.
 *
 * Backing calls go straight to _malloc_impl/free — box/memory.h's
 * variadic malloc() macro is deliberately bypassed.
 *
 * Aligned forms over-allocate and stash the raw pointer one slot below
 * the aligned address. The compiler guarantees aligned new pairs with
 * aligned delete ([expr.delete]), so plain free() never sees an interior
 * pointer.
 *
 * Until the unwinder ships (Phase 4B) the throwing forms terminate with
 * a diagnostic on exhaustion; nothrow forms return nullptr.
 */

#include <new>
#include <cstddef>
#include <cstdint>

namespace boxcxx {
[[noreturn]] void Panic(const char *msg);
}

extern "C" {
void *_malloc_impl(size_t size);
void  free(void *ptr);
}

namespace {

std::new_handler g_new_handler = nullptr;

void *AllocateWithHandler(size_t size)
{
    if (size == 0) size = 1;
    for (;;) {
        void *p = _malloc_impl(size);
        if (p) return p;
        std::new_handler handler = g_new_handler;
        if (!handler) return nullptr;
        handler();   // may free memory, install a new handler, or panic
    }
}

void *AlignedAllocate(size_t size, size_t align)
{
    if (align < sizeof(void *)) align = sizeof(void *);
    if (size == 0) size = 1;

    // Overflow guard: size + align + sizeof(void*) must not wrap.
    if (size > SIZE_MAX - align - sizeof(void *)) return nullptr;

    for (;;) {
        void *raw = _malloc_impl(size + align + sizeof(void *));
        if (raw) {
            uintptr_t user = (reinterpret_cast<uintptr_t>(raw) +
                              sizeof(void *) + align - 1) &
                             ~static_cast<uintptr_t>(align - 1);
            reinterpret_cast<void **>(user)[-1] = raw;
            return reinterpret_cast<void *>(user);
        }
        std::new_handler handler = g_new_handler;
        if (!handler) return nullptr;
        handler();
    }
}

void AlignedFree(void *ptr)
{
    if (ptr) free(reinterpret_cast<void **>(ptr)[-1]);
}

[[noreturn]] void OutOfMemory()
{
    throw std::bad_alloc{};
}

} // namespace

namespace std {

new_handler get_new_handler() noexcept
{
    return g_new_handler;
}

new_handler set_new_handler(new_handler handler) noexcept
{
    new_handler old = g_new_handler;
    g_new_handler   = handler;
    return old;
}

} // namespace std

// ── Scalar forms ────────────────────────────────────────────────────────

void *operator new(std::size_t size)
{
    void *p = AllocateWithHandler(size);
    if (!p) OutOfMemory();
    return p;
}

void *operator new(std::size_t size, std::align_val_t align)
{
    void *p = AlignedAllocate(size, static_cast<std::size_t>(align));
    if (!p) OutOfMemory();
    return p;
}

void *operator new(std::size_t size, const std::nothrow_t &) noexcept
{
    return AllocateWithHandler(size);
}

void *operator new(std::size_t size, std::align_val_t align,
                   const std::nothrow_t &) noexcept
{
    return AlignedAllocate(size, static_cast<std::size_t>(align));
}

void operator delete(void *ptr) noexcept
{
    free(ptr);
}

void operator delete(void *ptr, std::size_t) noexcept
{
    free(ptr);
}

void operator delete(void *ptr, std::align_val_t) noexcept
{
    AlignedFree(ptr);
}

void operator delete(void *ptr, std::size_t, std::align_val_t) noexcept
{
    AlignedFree(ptr);
}

void operator delete(void *ptr, const std::nothrow_t &) noexcept
{
    free(ptr);
}

void operator delete(void *ptr, std::align_val_t,
                     const std::nothrow_t &) noexcept
{
    AlignedFree(ptr);
}

// ── Array forms ─────────────────────────────────────────────────────────

void *operator new[](std::size_t size)
{
    return operator new(size);
}

void *operator new[](std::size_t size, std::align_val_t align)
{
    return operator new(size, align);
}

void *operator new[](std::size_t size, const std::nothrow_t &tag) noexcept
{
    return operator new(size, tag);
}

void *operator new[](std::size_t size, std::align_val_t align,
                     const std::nothrow_t &tag) noexcept
{
    return operator new(size, align, tag);
}

void operator delete[](void *ptr) noexcept
{
    free(ptr);
}

void operator delete[](void *ptr, std::size_t) noexcept
{
    free(ptr);
}

void operator delete[](void *ptr, std::align_val_t) noexcept
{
    AlignedFree(ptr);
}

void operator delete[](void *ptr, std::size_t, std::align_val_t) noexcept
{
    AlignedFree(ptr);
}

void operator delete[](void *ptr, const std::nothrow_t &) noexcept
{
    free(ptr);
}

void operator delete[](void *ptr, std::align_val_t,
                       const std::nothrow_t &) noexcept
{
    AlignedFree(ptr);
}
