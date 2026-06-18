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
//
//   * box::tagfs::create(name, {tags…}) / query(tagspec) / all() / find(name) —
//     creation and tag-query returning a range of files. Richer predicates
//     compose over the result with std::views:
//         for (auto& f : box::tagfs::query("video:cam0")
//                        | std::views::filter([](auto& f){ return f.size() > 0; }))
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
#include <vector>

#include "box/file.h"
#include "box/cxx/current.h"  // box::byte_current, box::role, box::file(), CURRENT_CREATE

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

    // Full metadata snapshot (one syscall); nullopt on error. The convenience
    // accessors below each take their own snapshot — call info() once if you
    // need several fields.
    std::optional<file_info_t> info() const
    {
        if (!id_) return std::nullopt;
        file_info_t inf{};
        if (::file_info(id_, &inf) != 0) return std::nullopt;
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

    // ── tag mutations & lifecycle (true on success) ──────────────────────
    bool add_tag(const char *tag)        { return id_ && ::tag_add(id_, tag) == 0; }
    bool remove_tag(const char *key)     { return id_ && ::tag_remove(id_, key) == 0; }
    bool rename(const char *new_name)    { return id_ && ::file_rename(id_, new_name) == 0; }
    bool remove()                        { return id_ && ::file_delete(id_) == 0; }
    bool anchor()                        { return id_ && ::anchor(id_) == 0; }

    // ── random-access byte I/O, bound to this file_id ────────────────────
    // Return the byte count transferred, or -1 on error (the native fread/
    // fwrite contract).
    int read_at(std::uint64_t offset, void *p, std::size_t n) const
    {
        return id_ ? ::fread(id_, offset, p, n) : -1;
    }
    int write_at(std::uint64_t offset, const void *p, std::size_t n)
    {
        return id_ ? ::fwrite(id_, offset, p, n) : -1;
    }
    int read_at(std::uint64_t offset, std::span<std::byte> buf) const
    {
        return read_at(offset, buf.data(), buf.size());
    }
    int write_at(std::uint64_t offset, std::span<const std::byte> buf)
    {
        return write_at(offset, buf.data(), buf.size());
    }

    template <class T>
    std::optional<T> read_object(std::uint64_t offset) const
    {
        static_assert(std::is_trivially_copyable_v<T>,
                      "read_object<T>: T must be trivially copyable");
        T v;
        if (read_at(offset, &v, sizeof(T)) == static_cast<int>(sizeof(T))) return v;
        return std::nullopt;
    }
    template <class T>
    bool write_object(std::uint64_t offset, const T &v)
    {
        static_assert(std::is_trivially_copyable_v<T>,
                      "write_object<T>: T must be trivially copyable");
        return write_at(offset, &v, sizeof(T)) == static_cast<int>(sizeof(T));
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
        unsigned    fl = (r == box::role::write) ? CURRENT_CREATE : 0u;
        return box::file(nm.c_str(), r, fl);
    }
};

// ── creation ────────────────────────────────────────────────────────────
// create(name, {"key:value", "bare", ...}) — the tag list is joined with the
// comma separator the TagFS create expects. Returns an empty file (operator
// bool == false) on failure.
inline file create(const char *name, std::initializer_list<const char *> tags)
{
    std::string spec;
    for (const char *t : tags) {
        if (!spec.empty()) spec.push_back(',');
        spec += t;
    }
    int id = ::create(name, spec.c_str());
    return id > 0 ? file(static_cast<std::uint32_t>(id)) : file{};
}
inline file create(const char *name, const char *tagspec = "")
{
    int id = ::create(name, tagspec);
    return id > 0 ? file(static_cast<std::uint32_t>(id)) : file{};
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
inline std::optional<file> find(const char *name)
{
    std::uint32_t ids[8];
    file_info_t   infos[8];
    int           n = ::find_file_by_name(name, ids, infos, 8);
    if (n > 0) return file(ids[0]);
    return std::nullopt;
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

}  // namespace tagfs
}  // namespace box

#endif  // BOXCXX_BOX_TAGFS_H
