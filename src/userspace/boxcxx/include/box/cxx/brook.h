#ifndef BOXCXX_BOX_BROOK_H
#define BOXCXX_BOX_BROOK_H

#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <type_traits>

#include "box/brook.h"
#include "box/error.h"
#include "box/cxx/executor.h"

namespace box {

template <class T>
class brook {
    static_assert(std::is_trivially_copyable_v<T>,
                  "box::brook<T> frame must be trivially copyable");
    static_assert(std::is_default_constructible_v<T>,
                  "box::brook<T> frame must be default-constructible (ring storage)");
    static_assert(sizeof(T) >= 8 && sizeof(T) <= 65536,
                  "box::brook<T> frame size must be in [8, 65536] bytes");

    Brook *b_ = nullptr;
    explicit brook(Brook *b) noexcept : b_(b) {}

public:
    using value_type = T;

    brook() noexcept                = default;
    brook(const brook &)            = delete;
    brook &operator=(const brook &) = delete;
    brook(brook &&o) noexcept : b_(o.b_) { o.b_ = nullptr; }
    brook &operator=(brook &&o) noexcept
    {
        if (this != &o) {
            if (b_) brook_release(b_);
            b_   = o.b_;
            o.b_ = nullptr;
        }
        return *this;
    }
    ~brook() { if (b_) brook_release(b_); }

    static brook writer(const char *tag, std::uint32_t capacity, bool stream = false)
    {
        return brook(brook_open(tag, static_cast<std::uint32_t>(sizeof(T)), capacity,
                                BROOK_WRITER | BROOK_CREATE | (stream ? BROOK_STREAM : 0u)));
    }
    static brook reader(const char *tag, bool stream = false)
    {
        return brook(brook_open(tag, static_cast<std::uint32_t>(sizeof(T)), 0,
                                BROOK_READER | (stream ? BROOK_STREAM : 0u)));
    }

    explicit operator bool() const noexcept { return b_ != nullptr; }
    Brook *handle() const noexcept { return b_; }

    std::uint32_t frame_size() const noexcept { return b_ ? brook_frame_size(b_) : 0u; }
    std::uint32_t capacity()   const noexcept { return b_ ? brook_frame_count(b_) : 0u; }
    std::uint32_t available()  const noexcept { return b_ ? brook_available(b_) : 0u; }
    std::uint32_t free()       const noexcept { return b_ ? brook_free(b_) : 0u; }

    bool push(const T &v) noexcept { return b_ && brook_push(b_, &v) == OK; }
    int try_push(const T &v) noexcept
    {
        return b_ ? brook_try_push(b_, &v) : -ERR_INVALID_ARGUMENT;
    }
    int push_for(const T &v, std::uint32_t timeout_ms) noexcept
    {
        return b_ ? brook_push_timeout(b_, &v, timeout_ms) : -ERR_INVALID_ARGUMENT;
    }

    bool pop(T &out) noexcept { return b_ && brook_pop(b_, &out) == OK; }
    int try_pop(T &out) noexcept
    {
        return b_ ? brook_try_pop(b_, &out) : -ERR_INVALID_ARGUMENT;
    }
    int pop_for(T &out, std::uint32_t timeout_ms) noexcept
    {
        return b_ ? brook_pop_timeout(b_, &out, timeout_ms) : -ERR_INVALID_ARGUMENT;
    }

    class read_awaiter {
    public:
        explicit read_awaiter(Brook *b) noexcept : _M_b(b) {}

        bool await_ready() noexcept
        {
            _M_rc = brook_try_pop(_M_b, &_M_val);
            return _M_rc != -ERR_WOULD_BLOCK;
        }
        bool await_suspend(std::coroutine_handle<> __h)
        {
            executor::current()->wait_on(__h, this, &_S_poll, &_S_block,
                                         __exec::wait_domain::brook);
            return true;
        }
        std::optional<T> await_resume() noexcept
        {
            if (_M_rc == OK) return _M_val;
            return std::nullopt;
        }

    private:
        static bool _S_poll(void *__s)
        {
            auto *__a  = static_cast<read_awaiter *>(__s);
            if (__a->_M_rc != -ERR_WOULD_BLOCK) return true;
            __a->_M_rc = brook_try_pop(__a->_M_b, &__a->_M_val);
            return __a->_M_rc != -ERR_WOULD_BLOCK;
        }
        static void _S_block(void *__s, std::uint32_t __ms)
        {
            auto *__a  = static_cast<read_awaiter *>(__s);
            if (__a->_M_rc != -ERR_WOULD_BLOCK) return;
            __a->_M_rc = brook_pop_timeout(__a->_M_b, &__a->_M_val, __ms);
            if (__a->_M_rc == -ERR_TIMEOUT) __a->_M_rc = -ERR_WOULD_BLOCK;
        }

        Brook *_M_b;
        T      _M_val{};
        int    _M_rc = -ERR_WOULD_BLOCK;
    };

    class write_awaiter {
    public:
        write_awaiter(Brook *b, const T &v) noexcept : _M_b(b), _M_val(v) {}

        bool await_ready() noexcept
        {
            _M_rc = brook_try_push(_M_b, &_M_val);
            return _M_rc != -ERR_WOULD_BLOCK;
        }
        bool await_suspend(std::coroutine_handle<> __h)
        {
            executor::current()->wait_on(__h, this, &_S_poll, &_S_block,
                                         __exec::wait_domain::brook);
            return true;
        }
        bool await_resume() noexcept { return _M_rc == OK; }

    private:
        static bool _S_poll(void *__s)
        {
            auto *__a  = static_cast<write_awaiter *>(__s);
            if (__a->_M_rc != -ERR_WOULD_BLOCK) return true;
            __a->_M_rc = brook_try_push(__a->_M_b, &__a->_M_val);
            return __a->_M_rc != -ERR_WOULD_BLOCK;
        }
        static void _S_block(void *__s, std::uint32_t __ms)
        {
            auto *__a  = static_cast<write_awaiter *>(__s);
            if (__a->_M_rc != -ERR_WOULD_BLOCK) return;
            __a->_M_rc = brook_push_timeout(__a->_M_b, &__a->_M_val, __ms);
            if (__a->_M_rc == -ERR_TIMEOUT) __a->_M_rc = -ERR_WOULD_BLOCK;
        }

        Brook *_M_b;
        T      _M_val;
        int    _M_rc = -ERR_WOULD_BLOCK;
    };

    read_awaiter  next() noexcept { return read_awaiter{b_}; }
    write_awaiter send(const T &v) noexcept { return write_awaiter{b_, v}; }

    class iterator {
    public:
        using iterator_concept  = std::input_iterator_tag;
        using iterator_category = std::input_iterator_tag;
        using value_type        = T;
        using difference_type   = std::ptrdiff_t;
        using reference         = const T &;
        using pointer           = const T *;

        iterator() noexcept = default;
        explicit iterator(brook *owner) : _M_owner(owner) { _M_advance(); }

        reference operator*() const noexcept { return _M_val; }
        pointer   operator->() const noexcept { return &_M_val; }
        iterator &operator++() { _M_advance(); return *this; }
        void      operator++(int) { _M_advance(); }

        friend bool operator==(const iterator &it, std::default_sentinel_t) noexcept
        {
            return it._M_owner == nullptr;
        }

    private:
        void _M_advance()
        {
            if (_M_owner && !_M_owner->pop(_M_val)) _M_owner = nullptr;
        }

        brook *_M_owner = nullptr;
        T      _M_val{};
    };

    iterator                begin() noexcept { return iterator{b_ ? this : nullptr}; }
    std::default_sentinel_t end() const noexcept { return {}; }
};

}

#endif