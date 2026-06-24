// boxcxx — box::error / box::errc / box::result  (the native BoxOS error model)
//
// BoxOS is not Unix, and it does not borrow Unix's error vocabulary. The kernel
// reports failure with a categorised error_t (box/error.h: ~70 ERR_* codes in
// numbered ranges — memory 100s, io 200s, storage 300s, process 400s, …), NOT
// with the Linux errno table. This header is the idiomatic C++ face of that
// native model, so a fallible box:: call hands back the REAL kernel cause
// instead of collapsing it to bool / -1 / nullopt:
//
//   box::errc      — every ERR_* as a strongly-typed enumerator. The values are
//                    taken FROM the box/error.h macros, so that header stays the
//                    single source of truth: renumber there and this follows.
//   box::error     — a 4-byte trivially-copyable value carrying one errc, with
//                    .code() / .raw() / .message() / .category_name(), an
//                    operator bool that is true when it names a FAILURE (the
//                    std::error_code polarity, for painless interop), ordering,
//                    and a std::formatter so errors print with std::format.
//   box::result<T> — std::expected<T, box::error>: the canonical return of a
//                    fallible box:: operation. operator bool is true on SUCCESS
//                    (expected's polarity), so `if (auto r = f.thing())` reads
//                    naturally and `r.error().code()` is the exact cause.
//   box::status    — box::result<void>, for operations that either succeed or
//                    fail with no payload.
//
// Bridging the boxlib C ABI: a boxlib stub reports failure as a value < 0
// whose magnitude is the error_t (see box/error.h box_fail/box_errno_of — the
// stubs are converted to this rule in Ф23b). box::_detail::from_ret /
// from_status / from_err turn those returns into a box::result, so the cause
// survives the whole stack: kernel error_t → boxlib -error_t → box::error,
// with no errno register anywhere on the box::error path.
//
// std interop, without lying: box::errc opts into std::is_error_code_enum and
// box::make_error_code maps it onto box::error_category() — a category with its
// OWN numbering and messages, whose default_error_condition maps each code to
// ITSELF (never onto the Linux generic_category, which has no faithful BoxOS
// equivalent). So `std::error_code ec = box::errc::no_memory;` is honest: value
// 106, category "box" — it is never silently reinterpreted as POSIX ENOMEM.
//
// This is a box:: extension, not part of std. std::errc remains a thin
// conformance shim for the std machinery; it is not the BoxOS error language.
#ifndef BOXCXX_BOX_ERROR_H
#define BOXCXX_BOX_ERROR_H

#include <compare>
#include <cstdint>
#include <expected>
#include <format>
#include <string>
#include <string_view>
#include <system_error>

#include "box/error.h"  // error_t, the ERR_* table, box_fail / box_errno_of

namespace box {

// ── box::errc — the kernel error_t table, strongly typed ────────────────────
// One enumerator per box/error.h code; each value is the macro itself, so the C
// header remains the single source of truth (no duplicated literals here).
enum class errc : ::error_t {
    ok                     = OK,

    unknown                = ERR_UNKNOWN,
    not_implemented        = ERR_NOT_IMPLEMENTED,
    invalid_argument       = ERR_INVALID_ARGUMENT,
    null_pointer           = ERR_NULL_POINTER,
    out_of_range           = ERR_OUT_OF_RANGE,
    buffer_too_small       = ERR_BUFFER_TOO_SMALL,
    timeout                = ERR_TIMEOUT,
    busy                   = ERR_BUSY,
    would_block            = ERR_WOULD_BLOCK,
    alignment              = ERR_ALIGNMENT,
    corrupted              = ERR_CORRUPTED,
    internal               = ERR_INTERNAL,

    no_memory              = ERR_NO_MEMORY,
    invalid_address        = ERR_INVALID_ADDRESS,
    page_fault             = ERR_PAGE_FAULT,
    already_mapped         = ERR_ALREADY_MAPPED,
    not_mapped             = ERR_NOT_MAPPED,
    permission_denied_mem  = ERR_PERMISSION_DENIED_MEM,
    heap_exhausted         = ERR_HEAP_EXHAUSTED,
    stack_overflow         = ERR_STACK_OVERFLOW,
    buffer_overflow        = ERR_BUFFER_OVERFLOW,
    invalid_buffer_id      = ERR_INVALID_BUFFER_ID,
    buffer_in_use          = ERR_BUFFER_IN_USE,
    buffer_limit_exceeded  = ERR_BUFFER_LIMIT_EXCEEDED,

    io                     = ERR_IO,
    read_failed            = ERR_READ_FAILED,
    write_failed           = ERR_WRITE_FAILED,
    device_not_ready       = ERR_DEVICE_NOT_READY,
    device_error           = ERR_DEVICE_ERROR,
    stream_closed          = ERR_STREAM_CLOSED,
    disk_full              = ERR_DISK_FULL,
    bad_sector             = ERR_BAD_SECTOR,

    file_not_found         = ERR_FILE_NOT_FOUND,
    object_not_found       = ERR_OBJECT_NOT_FOUND,
    tag_not_found          = ERR_TAG_NOT_FOUND,
    already_exists         = ERR_ALREADY_EXISTS,
    invalid_tag            = ERR_INVALID_TAG,
    tag_limit_exceeded     = ERR_TAG_LIMIT_EXCEEDED,
    object_corrupted       = ERR_OBJECT_CORRUPTED,
    journal_full           = ERR_JOURNAL_FULL,
    journal_corrupted      = ERR_JOURNAL_CORRUPTED,
    metadata_corrupted     = ERR_METADATA_CORRUPTED,

    process_not_found      = ERR_PROCESS_NOT_FOUND,
    invalid_pid            = ERR_INVALID_PID,
    process_limit_exceeded = ERR_PROCESS_LIMIT_EXCEEDED,
    process_terminated     = ERR_PROCESS_TERMINATED,
    process_blocked        = ERR_PROCESS_BLOCKED,
    invalid_elf            = ERR_INVALID_ELF,
    binary_too_large       = ERR_BINARY_TOO_LARGE,
    spawn_failed           = ERR_SPAWN_FAILED,

    access_denied          = ERR_ACCESS_DENIED,
    permission_denied      = ERR_PERMISSION_DENIED,
    security_violation     = ERR_SECURITY_VIOLATION,
    tag_mismatch           = ERR_TAG_MISMATCH,
    invalid_operation      = ERR_INVALID_OPERATION,

    hardware               = ERR_HARDWARE,
    invalid_device         = ERR_INVALID_DEVICE,
    device_busy            = ERR_DEVICE_BUSY,
    keyboard_buffer_full   = ERR_KEYBOARD_BUFFER_FULL,
    vga_error              = ERR_VGA_ERROR,
    ata_error              = ERR_ATA_ERROR,
    pci_error              = ERR_PCI_ERROR,

    pocket_ring_full       = ERR_POCKET_RING_FULL,
    result_ring_full       = ERR_RESULT_RING_FULL,
    invalid_pocket         = ERR_INVALID_POCKET,
    invalid_deck_id        = ERR_INVALID_DECK_ID,
    invalid_opcode         = ERR_INVALID_OPCODE,
    prefix_chain_too_long  = ERR_PREFIX_CHAIN_TOO_LONG,
    pocket_processing_failed = ERR_POCKET_PROCESSING_FAILED,
    pending_queue_full     = ERR_PENDING_QUEUE_FULL,

    addr_value_mismatch    = ERR_ADDR_VALUE_MISMATCH,
};

namespace _detail {
// Defined in src/runtime/box_error.cpp — a static, allocation-free table.
// error_message never returns nullptr; category_name groups by the numeric
// range ("memory" / "io" / "storage" / "process" / …).
std::string_view error_message(::error_t code) noexcept;
std::string_view error_category_name(::error_t code) noexcept;
}  // namespace _detail

// ── box::error — a typed kernel cause ───────────────────────────────────────
class error {
    ::error_t code_ = OK;

public:
    constexpr error() noexcept = default;
    constexpr explicit error(::error_t c) noexcept : code_(c) {}
    constexpr error(errc e) noexcept : code_(static_cast<::error_t>(e)) {}

    constexpr ::error_t raw() const noexcept { return code_; }
    constexpr errc code() const noexcept { return static_cast<errc>(code_); }
    constexpr bool ok() const noexcept { return code_ == OK; }

    // Polarity: true means this names a FAILURE (matches std::error_code, so
    // the two interoperate without a trap). On the happy path you test the
    // box::result, not the bare error — its bool is true on SUCCESS.
    constexpr explicit operator bool() const noexcept { return code_ != OK; }

    // Human text + category name, from the static table (no allocation, safe in
    // noexcept paths and before/after static init).
    std::string_view message() const noexcept { return _detail::error_message(code_); }
    std::string_view category_name() const noexcept
    {
        return _detail::error_category_name(code_);
    }

    friend constexpr bool operator==(error, error) noexcept = default;
    friend constexpr std::strong_ordering operator<=>(error, error) noexcept = default;
};

// box::result/box::status (std::expected) rely on box::error staying a small,
// trivially-copyable value for their trivial-special-member fast paths. Pin it
// so a future field addition can't silently regress that.
static_assert(sizeof(error) == sizeof(::error_t) && std::is_trivially_copyable_v<error>,
              "box::error must stay a trivially-copyable error_t-sized value");

// ── box::result / box::status — the canonical fallible returns ──────────────
template <class T>
using result = std::expected<T, error>;
using status = std::expected<void, error>;

namespace _detail {
// Build a box::result from a boxlib stub return under the box_fail contract:
//   from_ret<T>(r)  — payload stub: r >= 0 is the value, r < 0 is -error_t.
//   from_status(r)  — 0/-error_t stub: 0 is success, r < 0 is the cause.
//   from_err(e)     — a stub that already hands back a raw error_t out-of-band.
// Contract for from_ret: a SUCCESS payload must fit 0 <= payload <= INT_MAX (it
// rides the int return, and a negative value is read as a cause). Stubs whose
// payload can exceed that — a multi-GiB byte count, an id near 2^31 — need a
// wider bridge (an int64/out-param form); none of the Ф23b consumers do today.
template <class T = int>
inline result<T> from_ret(int r) noexcept
{
    if (r < 0) return std::unexpected(error{box_errno_of(r)});
    return static_cast<T>(r);
}
inline status from_status(int r) noexcept
{
    if (r != 0) return std::unexpected(error{box_errno_of(r)});
    return {};
}
inline status from_err(::error_t e) noexcept
{
    if (e != OK) return std::unexpected(error{e});
    return {};
}
}  // namespace _detail

// ── std::error_code interop, honest ─────────────────────────────────────────
// box::error_category() is a category with BoxOS numbering and messages; its
// default_error_condition is identity (a box code maps only to itself, never
// onto the Linux generic_category). Defined in src/runtime/box_error.cpp.
const std::error_category &error_category() noexcept;

inline std::error_code make_error_code(errc e) noexcept
{
    return std::error_code(static_cast<int>(e), box::error_category());
}

// Bridge a box::error to a std::error_code (same value, "box" category).
inline std::error_code to_error_code(error e) noexcept
{
    return std::error_code(static_cast<int>(e.raw()), box::error_category());
}

}  // namespace box

// box::errc is usable as a std::error_code enum (so std::error_code ec =
// box::errc::...; finds box::make_error_code by ADL).
template <>
struct std::is_error_code_enum<box::errc> : std::true_type {};

// ── std::formatter<box::error> — print as "<category>:<raw> <message>" ──────
// e.g. "memory:106 out of memory". Reuses formatter<string_view>'s spec
// parser, so width/fill/align/precision all work.
template <>
struct std::formatter<box::error, char> : std::formatter<std::string_view, char> {
    auto format(box::error e, std::format_context &ctx) const
    {
        std::string s(e.category_name());
        s += ':';
        s += std::to_string(e.raw());
        s += ' ';
        s += e.message();
        return std::formatter<std::string_view, char>::format(s, ctx);
    }
};

#endif  // BOXCXX_BOX_ERROR_H
