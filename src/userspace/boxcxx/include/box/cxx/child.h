#ifndef BOXCXX_BOX_CHILD_H
#define BOXCXX_BOX_CHILD_H

#include <coroutine>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "box/cxx/error.h"
#include "box/cxx/process.h"
#include "box/cxx/touch.h"
#include "box/cxx/executor.h"
#include "box/system.h"
#include "box/touch.h"
#include "box/error.h"
#include "box/cpu.h"
#include "proc_exit.h"

namespace box {

namespace _detail {

struct child_slot {
    std::uint32_t pid;
    std::uint32_t gen;
    int           code;
    bool          has_death;
};

struct child_station {
    box::subscription       sub_;
    std::vector<child_slot> slots_;

    child_station() noexcept : sub_(box::tags::process_died()) {}

    bool ok() const noexcept { return static_cast<bool>(sub_); }

    void register_child(std::uint32_t pid, std::uint32_t gen)
    {
        slots_.push_back(child_slot{pid, gen, 0, false});
    }

    void route(const box::touch& ev) noexcept
    {
        std::optional<::TouchProcessDied> d = ev.payload_as<::TouchProcessDied>();
        if (!d) return;
        std::uint32_t pid  = d->pid;
        std::uint32_t gen  = d->generation;
        int           code = d->exit_code;
        for (auto &s : slots_)
            if (s.pid == pid && s.gen == gen) { s.code = code; s.has_death = true; return; }
    }

    bool poll_collect(std::uint32_t pid, std::uint32_t gen, int *out)
    {
        while (std::optional<box::touch> ev = sub_.poll()) route(*ev);
        return consume(pid, gen, out);
    }

    bool wait_collect(std::uint32_t pid, std::uint32_t gen, std::uint32_t ms, int *out)
    {
        if (poll_collect(pid, gen, out)) return true;
        if (!sub_) return false;

        if (ms == 0) {
            for (;;) {
                if (std::optional<box::touch> ev = sub_.wait(0)) {
                    route(*ev);
                    if (consume(pid, gen, out)) return true;
                }
            }
        }

        std::uint64_t deadline = cpu_rdtsc() + cpu_ms_to_tsc(ms);
        for (;;) {
            std::uint64_t now = cpu_rdtsc();
            if (now >= deadline) return false;
            std::uint64_t rem = cpu_tsc_to_ms(deadline - now);
            std::uint32_t slice = rem == 0           ? 1u
                                : rem > 0xFFFFFFFFull ? 0xFFFFFFFFu
                                                      : static_cast<std::uint32_t>(rem);
            std::optional<box::touch> ev = sub_.wait(slice);
            if (!ev) return false;
            route(*ev);
            if (consume(pid, gen, out)) return true;
        }
    }

    bool consume(std::uint32_t pid, std::uint32_t gen, int *out) noexcept
    {
        for (std::size_t i = 0; i < slots_.size(); ++i) {
            if (slots_[i].pid == pid && slots_[i].gen == gen && slots_[i].has_death) {
                *out = slots_[i].code;
                slots_[i] = slots_.back();
                slots_.pop_back();
                return true;
            }
        }
        return false;
    }

    void deregister(std::uint32_t pid, std::uint32_t gen) noexcept
    {
        for (std::size_t i = 0; i < slots_.size(); ++i) {
            if (slots_[i].pid == pid && slots_[i].gen == gen) {
                slots_[i] = slots_.back();
                slots_.pop_back();
                return;
            }
        }
    }
};

inline child_station &station()
{
    thread_local child_station s;
    return s;
}

}

class child {
    std::uint32_t pid_    = 0;
    std::uint32_t gen_    = 0;
    int           code_   = 0;
    bool          exited_ = false;

public:
    static result<child> spawn(const char *name) noexcept
    {
        _detail::child_station &st = _detail::station();
        if (!st.ok()) return std::unexpected(error{errc::process_blocked});

        std::uint32_t gen = 0;
        result<process> p = process::spawn(name, &gen);
        if (!p) return std::unexpected(p.error());

        std::uint32_t pid = p->pid();
        if (gen == 0) {
            ::proc_kill(pid);
            return std::unexpected(error{errc::process_blocked});
        }
        try {
            st.register_child(pid, gen);
        } catch (...) {
            ::proc_kill(pid);
            return std::unexpected(error{errc::no_memory});
        }
        return child(pid, gen);
    }

    child() noexcept = default;
    child(const child &)            = delete;
    child &operator=(const child &) = delete;

    child(child &&o) noexcept
        : pid_(o.pid_), gen_(o.gen_), code_(o.code_), exited_(o.exited_)
    {
        o.pid_ = 0; o.gen_ = 0; o.code_ = 0; o.exited_ = false;
    }
    child &operator=(child &&o) noexcept
    {
        if (this != &o) {
            _M_detach();
            pid_ = o.pid_; gen_ = o.gen_; code_ = o.code_; exited_ = o.exited_;
            o.pid_ = 0; o.gen_ = 0; o.code_ = 0; o.exited_ = false;
        }
        return *this;
    }
    ~child() { _M_detach(); }

    explicit operator bool() const noexcept { return pid_ != 0; }
    std::uint32_t pid() const noexcept { return pid_; }

    bool alive() noexcept
    {
        if (pid_ == 0) return false;
        if (exited_) return false;
        _M_poll_reap();
        return !exited_;
    }

    status kill() noexcept
    {
        if (pid_ == 0) return {};
        _M_poll_reap();
        if (exited_) return {};
        int rc = ::proc_kill(pid_);
        if (rc == 0) return {};
        ::error_t e = box_errno_of(rc);
        if (e == ERR_PROCESS_NOT_FOUND || e == ERR_PROCESS_TERMINATED) return {};
        return std::unexpected(error{e});
    }

    result<int> wait(std::uint32_t timeout_ms = 0) noexcept
    {
        if (pid_ == 0) return std::unexpected(error{errc::invalid_argument});
        if (!exited_) _M_wait_reap(timeout_ms);
        if (!exited_) return std::unexpected(error{errc::timeout});
        return _M_to_result();
    }

    class exited_awaiter {
    public:
        explicit exited_awaiter(child *c) noexcept : _M_c(c) {}

        bool await_ready() noexcept
        {
            return _M_c->pid_ == 0 || _M_c->exited_ || _M_c->_M_poll_reap();
        }
        bool await_suspend(std::coroutine_handle<> h)
        {
            executor::current()->wait_on(h, this, &_S_poll, &_S_block,
                                         __exec::wait_domain::touch);
            return true;
        }
        result<int> await_resume() noexcept
        {
            if (_M_c->pid_ == 0 && !_M_c->exited_)
                return std::unexpected(error{errc::invalid_argument});
            return _M_c->_M_to_result();
        }

    private:
        static bool _S_poll(void *s)
        {
            child *c = static_cast<exited_awaiter *>(s)->_M_c;
            return c->exited_ || c->_M_poll_reap();
        }
        static void _S_block(void *s, std::uint32_t ms)
        {
            child *c = static_cast<exited_awaiter *>(s)->_M_c;
            if (!c->exited_) c->_M_wait_reap(ms);
        }

        child *_M_c;
    };

    exited_awaiter exited() noexcept { return exited_awaiter{this}; }

private:
    explicit child(std::uint32_t pid, std::uint32_t gen) noexcept : pid_(pid), gen_(gen) {}

    void _M_detach() noexcept
    {
        if (pid_ != 0 && !exited_) _detail::station().deregister(pid_, gen_);
        pid_ = 0;
    }

    result<int> _M_to_result() const noexcept
    {
        if (code_ >= 0)                  return code_;
        if (code_ == PROC_EXIT_KILLED)   return std::unexpected(error{errc::process_killed});
        return std::unexpected(error{errc::process_crashed});
    }

    bool _M_poll_reap() noexcept
    {
        if (exited_) return true;
        try {
            if (_detail::station().poll_collect(pid_, gen_, &code_)) exited_ = true;
        } catch (...) {}
        return exited_;
    }
    bool _M_wait_reap(std::uint32_t ms) noexcept
    {
        if (exited_) return true;
        try {
            if (_detail::station().wait_collect(pid_, gen_, ms, &code_)) exited_ = true;
        } catch (...) {}
        return exited_;
    }
};

}

#endif