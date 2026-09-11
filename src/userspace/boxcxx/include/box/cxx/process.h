#ifndef BOXCXX_BOX_PROCESS_H
#define BOXCXX_BOX_PROCESS_H

#include <cstdint>
#include <string>
#include <utility>

#include "box/system.h"
#include "box/core/cabin.h"
#include "box/cxx/error.h"

namespace box {

namespace this_process {

inline std::uint32_t pid() noexcept { return cabin_info()->pid; }
inline std::uint32_t spawner() noexcept { return cabin_info()->spawner_pid; }

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
inline bool has_tag(const char *tag) noexcept
{
    bool h = false;
    return tag && ::proc_tag_check(tag, &h) == 0 && h;
}

}

namespace cabin {

inline std::uint32_t  pid() noexcept { return cabin_info()->pid; }
inline std::uint32_t  spawner() noexcept { return cabin_info()->spawner_pid; }
inline std::uint64_t  heap_base() noexcept { return cabin_info()->heap_base; }
inline std::uint64_t  heap_max() noexcept { return cabin_info()->heap_max_size; }
inline std::uint64_t  buf_heap_base() noexcept { return cabin_info()->buf_heap_base; }
inline std::uint64_t  stack_top() noexcept { return cabin_info()->stack_top; }
inline const CabinInfo &info() noexcept { return *cabin_info(); }

}

class process {
    std::uint32_t pid_ = 0;

public:
    process() noexcept = default;
    explicit process(std::uint32_t pid) noexcept : pid_(pid) {}

    static result<process> spawn(const char *line,
                                 std::uint32_t *out_gen = nullptr) noexcept
    {
        if (out_gen) *out_gen = 0;
        if (!line) return std::unexpected(error{errc::invalid_argument});
        std::uint32_t gen = 0;
        int p = ::proc_exec_gen(line, nullptr, &gen);
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

    result<proc_info_t> info() const noexcept
    {
        if (pid_ == 0 || pid_ > 0xFFFFu)
            return std::unexpected(error{errc::invalid_pid});
        proc_info_t i{};
        int rc = ::proc_info(static_cast<std::uint16_t>(pid_), &i);
        if (rc != 0) return std::unexpected(error{box_errno_of(rc)});
        return i;
    }
    bool alive() const noexcept
    {
        auto i = info();
        return i.has_value() && i->state != PROC_STATE_TERMINATED;
    }
};

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
            tag_ = tag;
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

}

#endif