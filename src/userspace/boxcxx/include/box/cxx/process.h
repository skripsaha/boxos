// boxcxx — box::process / box::this_process / box::cabin  (process & cabin introspection)
//
// In BoxOS a "spawn" creates a new cabin — a fresh address space with its own
// pid — not a thread (shared-address-space threads are the future Strands epic).
// This header is the idiomatic C++ surface over the boxlib process/cabin API
// (box/system.h, box/core/cabin.h):
//
//   box::this_process — identity of the running process (pid / spawner) and its
//                       own process tags (add/remove/has — the tags box::message
//                       broadcasts target via process membership).
//   box::process      — a handle to a process by pid: spawn(name) (a new cabin),
//                       self(), pid(), info(), alive().
//   box::cabin        — this cabin's address-space layout (heap / stack) + ids.
//   box::tag_scope    — RAII: carry a process tag for the duration of a scope, so
//                       this process is a broadcast target for it, then drop it.
//
// This is a box:: extension, not part of std. Process tags are self-scoped
// (proc_tag_* act on the calling process); to tag another process you spawn it.
#ifndef BOXCXX_BOX_PROCESS_H
#define BOXCXX_BOX_PROCESS_H

#include <cstdint>
#include <string>
#include <utility>

#include "box/system.h"       // proc_exec / proc_info / proc_tag_* / proc_info_t
#include "box/core/cabin.h"   // CabinInfo + cabin_info()
#include "box/cxx/error.h"    // box::status / box::result / box::error

namespace box {

// ── box::this_process — identity + self process tags ────────────────────────
namespace this_process {

inline std::uint32_t pid() noexcept { return cabin_info()->pid; }
inline std::uint32_t spawner() noexcept { return cabin_info()->spawner_pid; }  // launcher pid

// Process tags decide broadcast membership (box::broadcast(tag, …) reaches every
// process carrying that tag). These act on the calling process. Empty status on
// success; the error arm carries the real kernel cause (e.g. tag_limit_exceeded,
// invalid_tag) instead of collapsing it to a bool.
inline status add_tag(const char *tag) noexcept
{
    return tag ? _detail::from_status(::proc_tag_add(tag))
               : std::unexpected(error{errc::invalid_argument});
}
inline status remove_tag(const char *tag) noexcept
{
    return tag ? _detail::from_status(::proc_tag_remove(tag))
               : std::unexpected(error{errc::invalid_argument});
}
// has_tag stays a predicate: "not present" is a normal false, not a cause.
inline bool has_tag(const char *tag) noexcept
{
    bool h = false;
    return tag && ::proc_tag_check(tag, &h) == 0 && h;
}

}  // namespace this_process

// ── box::cabin — this cabin's address space + identity ──────────────────────
namespace cabin {

inline std::uint32_t  pid() noexcept { return cabin_info()->pid; }
inline std::uint32_t  spawner() noexcept { return cabin_info()->spawner_pid; }
inline std::uint64_t  heap_base() noexcept { return cabin_info()->heap_base; }
inline std::uint64_t  heap_max() noexcept { return cabin_info()->heap_max_size; }
inline std::uint64_t  buf_heap_base() noexcept { return cabin_info()->buf_heap_base; }
inline std::uint64_t  stack_top() noexcept { return cabin_info()->stack_top; }
inline const CabinInfo &info() noexcept { return *cabin_info(); }

}  // namespace cabin

// ── box::process — a handle to a process (by pid) ───────────────────────────
class process {
    std::uint32_t pid_ = 0;

public:
    process() noexcept = default;
    explicit process(std::uint32_t pid) noexcept : pid_(pid) {}

    // Spawn a program as a new cabin. On success a process handle for the child's
    // pid; the error arm carries the real cause — the recovered kernel error_t
    // (e.g. file_not_found for an unknown binary, process_limit_exceeded), or
    // spawn_failed when the kernel returned no pid without naming a cause.
    //
    // out_gen (optional) receives the child's pid-allocator generation — the
    // second half of its canonical (pid, generation) identity, used by
    // box::child to match a death to the exact incarnation. 0 on failure or if
    // the kernel reported no generation; existing spawn(name) callers ignore it.
    static result<process> spawn(const char *name,
                                 std::uint32_t *out_gen = nullptr) noexcept
    {
        if (out_gen) *out_gen = 0;
        if (!name) return std::unexpected(error{errc::invalid_argument});
        std::uint32_t gen = 0;
        int p = ::proc_exec_gen(name, nullptr, &gen);
        if (p > 0) {
            if (out_gen) *out_gen = gen;
            return process(static_cast<std::uint32_t>(p));
        }
        return std::unexpected(error{p < 0 ? box_errno_of(p)
                                           : static_cast<::error_t>(ERR_SPAWN_FAILED)});
    }
    static process self() noexcept { return process(this_process::pid()); }

    std::uint32_t pid() const noexcept { return pid_; }
    explicit operator bool() const noexcept { return pid_ != 0; }

    // A snapshot of this process (pid / state / priority). On success the
    // descriptor; the error arm carries the cause — invalid_pid for an out-of-
    // range handle, or the recovered kernel error_t (e.g. process_not_found once
    // the process has gone).
    result<proc_info_t> info() const noexcept
    {
        if (pid_ == 0 || pid_ > 0xFFFFu)  // proc_info pid is 16-bit
            return std::unexpected(error{errc::invalid_pid});
        proc_info_t i{};
        int rc = ::proc_info(static_cast<std::uint16_t>(pid_), &i);
        if (rc != 0) return std::unexpected(error{box_errno_of(rc)});
        return i;
    }
    // Predicate: alive iff info() succeeds and the state is not terminated. "Gone"
    // (info() in the error arm) reads as not-alive, never a thrown cause.
    bool alive() const noexcept
    {
        auto i = info();
        return i.has_value() && i->state != PROC_STATE_TERMINATED;
    }
};

// ── box::tag_scope — a process tag held for the duration of a scope ─────────
// While alive, the process carries `tag`, so box::broadcast(tag, …) from another
// process reaches it; the tag is removed when the scope ends. Move-only.
class tag_scope {
    std::string tag_;
    bool        active_ = false;

    void _M_release() noexcept
    {
        if (active_) {
            ::proc_tag_remove(tag_.c_str());
            active_ = false;
        }
    }

public:
    tag_scope() noexcept = default;
    explicit tag_scope(const char *tag)
    {
        if (tag && *tag) {
            tag_ = tag;  // copy the name BEFORE registering, so a throw here
                         // (bad_alloc) cannot orphan a kernel tag
            if (::proc_tag_add(tag) == 0) active_ = true;
            else tag_.clear();
        }
    }
    tag_scope(const tag_scope &)            = delete;
    tag_scope &operator=(const tag_scope &) = delete;
    tag_scope(tag_scope &&o) noexcept : tag_(std::move(o.tag_)), active_(o.active_)
    {
        o.active_ = false;
    }
    tag_scope &operator=(tag_scope &&o) noexcept
    {
        if (this != &o) {
            _M_release();
            tag_      = std::move(o.tag_);
            active_   = o.active_;
            o.active_ = false;
        }
        return *this;
    }
    ~tag_scope() { _M_release(); }

    explicit operator bool() const noexcept { return active_; }
    const std::string &tag() const noexcept { return tag_; }
};

}  // namespace box

#endif  // BOXCXX_BOX_PROCESS_H
