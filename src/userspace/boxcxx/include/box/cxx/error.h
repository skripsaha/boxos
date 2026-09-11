#ifndef BOXCXX_BOX_ERROR_H
#define BOXCXX_BOX_ERROR_H

#include <compare>
#include <cstdint>
#include <expected>
#include <format>
#include <string>
#include <string_view>
#include <system_error>

#include "box/error.h"

namespace box {

enum class errc : ::error_t {
    ok = OK,
#define BOX_ERRC_ENUMERATOR(SUFFIX, name, value, msg, cat) name = ERR_##SUFFIX,
    BOX_ERROR_LIST(BOX_ERRC_ENUMERATOR)
#undef BOX_ERRC_ENUMERATOR
};

namespace _detail {
std::string_view error_message(::error_t code) noexcept;
std::string_view error_category_name(::error_t code) noexcept;
}

class error {
    ::error_t code_ = OK;

public:
    constexpr error() noexcept = default;
    constexpr explicit error(::error_t c) noexcept : code_(c) {}
    constexpr error(errc e) noexcept : code_(static_cast<::error_t>(e)) {}

    constexpr ::error_t raw() const noexcept { return code_; }
    constexpr errc code() const noexcept { return static_cast<errc>(code_); }
    constexpr bool ok() const noexcept { return code_ == OK; }

    constexpr explicit operator bool() const noexcept { return code_ != OK; }

    std::string_view message() const noexcept { return _detail::error_message(code_); }
    std::string_view category_name() const noexcept
    {
        return _detail::error_category_name(code_);
    }

    friend constexpr bool operator==(error, error) noexcept = default;
    friend constexpr std::strong_ordering operator<=>(error, error) noexcept = default;
};

static_assert(sizeof(error) == sizeof(::error_t) && std::is_trivially_copyable_v<error>,
              "box::error must stay a trivially-copyable error_t-sized value");

template <class T>
using result = std::expected<T, error>;
using status = std::expected<void, error>;

namespace _detail {
template <class T = int>
inline result<T> from_ret(int r) noexcept
{
    if (r < 0) return std::unexpected(error{box_errno_of(r)});
    return static_cast<T>(r);
}
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
}

const std::error_category &error_category() noexcept;

inline std::error_code make_error_code(errc e) noexcept
{
    return std::error_code(static_cast<int>(e), box::error_category());
}

inline std::error_code to_error_code(error e) noexcept
{
    return std::error_code(static_cast<int>(e.raw()), box::error_category());
}

}

template <>
struct std::is_error_code_enum<box::errc> : std::true_type {};

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

#endif