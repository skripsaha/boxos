#ifndef BOXCXX_BOX_SYSTEM_H
#define BOXCXX_BOX_SYSTEM_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "box/system.h"
#include "box/cxx/error.h"

namespace box {

namespace __sysdetail {
inline std::string_view bounded(const char *p, std::size_t cap) noexcept
{
    std::size_t n = 0;
    while (n < cap && p[n] != '\0') ++n;
    return {p, n};
}
}

class system_info {
    system_info_t s_{};

public:
    system_info() noexcept = default;
    explicit system_info(const system_info_t &s) noexcept : s_(s) {}
    const system_info_t &raw() const noexcept { return s_; }

    std::string_view         version() const noexcept { return __sysdetail::bounded(s_.version, sizeof(s_.version)); }
    std::chrono::nanoseconds uptime() const noexcept { return std::chrono::nanoseconds(s_.uptime_ns); }
    std::uint64_t            total_memory() const noexcept { return s_.total_memory; }
    std::uint64_t            used_memory() const noexcept { return s_.used_memory; }
    std::uint64_t            free_memory() const noexcept { return s_.free_memory; }
    std::uint64_t            tsc_freq_khz() const noexcept { return s_.tsc_freq_khz; }
    std::uint32_t            cpu_total() const noexcept { return s_.cpu_total; }
    std::uint32_t            cpu_k_cores() const noexcept { return s_.cpu_k_cores; }
    std::uint32_t            cpu_app_cores() const noexcept { return s_.cpu_app_cores; }
    std::uint32_t            process_count() const noexcept { return s_.process_count; }
    std::uint32_t            pit_freq_hz() const noexcept { return s_.pit_freq_hz; }
    bool                     multicore() const noexcept { return s_.multicore_active != 0; }
    bool                     invariant_tsc() const noexcept { return s_.has_invariant_tsc != 0; }
    bool                     waitpkg() const noexcept { return s_.has_waitpkg != 0; }
};

namespace system {

inline result<box::system_info> info() noexcept
{
    system_info_t s{};
    int rc = ::sysinfo(&s);
    if (rc != 0) return std::unexpected(error{box_errno_of(rc)});
    return box::system_info(s);
}

inline status perf_dump() noexcept { return _detail::from_status(::perf_dump()); }

inline result<int> fragmentation() noexcept { return _detail::from_ret<int>(::fragmentation()); }

inline result<int> defrag(std::uint32_t file_id, std::uint32_t target_block) noexcept
{
    return _detail::from_ret<int>(::defrag(file_id, target_block));
}

inline status reboot() noexcept { return _detail::from_status(::reboot()); }
inline status shutdown() noexcept { return _detail::from_status(::shutdown()); }

}

class efi_status {
    efi_info_t e_{};

public:
    efi_status() noexcept = default;
    explicit efi_status(const efi_info_t &e) noexcept : e_(e) {}
    const efi_info_t &raw() const noexcept { return e_; }

    bool runtime_available() const noexcept { return e_.rt_available != 0; }
    bool esrt_available() const noexcept { return e_.esrt_available != 0; }
    bool secure_boot_available() const noexcept { return e_.sb_available != 0; }
    bool secure_boot_enforced() const noexcept { return e_.sb_enforced != 0; }
    bool setup_mode() const noexcept { return e_.sb_setup_mode != 0; }
    bool audit_mode() const noexcept { return e_.sb_audit_mode != 0; }
    bool deployed_mode() const noexcept { return e_.sb_deployed_mode != 0; }

    std::uint32_t esrt_count() const noexcept { return e_.esrt_count; }
    std::uint32_t cert_count() const noexcept { return e_.cert_count_total; }
    std::uint32_t hash_count() const noexcept { return e_.hash_count_total; }
};

namespace efi {

inline result<box::efi_status> info() noexcept
{
    efi_info_t e{};
    int rc = ::efi_info(&e);
    if (rc != 0) return std::unexpected(error{box_errno_of(rc)});
    return box::efi_status(e);
}

inline result<efi_esrt_entry_t> esrt_entry(std::uint32_t idx) noexcept
{
    efi_esrt_entry_t en{};
    int rc = ::efi_esrt_entry(idx, &en);
    if (rc != 0) return std::unexpected(error{box_errno_of(rc)});
    return en;
}

inline std::vector<efi_esrt_entry_t> esrt()
{
    std::vector<efi_esrt_entry_t> out;
    result<box::efi_status> st = info();
    if (!st) return out;
    out.reserve(st->esrt_count());
    for (std::uint32_t i = 0; i < st->esrt_count(); ++i)
        if (result<efi_esrt_entry_t> e = esrt_entry(i)) out.push_back(*e);
    return out;
}

inline efi_verify_t verify_pe(std::span<const std::byte> pe) noexcept
{
    efi_verify_t v{};
    v.result = EFI_VERIFY_SB_UNAVAILABLE;
    ::efi_verify_pe(pe.data(), static_cast<std::uint32_t>(pe.size()), &v);
    return v;
}
inline bool verified(std::span<const std::byte> pe) noexcept
{
    return verify_pe(pe).result == EFI_VERIFY_OK;
}

}

}

#endif