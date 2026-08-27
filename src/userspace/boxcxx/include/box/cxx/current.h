// boxcxx — box::current  (the C++ face of the BoxOS Current I/O spine)
//
// A typed, RAII view over a boxlib Current (box/current.h). Two honest shapes,
// one name:
//
//   box::current<std::byte>  — a byte / text channel (screen, log, keyboard,
//                              file). write()/read()/read_line()/seek().
//                              box::screen()/log()/keyboard() open the three
//                              conventional channels; box::print()/println()
//                              format into one via <format>.
//
//   box::current<T>          — a typed framed stream (Brook-backed) of
//                              trivially-copyable T. put()/take()/close().
//                              put() -> box::status (process_terminated when the
//                              reader is gone, would_block on a full NONBLOCK
//                              stream). take() -> box::result<bool>: a `false`
//                              VALUE is the honest end-of-stream (writer closed +
//                              drained, CURRENT_CLOSED) — never a Unix EOF — while
//                              would_block / real faults land in the error arm.
//
// This is a box:: extension — not part of std. It is the BoxOS-native I/O
// surface; std::print stays available and (after the spine landed) routes
// through the same console Current under the hood.
#ifndef BOXCXX_BOX_CURRENT_H
#define BOXCXX_BOX_CURRENT_H

#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "box/current.h"
#include "box/cxx/error.h"
#include "box/cxx/executor.h"  // box::executor, wait_on, __exec::wait_domain

namespace box {

enum class role : unsigned {
    read  = CURRENT_READ,
    write = CURRENT_WRITE,
};

// ── box::opening — the typed open-mode of a Current ─────────────────────────
// The create/non-block intent you hand current_open, as a checked enum rather
// than a bag of loose CURRENT_* ints (BoxOS keeps its own vocabulary — this is
// the open *manifest*, not POSIX open(2) flags). Compose with operator|:
//   box::current<T>("t", role::write, opening::create | opening::nonblock);
// `none` is the blocking, must-already-exist default.
enum class opening : unsigned {
    none     = 0,
    create   = CURRENT_CREATE,    // create the backing if absent (stream / file writer)
    nonblock = CURRENT_NONBLOCK,  // put/take never block: would_block when full / empty
    truncate = CURRENT_TRUNCATE,  // file writer: discard existing content at open
};
constexpr opening operator|(opening a, opening b) noexcept
{
    return static_cast<opening>(static_cast<unsigned>(a) | static_cast<unsigned>(b));
}
constexpr opening operator&(opening a, opening b) noexcept
{
    return static_cast<opening>(static_cast<unsigned>(a) & static_cast<unsigned>(b));
}
constexpr bool any(opening a) noexcept { return static_cast<unsigned>(a) != 0; }

// ---------------------------------------------------------------------------
// box::current<T> — typed framed stream (Brook-backed). Primary template.
// ---------------------------------------------------------------------------
template <class T = std::byte>
class current {
    static_assert(std::is_trivially_copyable_v<T>,
                  "box::current<T> stream item must be trivially copyable");
    static_assert(sizeof(T) <= 65536,
                  "box::current<T> item exceeds the Brook frame maximum (64 KiB)");
    Current *c_ = nullptr;

public:
    current() noexcept = default;
    current(const char *tag, role r, opening o = opening::none) noexcept
        : c_(current_open(tag, static_cast<std::uint32_t>(r),
                          static_cast<std::uint32_t>(sizeof(T)),
                          static_cast<std::uint32_t>(o)))
    {
    }
    current(const current &)            = delete;
    current &operator=(const current &) = delete;
    current(current &&o) noexcept : c_(o.c_) { o.c_ = nullptr; }
    current &operator=(current &&o) noexcept
    {
        if (this != &o) {
            if (c_) current_release(c_);
            c_   = o.c_;
            o.c_ = nullptr;
        }
        return *this;
    }
    ~current() { if (c_) current_release(c_); }

    explicit operator bool() const noexcept { return c_ != nullptr; }
    unsigned caps() const noexcept { return c_ ? current_caps(c_) : 0u; }
    Current *handle() const noexcept { return c_; }

    // Append one item. Empty status on success; the error arm carries the real
    // cause: errc::process_terminated when the reader is gone (the honest Current
    // name for the underlying peer-gone condition, CURRENT_NO_READER), would_block
    // on a NONBLOCK + full stream, errc::invalid_argument on a closed handle.
    status put(const T &v) noexcept
    {
        if (!c_) return std::unexpected(error{errc::invalid_argument});
        int rc = current_put(c_, &v);
        if (rc == static_cast<int>(sizeof(T))) return {};
        if (rc < 0) return std::unexpected(error{box_errno_of(rc)});
        return std::unexpected(error{errc::io});  // partial / 0 — never on a framed put
    }
    // Take one item — the honest stream tri-state. A `true` VALUE means an item
    // was read; a `false` VALUE means the writer closed and the stream drained
    // (CURRENT_CLOSED) — the non-error terminator, NEVER a Unix EOF. The error
    // arm carries a real cause: would_block on a NONBLOCK + empty stream, or any
    // other -ERR_*. Idiomatic drain: `while (auto r = s.take(v)) { if (!*r) break;
    // use(v); }` stops on close and surfaces a fault.
    result<bool> take(T &out) noexcept
    {
        if (!c_) return std::unexpected(error{errc::invalid_argument});
        int rc = current_take(c_, &out);
        if (rc == static_cast<int>(sizeof(T))) return true;
        if (rc == CURRENT_CLOSED) return false;
        return std::unexpected(error{box_errno_of(rc)});
    }
    // Announce end-of-stream to the reader without releasing the handle.
    void close() noexcept { if (c_) current_close(c_); }

    // ── coroutine-native async read (suspends on the current box::executor) ──
    // Mirrors box::brook<T>::read_awaiter but routes through the Current spine's
    // framed-take primitives (honoring the small-item frame repad). The Brook
    // wait-domain inherits the executor's lone-waiter mitigation (bounded slice
    // + yield); a brace-init waiter would mis-tag domain=0=touch, so use the
    // explicit 5-arg wait_on overload.
    //
    // PRECONDITION (SPSC, like Brook): at most ONE outstanding co_await next()
    // per handle at a time (the awaiter stages through the handle-scoped
    // frame_buf), and the current<T> must OUTLIVE the await and not be
    // closed/released while it is in flight.
    class read_awaiter {
    public:
        explicit read_awaiter(Current *c) noexcept : _M_c(c) {}

        bool await_ready() noexcept
        {
            _M_rc = current_take_now(_M_c, &_M_val);
            return _M_rc != -ERR_WOULD_BLOCK;   // ready on item / close / hard error
        }
        bool await_suspend(std::coroutine_handle<> __h)
        {
            executor::current()->wait_on(__h, this, &_S_poll, &_S_block,
                                         __exec::wait_domain::brook);
            return true;
        }
        // nullopt: writer closed + drained (CURRENT_CLOSED) OR error — the
        // unified "no more frames" terminal, matching box::brook<T>::next().
        std::optional<T> await_resume() noexcept
        {
            // Current framed success is the item SIZE (>0), NOT OK(0): copy
            // take()'s `rc == sizeof(T)` test, never Brook's `rc == OK`.
            if (_M_rc == static_cast<int>(sizeof(T))) return _M_val;
            return std::nullopt;
        }

    private:
        static bool _S_poll(void *__s)
        {
            auto *__a  = static_cast<read_awaiter *>(__s);
            // Latch: do NOT re-pop once _S_block (or a prior poll) delivered a
            // frame/terminal — the executor's Phase-3c re-polls every waiter
            // right after its native block, and a re-pop would advance past the
            // just-delivered frame (head already moved) and clobber _M_rc to
            // WOULD_BLOCK, losing it. Mirrors touch_event/pocket_recv.
            if (__a->_M_rc != -ERR_WOULD_BLOCK) return true;
            __a->_M_rc = current_take_now(__a->_M_c, &__a->_M_val);
            return __a->_M_rc != -ERR_WOULD_BLOCK;
        }
        static void _S_block(void *__s, std::uint32_t __ms)
        {
            auto *__a  = static_cast<read_awaiter *>(__s);
            if (__a->_M_rc != -ERR_WOULD_BLOCK) return;  // already delivered — don't re-block
            __a->_M_rc = current_take_for(__a->_M_c, &__a->_M_val, __ms);
            if (__a->_M_rc == -ERR_TIMEOUT) __a->_M_rc = -ERR_WOULD_BLOCK;  // re-poll
        }

        Current *_M_c;
        T        _M_val{};
        int      _M_rc = -ERR_WOULD_BLOCK;
    };

    // co_await s.next() -> std::optional<T>  (nullopt at the stream terminal).
    read_awaiter next() noexcept { return read_awaiter{c_}; }
};

// ---------------------------------------------------------------------------
// box::current<std::byte> — byte / text channel (screen, log, keyboard, file).
// ---------------------------------------------------------------------------
template <>
class current<std::byte> {
    Current *c_ = nullptr;

public:
    current() noexcept = default;
    current(const char *tag, role r, opening o = opening::none) noexcept
        : c_(current_open(tag, static_cast<std::uint32_t>(r), 0,
                          static_cast<std::uint32_t>(o)))
    {
    }
    current(const current &)            = delete;
    current &operator=(const current &) = delete;
    current(current &&o) noexcept : c_(o.c_) { o.c_ = nullptr; }
    current &operator=(current &&o) noexcept
    {
        if (this != &o) {
            if (c_) current_release(c_);
            c_   = o.c_;
            o.c_ = nullptr;
        }
        return *this;
    }
    ~current() { if (c_) current_release(c_); }

    explicit operator bool() const noexcept { return c_ != nullptr; }
    unsigned caps() const noexcept { return c_ ? current_caps(c_) : 0u; }
    Current *handle() const noexcept { return c_; }

    // Write bytes. Returns the number accepted (0 on error / unsupported).
    std::size_t write(const void *p, std::size_t n) noexcept
    {
        int rc = c_ ? current_write(c_, p, n) : -1;
        return rc > 0 ? static_cast<std::size_t>(rc) : 0u;
    }
    std::size_t write(std::string_view s) noexcept { return write(s.data(), s.size()); }

    // Read up to n bytes. Returns bytes read (>0), 0 at end-of-stream
    // (CURRENT_CLOSED), or a negative -ERR_*.
    int read(void *p, std::size_t n) noexcept { return c_ ? current_read(c_, p, n) : -1; }

    // Tri-state read — the honest byte-channel shape, mirroring current<T>::take().
    // A value > 0 is the byte count; a value == 0 is end-of-stream (the writer
    // closed and the channel drained, CURRENT_CLOSED) — NEVER a Unix EOF; the
    // error arm carries the real cause: would_block on a NONBLOCK + empty channel,
    // invalid_argument on a closed handle, or any other -ERR_*. Prefer this over
    // read() when the channel is opened opening::nonblock and you must tell "no
    // data right now" (would_block) apart from "stream finished" (value 0).
    result<std::size_t> read_some(void *p, std::size_t n) noexcept
    {
        if (!c_) return std::unexpected(error{errc::invalid_argument});
        int rc = current_read(c_, p, n);
        if (rc > 0) return static_cast<std::size_t>(rc);
        if (rc == CURRENT_CLOSED) return std::size_t{0};  // writer closed + drained
        return std::unexpected(error{box_errno_of(rc)});
    }

    // Read one line (keyboard: one edited line; file: up to `max` bytes).
    // Empty string at end / on error.
    std::string read_line(std::size_t max = 1024)
    {
        std::string s;
        if (!c_ || max == 0) return s;
        s.resize(max);
        int n = current_read(c_, s.data(), s.size());
        if (n <= 0) { s.clear(); return s; }
        s.resize(static_cast<std::size_t>(n));
        return s;
    }

    void     flush() noexcept { if (c_) current_flush(c_); }
    // Move the byte cursor (seekable backings — file). Empty status on success;
    // the error arm carries the cause (invalid_argument on a closed handle,
    // invalid_operation when the backing is not seekable).
    status   seek(std::uint64_t off) noexcept
    {
        return _detail::from_status(c_ ? current_seek(c_, off) : -ERR_INVALID_ARGUMENT);
    }
    std::uint64_t tell() const noexcept { return c_ ? current_tell(c_) : 0u; }

    // How long the channel is, in bytes. The error arm carries the cause:
    // invalid_operation on a backing with no knowable extent (a stream, the
    // screen, the keyboard), invalid_argument on a closed handle.
    result<std::uint64_t> size() const noexcept
    {
        if (!c_) return std::unexpected(error{errc::invalid_argument});
        std::int64_t n = current_size(c_);
        if (n < 0) return std::unexpected(error{box_errno_of(static_cast<int>(n))});
        return static_cast<std::uint64_t>(n);
    }

    // Discard everything past `n`. SHRINK ONLY — a request to grow lands in
    // the error arm as invalid_argument, because TagFS does not zero fresh
    // blocks and growing here would hand back the previous tenant's bytes.
    // Also refused on a snapshotted file (invalid_operation): a block not yet
    // copied since the snapshot is still the snapshot's only copy. The byte
    // cursor is clamped into the new range.
    status resize(std::uint64_t n) noexcept
    {
        return _detail::from_status(c_ ? current_resize(c_, n) : -ERR_INVALID_ARGUMENT);
    }
};

using byte_current = current<std::byte>;

// ---------------------------------------------------------------------------
// Conventional channels — the named counterpart of stdout/stdin/stderr.
// ---------------------------------------------------------------------------
inline byte_current screen()   { return byte_current("screen",   role::write); }
inline byte_current log()      { return byte_current("log:serial", role::write); }
inline byte_current keyboard(opening o = opening::none) { return byte_current("keyboard", role::read, o); }
inline byte_current file(const char *path, role r, opening o = opening::create)
{
    // path is the bare TagFS name; the "file:" scheme prefix is added here.
    std::string tag = "file:";
    tag += path;
    return byte_current(tag.c_str(), r, o);
}

// ---------------------------------------------------------------------------
// Formatted output to a byte channel — reuses <format> (no <iostream>).
// ---------------------------------------------------------------------------
template <class... Args>
inline void print(byte_current &ch, std::format_string<Args...> fmt, Args &&...args)
{
    std::string s = std::vformat(fmt.get(), std::make_format_args(args...));
    ch.write(s.data(), s.size());
}

template <class... Args>
inline void println(byte_current &ch, std::format_string<Args...> fmt, Args &&...args)
{
    std::string s = std::vformat(fmt.get(), std::make_format_args(args...));
    s.push_back('\n');
    ch.write(s.data(), s.size());
}

inline void println(byte_current &ch) { ch.write("\n", 1); }

// rvalue-channel overloads — allow one-shot temporaries, e.g.
//   box::println(box::screen(), "hi");   box::print(box::log(), "{}", x);
template <class... Args>
inline void print(byte_current &&ch, std::format_string<Args...> fmt, Args &&...args)
{
    print(ch, fmt, std::forward<Args>(args)...);
}
template <class... Args>
inline void println(byte_current &&ch, std::format_string<Args...> fmt, Args &&...args)
{
    println(ch, fmt, std::forward<Args>(args)...);
}
inline void println(byte_current &&ch) { println(ch); }

} // namespace box

#endif // BOXCXX_BOX_CURRENT_H
