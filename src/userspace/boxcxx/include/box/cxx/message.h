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
#include "box/cxx/error.h"     // box::status / box::error

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

    // Reply to the sender (send a payload back to from()). Empty status on
    // success; the error arm carries the cause — process_not_found when this is
    // not a real delivery (no sender to reply to), or the recovered kernel
    // error_t from the underlying send (e.g. process_terminated if the sender has
    // since gone, result_ring_full).
    status reply(const void *data, std::uint16_t n) const noexcept
    {
        if (r_.sender_pid == 0) return std::unexpected(error{errc::process_not_found});
        return _detail::from_status(::send(r_.sender_pid, data, n));
    }
    template <class T>
    status reply(const T &v) const noexcept
    {
        static_assert(std::is_trivially_copyable_v<T>,
                      "box::message::reply(T) requires a trivially copyable payload");
        static_assert(sizeof(T) <= 0xFFFFu, "box::message::reply payload exceeds 65535 bytes");
        return reply(&v, static_cast<std::uint16_t>(sizeof(T)));
    }
};

// ── producer side ───────────────────────────────────────────────────────────
// Empty box::status on accepted delivery. Every route failure now SURFACES its
// real cause through the error arm instead of collapsing to false: the recovered
// kernel error_t (invalid_argument for a bad pid, process_terminated /
// process_not_found for a gone target, ring full, no broadcast subscribers) is
// in .error().code().
inline status send(std::uint32_t pid, const void *data, std::uint16_t n) noexcept
{
    return _detail::from_status(::send(pid, data, n));
}
template <class T>
inline status send(std::uint32_t pid, const T &v) noexcept
{
    static_assert(std::is_trivially_copyable_v<T>,
                  "box::send(pid, T) requires a trivially copyable payload");
    static_assert(sizeof(T) <= 0xFFFFu, "box::send payload exceeds 65535 bytes");
    return _detail::from_status(::send(pid, &v, static_cast<std::uint16_t>(sizeof(T))));
}
inline status broadcast(const char *tag, const void *data, std::uint16_t n) noexcept
{
    return _detail::from_status(::broadcast(tag, data, n));
}
template <class T>
inline status broadcast(const char *tag, const T &v) noexcept
{
    static_assert(std::is_trivially_copyable_v<T>,
                  "box::broadcast(tag, T) requires a trivially copyable payload");
    static_assert(sizeof(T) <= 0xFFFFu, "box::broadcast payload exceeds 65535 bytes");
    return _detail::from_status(::broadcast(tag, &v, static_cast<std::uint16_t>(sizeof(T))));
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
        executor::current()->wait_on(__h, this, &_S_poll, &_S_block,
                                     __exec::wait_domain::result);
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

// ── co_await box::result_any() -> box::any_result ───────────────────────────
// The ResultRing carries more than IPC into a strand: IPC messages (sender pid
// != 0) AND kernel records (sender pid == 0 — a manifest reply / kernel result
// from a VGA, storage or sync op this strand issued, or an addr_park
// completion). An IPC server or the display daemon must consume ALL of them on
// ONE await, not just IPC. result_any() is that await; like boxlib
// result_wait_any it does NOT filter — it hands back the next raw record for
// the caller to demux. This is EVENTS, not failures: it never yields a
// box::error (a box::message reply path is where failures surface).
class any_result {
    Result r_{};

public:
    any_result() noexcept = default;
    explicit any_result(const Result &r) noexcept : r_(r) {}

    // A genuine record arrived (vs a spurious resume on a drained ring): a zero
    // record carries no context, no sender and no payload.
    bool valid() const noexcept
    {
        return r_.context != KCTX_NONE || r_.sender_pid != 0 || r_.data_addr != 0;
    }
    explicit operator bool() const noexcept { return valid(); }

    // Demux faces — exactly one is meaningful per record.
    bool          is_ipc() const noexcept { return r_.sender_pid != 0; }
    bool          is_kernel() const noexcept { return r_.sender_pid == 0; }
    std::uint32_t context() const noexcept { return r_.context; }      // KCTX_*
    std::uint32_t error_code() const noexcept { return r_.error_code; }

    // The IPC face — an EMPTY (invalid) message when this is a kernel record;
    // reuses box::message's cross-cabin data_addr guard verbatim.
    message       as_message() const noexcept { return message(r_); }
    const Result &raw() const noexcept { return r_; }
};

// co_await box::result_any() — suspends on the current box::executor (Result
// domain) until ANY ResultRing record lands. Spurious-safe: a stray resume
// yields an invalid any_result, never a dereference.
class __result_any_awaiter {
public:
    __result_any_awaiter() noexcept = default;

    bool await_ready() noexcept { return (_M_got = _S_try(&_M_r)); }
    bool await_suspend(std::coroutine_handle<> __h)
    {
        // accept_any: this block (result_wait_any) consumes ANY ResultRing
        // record, so the executor prefers it as the collapsed-block
        // representative — an IPC-only sibling block would strand a kernel
        // record this awaiter is owed and hang it (Phase 3b contract).
        executor::current()->wait_on(__h, this, &_S_poll, &_S_block,
                                     __exec::wait_domain::result,
                                     /*deadline_tsc=*/0, /*accept_any=*/true);
        return true;
    }
    any_result await_resume() noexcept { return _M_got ? any_result(_M_r) : any_result(); }

private:
    // Non-blocking drain in result_wait_any's own order (ipc stash/ring ->
    // non-ipc stash/ring -> bare ring). NOT result_wait_any(., 0): a 0 timeout
    // there means "forever", which would hang the run-loop's poll sweep.
    static bool _S_try(Result *__out)
    {
        if (result_pop_ipc(__out)) return true;
        if (result_pop_non_ipc(__out)) return true;
        return result_pop(__out);
    }
    static bool _S_poll(void *__s)
    {
        auto *__a = static_cast<__result_any_awaiter *>(__s);
        return __a->_M_got || (__a->_M_got = _S_try(&__a->_M_r));
    }
    static void _S_block(void *__s, std::uint32_t __ms)
    {
        auto *__a = static_cast<__result_any_awaiter *>(__s);
        if (!__a->_M_got) __a->_M_got = result_wait_any(&__a->_M_r, __ms);
    }

    Result _M_r{};
    bool   _M_got = false;
};

inline __result_any_awaiter result_any() noexcept { return __result_any_awaiter{}; }

}  // namespace box

#endif  // BOXCXX_BOX_MESSAGE_H
