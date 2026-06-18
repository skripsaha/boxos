// boxcxx — box::brook<T>  (the direct C++ face of the BoxOS Brook primitive)
//
// A typed, RAII, single-producer/single-consumer ordered stream over a boxlib
// Brook (box/brook.h). Where box::current<T> frames a typed stream through the
// high-level Current I/O spine, box::brook<T> is the raw Brook itself — it
// exposes the full native surface:
//
//   * explicit reader / writer roles (writer creates and fixes the shape;
//     reader attaches to an existing stream and pins the frame size to T),
//   * the ring shape and live counters (frame_size / capacity / available /
//     free),
//   * the non-blocking / bounded / blocking push & pop trio,
//   * a synchronous input_range that drains until the writer leaves,
//   * coroutine-native next() / send() that suspend on the Ф12 box::executor.
//
//   auto w = box::brook<Tick>::writer("metric:tick", 1024);
//   auto r = box::brook<Tick>::reader("metric:tick");
//   w.push(t);                          // blocking, backpressured
//   if (r.try_pop(t) == OK) { ... }     // non-blocking
//   for (const Tick& t : r) { ... }     // drains until the writer leaves
//   if (auto t = co_await r.next()) ... // suspends on the current executor
//   co_await w.send(t);
//
// This is a box:: extension, not part of std. Items are trivially-copyable,
// default-constructible frames in the Brook size range [8, 65536] bytes. The
// honest end-of-stream: pop() / next() / the range terminate once the writer
// has left and the ring has drained (the Brook writer-leave terminal), never a
// Unix EOF.
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
#include "box/cxx/executor.h"  // box::executor, box::__exec::waiter, wait_on

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

    // ── open ──────────────────────────────────────────────────────────────
    // writer() creates the stream and fixes its shape: frame_size == sizeof(T),
    // capacity == frame_count (a power of two in [2, 16384]). reader() attaches
    // to an existing stream and passes sizeof(T) so a frame-size mismatch
    // against the creator is rejected at open (the count is inherited).
    //
    // stream == true selects a long-running daemon stream that survives peer
    // swaps (no writer-leave terminal — use the bounded forms for liveness).
    // The default single-session mode delivers the clean drain-then-terminal
    // that pop() / next() / the range rely on as their loop end.
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

    // ── shape & live counters (read from the shared header, no syscall) ─────
    std::uint32_t frame_size() const noexcept { return b_ ? brook_frame_size(b_) : 0u; }
    std::uint32_t capacity()   const noexcept { return b_ ? brook_frame_count(b_) : 0u; }
    std::uint32_t available()  const noexcept { return b_ ? brook_available(b_) : 0u; }
    std::uint32_t free()       const noexcept { return b_ ? brook_free(b_) : 0u; }

    // ── writer side ─────────────────────────────────────────────────────
    // push(): blocking, backpressured. Returns false once the reader is gone.
    bool push(const T &v) noexcept { return b_ && brook_push(b_, &v) == OK; }
    // Non-blocking / bounded forms return the native BoxOS rc: OK,
    // -ERR_WOULD_BLOCK when the ring is full, -ERR_TIMEOUT on a deadline,
    // -ERR_PROCESS_TERMINATED once the reader has left.
    int try_push(const T &v) noexcept
    {
        return b_ ? brook_try_push(b_, &v) : -ERR_INVALID_ARGUMENT;
    }
    int push_for(const T &v, std::uint32_t timeout_ms) noexcept
    {
        return b_ ? brook_push_timeout(b_, &v, timeout_ms) : -ERR_INVALID_ARGUMENT;
    }

    // ── reader side ─────────────────────────────────────────────────────
    // pop(): blocking. Returns false once the writer has left and the ring has
    // drained (the clean stream terminal) — the honest loop end, not a Unix EOF.
    bool pop(T &out) noexcept { return b_ && brook_pop(b_, &out) == OK; }
    int try_pop(T &out) noexcept
    {
        return b_ ? brook_try_pop(b_, &out) : -ERR_INVALID_ARGUMENT;
    }
    int pop_for(T &out, std::uint32_t timeout_ms) noexcept
    {
        return b_ ? brook_pop_timeout(b_, &out, timeout_ms) : -ERR_INVALID_ARGUMENT;
    }

    // ── coroutine-native async (suspends on the current box::executor) ────
    // Each awaiter pairs a non-blocking poll (consumes one frame into its own
    // storage) with the native bounded wait, and registers (handle, poll,
    // block) with the running executor on suspend — the same mechanism the Ф12
    // box::brook_read / touch_event awaiters use. Awaiting outside an executor-
    // driven coroutine is a precondition violation (executor::current() null).
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
            executor::current()->wait_on({__h, this, &_S_poll, &_S_block});
            return true;
        }
        // nullopt: the writer has left and the ring drained (or an error) — the
        // stream terminal, expressed as "no more frames".
        std::optional<T> await_resume() noexcept
        {
            if (_M_rc == OK) return _M_val;
            return std::nullopt;
        }

    private:
        static bool _S_poll(void *__s)
        {
            auto *__a  = static_cast<read_awaiter *>(__s);
            __a->_M_rc = brook_try_pop(__a->_M_b, &__a->_M_val);
            return __a->_M_rc != -ERR_WOULD_BLOCK;
        }
        static void _S_block(void *__s, std::uint32_t __ms)
        {
            auto *__a  = static_cast<read_awaiter *>(__s);
            __a->_M_rc = brook_pop_timeout(__a->_M_b, &__a->_M_val, __ms);
            if (__a->_M_rc == -ERR_TIMEOUT) __a->_M_rc = -ERR_WOULD_BLOCK;  // re-poll
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
            executor::current()->wait_on({__h, this, &_S_poll, &_S_block});
            return true;
        }
        // false: the reader has left (-ERR_PROCESS_TERMINATED) — no further drain.
        bool await_resume() noexcept { return _M_rc == OK; }

    private:
        static bool _S_poll(void *__s)
        {
            auto *__a  = static_cast<write_awaiter *>(__s);
            __a->_M_rc = brook_try_push(__a->_M_b, &__a->_M_val);
            return __a->_M_rc != -ERR_WOULD_BLOCK;
        }
        static void _S_block(void *__s, std::uint32_t __ms)
        {
            auto *__a  = static_cast<write_awaiter *>(__s);
            __a->_M_rc = brook_push_timeout(__a->_M_b, &__a->_M_val, __ms);
            if (__a->_M_rc == -ERR_TIMEOUT) __a->_M_rc = -ERR_WOULD_BLOCK;
        }

        Brook *_M_b;
        T      _M_val;
        int    _M_rc = -ERR_WOULD_BLOCK;
    };

    // co_await r.next()   -> std::optional<T>  (nullopt at the stream terminal)
    // co_await w.send(v)  -> bool              (false once the reader has left)
    read_awaiter  next() noexcept { return read_awaiter{b_}; }
    write_awaiter send(const T &v) noexcept { return write_awaiter{b_, v}; }

    // ── synchronous input_range ───────────────────────────────────────────
    // Single-pass: each ++ blocks for the next frame; the range ends when the
    // writer leaves and the ring drains.  for (const T& v : r) { ... }
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

        brook *_M_owner = nullptr;  // nullptr == past-the-end
        T      _M_val{};
    };

    iterator                begin() noexcept { return iterator{b_ ? this : nullptr}; }
    std::default_sentinel_t end() const noexcept { return {}; }
};

}  // namespace box

#endif  // BOXCXX_BOX_BROOK_H
