// boxcxx — box::hw  (real-HW per-process CPU knobs: LAM, TME state, tagged pointers)
//
// The idiomatic C++ surface over the boxlib HW API (box/hw.h):
//
//   box::hw            — lam()/set_lam() (Linear Address Masking, per-process)
//                        and tme()/tme_state (TME/TME-MK platform snapshot).
//   box::tagged_pointer<T> — a pointer carrying a 7-bit tag in the LAM-U48
//                        metadata bits [62:56]; the CPU ignores those bits on
//                        access once LAM-U48 is engaged (box::hw::set_lam).
//
// This is a box:: extension, not part of std. Reads are safe anywhere.
// set_lam() returns std::expected: U57 always fails (BoxOS does not enable
// 5-level paging) and U48 fails on a CPU without LAM — the .error() carries the
// boxlib -ERR code. tagged_pointer's bit math is pure; DEREFERENCING a tagged
// pointer requires LAM-U48 to be engaged (otherwise the address is non-canonical
// and faults) — that, like the masking itself, is a real-HW property.
#ifndef BOXCXX_BOX_HW_H
#define BOXCXX_BOX_HW_H

#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>     // std::formatter<box::hw::tme_state>
#include <optional>

#include "box/cpu.h"  // cpu_has_lam / cpu_has_tme
#include "box/hw.h"   // hw_lam_get/set + hw_tme_state + hw_lam_mode_t + hw_tme_state_t

namespace box {

namespace hw {

// Linear Address Masking mode (mirrors hw_lam_mode_t).
enum class lam_mode : std::uint8_t {
    none = HW_LAM_NONE,  // canonical 48-bit, no masking (default)
    u48  = HW_LAM_U48,   // bits 62:48 of user VAs ignored (BoxOS tags bits 62:56)
    u57  = HW_LAM_U57,   // needs 5-level paging — BoxOS does not enable it
};

// True iff the CPU exposes LAM (per the kernel-published caps page).
inline bool lam_available() noexcept { return ::cpu_has_lam(); }
// True iff the platform exposes TME.
inline bool tme_available() noexcept { return ::cpu_has_tme(); }

// The calling process's current LAM mode (none if unset or unreadable).
inline lam_mode lam() noexcept
{
    int m = ::hw_lam_get();
    if (m < 0 || m > static_cast<int>(HW_LAM_U57)) return lam_mode::none;
    return static_cast<lam_mode>(m);
}

// Set the calling process's LAM mode. The new bits land on the next CR3 reload.
// Returns std::unexpected(boxlib -ERR) on failure — notably U57 (no 5-level
// paging) and U48 on a CPU without LAM.
inline std::expected<void, int> set_lam(lam_mode mode) noexcept
{
    int rc = ::hw_lam_set(static_cast<hw_lam_mode_t>(mode));
    if (rc != 0) return std::unexpected(rc);
    return {};
}

// ── box::hw::tme_state — a TME / TME-MK platform snapshot (view) ────────────
class tme_state {
    hw_tme_state_t s_{};

public:
    tme_state() noexcept = default;
    explicit tme_state(const hw_tme_state_t &s) noexcept : s_(s) {}
    const hw_tme_state_t &raw() const noexcept { return s_; }

    bool          active() const noexcept { return s_.tme_active != 0; }     // TME engaged
    bool          mk_active() const noexcept { return s_.mk_active != 0; }   // TME-MK (multi-key) engaged
    std::uint8_t  keyid_bits() const noexcept { return s_.num_keyid_bits; }
    std::uint8_t  algorithm() const noexcept { return s_.activated_alg; }
    std::uint16_t max_keyid() const noexcept { return s_.max_keyid; }
    std::uint16_t pool_programmed() const noexcept { return s_.pool_programmed; }
    std::uint16_t in_use() const noexcept { return s_.in_use; }
    std::uint16_t per_proc_quota() const noexcept { return s_.per_proc_quota; }
    std::uint8_t  reduced_maxphyaddr() const noexcept { return s_.reduced_maxphyaddr; }
    std::uint16_t this_proc_held() const noexcept { return s_.this_proc_held; }
};

// The platform TME state; nullopt if the kernel call fails.
inline std::optional<tme_state> tme() noexcept
{
    hw_tme_state_t s{};
    if (::hw_tme_state(&s) != 0) return std::nullopt;
    return tme_state(s);
}

// ── TME encryption synthesis (Ф25d) ─────────────────────────────────────────
// There is NO kernel "is region X encrypted" call — TME state is a GLOBAL
// platform snapshot. These synthesise the per-address answer HONESTLY from that
// snapshot plus the physical-address keyid lane the platform itself encodes:
//   • !active           → not encrypted, keyid 0.
//   • active, plain TME → encrypted with the platform key, keyid 0.
//   • active, TME-MK    → encrypted; keyid = phys bits [reduced_maxphyaddr +:
//                         num_keyid_bits] (the lane TME-MK steals from the phys
//                         address space — up to 15 bits, hence uint16_t).
// This is a DERIVED view, never a per-region kernel fact — name it as such.

// Encrypted iff TME is engaged: plain TME encrypts all RAM; under TME-MK the
// keyid only selects WHICH key, the page is still encrypted.
inline bool encrypted(const tme_state &s) noexcept { return s.active(); }

// The TME-MK keyid encoded in `phys` (0 under plain TME / no TME / a malformed
// snapshot). The guards also keep the shifts in-range (rmpa < 64, kb <= 15).
inline std::uint16_t keyid_of(const tme_state &s, std::uint64_t phys) noexcept
{
    const unsigned kb   = s.keyid_bits();
    const unsigned rmpa = s.reduced_maxphyaddr();
    if (!s.active() || !s.mk_active() || kb == 0 || kb > 15 || rmpa == 0 || rmpa >= 64)
        return 0;
    return static_cast<std::uint16_t>((phys >> rmpa) & ((1u << kb) - 1u));
}

// Convenience: fetch the current platform TME snapshot once and answer for the
// caller. Both report the dormant answer (false / 0) when TME is unavailable.
inline bool encrypted() noexcept
{
    auto s = tme();
    return s && encrypted(*s);
}
inline std::uint16_t keyid_of(std::uint64_t phys) noexcept
{
    auto s = tme();
    return s ? keyid_of(*s, phys) : static_cast<std::uint16_t>(0);
}

}  // namespace hw

// ── box::tagged_pointer<T> — a LAM-U48 tagged pointer ───────────────────────
// Stashes a 7-bit tag in linear-address bits [62:56] — the field BoxOS reserves
// under LAM-U48 (CR3.LAM_U48; the hardware ignores those bits on access once it
// is engaged via box::hw::set_lam(lam_mode::u48)). The bit math is always valid;
// dereferencing get() (or operator*/->) is only safe with LAM-U48 engaged —
// otherwise the address is non-canonical and faults. untagged() is always
// canonical and safe to dereference.
template <class T>
class tagged_pointer {
    std::uintptr_t bits_{0};

    static constexpr int           kShift = 56;
    static constexpr std::uintptr_t kTagMask = static_cast<std::uintptr_t>(0x7F) << kShift;

public:
    static constexpr std::uint8_t max_tag = 0x7F;  // 7 bits

    tagged_pointer() noexcept = default;
    tagged_pointer(T *p, std::uint8_t tag) noexcept
        : bits_((reinterpret_cast<std::uintptr_t>(p) & ~kTagMask) |
                (static_cast<std::uintptr_t>(tag & max_tag) << kShift))
    {}

    // The raw tagged pointer — dereferenceable only under LAM-U48.
    T *get() const noexcept { return reinterpret_cast<T *>(bits_); }
    // The canonical pointer with the tag stripped — always dereferenceable.
    T *untagged() const noexcept { return reinterpret_cast<T *>(bits_ & ~kTagMask); }
    // The stashed 7-bit tag.
    std::uint8_t tag() const noexcept { return static_cast<std::uint8_t>((bits_ >> kShift) & max_tag); }
    std::uintptr_t value() const noexcept { return bits_; }

    // Replace the tag in place, keeping the address.
    void retag(std::uint8_t tag) noexcept
    {
        bits_ = (bits_ & ~kTagMask) | (static_cast<std::uintptr_t>(tag & max_tag) << kShift);
    }

    explicit operator bool() const noexcept { return untagged() != nullptr; }
    T &operator*() const noexcept { return *get(); }   // requires LAM-U48 engaged
    T *operator->() const noexcept { return get(); }   // requires LAM-U48 engaged
};

}  // namespace box

// ── std::formatter<box::hw::tme_state> — one greppable line, no spec ─────────
template <>
struct std::formatter<box::hw::tme_state, char> {
    constexpr auto parse(std::format_parse_context &ctx) { return ctx.begin(); }
    auto format(const box::hw::tme_state &s, std::format_context &ctx) const
    {
        return std::format_to(
            ctx.out(),
            "tme active={} mk={} keyid_bits={} alg={} max_keyid={} in_use={} rmpa={} held={}",
            static_cast<unsigned>(s.active()), static_cast<unsigned>(s.mk_active()),
            static_cast<unsigned>(s.keyid_bits()), static_cast<unsigned>(s.algorithm()),
            static_cast<unsigned>(s.max_keyid()), static_cast<unsigned>(s.in_use()),
            static_cast<unsigned>(s.reduced_maxphyaddr()),
            static_cast<unsigned>(s.this_proc_held()));
    }
};

#endif  // BOXCXX_BOX_HW_H
