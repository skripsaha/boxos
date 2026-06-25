// boxcxx — <box/cxx/executor.h>
//
// BoxOS-native cooperative async for C++ coroutines — the flagship Ф12
// surface, carrying the Ф24a group-by-domain wait-any core. This is NOT a
// Unix event loop (no epoll/select/fd/reactor): it is a per-strand cooperative
// run-loop that resumes a coroutine when its BoxOS event source reports ready,
// using that primitive's native non-blocking poll + blocking-wait pair.
//
//   box::task<T>          — a lazy, awaitable coroutine returning T
//   box::executor         — the cooperative run-loop (one per strand)
//   co_await box::touch_event()   — next Touch event  (-> Touch domain)
//   co_await box::brook_read(b,f) — next Brook frame  (-> Brook domain)
//   co_await box::pocket_recv()   — next Pocket reply (-> Result domain)
//   co_await box::result_any()    — next ANY ResultRing record: an IPC message
//                                   OR a kernel result, for IPC-server/daemon
//                                   loops (box/cxx/message.h)
//
// Concurrency model: a strand drives one executor. Parallel execution is real
// (Ф21 box::strand / std::thread spawn sibling strands that share the cabin);
// events are produced by those sibling strands and by other cores. When EVERY
// live coroutine here is blocked, this strand has no other work, so the
// executor performs a real kernel WAIT rather than spinning:
//   - Result-domain waiters (one, or several sharing the ONE ResultRing tail)
//     -> a single native block, forever or until the earliest deadline. Safe
//     because the ResultRing is IPI-woken (a cross-core/cross-cabin producer
//     breaks the wait even on one App-Core; a same-cabin result producer cannot
//     exist) — event-driven, zero spin.
//   - Touch / Brook waiters (each monitors a PER-OBJECT cacheline via UMWAIT,
//     and their producer may be a same-core sibling strand) -> a bounded round-
//     robin of short native blocks per present domain, re-polling between slices
//     and yielding once per fruitless rotation (the box::strand_watch::wait
//     idiom). The yield is what lets a same-core producer run; the slices keep
//     the core idle, not spinning. A bounded multiplex, never a busy-yield,
//     never a lost wake (a no-IPI store during another slice is caught by the
//     next poll).
// An absolute per-waiter deadline (the Ф24b timer awaiter sets it) caps the
// block budget, so a timed await wakes ~on time instead of forever.
#ifndef BOXCXX_BOX_EXECUTOR_H
#define BOXCXX_BOX_EXECUTOR_H

#include <coroutine>
#include <vector>
#include <utility>
#include <exception>
#include <cstdint>
#include <type_traits>
#include <chrono>      // box::after / box::until timer awaiter (steady_clock deadline)

#include <box/touch.h>
#include <box/brook.h>
#include <box/ipc.h>
#include <box/core/result.h>
#include <box/error.h>
#include <box/sync.h>   // yield()
#include <box/cpu.h>    // cpu_rdtsc / cpu_ms_to_tsc / cpu_tsc_to_ms (deadline math)

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

// Which kernel wait-line a suspended awaiter blocks on. BoxOS gives a strand
// three disjoint UMWAIT cachelines it can monitor — the TouchRing tail, the
// ResultRing tail, and a Brook peer cursor — and UMONITOR watches exactly ONE
// line. A run-loop whose waiters span different lines cannot arm a single
// hardware wait; it groups by domain and rotates bounded native blocks. This
// tag is how _M_pump_waiters groups. Only `result` is a single per-strand line
// (one ResultRing) that several waiters can share; `touch`/`brook` are
// per-object lines, so >1 of those must rotate. Names mirror the substrate.
//
// CONTRACT — tagging `result` is a hard promise that the awaiter's source is
// ResultRing-delivered (KResultPush + IPI). A lone `result` waiter forever-
// blocks (Phase 3a) trusting that IPI to break the wait; a non-ResultRing
// source mis-tagged `result` would hang forever on a single App-Core. Touch and
// Brook never forever-block (their producer may be a same-core sibling strand).
//
// `timer` and `join` (Ф24b) both block on a kernel addr_park, which RESCHEDULES
// (PROC_WAITING, async) and is IPI-woken — so, like `result`, a lone timer/join
// waiter forever-blocks safely on a single App-Core (its park does not idle the
// core in place the way a Touch/Brook UMWAIT does). They differ in collapsibility:
// `timer` co-collapses WITH `result` (one budget-capped block serves every timer,
// since a budget sleep advances the one shared wall clock); `join` does NOT —
// addr_wake delivers a Result only to an addr_park entry ARMED for the exact join
// flag's phys address, so each join MUST arm its own block and can never be a
// silent collapse passenger.
enum class wait_domain : std::uint8_t {
    touch  = 0,   // TouchRing  — touch_pop / touch_wait              (no IPI on push)
    result = 1,   // ResultRing — receive / receive_wait / result_wait_any (KResultPush IPIs)
    brook  = 2,   // Brook      — brook_try_pop / brook_pop_timeout   (no IPI on push)
    timer  = 3,   // pure-deadline addr_park   — reschedules + co-collapses WITH result
    join   = 4,   // strand completion addr_park — reschedules, NOT collapsible (addr-conditional wake)
};
inline constexpr std::size_t wait_domain_count = 5;

// ── wait-domain properties (Ф24b) ───────────────────────────────────────────
// Lone-forever-safe on a single App-Core: the source reschedules (PROC_WAITING)
// or is IPI-woken, so a lone waiter can block forever without starving a same-
// core sibling. result (IPI), timer & join (addr_park reschedule) = yes; touch/
// brook idle the core in-place (UMWAIT) and their producer may be a same-core
// sibling = no.
constexpr bool _S_reschedules(wait_domain __d) noexcept {
    return __d == wait_domain::result || __d == wait_domain::timer
        || __d == wait_domain::join;
}
// One armed block's observation satisfies EVERY waiter of this kind: result =
// one ResultRing tail; timer = the shared wall clock (one budget sleep advances
// all timers). join = NO — its wake is address-conditional (each join must arm
// its own addr_park entry), so a join can never be a silent collapse passenger.
constexpr bool _S_collapsible(wait_domain __d) noexcept {
    return __d == wait_domain::result || __d == wait_domain::timer;
}

// A type-erased suspended-coroutine record. `poll` tries to complete the
// awaitable (consuming the event into the awaiter's storage) and returns true
// when the coroutine may resume; `block` performs the awaitable's native
// blocking wait for up to `timeout_ms` (0 == forever). `domain` selects the
// kernel wait-line for the group-by-domain multiplex; `deadline_tsc` is an
// ABSOLUTE cpu_rdtsc deadline (0 == none) that caps the block budget — the
// plumbing the Ф24b timer awaiter sets (every Ф24a awaiter leaves it 0, i.e.
// waits forever, exactly as before).
//
// `accept_any` marks a `result` waiter whose native block consumes ANY
// ResultRing record (result_wait_any), versus an IPC-only block (receive_wait
// → result_wait_ipc) that drains a kernel record into a stash and re-blocks. It
// is load-bearing for the Phase 3b collapse: several `result` waiters share one
// wait-line but NOT one filter, so the single armed block MUST be an accept-any
// one when present — else an IPC-only representative would strand a kernel
// record a sibling result_any awaiter is owed, hanging it forever.
struct waiter {
    std::coroutine_handle<> _M_h;
    void* _M_self;
    bool (*_M_poll)(void*);
    void (*_M_block)(void*, std::uint32_t);
    wait_domain   _M_domain;
    std::uint64_t _M_deadline_tsc;
    bool          _M_accept_any;
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

    // Register a suspended coroutine waiting on a BoxOS event source. The
    // fully-specified form takes a built waiter; the convenience overload tags
    // the classic {h, self, poll, block} record with its wait-domain and an
    // optional absolute deadline (the Ф24b timer awaiter passes one).
    void wait_on(__exec::waiter __w) { _M_waiting.push_back(__w); }
    void wait_on(std::coroutine_handle<> __h, void* __self,
                 bool (*__poll)(void*), void (*__block)(void*, std::uint32_t),
                 __exec::wait_domain __d, std::uint64_t __deadline_tsc = 0,
                 bool __accept_any = false) {
        _M_waiting.push_back(
            {__h, __self, __poll, __block, __d, __deadline_tsc, __accept_any});
    }

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
    // Cross-domain round-robin slice (ms): the box::strand_watch::wait grain.
    // A no-IPI Touch/Brook store that lands while we block in a DIFFERENT
    // domain's slice costs at most one extra slice of latency before the
    // post-slice sweep observes it — bounded, never a lost wake.
    static constexpr std::uint32_t _S_cross_slice_ms = 16;

    // Poll every waiter once; move the ready ones onto the run queue. Returns
    // true iff at least one became ready. Swap-erase walk (the original idiom);
    // reused by the initial sweep and after each cross-domain slice.
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

    // The block timeout (ms) for this pump turn: time until the EARLIEST
    // waiter deadline, or 0 (== block forever) when none carries one. This is
    // what revives a bounded wait for >1 waiter (dead in the old pure-yield
    // path). A deadline already in the past returns 1 (one short final block,
    // after which the owning awaiter's poll observes it ready).
    std::uint32_t _M_block_budget_ms() const {
        std::uint64_t __min_deadline = 0;  // 0 == none seen
        for (const auto& __w : _M_waiting) {
            if (__w._M_deadline_tsc &&
                (__min_deadline == 0 || __w._M_deadline_tsc < __min_deadline))
                __min_deadline = __w._M_deadline_tsc;
        }
        if (__min_deadline == 0) return 0;  // forever
        std::uint64_t __now = cpu_rdtsc();
        if (__min_deadline <= __now) return 1;  // already due — one short block
        std::uint64_t __ms = cpu_tsc_to_ms(__min_deadline - __now);
        if (__ms == 0) __ms = 1;  // sub-ms but positive — never 0 (== forever)
        if (__ms > 0xFFFFFFFFull) __ms = 0xFFFFFFFFull;
        return static_cast<std::uint32_t>(__ms);
    }

    void _M_pump_waiters() {
        // Phase 1 — poll sweep. A newly-ready waiter, or any already-queued
        // coroutine, means the run-loop has work: do not block.
        if (_M_poll_sweep() || !_M_ready.empty() || _M_waiting.empty())
            return;

        // Phase 2 — everything is blocked. The block budget caps every native
        // wait this turn at the earliest waiter deadline (revives timeout_ms).
        const std::uint32_t __budget = _M_block_budget_ms();

        // Phase 3a — a lone waiter whose domain RESCHEDULES (result IPI-woken;
        // timer/join addr_park-reschedule) forever-blocks safely; touch/brook
        // fall through to 3c. The ResultRing is IPI-woken, so even on a SINGLE
        // App-Core a cross-core / cross-cabin producer breaks the wait, and a
        // same-cabin result producer cannot exist (the kernel rejects send-to-
        // self); a timer/join addr_park is rescheduled (PROC_WAITING) and woken
        // by its deadline / a sibling's addr_wake, so it too cannot starve a
        // same-core sibling. Touch and Brook producers CAN be a same-core sibling
        // strand, and a forever UMWAIT on one App-Core would starve it (the wait
        // idles the core without rescheduling) — so a lone touch/brook waiter
        // does NOT forever-block here; it falls through to the bounded+yield
        // rotation (Phase 3c), which gives the sibling a scheduling point. (A
        // deadline-bearing lone waiter passes its budget either way, so it still
        // wakes ~on time.)
        if (_M_waiting.size() == 1 &&
            __exec::_S_reschedules(_M_waiting[0]._M_domain)) {
            _M_waiting[0]._M_block(_M_waiting[0]._M_self, __budget);
            return;
        }

        // Phase 3b — collapsible single observation. result AND timer co-collapse: one
        // result_wait_any(budget) block wakes on a ResultRing KResultPush (IPI) OR after
        // budget == the earliest timer deadline; the post-block sweep then routes the
        // record to its result awaiter and observes every due timer. join is NOT
        // collapsible and never reaches here. Touch/Brook are PER-OBJECT lines (two
        // Brooks = two cursors), so >1 of those must rotate (Phase 3c) — a collapse
        // there would arm one line and miss the other's no-IPI store.
        bool __collapsible = true;
        for (const auto& __w : _M_waiting)
            if (!__exec::_S_collapsible(__w._M_domain)) { __collapsible = false; break; }
        if (__collapsible) {
            // REP must serve EVERY collapsed member's wake:
            //   1. accept_any RESULT (result_wait_any): returns on ANY record AND honours
            //      budget — also the Ф24a accept_any contract (don't strand a kernel
            //      record owed to a result_any sibling).
            //   2. any RESULT (receive_wait/pocket_recv): IPI-woken on its record AND
            //      honours budget, so it serves the timers too.
            //   3. ALL timers (no result waiter): any timer — a budget sleep advancing
            //      the shared clock.
            // ORDER IS LOAD-BEARING: when a result waiter is present it MUST be the rep,
            // NOT a timer. A timer rep sleeps the full budget and is NOT woken by an
            // incoming IPI, so an IPC at t=2ms with a 500ms timer would wait the whole
            // budget. A result rep is IPI-broken at t=2ms.
            std::size_t __rep = _M_waiting.size();
            for (std::size_t __i = 0; __i < _M_waiting.size(); ++__i)
                if (_M_waiting[__i]._M_domain == __exec::wait_domain::result &&
                    _M_waiting[__i]._M_accept_any) { __rep = __i; break; }
            if (__rep == _M_waiting.size())
                for (std::size_t __i = 0; __i < _M_waiting.size(); ++__i)
                    if (_M_waiting[__i]._M_domain == __exec::wait_domain::result) {
                        __rep = __i; break;
                    }
            // __rep=0 is safe ONLY because _S_collapsible excludes join — a join would
            // make __collapsible false, so slot 0 here is guaranteed result-or-timer.
            if (__rep == _M_waiting.size()) __rep = 0;  // all timers — slot 0 is a timer
            _M_waiting[__rep]._M_block(_M_waiting[__rep]._M_self, __budget);
            return;
        }

        // Phase 3c — waiters span >1 wait-line (and/or include a join, which is
        // never collapsible). No single UMWAIT covers them; rotate a BOUNDED
        // native block per present domain, re-polling between slices. Never a
        // pure busy-yield; never a lost wake (a no-IPI store during another
        // domain's slice is caught by the post-slice sweep). Each slice is
        // RECOMPUTED right before its block as min(grain, time-to-earliest-
        // deadline): wall-clock consumed by earlier domains shrinks the later
        // slices, so the rotation never overshoots — the block straddling the
        // deadline is sized to the EXACT remainder, and a timer co-resident with
        // per-object waiters fires ~on its deadline rather than up to one grain
        // late. (_M_block_budget_ms re-reads cpu_rdtsc each call: 0 == no deadline
        // → full grain; a past-due deadline → 1, one short block then the poll.)
        for (std::size_t __d = 0; __d < __exec::wait_domain_count; ++__d) {
            const auto __dom = static_cast<__exec::wait_domain>(__d);
            // Representative of this domain (re-found each pass: a prior slice's
            // sweep may have swap-erased entries, so cached pointers are stale).
            __exec::waiter* __rep = nullptr;
            for (auto& __w : _M_waiting)
                if (__w._M_domain == __dom) { __rep = &__w; break; }
            if (!__rep) continue;
            // Live remaining-to-deadline, recomputed per domain so earlier legs'
            // elapsed time tightens this slice (the deadline-precision fix).
            const std::uint32_t __rem = _M_block_budget_ms();
            const std::uint32_t __slice =
                (__rem == 0 || __rem > _S_cross_slice_ms) ? _S_cross_slice_ms : __rem;
            __rep->_M_block(__rep->_M_self, __slice);
            if (_M_poll_sweep() && !_M_ready.empty())
                return;  // delivered — let the run-loop resume the coroutine
        }
        // Nothing fired this rotation. A touch/brook slice idles the core in
        // place (UMWAIT) WITHOUT rescheduling, so on a SINGLE App-Core a sibling
        // strand that must PRODUCE the awaited event shares this core and can
        // never run while we hold it. (A timer/join slice DOES reschedule — its
        // addr_park frees the core — but a mixed rotation still holds the core
        // across its touch/brook legs.) Yield once per fruitless rotation to give
        // the scheduler a chance to run the producer — on multi-core a cheap
        // hand-off (the slices already supplied the idle), on one core what
        // guarantees forward progress. Bounded, never a busy-spin.
        yield();
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
//
// ‼ LATCH INVARIANT (load-bearing for every CONSUMING awaiter) ‼
// Phase 3c blocks a waiter on its native wait, then re-polls EVERY waiter
// (_M_pump_waiters → _M_poll_sweep). When `block` ITSELF delivers the event
// (e.g. brook_pop_timeout pops a frame, touch_wait dequeues a Touch — the
// source is one-shot/consumed), the immediately-following re-poll must NOT
// re-read the now-empty source: that would discard the just-delivered event
// (ring head already advanced) and strand the coroutine forever. So a
// consuming awaiter's `poll` MUST LATCH — report "ready" from the value
// `block` already delivered without re-consuming. touch_event/pocket_recv
// latch via `_M_got`; box::brook<T>/box::current<T> latch on
// `_M_rc != -ERR_WOULD_BLOCK`. A NON-consuming awaiter (timer/join: the
// clock / a flag is re-readable) needs no latch. Phase50 in cxxtest proves
// this contract deterministically.

// co_await box::touch_event() -> Touch  (the next Touch published to this
// cabin, of any tag). Tag-filtered subscription is the Ф13 box::touch layer.
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

// co_await box::brook_read(b, frame) -> int  (OK, or -ERR_STREAM_CLOSED once
// the writer has left and the ring is drained, or other negative error —
// this is BoxOS's race-free writer-leave terminal, not a Unix EOF). `frame`
// must point to a
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
        executor::current()->wait_on(__h, this, &_S_poll, &_S_block,
                                     __exec::wait_domain::brook);
        return true;
    }

    int await_resume() noexcept { return _M_rc; }

private:
    static bool _S_poll(void* __s) {
        auto* __a = static_cast<brook_read*>(__s);
        // Latch: once _S_block (or a prior poll) has delivered a frame/terminal,
        // do NOT re-pop — the executor re-polls every waiter right after its
        // native block (Phase 3c); a re-pop would advance past the just-delivered
        // frame (head already moved in _S_block) and clobber _M_rc to WOULD_BLOCK,
        // losing it. Mirrors touch_event/pocket_recv.
        if (__a->_M_rc != -ERR_WOULD_BLOCK) return true;
        __a->_M_rc = brook_try_pop(__a->_M_b, __a->_M_frame);
        return __a->_M_rc != -ERR_WOULD_BLOCK;
    }
    static void _S_block(void* __s, uint32_t __ms) {
        auto* __a = static_cast<brook_read*>(__s);
        if (__a->_M_rc != -ERR_WOULD_BLOCK) return;  // already delivered — don't re-block
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

// ── co_await box::after(dur) / box::until(tp) -> void  (Ф24b timer) ──────────
// A pure-deadline awaiter: suspend until the steady clock reaches an absolute
// deadline. No event source — its native block is a private-word addr_park for
// the executor-supplied budget (reschedules, returns on timeout). Tags
// wait_domain::timer: lone-forever-safe AND co-collapsible with the result line.
// The steady_clock time_point is the truth poll/await_ready test; the absolute
// rdtsc deadline is derived at SUSPEND from now()+remaining (not at construction
// — the awaiter may be co_awaited later), keeping it on the same clock the
// executor's _M_block_budget_ms uses.
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
            __dtsc = cpu_rdtsc();   // already due — budget yields 1 (one short block)
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
    static void _S_block(void* /*__s*/, std::uint32_t __ms) {
        // A private word nobody wakes → the park returns only on the timeout.
        // Clamp a stray 0 budget to 1: 0 == "forever" to the kernel, wrong here.
        volatile std::uint64_t __w = 0;
        addr_park((const volatile void*)&__w, 0, __ms ? __ms : 1);
    }

    std::chrono::steady_clock::time_point _M_deadline;
};

// Suspend until `d` from now has elapsed. Non-positive → immediately ready (no
// park); a sub-ms positive span rounds up so it waits at least the clock tick.
template <class Rep, class Period>
inline timer_await after(std::chrono::duration<Rep, Period> __d) noexcept {
    auto __now = std::chrono::steady_clock::now();
    if (__d <= std::chrono::duration<Rep, Period>::zero())
        return timer_await{__now};               // non-positive → immediately ready
    return timer_await{__now + std::chrono::ceil<std::chrono::steady_clock::duration>(__d)};
}

// Suspend until an absolute deadline. The steady_clock overload is exact; an
// arbitrary-clock deadline is projected onto steady_clock by measuring the
// remaining span on that clock and adding it to steady_clock::now().
template <class Clock, class Duration>
inline timer_await until(std::chrono::time_point<Clock, Duration> __tp) noexcept {
    auto __rem = __tp - Clock::now();
    return after(__rem);
}
inline timer_await until(std::chrono::steady_clock::time_point __tp) noexcept {
    return timer_await{__tp};
}

}  // namespace box

#endif  // BOXCXX_BOX_EXECUTOR_H
