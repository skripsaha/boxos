// boxcxx — box::ferry  (a co_await-able async file-I/O operation)
//
// box::tagfs::file::read_async / write_async submit a storage op WITHOUT blocking
// and hand back a box::ferry — a move-only handle you co_await on a box::executor
// to get the byte count (or the cause) when the disk completes. Several ferries
// can be in flight at once; each co_await resumes its own coroutine when THAT
// op's completion lands, in whatever order the disk retires them.
//
//   box::tagfs::file f = *box::tagfs::find("log")->live();
//   std::byte a[512], b[512];
//   box::ferry r0 = f.read_async(0,   a);            // submitted now, in flight
//   box::ferry r1 = f.read_async(512, b);            // both in flight
//   box::result<std::size_t> n0 = co_await r0;       // resumes on r0's completion
//   box::result<std::size_t> n1 = co_await r1;       // ... on r1's, out of order
//   co_await f.write_async(0, std::span<const std::byte>{a, 512});
//
// ── the waybill (why correlation is needed) ──────────────────────────────────
// Every storage completion lands in the strand's one ResultRing. With one op in
// flight (the synchronous fread/fwrite) the reply is unambiguous; with several,
// the completions interleave. So each ferry op carries a WAYBILL — a per-strand,
// monotonic, never-reused u64 the kernel echoes back in the completion's data
// address. A returning ferry flies the KCTX_STORAGE cargo flag (never mistaken
// for an IPC message or a plain kernel reply) and carries its waybill; the
// harbour-master (the station) reads the waybill and hands each captain only the
// cargo of THEIR ship. A never-reused waybill means a cancelled op's late
// completion carries an old number that matches no open berth and is turned away.
//
// ── the ferry station (routing + isolation) ──────────────────────────────────
// KCTX_STORAGE completions are FULLY ISOLATED in boxlib: every other ResultRing
// consumer (synchronous fread, box::result_any, IPC receive, orphan-drain)
// routes them out into a per-strand ferry stash, and they are handed out only by
// result_pop_ferry / result_wait_ferry. The strand-local station owns the live
// ops (keyed by waybill) and their submission blocks, routes each completion to
// its slot, and — crucially — OWNS THE SUBMISSION BLOCK until the completion
// arrives. The manifest + Crate descriptors a ferry submits are read by the
// kernel asynchronously (guide stages the pocket after ManifestSubmitNoWait
// returns), so freeing them the instant a ferry is dropped would let the kernel
// read a stale descriptor and commit to a wrong address. Holding the block in the
// station until the completion routes makes ~ferry a non-blocking DETACH that is
// still UAF-safe: a dropped ferry's slot is orphaned, and route frees its block
// when the op finishes (the strand's station destructor sweeps any that were
// never drained).
//
// ── the caller's buffer ──────────────────────────────────────────────────────
// The DATA buffer (the span you pass) is yours and the kernel DMAs into it for
// the whole op — it MUST outlive the co_await, exactly like the frame a
// brook_read fills. Only the small submission block (manifest + descriptors) is
// the station's to manage.
//
// ── caveats ──────────────────────────────────────────────────────────────────
// The station is thread_local: a ferry must be co_await'd on the SAME strand that
// created it. Do NOT run box::result_any() on a strand with ferries in flight —
// it consumes raw ResultRing records and would race the ferry channel; one
// harbour-master per strand (the same discipline box::child asks for its death
// watch).
#ifndef BOXCXX_BOX_FERRY_H
#define BOXCXX_BOX_FERRY_H

#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "box/cxx/error.h"      // box::result / box::error / box::errc
#include "box/cxx/executor.h"   // box::executor / __exec::wait_domain / wait_on  (+ box/core/result.h)
#include "box/cxx/tagfs.h"      // box::tagfs::file (out-of-line read_async/write_async definitions)
#include "box/core/manifest.h"  // Manifest / ManifestOp / ManifestBuilder / Crate / ManifestSubmitNoWait
#include "boxos_decks.h"        // DECK_STORAGE + STORAGE_OBJ_READ / WRITE — the single source
#include "box/memory.h"         // malloc / free — station-owned submission blocks
#include "box/error.h"          // ::error_t / ERR_* / OK

namespace box {

namespace _detail {

// The submission block a ferry hands to the kernel: the 1-op storage Manifest
// bytes plus its single Crate descriptor. The station owns one per in-flight op
// and frees it when the op's completion routes (the kernel is done with it then).
// Heap-allocated and never moved, so the Pocket's captured manifest/crate
// pointers stay valid for the whole op.
struct ferry_submission {
    std::uint8_t manifest[sizeof(Manifest) + sizeof(ManifestOp) + 24];
    Crate        crates[1];
};

// ferry_slot — one in-flight op, keyed by its waybill. `sub` is the station-owned
// submission block, freed the moment the completion routes here. code/bytes/done
// latch the outcome; orphaned marks a slot whose ferry was dropped (route frees
// sub + discards the result).
struct ferry_slot {
    std::uint64_t waybill;
    void         *sub;
    std::uint32_t bytes;
    int           code;      // ::error_t; 0 == OK
    bool          done;
    bool          orphaned;
};

// ferry_station — the strand-local ResultRing demultiplexer for KCTX_STORAGE
// completions. Single-writer on its owning strand (like the per-strand stash it
// drains through), so it needs no lock. slots_ holds every live op; a completion
// is routed IN-PLACE into its (waybill) slot, so register_op (on submit) is the
// only allocation and routing never allocates.
struct ferry_station {
    std::vector<ferry_slot> slots_;
    std::uint64_t           next_waybill_ = 0;   // monotonic, never reused; 0 == sync/no-token

    std::uint64_t mint() noexcept { return ++next_waybill_; }

    // The allocation point: reserve a slot for a freshly submitted op and take
    // ownership of its submission block. May throw bad_alloc (push_back) — the
    // caller (ferry::_make) frees `sub` and reports no_memory on throw.
    void register_op(std::uint64_t w, void *sub)
    {
        slots_.push_back(ferry_slot{w, sub, 0, 0, false, false});
    }

    // Route one KCTX_STORAGE completion into its (waybill) slot. Frees the
    // submission block — the kernel is finished with it now. An orphaned slot
    // (its ferry was dropped) is removed and the result discarded. A completion
    // for no slot (already collected / never registered) is dropped.
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

    // Non-blocking: drain every pending ferry completion into its slot. Non-ferry
    // records are routed to their own consumer inside result_pop_ferry, so this
    // only ever sees storage completions.
    void drain() noexcept
    {
        Result r;
        while (::result_pop_ferry(&r)) route_slot(r);
    }

    // Take (waybill)'s latched outcome, retiring the slot.
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

    // Submit failed before the op ever flew — free its block and drop the slot.
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

    // Detach: the ferry for (waybill) was dropped un-collected. If its completion
    // already routed (block freed), retire the slot; otherwise orphan it so route
    // frees the block and discards the result when the op finishes. Non-blocking.
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

    // Strand teardown: some ops may still be IN FLIGHT (sub != null, done == false)
    // — their submission block is still being read by the kernel guide, so freeing
    // it now would be a use-after-free. Drain to COMPLETION: every submitted op is
    // guaranteed a completion (the async finaliser, or the EOF/single-core
    // sync-fallback token), and route_slot frees each block as its completion
    // lands. Blocking here is the same disk wait a co_await performs — event-driven,
    // not a timeout — and it also guarantees the kernel has no outstanding push to
    // this strand's ResultRing before the ring is torn down.
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

// The calling strand's station: lazily constructed on first ferry and destroyed
// (freeing any un-drained submission blocks) when the strand ends.
inline ferry_station &ferry()
{
    thread_local ferry_station s;
    return s;
}

}  // namespace _detail

// ── box::ferry — a move-only, co_await-able async file-I/O operation ─────────
class ferry {
    std::uint64_t waybill_    = 0;      // 0 == empty / moved-from
    std::uint32_t bytes_      = 0;      // byte count once the completion latches
    int           code_       = 0;      // ::error_t; 0 == OK
    bool          done_       = false;  // completion collected (or submit failed)?
    bool          registered_ = false;  // is (waybill_) a live slot in the station?

public:
    ferry() noexcept = default;
    ferry(const ferry &)            = delete;
    ferry &operator=(const ferry &) = delete;

    ferry(ferry &&o) noexcept
        : waybill_(o.waybill_), bytes_(o.bytes_), code_(o.code_),
          done_(o.done_), registered_(o.registered_)
    {
        // The station slot is (waybill)-keyed and stays put — move only the handle.
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

    // ── the awaiter is the ferry itself ──────────────────────────────────────
    // An empty / moved-from ferry, a submit-failed ferry, or one whose completion
    // is already collectable is ready immediately (never suspends on a completion
    // that can never arrive); await_resume then reports the outcome.
    bool await_ready() noexcept
    {
        if (!registered_ || done_) return true;
        if (_detail::ferry().poll_collect(waybill_, &bytes_, &code_)) { done_ = true; return true; }
        return false;
    }
    bool await_suspend(std::coroutine_handle<> h)
    {
        // result domain, accept_any: the block consumes ANY ResultRing record and
        // routes it (ours → our slot, a sibling's → theirs), so it never strands
        // a co-waiting IPC/other awaiter (the Ф24a accept_any contract).
        executor::current()->wait_on(h, this, &_S_poll, &_S_block,
                                     __exec::wait_domain::result,
                                     /*deadline_tsc=*/0, /*accept_any=*/true);
        return true;
    }
    result<std::size_t> await_resume() noexcept
    {
        if (waybill_ == 0) return std::unexpected(error{errc::invalid_argument});  // empty / moved-from
        return _M_to_result();
    }

private:
    friend class tagfs::file;

    ferry(std::uint64_t w, bool registered, int code) noexcept
        : waybill_(w), code_(code), done_(!registered), registered_(registered) {}

    // Build the 1-op storage Manifest (with the trailing waybill), register it +
    // its submission block with the station, and submit without blocking. A
    // failure (out-of-memory or a full pocket ring) yields a ready ferry whose
    // await_resume reports the cause.
    static ferry _make(std::uint32_t file_id, std::uint64_t offset,
                       const void *buf, std::size_t n, bool is_write) noexcept
    {
        _detail::ferry_station &st = _detail::ferry();
        std::uint64_t w = st.mint();

        _detail::ferry_submission *sub =
            static_cast<_detail::ferry_submission *>(malloc(sizeof(_detail::ferry_submission)));
        if (!sub) return ferry(w, /*registered=*/false, ERR_NO_MEMORY);

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
                                      0, /*in*/0, /*out*/CRATE_INDEX_NONE, params, 24);
        } else {
            std::uint8_t params[20];
            __builtin_memcpy(params + 0,  &file_id, 4);
            __builtin_memcpy(params + 4,  &offset,  8);
            __builtin_memcpy(params + 12, &w,       8);
            CrateSetOutput(&sub->crates[0], const_cast<void *>(buf), n);
            rc = ManifestBuilderAddOp(&mb, DECK_STORAGE, STORAGE_OBJ_READ,
                                      0, /*in*/CRATE_INDEX_NONE, /*out*/0, params, 20);
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
            st.abandon(w);   // frees sub + retires slot
            return ferry(w, false, prc < 0 ? -prc : prc);
        }
        return ferry(w, /*registered=*/true, 0);
    }

    // Detach: a live, un-collected ferry hands its slot to the station to reap on
    // completion (never kills, never blocks). A collected or submit-failed ferry
    // owns no live slot.
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
        // Same receipt as file::read_at / write_at: a byte count on success, the
        // recovered error_t on failure.
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
        // Consume ONE record and route it (ours → slot; a sibling's → its own
        // stash inside result_wait_ferry) so the executor's post-block poll sweep
        // then services every waiter. The following _S_poll drains the rest.
        Result r;
        if (::result_wait_ferry(&r, ms)) _detail::ferry().route_slot(r);
    }
};

namespace tagfs {

// ── box::tagfs::file async byte I/O — submit now, co_await the box::ferry ─────
// The DATA buffer must outlive the co_await (the kernel DMAs into it). An empty
// file handle yields a ready ferry whose await_resume is invalid_argument.
inline ferry file::read_async(std::uint64_t offset, void *p, std::size_t n) const
{
    if (!id_) return ferry(0, false, ERR_INVALID_ARGUMENT);
    return ferry::_make(id_, offset, p, n, /*is_write=*/false);
}
inline ferry file::write_async(std::uint64_t offset, const void *p, std::size_t n) const
{
    if (!id_) return ferry(0, false, ERR_INVALID_ARGUMENT);
    return ferry::_make(id_, offset, p, n, /*is_write=*/true);
}
inline ferry file::read_async(std::uint64_t offset, std::span<std::byte> buf) const
{
    return read_async(offset, buf.data(), buf.size());
}
inline ferry file::write_async(std::uint64_t offset, std::span<const std::byte> buf) const
{
    return write_async(offset, buf.data(), buf.size());
}

}  // namespace tagfs

}  // namespace box

#endif  // BOXCXX_BOX_FERRY_H
