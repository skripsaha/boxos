// boxcxx — box::tagged_resource + box::heap
//   (the C++ face of the BoxOS tagged heap — per-subsystem memory accounting)
//
// The BoxOS heap labels every allocation with a tag, so memory can be accounted
// by subsystem ("render", "physics", "ml:weights", ...). This header dresses the
// boxlib C surface (box/memory.h) in two idiomatic pieces:
//
//   * box::tagged_resource — a std::pmr::memory_resource that routes every
//     allocation through malloc_tagged(bytes, tag). Bind a pmr container to it
//     and the whole container's memory is accounted under one heap tag:
//
//         box::tagged_resource res("render:mesh");
//         std::pmr::vector<Vertex> verts(&res);   // every buffer tagged
//         box::heap::count("render:mesh");         // how many live blocks
//
//   * box::heap — free queries over the live heap (stats / count / for_each /
//     for_each_tagged) plus box::heap::tag, a handle to one tag with count() /
//     bytes() / for_each() and a RAII watch() leak guard:
//
//         box::heap::tag t("render:mesh");
//         { auto g = t.watch(); load(); unload(); }   // reports if it grew
//
// This is a box:: extension, not part of std. The tag name string must outlive
// every use (a string literal is the natural choice — malloc_tagged interns a
// copy, but tagged_resource holds the pointer for its lifetime). The heap tag
// registry is bounded (HEAP_TAG_CAP); past it, tagging degrades to untagged
// allocation rather than failing.
//
// ‼ for_each / for_each_tagged invoke the callback with the heap lock HELD — the
// callback MUST NOT allocate, free, or call back into box::heap (no malloc / new /
// free / delete / count / bytes / for_each / stats, directly or via a container):
// each re-takes the same non-recursive heap lock and would deadlock. The callback
// may read, sum, and copy out. This mirrors and extends the C contract.
#ifndef BOXCXX_BOX_HEAP_H
#define BOXCXX_BOX_HEAP_H

#include <cstddef>
#include <cstdint>
#include <format>          // std::formatter<box::heap::stats>
#include <limits>          // widest-unsigned width for that formatter's buffer
#include <memory>
#include <memory_resource>
#include <new>
#include <type_traits>
#include <utility>         // std::forward (box::heap::make)

#include "box/cxx/error.h"
#include "box/memory.h"
#include "box/print.h"

namespace box {

// ── box::tagged_resource — pmr memory_resource tagging every allocation ──────
class tagged_resource : public std::pmr::memory_resource {
    const char *tag_;

public:
    explicit tagged_resource(const char *tag) noexcept : tag_(tag)
    {
        heap_register_tag(tag);  // intern up front so queries work before use
    }
    tagged_resource(const tagged_resource &)            = delete;
    tagged_resource &operator=(const tagged_resource &) = delete;

    const char *tag() const noexcept { return tag_; }

protected:
    void *do_allocate(std::size_t bytes, std::size_t align) override
    {
        // malloc_tagged guarantees max_align_t (16-byte) alignment, which covers
        // every fundamental type. Over-aligned requests reserve slack for the
        // alignment plus a back-pointer to the original block for do_deallocate.
        if (align <= alignof(std::max_align_t)) {
            void *p = malloc_tagged(bytes, tag_);
            if (!p) throw std::bad_alloc{};
            return p;
        }
        const std::size_t slack = align + sizeof(void *);
        const std::size_t total = bytes + slack;
        if (total < bytes) throw std::bad_alloc{};  // bytes + slack overflowed
        void *raw = malloc_tagged(total, tag_);
        if (!raw) throw std::bad_alloc{};
        std::uintptr_t base = reinterpret_cast<std::uintptr_t>(raw) + sizeof(void *);
        std::uintptr_t aligned =
            (base + align - 1) & ~(static_cast<std::uintptr_t>(align) - 1);
        reinterpret_cast<void **>(aligned)[-1] = raw;  // stash original for free
        return reinterpret_cast<void *>(aligned);
    }

    void do_deallocate(void *p, std::size_t, std::size_t align) override
    {
        if (!p) return;
        if (align <= alignof(std::max_align_t)) free(p);
        else free(reinterpret_cast<void **>(p)[-1]);
    }

    bool do_is_equal(const std::pmr::memory_resource &o) const noexcept override
    {
        return this == &o;  // conservative: distinct resources are not interchangeable
    }
};

namespace heap {

// ── tag registry lookup (Ф25d) ──────────────────────────────────────────────
// HEAP_TAG_NONE as a typed constant, plus lookup-WITHOUT-register (the query
// twin of box::heap::tag's registering ctor) and the id→name reverse.
inline constexpr std::uint8_t tag_none = HEAP_TAG_NONE;
// The id of an ALREADY-registered tag, or tag_none if it was never registered —
// does NOT create one (that is box::heap::tag's ctor / heap_register_tag).
inline std::uint8_t tag_id(const char *name) noexcept { return heap_lookup_tag(name); }
// The name interned for a tag id, or nullptr for tag_none / an unknown id.
inline const char  *tag_name(std::uint8_t id) noexcept { return heap_tag_name(id); }

// ── box::heap::stats — a typed view of the whole-heap counters (Ф25d) ────────
// Mirrors box::memtag::stats: a value view with named accessors over the raw
// heap_stats_t. A LIVE snapshot comes from box::heap::counters() — NOT from
// default-constructing this view (a default stats is all-zero). The explicit POD
// ctor lets callers (and tests) build a view over synthetic counters.
class stats {
    heap_stats_t s_{};

public:
    stats() noexcept = default;
    explicit stats(const heap_stats_t &s) noexcept : s_(s) {}
    const heap_stats_t &raw() const noexcept { return s_; }

    std::size_t heap_bytes()   const noexcept { return s_.heap_used; }        // bytes sbrk'd
    std::size_t in_use_bytes() const noexcept { return s_.total_allocated; }  // live (allocated)
    std::size_t free_bytes()   const noexcept { return s_.total_free; }       // in free blocks
    // heap_used minus what is accounted to live + free blocks (allocator
    // bookkeeping overhead); saturating, never negative.
    std::size_t overhead_bytes() const noexcept
    {
        std::size_t accounted = s_.total_allocated + s_.total_free;
        return s_.heap_used > accounted ? s_.heap_used - accounted : 0;
    }
    std::uint32_t live_blocks()  const noexcept { return s_.alloc_count; }
    std::uint32_t free_blocks()  const noexcept { return s_.free_count; }
    std::uint32_t malloc_calls() const noexcept { return s_.malloc_calls; }
    std::uint32_t free_calls()   const noexcept { return s_.free_calls; }
};

// Snapshot the whole-heap counters as a typed view (thread-safe in the boxlib
// heap; infallible — heap_get_stats always fills the struct).
inline stats counters() noexcept
{
    heap_stats_t s{};
    heap_get_stats(&s);
    return stats(s);
}

// Number of live (allocated) blocks carrying `tag_name`.
inline std::size_t count(const char *tag_name) noexcept { return heap_count_tag(tag_name); }

// ── fallible allocation carrying the real cause (Ф23c) ──────────────────────
// allocate / reallocate hand back a box::result: the pointer on success, or the
// EXACT kernel cause on failure (heap exhausted, corruption, overflow, …). The
// cause is this strand's own — heap_get_last_error() reads a per-strand cell, so
// a sibling strand's concurrent success can never mask this call's failure.
// These never throw; use them where bad_alloc is unwanted (operator new and
// box::tagged_resource keep the throwing path). last_error() is the same cell as
// a box::error, for a query between operations.

// The calling strand's last heap-operation cause.
inline error last_error() noexcept { return error{heap_get_last_error()}; }

// Allocate `bytes` (must be > 0; 0 is rejected as invalid_argument — an empty
// allocation has no pointer to hand back). On failure the result carries the
// real cause.
inline result<void *> allocate(std::size_t bytes)
{
    if (bytes == 0) return std::unexpected(error{errc::invalid_argument});
    void *p = ::_malloc_impl(bytes);
    if (p) return p;
    ::error_t e = heap_get_last_error();
    return std::unexpected(error{e != OK ? e : static_cast<::error_t>(ERR_NO_MEMORY)});
}

// Allocate `bytes` (> 0) accounted under `tag` (see box::tagged_resource).
inline result<void *> allocate(std::size_t bytes, const char *tag)
{
    if (bytes == 0) return std::unexpected(error{errc::invalid_argument});
    void *p = ::malloc_tagged(bytes, tag);
    if (p) return p;
    ::error_t e = heap_get_last_error();
    return std::unexpected(error{e != OK ? e : static_cast<::error_t>(ERR_NO_MEMORY)});
}

// Resize `p` to `bytes`. bytes == 0 frees `p`: a clean free yields a nullptr
// VALUE (a deallocation, not an error), but a free that detects a double-free
// or corruption surfaces that cause. On a genuine resize failure the result
// carries the cause and `p` is left untouched (the standard realloc contract).
inline result<void *> reallocate(void *p, std::size_t bytes)
{
    if (bytes == 0) {
        // A deallocation, not an allocation. free(nullptr) is a no-op that
        // writes no cell, so don't read a stale cause for it; a real pointer
        // that free flags (double-free / corruption) IS surfaced.
        if (!p) return static_cast<void *>(nullptr);
        ::free(p);
        ::error_t e = heap_get_last_error();
        if (e != OK) return std::unexpected(error{e});
        return static_cast<void *>(nullptr);
    }
    void *q = ::realloc(p, bytes);
    if (q) return q;
    ::error_t e = heap_get_last_error();
    return std::unexpected(error{e != OK ? e : static_cast<::error_t>(ERR_NO_MEMORY)});
}

// ── box::heap::make<T> — a typed, heap-tagged allocation (Ф25d) ──────────────
// The typed counterpart of malloc_tagged: construct a T accounted under `tag`
// and own it through box::heap::tagged<T> (a unique_ptr whose deleter runs the
// destructor and frees the block). Returns box::result — the error arm carries
// the real allocation cause (Ф23 doctrine), never a thrown bad_alloc, so it
// composes with the fallible box::heap::allocate family. A constructor that
// throws propagates (a T-construction failure is a different domain from an
// allocation failure) after freeing the raw block.

// Stateless deleter for a malloc_tagged block: ~T then free (free is tag-
// agnostic — exactly what tagged_resource::do_deallocate does for this memory).
struct tagged_deleter {
    template <class T>
    void operator()(T *p) const noexcept
    {
        if (p) { p->~T(); ::free(p); }
    }
};

// An owning handle to a heap-tagged T (move-only, like std::unique_ptr).
template <class T>
using tagged = std::unique_ptr<T, tagged_deleter>;

template <class T, class... Args>
result<tagged<T>> make(const char *tag, Args &&...args)
{
    static_assert(alignof(T) <= alignof(std::max_align_t),
                  "box::heap::make<T>: over-aligned T (alignof > 16) — malloc_tagged only "
                  "guarantees max_align_t; use box::tagged_resource for over-aligned types");
    void *raw = ::malloc_tagged(sizeof(T), tag);
    if (!raw) {
        ::error_t e = heap_get_last_error();
        return std::unexpected(error{e != OK ? e : static_cast<::error_t>(ERR_NO_MEMORY)});
    }
    try {
        T *obj = ::new (raw) T(std::forward<Args>(args)...);
        return tagged<T>(obj);
    } catch (...) {
        ::free(raw);  // T's constructor threw: release the (un-constructed) storage, rethrow
        throw;
    }
}

// Visit every live block under `tag_name`. fn is called as fn(void* ptr,
// std::size_t size, const char* tag_name) for each. ‼ Runs under the heap lock —
// fn MUST NOT allocate, free, or query box::heap (see the file banner).
template <class Fn>
void for_each(const char *tag_name, Fn &&fn)
{
    HeapTagCallback thunk = [](void *ptr, size_t size, const char *name, void *ud) {
        (*static_cast<std::remove_reference_t<Fn> *>(ud))(ptr, size, name);
    };
    heap_iterate_tag(tag_name, thunk,
                     const_cast<void *>(static_cast<const void *>(std::addressof(fn))));
}

// Visit every live tagged block (any tag). Same lock contract as for_each.
template <class Fn>
void for_each_tagged(Fn &&fn)
{
    HeapTagCallback thunk = [](void *ptr, size_t size, const char *name, void *ud) {
        (*static_cast<std::remove_reference_t<Fn> *>(ud))(ptr, size, name);
    };
    heap_iterate_all_tagged(thunk,
                            const_cast<void *>(static_cast<const void *>(std::addressof(fn))));
}

// ── box::heap::tag — a handle to one heap tag: queries + a RAII leak guard ───
class tag {
    const char *name_;

public:
    explicit tag(const char *name) noexcept : name_(name) { heap_register_tag(name); }

    const char *name() const noexcept { return name_; }

    // This tag's registry id (heap::tag_none only if the registry was full when
    // the ctor tried to register), and whether it is registered. The ctor
    // registers, so registered() is normally true.
    std::uint8_t id() const noexcept { return heap_lookup_tag(name_); }
    bool         registered() const noexcept { return id() != tag_none; }

    // Live block count under this tag.
    std::size_t count() const noexcept { return heap_count_tag(name_); }

    // Total live bytes under this tag (summed over its blocks).
    std::size_t bytes() const
    {
        std::size_t total = 0;
        heap::for_each(name_, [&total](void *, std::size_t sz, const char *) { total += sz; });
        return total;
    }

    // Visit each live block under this tag. Same lock contract as heap::for_each.
    template <class Fn>
    void for_each(Fn &&fn) const
    {
        heap::for_each(name_, static_cast<Fn &&>(fn));
    }

    // RAII leak guard: records the live count at construction; on destruction
    // (unless dismissed) reports any blocks under the tag that are still live
    // beyond that baseline. Move-only; copy would double-report.
    class watch_guard {
        const char *name_;
        std::size_t base_;
        bool        armed_;

    public:
        explicit watch_guard(const char *name) noexcept
            : name_(name), base_(heap_count_tag(name)), armed_(true)
        {
        }
        watch_guard(watch_guard &&o) noexcept
            : name_(o.name_), base_(o.base_), armed_(o.armed_)
        {
            o.armed_ = false;
        }
        watch_guard(const watch_guard &)            = delete;
        watch_guard &operator=(const watch_guard &) = delete;
        watch_guard &operator=(watch_guard &&)      = delete;

        // Live blocks created under the tag since construction that are not yet
        // freed (0 if the count is back at or below the baseline).
        std::size_t leaked() const noexcept
        {
            std::size_t now = heap_count_tag(name_);
            return now > base_ ? now - base_ : 0;
        }

        // Disarm the destructor report (when retained allocations are expected).
        void dismiss() noexcept { armed_ = false; }

        ~watch_guard()
        {
            if (!armed_) return;
            std::size_t n = leaked();
            if (n)
                printf("[heap leak] tag '%s': %u allocation(s) still live at scope exit\n",
                       name_, static_cast<unsigned>(n));
        }
    };

    [[nodiscard]] watch_guard watch() const noexcept { return watch_guard(name_); }
};

}  // namespace heap
}  // namespace box

// ── std::formatter<box::heap::stats> — one greppable line, full std spec ─────
// Reads only the already-snapshotted POD copy — no for_each, no malloc/free, no
// heap lock re-entry (the file banner's lock contract is respected).
//
// The line is rendered into a stack buffer and handed to formatter<string_view>,
// which is where width, fill, align and precision come from. box::error reaches
// for std::string to do the same thing; this one must not, because a formatter
// that allocates to print the heap's own numbers would perturb what it reports.
template <>
struct std::formatter<box::heap::stats, char> : std::formatter<std::string_view, char> {
    auto format(const box::heap::stats &s, std::format_context &ctx) const
    {
        static constexpr char kFmt[] =
            "heap used={} in_use={} free={} live={} freeblk={} mallocs={} frees={}";
        // Derived, not guessed: the format string itself plus one widest
        // unsigned per field. Every placeholder is at least two characters, so
        // this over-counts; the count of fields is the one thing to keep in
        // step, and phase142 pins it by formatting an all-maximum record.
        static constexpr std::size_t kCap =
            sizeof kFmt + 7 * (std::numeric_limits<unsigned long long>::digits10 + 1);
        char buf[kCap];
        auto r = std::format_to_n(
            buf, (std::ptrdiff_t)sizeof buf, kFmt,
            s.heap_bytes(), s.in_use_bytes(), s.free_bytes(), s.live_blocks(),
            s.free_blocks(), s.malloc_calls(), s.free_calls());
        return std::formatter<std::string_view, char>::format(
            std::string_view(buf, (std::size_t)(r.out - buf)), ctx);
    }
};

#endif  // BOXCXX_BOX_HEAP_H
