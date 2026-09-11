#ifndef BOXCXX_BOX_STRAND_H
#define BOXCXX_BOX_STRAND_H

#include <atomic>
#include <chrono>
#include <compare>
#include <cstdint>
#include <format>
#include <functional>
#include <optional>
#include <thread>
#include <type_traits>
#include <utility>

#include "box/sync.h"
#include "box/error.h"
#include "box/core/strand_self.h"
#include "box/cxx/touch.h"
#include "box/cxx/executor.h"
#include <coroutine>
#include <system_error>
#include <exception>
#include <tuple>

#include "box/strand.h"
#include "box/cxx/tls_strand.h"

namespace box {

enum class park_status {
    woke,
    timeout,
    value_mismatch,
    error,
};

namespace _strand_detail {
inline park_status map_park(error_t e) noexcept
{
    switch (e) {
    case OK:                      return park_status::woke;
    case ERR_TIMEOUT:             return park_status::timeout;
    case ERR_ADDR_VALUE_MISMATCH: return park_status::value_mismatch;
    default:                      return park_status::error;
    }
}
}

template <class T>
inline park_status park(const std::atomic<T> &a, T expected,
                        std::uint32_t timeout_ms = 0) noexcept
{
    static_assert(sizeof(T) == 8,
                  "box::park: the kernel compares a full 64-bit word — use an "
                  "8-byte atomic (uint64_t/int64_t/pointer); for narrower per-"
                  "object waits use std::atomic<T>::wait/notify");
    static_assert(std::is_trivially_copyable_v<T>,
                  "box::park requires a trivially copyable atomic value type");
    static_assert(std::has_unique_object_representations_v<T>,
                  "box::park: T must have no padding/trap bits (use uint64_t/"
                  "int64_t/a pointer — not floating-point or a padded struct)");
    std::uint64_t word;
    __builtin_memcpy(&word, &expected, 8);
    return _strand_detail::map_park(addr_park(&a, word, timeout_ms));
}

template <class T>
inline bool wake(const std::atomic<T> &a, std::uint32_t count = 0) noexcept
{
    static_assert(sizeof(T) == 8,
                  "box::wake: 8-byte atomic only (matches box::park's kernel word)");
    static_assert(std::has_unique_object_representations_v<T>,
                  "box::wake: 8-byte, padding-free atomic only (symmetric with box::park)");
    return addr_wake(&a, count) == OK;
}

inline park_status park(const volatile void *addr, std::uint64_t expected,
                        std::uint32_t timeout_ms = 0) noexcept
{
    return _strand_detail::map_park(addr_park(addr, expected, timeout_ms));
}
inline bool wake(const volatile void *addr, std::uint32_t count = 0) noexcept
{
    return addr_wake(addr, count) == OK;
}

template <class Rep, class Period>
inline park_status park_for(const std::atomic<std::uint64_t> &a, std::uint64_t expected,
                            std::chrono::duration<Rep, Period> d) noexcept
{
    if (d <= std::chrono::duration<Rep, Period>::zero())
        return park_status::timeout;
    auto ms = std::chrono::ceil<std::chrono::milliseconds>(d).count();
    if (ms < 1) ms = 1;
    if (ms > 0xFFFFFFFFLL) ms = 0xFFFFFFFFLL;
    return park(a, expected, static_cast<std::uint32_t>(ms));
}

template <class T> inline bool wake_one(const std::atomic<T> &a) noexcept { return wake(a, 1); }
template <class T> inline bool wake_all(const std::atomic<T> &a) noexcept { return wake(a, 0); }
inline bool wake_one(const volatile void *addr) noexcept { return wake(addr, 1); }
inline bool wake_all(const volatile void *addr) noexcept { return wake(addr, 0); }

class completion_await {
public:
    explicit completion_await(volatile std::uint64_t *__flag) noexcept
        : _M_flag(__flag) {}

    bool await_ready() {
        if (!_M_flag)
            throw std::system_error(
                std::make_error_code(std::errc::invalid_argument),
                "co_await box::strand::completion: not joinable");
        return __atomic_load_n(_M_flag, __ATOMIC_ACQUIRE) == 1;
    }

    bool await_suspend(std::coroutine_handle<> __h) {
        executor::current()->wait_on(__h, this, &_S_poll, &_S_block,
                                     __exec::wait_domain::join);
        return true;
    }

    void await_resume() const noexcept {}

private:
    static bool _S_poll(void *__s) {
        auto *__a = static_cast<completion_await *>(__s);
        return __atomic_load_n(__a->_M_flag, __ATOMIC_ACQUIRE) == 1;
    }
    static void _S_block(void *__s, std::uint32_t __ms) {
        auto *__a = static_cast<completion_await *>(__s);
        std::uint64_t __s0 = __atomic_load_n(__a->_M_flag, __ATOMIC_ACQUIRE);
        if (__s0 != 1)
            addr_park(__a->_M_flag, __s0, __ms ? __ms : 0);
    }

    volatile std::uint64_t *_M_flag;
};

class strand {
public:
    using native_handle_type = std::thread::native_handle_type;

    class id {
    public:
        id() noexcept = default;

        friend bool operator==(id a, id b) noexcept { return a.pid_ == b.pid_; }
        friend std::strong_ordering operator<=>(id a, id b) noexcept
        {
            return a.pid_ <=> b.pid_;
        }

        std::uint32_t native() const noexcept { return pid_; }

        explicit operator std::thread::id() const noexcept
        {
            return std::thread::MakeId(pid_);
        }

    private:
        friend class strand;
        friend struct std::hash<id>;
        template <class T, class CharT> friend struct std::formatter;
        explicit id(std::uint32_t pid) noexcept : pid_(pid) {}
        std::uint32_t pid_ = 0;
    };

    strand() noexcept = default;

    template <class F, class... Args,
              class = std::enable_if_t<!std::is_same_v<std::remove_cvref_t<F>, strand>>>
    explicit strand(F &&f, Args &&...args)
        : t_(std::forward<F>(f), std::forward<Args>(args)...)
    {
    }

    strand(const strand &)            = delete;
    strand &operator=(const strand &) = delete;

    strand(strand &&) noexcept = default;

    strand &operator=(strand &&o) noexcept
    {
        if (this != &o) {
            if (joinable()) join_nothrow();
            t_ = std::move(o.t_);
        }
        return *this;
    }

    ~strand()
    {
        if (joinable()) join_nothrow();
    }

    bool joinable() const noexcept { return t_.joinable(); }
    id   get_id() const noexcept { return id{t_.native_handle()}; }
    native_handle_type native_handle() const noexcept { return t_.native_handle(); }

    static id make_id(std::uint32_t pid) noexcept { return id{pid}; }

    void join() { t_.join(); }
    void detach() { t_.detach(); }

    completion_await completion() const noexcept
    {
        return completion_await{t_._M_completion_word()};
    }

    void swap(strand &o) noexcept { t_.swap(o.t_); }

    static unsigned hardware_concurrency() noexcept
    {
        return std::thread::hardware_concurrency();
    }

private:
    void join_nothrow() noexcept
    {
        try {
            t_.join();
        } catch (...) {
        }
    }

    std::thread t_{};
};

inline void swap(strand &a, strand &b) noexcept { a.swap(b); }

struct strand_info {
    strand::id id;
    bool       is_main;
};

namespace _strand_detail {
template <class Closure>
inline void detached_trampoline(void *p)
{
    auto *c = static_cast<Closure *>(p);
    __boxcxx_tls_strand_init();
    __boxcxx_thread_storage_enter();
    try {
        {
            auto call = std::move(c->call);
            delete c;
            std::apply(
                [](auto &&...a) { std::invoke(static_cast<decltype(a)>(a)...); },
                std::move(call));
        }
        __boxcxx_thread_storage_exit();
    } catch (...) {
        std::terminate();
    }
    strand_exit();
}
}

template <class F, class... Args>
inline strand::id spawn_detached(F &&f, Args &&...args)
{
    using Tup = std::tuple<std::decay_t<F>, std::decay_t<Args>...>;
    struct Closure { Tup call; };
    Closure *c = new Closure{Tup{std::forward<F>(f), std::forward<Args>(args)...}};
    std::uint32_t pid = strand_spawn(&_strand_detail::detached_trampoline<Closure>, c);
    if (pid == 0) {
        delete c;
        throw std::system_error(
            std::make_error_code(std::errc::resource_unavailable_try_again),
            "box::spawn_detached: strand_spawn failed");
    }
    return strand::make_id(pid);
}

namespace this_strand {
inline strand::id id() noexcept { return strand::make_id(strand_self()); }
inline void       yield() noexcept { ::yield(); }

inline bool is_main() noexcept { return strand_info_or_null() == nullptr; }

inline strand_info info() noexcept { return strand_info{id(), is_main()}; }

[[noreturn]] inline void exit() noexcept
{
    if (is_main())
        std::terminate();
    __boxcxx_thread_storage_exit();
    strand_exit();
}
}


struct strand_spawned {
    std::uint32_t pid;
    std::uint32_t cabin_pid;
};
struct strand_exited {
    std::uint32_t pid;
    std::uint32_t cabin_pid;
};

struct __attribute__((packed)) strand_parked {
    std::uint64_t phys;
    std::uint32_t pid;
};

struct __attribute__((packed)) strand_woken {
    std::uint64_t phys;
    std::uint32_t pid;
    std::uint32_t waker_pid;
};

static_assert(sizeof(strand_spawned) == 8,  "strand_spawned must match the 8-byte kernel payload");
static_assert(sizeof(strand_exited)  == 8,  "strand_exited must match the 8-byte kernel payload");
static_assert(sizeof(strand_parked)  == 12, "strand_parked must be packed to 12 bytes (kernel payload)");
static_assert(sizeof(strand_woken)   == 16, "strand_woken must be packed to 16 bytes (kernel payload)");

enum class strand_touch_kind { spawned, exited, parked, woken };

struct strand_touch {
    strand_touch_kind kind;
    std::uint32_t     source;
    union {
        strand_spawned spawned;
        strand_exited  exited;
        strand_parked  parked;
        strand_woken   woken;
    };

    std::uint32_t strand_pid() const noexcept
    {
        switch (kind) {
        case strand_touch_kind::spawned: return spawned.pid;
        case strand_touch_kind::exited:  return exited.pid;
        case strand_touch_kind::parked:  return parked.pid;
        case strand_touch_kind::woken:   return woken.pid;
        }
        return 0;
    }
};

class strand_watch {
public:
    strand_watch() noexcept
        : spawned_("strand:spawned"_tag),
          exited_("strand:exited"_tag),
          parked_("strand:parked"_tag),
          woken_("strand:woken"_tag)
    {
    }

    explicit operator bool() const noexcept
    {
        return spawned_ && exited_ && parked_ && woken_;
    }

    void set_filter_cabin(std::uint32_t spawner_pid) noexcept { filter_ = spawner_pid; }

    std::optional<strand_touch> poll() noexcept
    {
        if (auto e = poll_spawned()) return e;
        if (auto e = poll_exited())  return e;
        if (auto e = poll_parked())  return e;
        if (auto e = poll_woken())   return e;
        return std::nullopt;
    }

    std::optional<strand_touch> wait(std::uint32_t ms = 0) noexcept
    {
        if (auto e = poll()) return e;
        constexpr std::uint32_t step = 16;
        std::uint32_t waited = 0;
        for (;;) {
            std::uint32_t slice = (ms == 0) ? step
                                            : ((ms - waited < step) ? (ms - waited) : step);
            if (slice == 0) slice = 1;
            if (auto e = wait_one(spawned_, strand_touch_kind::spawned, slice)) return e;
            if (auto e = wait_one(exited_,  strand_touch_kind::exited,  slice)) return e;
            if (auto e = wait_one(parked_,  strand_touch_kind::parked,  slice)) return e;
            if (auto e = wait_one(woken_,   strand_touch_kind::woken,   slice)) return e;
            if (ms != 0) {
                waited += slice * 4;
                if (waited >= ms) return std::nullopt;
            }
        }
    }

    class next_awaiter {
    public:
        explicit next_awaiter(strand_watch *w) noexcept : _M_w(w) {}

        bool await_ready() noexcept
        {
            _M_ev = _M_w->poll();
            return _M_ev.has_value();
        }
        bool await_suspend(std::coroutine_handle<> __h)
        {
            executor::current()->wait_on(__h, this, &_S_poll, &_S_block,
                                         __exec::wait_domain::touch);
            return true;
        }
        std::optional<strand_touch> await_resume() noexcept { return _M_ev; }

    private:
        static bool _S_poll(void *__s)
        {
            auto *__a = static_cast<next_awaiter *>(__s);
            if (__a->_M_ev) return true;
            __a->_M_ev = __a->_M_w->poll();
            return __a->_M_ev.has_value();
        }
        static void _S_block(void *__s, std::uint32_t __ms)
        {
            auto *__a = static_cast<next_awaiter *>(__s);
            if (__a->_M_ev) return;
            __a->_M_ev = __a->_M_w->wait_round(__ms);
        }

        strand_watch               *_M_w;
        std::optional<strand_touch> _M_ev{};
    };

    next_awaiter next() noexcept { return next_awaiter{this}; }

private:
    std::optional<strand_touch> wait_round(std::uint32_t ms) noexcept
    {
        constexpr std::uint32_t step = 16;
        std::uint32_t slice = (ms == 0) ? step : (ms / 4);
        if (slice == 0) slice = 1;
        if (auto e = wait_one(spawned_, strand_touch_kind::spawned, slice)) return e;
        if (auto e = wait_one(exited_,  strand_touch_kind::exited,  slice)) return e;
        if (auto e = wait_one(parked_,  strand_touch_kind::parked,  slice)) return e;
        if (auto e = wait_one(woken_,   strand_touch_kind::woken,   slice)) return e;
        return std::nullopt;
    }

    std::optional<strand_touch> decode_spawned(const touch &ev) noexcept
    {
        auto p = ev.payload_as<strand_spawned>();
        if (!p) return std::nullopt;
        if (filter_ && p->cabin_pid != filter_) return std::nullopt;
        strand_touch e{strand_touch_kind::spawned, ev.source(), {}};
        e.spawned = *p;
        return e;
    }
    std::optional<strand_touch> decode_exited(const touch &ev) noexcept
    {
        auto p = ev.payload_as<strand_exited>();
        if (!p) return std::nullopt;
        if (filter_ && p->cabin_pid != filter_) return std::nullopt;
        strand_touch e{strand_touch_kind::exited, ev.source(), {}};
        e.exited = *p;
        return e;
    }
    std::optional<strand_touch> decode_parked(const touch &ev) noexcept
    {
        auto p = ev.payload_as<strand_parked>();
        if (!p) return std::nullopt;
        strand_touch e{strand_touch_kind::parked, ev.source(), {}};
        e.parked = *p;
        return e;
    }
    std::optional<strand_touch> decode_woken(const touch &ev) noexcept
    {
        auto p = ev.payload_as<strand_woken>();
        if (!p) return std::nullopt;
        strand_touch e{strand_touch_kind::woken, ev.source(), {}};
        e.woken = *p;
        return e;
    }

    std::optional<strand_touch> poll_spawned() noexcept
    {
        if (auto ev = spawned_.poll()) return decode_spawned(*ev);
        return std::nullopt;
    }
    std::optional<strand_touch> poll_exited() noexcept
    {
        if (auto ev = exited_.poll()) return decode_exited(*ev);
        return std::nullopt;
    }
    std::optional<strand_touch> poll_parked() noexcept
    {
        if (auto ev = parked_.poll()) return decode_parked(*ev);
        return std::nullopt;
    }
    std::optional<strand_touch> poll_woken() noexcept
    {
        if (auto ev = woken_.poll()) return decode_woken(*ev);
        return std::nullopt;
    }

    std::optional<strand_touch> wait_one(subscription &sub, strand_touch_kind kind,
                                         std::uint32_t slice) noexcept
    {
        if (auto ev = sub.wait(slice)) {
            switch (kind) {
            case strand_touch_kind::spawned: return decode_spawned(*ev);
            case strand_touch_kind::exited:  return decode_exited(*ev);
            case strand_touch_kind::parked:  return decode_parked(*ev);
            case strand_touch_kind::woken:   return decode_woken(*ev);
            }
        }
        return std::nullopt;
    }

    subscription  spawned_;
    subscription  exited_;
    subscription  parked_;
    subscription  woken_;
    std::uint32_t filter_ = 0;
};

}

template <>
struct std::hash<box::strand::id> {
    std::size_t operator()(box::strand::id v) const noexcept
    {
        return std::hash<std::uint32_t>{}(v.pid_);
    }
};

template <>
struct std::formatter<box::strand::id, char> : std::formatter<std::uint32_t, char> {
    auto format(box::strand::id v, std::format_context &ctx) const
    {
        return std::formatter<std::uint32_t, char>::format(v.pid_, ctx);
    }
};

#endif