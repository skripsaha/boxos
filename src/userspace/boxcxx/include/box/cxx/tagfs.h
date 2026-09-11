#ifndef BOXCXX_BOX_TAGFS_H
#define BOXCXX_BOX_TAGFS_H

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "box/file.h"
#include "box/cxx/current.h"
#include "box/cxx/error.h"
#include "box/cxx/touch.h"
#include "box/memory.h"

namespace box {

class ferry;

namespace tagfs {

inline std::string_view field_view(const char *p, std::size_t cap) noexcept
{
    std::size_t n = 0;
    while (n < cap && p[n] != '\0') ++n;
    return std::string_view(p, n);
}

struct tag {
    std::string key;
    std::string value;
    bool        system = false;

    template <class T>
    box::result<T> as() const
    {
        static_assert(std::is_integral_v<T>, "tag::as<T>: integer or bool only");
        if constexpr (std::is_same_v<T, bool>) {
            if (value == "1") return true;
            if (value == "0") return false;
            return std::unexpected(box::error{box::errc::invalid_argument});
        } else {
            T           out{};
            const char *b = value.data(), *e = b + value.size();
            auto [p, ec] = std::from_chars(b, e, out);
            if (ec == std::errc{} && p == e) return out;
            if (ec == std::errc::result_out_of_range)
                return std::unexpected(box::error{box::errc::out_of_range});
            return std::unexpected(box::error{box::errc::invalid_argument});
        }
    }
};

namespace _detail {
inline box::result<tag> tag_from_info(const file_info_t &i, const char *key)
{
    unsigned n = i.tag_count;
    if (n > 5) n = 5;
    for (unsigned k = 0; k < n; ++k)
        if (field_view(i.tags[k].key, sizeof(i.tags[k].key)) == key)
            return tag{std::string(field_view(i.tags[k].key, sizeof(i.tags[k].key))),
                       std::string(field_view(i.tags[k].value, sizeof(i.tags[k].value))),
                       i.tags[k].type == 1};
    return std::unexpected(box::error{box::errc::tag_not_found});
}
}

class file {
    std::uint32_t id_ = 0;

public:
    file() noexcept = default;
    explicit file(std::uint32_t id) noexcept : id_(id) {}

    std::uint32_t id() const noexcept { return id_; }
    explicit operator bool() const noexcept { return id_ != 0; }

    box::result<file_info_t> info() const
    {
        if (!id_) return std::unexpected(box::error{box::errc::invalid_argument});
        file_info_t inf{};
        int rc = ::file_info(id_, &inf);
        if (rc != 0) return std::unexpected(box::error{box_errno_of(rc)});
        return inf;
    }

    std::string name() const
    {
        auto i = info();
        return i ? std::string(field_view(i->filename, sizeof(i->filename))) : std::string{};
    }
    std::uint64_t size()  const { auto i = info(); return i ? i->size  : 0u; }
    std::uint32_t flags() const { auto i = info(); return i ? i->flags : 0u; }
    bool trashed() const { return (flags() & FILE_FLAG_TRASHED) != 0u; }

    std::vector<tag> tags() const
    {
        std::vector<tag> out;
        auto i = info();
        if (!i) return out;
        unsigned n = i->tag_count;
        if (n > 5) n = 5;
        out.reserve(n);
        for (unsigned k = 0; k < n; ++k) {
            const tag_t &t = i->tags[k];
            out.push_back(tag{std::string(field_view(t.key, sizeof(t.key))),
                              std::string(field_view(t.value, sizeof(t.value))),
                              t.type == 1});
        }
        return out;
    }

    bool has_tag(const char *key) const
    {
        auto i = info();
        if (!i) return false;
        unsigned n = i->tag_count;
        if (n > 5) n = 5;
        for (unsigned k = 0; k < n; ++k)
            if (field_view(i->tags[k].key, sizeof(i->tags[k].key)) == key) return true;
        return false;
    }

    box::result<tag> tag_named(const char *key) const
    {
        auto i = info();
        if (!i) return std::unexpected(i.error());
        return _detail::tag_from_info(*i, key);
    }

    box::status add_tag(const char *tag)
    {
        return id_ ? box::_detail::from_status(::tag_add(id_, tag))
                   : std::unexpected(box::error{box::errc::invalid_argument});
    }
    template <class T>
    box::status add_tag(const char *key, T v)
    {
        static_assert(std::is_integral_v<T>,
                      "add_tag<T>: integer or bool only — for a string value use add_tag(\"key:value\")");
        char        buf[12];
        const char *val;
        int         vlen;
        if constexpr (std::is_same_v<T, bool>) {
            val  = v ? "1" : "0";
            vlen = 1;
        } else {
            auto [p, ec] = std::to_chars(buf, buf + sizeof(buf), v);
            if (ec != std::errc{}) return std::unexpected(box::error{box::errc::buffer_too_small});
            vlen = int(p - buf);
            val  = buf;
        }
        if (vlen > 11) return std::unexpected(box::error{box::errc::buffer_too_small});
        std::string spec(key);
        spec.push_back(':');
        spec.append(val, static_cast<std::size_t>(vlen));
        return add_tag(spec.c_str());
    }
    box::status remove_tag(const char *key)
    {
        return id_ ? box::_detail::from_status(::tag_remove(id_, key))
                   : std::unexpected(box::error{box::errc::invalid_argument});
    }
    box::status rename(const char *new_name)
    {
        return id_ ? box::_detail::from_status(::file_rename(id_, new_name))
                   : std::unexpected(box::error{box::errc::invalid_argument});
    }
    box::status remove()
    {
        return id_ ? box::_detail::from_status(::file_delete(id_))
                   : std::unexpected(box::error{box::errc::invalid_argument});
    }
    box::status anchor()
    {
        return id_ ? box::_detail::from_status(::anchor(id_))
                   : std::unexpected(box::error{box::errc::invalid_argument});
    }
    box::status truncate(std::uint64_t n)
    {
        return id_ ? box::_detail::from_status(::file_truncate(id_, n))
                   : std::unexpected(box::error{box::errc::invalid_argument});
    }

    box::result<std::size_t> read_at(std::uint64_t offset, void *p, std::size_t n) const
    {
        return box::_detail::from_ret64<std::size_t>(
            id_ ? ::fread(id_, offset, p, n) : -ERR_INVALID_ARGUMENT);
    }
    box::result<std::size_t> write_at(std::uint64_t offset, const void *p, std::size_t n)
    {
        return box::_detail::from_ret64<std::size_t>(
            id_ ? ::fwrite(id_, offset, p, n) : -ERR_INVALID_ARGUMENT);
    }
    box::result<std::size_t> read_at(std::uint64_t offset, std::span<std::byte> buf) const
    {
        return read_at(offset, buf.data(), buf.size());
    }
    box::result<std::size_t> write_at(std::uint64_t offset, std::span<const std::byte> buf)
    {
        return write_at(offset, buf.data(), buf.size());
    }

    box::ferry read_async(std::uint64_t offset, void *p, std::size_t n) const;
    box::ferry write_async(std::uint64_t offset, const void *p, std::size_t n) const;
    box::ferry read_async(std::uint64_t offset, std::span<std::byte> buf) const;
    box::ferry write_async(std::uint64_t offset, std::span<const std::byte> buf) const;

    template <class T>
    box::result<T> read_object(std::uint64_t offset) const
    {
        static_assert(std::is_trivially_copyable_v<T>,
                      "read_object<T>: T must be trivially copyable");
        T v;
        auto r = read_at(offset, &v, sizeof(T));
        if (!r) return std::unexpected(r.error());
        if (*r != sizeof(T)) return std::unexpected(box::error{box::errc::io});
        return v;
    }
    template <class T>
    box::status write_object(std::uint64_t offset, const T &v)
    {
        static_assert(std::is_trivially_copyable_v<T>,
                      "write_object<T>: T must be trivially copyable");
        auto r = write_at(offset, &v, sizeof(T));
        if (!r) return std::unexpected(r.error());
        if (*r != sizeof(T)) return std::unexpected(box::error{box::errc::io});
        return {};
    }

    box::byte_current bytes(box::role r) const
    {
        auto i = info();
        if (!i) return {};
        std::string nm(field_view(i->filename, sizeof(i->filename)));
        box::opening fl = (r == box::role::write) ? box::opening::create : box::opening::none;
        return box::file(nm.c_str(), r, fl);
    }
};

class record {
    std::uint32_t id_ = 0;
    file_info_t   info_{};

public:
    record() noexcept = default;
    record(std::uint32_t id, const file_info_t &i) noexcept : id_(id), info_(i) {}

    std::uint32_t id() const noexcept { return id_; }
    explicit operator bool() const noexcept { return id_ != 0; }

    file live() const noexcept { return file(id_); }

    box::status reread()
    {
        if (!id_) return std::unexpected(box::error{box::errc::invalid_argument});
        file_info_t ni{};
        int         rc = ::file_info(id_, &ni);
        if (rc != 0) return std::unexpected(box::error{box_errno_of(rc)});
        info_ = ni;
        return {};
    }

    const file_info_t &raw() const noexcept { return info_; }

    std::string name() const { return std::string(field_view(info_.filename, sizeof(info_.filename))); }
    std::uint64_t    size() const noexcept { return info_.size; }
    std::uint32_t    flags() const noexcept { return info_.flags; }
    bool             trashed() const noexcept { return (info_.flags & FILE_FLAG_TRASHED) != 0u; }
    std::vector<tag> tags() const;
    bool             has_tag(const char *key) const noexcept;
    box::result<tag> tag_named(const char *key) const;
};
static_assert(std::is_trivially_copyable_v<record>,
              "box::tagfs::record stays a trivially-copyable snapshot");

inline std::vector<tag> record::tags() const
{
    std::vector<tag> out;
    unsigned         n = info_.tag_count;
    if (n > 5) n = 5;
    out.reserve(n);
    for (unsigned k = 0; k < n; ++k) {
        const tag_t &t = info_.tags[k];
        out.push_back(tag{std::string(field_view(t.key, sizeof(t.key))),
                          std::string(field_view(t.value, sizeof(t.value))),
                          t.type == 1});
    }
    return out;
}
inline bool record::has_tag(const char *key) const noexcept
{
    unsigned n = info_.tag_count;
    if (n > 5) n = 5;
    for (unsigned k = 0; k < n; ++k)
        if (field_view(info_.tags[k].key, sizeof(info_.tags[k].key)) == key) return true;
    return false;
}
inline box::result<tag> record::tag_named(const char *key) const
{
    return _detail::tag_from_info(info_, key);
}

inline box::result<file> create(const char *name, std::initializer_list<const char *> tags)
{
    std::string spec;
    for (const char *t : tags) {
        if (!spec.empty()) spec.push_back(',');
        spec += t;
    }
    int id = ::create(name, spec.c_str());
    if (id > 0) return file(static_cast<std::uint32_t>(id));
    return std::unexpected(
        box::error{id < 0 ? box_errno_of(id) : static_cast<::error_t>(ERR_INTERNAL)});
}
inline box::result<file> create(const char *name, const char *tagspec = "")
{
    int id = ::create(name, tagspec);
    if (id > 0) return file(static_cast<std::uint32_t>(id));
    return std::unexpected(
        box::error{id < 0 ? box_errno_of(id) : static_cast<::error_t>(ERR_INTERNAL)});
}

namespace _detail {
inline std::vector<file> files_of(int n, std::uint32_t *ids)
{
    std::vector<file> out;
    if (n > 0) {
        out.reserve(static_cast<std::size_t>(n));
        for (int i = 0; i < n; ++i) out.emplace_back(ids[i]);
    }
    ::free(ids);
    return out;
}
}
inline std::vector<file> query(const char *tagspec)
{
    std::uint32_t *ids = nullptr;
    int            n   = ::query_all(tagspec, &ids);
    return _detail::files_of(n, ids);
}
inline std::vector<file> all() { return query(nullptr); }

inline std::vector<file> query_everywhere(const char *tagspec)
{
    std::uint32_t *ids = nullptr;
    int            n   = ::query_all_everywhere(tagspec, &ids);
    return _detail::files_of(n, ids);
}
inline std::vector<file> all_everywhere() { return query_everywhere(nullptr); }

inline box::result<file> create_everywhere(const char *name, std::initializer_list<const char *> tags)
{
    std::string spec;
    for (const char *t : tags) {
        if (!spec.empty()) spec.push_back(',');
        spec += t;
    }
    int id = ::create_everywhere(name, spec.c_str());
    if (id > 0) return file(static_cast<std::uint32_t>(id));
    return std::unexpected(
        box::error{id < 0 ? box_errno_of(id) : static_cast<::error_t>(ERR_INTERNAL)});
}
inline box::result<file> create_everywhere(const char *name, const char *tagspec = "")
{
    int id = ::create_everywhere(name, tagspec);
    if (id > 0) return file(static_cast<std::uint32_t>(id));
    return std::unexpected(
        box::error{id < 0 ? box_errno_of(id) : static_cast<::error_t>(ERR_INTERNAL)});
}

inline box::result<record> find(const char *name)
{
    std::uint32_t ids[1];
    file_info_t   infos[1];
    int           n = ::find_file_by_name(name, ids, infos, 1);
    if (n > 0) return record(ids[0], infos[0]);
    if (n < 0) return std::unexpected(box::error{box_errno_of(n)});
    return std::unexpected(box::error{box::errc::file_not_found});
}
inline std::vector<record> find_all(const char *name)
{
    std::vector<record> out;
    int n = ::find_file_by_name(name, nullptr, nullptr, 0);
    while (n > 0) {
        std::vector<std::uint32_t> ids(static_cast<std::size_t>(n));
        std::vector<file_info_t>   infos(static_cast<std::size_t>(n));
        int got = ::find_file_by_name(name, ids.data(), infos.data(), ids.size());
        if (got <= n) {
            for (int i = 0; i < got; ++i) out.emplace_back(ids[i], infos[i]);
            break;
        }
        n = got;
    }
    return out;
}

class snapshot {
    std::uint32_t id_    = 0;
    bool          owned_ = false;
    std::string   name_;

    snapshot(std::uint32_t id, const char *name) : id_(id), owned_(true), name_(name) {}

public:
    snapshot() noexcept = default;
    snapshot(const snapshot &)            = delete;
    snapshot &operator=(const snapshot &) = delete;
    snapshot(snapshot &&o) noexcept
        : id_(o.id_), owned_(o.owned_), name_(std::move(o.name_))
    {
        o.id_ = 0;
        o.owned_ = false;
    }
    snapshot &operator=(snapshot &&o) noexcept
    {
        if (this != &o) {
            if (owned_) ::snap_delete(id_);
            id_      = o.id_;
            owned_   = o.owned_;
            name_    = std::move(o.name_);
            o.id_    = 0;
            o.owned_ = false;
        }
        return *this;
    }
    ~snapshot() { if (owned_) ::snap_delete(id_); }

    static box::result<snapshot> of(std::uint32_t file_id, const char *name)
    {
        if (!name) return std::unexpected(box::error{box::errc::invalid_argument});
        std::uint32_t sid = 0;
        int rc = ::snap_create(name, file_id, &sid);
        if (rc != 0) return std::unexpected(box::error{box_errno_of(rc)});
        return snapshot(sid, name);
    }
    static box::result<snapshot> of(const file &f, const char *name)
    {
        return of(f.id(), name);
    }
    static box::result<snapshot> of_all(const char *name) { return of(0u, name); }

    static box::result<snapshot> adopt(std::uint32_t id, const char *name)
    {
        if (id == 0) return std::unexpected(box::error{box::errc::invalid_argument});
        return snapshot(id, name ? name : "");
    }
    static box::result<snapshot> adopt(std::uint32_t id)
    {
        if (id == 0) return std::unexpected(box::error{box::errc::invalid_argument});
        snap_info_t si{};
        int rc = ::snap_info(id, &si);
        if (rc != 0) return std::unexpected(box::error{box_errno_of(rc)});
        return snapshot(id, si.name);
    }
    static box::result<snapshot> reclaim(const char *name);

    std::uint32_t    id() const noexcept { return id_; }
    std::string_view name() const noexcept { return name_; }
    explicit operator bool() const noexcept { return owned_; }

    std::uint32_t keep() noexcept
    {
        owned_ = false;
        return id_;
    }

    box::status drop() noexcept
    {
        if (!owned_) return std::unexpected(box::error{box::errc::invalid_operation});
        int rc = ::snap_delete(id_);
        owned_ = false;
        return box::_detail::from_status(rc);
    }
};

struct snapshot_info {
    std::uint32_t id;
    std::string   name;
    std::uint64_t created_unix;
    std::uint32_t parent_file_id;
    std::uint32_t file_count;
    std::uint64_t total_size;
};

inline std::vector<std::uint32_t> snapshots()
{
    std::uint32_t              ids[64];
    std::uint32_t              n = 0;
    std::vector<std::uint32_t> out;
    if (::snap_list(ids, 64, &n) == 0) {
        out.reserve(n);
        for (std::uint32_t i = 0; i < n; ++i) out.push_back(ids[i]);
    }
    return out;
}

inline std::vector<snapshot_info> snapshots_named()
{
    std::vector<snapshot_info> out;
    std::uint32_t              ids[64];
    std::uint32_t              n = 0;
    if (::snap_list(ids, 64, &n) != 0) return out;
    out.reserve(n);
    for (std::uint32_t i = 0; i < n; ++i) {
        snap_info_t si{};
        if (::snap_info(ids[i], &si) != 0) continue;
        out.push_back(snapshot_info{si.id,
                                    std::string(field_view(si.name, sizeof(si.name))),
                                    si.created_time,
                                    si.parent_file_id,
                                    si.file_count,
                                    si.total_size});
    }
    return out;
}

inline box::result<snapshot> snapshot::reclaim(const char *name)
{
    if (!name) return std::unexpected(box::error{box::errc::invalid_argument});
    for (const snapshot_info &si : snapshots_named())
        if (si.name == name)
            return snapshot(si.id, si.name.c_str());
    return std::unexpected(box::error{box::errc::snapshot_not_found});
}

inline box::status anchor_all() { return box::_detail::from_status(::anchor(0)); }


struct anchor_payload {
    std::uint32_t file_id;
    std::uint8_t  op;
    std::uint8_t  _pad[3];
    std::uint64_t now_us;
};
static_assert(sizeof(anchor_payload) == 16, "anchor_payload must match kernel layout");
static_assert(offsetof(anchor_payload, file_id) == 0, "anchor_payload.file_id offset");
static_assert(offsetof(anchor_payload, op) == 4, "anchor_payload.op offset");
static_assert(offsetof(anchor_payload, now_us) == 8, "anchor_payload.now_us offset");

class anchor_event {
    std::uint32_t file_id_ = 0;
    std::uint8_t  op_      = 0;
    bool          valid_   = false;

public:
    anchor_event() noexcept = default;
    explicit anchor_event(const box::touch &e) noexcept
    {
        if (std::optional<anchor_payload> p = e.payload_as<anchor_payload>()) {
            file_id_ = p->file_id;
            op_      = p->op;
            valid_   = true;
        }
    }

    std::uint32_t file_id() const noexcept { return file_id_; }
    std::uint8_t  op() const noexcept { return op_; }
    bool whole_fs() const noexcept { return valid_ && file_id_ == 0; }
    bool is_anchor() const noexcept { return valid_ && op_ == 2; }
    explicit operator bool() const noexcept { return valid_; }
};

inline box::subscription on_anchor()
{
    return box::subscription(box::tag("anchor"));
}

}
}

#endif