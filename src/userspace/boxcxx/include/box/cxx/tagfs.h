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
//     The derived accessors (name / size / flags / tags / has_tag / trashed)
//     stay plain values: they read info() once and report a sensible neutral on
//     the error arm. bytes(role) stays a stream (an open channel, not a scalar).
//
//   * box::tagfs::create(name, {tags…}) / query(tagspec) / all() / find(name) —
//     creation and tag-query returning a range of files. Richer predicates
//     compose over the result with std::views:
//         for (auto& f : box::tagfs::query("video:cam0")
//                        | std::views::filter([](auto& f){ return f.size() > 0; }))
//
//   * box::tagfs::context — a scoped, nesting-correct per-process tag filter
//     (RAII restore of the previous context); box::tagfs::snapshot — an owning
//     RAII handle on a CoW snapshot (auto-delete unless kept), with
//     box::tagfs::snapshots() listing all ids; box::tagfs::on_anchor() — a
//     box::touch subscription that delivers durability (anchor) events, read
//     through box::tagfs::anchor_event.
//
// This is a box:: extension, not part of std. The byte-stream bridge resolves by
// the file's NAME through the "file:" Current scheme; for I/O bound precisely to
// this file_id use read_at / write_at. query() returns up to 255 files (the
// boxlib query buffer limit).
#ifndef BOXCXX_BOX_TAGFS_H
#define BOXCXX_BOX_TAGFS_H

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

namespace box {
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
    bool        system = false;
};

class file {
    std::uint32_t id_ = 0;

public:
    file() noexcept = default;
    explicit file(std::uint32_t id) noexcept : id_(id) {}

    std::uint32_t id() const noexcept { return id_; }
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
    std::uint32_t flags() const { auto i = info(); return i ? i->flags : 0u; }
    bool trashed() const { return (flags() & FILE_FLAG_TRASHED) != 0u; }

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

    // ── tag mutations & lifecycle ────────────────────────────────────────
    // Empty status on success; the error arm carries the real kernel cause
    // (invalid_argument for an empty handle, else the recovered error_t — e.g.
    // tag_limit_exceeded, already_exists, object_not_found).
    box::status add_tag(const char *tag)
    {
        return id_ ? box::_detail::from_status(::tag_add(id_, tag))
                   : std::unexpected(box::error{box::errc::invalid_argument});
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

// ── creation ────────────────────────────────────────────────────────────
// create(name, {"key:value", "bare", ...}) — the tag list is joined with the
// comma separator the TagFS create expects. On success a live file; the error
// arm carries the recovered cause — invalid_argument for an empty/over-long
// name (boxlib create() validates it), or the kernel error_t (e.g.
// already_exists). The boxlib create returns a positive file_id, box_fail(rc)
// < 0 on failure; id == 0 cannot happen → internal.
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

// ── tag query → range of files ──────────────────────────────────────────
// tagspec == nullptr lists every file. Compose richer (non-tag) predicates
// over the result with std::views. Returns up to 255 files (boxlib limit).
inline std::vector<file> query(const char *tagspec)
{
    std::uint32_t      ids[255];
    int               n = ::query(tagspec, ids, 255);
    std::vector<file> out;
    if (n > 0) {
        out.reserve(static_cast<std::size_t>(n));
        for (int i = 0; i < n; ++i) out.emplace_back(ids[i]);
    }
    return out;
}
inline std::vector<file> all() { return query(nullptr); }

// ── lookup by name (TagFS names are not unique; find returns the first) ──
// On success the first matching file; the error arm separates the two no-result
// outcomes — a failed lookup (n < 0) surfaces its recovered cause, while a clean
// "no file by that name" (n == 0) is file_not_found, a queryable cause rather
// than a transport error. (To enumerate every match, use find_all.)
inline box::result<file> find(const char *name)
{
    std::uint32_t ids[8];
    file_info_t   infos[8];
    int           n = ::find_file_by_name(name, ids, infos, 8);
    if (n > 0) return file(ids[0]);
    if (n < 0) return std::unexpected(box::error{box_errno_of(n)});
    return std::unexpected(box::error{box::errc::file_not_found});
}
inline std::vector<file> find_all(const char *name)
{
    std::uint32_t     ids[8];
    file_info_t       infos[8];
    int               n = ::find_file_by_name(name, ids, infos, 8);
    std::vector<file> out;
    if (n > 0) {
        out.reserve(static_cast<std::size_t>(n));
        for (int i = 0; i < n; ++i) out.emplace_back(ids[i]);
    }
    return out;
}

// ── context — a scoped per-process tag filter (RAII, nesting-correct) ──────
// The context is a set of tags the kernel folds into every query/create this
// process makes (it narrows what files you see and auto-tags what you create).
// box::tagfs::context installs its tags on construction and *restores the
// previous context* on destruction, so contexts nest correctly:
//
//     box::tagfs::context outer("project:alpha");      // sees alpha files
//     {
//         box::tagfs::context inner("stage:review");    // alpha AND review
//     }                                                  // back to alpha-only
//
// Restoration is clear-then-reapply (not atomic) — correct for a cabin's single
// line of execution. Move-only.
class context {
    std::vector<std::string> prior_;
    bool                     active_ = false;

    void _M_enter(std::initializer_list<const char *> tags)
    {
        prior_ = current();  // capture the existing context before changing it
        for (const char *t : tags)
            if (t && *t) ::context_set(t);
        active_ = true;
    }
    void _M_restore()
    {
        ::context_clear();
        for (const std::string &t : prior_) ::context_set(t.c_str());
        active_ = false;
    }

public:
    context() noexcept = default;
    explicit context(const char *tag) { _M_enter({tag}); }
    context(std::initializer_list<const char *> tags) { _M_enter(tags); }

    context(const context &)            = delete;
    context &operator=(const context &) = delete;
    context(context &&o) noexcept : prior_(std::move(o.prior_)), active_(o.active_)
    {
        o.active_ = false;
    }
    context &operator=(context &&o) noexcept
    {
        if (this != &o) {
            if (active_) _M_restore();
            prior_    = std::move(o.prior_);
            active_   = o.active_;
            o.active_ = false;
        }
        return *this;
    }
    ~context() { if (active_) _M_restore(); }

    explicit operator bool() const noexcept { return active_; }

    // The tags in this process's context right now ("key" or "key:value"),
    // freshly read from the kernel.
    static std::vector<std::string> current()
    {
        std::vector<std::string> out;
        char                     buf[64][64];
        std::uint32_t            n = 0;
        if (::context_get(buf, 64, &n) == 0) {
            out.reserve(n);
            for (std::uint32_t i = 0; i < n; ++i) out.emplace_back(buf[i]);
        }
        return out;
    }
};

// ── snapshot — an owning RAII handle on a CoW snapshot ─────────────────────
// snap_create freezes a copy-on-write view of one file (or the whole
// filesystem) under a unique name; the snapshot's redirected blocks are
// reclaimed at snap_delete. This handle deletes the snapshot when it goes out
// of scope unless you keep() it. Move-only (like box::bay / box::brook).
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

    // Factories — a snapshot needs a unique name (1..31 chars). Capture one
    // file, or the whole filesystem (of_all). On success an owning snapshot; the
    // error arm carries the recovered cause — invalid_argument for a null name,
    // else the kernel error_t (e.g. already_exists when the name is taken).
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

    std::uint32_t    id() const noexcept { return id_; }
    std::string_view name() const noexcept { return name_; }
    explicit operator bool() const noexcept { return owned_; }

    // Detach — the snapshot outlives this handle (no auto-delete). Returns the
    // id so a caller can record it for a later box::tagfs::snapshots() scan.
    std::uint32_t keep() noexcept
    {
        owned_ = false;
        return id_;
    }

    // Delete now; after this the handle is empty. Empty status on success; the
    // error arm carries the cause — invalid_operation when the handle owns no
    // snapshot to drop, else the recovered snap_delete error_t. The snapshot is
    // always disowned (no double snap_delete from a later dtor).
    box::status drop() noexcept
    {
        if (!owned_) return std::unexpected(box::error{box::errc::invalid_operation});
        int rc = ::snap_delete(id_);
        owned_ = false;
        return box::_detail::from_status(rc);
    }
};

// Every CoW snapshot id currently known to the filesystem.
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

// ── anchor observer — bridge durability events into box::touch ─────────────
// anchor() (durability flush) publishes an ANCHOR event on the well-known
// "anchor" tag, and for a specific file also on each of that file's tags. A
// monitor subscribes with on_anchor() and reads the delivered box::touch
// through anchor_event to learn which file became durable.

// The payload anchor() publishes — must match the kernel's ObjAnchor record.
struct anchor_payload {
    std::uint32_t file_id;  // 0 == whole-filesystem anchor
    std::uint8_t  op;       // 2 == ANCHOR (1 == WRITE, on the same file tags)
    std::uint8_t  _pad[3];
    std::uint64_t now_us;
};
static_assert(sizeof(anchor_payload) == 16, "anchor_payload must match kernel layout");
static_assert(offsetof(anchor_payload, file_id) == 0, "anchor_payload.file_id offset");
static_assert(offsetof(anchor_payload, op) == 4, "anchor_payload.op offset");
static_assert(offsetof(anchor_payload, now_us) == 8, "anchor_payload.now_us offset");

// A typed view over a box::touch delivered on the "anchor" tag (or a file tag).
// Decodes once; file tags also carry WRITE events (op 1), so check is_anchor().
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

// Claim the "anchor" tag; poll() / wait() / co_await next() deliver each anchor
// as a box::touch you wrap in anchor_event. Returns a RAII box::subscription.
inline box::subscription on_anchor()
{
    return box::subscription(box::tag("anchor"));
}

}  // namespace tagfs
}  // namespace box

#endif  // BOXCXX_BOX_TAGFS_H
