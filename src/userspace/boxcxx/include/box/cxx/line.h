// boxcxx — box::line<T>  (the typed face of BoxOS process-to-process messaging)
//
// Where box::message is the raw received record and box::send/broadcast are the
// untyped verbs, box::line<T> is the typed BINDING that names one conversation
// and frames every payload as a trivially-copyable T — the sibling of
// box::brook<T> (typed Brook stream) and box::current<T> (typed Current stream),
// for the IPC spine.
//
// A line is NOT a buffer, NOT a socket, NOT a file descriptor: it stores nothing.
// The per-strand ResultRing IS the queue; a line is a non-owning VALUE (copyable,
// like std::span) that merely remembers WHO it talks to and at WHAT type. Two
// flavors, one name:
//
//   box::line<T>::to(pid)        — the PEER flavor: send() routes to one strand by
//                                  pid (::send). peer() is that pid, group() null.
//   box::line<T>::on("group")    — the GROUP flavor: send() fans out to every
//                                  strand that carries the tag (::broadcast). The
//                                  sender is never one of its own recipients, and
//                                  a receiver must itself CARRY the tag (box::
//                                  tag_scope / box::this_process::add_tag) for a
//                                  group message to land in its ring — membership
//                                  is the tag, not a subscription on the line.
//
//   auto l = box::line<Tick>::to(peer_pid);
//   l.send(t);                          // FALLIBLE -> box::status (real cause)
//   if (auto t = l.try_recv()) { ... }  // EVENT: non-blocking, nullopt if none
//   if (auto t = l.recv_for(100)) ...   // EVENT: block <=100ms, nullopt on timeout
//   if (auto t = co_await l.recv()) ... // suspends on the current box::executor
//
// The inbox contract (the substrate's one constraint): a strand's ResultRing is
// FIFO-SHARED by every IPC consumer in that strand (line/box::call/next_message/
// result_any all draw from the one ring). recv() does NOT filter by peer — a line
// reads whatever delivery is next, because filtering would STEAL a record a
// sibling consumer is owed. So a line is "one isolated conversation per strand":
// the routing isolates strands from each other (a spawned strand has its own ring;
// pids never cross), but WITHIN a strand keep at most one conversation in flight
// and validate from() (box::message) when identity matters.
//
// Fallible vs event: send() can fail for a real reason (bad pid, peer gone, ring
// full, no subscribers) and returns box::status carrying that cause; recv is an
// EVENT (a delivery either is or is not here) and returns std::optional<T>, never
// a box::error. A short or cross-cabin payload decodes to nullopt through
// box::message's data_addr guard — recv never dereferences an unmapped record.
//
// This is a box:: extension, not part of std.
#ifndef BOXCXX_BOX_LINE_H
#define BOXCXX_BOX_LINE_H

#include <coroutine>
#include <cstdint>
#include <optional>
#include <type_traits>

#include "box/ipc.h"            // ::receive / ::receive_wait + Result
#include "box/cxx/error.h"      // box::status
#include "box/cxx/executor.h"   // box::executor / __exec::wait_domain::result
#include "box/cxx/message.h"    // box::message (decode + box::send / box::broadcast)

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

    // An unbound line: operator bool == false, talks to nobody.
    line() noexcept = default;

    // PEER flavor — send routes to one strand by pid. A null/zero pid stays
    // unbound (send then surfaces invalid_argument, never a silent drop).
    static line to(std::uint32_t peer_pid) noexcept
    {
        line l;
        l.peer_ = peer_pid;
        return l;
    }
    // GROUP flavor — send broadcasts to every strand carrying group_tag. The tag
    // string must outlive the line (it is held by reference, like a tag name).
    static line on(const char *group_tag) noexcept
    {
        line l;
        l.group_ = group_tag;
        return l;
    }

    // Bound iff it names a peer pid or a group tag.
    explicit operator bool() const noexcept { return peer_ != 0 || group_ != nullptr; }
    std::uint32_t peer()  const noexcept { return peer_; }            // 0 for a group line
    const char   *group() const noexcept { return group_; }          // nullptr for a peer line

    // Send one T. Empty status on accepted delivery; the error arm carries the
    // REAL cause — invalid_argument (unbound / bad pid), process_not_found /
    // process_terminated (peer gone), route_target_full (peer ring full), or
    // route_no_subscribers (group with no member carrying the tag).
    status send(const T &v) const noexcept
    {
        if (group_) return box::broadcast(group_, v);
        return box::send(peer_, v);
    }

    // Take the next delivery, decoded as T — NON-blocking. nullopt when the ring
    // is empty, or when the next record is too short / cross-cabin to be a T (the
    // box::message data_addr guard). The record is CONSUMED either way: an
    // undecodable delivery is popped and reported as nullopt, indistinguishable
    // from an empty inbox — use box::receive / box::next_message when you need to
    // inspect a raw record instead of dropping it. Does NOT filter by peer: it
    // reads whatever is next in this strand's shared inbox (filtering would steal a
    // sibling's record).
    std::optional<T> try_recv() const noexcept
    {
        Result r;
        if (!::receive(&r)) return std::nullopt;
        return message(r).payload_as<T>();
    }
    // Like try_recv but blocks up to ms (0 == forever); nullopt on timeout or a
    // short/cross-cabin record.
    std::optional<T> recv_for(std::uint32_t ms) const noexcept
    {
        Result r;
        if (!::receive_wait(&r, ms)) return std::nullopt;
        return message(r).payload_as<T>();
    }

    // ── coroutine-native: co_await line.recv() -> std::optional<T> ───────────
    // Mirrors box::__message_awaiter exactly (wait_domain::result, the _M_got
    // latch), differing only in that await_resume DECODES the delivered record to
    // an optional<T>. Spurious-safe: a stray resume yields nullopt, never a deref.
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

}  // namespace box

#endif  // BOXCXX_BOX_LINE_H
