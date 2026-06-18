// boxcxx — box::manifest / box::crate  (the expert C++ face of the BoxOS syscall ABI)
//
// Every BoxOS syscall is a Manifest: an ordered list of ops (deck + opcode +
// inline params) plus a table of Crates (variable-size payload descriptors that
// point into the cabin heap). The typed box:: layers (box::tagfs, box::message,
// box::bay, ...) are built on this; this header is the low-level builder for
// expert code that needs raw deck/opcode access or multi-op batching.
//
//   box::crate              — a payload descriptor: crate::input(span) /
//                             output(span) / in_out(...) / input_object<T> /
//                             output_object<T>; produced() reads back what an op
//                             wrote into an output crate after a submit.
//   box::manifest<Buf,N>    — a fixed-capacity, fluent multi-op builder that owns
//                             its manifest buffer + crate table. add(crate) ->
//                             index; op(deck, opcode, in, out, params, flags);
//                             submit(target_pid, timeout) -> box::mf_outcome.
//   box::compiled_manifest  — a RAII "prepared statement": compile a manifest
//                             once, submit it repeatedly (skips the kernel's
//                             per-call validate/lookup), released on scope exit.
//   box::mf_call1(deck, op, params, in, out) — the single-op convenience.
//
// This is a box:: extension, not part of std. Crates are NON-owning views — the
// backing buffers must outlive the submit (the kernel reads/writes them in place).
#ifndef BOXCXX_BOX_MANIFEST_H
#define BOXCXX_BOX_MANIFEST_H

#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

#include "box/core/manifest.h"  // ManifestBuilder, MfCall1, ManifestSubmitFull, *Handle
#include "box/core/crate.h"     // Crate + CrateSetInput/Output/InOut
#include "boxos_decks.h"        // DECK_* ids (for expert callers)

namespace box {

// CRATE_INDEX_NONE as a named C++ constant — "this op has no input/output crate".
inline constexpr std::uint16_t no_crate = CRATE_INDEX_NONE;

// ── box::crate — a payload descriptor (non-owning view over caller memory) ──
class crate {
    Crate c_{};

public:
    crate() noexcept { CrateInit(&c_); }
    explicit crate(const Crate &c) noexcept : c_(c) {}

    // An op reads from this region (kernel never mutates an input crate's bytes).
    static crate input(std::span<const std::byte> b) noexcept
    {
        Crate c;
        CrateSetInput(&c, const_cast<std::byte *>(b.data()), b.size());
        return crate(c);
    }
    // An op writes up to b.size() bytes here; produced() reports how many.
    static crate output(std::span<std::byte> b) noexcept
    {
        Crate c;
        CrateSetOutput(&c, b.data(), b.size());
        return crate(c);
    }
    // An op reads `valid` bytes then writes back in place (capacity == b.size()).
    static crate in_out(std::span<std::byte> b, std::uint64_t valid) noexcept
    {
        Crate c;
        CrateSetInOut(&c, b.data(), valid, b.size());
        return crate(c);
    }
    template <class T>
    static crate input_object(const T &v) noexcept
    {
        static_assert(std::is_trivially_copyable_v<T>, "crate::input_object<T>: trivially copyable");
        Crate c;
        CrateSetInput(&c, const_cast<T *>(&v), sizeof(T));
        return crate(c);
    }
    template <class T>
    static crate output_object(T &v) noexcept
    {
        static_assert(std::is_trivially_copyable_v<T>, "crate::output_object<T>: trivially copyable");
        Crate c;
        CrateSetOutput(&c, &v, sizeof(T));
        return crate(c);
    }

    Crate         &raw() noexcept { return c_; }
    const Crate   &raw() const noexcept { return c_; }
    CrateKind      kind() const noexcept { return static_cast<CrateKind>(c_.kind); }
    std::uint64_t  size() const noexcept { return c_.size; }      // valid / produced bytes
    std::uint64_t  capacity() const noexcept { return c_.capacity; }

    // The bytes an op produced into an output crate (valid after submit).
    std::span<std::byte> produced() const noexcept
    {
        return {reinterpret_cast<std::byte *>(static_cast<std::uintptr_t>(c_.addr)),
                static_cast<std::size_t>(c_.size)};
    }
};

// ── box::mf_outcome — the result of a submit ────────────────────────────────
struct mf_outcome {
    int    rc;       // OK (0) on success; kernel error_t / negative builder error otherwise
    Result result;   // the kernel Result record
    explicit operator bool() const noexcept { return rc == 0; }
};

// ── box::manifest — a fixed-capacity, fluent multi-op syscall builder ───────
template <std::size_t BufBytes = 256, std::uint16_t MaxCrates = 8>
class manifest {
    alignas(8) std::uint8_t buf_[BufBytes];
    Crate           crates_[MaxCrates];
    ManifestBuilder mb_{};
    std::uint16_t   crate_count_ = 0;
    bool            ok_          = true;

public:
    manifest() noexcept { ok_ = (ManifestBuilderInit(&mb_, buf_, BufBytes) == 0); }
    manifest(const manifest &)            = delete;
    manifest &operator=(const manifest &) = delete;

    // Register a crate; returns its index for op() references (no_crate if full).
    std::uint16_t add(const crate &c) noexcept
    {
        if (crate_count_ >= MaxCrates) {
            ok_ = false;
            return no_crate;
        }
        crates_[crate_count_] = c.raw();
        return crate_count_++;
    }

    // Append one op (chainable). Crate args are indices from add() or no_crate.
    manifest &op(std::uint16_t deck, std::uint16_t opcode,
                 std::uint16_t in_crate = no_crate, std::uint16_t out_crate = no_crate,
                 std::span<const std::byte> params = {}, std::uint16_t flags = 0) noexcept
    {
        if (ManifestBuilderAddOp(&mb_, deck, opcode, flags, in_crate, out_crate,
                                 params.data(), static_cast<std::uint16_t>(params.size())) != 0)
            ok_ = false;
        return *this;
    }

    bool valid() const noexcept { return ok_; }
    explicit operator bool() const noexcept { return ok_; }

    // Finalize (idempotent) + submit. target_pid 0 == self; timeout 0 == default.
    mf_outcome submit(std::uint32_t target_pid = 0, std::uint32_t timeout_ms = 0) noexcept
    {
        if (!ok_) return {-1, Result{}};
        if (!mb_.finalized && ManifestBuilderFinalize(&mb_) != 0) {
            ok_ = false;
            return {-1, Result{}};
        }
        Result r{};
        int    rc = ManifestSubmitFull(reinterpret_cast<const Manifest *>(buf_),
                                       crates_, crate_count_, target_pid, &r, timeout_ms);
        return {rc, r};
    }

    // A crate's descriptor after submit (read produced() / size()).
    crate          crate_at(std::uint16_t idx) const noexcept
    {
        return idx < crate_count_ ? crate(crates_[idx]) : crate();
    }
    std::uint16_t   crate_count() const noexcept { return crate_count_; }
    Crate          *crates() noexcept { return crates_; }
    const Manifest *raw() const noexcept { return reinterpret_cast<const Manifest *>(buf_); }
    bool            finalize() noexcept
    {
        if (!ok_) return false;
        if (!mb_.finalized && ManifestBuilderFinalize(&mb_) != 0) ok_ = false;
        return ok_;
    }
};

// ── box::compiled_manifest — a RAII "prepared statement" handle ─────────────
// Compile a built box::manifest once, then submit it repeatedly with refreshed
// crate buffers — each submit skips the kernel's per-call validate + per-op
// registry lookup. Released on scope exit (move-only).
class compiled_manifest {
    ManifestHandle h_     = 0;
    bool           valid_ = false;

public:
    compiled_manifest() noexcept = default;

    template <std::size_t B, std::uint16_t M>
    explicit compiled_manifest(manifest<B, M> &mf) noexcept
    {
        if (mf.finalize()) valid_ = (ManifestCompileHandle(mf.raw(), &h_) == 0);
    }
    compiled_manifest(const compiled_manifest &)            = delete;
    compiled_manifest &operator=(const compiled_manifest &) = delete;
    compiled_manifest(compiled_manifest &&o) noexcept : h_(o.h_), valid_(o.valid_)
    {
        o.valid_ = false;
    }
    compiled_manifest &operator=(compiled_manifest &&o) noexcept
    {
        if (this != &o) {
            if (valid_) ManifestReleaseHandle(h_);
            h_       = o.h_;
            valid_   = o.valid_;
            o.valid_ = false;
        }
        return *this;
    }
    ~compiled_manifest() { if (valid_) ManifestReleaseHandle(h_); }

    explicit operator bool() const noexcept { return valid_; }
    ManifestHandle handle() const noexcept { return h_; }

    // Submit reusing a manifest's crate table (mutate the crate buffers between
    // submits to feed fresh data). The manifest's op structure is the cached one.
    template <std::size_t B, std::uint16_t M>
    mf_outcome submit(manifest<B, M> &mf, std::uint32_t timeout_ms = 0) noexcept
    {
        if (!valid_) return {-1, Result{}};
        Result r{};
        int    rc = ManifestSubmitHandleTimeout(h_, mf.crates(), mf.crate_count(), &r, timeout_ms);
        return {rc, r};
    }
    // Submit with an explicit crate array.
    mf_outcome submit(Crate *crates, std::uint16_t crate_count, std::uint32_t timeout_ms = 0) noexcept
    {
        if (!valid_) return {-1, Result{}};
        Result r{};
        int    rc = ManifestSubmitHandleTimeout(h_, crates, crate_count, &r, timeout_ms);
        return {rc, r};
    }
};

// ── box::mf_call1 — the single-op convenience over MfCall1 ──────────────────
struct mf_call_result {
    int           rc;        // OK (0) on success
    std::uint32_t produced;  // bytes the kernel wrote into the output buffer
    explicit operator bool() const noexcept { return rc == 0; }
};

inline mf_call_result mf_call1(std::uint16_t deck, std::uint16_t opcode,
                               std::span<const std::byte> params = {},
                               std::span<const std::byte> in     = {},
                               std::span<std::byte>       out     = {},
                               std::uint32_t              timeout_ms = 0) noexcept
{
    std::uint32_t actual = 0;
    int           rc = MfCall1(deck, opcode,
                               params.data(), static_cast<std::uint16_t>(params.size()),
                               in.data(), static_cast<std::uint32_t>(in.size()),
                               out.data(), static_cast<std::uint32_t>(out.size()), &actual,
                               timeout_ms, nullptr);
    return {rc, actual};
}

}  // namespace box

#endif  // BOXCXX_BOX_MANIFEST_H
