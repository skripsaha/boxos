// boxcxx — box::pku  (memory protection keys: per-key rights, W^X access windows,
//                     sealed regions)
//
// The idiomatic C++ surface over the boxlib PKU API (box/pku.h) — Intel Memory
// Protection Keys. Each leaf PTE carries a 4-bit key; the per-thread PKRU
// register holds an (access-disable, write-disable) pair per key, flipped from
// ring 3 with no syscall. BoxOS binds a key to a region's PTEs via MemTag.
//
//   box::pku             — available()/read()/write()/set_rights()/get_rights()/
//                          apply_region(): the raw per-key control.
//   box::protection_key  — a handle to one of the 16 keys: lock()/unlock()/
//                          read_only()/rights().
//   box::access_window   — RAII: open a key for a scope, restore its prior PKRU
//                          bits on exit. The W^X "reveal" window.
//   box::sealed_region<T>— bind a key to the region backing a typed span (the
//                          region is resolved from the pointer via the kernel's
//                          virt→region lookup), keep it sealed, reveal() on demand.
//
// This is a box:: extension, not part of std. Everything DISENGAGES cleanly when
// PKU is absent (available()==false): rights ops no-op, sealed_region stays
// unbound. The PKRU register round-trips on any PKU-capable CPU; the actual
// access *fault* on a sealed region requires hardware enforcement (an emulator
// may advertise PKU yet not fault) — that is a real-HW property.
#ifndef BOXCXX_BOX_PKU_H
#define BOXCXX_BOX_PKU_H

#include <cstdint>
#include <span>

#include "box/cpu.h"     // cpu_has_pku
#include "box/memtag.h"  // mem_region_from_virt + MEMTAG_INVALID_REGION_ID
#include "box/pku.h"     // pku_read_pkru/write_pkru/set_rights/get_rights/apply_region + PKU_MAX_KEYS

namespace box {

namespace pku {

// True iff CR4.PKE is on and PKU survived every AP intersect.
inline bool available() noexcept { return ::cpu_has_pku(); }

// The whole 16-key PKRU register (2 bits per key). Unprivileged RDPKRU/WRPKRU.
inline std::uint32_t read() noexcept { return ::pku_read_pkru(); }
inline void          write(std::uint32_t value) noexcept { ::pku_write_pkru(value); }

inline constexpr std::uint8_t max_keys = PKU_MAX_KEYS;  // 16

// A key's rights: access_disabled denies all access; write_disabled denies
// writes only (reads still allowed).
struct rights {
    bool access_disabled;
    bool write_disabled;
};

// Set / read a single key's rights. set_rights returns false on a CPU without
// PKU or an out-of-range key.
inline bool set_rights(std::uint8_t key, bool access_disable, bool write_disable) noexcept
{
    return ::pku_set_rights(key, access_disable ? 1 : 0, write_disable ? 1 : 0) == 0;
}
inline rights get_rights(std::uint8_t key) noexcept
{
    int ad = 0, wd = 0;
    ::pku_get_rights(key, &ad, &wd);
    return rights{ad != 0, wd != 0};
}

// Stamp pku:<key> onto a region's PTEs across every attach (key 0 clears the
// policy). The caller supplies a region_id it owns — see box::sealed_region or
// box::memtag / mem_region_from_virt to obtain one. Returns false on error.
inline bool apply_region(std::uint32_t region_id, std::uint8_t key) noexcept
{
    return ::pku_apply_region(region_id, key) == 0;
}

}  // namespace pku

// ── box::protection_key — a handle to one of the 16 hardware keys ───────────
class protection_key {
    std::uint8_t key_;

public:
    explicit constexpr protection_key(std::uint8_t key) noexcept : key_(key) {}
    constexpr std::uint8_t value() const noexcept { return key_; }

    bool unlock() const noexcept { return pku::set_rights(key_, false, false); }     // full access
    bool read_only() const noexcept { return pku::set_rights(key_, false, true); }   // reads only
    bool lock() const noexcept { return pku::set_rights(key_, true, false); }        // no access
    pku::rights rights() const noexcept { return pku::get_rights(key_); }
};

// ── box::access_window — RAII: open a key for a scope, restore it on exit ────
// Construction widens the key to full access (or reads-only when write==false);
// destruction restores exactly that key's two PKRU bits to their prior value,
// leaving any other key the scope may have touched alone.
class access_window {
    std::uint8_t  key_;
    std::uint32_t saved_;
    bool          engaged_;

public:
    explicit access_window(protection_key k, bool write = true) noexcept
        : key_(k.value()), saved_(pku::read()), engaged_(pku::available())
    {
        if (engaged_) pku::set_rights(key_, /*access_disable*/ false, /*write_disable*/ !write);
    }
    explicit access_window(std::uint8_t key, bool write = true) noexcept
        : access_window(protection_key(key), write) {}

    ~access_window() { if (engaged_) restore(); }

    access_window(const access_window &)            = delete;
    access_window &operator=(const access_window &) = delete;

private:
    void restore() noexcept
    {
        const std::uint32_t shift = static_cast<std::uint32_t>(key_) * 2u;
        const std::uint32_t mask  = 0x3u << shift;
        pku::write((pku::read() & ~mask) | (saved_ & mask));
        engaged_ = false;
    }
};

// ── box::sealed_region<T> — a key-protected view of region-backed memory ────
// Resolves the region covering `mem` (mem.data() → virt→region in the kernel),
// stamps a protection key onto it, and seals it (no access) by default. The
// memory MUST be region-backed — a box::bay<T> is the canonical source. bound()
// reports whether the binding took (PKU present + region resolved + stamped).
// On destruction the key tag is cleared from the region. reveal() opens a
// scoped access_window; outside it the region is sealed (enforced on real HW).
template <class T>
class sealed_region {
    std::span<T>  mem_{};
    std::uint32_t region_id_{MEMTAG_INVALID_REGION_ID};
    std::uint8_t  key_{0};
    bool          bound_{false};

public:
    sealed_region(std::span<T> mem, std::uint8_t key) noexcept : mem_(mem), key_(key)
    {
        if (!pku::available() || mem.empty()) return;
        region_id_ = ::mem_region_from_virt(static_cast<const void *>(mem.data()));
        if (region_id_ == MEMTAG_INVALID_REGION_ID) return;
        if (!pku::apply_region(region_id_, key_)) { region_id_ = MEMTAG_INVALID_REGION_ID; return; }
        bound_ = true;
        seal();  // sealed (no access) by default — open with reveal()
    }
    ~sealed_region()
    {
        if (bound_) pku::apply_region(region_id_, 0);  // clear the key tag from the region
    }
    sealed_region(const sealed_region &)            = delete;
    sealed_region &operator=(const sealed_region &) = delete;

    bool            bound() const noexcept { return bound_; }
    std::uint32_t   region() const noexcept { return region_id_; }
    protection_key  key() const noexcept { return protection_key(key_); }
    std::span<T>    data() const noexcept { return mem_; }

    void seal() const noexcept { pku::set_rights(key_, /*access_disable*/ true, false); }    // no access
    void seal_read_only() const noexcept { pku::set_rights(key_, false, /*write_disable*/ true); }

    // Open a scoped access window over this region's key (read+write by default).
    [[nodiscard]] access_window reveal(bool write = true) const noexcept
    {
        return access_window(key_, write);
    }
};

}  // namespace box

#endif  // BOXCXX_BOX_PKU_H
