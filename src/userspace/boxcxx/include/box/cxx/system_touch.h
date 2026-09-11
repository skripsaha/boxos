#ifndef BOXCXX_BOX_SYSTEM_TOUCH_H
#define BOXCXX_BOX_SYSTEM_TOUCH_H

#include <cstdint>
#include <optional>

#include "box/cxx/touch.h"

namespace box {

using process_died    = ::TouchProcessDied;
using process_spawned = ::TouchProcessSpawned;
using system_halt     = ::TouchSystemHalt;
using usb_device      = ::TouchUsbConnect;

static_assert(sizeof(process_died) == 12,
              "box::process_died must match the kernel's 12-byte process:died emit "
              "(pid@0, exit_code@4, generation@8)");
static_assert(sizeof(process_spawned) == 8 && sizeof(system_halt) == 8 &&
                  sizeof(usb_device) == 8,
              "box system payloads must match the kernel's 8-byte Touch emit");

enum class system_touch_kind {
    process_died,
    process_spawned,
    shutdown,
    reboot,
    usb_connect,
    usb_disconnect,
};

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

    explicit operator bool() const noexcept
    {
        return died_ && spawned_ && shutdown_ && reboot_ && usb_connect_ && usb_disconnect_;
    }

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

    std::optional<system_touch> wait(std::uint32_t ms = 0) noexcept
    {
        if (auto e = poll()) return e;
        constexpr std::uint32_t step = 16;
        std::uint64_t waited = 0;
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

}

#endif