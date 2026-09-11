#ifndef BOXCXX_BOX_HEAP_H
#define BOXCXX_BOX_HEAP_H

#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <memory>
#include <memory_resource>
#include <new>
#include <type_traits>
#include <utility>

#include "box/cxx/error.h"
#include "box/memory.h"
#include "box/print.h"

namespace box {

class tagged_resource : public std::pmr::memory_resource {
    const char *tag_;

public:
    explicit tagged_resource(const char *tag) noexcept : tag_(tag)
    {
        heap_register_tag(tag);
    }
    tagged_resource(const tagged_resource &)            = delete;
    tagged_resource &operator=(const tagged_resource &) = delete;

    const char *tag() const noexcept { return tag_; }

protected:
    void *do_allocate(std::size_t bytes, std::size_t align) override
    {
        if (align <= alignof(std::max_align_t)) {
            void *p = malloc_tagged(bytes, tag_);
            if (!p) throw std::bad_alloc{};
            return p;
        }
        const std::size_t slack = align + sizeof(void *);
        const std::size_t total = bytes + slack;
        if (total < bytes) throw std::bad_alloc{};
        void *raw = malloc_tagged(total, tag_);
        if (!raw) throw std::bad_alloc{};
        std::uintptr_t base = reinterpret_cast<std::uintptr_t>(raw) + sizeof(void *);
        std::uintptr_t aligned =
            (base + align - 1) & ~(static_cast<std::uintptr_t>(align) - 1);
        reinterpret_cast<void **>(aligned)[-1] = raw;
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
        return this == &o;
    }
};

namespace heap {

inline constexpr std::uint8_t tag_none = HEAP_TAG_NONE;
inline std::uint8_t tag_id(const char *name) noexcept { return heap_lookup_tag(name); }
inline const char  *tag_name(std::uint8_t id) noexcept { return heap_tag_name(id); }

class stats {
    heap_stats_t s_{};

public:
    stats() noexcept = default;
    explicit stats(const heap_stats_t &s) noexcept : s_(s) {}
    const heap_stats_t &raw() const noexcept { return s_; }

    std::size_t heap_bytes()   const noexcept { return s_.heap_used; }
    std::size_t in_use_bytes() const noexcept { return s_.total_allocated; }
    std::size_t free_bytes()   const noexcept { return s_.total_free; }
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

inline stats counters() noexcept
{
    heap_stats_t s{};
    heap_get_stats(&s);
    return stats(s);
}

inline std::size_t count(const char *tag_name) noexcept { return heap_count_tag(tag_name); }


inline error last_error() noexcept { return error{heap_get_last_error()}; }

inline result<void *> allocate(std::size_t bytes)
{
    if (bytes == 0) return std::unexpected(error{errc::invalid_argument});
    void *p = ::malloc(bytes);
    if (p) return p;
    ::error_t e = heap_get_last_error();
    return std::unexpected(error{e != OK ? e : static_cast<::error_t>(ERR_NO_MEMORY)});
}

inline result<void *> allocate(std::size_t bytes, const char *tag)
{
    if (bytes == 0) return std::unexpected(error{errc::invalid_argument});
    void *p = ::malloc_tagged(bytes, tag);
    if (p) return p;
    ::error_t e = heap_get_last_error();
    return std::unexpected(error{e != OK ? e : static_cast<::error_t>(ERR_NO_MEMORY)});
}

inline result<void *> reallocate(void *p, std::size_t bytes)
{
    if (bytes == 0) {
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


struct tagged_deleter {
    template <class T>
    void operator()(T *p) const noexcept
    {
        if (p) { p->~T(); ::free(p); }
    }
};

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
        ::free(raw);
        throw;
    }
}

template <class Fn>
void for_each(const char *tag_name, Fn &&fn)
{
    HeapTagCallback thunk = [](void *ptr, size_t size, const char *name, void *ud) {
        (*static_cast<std::remove_reference_t<Fn> *>(ud))(ptr, size, name);
    };
    heap_iterate_tag(tag_name, thunk,
                     const_cast<void *>(static_cast<const void *>(std::addressof(fn))));
}

template <class Fn>
void for_each_tagged(Fn &&fn)
{
    HeapTagCallback thunk = [](void *ptr, size_t size, const char *name, void *ud) {
        (*static_cast<std::remove_reference_t<Fn> *>(ud))(ptr, size, name);
    };
    heap_iterate_all_tagged(thunk,
                            const_cast<void *>(static_cast<const void *>(std::addressof(fn))));
}

class tag {
    const char *name_;

public:
    explicit tag(const char *name) noexcept : name_(name) { heap_register_tag(name); }

    const char *name() const noexcept { return name_; }

    std::uint8_t id() const noexcept { return heap_lookup_tag(name_); }
    bool         registered() const noexcept { return id() != tag_none; }

    std::size_t count() const noexcept { return heap_count_tag(name_); }

    std::size_t bytes() const
    {
        std::size_t total = 0;
        heap::for_each(name_, [&total](void *, std::size_t sz, const char *) { total += sz; });
        return total;
    }

    template <class Fn>
    void for_each(Fn &&fn) const
    {
        heap::for_each(name_, static_cast<Fn &&>(fn));
    }

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

        std::size_t leaked() const noexcept
        {
            std::size_t now = heap_count_tag(name_);
            return now > base_ ? now - base_ : 0;
        }

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

}
}

template <>
struct std::formatter<box::heap::stats, char> : std::formatter<std::string_view, char> {
    auto format(const box::heap::stats &s, std::format_context &ctx) const
    {
        static constexpr char kFmt[] =
            "heap used={} in_use={} free={} live={} freeblk={} mallocs={} frees={}";
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

#endif