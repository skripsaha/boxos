// boxcxx — box::tagfs  (the native C++ face of the BoxOS TagFS)
//
// BoxOS storage is a tag-named object store, not a path tree (no std::filesystem
// — that is a deliberate non-goal). A file is identified by a numeric id and
// carries key:value tags; you find files by querying tags, not by walking
// directories. This header is the idiomatic C++ surface over the boxlib C API
// (box/file.h):
//
//   * box::tagfs::file — a lightweight, NON-owning handle over a file_id (a file
//     is persistent shared state, so the handle is a copyable value, not RAII;
//     deletion is explicit via remove()). It exposes the metadata (info / name /
//     size / flags / tags), the tag mutations (add_tag / remove_tag / rename),
//     durability (anchor), and byte I/O in two shapes:
//       - random access bound to this file_id: read_at / write_at (span or raw)
//         and typed read_object<T> / write_object<T>,
//       - a streaming channel through the Current spine: bytes(role) →
//         box::byte_current (write / read / read_line / seek).
//     A fallible scalar operation hands back the REAL kernel cause through a
//     box::result<T> / box::status (info / read_at / write_at / read_object /
//     write_object return result; add_tag / remove_tag / rename / remove /
//     anchor return status), instead of collapsing it to bool / -1 / nullopt.
//     The derived accessors (name / size / flags / tags / has_tag)
//     stay plain values: they read info() once and report a sensible neutral on
//     the error arm. bytes(role) stays a stream (an open channel, not a scalar).
//
//   * box::tagfs::create(name, {tags…}) / query(tagspec, without) / all(without)
//     / find(name, without) — creation and tag-query, `without` the tags whose
//     files the asker leaves out (nullptr: none; the volume leaves out nothing
//     of its own accord); box::tagfs::tags(without) — the tags files wear, each
//     with how many files wear it. The return types are honestly asymmetric: find /
//     find_all hand back box::tagfs::record — an immutable metadata snapshot,
//     because the kernel already returned the descriptor — while query / all
//     hand back live box::tagfs::file handles, because the C query returns ids
//     only. A record reads its captured fields with zero syscalls; reach a live
//     handle for mutations or a fresh read through record::live(). Richer
//     predicates compose over either result with std::views:
//         for (auto& f : box::tagfs::query("video:cam0", nullptr)
//                        | std::views::filter([](auto& f){ return f.size() > 0; }))
//
//   * The Use Context (box/cxx/use.h) is the user's, one per machine, and
//     every query / create above is asked INSIDE it — `use code cpp` makes
//     query("project") mean code AND cpp AND project, and stamps a created file
//     code and cpp. query_everywhere / all_everywhere / create_everywhere ask
//     the same of the whole volume, the context left out.
//
//   * box::tagfs::on_anchor() — a box::touch subscription that delivers
//     durability (anchor) events, read through box::tagfs::anchor_event. There
//     is no snapshot handle: a format-v1 volume answers every snapshot request
//     unsupported (::snap_create and its kin say so), and snapshots arrive
//     with format v2.
//
// This is a box:: extension, not part of std. The byte-stream bridge resolves by
// the file's NAME through the "file:" Current scheme; for I/O bound precisely to
// this file_id use read_at / write_at. query() returns up to 255 files (the
// boxlib query buffer limit).
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
#include "box/cxx/current.h"  // box::byte_current, box::role, box::file(), CURRENT_CREATE
#include "box/cxx/error.h"    // box::result / box::status / box::error / box_errno_of
#include "box/cxx/touch.h"    // box::tag, box::touch, box::subscription (anchor observer)
#include "box/memory.h"       // free — query_all hands back a list the caller frees

namespace box {

class ferry;  // box/cxx/ferry.h — returned by file::read_async / write_async (co_await-able)

namespace tagfs {

// Read a NUL-terminated fixed-width char field without overrunning it (TagFS
// metadata fields are capacity-bounded; a missing terminator must not leak).
inline std::string_view field_view(const char *p, std::size_t cap) noexcept
{
    std::size_t n = 0;
    while (n < cap && p[n] != '\0') ++n;
    return std::string_view(p, n);
}

// One of a file's tags: key[:value], user or system.
struct tag {
    std::string key;
    std::string value;
    bool        system = false;  // true iff this tag is the bare reserved-vocabulary
                                 // key the kernel stamps system at mount; a
                                 // value-bearing key (e.g. "system:foo") is never
                                 // system, nor is an auto-label / user tag

    // Read the value as an integer (or bool) — the typed inverse of file's
    // add_tag<T>. On success the parsed T; the error arm carries the cause —
    // out_of_range for a value the type can't hold, else invalid_argument for a
    // non-numeric / partly-numeric value (or a bool that is neither "0" nor "1").
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
// Find one tag by key inside an already-captured file_info_t — the shared scan
// behind file::tag_named (after a single info() syscall) and record::tag_named
// (zero syscalls). tag_not_found when the key is not among the (<=5) tags the
// descriptor carries.
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
}  // namespace _detail

class file {
    file_id_t id_ = 0;

public:
    file() noexcept = default;
    explicit file(file_id_t id) noexcept : id_(id) {}

    file_id_t id() const noexcept { return id_; }
    explicit operator bool() const noexcept { return id_ != 0; }

    // Full metadata snapshot (one syscall). On success the descriptor; the error
    // arm carries the recovered cause — invalid_argument for an empty handle, or
    // the kernel error_t (e.g. object_not_found once the file is gone). The
    // convenience accessors below each take their own snapshot — call info() once
    // if you need several fields.
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

    std::vector<tag> tags() const
    {
        std::vector<tag> out;
        auto i = info();
        if (!i) return out;
        unsigned n = i->tag_count;
        if (n > 5) n = 5;  // file_info_t carries at most 5 tags
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

    // One named tag, typed-read-ready (pair with tag::as<T>). Takes one info()
    // snapshot, then scans it; the error arm carries the cause — info()'s error
    // (empty handle / object_not_found), else tag_not_found when the key isn't
    // present.
    box::result<tag> tag_named(const char *key) const
    {
        auto i = info();
        if (!i) return std::unexpected(i.error());
        return _detail::tag_from_info(*i, key);
    }

    // ── tag mutations & lifecycle ────────────────────────────────────────
    // Empty status on success; the error arm carries the real kernel cause
    // (invalid_argument for an empty handle, else the recovered error_t — e.g.
    // tag_limit_exceeded, already_exists, object_not_found).
    box::status add_tag(const char *tag)
    {
        return id_ ? box::_detail::from_status(::tag_add(id_, tag))
                   : std::unexpected(box::error{box::errc::invalid_argument});
    }
    // Typed value write — the inverse of tag::as<T>. Formats the integer (or
    // bool as "0"/"1") and adds "key:value" through the raw overload above.
    // buffer_too_small when the formatted value won't fit the tag value field
    // (11 chars + NUL); the two overloads differ in arity, so this never hijacks
    // the raw add_tag(const char*).
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
    // The file made exactly `n` bytes long (file_span): shorter cuts the tail
    // and gives its blocks back; longer reads as zeros from the old size on,
    // never another file's bytes. A write grows a file only at its end — one
    // past it lands in the error arm — so this is how a gap is made. Refused
    // on a file the system keeps in a role (permission_denied), like remove(). The
    // change is published on every tag of the file.
    box::status span(std::uint64_t n)
    {
        return id_ ? box::_detail::from_status(::file_span(id_, n, 0))
                   : std::unexpected(box::error{box::errc::invalid_argument});
    }

    // ── random-access byte I/O, bound to this file_id ────────────────────
    // On success the byte count transferred; the error arm carries the recovered
    // cause (invalid_argument for an empty handle, else the native fread/fwrite
    // error_t — e.g. out_of_range, io). The boxlib fread/fwrite hand back an
    // int64 count >= 0 (up to 4 GiB) or box_fail(rc) < 0, which from_ret64 turns
    // into value-or-cause without the count ever colliding with the cause sign.
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
    // At the file's end as it is when the write reaches it (file_append):
    // writers adding to one file never lay their bytes over each other's.
    box::result<std::size_t> append(const void *p, std::size_t n)
    {
        return box::_detail::from_ret64<std::size_t>(
            id_ ? ::file_append(id_, p, n) : -ERR_INVALID_ARGUMENT);
    }
    box::result<std::size_t> append(std::span<const std::byte> buf)
    {
        return append(buf.data(), buf.size());
    }

    // ── async byte I/O — submit now, co_await the returned box::ferry ────────
    // read_async / write_async submit a storage op WITHOUT blocking and hand back
    // a box::ferry you co_await on a box::executor for the byte count (or cause).
    // Several may be in flight at once; each resumes when ITS completion lands.
    // The DATA buffer must outlive the co_await (the kernel DMAs into it). Defined
    // out-of-line in box/cxx/ferry.h — include it to use these (keeps <coroutine>
    // out of this header). An empty handle yields a ready invalid_argument ferry.
    box::ferry read_async(std::uint64_t offset, void *p, std::size_t n) const;
    box::ferry write_async(std::uint64_t offset, const void *p, std::size_t n) const;
    box::ferry read_async(std::uint64_t offset, std::span<std::byte> buf) const;
    box::ferry write_async(std::uint64_t offset, std::span<const std::byte> buf) const;

    // Typed whole-object I/O. On success the T / empty status; the error arm
    // carries the cause. A short transfer (the op succeeded but moved the wrong
    // number of bytes) is reported as errc::io rather than a silent truncation.
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

    // ── streaming byte channel through the Current spine ─────────────────
    // Opens a byte_current on this file's name via the "file:" scheme
    // (write/read/read_line/seek). Resolves by NAME; for id-precise I/O use
    // read_at / write_at.
    box::byte_current bytes(box::role r) const
    {
        auto i = info();
        if (!i) return {};
        std::string nm(field_view(i->filename, sizeof(i->filename)));
        box::opening fl = (r == box::role::write) ? box::opening::create : box::opening::none;
        return box::file(nm.c_str(), r, fl);
    }
};

// ── record — an immutable metadata snapshot (id + an owned file_info_t) ─────
// Where box::tagfs::file is a live handle whose accessors each take a fresh
// syscall, a record captures the descriptor once and reads it back with zero
// syscalls — the speed win when a lookup already returned the metadata (find /
// find_all do). It is a plain trivially-copyable value; mutate or re-read fresh
// through live(), and re-capture the snapshot through reread().
class record {
    file_id_t   id_ = 0;
    file_info_t info_{};

public:
    record() noexcept = default;
    record(file_id_t id, const file_info_t &i) noexcept : id_(id), info_(i) {}

    file_id_t id() const noexcept { return id_; }
    explicit operator bool() const noexcept { return id_ != 0; }

    // A live handle on the same file — for mutations or an always-fresh read.
    file live() const noexcept { return file(id_); }

    // Re-capture the descriptor from the kernel. Empty status on success; the
    // error arm carries the cause (invalid_argument for an empty record, else
    // the recovered file_info error_t).
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

    // The captured fields — every read hits the local snapshot, not the kernel
    // (the speed win is the absent syscall, not an absent copy). name() returns
    // an owning std::string like file::name() and record's own tags()/tag_named(),
    // so reading from a temporary record — find("x")->name() — is safe.
    std::string name() const { return std::string(field_view(info_.filename, sizeof(info_.filename))); }
    std::uint64_t    size() const noexcept { return info_.size; }
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

// ── creation ────────────────────────────────────────────────────────────
// create(name, {"key:value", "bare", ...}) — the tag list is joined with the
// comma separator the TagFS create expects. On success a live file; the error
// arm carries the recovered cause — invalid_argument for an empty/over-long
// name (boxlib create() validates it), or the kernel error_t (e.g.
// already_exists). The boxlib create returns a positive file_id in an int64,
// box_fail(rc) < 0 on failure; id == 0 cannot happen → internal.
inline box::result<file> create(const char *name, std::initializer_list<const char *> tags)
{
    std::string spec;
    for (const char *t : tags) {
        if (!spec.empty()) spec.push_back(',');
        spec += t;
    }
    std::int64_t id = ::create(name, spec.c_str());
    if (id > 0) return file(static_cast<file_id_t>(id));
    return std::unexpected(box::error{
        id < 0 ? box_errno_of(static_cast<int>(id)) : static_cast<::error_t>(ERR_INTERNAL)});
}
inline box::result<file> create(const char *name, const char *tagspec = "")
{
    std::int64_t id = ::create(name, tagspec);
    if (id > 0) return file(static_cast<file_id_t>(id));
    return std::unexpected(box::error{
        id < 0 ? box_errno_of(static_cast<int>(id)) : static_cast<::error_t>(ERR_INTERNAL)});
}

// ── tag query → range of files ──────────────────────────────────────────
// tagspec == nullptr lists every file; `without` (a comma list, nullptr for
// none) the tags no file of the answer wears. The volume sets aside nothing of
// its own accord — hidden is a tag like any other, and a deleted file is not
// met at all (query_deleted) — so the asker says
// what it leaves out (box/file.h). Compose richer (non-tag) predicates over the
// result with std::views. Every match: the kernel says how many
// there are and boxlib sizes the list to that (query_all), so no file is
// past the end of the answer.
namespace _detail {
inline std::vector<file> files_of(int n, file_id_t *ids)
{
    std::vector<file> out;
    if (n > 0) {
        out.reserve(static_cast<std::size_t>(n));
        for (int i = 0; i < n; ++i) out.emplace_back(ids[i]);
    }
    ::free(ids);
    return out;
}
} // namespace _detail
inline std::vector<file> query(const char *tagspec, const char *without)
{
    file_id_t *ids = nullptr;
    int        n   = ::query_all(tagspec, without, &ids);
    return _detail::files_of(n, ids);
}
inline std::vector<file> all(const char *without) { return query(nullptr, without); }

// ── the same, of the whole volume — the user's Use Context left out ─────
// query / all / create above are asked inside the Use Context; these ask the
// same question of everything on the volume. See box/cxx/use.h.
inline std::vector<file> query_everywhere(const char *tagspec, const char *without)
{
    file_id_t *ids = nullptr;
    int        n   = ::query_all_everywhere(tagspec, without, &ids);
    return _detail::files_of(n, ids);
}
inline std::vector<file> all_everywhere(const char *without) { return query_everywhere(nullptr, without); }

inline box::result<file> create_everywhere(const char *name, std::initializer_list<const char *> tags)
{
    std::string spec;
    for (const char *t : tags) {
        if (!spec.empty()) spec.push_back(',');
        spec += t;
    }
    std::int64_t id = ::create_everywhere(name, spec.c_str());
    if (id > 0) return file(static_cast<file_id_t>(id));
    return std::unexpected(box::error{
        id < 0 ? box_errno_of(static_cast<int>(id)) : static_cast<::error_t>(ERR_INTERNAL)});
}
inline box::result<file> create_everywhere(const char *name, const char *tagspec = "")
{
    std::int64_t id = ::create_everywhere(name, tagspec);
    if (id > 0) return file(static_cast<file_id_t>(id));
    return std::unexpected(box::error{
        id < 0 ? box_errno_of(static_cast<int>(id)) : static_cast<::error_t>(ERR_INTERNAL)});
}

// ── the volume's tags ───────────────────────────────────────────────────
// tags() — every tag a file wears, with how many files wear it, inside the Use
// Context (counted among its files); tags_everywhere() the same of the whole
// volume. One ask of the volume's own list of its tags (storage.tag.list), not
// a walk of its files: a tag no file wears — a process's, a Touch's — is not
// in it. In the order the volume first knew the tags; a refusal is an empty
// list, as query()'s is. The files wearing a tag of `without` are not counted.
struct tag_count {
    tag           what;
    std::uint32_t files = 0;
};
namespace _detail {
inline std::vector<tag_count> tags_of(int rc, TagListed *list, std::size_t n)
{
    std::vector<tag_count> out;
    if (rc == 0 && n > 0) {
        out.reserve(n);
        for (std::size_t i = 0; i < n; ++i) {
            const std::string_view text(list[i].Tag);
            const std::size_t      colon = text.find(':');
            tag what{std::string(text.substr(0, colon)),
                     colon == std::string_view::npos ? std::string() : std::string(text.substr(colon + 1)),
                     list[i].System};
            out.push_back(tag_count{std::move(what), list[i].Files});
        }
    }
    ::free(list);
    return out;
}
}  // namespace _detail
inline std::vector<tag_count> tags(const char *without)
{
    TagListed  *list = nullptr;
    std::size_t n    = 0;
    int         rc   = ::tag_list(without, &list, &n);
    return _detail::tags_of(rc, list, n);
}
inline std::vector<tag_count> tags_everywhere(const char *without)
{
    TagListed  *list = nullptr;
    std::size_t n    = 0;
    int         rc   = ::tag_list_everywhere(without, &list, &n);
    return _detail::tags_of(rc, list, n);
}

// ── lookup by name (TagFS names are not unique; find returns the first) ──
// find / find_all hand back records (the kernel already returned the metadata),
// so their accessors read with zero syscalls — the asymmetry with query/all
// (live file handles, since the query op returns ids only) is intentional.
// On success the first matching record; the error arm separates the two no-result
// outcomes — a failed lookup (n < 0) surfaces its recovered cause, while a clean
// "no file by that name" (n == 0) is file_not_found, a queryable cause rather
// than a transport error. (For every same-named match use find_all.) Files
// wearing a tag of `without` are not matched, as query() leaves them out.
inline box::result<record> find(const char *name, const char *without)
{
    file_id_t   ids[1];
    file_info_t infos[1];
    int         n = ::find_file_by_name(name, without, ids, infos, 1);
    if (n > 0) return record(ids[0], infos[0]);
    if (n < 0) return std::unexpected(box::error{box_errno_of(n)});
    return std::unexpected(box::error{box::errc::file_not_found});
}
// Every same-named match as a record. find_file_by_name returns how many files
// carry the name and writes as many as there is room for, so the count is asked
// first and the buffers sized to it; a file created between the two asks makes
// the second answer larger, and the ask is repeated with room for it. A
// transport failure is reported as an empty range, consistent with
// query()/all(); use find() when you need the recovered cause of a lookup.
inline std::vector<record> find_all(const char *name, const char *without)
{
    std::vector<record> out;
    int n = ::find_file_by_name(name, without, nullptr, nullptr, 0);
    while (n > 0) {
        std::vector<file_id_t>   ids(static_cast<std::size_t>(n));
        std::vector<file_info_t> infos(static_cast<std::size_t>(n));
        int got = ::find_file_by_name(name, without, ids.data(), infos.data(), ids.size());
        if (got <= n) {
            for (int i = 0; i < got; ++i) out.emplace_back(ids[i], infos[i]);
            break;
        }
        n = got;
    }
    return out;
}

// Flush the whole filesystem to durable storage — the whole-FS counterpart of
// file::anchor(), over the C anchor(0). Empty status on success; the error arm
// carries the recovered anchor error_t.
inline box::status anchor_all() { return box::_detail::from_status(::anchor(0)); }

// ── anchor observer — bridge durability events into box::touch ─────────────
// anchor() (durability flush) publishes an ANCHOR event on the well-known
// "anchor" tag, and for a specific file also on each of that file's tags. A
// monitor subscribes with on_anchor() and reads the delivered box::touch
// through anchor_event to learn which file became durable.

// The payload anchor() publishes — must match the kernel's ObjAnchor record.
struct anchor_payload {
    file_id_t     file_id;  // 0 == whole-filesystem anchor
    std::uint8_t  op;       // 2 == ANCHOR (1 == WRITE, on the same file tags)
    std::uint8_t  _pad[7];
    std::uint64_t now_us;
};
static_assert(sizeof(anchor_payload) == 24, "anchor_payload must match kernel layout");
static_assert(offsetof(anchor_payload, file_id) == 0, "anchor_payload.file_id offset");
static_assert(offsetof(anchor_payload, op) == 8, "anchor_payload.op offset");
static_assert(offsetof(anchor_payload, now_us) == 16, "anchor_payload.now_us offset");

// A typed view over a box::touch delivered on the "anchor" tag (or a file tag).
// Decodes once; file tags also carry WRITE events (op 1), so check is_anchor().
class anchor_event {
    file_id_t     file_id_ = 0;
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

    file_id_t     file_id() const noexcept { return file_id_; }
    std::uint8_t  op() const noexcept { return op_; }
    bool whole_fs() const noexcept { return valid_ && file_id_ == 0; }
    bool is_anchor() const noexcept { return valid_ && op_ == 2; }
    explicit operator bool() const noexcept { return valid_; }
};

// Claim the "anchor" tag; poll() / wait() / co_await next() deliver each anchor
// as a box::touch you wrap in anchor_event. Returns a RAII box::subscription.
inline box::subscription on_anchor()
{
    return box::subscription(box::tag("anchor"));
}

}  // namespace tagfs
}  // namespace box

#endif  // BOXCXX_BOX_TAGFS_H
