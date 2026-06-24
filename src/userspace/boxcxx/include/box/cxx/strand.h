// boxcxx — box::strand  (the native BoxOS face of an in-cabin execution context)
//
// A strand is an extra execution context inside the caller's cabin: it shares
// the cabin's address space (CR3), heap, code and IPC rings, but carries its own
// stack and scheduler state. std::thread is already a strand under the hood; this
// header is the BoxOS-native superset that names the substrate directly and adds
// what std cannot express:
//
//   box::strand        — std::thread, but the destructor JOINS instead of calling
//                        std::terminate. Dropping a strand handle winds the strand
//                        down cleanly; you are never punished for forgetting an
//                        explicit join (decision: join-on-destroy). Everything else
//                        (spawn + per-strand TLS bootstrap + the UAF-safe join
//                        rendezvous) is inherited verbatim from std::thread, which
//                        this class COMPOSES — zero duplicated lifecycle code.
//   box::park / box::wake
//                      — the kernel address-park primitive (addr_park/addr_wake)
//                        as typed, result-returning free functions. This is the
//                        BoxOS analogue of futex without the futex vocabulary: a
//                        strand parks on a shared word until a sibling writes it and
//                        wakes the waiter, with no lost wakes (the kernel re-checks
//                        the word at park time). The typed overload is 8-byte-only
//                        because the kernel compares a FULL 64-bit word — for
//                        narrower per-object waits use std::atomic<T>::wait/notify.
//   box::this_strand   — id() / yield() for the running strand (main strand → cabin
//                        pid), the strand-flavoured spelling of this_thread.
//   strand_spawned / strand_exited / strand_parked / strand_woken + strand_watch
//                      — POD decoders for the kernel's system-wide strand lifecycle
//                        Touch broadcasts, and a thin RAII multiplexer over them.
//
//   box::strand s([]{ work(); });   // spawns; joins when s leaves scope
//   box::strand_watch w;            // observe the whole system's strand lifecycle
//   for (;;) if (auto e = w.poll()) on_event(*e);
//
// Scheduling: box::strand deliberately exposes NO per-strand scheduling API. In
// BoxOS scheduling stance is a CABIN-level property — steer it with process tags
// (box::this_process::add_tag / box::tag_scope), not per-strand priority, which is
// not a BoxOS concept. The only per-strand control here is a cooperative
// box::this_strand::yield().
//
// Deadlock caveat (join-on-destroy): because ~box::strand joins, letting a strand
// handle die while its strand is blocked waiting on the destroying context will
// hang exactly as an explicit join() would. This is the deliberate, std::jthread-
// style trade (clean shutdown over a terminate landmine) — make the strand
// finishable (signal it, drop the shared flag) before its handle is destroyed.
//
// park address caveat: box::park resolves the atomic's VIRTUAL address to its
// physical frame in the kernel and parks on that frame, so two cabins parking on
// the same shared physical page rendezvous correctly, while a private stack word
// is unique to its parker. The word must stay mapped for the park's duration
// (a live std::atomic on the stack/heap satisfies this).
//
// This is a box:: extension, not part of std; the std::thread bridge (id and the
// shared substrate) is one-way and lossless.
#ifndef BOXCXX_BOX_STRAND_H
#define BOXCXX_BOX_STRAND_H

#include <atomic>
#include <chrono>
#include <compare>
#include <cstdint>
#include <format>         // std::formatter<box::strand::id> (decimal pid)
#include <functional>     // std::hash<box::strand::id>
#include <optional>
#include <thread>         // box::strand COMPOSES std::thread (spawn + Join + TLS)
#include <type_traits>
#include <utility>        // std::move / std::swap / std::forward

#include "box/sync.h"               // addr_park / addr_wake — the kernel park word
#include "box/error.h"              // OK / ERR_TIMEOUT / ERR_ADDR_VALUE_MISMATCH
#include "box/core/strand_self.h"   // strand_self — this_strand::id (main → cabin pid)
#include "box/cxx/touch.h"          // box::tag / box::subscription / box::event

namespace box {

// ── box::park_status — why a box::park call returned ────────────────────────
// Re-check your predicate on every woke (a spurious-looking wake is just a sibling
// store you should re-read). value_mismatch means the word already moved at park
// time, so no sleep happened — treat it as "the condition you waited for is
// already true". Errors come back as this enum, never as an errno.
enum class park_status {
    woke,            // a sibling addr_wake'd this word
    timeout,         // the timeout elapsed before any wake
    value_mismatch,  // *addr != expected at park time — no sleep occurred
    error,           // a kernel error (bad address, etc.)
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
}  // namespace _strand_detail

// ── box::park / box::wake — the kernel address-park, typed ──────────────────
// Typed, 8-byte-only: the kernel compares a full 64-bit word at park time, so the
// atomic MUST be 8 bytes wide (uint64_t / int64_t / a pointer). `expected` is the
// value the word must still hold to actually park; if it already changed you get
// value_mismatch and no sleep (lost-wake-safe). timeout_ms == 0 waits forever.
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
    // The kernel compares the FULL 64-bit word, padding bytes included, while
    // std::atomic canonicalises stored padding to zero — so a T with padding (or
    // a floating-point T with -0.0/NaN aliases) would compare a value the store
    // can never reproduce, parking forever or never. Require a unique object
    // representation so `expected` is exactly the bytes the kernel sees.
    static_assert(std::has_unique_object_representations_v<T>,
                  "box::park: T must have no padding/trap bits (use uint64_t/"
                  "int64_t/a pointer — not floating-point or a padded struct)");
    std::uint64_t word;
    __builtin_memcpy(&word, &expected, 8);
    return _strand_detail::map_park(addr_park(&a, word, timeout_ms));
}

// Wake up to `count` strands parked on this word (count == 0 wakes all). Returns
// true iff the kernel accepted the wake.
template <class T>
inline bool wake(const std::atomic<T> &a, std::uint32_t count = 0) noexcept
{
    static_assert(sizeof(T) == 8,
                  "box::wake: 8-byte atomic only (matches box::park's kernel word)");
    static_assert(std::has_unique_object_representations_v<T>,
                  "box::wake: 8-byte, padding-free atomic only (symmetric with box::park)");
    return addr_wake(&a, count) == OK;
}

// Raw expert escape hatch (mirrors the boxlib addr_park/addr_wake shape): park on
// any 8-byte-aligned word by address. Prefer the typed overloads; reach for this
// only when the word is not a std::atomic you hold.
inline park_status park(const volatile void *addr, std::uint64_t expected,
                        std::uint32_t timeout_ms = 0) noexcept
{
    return _strand_detail::map_park(addr_park(addr, expected, timeout_ms));
}
inline bool wake(const volatile void *addr, std::uint32_t count = 0) noexcept
{
    return addr_wake(addr, count) == OK;
}

// Chrono-typed park: ceil the duration to whole milliseconds (>= requested),
// nudge a sub-ms positive wait up to 1ms, clamp to the kernel's u32 — the same
// conversion std::this_thread::sleep_for performs — then park. A non-positive
// duration returns `timeout` WITHOUT inspecting the word (the deadline has already
// passed, so no syscall happens) — re-check your predicate before treating a
// timeout as failure rather than as "already satisfied".
template <class Rep, class Period>
inline park_status park_for(const std::atomic<std::uint64_t> &a, std::uint64_t expected,
                            std::chrono::duration<Rep, Period> d) noexcept
{
    if (d <= std::chrono::duration<Rep, Period>::zero())
        return park_status::timeout;  // <=0 → the deadline has already passed, no park
    auto ms = std::chrono::ceil<std::chrono::milliseconds>(d).count();
    if (ms < 1) ms = 1;
    if (ms > 0xFFFFFFFFLL) ms = 0xFFFFFFFFLL;
    return park(a, expected, static_cast<std::uint32_t>(ms));
}

// Readability sugar over the magic count (1 == wake one waiter, 0 == wake all).
template <class T> inline bool wake_one(const std::atomic<T> &a) noexcept { return wake(a, 1); }
template <class T> inline bool wake_all(const std::atomic<T> &a) noexcept { return wake(a, 0); }
inline bool wake_one(const volatile void *addr) noexcept { return wake(addr, 1); }
inline bool wake_all(const volatile void *addr) noexcept { return wake(addr, 0); }

// ── box::strand ──────────────────────────────────────────────────────────────
// An in-cabin execution context with std::thread semantics, except the destructor
// JOINS a still-joinable strand instead of calling std::terminate (decision:
// join-on-destroy). The class composes a std::thread, so spawn, the per-strand
// TLS bootstrap, and the UAF/double-free-safe join rendezvous are all inherited;
// only the lifetime policy differs.
class strand {
public:
    using native_handle_type = std::thread::native_handle_type;  // the strand pid

    // ── box::strand::id — a strand's kernel identity ──────────────────────────
    // The value IS the strand's kernel pid (Touch-observable, system-wide unique
    // while joinable), so two ids compare equal iff they name the same kernel
    // strand. pid_ == 0 is the "not a strand" id (default / after join / detach /
    // moved-from) and never collides with a live strand. This is box::strand's own
    // value type (std::thread::id's ctor is private); it bridges losslessly to a
    // std::thread::id via the explicit conversion.
    class id {
    public:
        id() noexcept = default;

        friend bool operator==(id a, id b) noexcept { return a.pid_ == b.pid_; }
        friend std::strong_ordering operator<=>(id a, id b) noexcept
        {
            return a.pid_ <=> b.pid_;
        }

        std::uint32_t native() const noexcept { return pid_; }

        // Lossless bridge to std::thread::id (its ctor is private, so go through
        // the public MakeId factory): the same pid, viewed as a std identity.
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

    // Spawn a strand running f(args...). Delegates to the composed std::thread,
    // which does the decay-copy, joinable spawn, TLS bootstrap and Join wiring.
    // Constrained so it is never chosen as the move ctor.
    template <class F, class... Args,
              class = std::enable_if_t<!std::is_same_v<std::remove_cvref_t<F>, strand>>>
    explicit strand(F &&f, Args &&...args)
        : t_(std::forward<F>(f), std::forward<Args>(args)...)
    {
    }

    strand(const strand &)            = delete;
    strand &operator=(const strand &) = delete;

    strand(strand &&) noexcept = default;

    // Move-assign: join (NOT terminate) a still-running target first — the join-
    // on-destroy policy applied to the value being overwritten — then steal o.
    strand &operator=(strand &&o) noexcept
    {
        if (this != &o) {
            if (joinable()) join_nothrow();
            t_ = std::move(o.t_);
        }
        return *this;
    }

    // The join-on-destroy decision: a still-joinable strand is wound down cleanly,
    // never std::terminate. See the deadlock caveat at the top of this header.
    ~strand()
    {
        if (joinable()) join_nothrow();
    }

    bool joinable() const noexcept { return t_.joinable(); }
    id   get_id() const noexcept { return id{t_.native_handle()}; }
    native_handle_type native_handle() const noexcept { return t_.native_handle(); }

    // Wrap a strand pid as a box::strand::id (the id ctor is private; strand is its
    // friend). Used by box::this_strand::id for the running strand — the same
    // public-factory shape as std::thread::MakeId.
    static id make_id(std::uint32_t pid) noexcept { return id{pid}; }

    // Blocking join. May throw (std::thread::join throws if !joinable) — for the
    // never-throwing lifetime path the destructor uses join_nothrow internally.
    void join() { t_.join(); }
    // Still available: drop ownership and let the strand run on unsupervised.
    void detach() { t_.detach(); }

    void swap(strand &o) noexcept { t_.swap(o.t_); }

    // App-Cores that can make true parallel forward progress (0 if unknown).
    static unsigned hardware_concurrency() noexcept
    {
        return std::thread::hardware_concurrency();
    }

private:
    // The never-throwing join the destructor and move-assign use: a join failure
    // (e.g. a not-joinable race) must not throw out of those paths.
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

// ── box::this_strand — identity / yield for the running strand ──────────────
// The strand-flavoured spelling of std::this_thread. id() of the main strand is
// the cabin pid (a cabin's first strand has no separate pid). Scheduling stance
// is cabin-level (steer via box::this_process::add_tag / box::tag_scope); the only
// per-strand control is this cooperative yield.
namespace this_strand {
inline strand::id id() noexcept { return strand::make_id(strand_self()); }
inline void       yield() noexcept { ::yield(); }
}  // namespace this_strand

// ── strand lifecycle decoders ───────────────────────────────────────────────
// POD views of the kernel's strand lifecycle Touch payloads, byte-for-byte. All
// four tags are SYSTEM-WIDE broadcasts: every cabin claiming the tag sees ALL
// cabins' strand events, not just its own.
//
// Quirk to know: `cabin_pid` is NOT the strand's own cabin's pid — it is the
// cabin's spawner (launcher) pid. parked/woken carry no cabin field at all.

// strand:spawned / strand:exited — 8 bytes, `{ pid, cabin_pid }`.
struct strand_spawned {
    std::uint32_t pid;        // the strand's kernel pid
    std::uint32_t cabin_pid;  // the cabin's LAUNCHER pid (spawner), not its own pid
};
struct strand_exited {
    std::uint32_t pid;        // the strand's kernel pid
    std::uint32_t cabin_pid;  // the cabin's LAUNCHER pid (spawner), not its own pid
};

// strand:parked — packed to 12 bytes, `{ phys, pid }`. MUST be packed: the kernel
// emits a 12-byte payload and box::event::payload_as rejects a wider T.
struct __attribute__((packed)) strand_parked {
    std::uint64_t phys;  // physical frame the strand parked on
    std::uint32_t pid;   // the parking strand's kernel pid
};

// strand:woken — packed to 16 bytes, `{ phys, pid, waker_pid }`.
struct __attribute__((packed)) strand_woken {
    std::uint64_t phys;       // physical frame the strand was parked on
    std::uint32_t pid;        // the woken strand's kernel pid
    std::uint32_t waker_pid;  // the strand that issued the wake
};

static_assert(sizeof(strand_spawned) == 8,  "strand_spawned must match the 8-byte kernel payload");
static_assert(sizeof(strand_exited)  == 8,  "strand_exited must match the 8-byte kernel payload");
static_assert(sizeof(strand_parked)  == 12, "strand_parked must be packed to 12 bytes (kernel payload)");
static_assert(sizeof(strand_woken)   == 16, "strand_woken must be packed to 16 bytes (kernel payload)");

// ── box::strand_watch — observe the system-wide strand lifecycle ────────────
enum class strand_event_kind { spawned, exited, parked, woken };

// One decoded lifecycle event, tagged by kind with the matching decoder in the
// union. `source` is the publisher pid (the kernel). strand_pid() returns the
// active member's strand pid regardless of kind.
struct strand_event {
    strand_event_kind kind;
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
        case strand_event_kind::spawned: return spawned.pid;
        case strand_event_kind::exited:  return exited.pid;
        case strand_event_kind::parked:  return parked.pid;
        case strand_event_kind::woken:   return woken.pid;
        }
        return 0;
    }
};

// A thin RAII multiplexer over the four strand:* tags. Holds one subscription
// each; poll() drains them in a fixed order and decodes the next event. This is a
// light convenience, not a heavy fair multiplexer — wait() round-robins with a
// bounded per-tag step, so it may return slightly after the requested ms. Move-
// only (its subscription members are move-only).
//
// Per-strand consumption (Ф21): Touch consumption (this watch, and box::
// subscription generally) is now safe concurrently from multiple strands in one
// cabin. Each strand drains its OWN TouchRing into its OWN per-strand stash (the
// boxlib substrate stash is per-strand, no shared mutable state, no lock), so
// two strands may each drive their own watch/subscriptions at the same time. A
// single watch object is still move-only and owned by one strand — share the
// observation by giving each strand its own watch, not by passing one across
// strands. Observing other strands' lifecycle from the main strand (the usual
// case) remains exactly right. Routing note: a tag claim delivers only to the
// CLAIMING strand's ring, so each strand must make its own claim — a tag claimed
// by one strand is not received by another.
//
// Filtering: spawned/exited can be narrowed to a cabin with set_filter_cabin
// (matching the LAUNCHER pid). parked/woken carry no cabin field, so they are
// delivered system-wide unfiltered — match on strand_pid() yourself.
//
// wait() round-robins because there is no kernel "wait on any of N tags"
// primitive yet (a future kernel addition); each per-tag slice is a real event-
// driven kernel wait, only the 4-way fan-in is a userspace rotation. For a truly
// blocking wait on ONE kind, watch that single tag with box::subscription.
class strand_watch {
public:
    strand_watch() noexcept
        : spawned_("strand:spawned"_tag),
          exited_("strand:exited"_tag),
          parked_("strand:parked"_tag),
          woken_("strand:woken"_tag)
    {
    }

    // True iff all four tags were claimed.
    explicit operator bool() const noexcept
    {
        return spawned_ && exited_ && parked_ && woken_;
    }

    // Best-effort cabin filter: drop spawned/exited events whose cabin_pid does
    // not match `spawner_pid`. parked/woken have no cabin field and always pass
    // through; pass 0 to disable filtering.
    void set_filter_cabin(std::uint32_t spawner_pid) noexcept { filter_ = spawner_pid; }

    // Non-blocking: the next strand event from any of the four tags, in a fixed
    // deterministic order (spawned, exited, parked, woken). nullopt if none.
    std::optional<strand_event> poll() noexcept
    {
        if (auto e = poll_spawned()) return e;
        if (auto e = poll_exited())  return e;
        if (auto e = poll_parked())  return e;
        if (auto e = poll_woken())   return e;
        return std::nullopt;
    }

    // Blocking: poll first, then round-robin a short bounded wait across the four
    // tags until one fires or ms elapses (ms == 0 blocks forever). May return
    // slightly after ms — this is a thin helper, not a precise multiplex.
    std::optional<strand_event> wait(std::uint32_t ms = 0) noexcept
    {
        if (auto e = poll()) return e;
        constexpr std::uint32_t step = 16;  // per-tag round-robin slice
        std::uint32_t waited = 0;
        for (;;) {
            std::uint32_t slice = (ms == 0) ? step
                                            : ((ms - waited < step) ? (ms - waited) : step);
            if (slice == 0) slice = 1;
            if (auto e = wait_one(spawned_, strand_event_kind::spawned, slice)) return e;
            if (auto e = wait_one(exited_,  strand_event_kind::exited,  slice)) return e;
            if (auto e = wait_one(parked_,  strand_event_kind::parked,  slice)) return e;
            if (auto e = wait_one(woken_,   strand_event_kind::woken,   slice)) return e;
            if (ms != 0) {
                waited += slice * 4;
                if (waited >= ms) return std::nullopt;
            }
        }
    }

private:
    std::optional<strand_event> decode_spawned(const event &ev) noexcept
    {
        auto p = ev.payload_as<strand_spawned>();
        if (!p) return std::nullopt;
        if (filter_ && p->cabin_pid != filter_) return std::nullopt;
        strand_event e{strand_event_kind::spawned, ev.source(), {}};
        e.spawned = *p;
        return e;
    }
    std::optional<strand_event> decode_exited(const event &ev) noexcept
    {
        auto p = ev.payload_as<strand_exited>();
        if (!p) return std::nullopt;
        if (filter_ && p->cabin_pid != filter_) return std::nullopt;
        strand_event e{strand_event_kind::exited, ev.source(), {}};
        e.exited = *p;
        return e;
    }
    std::optional<strand_event> decode_parked(const event &ev) noexcept
    {
        auto p = ev.payload_as<strand_parked>();
        if (!p) return std::nullopt;
        strand_event e{strand_event_kind::parked, ev.source(), {}};
        e.parked = *p;
        return e;
    }
    std::optional<strand_event> decode_woken(const event &ev) noexcept
    {
        auto p = ev.payload_as<strand_woken>();
        if (!p) return std::nullopt;
        strand_event e{strand_event_kind::woken, ev.source(), {}};
        e.woken = *p;
        return e;
    }

    std::optional<strand_event> poll_spawned() noexcept
    {
        if (auto ev = spawned_.poll()) return decode_spawned(*ev);
        return std::nullopt;
    }
    std::optional<strand_event> poll_exited() noexcept
    {
        if (auto ev = exited_.poll()) return decode_exited(*ev);
        return std::nullopt;
    }
    std::optional<strand_event> poll_parked() noexcept
    {
        if (auto ev = parked_.poll()) return decode_parked(*ev);
        return std::nullopt;
    }
    std::optional<strand_event> poll_woken() noexcept
    {
        if (auto ev = woken_.poll()) return decode_woken(*ev);
        return std::nullopt;
    }

    // A filtered-out event must not consume the whole slice without a retry, so a
    // dropped spawned/exited re-arms a fresh wait until the slice is spent.
    std::optional<strand_event> wait_one(subscription &sub, strand_event_kind kind,
                                         std::uint32_t slice) noexcept
    {
        if (auto ev = sub.wait(slice)) {
            switch (kind) {
            case strand_event_kind::spawned: return decode_spawned(*ev);
            case strand_event_kind::exited:  return decode_exited(*ev);
            case strand_event_kind::parked:  return decode_parked(*ev);
            case strand_event_kind::woken:   return decode_woken(*ev);
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

}  // namespace box

// ── std::hash / std::formatter for box::strand::id ──────────────────────────
// The identity is a uint32_t pid: equal ids hash equal (hash key requirement),
// and the formatter prints the pid in decimal, inheriting the integer spec parser
// from formatter<uint32_t> so width/fill/align work.
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

#endif  // BOXCXX_BOX_STRAND_H
