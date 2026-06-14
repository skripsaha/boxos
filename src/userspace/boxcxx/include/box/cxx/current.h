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
//                              trivially-copyable T. put()/take()/close(),
//                              with the honest end-of-stream: take() returns
//                              false once the writer closed and the stream
//                              drained (CURRENT_CLOSED) — never a Unix EOF.
//
// This is a box:: extension — not part of std. It is the BoxOS-native I/O
// surface; std::print stays available and (after the spine landed) routes
// through the same console Current under the hood.
#ifndef BOXCXX_BOX_CURRENT_H
#define BOXCXX_BOX_CURRENT_H

#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <type_traits>

#include "box/current.h"

namespace box {

enum class role : unsigned {
    read  = CURRENT_READ,
    write = CURRENT_WRITE,
};

// ---------------------------------------------------------------------------
// box::current<T> — typed framed stream (Brook-backed). Primary template.
// ---------------------------------------------------------------------------
template <class T = std::byte>
class current {
    static_assert(std::is_trivially_copyable_v<T>,
                  "box::current<T> stream item must be trivially copyable");
    Current *c_ = nullptr;

public:
    current() noexcept = default;
    current(const char *tag, role r, unsigned flags = 0) noexcept
        : c_(current_open(tag, static_cast<std::uint32_t>(r),
                          static_cast<std::uint32_t>(sizeof(T)), flags))
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

    // Append one item. Returns false if the reader is gone (CURRENT_NO_READER)
    // or on error.
    bool put(const T &v) noexcept
    {
        return c_ && current_put(c_, &v) == static_cast<int>(sizeof(T));
    }
    // Take one item. Returns false once the writer closed and the stream
    // drained (CURRENT_CLOSED), or on error — the honest loop terminator.
    bool take(T &out) noexcept
    {
        return c_ && current_take(c_, &out) == static_cast<int>(sizeof(T));
    }
    // Announce end-of-stream to the reader without releasing the handle.
    void close() noexcept { if (c_) current_close(c_); }
};

// ---------------------------------------------------------------------------
// box::current<std::byte> — byte / text channel (screen, log, keyboard, file).
// ---------------------------------------------------------------------------
template <>
class current<std::byte> {
    Current *c_ = nullptr;

public:
    current() noexcept = default;
    current(const char *tag, role r, unsigned flags = 0) noexcept
        : c_(current_open(tag, static_cast<std::uint32_t>(r), 0, flags))
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
    bool     seek(std::uint64_t off) noexcept { return c_ && current_seek(c_, off) == OK; }
    std::uint64_t tell() const noexcept { return c_ ? current_tell(c_) : 0u; }
};

using byte_current = current<std::byte>;

// ---------------------------------------------------------------------------
// Conventional channels — the named counterpart of stdout/stdin/stderr.
// ---------------------------------------------------------------------------
inline byte_current screen()   { return byte_current("screen",   role::write); }
inline byte_current log()      { return byte_current("log",      role::write); }
inline byte_current keyboard() { return byte_current("keyboard", role::read); }
inline byte_current file(const char *path, role r, unsigned flags = CURRENT_CREATE)
{
    // path is the bare TagFS name; the "file:" scheme prefix is added here.
    std::string tag = "file:";
    tag += path;
    return byte_current(tag.c_str(), r, flags);
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

} // namespace box

#endif // BOXCXX_BOX_CURRENT_H
