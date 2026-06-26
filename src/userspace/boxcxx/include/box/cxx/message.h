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

#include <array>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iterator>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>

#include "box/ipc.h"           // send / broadcast / receive / receive_wait + Result + send_args / receive_args
#include "box/cpu.h"           // cpu_rdtsc / cpu_ms_to_tsc (box::call deadline math)
#include "box/cxx/executor.h"  // box::executor, __exec::waiter, wait_on, box::task (co_await)
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

// ── box::call — send a request, await exactly ONE reply (the request/reply verb) ──
// A coroutine: it sends the request, then suspends on the current box::executor
// until the NEXT inbox record arrives or an optional deadline elapses. It is the
// request/reply shape over the same per-strand inbox box::message uses — NOT a
// Unix RPC: correlation is by strand-pid ARRIVAL, not a request id.
//
// Contract (the per-strand inbox is FIFO-shared — see box/cxx/line.h):
//   * box::call consumes EXACTLY ONE record — the next one. It does not drain
//     (which would discard a legitimately-queued earlier message) and does not
//     filter (which would steal a record a sibling consumer is owed).
//   * So keep at most ONE call in flight per strand and keep that strand's inbox
//     quiescent for the call's duration; VALIDATE from() on the returned message
//     (the callee replies via box::message::reply, i.e. ::send back to this
//     strand's pid, so a correct reply carries from() == the callee's pid).
//   * timeout_ms == 0 waits forever; otherwise an elapsed deadline with no reply
//     resolves to the error arm errc::timeout. Calling your own strand pid is not
//     a self-deadlock — the kernel rejects it as errc::route_self in the error arm.
class __call_awaiter {
public:
    explicit __call_awaiter(std::uint64_t __deadline_tsc) noexcept
        : _M_deadline_tsc(__deadline_tsc) {}

    bool await_ready() noexcept { return (_M_got = ::receive(&_M_r)); }
    bool await_suspend(std::coroutine_handle<> __h)
    {
        executor::current()->wait_on(__h, this, &_S_poll, &_S_block,
                                     __exec::wait_domain::result, _M_deadline_tsc);
        return true;
    }
    // A delivered record -> the reply; nullopt -> the deadline elapsed (timeout).
    std::optional<message> await_resume() noexcept
    {
        return _M_got ? std::optional<message>(message(_M_r)) : std::nullopt;
    }

private:
    static bool _S_poll(void *__s)
    {
        auto *__a = static_cast<__call_awaiter *>(__s);
        // Latch (mirrors __message_awaiter): once _S_block delivered a record, do
        // NOT re-receive — report it ready from _M_got.
        if (__a->_M_got) return true;
        if ((__a->_M_got = ::receive(&__a->_M_r))) return true;
        // The budget block expired with no reply: a lone result waiter would
        // otherwise re-block forever past its deadline. Report ready so
        // await_resume yields the timeout (nullopt).
        return __a->_M_deadline_tsc != 0 && cpu_rdtsc() >= __a->_M_deadline_tsc;
    }
    static void _S_block(void *__s, std::uint32_t __ms)
    {
        auto *__a = static_cast<__call_awaiter *>(__s);
        if (!__a->_M_got) __a->_M_got = ::receive_wait(&__a->_M_r, __ms);
    }

    Result        _M_r{};
    std::uint64_t _M_deadline_tsc;
    bool          _M_got = false;
};

// Send `n` bytes at `data` to `pid`, then await one reply. `data` must remain
// valid until the coroutine first runs (the internal send happens before the
// first suspension — the standard coroutine pointer-parameter lifetime rule).
inline task<result<message>> call(std::uint32_t pid, const void *data, std::uint16_t n,
                                  std::uint32_t timeout_ms = 0) noexcept
{
    status __s = box::send(pid, data, n);
    if (!__s) co_return std::unexpected(__s.error());
    std::uint64_t __deadline =
        timeout_ms ? cpu_rdtsc() + cpu_ms_to_tsc(static_cast<std::uint64_t>(timeout_ms)) : 0;
    std::optional<message> __m = co_await __call_awaiter{__deadline};
    if (!__m) co_return std::unexpected(error{errc::timeout});
    co_return *__m;
}
// Typed request overload. `req` is taken BY VALUE — it is copied into the
// coroutine frame, so it stays alive across the suspension even when the task is
// stored and awaited later (auto t = call(...); ... co_await t;). A reference
// parameter would dangle there: the internal send runs at first resume, by which
// point a temporary request would be long destroyed (CppCoreGuidelines CP.53).
// The `requires` excludes pointer requests so call(pid, ptr, len) cannot bind
// here — it routes unambiguously to the const void* overload above instead of
// silently shipping sizeof(void*) bytes of the pointer with len as the timeout.
template <class Req>
    requires (!std::is_pointer_v<Req>)
inline task<result<message>> call(std::uint32_t pid, Req req,
                                  std::uint32_t timeout_ms = 0) noexcept
{
    static_assert(std::is_trivially_copyable_v<Req>,
                  "box::call(pid, Req) requires a trivially copyable request");
    static_assert(sizeof(Req) <= 0xFFFFu, "box::call request exceeds 65535 bytes");
    status __s = box::send(pid, &req, static_cast<std::uint16_t>(sizeof(Req)));
    if (!__s) co_return std::unexpected(__s.error());
    std::uint64_t __deadline =
        timeout_ms ? cpu_rdtsc() + cpu_ms_to_tsc(static_cast<std::uint64_t>(timeout_ms)) : 0;
    std::optional<message> __m = co_await __call_awaiter{__deadline};
    if (!__m) co_return std::unexpected(error{errc::timeout});
    co_return *__m;
}

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

// ── box::args<N> / box::send_args / box::receive_args — the argv face of IPC ──
// The argv-style message: a fixed "[count][arg\0][arg\0]…" payload, the wire every
// BoxOS utility speaks (its main() reads char argv[16][64]). box::args<N> is that
// fixed value — NO heap, NUL-bounded views into fixed 64-byte cells — and the
// default N == 16 matches every util. This is a box:: extension, not part of std.
template <std::size_t N> class args;
template <std::size_t N = 16> result<args<N>> receive_args() noexcept;

template <std::size_t N = 16>
class args {
    static_assert(N >= 1, "box::args<N> needs at least one slot");

public:
    using value_type = std::string_view;

    // The number of populated arguments (clamped into [0, N]).
    std::size_t size() const noexcept
    {
        if (n_ <= 0) return 0u;
        return static_cast<std::size_t>(n_) > N ? N : static_cast<std::size_t>(n_);
    }
    bool empty() const noexcept { return size() == 0; }

    // The i-th argument as a NUL-bounded view into its fixed 64-byte cell. An
    // out-of-range index is an empty view — never an out-of-bounds read.
    std::string_view operator[](std::size_t i) const noexcept
    {
        if (i >= size()) return {};
        const char *p   = raw_[i].data();
        std::size_t len = 0;
        while (len < raw_[i].size() && p[len] != '\0') ++len;
        return std::string_view(p, len);
    }
    std::string_view front() const noexcept { return (*this)[0]; }
    std::string_view back()  const noexcept
    {
        return size() ? (*this)[size() - 1] : std::string_view{};
    }

    // Range-for: a forward iterator yielding string_view BY VALUE (each view is
    // synthesised on deref from the fixed cell).
    class iterator {
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type        = std::string_view;
        using difference_type   = std::ptrdiff_t;
        using reference         = std::string_view;
        using pointer           = void;

        iterator() noexcept = default;
        iterator(const args *__a, std::size_t __i) noexcept : _M_a(__a), _M_i(__i) {}

        std::string_view operator*() const noexcept { return (*_M_a)[_M_i]; }
        iterator        &operator++() noexcept { ++_M_i; return *this; }
        iterator         operator++(int) noexcept { iterator __t = *this; ++_M_i; return __t; }
        friend bool operator==(const iterator &__x, const iterator &__y) noexcept
        {
            return __x._M_i == __y._M_i;
        }

    private:
        const args *_M_a = nullptr;
        std::size_t _M_i = 0;
    };

    iterator begin() const noexcept { return iterator(this, 0); }
    iterator end()   const noexcept { return iterator(this, size()); }

private:
    template <std::size_t M> friend result<args<M>> receive_args() noexcept;

    std::array<std::array<char, 64>, N> raw_{};
    int                                 n_ = 0;
};

// Marshal the args into the "[count][arg\0]…" wire and route to `pid`. Empty
// status on accepted delivery; the error arm carries the real send cause. Maps
// the int return by the from_ret rule (a non-negative return is an accept, a
// negative return is the -error_t cause — the value is discarded). Honors the
// boxlib caps (<=127 args, the 240-byte wire ceiling): an arg past the ceiling is
// dropped exactly as the C marshaller drops it. An argument containing an embedded
// NUL is truncated at the first NUL (the argv wire is NUL-delimited).
inline status send_args(std::uint32_t pid, std::span<const std::string_view> a) noexcept
{
    constexpr std::size_t kMaxArgs = 127;   // ipc.c caps the arg count at 127
    constexpr std::size_t kScratch = 256;   // > ipc.c's 240-byte wire buffer
    char        scratch[kScratch];
    char       *argv[kMaxArgs];
    std::size_t pos  = 0;
    int         argc = 0;
    for (std::size_t i = 0; i < a.size() && static_cast<std::size_t>(argc) < kMaxArgs; ++i) {
        if (pos + 1 >= kScratch) break;             // no room left even for a NUL
        std::size_t room = kScratch - 1 - pos;      // chars we can take (keep 1 for the NUL)
        std::size_t len  = a[i].size() < room ? a[i].size() : room;
        if (len) __builtin_memcpy(scratch + pos, a[i].data(), len);
        scratch[pos + len] = '\0';
        argv[argc++]       = scratch + pos;
        pos               += len + 1;
    }
    int r = ::send_args(pid, argc, argv);
    if (r < 0) return std::unexpected(error{box_errno_of(r)});
    return {};
}
inline status send_args(std::uint32_t pid, std::initializer_list<std::string_view> a) noexcept
{
    return send_args(pid, std::span<const std::string_view>(a.begin(), a.size()));
}

// Receive the next "[count][arg\0]…" payload into a fixed args<N>. Fallible: the
// error arm carries the recovered cause (errc::timeout when no message arrives in
// the boxlib inbox wait, errc::internal on a malformed payload); the value arm is
// the populated args. Calls straight through ::receive_args, preserving its
// context-tag side effect. The boxlib inbox wait is bounded (~1000 ms, a C-layer
// constant) — receive_args cannot block forever; a not-yet-arrived payload
// surfaces as errc::timeout, never a hang.
template <std::size_t N>
inline result<args<N>> receive_args() noexcept
{
    args<N> out;
    int r = ::receive_args(&out.n_,
                           reinterpret_cast<char (*)[64]>(out.raw_.data()),
                           static_cast<int>(N));
    if (r < 0) return std::unexpected(error{box_errno_of(r)});
    if (out.n_ < 0) out.n_ = 0;
    if (out.n_ > static_cast<int>(N)) out.n_ = static_cast<int>(N);
    return out;
}

}  // namespace box

#endif  // BOXCXX_BOX_MESSAGE_H
