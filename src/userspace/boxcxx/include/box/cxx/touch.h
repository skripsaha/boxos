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
#include "box/cxx/executor.h"

namespace box {

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
inline tag operator""_tag(const char *s, std::size_t) { return tag(s); }
}

namespace tags {
inline const tag &keyboard()        { static const tag t{TOUCH_TAG_KEYBOARD};        return t; }
inline const tag &process_died()    { static const tag t{TOUCH_TAG_PROCESS_DIED};    return t; }
inline const tag &process_spawned() { static const tag t{TOUCH_TAG_PROCESS_SPAWNED}; return t; }
inline const tag &system_shutdown() { static const tag t{TOUCH_TAG_SYSTEM_SHUTDOWN}; return t; }
inline const tag &system_reboot()   { static const tag t{TOUCH_TAG_SYSTEM_REBOOT};   return t; }
inline const tag &usb_connect()     { static const tag t{TOUCH_TAG_USB_CONNECT};     return t; }
inline const tag &usb_disconnect()  { static const tag t{TOUCH_TAG_USB_DISCONNECT};  return t; }
}

enum class provenance : std::uint16_t {
    none   = 0,
    kernel = TOUCH_FLAG_KERNEL,
    user   = TOUCH_FLAG_USER,
    tagfs  = TOUCH_FLAG_TAGFS,
};

class touch {
    Touch t_{};

public:
    touch() noexcept = default;
    explicit touch(const Touch &t) noexcept : t_(t) {}

    TouchTag      tag_id() const noexcept { return t_.tag_id; }
    std::uint32_t source() const noexcept { return t_.source_pid; }
    std::uint64_t timestamp() const noexcept { return t_.timestamp_tsc; }
    const Touch  &raw() const noexcept { return t_; }

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

inline bool publish(const tag &tg, const void *payload, std::uint32_t plen,
                    std::uint32_t after_ms = 0) noexcept
{
    return touch_send(tg.pair(), payload, plen, after_ms) >= 0;
}

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

inline bool register_tag(const tag &tg, touch_policy pol, touch_capability cap) noexcept
{
    return tg && touch_register(tg.id(), static_cast<TouchPolicy>(pol),
                                static_cast<TouchCapability>(cap)) == OK;
}

inline bool touch_irq_return() noexcept { return ::touch_irq_return() == OK; }

class subscription {
    TouchTag      id_       = TOUCH_TAG_INVALID;
    std::uint32_t drain_ms_ = 0;

public:
    subscription() noexcept = default;
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

    std::optional<touch> poll() noexcept
    {
        Touch t;
        if (id_ != TOUCH_TAG_INVALID && touch_try_pop_tag(id_, &t)) return touch(t);
        return std::nullopt;
    }
    std::optional<touch> wait(std::uint32_t timeout_ms = 0) noexcept
    {
        Touch t;
        if (id_ != TOUCH_TAG_INVALID && touch_wait_tag(id_, &t, timeout_ms)) return touch(t);
        return std::nullopt;
    }
    bool ack() noexcept { return id_ != TOUCH_TAG_INVALID && touch_ack(id_) == OK; }

    class next_awaiter {
    public:
        explicit next_awaiter(TouchTag tag_id) noexcept : _M_tag(tag_id) {}

        bool await_ready() noexcept { return (_M_got = touch_try_pop_tag(_M_tag, &_M_ev)); }
        bool await_suspend(std::coroutine_handle<> __h)
        {
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

        subscription *_M_owner = nullptr;
        touch         _M_ev{};
    };

    iterator                begin() noexcept { return iterator{id_ != TOUCH_TAG_INVALID ? this : nullptr}; }
    std::default_sentinel_t end() const noexcept { return {}; }
};

}

#endif