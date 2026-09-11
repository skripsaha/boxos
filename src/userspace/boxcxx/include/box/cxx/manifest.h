#ifndef BOXCXX_BOX_MANIFEST_H
#define BOXCXX_BOX_MANIFEST_H

#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

#include "box/core/manifest.h"
#include "box/core/crate.h"
#include "boxos_decks.h"

namespace box {

inline constexpr std::uint16_t no_crate = CRATE_INDEX_NONE;

class crate {
    Crate c_{};

public:
    crate() noexcept { CrateInit(&c_); }
    explicit crate(const Crate &c) noexcept : c_(c) {}

    static crate input(std::span<const std::byte> b) noexcept
    {
        Crate c;
        CrateSetInput(&c, const_cast<std::byte *>(b.data()), b.size());
        return crate(c);
    }
    static crate output(std::span<std::byte> b) noexcept
    {
        Crate c;
        CrateSetOutput(&c, b.data(), b.size());
        return crate(c);
    }
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
    std::uint64_t  size() const noexcept { return c_.size; }
    std::uint64_t  capacity() const noexcept { return c_.capacity; }

    std::span<std::byte> produced() const noexcept
    {
        return {reinterpret_cast<std::byte *>(static_cast<std::uintptr_t>(c_.addr)),
                static_cast<std::size_t>(c_.size)};
    }
};

struct mf_outcome {
    int    rc;
    Result result;
    explicit operator bool() const noexcept { return rc == 0; }
};

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

    std::uint16_t add(const crate &c) noexcept
    {
        if (crate_count_ >= MaxCrates) {
            ok_ = false;
            return no_crate;
        }
        crates_[crate_count_] = c.raw();
        return crate_count_++;
    }

    manifest &op(std::uint16_t deck, std::uint16_t opcode,
                 std::uint16_t in_crate = no_crate, std::uint16_t out_crate = no_crate,
                 std::span<const std::byte> params = {}, std::uint16_t flags = 0) noexcept
    {
        if (params.size() > 0xFFFFu) {
            ok_ = false;
            return *this;
        }
        if (ManifestBuilderAddOp(&mb_, deck, opcode, flags, in_crate, out_crate,
                                 params.data(), static_cast<std::uint16_t>(params.size())) != 0)
            ok_ = false;
        return *this;
    }

    bool valid() const noexcept { return ok_; }
    explicit operator bool() const noexcept { return ok_; }

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

    template <std::size_t B, std::uint16_t M>
    mf_outcome submit(manifest<B, M> &mf, std::uint32_t timeout_ms = 0) noexcept
    {
        if (!valid_) return {-1, Result{}};
        Result r{};
        int    rc = ManifestSubmitHandleTimeout(h_, mf.crates(), mf.crate_count(), &r, timeout_ms);
        return {rc, r};
    }
    mf_outcome submit(Crate *crates, std::uint16_t crate_count, std::uint32_t timeout_ms = 0) noexcept
    {
        if (!valid_) return {-1, Result{}};
        Result r{};
        int    rc = ManifestSubmitHandleTimeout(h_, crates, crate_count, &r, timeout_ms);
        return {rc, r};
    }
};

struct mf_call_result {
    int           rc;
    std::uint32_t produced;
    explicit operator bool() const noexcept { return rc == 0; }
};

inline mf_call_result mf_call1(std::uint16_t deck, std::uint16_t opcode,
                               std::span<const std::byte> params = {},
                               std::span<const std::byte> in     = {},
                               std::span<std::byte>       out     = {},
                               std::uint32_t              timeout_ms = 0) noexcept
{
    if (params.size() > 0xFFFFu || in.size() > 0xFFFFFFFFull || out.size() > 0xFFFFFFFFull)
        return {-1, 0};
    std::uint32_t actual = 0;
    int           rc = MfCall1(deck, opcode,
                               params.data(), static_cast<std::uint16_t>(params.size()),
                               in.data(), static_cast<std::uint32_t>(in.size()),
                               out.data(), static_cast<std::uint32_t>(out.size()), &actual,
                               timeout_ms, nullptr);
    return {rc, actual};
}

}

#endif