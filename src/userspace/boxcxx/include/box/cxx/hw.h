#ifndef BOXCXX_BOX_HW_H
#define BOXCXX_BOX_HW_H

#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <limits>
#include <optional>

#include "box/cpu.h"
#include "box/hw.h"

namespace box {

namespace hw {

enum class lam_mode : std::uint8_t {
    none = HW_LAM_NONE,
    u48  = HW_LAM_U48,
    u57  = HW_LAM_U57,
};

inline bool lam_available() noexcept { return ::cpu_has_lam(); }
inline bool tme_available() noexcept { return ::cpu_has_tme(); }

inline lam_mode lam() noexcept
{
    int m = ::hw_lam_get();
    if (m < 0 || m > static_cast<int>(HW_LAM_U57)) return lam_mode::none;
    return static_cast<lam_mode>(m);
}

inline std::expected<void, int> set_lam(lam_mode mode) noexcept
{
    int rc = ::hw_lam_set(static_cast<hw_lam_mode_t>(mode));
    if (rc != 0) return std::unexpected(rc);
    return {};
}

class tme_state {
    hw_tme_state_t s_{};

public:
    tme_state() noexcept = default;
    explicit tme_state(const hw_tme_state_t &s) noexcept : s_(s) {}
    const hw_tme_state_t &raw() const noexcept { return s_; }

    bool          active() const noexcept { return s_.tme_active != 0; }
    bool          mk_active() const noexcept { return s_.mk_active != 0; }
    std::uint8_t  keyid_bits() const noexcept { return s_.num_keyid_bits; }
    std::uint8_t  algorithm() const noexcept { return s_.activated_alg; }
    std::uint16_t max_keyid() const noexcept { return s_.max_keyid; }
    std::uint16_t pool_programmed() const noexcept { return s_.pool_programmed; }
    std::uint16_t in_use() const noexcept { return s_.in_use; }
    std::uint16_t per_proc_quota() const noexcept { return s_.per_proc_quota; }
    std::uint8_t  reduced_maxphyaddr() const noexcept { return s_.reduced_maxphyaddr; }
    std::uint16_t this_proc_held() const noexcept { return s_.this_proc_held; }
};

inline std::optional<tme_state> tme() noexcept
{
    hw_tme_state_t s{};
    if (::hw_tme_state(&s) != 0) return std::nullopt;
    return tme_state(s);
}


inline bool encrypted(const tme_state &s) noexcept { return s.active(); }

inline std::uint16_t keyid_of(const tme_state &s, std::uint64_t phys) noexcept
{
    const unsigned kb   = s.keyid_bits();
    const unsigned rmpa = s.reduced_maxphyaddr();
    if (!s.active() || !s.mk_active() || kb == 0 || kb > 15 || rmpa == 0 || rmpa >= 64)
        return 0;
    return static_cast<std::uint16_t>((phys >> rmpa) & ((1u << kb) - 1u));
}

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

}

template <class T>
class tagged_pointer {
    std::uintptr_t bits_{0};

    static constexpr int           kShift = 56;
    static constexpr std::uintptr_t kTagMask = static_cast<std::uintptr_t>(0x7F) << kShift;

public:
    static constexpr std::uint8_t max_tag = 0x7F;

    tagged_pointer() noexcept = default;
    tagged_pointer(T *p, std::uint8_t tag) noexcept
        : bits_((reinterpret_cast<std::uintptr_t>(p) & ~kTagMask) |
                (static_cast<std::uintptr_t>(tag & max_tag) << kShift))
    {}

    T *get() const noexcept { return reinterpret_cast<T *>(bits_); }
    T *untagged() const noexcept { return reinterpret_cast<T *>(bits_ & ~kTagMask); }
    std::uint8_t tag() const noexcept { return static_cast<std::uint8_t>((bits_ >> kShift) & max_tag); }
    std::uintptr_t value() const noexcept { return bits_; }

    void retag(std::uint8_t tag) noexcept
    {
        bits_ = (bits_ & ~kTagMask) | (static_cast<std::uintptr_t>(tag & max_tag) << kShift);
    }

    explicit operator bool() const noexcept { return untagged() != nullptr; }
    T &operator*() const noexcept { return *get(); }
    T *operator->() const noexcept { return get(); }
};

}

template <>
struct std::formatter<box::hw::tme_state, char> : std::formatter<std::string_view, char> {
    auto format(const box::hw::tme_state &s, std::format_context &ctx) const
    {
        static constexpr char kFmt[] =
            "tme active={} mk={} keyid_bits={} alg={} max_keyid={} in_use={} rmpa={} held={}";
        static constexpr std::size_t kCap =
            sizeof kFmt + 8 * (std::numeric_limits<unsigned long long>::digits10 + 1);
        char buf[kCap];
        auto r = std::format_to_n(
            buf, (std::ptrdiff_t)sizeof buf, kFmt,
            static_cast<unsigned>(s.active()), static_cast<unsigned>(s.mk_active()),
            static_cast<unsigned>(s.keyid_bits()), static_cast<unsigned>(s.algorithm()),
            static_cast<unsigned>(s.max_keyid()), static_cast<unsigned>(s.in_use()),
            static_cast<unsigned>(s.reduced_maxphyaddr()),
            static_cast<unsigned>(s.this_proc_held()));
        return std::formatter<std::string_view, char>::format(
            std::string_view(buf, (std::size_t)(r.out - buf)), ctx);
    }
};

#endif