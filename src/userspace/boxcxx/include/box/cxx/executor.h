#ifndef BOXCXX_BOX_EXECUTOR_H
#define BOXCXX_BOX_EXECUTOR_H

#include <coroutine>
#include <vector>
#include <utility>
#include <exception>
#include <cstdint>
#include <type_traits>
#include <chrono>

#include <box/touch.h>
#include <box/brook.h>
#include <box/ipc.h>
#include <box/core/result.h>
#include <box/error.h>
#include <box/sync.h>
#include <box/cpu.h>

namespace box {

template <typename T>
class task;

namespace __exec {

struct promise_base {
    std::coroutine_handle<> _M_continuation{std::noop_coroutine()};
    std::exception_ptr _M_exc{};

    std::suspend_always initial_suspend() const noexcept { return {}; }

    struct final_awaiter {
        bool await_ready() const noexcept { return false; }
        template <typename _P>
        std::coroutine_handle<> await_suspend(
            std::coroutine_handle<_P> __h) const noexcept {
            return __h.promise()._M_continuation;
        }
        void await_resume() const noexcept {}
    };

    final_awaiter final_suspend() noexcept { return {}; }
    void unhandled_exception() noexcept { _M_exc = std::current_exception(); }
};

}

template <typename T>
class task {
public:
    struct promise_type : __exec::promise_base {
        union {
            T _M_value;
        };
        bool _M_has = false;

        promise_type() noexcept {}
        ~promise_type() {
            if (_M_has) _M_value.~T();
        }

        task get_return_object() noexcept {
            return task{std::coroutine_handle<promise_type>::from_promise(*this)};
        }

        template <typename U = T>
        void return_value(U&& __v) noexcept(
            std::is_nothrow_constructible_v<T, U>) {
            ::new (static_cast<void*>(&_M_value)) T(std::forward<U>(__v));
            _M_has = true;
        }
    };

    using handle_type = std::coroutine_handle<promise_type>;

    task() noexcept = default;
    task(const task&) = delete;
    task& operator=(const task&) = delete;

    task(task&& __o) noexcept : _M_coro(std::exchange(__o._M_coro, {})) {}
    task& operator=(task&& __o) noexcept {
        if (this != &__o) {
            if (_M_coro) _M_coro.destroy();
            _M_coro = std::exchange(__o._M_coro, {});
        }
        return *this;
    }
    ~task() {
        if (_M_coro) _M_coro.destroy();
    }

    bool await_ready() const noexcept { return !_M_coro || _M_coro.done(); }
    handle_type await_suspend(std::coroutine_handle<> __awaiting) noexcept {
        _M_coro.promise()._M_continuation = __awaiting;
        return _M_coro;
    }
    T await_resume() { return _M_take(); }

    T result() { return _M_take(); }

    bool done() const noexcept { return !_M_coro || _M_coro.done(); }
    std::coroutine_handle<> handle() const noexcept { return _M_coro; }
    handle_type release() noexcept { return std::exchange(_M_coro, {}); }

private:
    friend class executor;
    explicit task(handle_type __h) noexcept : _M_coro(__h) {}

    T _M_take() {
        if (_M_coro && _M_coro.promise()._M_exc)
            std::rethrow_exception(_M_coro.promise()._M_exc);
        if (!_M_coro || !_M_coro.promise()._M_has) __builtin_trap();
        return std::move(_M_coro.promise()._M_value);
    }

    handle_type _M_coro{};
};

template <>
class task<void> {
public:
    struct promise_type : __exec::promise_base {
        task get_return_object() noexcept {
            return task{std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        void return_void() const noexcept {}
    };

    using handle_type = std::coroutine_handle<promise_type>;

    task() noexcept = default;
    task(const task&) = delete;
    task& operator=(const task&) = delete;

    task(task&& __o) noexcept : _M_coro(std::exchange(__o._M_coro, {})) {}
    task& operator=(task&& __o) noexcept {
        if (this != &__o) {
            if (_M_coro) _M_coro.destroy();
            _M_coro = std::exchange(__o._M_coro, {});
        }
        return *this;
    }
    ~task() {
        if (_M_coro) _M_coro.destroy();
    }

    bool await_ready() const noexcept { return !_M_coro || _M_coro.done(); }
    handle_type await_suspend(std::coroutine_handle<> __awaiting) noexcept {
        _M_coro.promise()._M_continuation = __awaiting;
        return _M_coro;
    }
    void await_resume() { _M_take(); }
    void result() { _M_take(); }

    bool done() const noexcept { return !_M_coro || _M_coro.done(); }
    std::coroutine_handle<> handle() const noexcept { return _M_coro; }
    handle_type release() noexcept { return std::exchange(_M_coro, {}); }

private:
    friend class executor;
    explicit task(handle_type __h) noexcept : _M_coro(__h) {}

    void _M_take() {
        if (_M_coro && _M_coro.promise()._M_exc)
            std::rethrow_exception(_M_coro.promise()._M_exc);
    }

    handle_type _M_coro{};
};

namespace __exec {

enum class wait_domain : std::uint8_t {
    touch  = 0,
    result = 1,
    brook  = 2,
    timer  = 3,
    join   = 4,
};
inline constexpr std::size_t wait_domain_count = 5;

constexpr bool _S_reschedules(wait_domain __d) noexcept {
    return __d == wait_domain::result || __d == wait_domain::timer
        || __d == wait_domain::join;
}
constexpr bool _S_collapsible(wait_domain __d) noexcept {
    return __d == wait_domain::result || __d == wait_domain::timer;
}

struct waiter {
    std::coroutine_handle<> _M_h;
    void* _M_self;
    bool (*_M_poll)(void*);
    void (*_M_block)(void*, std::uint32_t);
    wait_domain   _M_domain;
    std::uint64_t _M_deadline_tsc;
    bool          _M_accept_any;
};

}

class executor {
public:
    executor() = default;
    executor(const executor&) = delete;
    executor& operator=(const executor&) = delete;

    ~executor() {
        for (auto __h : _M_owned)
            if (__h) __h.destroy();
    }

    static executor* current() noexcept { return _S_current; }

    void schedule(std::coroutine_handle<> __h) { _M_ready.push_back(__h); }

    void wait_on(__exec::waiter __w) { _M_waiting.push_back(__w); }
    void wait_on(std::coroutine_handle<> __h, void* __self,
                 bool (*__poll)(void*), void (*__block)(void*, std::uint32_t),
                 __exec::wait_domain __d, std::uint64_t __deadline_tsc = 0,
                 bool __accept_any = false) {
        _M_waiting.push_back(
            {__h, __self, __poll, __block, __d, __deadline_tsc, __accept_any});
    }

    template <typename T>
    void spawn(task<T> __t) {
        auto __h = __t.release();
        _M_owned.push_back(__h);
        _M_ready.push_back(__h);
    }

    void run() {
        struct ScopeGuard {
            executor* __ex;
            executor* __prev;
            ~ScopeGuard() {
                for (auto __h : __ex->_M_owned)
                    if (__h) __h.destroy();
                __ex->_M_owned.clear();
                _S_current = __prev;
            }
        } __guard{this, _S_current};
        _S_current = this;
        while (!_M_ready.empty() || !_M_waiting.empty()) {
            while (!_M_ready.empty()) {
                auto __h = _M_ready.back();
                _M_ready.pop_back();
                if (__h && !__h.done()) __h.resume();
            }
            if (!_M_waiting.empty()) _M_pump_waiters();
        }
    }

    template <typename T>
    T block_on(task<T> __t) {
        schedule(__t.handle());
        run();
        return __t.result();
    }

private:
    static constexpr std::uint32_t _S_cross_slice_ms = 16;

    bool _M_poll_sweep() {
        bool __any = false;
        for (std::size_t __i = 0; __i < _M_waiting.size();) {
            if (_M_waiting[__i]._M_poll(_M_waiting[__i]._M_self)) {
                _M_ready.push_back(_M_waiting[__i]._M_h);
                _M_waiting[__i] = _M_waiting.back();
                _M_waiting.pop_back();
                __any = true;
            } else {
                ++__i;
            }
        }
        return __any;
    }

    std::uint32_t _M_block_budget_ms() const {
        std::uint64_t __min_deadline = 0;
        for (const auto& __w : _M_waiting) {
            if (__w._M_deadline_tsc &&
                (__min_deadline == 0 || __w._M_deadline_tsc < __min_deadline))
                __min_deadline = __w._M_deadline_tsc;
        }
        if (__min_deadline == 0) return 0;
        std::uint64_t __now = cpu_rdtsc();
        if (__min_deadline <= __now) return 1;
        std::uint64_t __ms = cpu_tsc_to_ms(__min_deadline - __now);
        if (__ms == 0) __ms = 1;
        if (__ms > 0xFFFFFFFFull) __ms = 0xFFFFFFFFull;
        return static_cast<std::uint32_t>(__ms);
    }

    void _M_pump_waiters() {
        if (_M_poll_sweep() || !_M_ready.empty() || _M_waiting.empty())
            return;

        const std::uint32_t __budget = _M_block_budget_ms();

        if (_M_waiting.size() == 1 &&
            __exec::_S_reschedules(_M_waiting[0]._M_domain)) {
            _M_waiting[0]._M_block(_M_waiting[0]._M_self, __budget);
            return;
        }

        bool __collapsible = true;
        for (const auto& __w : _M_waiting)
            if (!__exec::_S_collapsible(__w._M_domain)) { __collapsible = false; break; }
        if (__collapsible) {
            std::size_t __rep = _M_waiting.size();
            for (std::size_t __i = 0; __i < _M_waiting.size(); ++__i)
                if (_M_waiting[__i]._M_domain == __exec::wait_domain::result &&
                    _M_waiting[__i]._M_accept_any) { __rep = __i; break; }
            if (__rep == _M_waiting.size())
                for (std::size_t __i = 0; __i < _M_waiting.size(); ++__i)
                    if (_M_waiting[__i]._M_domain == __exec::wait_domain::result) {
                        __rep = __i; break;
                    }
            if (__rep == _M_waiting.size()) __rep = 0;
            _M_waiting[__rep]._M_block(_M_waiting[__rep]._M_self, __budget);
            return;
        }

        for (std::size_t __d = 0; __d < __exec::wait_domain_count; ++__d) {
            const auto __dom = static_cast<__exec::wait_domain>(__d);
            __exec::waiter* __rep = nullptr;
            for (auto& __w : _M_waiting)
                if (__w._M_domain == __dom) { __rep = &__w; break; }
            if (!__rep) continue;
            const std::uint32_t __rem = _M_block_budget_ms();
            const std::uint32_t __slice =
                (__rem == 0 || __rem > _S_cross_slice_ms) ? _S_cross_slice_ms : __rem;
            __rep->_M_block(__rep->_M_self, __slice);
            if (_M_poll_sweep() && !_M_ready.empty())
                return;
        }
        yield();
    }

    std::vector<std::coroutine_handle<>> _M_ready;
    std::vector<__exec::waiter> _M_waiting;
    std::vector<std::coroutine_handle<>> _M_owned;

    static inline thread_local executor* _S_current = nullptr;
};


class touch_event {
public:
    touch_event() noexcept = default;

    bool await_ready() noexcept { return (_M_got = touch_pop(&_M_ev)); }

    bool await_suspend(std::coroutine_handle<> __h) {
        executor::current()->wait_on(__h, this, &_S_poll, &_S_block,
                                     __exec::wait_domain::touch);
        return true;
    }

    Touch await_resume() noexcept { return _M_ev; }

private:
    static bool _S_poll(void* __s) {
        auto* __a = static_cast<touch_event*>(__s);
        return __a->_M_got || (__a->_M_got = touch_pop(&__a->_M_ev));
    }
    static void _S_block(void* __s, uint32_t __ms) {
        auto* __a = static_cast<touch_event*>(__s);
        if (!__a->_M_got) __a->_M_got = touch_wait(&__a->_M_ev, __ms);
    }

    Touch _M_ev{};
    bool _M_got = false;
};

class brook_read {
public:
    brook_read(Brook* __b, void* __frame) noexcept
        : _M_b(__b), _M_frame(__frame) {}

    bool await_ready() noexcept {
        _M_rc = brook_try_pop(_M_b, _M_frame);
        return _M_rc != -ERR_WOULD_BLOCK;
    }

    bool await_suspend(std::coroutine_handle<> __h) {
        executor::current()->wait_on(__h, this, &_S_poll, &_S_block,
                                     __exec::wait_domain::brook);
        return true;
    }

    int await_resume() noexcept { return _M_rc; }

private:
    static bool _S_poll(void* __s) {
        auto* __a = static_cast<brook_read*>(__s);
        if (__a->_M_rc != -ERR_WOULD_BLOCK) return true;
        __a->_M_rc = brook_try_pop(__a->_M_b, __a->_M_frame);
        return __a->_M_rc != -ERR_WOULD_BLOCK;
    }
    static void _S_block(void* __s, uint32_t __ms) {
        auto* __a = static_cast<brook_read*>(__s);
        if (__a->_M_rc != -ERR_WOULD_BLOCK) return;
        __a->_M_rc = brook_pop_timeout(__a->_M_b, __a->_M_frame, __ms);
        if (__a->_M_rc == -ERR_TIMEOUT) __a->_M_rc = -ERR_WOULD_BLOCK;
    }

    Brook* _M_b;
    void* _M_frame;
    int _M_rc = -ERR_WOULD_BLOCK;
};

class pocket_recv {
public:
    pocket_recv() noexcept = default;

    bool await_ready() noexcept { return (_M_got = receive(&_M_r)); }

    bool await_suspend(std::coroutine_handle<> __h) {
        executor::current()->wait_on(__h, this, &_S_poll, &_S_block,
                                     __exec::wait_domain::result);
        return true;
    }

    Result await_resume() noexcept { return _M_r; }

private:
    static bool _S_poll(void* __s) {
        auto* __a = static_cast<pocket_recv*>(__s);
        return __a->_M_got || (__a->_M_got = receive(&__a->_M_r));
    }
    static void _S_block(void* __s, uint32_t __ms) {
        auto* __a = static_cast<pocket_recv*>(__s);
        if (!__a->_M_got) __a->_M_got = receive_wait(&__a->_M_r, __ms);
    }

    Result _M_r{};
    bool _M_got = false;
};

class timer_await {
public:
    explicit timer_await(std::chrono::steady_clock::time_point __dl) noexcept
        : _M_deadline(__dl) {}

    bool await_ready() noexcept {
        return std::chrono::steady_clock::now() >= _M_deadline;
    }

    bool await_suspend(std::coroutine_handle<> __h) {
        std::uint64_t __dtsc;
        auto __now = std::chrono::steady_clock::now();
        if (_M_deadline > __now) {
            auto __ms = std::chrono::ceil<std::chrono::milliseconds>(
                            _M_deadline - __now).count();
            if (__ms < 1) __ms = 1;
            __dtsc = cpu_rdtsc() + cpu_ms_to_tsc(static_cast<std::uint64_t>(__ms));
        } else {
            __dtsc = cpu_rdtsc();
        }
        executor::current()->wait_on(__h, this, &_S_poll, &_S_block,
                                     __exec::wait_domain::timer, __dtsc);
        return true;
    }

    void await_resume() const noexcept {}

private:
    static bool _S_poll(void* __s) {
        return std::chrono::steady_clock::now()
                   >= static_cast<timer_await*>(__s)->_M_deadline;
    }
    static void _S_block(void* , std::uint32_t __ms) {
        volatile std::uint64_t __w = 0;
        addr_park((const volatile void*)&__w, 0, __ms ? __ms : 1);
    }

    std::chrono::steady_clock::time_point _M_deadline;
};

template <class Rep, class Period>
inline timer_await after(std::chrono::duration<Rep, Period> __d) noexcept {
    auto __now = std::chrono::steady_clock::now();
    if (__d <= std::chrono::duration<Rep, Period>::zero())
        return timer_await{__now};
    return timer_await{__now + std::chrono::ceil<std::chrono::steady_clock::duration>(__d)};
}

template <class Clock, class Duration>
inline timer_await until(std::chrono::time_point<Clock, Duration> __tp) noexcept {
    auto __rem = __tp - Clock::now();
    return after(__rem);
}
inline timer_await until(std::chrono::steady_clock::time_point __tp) noexcept {
    return timer_await{__tp};
}

}

#endif