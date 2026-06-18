// boxcxx — box::message  (the C++ face of BoxOS process-to-process messaging)
//
// BoxOS IPC: a process sends a byte payload to another by PID (send) or to every
// process that carries a tag (broadcast). The kernel copies the payload into the
// receiver's cabin heap and posts a record into its ResultRing; the receiver
// pops it. This is distinct from Touch (tag-multicast events, box::subscription)
// and Brook (SPSC streaming, box::brook<T>) — each has its own box:: layer.
//
//   box::message            — a received message: from() (sender pid),
//                             bytes() / payload_as<T>() / text(), reply(...).
//   box::send(pid, v)       — send to one process (by pid).
//   box::broadcast(tag, v)  — send to every process carrying a tag.
//   box::receive()          — non-blocking: the next message, or nullopt.
//   box::receive_for(ms)    — blocking up to ms (0 == forever); nullopt on timeout.
//   co_await box::next_message() -> box::message  — suspends on the box::executor.
//
// A box::message is only valid() (operator bool) when it is a genuine cross-
// process delivery (sender pid != 0). bytes()/payload_as/text NEVER dereference
// an empty or kernel-origin record (data_addr 0 / length 0) — that validation is
// what makes the receive path memory-safe (a raw unguarded data_addr read is the
// classic cross-cabin fault). This is a box:: extension, not part of std.
#ifndef BOXCXX_BOX_MESSAGE_H
#define BOXCXX_BOX_MESSAGE_H

#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>

#include "box/ipc.h"           // send / broadcast / receive / receive_wait + Result
#include "box/cxx/executor.h"  // box::executor, __exec::waiter, wait_on (co_await)

namespace box {

// ── box::message — a received cross-process message ─────────────────────────
class message {
    Result r_{};

public:
    message() noexcept = default;
    explicit message(const Result &r) noexcept : r_(r) {}

    // A genuine IPC delivery carries a non-zero sender pid (kernel-origin records
    // carry pid 0 and are never returned as messages).
    bool valid() const noexcept { return r_.sender_pid != 0; }
    explicit operator bool() const noexcept { return valid(); }

    std::uint32_t from() const noexcept { return r_.sender_pid; }   // sender pid
    std::uint32_t size() const noexcept { return r_.data_length; }  // payload bytes
    const Result &raw() const noexcept { return r_; }

    // The payload bytes — an EMPTY span unless this is a real delivery with a
    // mapped, non-empty payload. Never hands out a pointer to dereference when
    // sender pid / data_addr is 0 or the length is 0 (the cross-cabin read guard).
    std::span<const std::byte> bytes() const noexcept
    {
        if (r_.sender_pid == 0 || r_.data_addr == 0 || r_.data_length == 0) return {};
        return {reinterpret_cast<const std::byte *>(static_cast<std::uintptr_t>(r_.data_addr)),
                r_.data_length};
    }

    // The payload viewed as text. Empty unless a valid, non-empty delivery.
    std::string_view text() const noexcept
    {
        std::span<const std::byte> b = bytes();
        return {reinterpret_cast<const char *>(b.data()), b.size()};
    }

    // Reconstruct a trivially-copyable T from the payload prefix; nullopt unless a
    // valid delivery holds at least sizeof(T) bytes.
    template <class T>
    std::optional<T> payload_as() const noexcept
    {
        static_assert(std::is_trivially_copyable_v<T>,
                      "box::message::payload_as<T> requires a trivially copyable T");
        if (r_.sender_pid == 0 || r_.data_addr == 0 || r_.data_length < sizeof(T))
            return std::nullopt;
        T v;
        __builtin_memcpy(&v,
                         reinterpret_cast<const void *>(static_cast<std::uintptr_t>(r_.data_addr)),
                         sizeof(T));
        return v;
    }

    // Reply to the sender (send a payload back to from()). false if this is not a
    // real delivery or the sender is gone.
    bool reply(const void *data, std::uint16_t n) const noexcept
    {
        return r_.sender_pid != 0 && ::send(r_.sender_pid, data, n) == 0;
    }
    template <class T>
    bool reply(const T &v) const noexcept
    {
        static_assert(std::is_trivially_copyable_v<T>,
                      "box::message::reply(T) requires a trivially copyable payload");
        static_assert(sizeof(T) <= 0xFFFFu, "box::message::reply payload exceeds 65535 bytes");
        return reply(&v, static_cast<std::uint16_t>(sizeof(T)));
    }
};

// ── producer side ───────────────────────────────────────────────────────────
// Return true on accepted delivery. false collapses every route failure (pid 0 /
// target gone / ring full / no broadcast subscribers) — the native rc is not
// surfaced here (consistent with box::brook's friendly push/pop -> bool forms).
inline bool send(std::uint32_t pid, const void *data, std::uint16_t n) noexcept
{
    return ::send(pid, data, n) == 0;
}
template <class T>
inline bool send(std::uint32_t pid, const T &v) noexcept
{
    static_assert(std::is_trivially_copyable_v<T>,
                  "box::send(pid, T) requires a trivially copyable payload");
    static_assert(sizeof(T) <= 0xFFFFu, "box::send payload exceeds 65535 bytes");
    return ::send(pid, &v, static_cast<std::uint16_t>(sizeof(T))) == 0;
}
inline bool broadcast(const char *tag, const void *data, std::uint16_t n) noexcept
{
    return ::broadcast(tag, data, n) == 0;
}
template <class T>
inline bool broadcast(const char *tag, const T &v) noexcept
{
    static_assert(std::is_trivially_copyable_v<T>,
                  "box::broadcast(tag, T) requires a trivially copyable payload");
    static_assert(sizeof(T) <= 0xFFFFu, "box::broadcast payload exceeds 65535 bytes");
    return ::broadcast(tag, &v, static_cast<std::uint16_t>(sizeof(T))) == 0;
}

// ── consumer side ───────────────────────────────────────────────────────────
// Receiving is stateless: a process reads its own cabin ResultRing. No claim /
// registration (unlike box::touch). receive() is non-blocking; receive_for(ms)
// blocks up to ms (0 == forever).
inline std::optional<message> receive() noexcept
{
    Result r;
    if (::receive(&r)) return message(r);
    return std::nullopt;
}
inline std::optional<message> receive_for(std::uint32_t timeout_ms) noexcept
{
    Result r;
    if (::receive_wait(&r, timeout_ms)) return message(r);
    return std::nullopt;
}

// ── coroutine-native: co_await box::next_message() -> box::message ───────────
// Suspends on the current box::executor (same wait_on mechanism as the box::brook
// / box::touch awaiters); only the leaf primitive differs (receive / receive_wait
// vs brook / touch pop). With a single waiter the executor blocks forever on the
// native source; with several it cooperatively re-polls. A spurious resume yields
// an invalid message (operator bool == false), never a dereference.
class __message_awaiter {
public:
    __message_awaiter() noexcept = default;

    bool await_ready() noexcept { return (_M_got = ::receive(&_M_r)); }
    bool await_suspend(std::coroutine_handle<> __h)
    {
        executor::current()->wait_on({__h, this, &_S_poll, &_S_block});
        return true;
    }
    message await_resume() noexcept { return _M_got ? message(_M_r) : message(); }

private:
    static bool _S_poll(void *__s)
    {
        auto *__a = static_cast<__message_awaiter *>(__s);
        return __a->_M_got || (__a->_M_got = ::receive(&__a->_M_r));
    }
    static void _S_block(void *__s, std::uint32_t __ms)
    {
        auto *__a = static_cast<__message_awaiter *>(__s);
        if (!__a->_M_got) __a->_M_got = ::receive_wait(&__a->_M_r, __ms);
    }

    Result _M_r{};
    bool   _M_got = false;
};

inline __message_awaiter next_message() noexcept { return __message_awaiter{}; }

}  // namespace box

#endif  // BOXCXX_BOX_MESSAGE_H
