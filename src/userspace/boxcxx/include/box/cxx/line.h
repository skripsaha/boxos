#ifndef BOXCXX_BOX_LINE_H
#define BOXCXX_BOX_LINE_H

#include <coroutine>
#include <cstdint>
#include <optional>
#include <type_traits>

#include "box/ipc.h"
#include "box/cxx/error.h"
#include "box/cxx/executor.h"
#include "box/cxx/message.h"

namespace box {

template <class T>
class line {
    static_assert(std::is_trivially_copyable_v<T>,
                  "box::line<T> payload must be trivially copyable");
    static_assert(std::is_default_constructible_v<T>,
                  "box::line<T> payload must be default-constructible (decode storage)");
    static_assert(sizeof(T) >= 1 && sizeof(T) <= 0xFFFFu,
                  "box::line<T> payload size must be in [1, 65535] bytes");

public:
    using value_type = T;

    line() noexcept = default;

    static line to(std::uint32_t peer_pid) noexcept
    {
        line l;
        l.peer_ = peer_pid;
        return l;
    }
    static line on(const char *group_tag) noexcept
    {
        line l;
        l.group_ = group_tag;
        return l;
    }

    explicit operator bool() const noexcept { return peer_ != 0 || group_ != nullptr; }
    std::uint32_t peer()  const noexcept { return peer_; }
    const char   *group() const noexcept { return group_; }

    status send(const T &v) const noexcept
    {
        if (group_) return box::broadcast(group_, v);
        return box::send(peer_, v);
    }

    std::optional<T> try_recv() const noexcept
    {
        Result r;
        if (!::receive(&r)) return std::nullopt;
        return message(r).payload_as<T>();
    }
    std::optional<T> recv_for(std::uint32_t ms) const noexcept
    {
        Result r;
        if (!::receive_wait(&r, ms)) return std::nullopt;
        return message(r).payload_as<T>();
    }

    class recv_awaiter {
    public:
        recv_awaiter() noexcept = default;

        bool await_ready() noexcept { return (_M_got = ::receive(&_M_r)); }
        bool await_suspend(std::coroutine_handle<> __h)
        {
            executor::current()->wait_on(__h, this, &_S_poll, &_S_block,
                                         __exec::wait_domain::result);
            return true;
        }
        std::optional<T> await_resume() noexcept
        {
            return _M_got ? message(_M_r).payload_as<T>() : std::nullopt;
        }

    private:
        static bool _S_poll(void *__s)
        {
            auto *__a = static_cast<recv_awaiter *>(__s);
            return __a->_M_got || (__a->_M_got = ::receive(&__a->_M_r));
        }
        static void _S_block(void *__s, std::uint32_t __ms)
        {
            auto *__a = static_cast<recv_awaiter *>(__s);
            if (!__a->_M_got) __a->_M_got = ::receive_wait(&__a->_M_r, __ms);
        }

        Result _M_r{};
        bool   _M_got = false;
    };

    recv_awaiter recv() const noexcept { return recv_awaiter{}; }

private:
    std::uint32_t peer_  = 0;
    const char   *group_ = nullptr;
};

}

#endif