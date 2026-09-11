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

    void       *p_     = nullptr;
    std::size_t bytes_ = 0;
    bool        enc_   = false;

    bay(void *p, bool enc) noexcept : p_(p), bytes_(p ? bay_size(p) : 0), enc_(enc) {}

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

    static bay create(const char *tag, size_type count)
        requires (!_S_ro)
    {
        std::uint64_t bytes = _S_bytes(count);
        if (bytes == 0) return bay{};
        return bay(bay_open(tag, bytes, BAY_CREATE), false);
    }

    static bay open(const char *tag)
    {
        return bay(bay_open(tag, 0, BAY_OPEN | (_S_ro ? BAY_RO : 0u)), false);
    }

    static bay create_encrypted(const char *tag, size_type count)
        requires (!_S_ro)
    {
        std::uint64_t bytes = _S_bytes(count);
        if (bytes == 0) return bay{};
        void *p = bay_open(tag, bytes, BAY_CREATE | BAY_ENCRYPTED);
        return bay(p, p != nullptr);
    }

    static bay open_encrypted(const char *tag)
    {
        void *p = bay_open(tag, 0, BAY_OPEN | BAY_ENCRYPTED | (_S_ro ? BAY_RO : 0u));
        return bay(p, p != nullptr);
    }

    explicit operator bool() const noexcept { return p_ != nullptr; }
    bool encrypted()         const noexcept { return enc_; }
    bool read_only()         const noexcept { return _S_ro; }

    size_type size_bytes() const noexcept { return bytes_; }
    size_type size()       const noexcept { return bytes_ / sizeof(T); }
    bool      empty()      const noexcept { return bytes_ == 0; }

    pointer   data()                  const noexcept { return static_cast<pointer>(p_); }
    reference operator[](size_type i) const noexcept { return data()[i]; }
    std::span<T> as_span()            const noexcept { return std::span<T>(data(), size()); }

    iterator begin() const noexcept { return data(); }
    iterator end()   const noexcept { return data() + size(); }
};

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

}

#endif