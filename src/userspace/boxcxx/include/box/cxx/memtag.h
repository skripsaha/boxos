#ifndef BOXCXX_BOX_MEMTAG_H
#define BOXCXX_BOX_MEMTAG_H

#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "box/memtag.h"
#include "box/cxx/error.h"
#include "box/cxx/hw.h"

namespace box {
namespace memtag {

namespace __mtdetail {
inline std::vector<std::string> unpack(const char *buf, std::uint32_t count, std::size_t bytes)
{
    std::vector<std::string> v;
    v.reserve(count);
    const char *p = buf, *end = buf + bytes;
    for (std::uint32_t i = 0; i < count && p < end; ++i) {
        std::size_t n = 0;
        while (p + n < end && p[n] != '\0') ++n;
        v.emplace_back(p, n);
        p += n + 1;
    }
    return v;
}
}

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

    std::vector<std::string> tags() const
    {
        if (info_.tag_count == 0) return {};
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

    bool          encrypted() const noexcept { return box::hw::encrypted(); }
    std::uint16_t keyid()     const noexcept { return box::hw::keyid_of(info_.base_phys); }
};

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


inline result<region> find(std::uint32_t region_id)
{
    mem_region_info_t info{};
    int rc = ::mem_region_info(region_id, &info);
    if (rc != 0) return std::unexpected(error{box_errno_of(rc)});
    return region(region_id, info);
}

inline result<region> covering(std::uint64_t phys)
{
    std::uint32_t id = 0;
    ::error_t e = ::mem_region_from_phys_ex(phys, &id);
    if (e != OK) return std::unexpected(error{e});
    if (id == MEMTAG_INVALID_REGION_ID) return std::unexpected(error{errc::object_not_found});
    return find(id);
}

inline std::vector<region> query(std::initializer_list<const char *> required,
                                 std::initializer_list<const char *> any      = {},
                                 std::initializer_list<const char *> excluded = {})
{
    std::vector<const char *> rv, av, ev;
    auto terminate = [](std::initializer_list<const char *> il,
                        std::vector<const char *> &v) -> const char *const * {
        if (il.size() == 0) return nullptr;
        v.assign(il.begin(), il.end());
        v.push_back(nullptr);
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

inline result<stats> counters()
{
    mem_stats_t s{};
    int rc = ::mem_stats(&s);
    if (rc != 0) return std::unexpected(error{box_errno_of(rc)});
    return stats(s);
}


inline status set_guard(const char *tag, bool on)
{
    return tag ? _detail::from_status(::mem_set_guard(tag, on ? 1 : 0))
               : std::unexpected(error{errc::invalid_argument});
}

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

inline std::vector<std::string> cabin_tags(std::uint32_t pid)
{
    std::vector<char> buf(8192);
    std::uint32_t count = 0;
    if (::mem_cabin_tags(pid, buf.data(), static_cast<std::uint32_t>(buf.size()), &count) != 0)
        return {};
    return __mtdetail::unpack(buf.data(), count, buf.size());
}

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

class scoped_grant {
    std::uint32_t pid_{};
    std::string   tag_;
    bool          owned_{false};

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
            release();
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
    bool held() const noexcept { return owned_; }
};

inline result<scoped_grant> grant_scope(std::uint32_t pid, const char *tag)
{
    if (!tag) return std::unexpected(error{errc::invalid_argument});
    for (const std::string &t : cabin_tags(pid))
        if (t == tag) return scoped_grant(pid, std::string(tag), false);
    int rc = ::mem_cabin_grant(pid, tag);
    if (rc != 0) return std::unexpected(error{box_errno_of(rc)});
    return scoped_grant(pid, std::string(tag), true);
}

}
}

template <>
struct std::formatter<box::memtag::region, char> : std::formatter<std::string_view, char> {
    auto format(const box::memtag::region &r, std::format_context &ctx) const
    {
        static constexpr char kFmt[] =
            "region id={} phys={:#x} virt={:#x} pages={} tags={} flags={:#x} gen={}";
        static constexpr std::size_t kCap =
            sizeof kFmt + 7 * (std::numeric_limits<unsigned long long>::digits10 + 1);
        char buf[kCap];
        auto res = std::format_to_n(
            buf, (std::ptrdiff_t)sizeof buf, kFmt,
            r.id(), r.base_phys(), r.base_virt(), r.pages(),
            static_cast<unsigned>(r.tag_count()), static_cast<unsigned>(r.flags()),
            r.generation());
        return std::formatter<std::string_view, char>::format(
            std::string_view(buf, (std::size_t)(res.out - buf)), ctx);
    }
};

template <>
struct std::formatter<box::memtag::stats, char> : std::formatter<std::string_view, char> {
    auto format(const box::memtag::stats &s, std::format_context &ctx) const
    {
        static constexpr char kFmt[] =
            "memtag tags={} active={} slots={} cap={} reg_gen={} hits={} misses={}";
        static constexpr std::size_t kCap =
            sizeof kFmt + 7 * (std::numeric_limits<unsigned long long>::digits10 + 1);
        char buf[kCap];
        auto res = std::format_to_n(
            buf, (std::ptrdiff_t)sizeof buf, kFmt,
            s.tag_count(), s.region_active(), s.region_slot_count(), s.region_slot_cap(),
            s.registry_generation(), s.cache_hits(), s.cache_misses());
        return std::formatter<std::string_view, char>::format(
            std::string_view(buf, (std::size_t)(res.out - buf)), ctx);
    }
};

#endif