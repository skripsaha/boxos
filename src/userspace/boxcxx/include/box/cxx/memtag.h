// boxcxx — box::memtag  (tagged-RAM introspection + cabin capability control)
//
// The idiomatic C++ surface over the boxlib MemTag API (box/memtag.h). The
// kernel runs a TagFS-shaped registry over RAM areas: each region carries a
// sorted "key:value" tag set, and cabins gain/lose access to guarded tags.
//
//   box::memtag::region  — a view of one tagged region (id + descriptor +
//                          its tag strings).
//   box::memtag::stats   — a snapshot of the global registry counters.
//   box::memtag           — the verbs: query() / find() / covering() /
//                          counters() (introspection, unprivileged) and
//                          set_guard() / grant() / revoke() / cabin_tags() /
//                          check_access() (capability control).
//
// This is a box:: extension, not part of std. Introspection is read-only and
// safe anywhere. set_guard()/grant()/revoke() require the caller to hold the
// TagFS "system" tag-bit — they return false from an unprivileged cabin
// (cabin_tags()/check_access() are unprivileged). The default registry state
// is permissive: with no guard set, check_access() is true for every region.
#ifndef BOXCXX_BOX_MEMTAG_H
#define BOXCXX_BOX_MEMTAG_H

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "box/memtag.h"  // mem_query / mem_region_info / mem_region_from_phys /
                        // mem_region_tags / mem_stats / mem_set_guard /
                        // mem_cabin_grant / mem_cabin_revoke / mem_cabin_tags /
                        // mem_check_access + the POD types & constants

namespace box {
namespace memtag {

namespace __mtdetail {
// Unpack `count` NUL-separated strings packed into [buf, buf+bytes). The end
// bound makes a short / malformed buffer safe (never reads past it).
inline std::vector<std::string> unpack(const char *buf, std::uint32_t count, std::size_t bytes)
{
    std::vector<std::string> v;
    v.reserve(count);
    const char *p = buf, *end = buf + bytes;
    for (std::uint32_t i = 0; i < count && p < end; ++i) {
        std::size_t n = 0;
        while (p + n < end && p[n] != '\0') ++n;
        v.emplace_back(p, n);
        p += n + 1;  // step over the NUL
    }
    return v;
}
}  // namespace __mtdetail

// ── box::memtag::region — a view of one tagged RAM region ───────────────────
class region {
    std::uint32_t     id_{MEMTAG_INVALID_REGION_ID};
    mem_region_info_t info_{};

public:
    region(std::uint32_t id, const mem_region_info_t &info) noexcept : id_(id), info_(info) {}

    std::uint32_t            id() const noexcept { return id_; }
    const mem_region_info_t &raw() const noexcept { return info_; }
    std::uint64_t            base_phys() const noexcept { return info_.base_phys; }
    std::uint64_t            base_virt() const noexcept { return info_.base_virt; }
    std::uint64_t            pages() const noexcept { return info_.pages; }
    std::uint64_t            size_bytes() const noexcept { return info_.pages * 4096ull; }
    std::uint16_t            tag_count() const noexcept { return info_.tag_count; }
    std::uint16_t            flags() const noexcept { return info_.flags; }
    std::uint32_t            generation() const noexcept { return info_.generation; }

    // The region's "key:value" tag strings (empty when it carries none, or on
    // a failed read). The buffer is sized from the known tag_count, so it is
    // never silently truncated.
    std::vector<std::string> tags() const
    {
        if (info_.tag_count == 0) return {};
        // 256 B/tag is generous for "key:value" strings; clamp to the kernel's
        // per-call ceiling (the SYSTEM_OP_MEMTAG_TAGS handler rejects a larger
        // out buffer), so a high tag_count can't push the request past it.
        std::size_t cap = sizeof(std::uint32_t) + (static_cast<std::size_t>(info_.tag_count) + 1) * 256;
        if (cap > 16384) cap = 16384;
        std::vector<char> buf(cap);
        std::uint32_t count = 0;
        if (::mem_region_tags(id_, buf.data(), static_cast<std::uint32_t>(buf.size()), &count) != 0)
            return {};
        return __mtdetail::unpack(buf.data(), count, buf.size());
    }

    bool has_tag(std::string_view tag) const
    {
        for (const std::string &t : tags())
            if (t == tag) return true;
        return false;
    }
};

// ── box::memtag::stats — a snapshot of the global registry counters ─────────
class stats {
    mem_stats_t s_{};

public:
    stats() noexcept = default;
    explicit stats(const mem_stats_t &s) noexcept : s_(s) {}
    const mem_stats_t &raw() const noexcept { return s_; }

    std::uint32_t tag_count() const noexcept { return s_.tag_count; }
    std::uint32_t region_active() const noexcept { return s_.region_active; }
    std::uint32_t region_slot_count() const noexcept { return s_.region_slot_count; }
    std::uint32_t region_slot_cap() const noexcept { return s_.region_slot_cap; }
    std::uint64_t registry_generation() const noexcept { return s_.registry_generation; }
    std::uint64_t region_generation() const noexcept { return s_.region_generation; }
    std::uint64_t bitmap_generation() const noexcept { return s_.bitmap_generation; }
    std::uint64_t cache_hits() const noexcept { return s_.cache_hits; }
    std::uint64_t cache_misses() const noexcept { return s_.cache_misses; }
};

// ── introspection (unprivileged) ────────────────────────────────────────────

// One region by id; nullopt if the id names no live region.
inline std::optional<region> find(std::uint32_t region_id)
{
    mem_region_info_t info{};
    if (::mem_region_info(region_id, &info) != 0) return std::nullopt;
    return region(region_id, info);
}

// The region covering a physical address; nullopt if none does.
inline std::optional<region> covering(std::uint64_t phys)
{
    std::uint32_t id = ::mem_region_from_phys(phys);
    if (id == MEMTAG_INVALID_REGION_ID) return std::nullopt;
    return find(id);
}

// Regions whose tag set satisfies (required ∧ any ∧ ¬excluded). Each argument
// is a brace list of "key:value" C-strings; an empty list skips that section.
// e.g. query({"cache:wb"}) / query({"zone:0"}, {}, {"reserved:true"}).
inline std::vector<region> query(std::initializer_list<const char *> required,
                                 std::initializer_list<const char *> any      = {},
                                 std::initializer_list<const char *> excluded = {})
{
    std::vector<const char *> rv, av, ev;
    auto terminate = [](std::initializer_list<const char *> il,
                        std::vector<const char *> &v) -> const char *const * {
        if (il.size() == 0) return nullptr;
        v.assign(il.begin(), il.end());
        v.push_back(nullptr);  // the C API expects a NULL-terminated list
        return v.data();
    };
    const char *const *rp = terminate(required, rv);
    const char *const *ap = terminate(any, av);
    const char *const *ep = terminate(excluded, ev);

    std::uint32_t ids[MEMTAG_MAX_QUERY_RESULTS];
    int n = ::mem_query(rp, ap, ep, ids, MEMTAG_MAX_QUERY_RESULTS);
    std::vector<region> out;
    if (n <= 0) return out;
    out.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i)
        if (std::optional<region> r = find(ids[i])) out.push_back(*r);
    return out;
}

// A snapshot of the global MemTag counters; nullopt on a failed call.
inline std::optional<stats> counters()
{
    mem_stats_t s{};
    if (::mem_stats(&s) != 0) return std::nullopt;
    return stats(s);
}

// ── capability control ──────────────────────────────────────────────────────
// set_guard/grant/revoke need the TagFS "system" tag-bit; they return false
// (denied) from an unprivileged cabin. Guarding a tag flips every region that
// bears it into enforcement: only cabins granted that tag may then access it.

// Flip a tag into (on) / out of (off) enforcement mode.
inline bool set_guard(const char *tag, bool on)
{
    return tag && ::mem_set_guard(tag, on ? 1 : 0) == 0;
}

// Grant / revoke a cabin's access to a guarded tag.
inline bool grant(std::uint32_t pid, const char *tag)
{
    return tag && ::mem_cabin_grant(pid, tag) == 0;
}
inline bool revoke(std::uint32_t pid, const char *tag)
{
    return tag && ::mem_cabin_revoke(pid, tag) == 0;
}

// The "key:value" capability tags granted to a cabin (unprivileged read).
inline std::vector<std::string> cabin_tags(std::uint32_t pid)
{
    std::vector<char> buf(8192);  // a cabin holds few grant-tags
    std::uint32_t count = 0;
    if (::mem_cabin_tags(pid, buf.data(), static_cast<std::uint32_t>(buf.size()), &count) != 0)
        return {};
    return __mtdetail::unpack(buf.data(), count, buf.size());
}

// Whether `pid` may access a region under the current guard policy (true in
// the default-permissive state where no guard applies). Unprivileged.
inline bool check_access(std::uint32_t pid, std::uint32_t region_id)
{
    mem_check_t c{};
    if (::mem_check_access(pid, region_id, &c) != 0) return false;
    return c.allowed != 0;
}
inline bool check_access(std::uint32_t pid, const region &r)
{
    return check_access(pid, r.id());
}

}  // namespace memtag
}  // namespace box

#endif  // BOXCXX_BOX_MEMTAG_H
