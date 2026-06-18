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
#include <memory>
#include <memory_resource>
#include <new>
#include <type_traits>

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

// Snapshot of the whole-heap counters (thread-safe in the boxlib heap).
inline heap_stats_t stats() noexcept
{
    heap_stats_t s{};
    heap_get_stats(&s);
    return s;
}

// Number of live (allocated) blocks carrying `tag_name`.
inline std::size_t count(const char *tag_name) noexcept { return heap_count_tag(tag_name); }

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

#endif  // BOXCXX_BOX_HEAP_H
