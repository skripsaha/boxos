// boxcxx — box::bay<T> / box::shared_object<T>
//   (the typed C++ face of the BoxOS Bay primitive — cross-cabin shared memory)
//
// A Bay is a tag-named, refcounted region of physical pages that the kernel maps
// into every cabin that opens the same tag — the BoxOS answer to shared memory,
// not a Unix mmap clone. box::bay<T> dresses that raw region as a typed,
// contiguous, RAII view:
//
//   * explicit create / open roles (a writer creates and sizes the region; any
//     cabin — or a second claim in this one — opens the existing tag),
//   * element access into the shared pages (operator[] / data / as_span) and the
//     region shape (size in elements / size_bytes),
//   * a real std::ranges::contiguous_range, so the std algorithms and views run
//     directly over cross-cabin memory,
//   * a const element type (box::bay<const T>) that maps the pages read-only
//     (BAY_RO) and hands out only const references,
//   * a strict TME-MK encrypted factory: on a host without active TME-MK the
//     create yields an empty bay — never a silent downgrade to plaintext.
//
//   auto grid = box::bay<Cell>::create("video:frame:0", 1920 * 1080);
//   grid[i] = c;                                  // write into the shared pages
//   for (const Cell& c : grid) { ... }            // contiguous range
//   auto view = box::bay<const Cell>::open("video:frame:0");   // read-only map
//   auto sec  = box::bay<Key>::create_encrypted("vault:keys", 256);
//   if (!sec) sec = box::bay<Key>::create("vault:keys", 256);  // explicit fallback
//
// This is a box:: extension, not part of std. Elements are trivially-copyable
// (the pages are raw shared bytes — no constructor or destructor runs over them),
// and the element alignment must fit the 4 KiB Bay page grain. The claim is
// released when the view is destroyed; the backing pages return to the PMM once
// the last claim across all cabins drops.
#ifndef BOXCXX_BOX_BAY_H
#define BOXCXX_BOX_BAY_H

#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>
#include <utility>

#include "box/bay.h"
#include "box/error.h"

namespace box {

template <class T>
class bay {
    using _Val = std::remove_cv_t<T>;
    static_assert(std::is_trivially_copyable_v<_Val>,
                  "box::bay<T> element must be trivially copyable "
                  "(raw cross-cabin shared memory, no constructors run)");
    static_assert(sizeof(T) >= 1, "box::bay<T> element must have a non-zero size");
    static_assert(alignof(T) <= 4096,
                  "box::bay<T> element over-aligned beyond the 4 KiB Bay page grain");

    static constexpr bool _S_ro = std::is_const_v<T>;

    void       *p_     = nullptr;  // this cabin's mapping of the shared pages
    std::size_t bytes_ = 0;        // logical size in bytes — immutable, cached once at open
    bool        enc_   = false;    // created/opened through the TME-MK encrypted factory

    bay(void *p, bool enc) noexcept : p_(p), bytes_(p ? bay_size(p) : 0), enc_(enc) {}

    // count * sizeof(T) with an overflow guard — a request that does not fit a
    // 64-bit byte count yields 0, which the factories turn into an empty bay.
    static std::uint64_t _S_bytes(std::size_t count) noexcept
    {
        std::uint64_t bytes;
        if (__builtin_mul_overflow(static_cast<std::uint64_t>(count), sizeof(T), &bytes))
            return 0;
        return bytes;
    }

public:
    using element_type   = T;
    using value_type     = _Val;
    using size_type      = std::size_t;
    using reference      = T &;
    using pointer        = T *;
    using iterator       = T *;
    using const_iterator = const T *;

    bay() noexcept                = default;
    bay(const bay &)              = delete;
    bay &operator=(const bay &)   = delete;
    bay(bay &&o) noexcept : p_(o.p_), bytes_(o.bytes_), enc_(o.enc_)
    {
        o.p_ = nullptr; o.bytes_ = 0; o.enc_ = false;
    }
    bay &operator=(bay &&o) noexcept
    {
        if (this != &o) {
            if (p_) bay_release(p_);
            p_       = o.p_;
            bytes_   = o.bytes_;
            enc_     = o.enc_;
            o.p_     = nullptr;
            o.bytes_ = 0;
            o.enc_   = false;
        }
        return *this;
    }
    ~bay() { if (p_) bay_release(p_); }

    // ── factories ──────────────────────────────────────────────────────────
    // create(): allocate `count` fresh shared elements under `tag` and map them
    // read-write. Only a writable element type may create — a box::bay<const T>
    // can only attach to a Bay another writer already created.
    static bay create(const char *tag, size_type count)
        requires (!_S_ro)
    {
        std::uint64_t bytes = _S_bytes(count);
        if (bytes == 0) return bay{};
        return bay(bay_open(tag, bytes, BAY_CREATE), false);
    }

    // open(): attach to an existing Bay by tag. A const element type maps the
    // pages read-only (BAY_RO); a writable element type maps them read-write.
    // The region's size is inherited from the creator.
    static bay open(const char *tag)
    {
        return bay(bay_open(tag, 0, BAY_OPEN | (_S_ro ? BAY_RO : 0u)), false);
    }

    // create_encrypted(): like create() but backs the pages with a unique TME-MK
    // KeyID so DRAM contents are opaque without the key. STRICT — on a host
    // without active TME-MK the kernel grants no mapping and this yields an empty
    // bay (operator bool == false); the caller decides whether to fall back to a
    // plaintext create(). There is no silent downgrade of an encryption request.
    static bay create_encrypted(const char *tag, size_type count)
        requires (!_S_ro)
    {
        std::uint64_t bytes = _S_bytes(count);
        if (bytes == 0) return bay{};
        void *p = bay_open(tag, bytes, BAY_CREATE | BAY_ENCRYPTED);
        return bay(p, p != nullptr);
    }

    // open_encrypted(): attach to an existing encrypted Bay. The kernel rejects
    // an encrypted/plaintext flag mismatch, so an encrypted Bay must be opened
    // through this factory — a plain open() returns an empty bay for it.
    static bay open_encrypted(const char *tag)
    {
        void *p = bay_open(tag, 0, BAY_OPEN | BAY_ENCRYPTED | (_S_ro ? BAY_RO : 0u));
        return bay(p, p != nullptr);
    }

    // ── identity / shape ─────────────────────────────────────────────────────
    explicit operator bool() const noexcept { return p_ != nullptr; }
    bool encrypted()         const noexcept { return enc_; }
    bool read_only()         const noexcept { return _S_ro; }

    // size() is the LOGICAL element count the creator requested (bay_size reports
    // the requested bytes, not the page-rounded physical mapping) — exact for both
    // create and open. Cached at construction; the Bay's size is immutable.
    size_type size_bytes() const noexcept { return bytes_; }
    size_type size()       const noexcept { return bytes_ / sizeof(T); }
    bool      empty()      const noexcept { return bytes_ == 0; }

    // ── element access (a view into the shared pages) ────────────────────────
    pointer   data()                  const noexcept { return static_cast<pointer>(p_); }
    reference operator[](size_type i) const noexcept { return data()[i]; }
    std::span<T> as_span()            const noexcept { return std::span<T>(data(), size()); }

    // ── contiguous range over the whole Bay ──────────────────────────────────
    iterator begin() const noexcept { return data(); }
    iterator end()   const noexcept { return data() + size(); }
};

// box::shared_object<T> — a single shared T living in a Bay (the count-of-one
// convenience over box::bay<T>), with pointer-like access. Same roles, same
// strict encrypted factory; const T maps read-only.
//
//   auto reg = box::shared_object<Registers>::create("dev:nic:regs");
//   reg->status = 1;                                    // operator->
//   auto peek = box::shared_object<const Registers>::open("dev:nic:regs");
template <class T>
class shared_object {
    bay<T> slot_;
    explicit shared_object(bay<T> b) noexcept : slot_(std::move(b)) {}

public:
    using element_type = T;

    shared_object() noexcept                            = default;
    shared_object(shared_object &&) noexcept            = default;
    shared_object &operator=(shared_object &&) noexcept = default;
    shared_object(const shared_object &)                = delete;
    shared_object &operator=(const shared_object &)     = delete;

    static shared_object create(const char *tag)
        requires (!std::is_const_v<T>)
    {
        return shared_object(bay<T>::create(tag, 1));
    }
    static shared_object open(const char *tag) { return shared_object(bay<T>::open(tag)); }
    static shared_object create_encrypted(const char *tag)
        requires (!std::is_const_v<T>)
    {
        return shared_object(bay<T>::create_encrypted(tag, 1));
    }
    static shared_object open_encrypted(const char *tag)
    {
        return shared_object(bay<T>::open_encrypted(tag));
    }

    explicit operator bool() const noexcept { return static_cast<bool>(slot_); }
    bool encrypted()         const noexcept { return slot_.encrypted(); }
    bool read_only()         const noexcept { return slot_.read_only(); }

    T *get()        const noexcept { return slot_.data(); }
    T &operator*()  const noexcept { return slot_.data()[0]; }
    T *operator->() const noexcept { return slot_.data(); }
};

}  // namespace box

#endif  // BOXCXX_BOX_BAY_H
