#ifndef BOXCXX_BOX_PKU_H
#define BOXCXX_BOX_PKU_H

#include <cstdint>
#include <span>

#include "box/cpu.h"
#include "box/memtag.h"
#include "box/pku.h"

namespace box {

namespace pku {

inline bool available() noexcept { return ::cpu_has_pku(); }

inline std::uint32_t read() noexcept { return ::pku_read_pkru(); }
inline void          write(std::uint32_t value) noexcept { ::pku_write_pkru(value); }

inline constexpr std::uint8_t max_keys = PKU_MAX_KEYS;

struct rights {
    bool access_disabled;
    bool write_disabled;
};

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

inline bool apply_region(std::uint32_t region_id, std::uint8_t key) noexcept
{
    return ::pku_apply_region(region_id, key) == 0;
}

}

class protection_key {
    std::uint8_t key_;

public:
    explicit constexpr protection_key(std::uint8_t key) noexcept : key_(key) {}
    constexpr std::uint8_t value() const noexcept { return key_; }

    bool unlock() const noexcept { return pku::set_rights(key_, false, false); }
    bool read_only() const noexcept { return pku::set_rights(key_, false, true); }
    bool lock() const noexcept { return pku::set_rights(key_, true, false); }
    pku::rights rights() const noexcept { return pku::get_rights(key_); }
};

class access_window {
    std::uint8_t  key_;
    std::uint32_t saved_;
    bool          engaged_;

public:
    explicit access_window(protection_key k, bool write = true) noexcept
        : key_(k.value()), saved_(pku::read()), engaged_(pku::available())
    {
        if (engaged_) pku::set_rights(key_,  false,  !write);
    }
    explicit access_window(std::uint8_t key, bool write = true) noexcept
        : access_window(protection_key(key), write) {}

    ~access_window() { if (engaged_) restore(); }

    access_window(access_window &&o) noexcept
        : key_(o.key_), saved_(o.saved_), engaged_(o.engaged_) { o.engaged_ = false; }
    access_window(const access_window &)            = delete;
    access_window &operator=(const access_window &) = delete;
    access_window &operator=(access_window &&)      = delete;

private:
    void restore() noexcept
    {
        const std::uint32_t shift = static_cast<std::uint32_t>(key_) * 2u;
        const std::uint32_t mask  = 0x3u << shift;
        pku::write((pku::read() & ~mask) | (saved_ & mask));
        engaged_ = false;
    }
};

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
        seal();
    }
    ~sealed_region()
    {
        if (bound_) pku::apply_region(region_id_, 0);
    }
    sealed_region(const sealed_region &)            = delete;
    sealed_region &operator=(const sealed_region &) = delete;

    bool            bound() const noexcept { return bound_; }
    std::uint32_t   region() const noexcept { return region_id_; }
    protection_key  key() const noexcept { return protection_key(key_); }
    std::span<T>    data() const noexcept { return mem_; }

    void seal() const noexcept { pku::set_rights(key_,  true, false); }
    void seal_read_only() const noexcept { pku::set_rights(key_, false,  true); }

    [[nodiscard]] access_window reveal(bool write = true) const noexcept
    {
        return access_window(key_, write);
    }
};

}

#endif