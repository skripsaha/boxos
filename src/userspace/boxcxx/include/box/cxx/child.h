// boxcxx — box::child  (a RAII supervisor for a spawned child process)
//
// box::process::spawn launches a child cabin and hands back a bare pid handle;
// box::child wraps that pid as an OWNING, move-only supervisor that can wait for
// the child to finish, learn HOW it finished (clean / killed / crashed), kill it,
// and ask whether it is still alive — all event-driven over the kernel's
// process:died Touch, never a Unix waitpid/SIGCHLD poll.
//
//   box::result<box::child> c = box::child::spawn("worker");
//   if (c) {
//       box::result<int> r = c->wait();          // block until it finishes
//       if (r)                       done(*r);    // clean exit code  (>= 0)
//       else if (r.error().code() == box::errc::process_killed)  ...   // -1
//       else if (r.error().code() == box::errc::process_crashed) ...   // -2
//   }
//   co_await c->exited();                         // the same, on a box::executor
//
// Exit disposition rides the ERROR arm (the BoxOS error model, not a magic int):
// a clean exit is the success value `*r == code`; PROC_EXIT_KILLED maps to
// errc::process_killed and PROC_EXIT_CRASHED to errc::process_crashed; a
// bounded wait(ms) that expires while the child is still alive returns
// errc::timeout. ~child DETACHES — it never kills or waits; a child you drop
// keeps running on its own (the mooring line is cast off, the vessel sails on).
//
// ── the death station (why per-child subscriptions would be wrong) ───────────
// process:died is a MULTICAST: one death rings the bell for the whole strand and
// the subject is in the payload, not in the tag. The per-strand Touch stash also
// pulls by TAG, so two box::child objects each holding their OWN process:died
// claim would (a) fight for the single per-strand claim and (b) have the first
// destroyed one silence the others. Instead a strand-local STATION owns the ONE
// process:died subscription and DEMULTIPLEXES by the child's CANONICAL IDENTITY —
// the (pid, generation) pair the kernel now stamps into every death. It keeps the
// live supervised incarnations and the deaths it has seen-but-not-yet-collected,
// routes each incoming death to the matching (pid, generation) slot (DROPPING
// deaths it does not supervise), and hands each box::child only its own. The
// station is the harbour-master's death watch: every death is announced once, and
// the master tells each captain only when THEIR ship — that exact hull, not a
// later ship that reused the berth number — has gone down.
//
// ── pid-reuse robustness (why the generation matters) ────────────────────────
// The kernel recycles a pid the instant its process is reaped, and publishes
// process:died BEFORE the pid is freed. A supervisor keyed on the bare pid could
// therefore hand a fresh child the stale death of the previous tenant of its pid
// (misroute), or hang forever if that stale death was consumed by the wrong
// handle. box::child is immune because it matches on (pid, generation): a recycled
// pid always carries a NEW generation, so the predecessor's death — same pid, OLD
// generation — finds no slot and is dropped. The (pid, generation) pair is the
// same canonical identity the kernel uses for proc-op authorization.
//
// ── caveats (mirror box::system_watch) ───────────────────────────────────────
// The station is thread_local: a box::child must be wait()/exited()/kill()'d on
// the SAME strand that spawned it (that strand holds the process:died claim). Do
// NOT run a box::system_watch on a strand that supervises children — it would
// fight that strand for the single process:died claim. The station's claim is
// released when the strand ends (its thread_local destructor runs the
// box::subscription destructor — main at __cxa_finalize, a spawned strand at
// __boxcxx_thread_storage_exit). box::child is SPAWN-ONLY: every supervised
// incarnation is registered fresh from a spawn (which carries its generation) and
// deregistered when the child is reaped or detached.
#ifndef BOXCXX_BOX_CHILD_H
#define BOXCXX_BOX_CHILD_H

#include <coroutine>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "box/cxx/error.h"     // box::result / box::error / box::errc / box::status
#include "box/cxx/process.h"   // box::process::spawn (reused by child::spawn)
#include "box/cxx/touch.h"     // box::tag / box::tags::process_died / box::subscription / box::touch
#include "box/cxx/executor.h"  // box::executor / __exec::wait_domain / wait_on
#include "box/system.h"        // proc_kill (C ABI)
#include "box/touch.h"         // ::TouchProcessDied payload shape
#include "box/error.h"         // ::error_t / ERR_PROCESS_* / box_errno_of
#include "box/cpu.h"           // cpu_rdtsc / cpu_ms_to_tsc / cpu_tsc_to_ms (bounded-wait budget)
#include "proc_exit.h"         // PROC_EXIT_KILLED / PROC_EXIT_CRASHED

namespace box {

namespace _detail {

// child_slot — one supervised incarnation. Its identity is the (pid, generation)
// pair: process:died now carries the generation, so a recycled pid (same pid, new
// generation) is a DIFFERENT slot and the dead predecessor's stale death is
// dropped. code/has_death latch the outcome once the death routes here.
struct child_slot {
    std::uint32_t pid;
    std::uint32_t gen;
    int           code;
    bool          has_death;
};

// child_station — the strand-local process:died demultiplexer. Single-writer on
// its owning strand (like the per-strand Touch stash it sits on), so it needs no
// lock. slots_ holds every live supervised incarnation, keyed by (pid, gen); a
// death is latched IN-PLACE into its pre-allocated slot, so register_child (on
// spawn) is the ONLY allocation and routing a death never allocates. The table is
// small (children-per-strand), so a flat vector with a linear scan over the
// StrandPool fast-malloc heap is the right shape.
struct child_station {
    box::subscription       sub_;     // the ONE process:died claim
    std::vector<child_slot> slots_;   // live supervised incarnations

    child_station() noexcept : sub_(box::tags::process_died()) {}

    bool ok() const noexcept { return static_cast<bool>(sub_); }

    // The ONLY allocation point: reserve a slot for a freshly spawned child,
    // keyed by its canonical (pid, generation). May throw bad_alloc (push_back) —
    // child::spawn handles that by killing the child it cannot supervise.
    void register_child(std::uint32_t pid, std::uint32_t gen)
    {
        slots_.push_back(child_slot{pid, gen, 0, false});
    }

    // Decode one process:died and latch it into the matching (pid, generation)
    // slot IN-PLACE. A death with no matching slot — a foreign process, or the
    // STALE death of a now-recycled pid (same pid, OLD generation) — is DROPPED.
    // The generation is precisely what stops a stale death from latching onto a
    // live incarnation that reused the pid.
    void route(const box::touch& ev) noexcept
    {
        std::optional<::TouchProcessDied> d = ev.payload_as<::TouchProcessDied>();
        if (!d) return;
        std::uint32_t pid  = d->pid;         // copy out of the packed struct — a
        std::uint32_t gen  = d->generation;  // reference to a packed field is
        int           code = d->exit_code;   // ill-formed
        for (auto &s : slots_)
            if (s.pid == pid && s.gen == gen) { s.code = code; s.has_death = true; return; }
        // unmatched (pid, generation) → DROP
    }

    // Non-blocking: drain every pending process:died into the slots, then collect
    // (pid, gen) if its death has latched (consuming + retiring the slot).
    bool poll_collect(std::uint32_t pid, std::uint32_t gen, int *out)
    {
        while (std::optional<box::touch> ev = sub_.poll()) route(*ev);
        return consume(pid, gen, out);
    }

    // Blocking: collect (pid, gen)'s death within `ms` (0 == forever). Deaths of
    // OTHER supervised incarnations that arrive while we wait are latched into
    // their slots (for their own box::child later) and we keep waiting.
    bool wait_collect(std::uint32_t pid, std::uint32_t gen, std::uint32_t ms, int *out)
    {
        if (poll_collect(pid, gen, out)) return true;
        if (!sub_) return false;                               // no claim → cannot observe a death

        if (ms == 0) {
            for (;;) {                                         // one kernel-park per next death
                if (std::optional<box::touch> ev = sub_.wait(0)) {
                    route(*ev);
                    if (consume(pid, gen, out)) return true;
                }
            }
        }

        std::uint64_t deadline = cpu_rdtsc() + cpu_ms_to_tsc(ms);
        for (;;) {
            std::uint64_t now = cpu_rdtsc();
            if (now >= deadline) return false;                 // budget spent, (pid,gen) still alive
            std::uint64_t rem = cpu_tsc_to_ms(deadline - now);
            std::uint32_t slice = rem == 0           ? 1u
                                : rem > 0xFFFFFFFFull ? 0xFFFFFFFFu
                                                      : static_cast<std::uint32_t>(rem);
            std::optional<box::touch> ev = sub_.wait(slice);
            if (!ev) return false;                             // slice == remaining budget expired
            route(*ev);
            if (consume(pid, gen, out)) return true;
        }
    }

    // Take (pid, gen)'s latched death (if any): hand back its code and retire the
    // slot — once collected the incarnation is no longer a live supervised child.
    bool consume(std::uint32_t pid, std::uint32_t gen, int *out) noexcept
    {
        for (std::size_t i = 0; i < slots_.size(); ++i) {
            if (slots_[i].pid == pid && slots_[i].gen == gen && slots_[i].has_death) {
                *out = slots_[i].code;
                slots_[i] = slots_.back();
                slots_.pop_back();
                return true;
            }
        }
        return false;
    }

    // Detach: forget an incarnation — its slot leaves the station, so a later
    // death of (pid, gen) routes to no slot and is dropped (nobody's outcome).
    void deregister(std::uint32_t pid, std::uint32_t gen) noexcept
    {
        for (std::size_t i = 0; i < slots_.size(); ++i) {
            if (slots_[i].pid == pid && slots_[i].gen == gen) {
                slots_[i] = slots_.back();
                slots_.pop_back();
                return;
            }
        }
    }
};

// The calling strand's station: lazily constructed (claiming process:died) on
// first use — i.e. on the first child::spawn, BEFORE the child exists — and
// destroyed (releasing the claim) when the strand ends.
inline child_station &station()
{
    thread_local child_station s;
    return s;
}

}  // namespace _detail

// ── box::child — an owning, move-only supervisor for one spawned process ─────
class child {
    std::uint32_t pid_    = 0;      // 0 == empty / moved-from
    std::uint32_t gen_    = 0;      // pid-allocator generation — the other half of identity
    int           code_   = 0;      // cached disposition once latched
    bool          exited_ = false;  // have we seen + cached this child's death?

public:
    // Claim process:died on this strand (before the spawn, so the first death is
    // never missed), launch `name` as a new cabin, and register its canonical
    // (pid, generation) with the station. Propagates box::process::spawn's cause
    // on failure; errc::process_blocked if this strand could not claim
    // process:died, or if the kernel reported no generation (without it a death
    // cannot be matched to this exact incarnation, so we refuse rather than
    // supervise blind).
    static result<child> spawn(const char *name) noexcept
    {
        _detail::child_station &st = _detail::station();
        if (!st.ok()) return std::unexpected(error{errc::process_blocked});

        std::uint32_t gen = 0;
        result<process> p = process::spawn(name, &gen);
        if (!p) return std::unexpected(p.error());

        std::uint32_t pid = p->pid();
        if (gen == 0) {  // no generation → cannot match this incarnation's death
            ::proc_kill(pid);
            return std::unexpected(error{errc::process_blocked});
        }
        try {
            st.register_child(pid, gen);
        } catch (...) {
            ::proc_kill(pid);  // cannot supervise an unregistered child — don't leak it
            return std::unexpected(error{errc::no_memory});
        }
        return child(pid, gen);
    }

    child() noexcept = default;
    child(const child &)            = delete;
    child &operator=(const child &) = delete;

    child(child &&o) noexcept
        : pid_(o.pid_), gen_(o.gen_), code_(o.code_), exited_(o.exited_)
    {
        // Station is (pid, gen)-keyed — the slot stays put; we only move the handle.
        o.pid_ = 0; o.gen_ = 0; o.code_ = 0; o.exited_ = false;
    }
    child &operator=(child &&o) noexcept
    {
        if (this != &o) {
            _M_detach();
            pid_ = o.pid_; gen_ = o.gen_; code_ = o.code_; exited_ = o.exited_;
            o.pid_ = 0; o.gen_ = 0; o.code_ = 0; o.exited_ = false;
        }
        return *this;
    }
    ~child() { _M_detach(); }

    explicit operator bool() const noexcept { return pid_ != 0; }
    std::uint32_t pid() const noexcept { return pid_; }

    // Event-driven liveness: collect any pending death (non-blocking), then report
    // whether this child is still unreaped. Never a kernel proc_info round-trip —
    // for the kernel's own view use box::process(pid).alive().
    bool alive() noexcept
    {
        if (pid_ == 0) return false;     // empty / moved-from supervises nothing
        if (exited_) return false;
        _M_poll_reap();
        return !exited_;
    }

    // Terminate the child. Idempotent: an empty/moved-from handle, a child already
    // reaped, or one that died on its own (the kernel reports process_not_found /
    // process_terminated) is a satisfied success. A real failure carries the
    // kernel cause. We poll for an already-observable death FIRST so we never fire
    // proc_kill at a pid the kernel may have recycled to another process.
    status kill() noexcept
    {
        if (pid_ == 0) return {};
        _M_poll_reap();                  // latch our own death if it already arrived
        if (exited_) return {};
        int rc = ::proc_kill(pid_);
        if (rc == 0) return {};
        ::error_t e = box_errno_of(rc);
        if (e == ERR_PROCESS_NOT_FOUND || e == ERR_PROCESS_TERMINATED) return {};
        return std::unexpected(error{e});
    }

    // Wait for the child to finish (timeout_ms == 0 blocks forever). An empty /
    // moved-from handle is errc::invalid_argument (there is nothing to wait for —
    // never a forever-park on pid 0). A bounded wait that expires with the child
    // still alive returns errc::timeout; otherwise the disposition (success code /
    // killed / crashed). Re-waiting a reaped child returns the cache, no block.
    result<int> wait(std::uint32_t timeout_ms = 0) noexcept
    {
        if (pid_ == 0) return std::unexpected(error{errc::invalid_argument});
        if (!exited_) _M_wait_reap(timeout_ms);
        if (!exited_) return std::unexpected(error{errc::timeout});
        return _M_to_result();
    }

    // co_await child.exited() -> result<int>. The FOREVER awaiter: suspends on the
    // current box::executor until the child finishes (for a bounded wait use
    // wait(ms)). One executor block serves several awaiters; the station table is
    // the executor's LATCH — a block routes the death INTO the durable table and
    // the following poll re-reads it (not the consumed ring slot).
    class exited_awaiter {
    public:
        explicit exited_awaiter(child *c) noexcept : _M_c(c) {}

        // An empty / moved-from handle is ready immediately (never suspends on a
        // pid-0 death that can never arrive); await_resume then reports the cause.
        bool await_ready() noexcept
        {
            return _M_c->pid_ == 0 || _M_c->exited_ || _M_c->_M_poll_reap();
        }
        bool await_suspend(std::coroutine_handle<> h)
        {
            executor::current()->wait_on(h, this, &_S_poll, &_S_block,
                                         __exec::wait_domain::touch);
            return true;
        }
        result<int> await_resume() noexcept
        {
            if (_M_c->pid_ == 0 && !_M_c->exited_)
                return std::unexpected(error{errc::invalid_argument});
            return _M_c->_M_to_result();
        }

    private:
        static bool _S_poll(void *s)
        {
            child *c = static_cast<exited_awaiter *>(s)->_M_c;
            return c->exited_ || c->_M_poll_reap();
        }
        static void _S_block(void *s, std::uint32_t ms)
        {
            child *c = static_cast<exited_awaiter *>(s)->_M_c;
            if (!c->exited_) c->_M_wait_reap(ms);
        }

        child *_M_c;
    };

    exited_awaiter exited() noexcept { return exited_awaiter{this}; }

private:
    explicit child(std::uint32_t pid, std::uint32_t gen) noexcept : pid_(pid), gen_(gen) {}

    // ~child / move-assign detach: a child still supervised (pid set, not yet
    // reaped) is forgotten from the station by its (pid, generation) so its later
    // death routes to no slot and is dropped. A reaped child is already gone from
    // the station. Never kills, never waits.
    void _M_detach() noexcept
    {
        if (pid_ != 0 && !exited_) _detail::station().deregister(pid_, gen_);
        pid_ = 0;
    }

    result<int> _M_to_result() const noexcept
    {
        if (code_ >= 0)                  return code_;
        if (code_ == PROC_EXIT_KILLED)   return std::unexpected(error{errc::process_killed});
        return std::unexpected(error{errc::process_crashed});
    }

    // Latch this child's death from the station (poll = non-blocking, wait =
    // bounded/forever). Routing a death is allocation-free (it lands in the slot
    // register_child already reserved), so these do not throw in practice; the
    // try/catch is belt-and-suspenders to keep the noexcept contract unconditional.
    bool _M_poll_reap() noexcept
    {
        if (exited_) return true;
        try {
            if (_detail::station().poll_collect(pid_, gen_, &code_)) exited_ = true;
        } catch (...) {}
        return exited_;
    }
    bool _M_wait_reap(std::uint32_t ms) noexcept
    {
        if (exited_) return true;
        try {
            if (_detail::station().wait_collect(pid_, gen_, ms, &code_)) exited_ = true;
        } catch (...) {}
        return exited_;
    }
};

}  // namespace box

#endif  // BOXCXX_BOX_CHILD_H
