// boxcxx — <box/cxx/executor.h>
//
// BoxOS-native cooperative async for C++ coroutines — the flagship Ф12
// surface. This is NOT a Unix event loop (no epoll/fd/reactor mimicry): it
// is a single-thread-per-cabin cooperative run-loop that resumes coroutines
// when their BoxOS event sources (Touch / Brook / Pocket-Result) report
// ready, using each primitive's native poll + blocking-wait pair.
//
//   box::task<T>          — a lazy, awaitable coroutine returning T
//   box::executor         — the cooperative run-loop (one per cabin)
//   co_await box::touch_event()   — next Touch event   (-> Touch)
//   co_await box::brook_read(b,f)  — next Brook frame  (-> int rc)
//   co_await box::pocket_recv()    — next Pocket reply (-> Result)
//
// Concurrency model: a cabin has ONE execution context. When every live
// coroutine is blocked, the cabin genuinely has no other work, so the
// executor blocks on the sole waiter's native wait (forever) when there is
// exactly one, or cooperatively yields and re-polls when several waiters
// span different sources. True parallel threads are a separate kernel epic
// (Strands) — until then this cooperative executor is the concurrency story.
#ifndef BOXCXX_BOX_EXECUTOR_H
#define BOXCXX_BOX_EXECUTOR_H

#include <coroutine>
#include <vector>
#include <utility>
#include <exception>
#include <cstdint>
#include <type_traits>

#include <box/touch.h>
#include <box/brook.h>
#include <box/ipc.h>
#include <box/core/result.h>
#include <box/error.h>
#include <box/sync.h>   // yield()

namespace box {

// ── task<T> ─────────────────────────────────────────────────────────────
// A lazy coroutine: suspends at initial_suspend, runs only when resumed
// (by an awaiting coroutine via symmetric transfer, or by an executor).
// Awaiting a task starts it and, on completion, resumes the awaiter via the
// stored continuation (symmetric transfer — no unbounded stack growth on
// long co_await chains). Move-only; the frame is owned by this object.
template <typename T>
class task;

namespace __exec {

// Common promise state: the continuation to resume on completion plus any
// escaped exception. final_suspend transfers control to the continuation
// (noop_coroutine for a root task driven by an executor).
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

}  // namespace __exec

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

    // Awaitable: starting this task is a symmetric transfer into its frame.
    bool await_ready() const noexcept { return !_M_coro || _M_coro.done(); }
    handle_type await_suspend(std::coroutine_handle<> __awaiting) noexcept {
        _M_coro.promise()._M_continuation = __awaiting;
        return _M_coro;
    }
    T await_resume() { return _M_take(); }

    // Non-coroutine extraction (used by executor::block_on after the loop).
    T result() { return _M_take(); }

    bool done() const noexcept { return !_M_coro || _M_coro.done(); }
    std::coroutine_handle<> handle() const noexcept { return _M_coro; }
    handle_type release() noexcept { return std::exchange(_M_coro, {}); }

private:
    friend class executor;
    explicit task(handle_type __h) noexcept : _M_coro(__h) {}

    T _M_take() {
        // Null-safe (mirrors task<void>): rethrow a stored exception first.
        if (_M_coro && _M_coro.promise()._M_exc)
            std::rethrow_exception(_M_coro.promise()._M_exc);
        // Precondition: result()/await_resume() run only on a completed task
        // that returned a value (block_on/await_resume guarantee done()). A
        // null or value-less task here is a caller contract violation — fail
        // deterministically rather than move an unconstructed union member.
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

// ── executor ────────────────────────────────────────────────────────────
namespace __exec {

// A type-erased suspended-coroutine record. `poll` tries to complete the
// awaitable (consuming the event into the awaiter's storage) and returns
// true when the coroutine may resume; `block` performs the awaitable's
// native blocking wait for up to `timeout_ms` (0 == forever).
struct waiter {
    std::coroutine_handle<> _M_h;
    void* _M_self;
    bool (*_M_poll)(void*);
    void (*_M_block)(void*, uint32_t);
};

}  // namespace __exec

class executor {
public:
    executor() = default;
    executor(const executor&) = delete;
    executor& operator=(const executor&) = delete;

    ~executor() {
        // Safety net: reap detached frames spawned but never drained by run()
        // (spawn() without run(), or an abnormal run() exit). run()'s own
        // scope-guard normally clears _M_owned, so this is usually empty.
        for (auto __h : _M_owned)
            if (__h) __h.destroy();
    }

    static executor* current() noexcept { return _S_current; }

    // Queue a coroutine to be resumed by the run-loop.
    void schedule(std::coroutine_handle<> __h) { _M_ready.push_back(__h); }

    // Register a suspended coroutine waiting on a BoxOS event source.
    void wait_on(__exec::waiter __w) { _M_waiting.push_back(__w); }

    // Take ownership of a detached task and queue it (fire-and-forget).
    template <typename T>
    void spawn(task<T> __t) {
        auto __h = __t.release();
        _M_owned.push_back(__h);
        _M_ready.push_back(__h);
    }

    // Drive the loop until no coroutine is runnable or waiting.
    void run() {
        // Exception-safety: restore _S_current and reap detached frames on
        // EVERY exit path. A non-noexcept allocation inside an awaiter's
        // wait_on / _M_pump_waiters push_back (or a resumed/foreign frame) can
        // throw bad_alloc straight out of resume(); without this guard
        // _S_current would be left dangling (use-after-free for a stack-local
        // executor) and _M_owned frames would leak permanently.
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

    // Run a single root task to completion and return its result.
    template <typename T>
    T block_on(task<T> __t) {
        schedule(__t.handle());
        run();
        return __t.result();
    }

private:
    void _M_pump_waiters() {
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
        if (!__any && _M_ready.empty() && !_M_waiting.empty()) {
            // Everything is blocked. With a single waiter we can block on its
            // native source forever; with several spanning different sources
            // we cooperatively yield and re-poll (no Unix wait-any primitive).
            if (_M_waiting.size() == 1)
                _M_waiting[0]._M_block(_M_waiting[0]._M_self, 0);
            else
                yield();
        }
    }

    std::vector<std::coroutine_handle<>> _M_ready;
    std::vector<__exec::waiter> _M_waiting;
    std::vector<std::coroutine_handle<>> _M_owned;

    static inline thread_local executor* _S_current = nullptr;
};

// ── awaitables over the BoxOS event sources ─────────────────────────────
// Each awaiter pairs a non-blocking poll (consumes the event into storage)
// with a native blocking wait. await_ready fast-paths an already-ready
// event; otherwise await_suspend registers the (handle, poll, block) triple
// with the current executor and suspends back into the run-loop.

// co_await box::touch_event() -> Touch  (the next Touch published to this
// cabin, of any tag). Tag-filtered subscription is the Ф13 box::touch layer.
class touch_event {
public:
    touch_event() noexcept = default;

    bool await_ready() noexcept { return (_M_got = touch_pop(&_M_ev)); }

    bool await_suspend(std::coroutine_handle<> __h) {
        executor::current()->wait_on({__h, this, &_S_poll, &_S_block});
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

// co_await box::brook_read(b, frame) -> int  (OK, or -ERR_STREAM_CLOSED on
// clean writer-leave EOF, or other negative error). `frame` must point to a
// brook_frame_size(b)-byte buffer owned by the caller.
class brook_read {
public:
    brook_read(Brook* __b, void* __frame) noexcept
        : _M_b(__b), _M_frame(__frame) {}

    bool await_ready() noexcept {
        _M_rc = brook_try_pop(_M_b, _M_frame);
        return _M_rc != -ERR_WOULD_BLOCK;
    }

    bool await_suspend(std::coroutine_handle<> __h) {
        executor::current()->wait_on({__h, this, &_S_poll, &_S_block});
        return true;
    }

    int await_resume() noexcept { return _M_rc; }

private:
    static bool _S_poll(void* __s) {
        auto* __a = static_cast<brook_read*>(__s);
        __a->_M_rc = brook_try_pop(__a->_M_b, __a->_M_frame);
        return __a->_M_rc != -ERR_WOULD_BLOCK;
    }
    static void _S_block(void* __s, uint32_t __ms) {
        auto* __a = static_cast<brook_read*>(__s);
        __a->_M_rc = brook_pop_timeout(__a->_M_b, __a->_M_frame, __ms);
        // A timeout just means "re-poll"; map it back to would-block.
        if (__a->_M_rc == -ERR_TIMEOUT) __a->_M_rc = -ERR_WOULD_BLOCK;
    }

    Brook* _M_b;
    void* _M_frame;
    int _M_rc = -ERR_WOULD_BLOCK;
};

// co_await box::pocket_recv() -> Result  (the next Pocket/IPC reply).
class pocket_recv {
public:
    pocket_recv() noexcept = default;

    bool await_ready() noexcept { return (_M_got = receive(&_M_r)); }

    bool await_suspend(std::coroutine_handle<> __h) {
        executor::current()->wait_on({__h, this, &_S_poll, &_S_block});
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

}  // namespace box

#endif  // BOXCXX_BOX_EXECUTOR_H
