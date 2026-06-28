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
#include <format>          // std::formatter<region> / <stats>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>         // std::move (scoped_grant)
#include <vector>

#include "box/memtag.h"  // mem_query / mem_region_info / mem_region_from_phys /
                        // mem_region_tags / mem_stats / mem_set_guard /
                        // mem_cabin_grant / mem_cabin_revoke / mem_cabin_tags /
                        // mem_check_access + the POD types & constants
#include "box/cxx/error.h"  // box::status / box::result / box::error
#include "box/cxx/hw.h"     // box::hw::encrypted / keyid_of — region encryption delegates (one-way)

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

    // ── TME encryption (Ф25d) — a DERIVED view, not a per-region kernel fact ──
    // There is no kernel "is this region encrypted" call; these delegate to the
    // honest box::hw synthesis over the GLOBAL TME snapshot + this region's
    // physical keyid lane (see box/cxx/hw.h). encrypted() is true iff TME is
    // engaged platform-wide; keyid() is this region's TME-MK key (0 under plain
    // TME / no TME). Each does one hw_tme_state read — introspection, not a hot
    // path.
    bool          encrypted() const noexcept { return box::hw::encrypted(); }
    std::uint16_t keyid()     const noexcept { return box::hw::keyid_of(info_.base_phys); }
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

// One region by id; on success the view, otherwise the recovered cause in the
// error arm (e.g. object_not_found when the id names no live region).
inline result<region> find(std::uint32_t region_id)
{
    mem_region_info_t info{};
    int rc = ::mem_region_info(region_id, &info);
    if (rc != 0) return std::unexpected(error{box_errno_of(rc)});
    return region(region_id, info);
}

// The region covering a physical address. The lossless twin separates the two
// outcomes: a failed lookup surfaces its real cause, while a successful lookup
// that no region covers is object_not_found (not folded into a transport error).
inline result<region> covering(std::uint64_t phys)
{
    std::uint32_t id = 0;
    ::error_t e = ::mem_region_from_phys_ex(phys, &id);
    if (e != OK) return std::unexpected(error{e});
    if (id == MEMTAG_INVALID_REGION_ID) return std::unexpected(error{errc::object_not_found});
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
        if (result<region> r = find(ids[i])) out.push_back(*r);
    return out;
}

// A snapshot of the global MemTag counters; on success the view, otherwise the
// recovered cause in the error arm.
inline result<stats> counters()
{
    mem_stats_t s{};
    int rc = ::mem_stats(&s);
    if (rc != 0) return std::unexpected(error{box_errno_of(rc)});
    return stats(s);
}

// ── capability control ──────────────────────────────────────────────────────
// set_guard/grant/revoke need the TagFS "system" tag-bit; they return false
// (denied) from an unprivileged cabin. Guarding a tag flips every region that
// bears it into enforcement: only cabins granted that tag may then access it.

// Flip a tag into (on) / out of (off) enforcement mode. Empty status on success;
// the error arm carries the cause — invalid_argument on a null tag, or the
// recovered kernel error_t (e.g. access_denied from an unprivileged cabin).
inline status set_guard(const char *tag, bool on)
{
    return tag ? _detail::from_status(::mem_set_guard(tag, on ? 1 : 0))
               : std::unexpected(error{errc::invalid_argument});
}

// Grant / revoke a cabin's access to a guarded tag. Empty status on success; the
// error arm carries the cause (e.g. access_denied without the "system" tag-bit).
inline status grant(std::uint32_t pid, const char *tag)
{
    return tag ? _detail::from_status(::mem_cabin_grant(pid, tag))
               : std::unexpected(error{errc::invalid_argument});
}
inline status revoke(std::uint32_t pid, const char *tag)
{
    return tag ? _detail::from_status(::mem_cabin_revoke(pid, tag))
               : std::unexpected(error{errc::invalid_argument});
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

// ── box::memtag::scoped_grant — a RAII capability lease (Ф25d) ───────────────
// Owns a cabin's grant of one guarded tag: granted at construction (via the
// grant_scope factory), revoked at destruction — but ONLY the grant THIS scope
// made. If the cabin already held the tag, the scope is inert (held()==false)
// and the destructor revokes nothing, so a pre-existing capability is never
// clawed back. Move-only (a copy would double-revoke). There is intentionally
// no scoped_guard twin: mem_set_guard has no read-back, so an honest RAII
// restore of the prior guard state is impossible (deferred — see Ф25 notes).
class scoped_grant {
    std::uint32_t pid_{};
    std::string   tag_;
    bool          owned_{false};   // true iff THIS scope performed the grant

    scoped_grant(std::uint32_t pid, std::string tag, bool owned) noexcept
        : pid_(pid), tag_(std::move(tag)), owned_(owned) {}
    friend result<scoped_grant> grant_scope(std::uint32_t, const char *);

    void release() noexcept
    {
        if (owned_) { ::mem_cabin_revoke(pid_, tag_.c_str()); owned_ = false; }
    }

public:
    scoped_grant(scoped_grant &&o) noexcept
        : pid_(o.pid_), tag_(std::move(o.tag_)), owned_(o.owned_) { o.owned_ = false; }
    scoped_grant &operator=(scoped_grant &&o) noexcept
    {
        if (this != &o) {
            release();                 // give up our current lease before stealing o's
            pid_     = o.pid_;
            tag_     = std::move(o.tag_);
            owned_   = o.owned_;
            o.owned_ = false;
        }
        return *this;
    }
    scoped_grant(const scoped_grant &)            = delete;
    scoped_grant &operator=(const scoped_grant &) = delete;
    ~scoped_grant() { release(); }

    std::uint32_t    pid() const noexcept { return pid_; }
    std::string_view tag() const noexcept { return tag_; }
    // True iff THIS scope granted the tag (false if the cabin already held it,
    // in which case the destructor is a no-op).
    bool held() const noexcept { return owned_; }
};

// Lease a grant of `tag` to cabin `pid` for the returned scope's lifetime. If the
// cabin already holds the tag, the lease is inert (held()==false) and revokes
// nothing on destruction. The error arm carries the cause — invalid_argument on
// a null tag, or the kernel cause (e.g. access_denied without the "system"
// tag-bit, like grant()).
inline result<scoped_grant> grant_scope(std::uint32_t pid, const char *tag)
{
    if (!tag) return std::unexpected(error{errc::invalid_argument});
    // Don't re-grant (and later wrongly revoke) a capability the cabin already
    // holds — a pre-existing grant must survive this scope untouched.
    for (const std::string &t : cabin_tags(pid))
        if (t == tag) return scoped_grant(pid, std::string(tag), false);
    int rc = ::mem_cabin_grant(pid, tag);
    if (rc != 0) return std::unexpected(error{box_errno_of(rc)});
    return scoped_grant(pid, std::string(tag), true);
}

}  // namespace memtag
}  // namespace box

// ── std::formatter<box::memtag::region> / <stats> — one greppable line, no spec
// Pure POD reads — region prints tag_count() (the POD field), NEVER the tags()
// syscall, so formatting never re-enters the kernel or takes a lock.
template <>
struct std::formatter<box::memtag::region, char> {
    constexpr auto parse(std::format_parse_context &ctx) { return ctx.begin(); }
    auto format(const box::memtag::region &r, std::format_context &ctx) const
    {
        return std::format_to(
            ctx.out(),
            "region id={} phys={:#x} virt={:#x} pages={} tags={} flags={:#x} gen={}",
            r.id(), r.base_phys(), r.base_virt(), r.pages(),
            static_cast<unsigned>(r.tag_count()), static_cast<unsigned>(r.flags()),
            r.generation());
    }
};

template <>
struct std::formatter<box::memtag::stats, char> {
    constexpr auto parse(std::format_parse_context &ctx) { return ctx.begin(); }
    auto format(const box::memtag::stats &s, std::format_context &ctx) const
    {
        return std::format_to(
            ctx.out(),
            "memtag tags={} active={} slots={} cap={} reg_gen={} hits={} misses={}",
            s.tag_count(), s.region_active(), s.region_slot_count(), s.region_slot_cap(),
            s.registry_generation(), s.cache_hits(), s.cache_misses());
    }
};

#endif  // BOXCXX_BOX_MEMTAG_H
