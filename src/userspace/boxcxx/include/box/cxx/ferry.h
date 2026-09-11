#ifndef BOXCXX_BOX_FERRY_H
#define BOXCXX_BOX_FERRY_H

#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "box/cxx/error.h"
#include "box/cxx/executor.h"
#include "box/cxx/tagfs.h"
#include "box/core/manifest.h"
#include "boxos_decks.h"
#include "box/memory.h"
#include "box/error.h"

namespace box {

namespace _detail {

struct ferry_submission {
    std::uint8_t manifest[sizeof(Manifest) + sizeof(ManifestOp) + 24];
    Crate        crates[1];
};

struct ferry_slot {
    std::uint64_t waybill;
    void         *sub;
    std::uint32_t bytes;
    int           code;
    bool          done;
    bool          orphaned;
};

struct ferry_station {
    std::vector<ferry_slot> slots_;
    std::uint64_t           next_waybill_ = 0;

    std::uint64_t mint() noexcept { return ++next_waybill_; }

    void register_op(std::uint64_t w, void *sub)
    {
        slots_.push_back(ferry_slot{w, sub, 0, 0, false, false});
    }

    void route_slot(const Result &r) noexcept
    {
        for (std::size_t i = 0; i < slots_.size(); ++i) {
            if (slots_[i].waybill == r.data_addr) {
                if (slots_[i].sub) { free(slots_[i].sub); slots_[i].sub = nullptr; }
                if (slots_[i].orphaned) { slots_[i] = slots_.back(); slots_.pop_back(); return; }
                slots_[i].bytes = r.data_length;
                slots_[i].code  = static_cast<int>(r.error_code);
                slots_[i].done  = true;
                return;
            }
        }
    }

    void drain() noexcept
    {
        Result r;
        while (::result_pop_ferry(&r)) route_slot(r);
    }

    bool collect(std::uint64_t w, std::uint32_t *bytes, int *code) noexcept
    {
        for (std::size_t i = 0; i < slots_.size(); ++i) {
            if (slots_[i].waybill == w && slots_[i].done) {
                *bytes = slots_[i].bytes;
                *code  = slots_[i].code;
                slots_[i] = slots_.back();
                slots_.pop_back();
                return true;
            }
        }
        return false;
    }

    bool poll_collect(std::uint64_t w, std::uint32_t *bytes, int *code) noexcept
    {
        drain();
        return collect(w, bytes, code);
    }

    void abandon(std::uint64_t w) noexcept
    {
        for (std::size_t i = 0; i < slots_.size(); ++i) {
            if (slots_[i].waybill == w) {
                if (slots_[i].sub) free(slots_[i].sub);
                slots_[i] = slots_.back();
                slots_.pop_back();
                return;
            }
        }
    }

    void orphan(std::uint64_t w) noexcept
    {
        for (std::size_t i = 0; i < slots_.size(); ++i) {
            if (slots_[i].waybill == w) {
                if (slots_[i].done) { slots_[i] = slots_.back(); slots_.pop_back(); }
                else                { slots_[i].orphaned = true; }
                return;
            }
        }
    }

    ~ferry_station()
    {
        for (;;) {
            drain();
            bool inflight = false;
            for (auto &s : slots_) if (s.sub) { inflight = true; break; }
            if (!inflight) break;
            Result r;
            if (::result_wait_ferry(&r, 0)) route_slot(r);
        }
    }
};

inline ferry_station &ferry()
{
    thread_local ferry_station s;
    return s;
}

}

class ferry {
    std::uint64_t waybill_    = 0;
    std::uint32_t bytes_      = 0;
    int           code_       = 0;
    bool          done_       = false;
    bool          registered_ = false;

public:
    ferry() noexcept = default;
    ferry(const ferry &)            = delete;
    ferry &operator=(const ferry &) = delete;

    ferry(ferry &&o) noexcept
        : waybill_(o.waybill_), bytes_(o.bytes_), code_(o.code_),
          done_(o.done_), registered_(o.registered_)
    {
        o.waybill_ = 0; o.registered_ = false; o.done_ = false;
    }
    ferry &operator=(ferry &&o) noexcept
    {
        if (this != &o) {
            _M_detach();
            waybill_ = o.waybill_; bytes_ = o.bytes_; code_ = o.code_;
            done_ = o.done_; registered_ = o.registered_;
            o.waybill_ = 0; o.registered_ = false; o.done_ = false;
        }
        return *this;
    }
    ~ferry() { _M_detach(); }

    bool await_ready() noexcept
    {
        if (!registered_ || done_) return true;
        if (_detail::ferry().poll_collect(waybill_, &bytes_, &code_)) { done_ = true; return true; }
        return false;
    }
    bool await_suspend(std::coroutine_handle<> h)
    {
        executor::current()->wait_on(h, this, &_S_poll, &_S_block,
                                     __exec::wait_domain::result,
                                     0, true);
        return true;
    }
    result<std::size_t> await_resume() noexcept
    {
        if (waybill_ == 0) return std::unexpected(error{errc::invalid_argument});
        return _M_to_result();
    }

private:
    friend class tagfs::file;

    ferry(std::uint64_t w, bool registered, int code) noexcept
        : waybill_(w), code_(code), done_(!registered), registered_(registered) {}

    static ferry _make(std::uint32_t file_id, std::uint64_t offset,
                       const void *buf, std::size_t n, bool is_write) noexcept
    {
        _detail::ferry_station &st = _detail::ferry();
        std::uint64_t w = st.mint();

        _detail::ferry_submission *sub =
            static_cast<_detail::ferry_submission *>(malloc(sizeof(_detail::ferry_submission)));
        if (!sub) return ferry(w, false, ERR_NO_MEMORY);

        ManifestBuilder mb;
        if (ManifestBuilderInit(&mb, sub->manifest, sizeof(sub->manifest)) != 0) {
            free(sub);
            return ferry(w, false, ERR_INVALID_ARGS);
        }

        int rc;
        if (is_write) {
            std::uint8_t params[24];
            std::uint32_t flags = 0;
            __builtin_memcpy(params + 0,  &file_id, 4);
            __builtin_memcpy(params + 4,  &offset,  8);
            __builtin_memcpy(params + 12, &flags,   4);
            __builtin_memcpy(params + 16, &w,       8);
            CrateSetInput(&sub->crates[0], const_cast<void *>(buf), n);
            rc = ManifestBuilderAddOp(&mb, DECK_STORAGE, STORAGE_OBJ_WRITE,
                                      0, 0, CRATE_INDEX_NONE, params, 24);
        } else {
            std::uint8_t params[20];
            __builtin_memcpy(params + 0,  &file_id, 4);
            __builtin_memcpy(params + 4,  &offset,  8);
            __builtin_memcpy(params + 12, &w,       8);
            CrateSetOutput(&sub->crates[0], const_cast<void *>(buf), n);
            rc = ManifestBuilderAddOp(&mb, DECK_STORAGE, STORAGE_OBJ_READ,
                                      0, CRATE_INDEX_NONE, 0, params, 20);
        }
        if (rc != 0 || ManifestBuilderFinalize(&mb) != 0) {
            free(sub);
            return ferry(w, false, ERR_INVALID_ARGS);
        }

        try {
            st.register_op(w, sub);
        } catch (...) {
            free(sub);
            return ferry(w, false, ERR_NO_MEMORY);
        }

        int prc = ManifestSubmitNoWait(reinterpret_cast<const Manifest *>(sub->manifest),
                                       sub->crates, 1, 0);
        if (prc != OK) {
            st.abandon(w);
            return ferry(w, false, prc < 0 ? -prc : prc);
        }
        return ferry(w, true, 0);
    }

    void _M_detach() noexcept
    {
        if (registered_ && !done_) {
            try { _detail::ferry().orphan(waybill_); } catch (...) {}
        }
        registered_ = false;
        waybill_    = 0;
    }

    result<std::size_t> _M_to_result() const noexcept
    {
        return box::_detail::from_ret64<std::size_t>(
            code_ == 0 ? static_cast<std::int64_t>(bytes_)
                       : -static_cast<std::int64_t>(code_));
    }

    static bool _S_poll(void *s)
    {
        ferry *f = static_cast<ferry *>(s);
        if (f->done_) return true;
        if (_detail::ferry().poll_collect(f->waybill_, &f->bytes_, &f->code_)) {
            f->done_ = true;
            return true;
        }
        return false;
    }
    static void _S_block(void *s, std::uint32_t ms)
    {
        ferry *f = static_cast<ferry *>(s);
        if (f->done_) return;
        Result r;
        if (::result_wait_ferry(&r, ms)) _detail::ferry().route_slot(r);
    }
};

namespace tagfs {

inline ferry file::read_async(std::uint64_t offset, void *p, std::size_t n) const
{
    if (!id_) return ferry(0, false, ERR_INVALID_ARGUMENT);
    return ferry::_make(id_, offset, p, n, false);
}
inline ferry file::write_async(std::uint64_t offset, const void *p, std::size_t n) const
{
    if (!id_) return ferry(0, false, ERR_INVALID_ARGUMENT);
    return ferry::_make(id_, offset, p, n, true);
}
inline ferry file::read_async(std::uint64_t offset, std::span<std::byte> buf) const
{
    return read_async(offset, buf.data(), buf.size());
}
inline ferry file::write_async(std::uint64_t offset, std::span<const std::byte> buf) const
{
    return write_async(offset, buf.data(), buf.size());
}

}

}

#endif