// boxcxx — box::system / box::efi  (system & firmware introspection / control)
//
// The idiomatic C++ surface over the boxlib system + EFI API (box/system.h):
//
//   box::system_info  — a snapshot of the machine (version / uptime as
//                       std::chrono::nanoseconds / memory / cpu topology /
//                       CPU feature flags).
//   box::system       — box::system::info(); maintenance perf_dump() /
//                       fragmentation() / defrag(); and reboot() / shutdown()
//                       (which return ONLY on failure).
//   box::efi_status   — firmware state: runtime/ESRT/Secure-Boot availability,
//                       enforcement, and counts.
//   box::efi          — box::efi::info(); esrt() (the full EFI System Resource
//                       Table as a range) / esrt_entry(i); verify_pe(span) /
//                       verified(span) (Authenticode against db/dbx).
//
// This is a box:: extension, not part of std. Read-only introspection is safe
// anywhere; reboot()/shutdown() act on the whole machine.
#ifndef BOXCXX_BOX_SYSTEM_H
#define BOXCXX_BOX_SYSTEM_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "box/system.h"  // sysinfo / reboot / shutdown / perf_dump / defrag /
                         // fragmentation / efi_* + the POD types & enums
#include "box/cxx/error.h"  // box::status / box::result / box::error

namespace box {

namespace __sysdetail {
// NUL-bounded view of a fixed-width char field (never overruns a missing NUL).
inline std::string_view bounded(const char *p, std::size_t cap) noexcept
{
    std::size_t n = 0;
    while (n < cap && p[n] != '\0') ++n;
    return {p, n};
}
}  // namespace __sysdetail

// ── box::system_info — a machine snapshot (idiomatic view over system_info_t) ─
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

// ── box::system — info + maintenance + control ──────────────────────────────
namespace system {

// A snapshot of the machine; on success the view, otherwise the recovered
// kernel cause in the error arm.
inline result<box::system_info> info() noexcept
{
    system_info_t s{};
    int rc = ::sysinfo(&s);
    if (rc != 0) return std::unexpected(error{box_errno_of(rc)});
    return box::system_info(s);
}

// Dump perf counters to the log. Empty status on success; the error arm carries
// the cause.
inline status perf_dump() noexcept { return _detail::from_status(::perf_dump()); }

// The kernel-defined fragmentation metric (>= 0 on success); the error arm
// carries the cause on a failed call.
inline result<int> fragmentation() noexcept { return _detail::from_ret<int>(::fragmentation()); }

// Defragment a file toward target_block. On success the kernel returns the
// resulting fragmentation SCORE (>= 0) — surfaced as the value here; the error
// arm carries the cause. (A nonzero score is a SUCCESS, not a failure: the old
// `== 0` bool form wrongly read any nonzero score as failure.)
inline result<int> defrag(std::uint32_t file_id, std::uint32_t target_block) noexcept
{
    return _detail::from_ret<int>(::defrag(file_id, target_block));
}

// reboot() / shutdown() act on the whole machine and return ONLY on failure
// (on success control never comes back). The error arm then names the real
// cause the request failed with (e.g. no ACPI path -> internal).
inline status reboot() noexcept { return _detail::from_status(::reboot()); }
inline status shutdown() noexcept { return _detail::from_status(::shutdown()); }

}  // namespace system

// ── box::efi_status — firmware / Secure Boot state (view over efi_info_t) ────
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

// ── box::efi — firmware introspection ───────────────────────────────────────
namespace efi {

// Firmware state snapshot; on success the view, otherwise the recovered cause.
inline result<box::efi_status> info() noexcept
{
    efi_info_t e{};
    int rc = ::efi_info(&e);
    if (rc != 0) return std::unexpected(error{box_errno_of(rc)});
    return box::efi_status(e);
}

// One ESRT entry by index ([0, esrt_count)); on success the entry, otherwise the
// recovered cause in the error arm.
inline result<efi_esrt_entry_t> esrt_entry(std::uint32_t idx) noexcept
{
    efi_esrt_entry_t en{};
    int rc = ::efi_esrt_entry(idx, &en);
    if (rc != 0) return std::unexpected(error{box_errno_of(rc)});
    return en;
}

// The whole EFI System Resource Table as a range (empty when unpublished — a
// valid answer, so this stays a collection rather than a result<vector>).
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

// Authenticode-verify a PE image against the firmware's db/dbx. The returned
// efi_verify_t carries the verdict in .result (EFI_VERIFY_OK == trusted) plus
// the computed PE / signer SHA-256.
inline efi_verify_t verify_pe(std::span<const std::byte> pe) noexcept
{
    efi_verify_t v{};
    // Default to a non-trusted verdict: a zero-initialized result would read as
    // EFI_VERIFY_OK (== 0), so if the call cannot run (e.g. Secure Boot
    // unavailable) the image must NOT come back "trusted".
    v.result = EFI_VERIFY_SB_UNAVAILABLE;
    ::efi_verify_pe(pe.data(), static_cast<std::uint32_t>(pe.size()), &v);
    return v;
}
// true iff the image is trusted by the current Secure Boot policy.
inline bool verified(std::span<const std::byte> pe) noexcept
{
    return verify_pe(pe).result == EFI_VERIFY_OK;
}

}  // namespace efi

}  // namespace box

#endif  // BOXCXX_BOX_SYSTEM_H
