// boxcxx — box::error / box::errc / box::result  (the native BoxOS error model)
//
// BoxOS is not Unix, and it does not borrow Unix's error vocabulary. The kernel
// reports failure with a categorised error_t (box/error.h: 202 ERR_* codes in
// numbered ranges — memory 100s, io 200s, storage 300s, process 400s, …), NOT
// with the Linux errno table. This header is the idiomatic C++ face of that
// native model, so a fallible box:: call hands back the REAL kernel cause
// instead of collapsing it to bool / -1 / nullopt:
//
//   box::errc      — every ERR_* as a strongly-typed enumerator, expanded from
//                    the one BOX_ERROR_LIST table in box/error.h, so that header
//                    stays the single source of truth: add a code there and the
//                    enumerator, its message and its category all follow.
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
// 100, category "box" — it is never silently reinterpreted as POSIX ENOMEM.
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
// One enumerator per box/error.h code, expanded straight from BOX_ERROR_LIST so
// this enum can NEVER drift from the C codes: each enumerator takes its value
// from its ERR_* constant, and box/error.h remains the single source of truth
// (no literal is duplicated here — add a code there and it appears here).
enum class errc : ::error_t {
    ok = OK,
#define BOX_ERRC_ENUMERATOR(SUFFIX, name, value, msg, cat) name = ERR_##SUFFIX,
    BOX_ERROR_LIST(BOX_ERRC_ENUMERATOR)
#undef BOX_ERRC_ENUMERATOR
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
// payload can exceed that — a multi-GiB byte count, an id near 2^31 — use
// from_ret64 below, whose return rides an int64 (the fread/fwrite byte-count
// path is the one consumer today).
template <class T = int>
inline result<T> from_ret(int r) noexcept
{
    if (r < 0) return std::unexpected(error{box_errno_of(r)});
    return static_cast<T>(r);
}
// Wide bridge for byte-count stubs (fread/fwrite): the return rides an int64,
// so a count of 0..4 GiB survives without colliding with the negative cause
// channel. r >= 0 is the count; r < 0 is -error_t. Every error_t is <= 1100, so
// a failing stub's r is a small negative — the cast to int for box_errno_of is
// exact and never truncates a real count (which only lives on the r >= 0 side).
template <class T = std::size_t>
inline result<T> from_ret64(std::int64_t r) noexcept
{
    if (r < 0) return std::unexpected(error{box_errno_of(static_cast<int>(r))});
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
