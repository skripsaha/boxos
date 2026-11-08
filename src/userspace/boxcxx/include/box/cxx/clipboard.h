// boxcxx — box::clipboard  (the machine's list of what was copied)
//
// The idiomatic C++ face of box/clipboard.h. One list, kept by the kernel: a
// program puts bytes and is given a number no other clip will ever have; any
// program lists the clips, newest first, and takes any of them by number. The
// list never reads the bytes and never lets a clip go on its own — a clip
// leaves when it is dropped, when the list is cleared, or when its own terms
// end it. What the bytes are, and which clip to take, is the program's to say.
//
//   box::clipboard::put(bytes)               — a clip; its number
//   box::clipboard::put(secret, {1, true})   — taken once, and gone with this program
//   box::clipboard::list()                   — every clip, newest first
//   box::clipboard::list(1)                  — the newest alone
//   box::clipboard::take(c)                  — clip c's bytes, whole
//   box::clipboard::take(n, offset, into)    — a piece of clip n's bytes
//   box::clipboard::drop(n), clear()
//   box::clipboard::on_change()              — a subscription to clipboard:changed;
//                                              each event's payload_as<ClipboardChange>()
//
// The keys every program says the same way: Ctrl+C and Ctrl+X put, Ctrl+V
// pastes the newest clip, Ctrl+Shift+V one chosen from the list.
//
// This is a box:: extension, not part of std.
#ifndef BOXCXX_BOX_CLIPBOARD_H
#define BOXCXX_BOX_CLIPBOARD_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "box/clipboard.h"
#include "box/cxx/error.h"  // box::result / box::status
#include "box/cxx/touch.h"  // box::subscription

namespace box {

// A clip's terms: how many whole takes it stands before it goes (0: no such
// term — such a clip is taken only whole), and whether it goes when the
// program that put it ends.
struct clip_terms {
    std::uint32_t takes       = 0;
    bool          while_alive = false;
};

// One clip, as the list tells of it.
struct clip {
    std::uint64_t number      = 0;
    std::uint64_t bytes       = 0;
    std::uint64_t put_us      = 0;  // microseconds since boot, as clock_uptime_us() counts them
    std::uint32_t takes_left  = 0;  // 0: no such term
    bool          while_alive = false;
};

struct clipboard {
    // The bytes as a new clip at the head of the list; its number.
    static result<std::uint64_t> put(std::string_view bytes, clip_terms terms = {})
    {
        const ClipTerms said{terms.takes, terms.while_alive};
        const std::int64_t number = ::clipboard_put(bytes.data(), bytes.size(), &said);
        if (number < 0) return std::unexpected(error{box_errno_of(static_cast<int>(number))});
        return static_cast<std::uint64_t>(number);
    }

    // Up to `most` clips (0: every one), newest first, from the newest
    // numbered `from` or below (0: the newest of all).
    static result<std::vector<clip>> list(std::size_t most = 0, std::uint64_t from = 0)
    {
        constexpr std::size_t        page_room = 256;
        std::vector<clip>            clips;
        std::vector<ClipboardRecord> page(page_room);
        do {
            const std::size_t room = most == 0 ? page_room : std::min(page_room, most - clips.size());
            ClipboardPage     head{};
            const int         rc = ::clipboard_list(from, page.data(), room, &head);
            if (rc < 0) return std::unexpected(error{box_errno_of(rc)});
            for (std::uint32_t i = 0; i < head.delivered; i++)
                clips.push_back({page[i].number, page[i].bytes, page[i].put_us, page[i].takes_left,
                                 (page[i].flags & CLIPBOARD_WHILE_ALIVE) != 0});
            from = head.next;
        } while (from != 0 && (most == 0 || clips.size() < most));
        return clips;
    }

    // The clip's bytes, whole. A clip with takes left spends one.
    static result<std::string> take(const clip &which)
    {
        std::string bytes(static_cast<std::size_t>(which.bytes), '\0');
        std::size_t got = 0;
        while (got < bytes.size()) {
            const std::int64_t came = ::clipboard_take(which.number, got, bytes.data() + got, bytes.size() - got);
            if (came < 0) return std::unexpected(error{box_errno_of(static_cast<int>(came))});
            got += static_cast<std::size_t>(came);
        }
        return bytes;
    }

    // A piece of clip `number`'s bytes from `offset` on, as many as `into`
    // holds; how many came.
    static result<std::size_t> take(std::uint64_t number, std::uint64_t offset, std::span<char> into)
    {
        const std::int64_t came = ::clipboard_take(number, offset, into.data(), into.size());
        if (came < 0) return std::unexpected(error{box_errno_of(static_cast<int>(came))});
        return static_cast<std::size_t>(came);
    }

    static status drop(std::uint64_t number)
    {
        const int rc = ::clipboard_drop(number);
        if (rc < 0) return std::unexpected(error{box_errno_of(rc)});
        return {};
    }

    // Every clip out of the list; how many went.
    static result<std::uint64_t> clear()
    {
        const std::int64_t went = ::clipboard_clear();
        if (went < 0) return std::unexpected(error{box_errno_of(static_cast<int>(went))});
        return static_cast<std::uint64_t>(went);
    }

    static subscription on_change() { return subscription(tag(CLIPBOARD_TOUCH_TAG)); }
};

}  // namespace box

#endif  // BOXCXX_BOX_CLIPBOARD_H
