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
#include "box/cxx/executor.h"

namespace box {

enum class role : unsigned {
    read  = CURRENT_READ,
    write = CURRENT_WRITE,
};

enum class opening : unsigned {
    none     = 0,
    create   = CURRENT_CREATE,
    nonblock = CURRENT_NONBLOCK,
    truncate = CURRENT_TRUNCATE,
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

    status put(const T &v) noexcept
    {
        if (!c_) return std::unexpected(error{errc::invalid_argument});
        int rc = current_put(c_, &v);
        if (rc == static_cast<int>(sizeof(T))) return {};
        if (rc < 0) return std::unexpected(error{box_errno_of(rc)});
        return std::unexpected(error{errc::io});
    }
    result<bool> take(T &out) noexcept
    {
        if (!c_) return std::unexpected(error{errc::invalid_argument});
        int rc = current_take(c_, &out);
        if (rc == static_cast<int>(sizeof(T))) return true;
        if (rc == CURRENT_CLOSED) return false;
        return std::unexpected(error{box_errno_of(rc)});
    }
    void close() noexcept { if (c_) current_close(c_); }

    class read_awaiter {
    public:
        explicit read_awaiter(Current *c) noexcept : _M_c(c) {}

        bool await_ready() noexcept
        {
            _M_rc = current_take_now(_M_c, &_M_val);
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
            if (_M_rc == static_cast<int>(sizeof(T))) return _M_val;
            return std::nullopt;
        }

    private:
        static bool _S_poll(void *__s)
        {
            auto *__a  = static_cast<read_awaiter *>(__s);
            if (__a->_M_rc != -ERR_WOULD_BLOCK) return true;
            __a->_M_rc = current_take_now(__a->_M_c, &__a->_M_val);
            return __a->_M_rc != -ERR_WOULD_BLOCK;
        }
        static void _S_block(void *__s, std::uint32_t __ms)
        {
            auto *__a  = static_cast<read_awaiter *>(__s);
            if (__a->_M_rc != -ERR_WOULD_BLOCK) return;
            __a->_M_rc = current_take_for(__a->_M_c, &__a->_M_val, __ms);
            if (__a->_M_rc == -ERR_TIMEOUT) __a->_M_rc = -ERR_WOULD_BLOCK;
        }

        Current *_M_c;
        T        _M_val{};
        int      _M_rc = -ERR_WOULD_BLOCK;
    };

    read_awaiter next() noexcept { return read_awaiter{c_}; }
};

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

    std::size_t write(const void *p, std::size_t n) noexcept
    {
        int rc = c_ ? current_write(c_, p, n) : -1;
        return rc > 0 ? static_cast<std::size_t>(rc) : 0u;
    }
    std::size_t write(std::string_view s) noexcept { return write(s.data(), s.size()); }

    int read(void *p, std::size_t n) noexcept { return c_ ? current_read(c_, p, n) : -1; }

    result<std::size_t> read_some(void *p, std::size_t n) noexcept
    {
        if (!c_) return std::unexpected(error{errc::invalid_argument});
        int rc = current_read(c_, p, n);
        if (rc > 0) return static_cast<std::size_t>(rc);
        if (rc == CURRENT_CLOSED) return std::size_t{0};
        return std::unexpected(error{box_errno_of(rc)});
    }

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
    status   seek(std::uint64_t off) noexcept
    {
        return _detail::from_status(c_ ? current_seek(c_, off) : -ERR_INVALID_ARGUMENT);
    }
    std::uint64_t tell() const noexcept { return c_ ? current_tell(c_) : 0u; }

    result<std::uint64_t> size() const noexcept
    {
        if (!c_) return std::unexpected(error{errc::invalid_argument});
        std::int64_t n = current_size(c_);
        if (n < 0) return std::unexpected(error{box_errno_of(static_cast<int>(n))});
        return static_cast<std::uint64_t>(n);
    }

    status resize(std::uint64_t n) noexcept
    {
        return _detail::from_status(c_ ? current_resize(c_, n) : -ERR_INVALID_ARGUMENT);
    }
};

using byte_current = current<std::byte>;

inline byte_current screen()   { return byte_current("screen",   role::write); }
inline byte_current log()      { return byte_current("log:serial", role::write); }
inline byte_current keyboard(opening o = opening::none) { return byte_current("keyboard", role::read, o); }
inline byte_current file(const char *path, role r, opening o = opening::create)
{
    std::string tag = "file:";
    tag += path;
    return byte_current(tag.c_str(), r, o);
}

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

}

#endif