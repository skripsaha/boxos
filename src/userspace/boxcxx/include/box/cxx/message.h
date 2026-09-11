#ifndef BOXCXX_BOX_MESSAGE_H
#define BOXCXX_BOX_MESSAGE_H

#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>

#include "box/ipc.h"
#include "box/cpu.h"
#include "box/cxx/executor.h"
#include "box/cxx/error.h"

namespace box {

class message {
    Result r_{};

public:
    message() noexcept = default;
    explicit message(const Result &r) noexcept : r_(r) {}

    bool valid() const noexcept { return r_.sender_pid != 0; }
    explicit operator bool() const noexcept { return valid(); }

    std::uint32_t from() const noexcept { return r_.sender_pid; }
    std::uint32_t size() const noexcept { return r_.data_length; }
    const Result &raw() const noexcept { return r_; }

    std::span<const std::byte> bytes() const noexcept
    {
        if (r_.sender_pid == 0 || r_.data_addr == 0 || r_.data_length == 0) return {};
        return {reinterpret_cast<const std::byte *>(static_cast<std::uintptr_t>(r_.data_addr)),
                r_.data_length};
    }

    std::string_view text() const noexcept
    {
        std::span<const std::byte> b = bytes();
        return {reinterpret_cast<const char *>(b.data()), b.size()};
    }

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
    std::optional<message> await_resume() noexcept
    {
        return _M_got ? std::optional<message>(message(_M_r)) : std::nullopt;
    }

private:
    static bool _S_poll(void *__s)
    {
        auto *__a = static_cast<__call_awaiter *>(__s);
        if (__a->_M_got) return true;
        if ((__a->_M_got = ::receive(&__a->_M_r))) return true;
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

class any_result {
    Result r_{};

public:
    any_result() noexcept = default;
    explicit any_result(const Result &r) noexcept : r_(r) {}

    bool valid() const noexcept
    {
        return r_.context != KCTX_NONE || r_.sender_pid != 0 || r_.data_addr != 0;
    }
    explicit operator bool() const noexcept { return valid(); }

    bool          is_ipc() const noexcept { return r_.sender_pid != 0; }
    bool          is_kernel() const noexcept { return r_.sender_pid == 0; }
    std::uint32_t context() const noexcept { return r_.context; }
    std::uint32_t error_code() const noexcept { return r_.error_code; }

    message       as_message() const noexcept { return message(r_); }
    const Result &raw() const noexcept { return r_; }
};

class __result_any_awaiter {
public:
    __result_any_awaiter() noexcept = default;

    bool await_ready() noexcept { return (_M_got = _S_try(&_M_r)); }
    bool await_suspend(std::coroutine_handle<> __h)
    {
        executor::current()->wait_on(__h, this, &_S_poll, &_S_block,
                                     __exec::wait_domain::result,
                                     0, true);
        return true;
    }
    any_result await_resume() noexcept { return _M_got ? any_result(_M_r) : any_result(); }

private:
    static bool _S_try(Result *__out)
    {
        return result_pop_any(__out);
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

}

#endif