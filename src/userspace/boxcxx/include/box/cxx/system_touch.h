// boxcxx — box::system_touch / box::system_watch  (the typed system lifecycle)
//
// The kernel narrates the whole vessel's life as Touch events on six well-known
// tags: a process born or dying, a power-state request, a USB port change. This
// header is the typed C++ face of those six — the sibling of box::strand_watch
// (which watches the crew, the strands) one deck up: box::system_watch is the
// bridge officer on watch over the SHIP.
//
//   box::process_died / process_spawned / system_halt / usb_device
//                       — the four kernel payload shapes (aliases of the C
//                         structs in box/touch.h; sized to the 8-byte emit).
//   box::system_touch   — one decoded event: a kind, the publisher pid, and the
//                         matching payload in the union; pid() is the subject.
//   box::system_watch   — a RAII multiplexer holding one box::subscription per
//                         system tag. poll() drains them in a fixed order;
//                         wait(ms) round-robins a bounded per-tag slice.
//
//   box::system_watch w;
//   if (auto e = w.poll())                          // non-blocking
//       if (e->kind == box::system_touch_kind::process_spawned)
//           launched(e->spawned.pid, e->spawned.parent_pid);
//   auto e = w.wait(500);                            // block, round-robin, <=~500ms
//
// This is a box:: extension, not std. There is no kernel "wait on N tags"
// primitive, so wait() fans in by round-robining a real event-driven kernel wait
// per tag — only the 6-way rotation is userspace. A drained watch (wait returns
// nullopt) is a CHOSEN timeout, never a Unix EOF.
//
// No cabin/pid filter: the six tags share no common key field (spawned keys on
// parent_pid, died on pid, halt/usb on neither), so a single filter would be a
// per-kind footgun — match on kind()/pid() in your own switch. A watch is move-
// only and owned by ONE strand (its claims deliver only to that strand's ring);
// give each strand its own watch rather than sharing one across strands. Claim
// the watch BEFORE the event you want to observe — a claim made after the
// publish does not see it (mirrors box::strand_watch).
#ifndef BOXCXX_BOX_SYSTEM_TOUCH_H
#define BOXCXX_BOX_SYSTEM_TOUCH_H

#include <cstdint>
#include <optional>

#include "box/cxx/touch.h"  // box::tag / box::tags / box::touch / box::subscription

namespace box {

// ── the four kernel payload shapes ──────────────────────────────────────────
// Aliases, not redefinitions: the packed structs already live in box/touch.h
// (single source of truth, kept in lock-step with the kernel's TouchPublish
// call sites). usb_device serves BOTH connect and disconnect (one C struct).
using process_died    = ::TouchProcessDied;     // { uint32_t pid;   int32_t  exit_code }
using process_spawned = ::TouchProcessSpawned;  // { uint32_t pid;   uint32_t parent_pid }
using system_halt     = ::TouchSystemHalt;      // { uint32_t reason; uint32_t grace_ms }
using usb_device      = ::TouchUsbConnect;      // { u8 port, speed; u16 vendor_id, product_id }

static_assert(sizeof(process_died) == 8 && sizeof(process_spawned) == 8 &&
                  sizeof(system_halt) == 8 && sizeof(usb_device) == 8,
              "box system payloads must match the kernel's 8-byte Touch emit");

// ── box::system_touch — one decoded system lifecycle event ──────────────────
enum class system_touch_kind {
    process_died,
    process_spawned,
    shutdown,
    reboot,
    usb_connect,
    usb_disconnect,
};

// `source` is the publisher pid (0 for kernel-originated events). The active
// union member is selected by `kind`; pid() returns the subject pid (the process
// born/died), or 0 for halt/usb which name no process.
struct system_touch {
    system_touch_kind kind;
    std::uint32_t     source;
    union {
        process_died    died;
        process_spawned spawned;
        system_halt     halt;
        usb_device      usb;
    };

    std::uint32_t pid() const noexcept
    {
        switch (kind) {
        case system_touch_kind::process_died:    return died.pid;
        case system_touch_kind::process_spawned: return spawned.pid;
        default:                                 return 0;
        }
    }
};

// ── box::system_watch — observe the system-wide lifecycle (6 tags) ──────────
class system_watch {
public:
    system_watch() noexcept
        : died_(tags::process_died()),
          spawned_(tags::process_spawned()),
          shutdown_(tags::system_shutdown()),
          reboot_(tags::system_reboot()),
          usb_connect_(tags::usb_connect()),
          usb_disconnect_(tags::usb_disconnect())
    {
    }

    // True iff all six tags were claimed.
    explicit operator bool() const noexcept
    {
        return died_ && spawned_ && shutdown_ && reboot_ && usb_connect_ && usb_disconnect_;
    }

    // Non-blocking: the next system event from any tag, in a fixed deterministic
    // order (died, spawned, shutdown, reboot, usb_connect, usb_disconnect).
    std::optional<system_touch> poll() noexcept
    {
        if (auto e = poll_one(died_, system_touch_kind::process_died)) return e;
        if (auto e = poll_one(spawned_, system_touch_kind::process_spawned)) return e;
        if (auto e = poll_one(shutdown_, system_touch_kind::shutdown)) return e;
        if (auto e = poll_one(reboot_, system_touch_kind::reboot)) return e;
        if (auto e = poll_one(usb_connect_, system_touch_kind::usb_connect)) return e;
        if (auto e = poll_one(usb_disconnect_, system_touch_kind::usb_disconnect)) return e;
        return std::nullopt;
    }

    // Blocking: poll first, then round-robin a short bounded wait across the six
    // tags until one fires or ms elapses (ms == 0 blocks forever). Worst-case
    // fan-in latency is 6 * step while five tags stay silent — system events are
    // rare; for tight single-kind latency, watch that one tag with box::
    // subscription instead. May return slightly after ms (thin helper, not a
    // precise multiplex).
    std::optional<system_touch> wait(std::uint32_t ms = 0) noexcept
    {
        if (auto e = poll()) return e;
        constexpr std::uint32_t step = 16;  // per-tag round-robin slice
        std::uint64_t waited = 0;  // 64-bit: `waited += slice*6` never wraps, even for a huge ms
        for (;;) {
            std::uint32_t slice = (ms == 0) ? step
                                            : ((ms - waited < step) ? (ms - waited) : step);
            if (slice == 0) slice = 1;
            if (auto e = wait_one(died_, system_touch_kind::process_died, slice)) return e;
            if (auto e = wait_one(spawned_, system_touch_kind::process_spawned, slice)) return e;
            if (auto e = wait_one(shutdown_, system_touch_kind::shutdown, slice)) return e;
            if (auto e = wait_one(reboot_, system_touch_kind::reboot, slice)) return e;
            if (auto e = wait_one(usb_connect_, system_touch_kind::usb_connect, slice)) return e;
            if (auto e = wait_one(usb_disconnect_, system_touch_kind::usb_disconnect, slice)) return e;
            if (ms != 0) {
                waited += slice * 6;
                if (waited >= ms) return std::nullopt;
            }
        }
    }

private:
    // Decode a delivered Touch into the typed record for `kind`. nullopt if the
    // payload is shorter than its struct (payload_as rejects a wider T). shutdown
    // and reboot share system_halt; usb_connect and usb_disconnect share usb_device.
    static std::optional<system_touch> decode(system_touch_kind kind, const touch &ev) noexcept
    {
        system_touch e{kind, ev.source(), {}};
        switch (kind) {
        case system_touch_kind::process_died:
            if (auto p = ev.payload_as<process_died>()) { e.died = *p; return e; }
            return std::nullopt;
        case system_touch_kind::process_spawned:
            if (auto p = ev.payload_as<process_spawned>()) { e.spawned = *p; return e; }
            return std::nullopt;
        case system_touch_kind::shutdown:
        case system_touch_kind::reboot:
            if (auto p = ev.payload_as<system_halt>()) { e.halt = *p; return e; }
            return std::nullopt;
        case system_touch_kind::usb_connect:
        case system_touch_kind::usb_disconnect:
            if (auto p = ev.payload_as<usb_device>()) { e.usb = *p; return e; }
            return std::nullopt;
        }
        return std::nullopt;
    }

    std::optional<system_touch> poll_one(subscription &sub, system_touch_kind kind) noexcept
    {
        if (auto ev = sub.poll()) return decode(kind, *ev);
        return std::nullopt;
    }
    std::optional<system_touch> wait_one(subscription &sub, system_touch_kind kind,
                                         std::uint32_t slice) noexcept
    {
        if (auto ev = sub.wait(slice)) return decode(kind, *ev);
        return std::nullopt;
    }

    subscription died_;
    subscription spawned_;
    subscription shutdown_;
    subscription reboot_;
    subscription usb_connect_;
    subscription usb_disconnect_;
};

}  // namespace box

#endif  // BOXCXX_BOX_SYSTEM_TOUCH_H
