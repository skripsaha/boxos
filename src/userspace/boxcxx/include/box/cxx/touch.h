// boxcxx — box::touch  (the C++ face of the BoxOS Touch event multicast)
//
// Touch is BoxOS's tag-multicast event surface: a publisher sends a payload to
// a tag, and every cabin that has CLAIMED that tag receives a copy in its
// TouchRing. This header is the idiomatic C++ layer over box/touch.h:
//
//   box::tag          — an interned tag handle ("system:reboot"_tag, or
//                       box::tag("keyboard")).
//   box::subscription — a RAII claim on a tag (the consumer side); poll() /
//                       wait() / co_await next() deliver the next event OF THAT
//                       TAG, leaving other tags for their own subscribers.
//   box::touch        — a received Touch, with typed payload_as<T>() access.
//   box::publish(tag, v) / register_tag(tag, …) — the producer / policy side.
//
//   box::subscription sub("metric:tick"_tag);
//   box::publish("metric:tick"_tag, Tick{...});
//   if (auto ev = sub.poll())          handle(ev->payload_as<Tick>());
//   auto ev = co_await sub.next();     // suspends on the current box::executor
//   for (box::touch e : sub) { ... }   // stream-view (set a drain timeout to end)
//
// Tag-filtered delivery rests on the boxlib touch_try_pop_tag / touch_wait_tag
// stash: the TouchRing is cabin-wide FIFO, so non-matching events are parked
// per-cabin and handed to their own tag's consumer later — multiple box::touch
// subscriptions in one cabin stay independent. This is a box:: extension, not
// std; the event terminal (a drained stream-view) is a chosen timeout, never a
// Unix EOF.
//
// Do NOT mix box::subscription with the Ф12 whole-ring box::touch_event() in
// the same cabin: touch_event() pops the cabin-wide ring with no tag stash, so
// it can swallow an event a subscription was waiting for (and vice-versa, the
// stash holds events touch_event() never inspects). Pick one consumption model
// per cabin — the tag-filtered box::touch layer here, or the raw any-tag awaiter.
#ifndef BOXCXX_BOX_TOUCH_H
#define BOXCXX_BOX_TOUCH_H

#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <span>
#include <type_traits>

#include "box/touch.h"
#include "box/error.h"
#include "box/cxx/executor.h"  // box::executor, box::__exec::waiter, wait_on

namespace box {

// ── box::tag — an interned Touch tag handle ─────────────────────────────────
// Wraps the kernel's (full, bare) id pair. Construction interns the string
// (one syscall); cache the tag in a variable rather than re-interning in a hot
// loop. id() is the specific id used to claim / consume; pair() hits both the
// (key:value) and (key) buckets on publish.
class tag {
    TouchTagPair p_{TOUCH_TAG_INVALID, TOUCH_TAG_INVALID};

public:
    tag() noexcept = default;
    explicit tag(const char *name) noexcept : p_(touch_intern(name)) {}
    explicit tag(TouchTagPair p) noexcept : p_(p) {}

    bool valid() const noexcept { return touch_pair_choose(p_) != TOUCH_TAG_INVALID; }
    explicit operator bool() const noexcept { return valid(); }
    TouchTag     id() const noexcept { return touch_pair_choose(p_); }
    TouchTagPair pair() const noexcept { return p_; }
};

inline namespace literals {
// "keyboard"_tag — interns at runtime (touch_intern is a syscall).
inline tag operator""_tag(const char *s, std::size_t) { return tag(s); }
} // namespace literals

// ── box::tags — the catalog of canonical kernel Touch tags ──────────────────
// Pre-printed luggage tags: grab the named handle off its hook instead of
// hand-interning a string each time. Each accessor caches a process-lifetime
// interned tag in a function-local static (lazy — interned on first call, once
// the registry is up), so a hot loop never round-trips the registry. The
// interned id is a process-global registry fact; each strand still makes its
// own claim (a box::subscription) to actually receive the tag.
namespace tags {
inline const tag &keyboard()        { static const tag t{TOUCH_TAG_KEYBOARD};        return t; }
inline const tag &process_died()    { static const tag t{TOUCH_TAG_PROCESS_DIED};    return t; }
inline const tag &process_spawned() { static const tag t{TOUCH_TAG_PROCESS_SPAWNED}; return t; }
inline const tag &system_shutdown() { static const tag t{TOUCH_TAG_SYSTEM_SHUTDOWN}; return t; }
inline const tag &system_reboot()   { static const tag t{TOUCH_TAG_SYSTEM_REBOOT};   return t; }
inline const tag &usb_connect()     { static const tag t{TOUCH_TAG_USB_CONNECT};     return t; }
inline const tag &usb_disconnect()  { static const tag t{TOUCH_TAG_USB_DISCONNECT};  return t; }
}  // namespace tags

// ── box::provenance — which layer franked a Touch into the system ───────────
// The postmark on the envelope. box::publish from userspace stamps `user`; the
// kernel's own publishers stamp `kernel`; TagFS-originated touches carry `tagfs`.
enum class provenance : std::uint16_t {
    none   = 0,
    kernel = TOUCH_FLAG_KERNEL,
    user   = TOUCH_FLAG_USER,
    tagfs  = TOUCH_FLAG_TAGFS,
};

// ── box::touch — a received Touch with typed payload access ─────────────────
class touch {
    Touch t_{};

public:
    touch() noexcept = default;
    explicit touch(const Touch &t) noexcept : t_(t) {}

    TouchTag      tag_id() const noexcept { return t_.tag_id; }
    std::uint32_t source() const noexcept { return t_.source_pid; }
    std::uint64_t timestamp() const noexcept { return t_.timestamp_tsc; }
    const Touch  &raw() const noexcept { return t_; }

    // Provenance — which layer franked this Touch into the system (Touch.flags).
    std::uint16_t   flags() const noexcept { return t_.flags; }
    box::provenance provenance() const noexcept
    {
        return static_cast<box::provenance>(
            t_.flags & (TOUCH_FLAG_KERNEL | TOUCH_FLAG_USER | TOUCH_FLAG_TAGFS));
    }
    bool from_kernel() const noexcept { return (t_.flags & TOUCH_FLAG_KERNEL) != 0; }
    bool from_user() const noexcept { return (t_.flags & TOUCH_FLAG_USER) != 0; }
    bool from_tagfs() const noexcept { return (t_.flags & TOUCH_FLAG_TAGFS) != 0; }

    std::span<const std::byte> payload() const noexcept
    {
        std::uint32_t n =
            t_.payload_len <= BOXOS_TOUCH_PAYLOAD_MAX ? t_.payload_len : BOXOS_TOUCH_PAYLOAD_MAX;
        return {reinterpret_cast<const std::byte *>(t_.payload), n};
    }

    // Reconstruct a trivially-copyable T from the payload prefix; nullopt if the
    // payload is shorter than T.
    template <class T>
    std::optional<T> payload_as() const noexcept
    {
        static_assert(std::is_trivially_copyable_v<T>,
                      "box::touch::payload_as<T> requires a trivially copyable T");
        static_assert(sizeof(T) <= BOXOS_TOUCH_PAYLOAD_MAX,
                      "box::touch::payload_as<T>: T exceeds the 96-byte Touch payload");
        if (t_.payload_len < sizeof(T)) return std::nullopt;
        T v;
        __builtin_memcpy(&v, t_.payload, sizeof(T));
        return v;
    }
};

// ── producer / policy side ──────────────────────────────────────────────────
inline bool publish(const tag &tg, const void *payload, std::uint32_t plen,
                    std::uint32_t after_ms = 0) noexcept
{
    return touch_send(tg.pair(), payload, plen, after_ms) >= 0;
}

// ‼ Constrained against pointers, and the reason was measured rather than
// imagined. `publish(tg, name.data(), name.size())` reads like the raw
// overload above and is not: a `const char*` binds to `const T&` by identity
// while the raw overload needs a pointer conversion, so the TEMPLATE wins —
// and it sends the eight bytes of the pointer itself, silently, with the
// length quietly becoming after_ms. Ф45 wrote exactly that call and the
// Touch arrived carrying 8 bytes of an address instead of "Asia/Tokyo".
// A pointer value means nothing in another address space, so no caller can
// have meant this; refusing it turns a silent wrong payload into a
// compile error, and leaves publish(tg, ptr, len) unambiguously raw.
template <class T>
    requires (!std::is_pointer_v<T>)
inline bool publish(const tag &tg, const T &v, std::uint32_t after_ms = 0) noexcept
{
    static_assert(std::is_trivially_copyable_v<T>,
                  "box::publish(tag, T) requires a trivially copyable payload");
    static_assert(sizeof(T) <= BOXOS_TOUCH_PAYLOAD_MAX,
                  "box::publish(tag, T): T exceeds the 96-byte Touch payload");
    return publish(tg, &v, static_cast<std::uint32_t>(sizeof(T)), after_ms);
}

enum class touch_policy : unsigned {
    edge    = TOUCH_POLICY_EDGE,
    level   = TOUCH_POLICY_LEVEL,
    latched = TOUCH_POLICY_LATCHED,
};
enum class touch_capability : unsigned {
    open        = TOUCH_CAP_OPEN,
    owners      = TOUCH_CAP_OWNERS,
    kernel_only = TOUCH_CAP_KERNEL_ONLY,
};

// Register a tag's delivery policy / capability (the registry side).
inline bool register_tag(const tag &tg, touch_policy pol, touch_capability cap) noexcept
{
    return tg && touch_register(tg.id(), static_cast<TouchPolicy>(pol),
                                static_cast<TouchCapability>(cap)) == OK;
}

// Low-level escape hatch for a hand-rolled INTERRUPT-mode handler: signal the
// kernel that the in-flight IRQ-mode upcall is done and any pending touches may
// replay. You almost certainly want REST (box::subscription / co_await next())
// instead — INTERRUPT mode is the Unix async-signal model this layer exists to
// replace; this is here only for the rare expert hand-roll.
inline bool touch_irq_return() noexcept { return ::touch_irq_return() == OK; }

// ── box::subscription — RAII claim on a tag (the consumer side) ─────────────
class subscription {
    TouchTag      id_       = TOUCH_TAG_INVALID;
    std::uint32_t drain_ms_ = 0;  // stream-view per-step wait (0 == block forever)

public:
    subscription() noexcept = default;
    // REST claim — the PULL model: poll() / wait() / co_await next() deliver the
    // event to YOU. The kernel's PUSH model (run a manifest on every touch, via
    // TOUCH_REACT) and INTERRUPT mode are deliberately not offered on this type.
    explicit subscription(const tag &tg) noexcept
        : id_(tg.id())
    {
        if (id_ != TOUCH_TAG_INVALID && touch_claim(id_, TOUCH_REST, 0, 0) != OK)
            id_ = TOUCH_TAG_INVALID;
    }
    subscription(const subscription &)            = delete;
    subscription &operator=(const subscription &) = delete;
    subscription(subscription &&o) noexcept : id_(o.id_), drain_ms_(o.drain_ms_)
    {
        o.id_ = TOUCH_TAG_INVALID;
    }
    subscription &operator=(subscription &&o) noexcept
    {
        if (this != &o) {
            if (id_ != TOUCH_TAG_INVALID) touch_release(id_);
            id_       = o.id_;
            drain_ms_ = o.drain_ms_;
            o.id_     = TOUCH_TAG_INVALID;
        }
        return *this;
    }
    ~subscription() { if (id_ != TOUCH_TAG_INVALID) touch_release(id_); }

    explicit operator bool() const noexcept { return id_ != TOUCH_TAG_INVALID; }
    TouchTag id() const noexcept { return id_; }

    // Non-blocking: the next event of this tag, stash-aware. nullopt if none.
    std::optional<touch> poll() noexcept
    {
        Touch t;
        if (id_ != TOUCH_TAG_INVALID && touch_try_pop_tag(id_, &t)) return touch(t);
        return std::nullopt;
    }
    // Blocking: wait up to timeout_ms (0 == forever) for the next event of this
    // tag. nullopt on timeout.
    std::optional<touch> wait(std::uint32_t timeout_ms = 0) noexcept
    {
        Touch t;
        if (id_ != TOUCH_TAG_INVALID && touch_wait_tag(id_, &t, timeout_ms)) return touch(t);
        return std::nullopt;
    }
    // Acknowledge a latched/level event so the next edge can fire.
    bool ack() noexcept { return id_ != TOUCH_TAG_INVALID && touch_ack(id_) == OK; }

    // ── coroutine-native: co_await sub.next() -> optional<touch> ────────────
    // Tag-filtered; suspends on the current box::executor (same mechanism as the
    // Ф12 / box::brook awaiters). nullopt only if the source never resolves.
    class next_awaiter {
    public:
        explicit next_awaiter(TouchTag tag_id) noexcept : _M_tag(tag_id) {}

        bool await_ready() noexcept { return (_M_got = touch_try_pop_tag(_M_tag, &_M_ev)); }
        bool await_suspend(std::coroutine_handle<> __h)
        {
            // Explicit TOUCH domain: the brace-init form happened to default
            // _M_domain to 0 (== touch) correctly, but relying on the enum's
            // numeric value is fragile — name it, and silence the missing-
            // initializer warning, like the box::brook awaiters.
            executor::current()->wait_on(__h, this, &_S_poll, &_S_block,
                                         __exec::wait_domain::touch);
            return true;
        }
        std::optional<touch> await_resume() noexcept
        {
            if (_M_got) return touch(_M_ev);
            return std::nullopt;
        }

    private:
        static bool _S_poll(void *__s)
        {
            auto *__a = static_cast<next_awaiter *>(__s);
            return __a->_M_got || (__a->_M_got = touch_try_pop_tag(__a->_M_tag, &__a->_M_ev));
        }
        static void _S_block(void *__s, std::uint32_t __ms)
        {
            auto *__a = static_cast<next_awaiter *>(__s);
            if (!__a->_M_got) __a->_M_got = touch_wait_tag(__a->_M_tag, &__a->_M_ev, __ms);
        }

        TouchTag _M_tag;
        Touch    _M_ev{};
        bool     _M_got = false;
    };

    next_awaiter next() noexcept { return next_awaiter{id_}; }

    // ── stream-view (input_range) ───────────────────────────────────────────
    // Each ++ waits drain_ms for the next event of this tag; the range ends
    // when a wait yields nothing. Set a finite drain timeout to drain the
    // currently-pending burst and stop; the default (0) is an endless event
    // loop you break out of yourself.
    void          set_drain_timeout(std::uint32_t ms) noexcept { drain_ms_ = ms; }
    std::uint32_t drain_timeout() const noexcept { return drain_ms_; }

    class iterator {
    public:
        using iterator_concept  = std::input_iterator_tag;
        using iterator_category = std::input_iterator_tag;
        using value_type        = touch;
        using difference_type   = std::ptrdiff_t;
        using reference         = const touch &;
        using pointer           = const touch *;

        iterator() noexcept = default;
        explicit iterator(subscription *owner) : _M_owner(owner) { _M_advance(); }

        reference operator*() const noexcept { return _M_ev; }
        pointer   operator->() const noexcept { return &_M_ev; }
        iterator &operator++() { _M_advance(); return *this; }
        void      operator++(int) { _M_advance(); }

        friend bool operator==(const iterator &it, std::default_sentinel_t) noexcept
        {
            return it._M_owner == nullptr;
        }

    private:
        void _M_advance()
        {
            if (!_M_owner) return;
            if (auto e = _M_owner->wait(_M_owner->drain_ms_)) _M_ev = *e;
            else _M_owner = nullptr;
        }

        subscription *_M_owner = nullptr;  // nullptr == past-the-end
        touch         _M_ev{};
    };

    iterator                begin() noexcept { return iterator{id_ != TOUCH_TAG_INVALID ? this : nullptr}; }
    std::default_sentinel_t end() const noexcept { return {}; }
};

} // namespace box

#endif // BOXCXX_BOX_TOUCH_H
